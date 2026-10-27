/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/dict/ob_ft_dict_hub.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "lib/utility/utility.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
namespace oceanbase
{
namespace storage
{
// One process level shared dictionary: the container has to outlive the range dict
// that reads it, and both live in the arena of the hub.
struct ObFTSharedDictItem
{
  explicit ObFTSharedDictItem(ObIAllocator &alloc) : container_(alloc), dict_(nullptr) {}
  ObFTCacheRangeContainer container_;
  ObIFTDict *dict_;
};

// Release one item: the range dict first because it reads the container, then the
// item whose destructor destroys the container, and that is what releases the pinned
// dictionary cache handles. Resetting the arena never runs a destructor, so every
// path that drops an item has to funnel through here.
static void destroy_shared_dict_item(ObFTSharedDictItem *item)
{
  if (OB_NOT_NULL(item)) {
    if (OB_NOT_NULL(item->dict_)) {
      ObIFTDict *dict = item->dict_;
      dict->~ObIFTDict();
      item->dict_ = nullptr;
    }
    item->~ObFTSharedDictItem();
  }
}

int ObFTDictHub::init()
{
  static constexpr int K_MAX_DICT_BUCKET = 128; // for now, only built-in dicts.
  int ret = OB_SUCCESS;
  if (OB_FAIL(dict_map_.create(K_MAX_DICT_BUCKET, "dict_map"))) {
  } else if (OB_FAIL(rw_dict_lock_.init(K_MAX_DICT_BUCKET))) {
  } else if (OB_FAIL(shared_dict_map_.create(K_MAX_SHARED_DICT_BUCKET, "shared_dict_map"))) {
  } else if (OB_FAIL(shared_dict_lock_.init(K_MAX_SHARED_DICT_BUCKET))) {
  } else {
    is_inited_ = true;
  }
  return ret;
};

int ObFTDictHub::destroy()
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    clear_shared_dicts();
    is_inited_ = false;
  }
  return ret;
}

ObFTDictHub::~ObFTDictHub()
{
  // Safety net: the hub is a process level singleton and is destroyed after all the
  // parsers of the process are gone, but keep the cleanup idempotent anyway.
  if (is_inited_) {
    clear_shared_dicts();
    is_inited_ = false;
  }
}

int ObFTDictHub::clear_shared_dicts()
{
  int ret = OB_SUCCESS;
  // A parser that is still matching against a shared dictionary must not see it
  // freed. Wait for the holders to drain and leak instead of freeing if they do not
  // (this only happens on the process exit path: a leak is better than a use after
  // free).
  static constexpr int64_t WAIT_INTERVAL_US = 1000;
  static constexpr int64_t MAX_WAIT_US = 1000 * 1000;
  int64_t waited_us = 0;
  while (holder_cnt_.load() > 0 && waited_us < MAX_WAIT_US) {
    ob_usleep(WAIT_INTERVAL_US);
    waited_us += WAIT_INTERVAL_US;
  }
  if (holder_cnt_.load() > 0) {
    LOG_ERROR("skip clearing shared fts dictionaries, parsers still hold them",
              K(get_shared_dict_holder_cnt()), K(waited_us));
  } else {
    for (hash::ObHashMap<ObFTDictInfoKey, ObFTSharedDictItem *>::iterator iter = shared_dict_map_.begin();
         iter != shared_dict_map_.end();
         ++iter) {
      destroy_shared_dict_item(iter->second);
      iter->second = nullptr;
    }
    shared_dict_map_.reuse();
    shared_dict_alloc_.reset();
  }
  return ret;
}

int ObFTDictHub::build_shared_dict(const ObFTDictDesc &desc, ObFTSharedDictItem *&item)
{
  int ret = OB_SUCCESS;
  ObFTSharedDictItem *new_item = nullptr;
  if (OB_ISNULL(new_item = OB_NEWx(ObFTSharedDictItem, &shared_dict_alloc_, shared_dict_alloc_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to allocate shared dict item", K(ret), K(desc.type_));
  } else if (OB_FAIL(load_cache(desc, new_item->container_))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      ret = OB_SUCCESS;
      if (OB_FAIL(build_cache(desc, new_item->container_))) {
        LOG_WARN("failed to build dict cache", K(ret), K(desc.type_));
      }
    } else {
      LOG_WARN("failed to load dict cache", K(ret), K(desc.type_));
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_ISNULL(new_item->dict_ = OB_NEWx(ObFTRangeDict,
                                            &shared_dict_alloc_,
                                            shared_dict_alloc_,
                                            &new_item->container_,
                                            desc))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate shared range dict", K(ret), K(desc.type_));
    } else if (OB_FAIL(new_item->dict_->init())) {
      LOG_WARN("failed to init shared range dict", K(ret), K(desc.type_));
    }
  }
  if (OB_FAIL(ret)) {
    // The half built item is not reachable from the map, so nothing else would ever
    // release it, and the arena reset would skip its destructor: that would leak the
    // range dict arena and keep the dictionary cache handles pinned for the lifetime
    // of the process.
    destroy_shared_dict_item(new_item);
    if (OB_NOT_NULL(new_item)) {
      shared_dict_alloc_.free(new_item);
    }
    item = nullptr;
  } else {
    item = new_item;
  }
  return ret;
}

int ObFTDictHub::get_shared_dict(const ObFTDictInfoKey &key, ObFTSharedDictItem *&item)
{
  int ret = OB_SUCCESS;
  ObBucketHashRLockGuard guard(shared_dict_lock_, key.hash());
  if (OB_FAIL(shared_dict_map_.get_refactored(key, item))) {
    if (OB_HASH_NOT_EXIST != ret) {
      LOG_WARN("failed to get shared dict", K(ret));
    }
  }
  return ret;
}

int ObFTDictHub::put_shared_dict(const ObFTDictInfoKey &key, ObFTSharedDictItem *item)
{
  int ret = OB_SUCCESS;
  ObBucketHashWLockGuard guard(shared_dict_lock_, key.hash());
  if (OB_FAIL(shared_dict_map_.set_refactored(key, item))) {
    LOG_WARN("failed to set shared dict", K(ret));
  }
  return ret;
}

int ObFTDictHub::acquire_ik_dict(const ObFTDictDesc &desc, ObIFTDict *&dict)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  ObFTSharedDictItem *item = nullptr;
  dict = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(get_shared_dict(key, item))) {
    if (OB_HASH_NOT_EXIST != ret) {
      LOG_WARN("failed to get shared dict", K(ret), K(desc.type_));
    } else {
      ret = OB_SUCCESS;
      // The build mutex is deliberately not one of the bucket latches: load_cache()
      // and build_cache() take the dictionary cache bucket latch themselves, and a
      // bucket latch can not be taken twice by the same thread.
      lib::ObMutexGuard build_guard(build_lock_);
      if (OB_FAIL(get_shared_dict(key, item))) {
        if (OB_HASH_NOT_EXIST != ret) {
          LOG_WARN("failed to recheck shared dict", K(ret), K(desc.type_));
        } else {
          ret = OB_SUCCESS;
          if (OB_FAIL(build_shared_dict(desc, item))) {
            LOG_WARN("failed to build shared dict", K(ret), K(desc.type_));
          } else if (OB_FAIL(put_shared_dict(key, item))) {
            // The item never reached the map, so clear_shared_dicts() can not see it
            // and nobody else can be using it.
            LOG_WARN("failed to publish shared dict", K(ret), K(desc.type_));
            destroy_shared_dict_item(item);
            item = nullptr;
          }
        }
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(item)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    dict = item->dict_;
  }
  return ret;
}

int ObFTDictHub::acquire_ik_dicts(const ObFTDictDesc &main_desc,
                                  const ObFTDictDesc &quan_desc,
                                  ObIFTDict *&main_dict,
                                  ObIFTDict *&quan_dict)
{
  int ret = OB_SUCCESS;
  main_dict = nullptr;
  quan_dict = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else {
    // Register as a holder before touching the dictionaries so that
    // clear_shared_dicts() can never free them under this parser.
    holder_cnt_.fetch_add(1);
    if (OB_FAIL(acquire_ik_dict(main_desc, main_dict))) {
    } else if (OB_FAIL(acquire_ik_dict(quan_desc, quan_dict))) {
    }
    if (OB_FAIL(ret)) {
      holder_cnt_.fetch_sub(1);
      main_dict = nullptr;
      quan_dict = nullptr;
    }
  }
  return ret;
}

int ObFTDictHub::release_ik_dicts()
{
  int ret = OB_SUCCESS;
  const int64_t old_cnt = holder_cnt_.fetch_sub(1);
  if (OB_UNLIKELY(old_cnt <= 0)) {
    holder_cnt_.fetch_add(1);
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unbalanced shared fts dictionary holder reference", K(ret), K(old_cnt));
  }
  return ret;
}

int ObFTDictHub::build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  ObFTDictInfo info;
  container.reset();

  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());

    // try if valid with no recursive lock
    if (OB_FAIL(get_dict_info(key, info))) {
      if (OB_HASH_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
        ret = OB_ENTRY_NOT_EXIST;
      } else {
      }
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
      } else {
      }
    }

    if (OB_FAIL(ret)) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        if (OB_FAIL(ObFTRangeDict::build_cache_from_ik_dict(desc, container))) {
        } else if (FALSE_IT(info.range_count_ = container.get_handles().size())) {
        } else if (OB_FAIL(put_dict_info(key, info))) {
        }
      }
    }
  }
  return ret;
}

int ObFTDictHub::load_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfo info;
  container.reset();
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    {
      ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
      if (OB_FAIL(get_dict_info(key, info))) {
        if (OB_HASH_NOT_EXIST == ret) {
          // dict not exist, make new one, by caller
          ret = OB_ENTRY_NOT_EXIST;
        } else {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
      } else {
      }
    }
  }

  return ret;
}


int ObFTDictHub::get_dict_info(const ObFTDictInfoKey &key, ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(dict_map_.get_refactored(key, info))) {
    if (OB_HASH_NOT_EXIST != ret) {
    }
  }

  return ret;
}

int ObFTDictHub::put_dict_info(const ObFTDictInfoKey &key, const ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;
  const int cover_exist_flag = 1;
  if (OB_FAIL(dict_map_.set_refactored(key, info, cover_exist_flag))) {
  }

  return ret;
}
} //  namespace storage
} //  namespace oceanbase
