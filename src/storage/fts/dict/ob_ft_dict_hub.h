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

#ifndef _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DICT_HUB_H_
#define _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DICT_HUB_H_

#include <atomic>

#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "lib/lock/ob_bucket_lock.h"
#include "lib/lock/ob_mutex.h"
#include "storage/fts/dict/ob_ft_dict_def.h"

namespace oceanbase
{
namespace storage
{
// typedef uint64_t ObFTTableID;
// typedef ObFTTableID ObFTDictTableID;

class ObFTDictInfo
{
public:
  ObFTDictInfo()
      : name_(""),
        type_(ObFTDictType::DICT_TYPE_INVALID),
        charset_(CHARSET_INVALID),
        version_(0),
        range_count_(0)
  {
  }

public:
  char name_[2048]; // for now
  ObFTDictType type_;
  ObCharsetType charset_;
  int64_t version_; // in memory
  int32_t range_count_;
};

struct ObFTDictInfoKey
{
public:
  ObFTDictInfoKey()
      : type_(static_cast<uint64_t>(ObFTDictType::DICT_TYPE_INVALID))
  {
  } // default constructor
  ObFTDictInfoKey(const uint64_t type)
      : type_(type)
  {
  }
  int hash(uint64_t &hash_value) const
  {
    int ret = OB_SUCCESS;
    hash_value = hash();
    return ret;
  }

  uint64_t hash() const
  {
    uint64_t hash = 0;
    hash = common::murmurhash(&type_, sizeof(int64_t), hash);
    return hash;
  }

  bool operator==(const ObFTDictInfoKey &other) const
  {
    return type_ == other.type_;
  }

  int compare(const ObFTDictInfoKey &other) const
  {
    int ret = 0;
    if (0 == ret) {
      ret = type_ - other.type_;
    }
    return ret;
  }

private:
  uint64_t type_;
  // name
};

class ObFTCacheRangeContainer;
class ObIFTDict;
class ObFTRangeDict;
struct ObFTSharedDictItem;

// A shared dictionary is immutable once it is built (the range dict only reads its
// dictionary cache container), so one instance per dictionary type can serve every
// parser of this process. That removes the per-document dictionary cache lookup and
// range-dict construction from the tokenize/build hot path.
//
// Ownership rules (they are the whole difficulty of the sharing):
//  - container and range dict live in the hub's own arena, and the container has to
//    outlive the range dict because the dict reads it;
//  - an arena reset never runs a destructor, so every path that drops an item has to
//    destroy the range dict first, then the item (whose destructor destroys the
//    container, and that is what releases the pinned dictionary cache handles);
//  - parsers never own a shared dictionary: they only hold a raw pointer while they
//    live, and report their existence through a holder counter.
class ObFTDictHub
{
public:
  ObFTDictHub()
      : is_inited_(false),
        dict_map_(),
        rw_dict_lock_(),
        shared_dict_map_(),
        shared_dict_lock_(),
        build_lock_(),
        shared_dict_alloc_(),
        holder_cnt_(0)
  {
  }
  ~ObFTDictHub();

  int init();

  int destroy();

  int build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container);

  int load_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container);

  // Acquire the shared IK dictionaries of one parser. The returned dictionaries are
  // owned by the hub; the caller must not destroy them, and must call
  // release_ik_dicts() exactly once when it stops using them.
  int acquire_ik_dicts(const ObFTDictDesc &main_desc,
                       const ObFTDictDesc &quan_desc,
                       ObIFTDict *&main_dict,
                       ObIFTDict *&quan_dict);

  // Drop the holder reference taken by one successful acquire_ik_dicts().
  int release_ik_dicts();

  int64_t get_shared_dict_holder_cnt() const { return holder_cnt_.load(); }

private:
  int get_dict_info(const ObFTDictInfoKey &key, ObFTDictInfo &info);

  int put_dict_info(const ObFTDictInfoKey &key, const ObFTDictInfo &info);

  int acquire_ik_dict(const ObFTDictDesc &desc, ObIFTDict *&dict);

  int get_shared_dict(const ObFTDictInfoKey &key, ObFTSharedDictItem *&item);

  int put_shared_dict(const ObFTDictInfoKey &key, ObFTSharedDictItem *item);

  int build_shared_dict(const ObFTDictDesc &desc, ObFTSharedDictItem *&item);

  // Destroy every shared dictionary. It must not run while a parser still holds one,
  // so it waits for the holders to drain and gives up (leaking) instead of freeing
  // dictionaries that somebody may still be matching against.
  int clear_shared_dicts();

private:
  static constexpr int K_MAX_SHARED_DICT_BUCKET = 128;
  bool is_inited_;
  // holds info of dict
  hash::ObHashMap<ObFTDictInfoKey, ObFTDictInfo> dict_map_;
  ObBucketLock rw_dict_lock_;
  // holds the process level shared immutable dictionaries
  hash::ObHashMap<ObFTDictInfoKey, ObFTSharedDictItem *> shared_dict_map_;
  ObBucketLock shared_dict_lock_;
  // one shot build mutex. It must never be nested inside a bucket latch: load_cache()
  // and build_cache() take the dict cache bucket latch themselves.
  lib::ObMutex build_lock_;
  // payload arena of the shared dictionaries
  ObArenaAllocator shared_dict_alloc_;
  // number of live parsers that hold a shared dictionary
  std::atomic<int64_t> holder_cnt_;
};

} //  namespace storage
} //  namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DICT_HUB_H_
