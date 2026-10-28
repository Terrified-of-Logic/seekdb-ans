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

#include "object/ob_object.h"
#define USING_LOG_PREFIX STORAGE_FTS

#include "share/rc/ob_server_runtime.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_fts_parser_helper.h"
#include "storage/fts/ob_fts_parser_property.h"

namespace oceanbase
{
namespace storage
{

////////////////////////////////////////////////////////////////////////////////
// class ObStopWordChecker
ObStopWordChecker::~ObStopWordChecker()
{
  destroy();
}

int ObStopWordChecker::init()
{
  int ret = OB_SUCCESS;
  
  

  if (inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(stopword_set_.create(DEFAULT_STOPWORD_BUCKET_NUM, "StopWordSet", "StopWordSet"))) {
  } else {
    ObObjMeta stop_meta;
    stop_meta.set_varchar();
    stop_meta.set_collation_type(ObCollationType::CS_TYPE_UTF8MB4_GENERAL_CI);

    stopword_type_.set_meta(stop_meta);
    const int64_t stopword_count = sizeof(ob_stop_word_list) / sizeof(ob_stop_word_list[0]);
    for (int64_t i = 0; OB_SUCC(ret) && i < stopword_count; ++i) {
      ObFTWord stopword(STRLEN(ob_stop_word_list[i]), ob_stop_word_list[i], stopword_type_);
      if (OB_FAIL(stopword_set_.set_refactored(stopword))) {
      }
    }

    if (OB_SUCC(ret)) {
      inited_ = true;
    }
  }
  return ret;
}

void ObStopWordChecker::destroy()
{
  if (inited_) {
    stopword_set_.destroy();
    inited_ = false;
  }
}

int ObStopWordChecker::check_stopword(const ObFTWord &word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // Fast path: the word already has the collation of the stop word set, so no charset conversion
    // (and no temporary allocator) is needed. Re-wrap the bytes with the set's own meta so that the
    // hash/compare functions are exactly the same as the slow path below uses.
    if (word.get_collation_type() == stopword_type_.get_collation_type()) {
      ObFTWord same_word(word.get_word().get_string().length(),
                         word.get_word().get_string().ptr(),
                         stopword_type_);
      ret = stopword_set_.exist_refactored(same_word);
    } else {
      common::ObArenaAllocator allocator(lib::ObMemAttr("ChkStopWord"));
      common::ObString cmp_str;
      if (OB_FAIL(common::ObCharset::charset_convert(
                                        allocator,
                                        word.get_word().get_string(),
                                        word.get_collation_type(),
                                        stopword_type_.get_collation_type(),
                                        cmp_str))) {
      } else {
        ObFTWord converted(cmp_str.length(), cmp_str.ptr(), stopword_type_);
        ret = stopword_set_.exist_refactored(converted);
      }
    }
    if (OB_HASH_NOT_EXIST == ret) {
      is_stopword = false;
      ret = OB_SUCCESS;
    } else if (OB_HASH_EXIST == ret) {
      is_stopword = true;
      ret = OB_SUCCESS;
    } else if (OB_SUCC(ret)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      // return the error of the hash set or of the charset conversion as is
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// class ObAddWord
ObAddWord::ObAddWord(
    const ObFTParserProperty &property,
    const ObObjMeta &meta,
    const ObAddWordFlag &flag,
    common::ObIAllocator &allocator,
    ObFTWordMap &word_map)
  : word_meta_(meta),
    allocator_(allocator),
    word_map_(word_map),
    min_max_word_cnt_(0),
    non_stopword_cnt_(0),
    stopword_cnt_(0),
    min_token_size_(property.min_token_size_),
    max_token_size_(property.max_token_size_),
    flag_(flag)
{
}

int ObAddWord::process_word(
    const char *word,
    const int64_t word_len,
    const int64_t char_cnt,
    const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  bool is_stopword = false;
  ObFTWord src_word(word_len, word, word_meta_);
  ObFTWord dst_word;
  if (OB_ISNULL(word) || OB_UNLIKELY(0 >= word_len || 0 >= char_cnt || 0 >= word_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (is_min_max_word(char_cnt)) {
    ++min_max_word_cnt_;
  } else if (OB_FAIL(casedown_word(src_word, dst_word))) {
  } else if (OB_FAIL(check_stopword(dst_word, is_stopword))) {
  } else if (OB_UNLIKELY(is_stopword)) {
    ++stopword_cnt_;
  } else if (OB_FAIL(groupby_word(dst_word, word_freq))) {
  } else {
    non_stopword_cnt_ += word_freq;
  }
  return ret;
}

bool ObAddWord::is_min_max_word(const int64_t c_len) const
{
  return flag_.min_max_word() && (c_len < min_token_size_ || c_len > max_token_size_);
}

// Fast path of casedown_word for the bulk of the mixed Chinese/English text: words made of ASCII
// only, or of 3-byte CJK code points (U+4000..U+9FFF, which have no case mapping at all and are
// copied byte for byte). Returns false as soon as a character that the generic case table has to
// look at is found, in which case the caller falls back to ObCharset::tolower. On success the
// output bytes are identical to what the case table would produce.
static bool casedown_fast_scan(const ObString &src, bool &has_upper)
{
  bool done = true;
  has_upper = false;
  const unsigned char *p = reinterpret_cast<const unsigned char *>(src.ptr());
  const int64_t len = src.length();
  for (int64_t i = 0; done && i < len;) {
    const unsigned char c = p[i];
    if (c < 0x80) {
      if (c >= 'A' && c <= 'Z') {
        has_upper = true;
      }
      ++i;
    } else if (c >= 0xE4 && c <= 0xE9 && i + 2 < len && (p[i + 1] & 0xC0) == 0x80
               && (p[i + 2] & 0xC0) == 0x80) {
      i += 3;
    } else {
      done = false;
    }
  }
  return done;
}

static void casedown_fast_write(const ObString &src, char *dst)
{
  const unsigned char *p = reinterpret_cast<const unsigned char *>(src.ptr());
  const int64_t len = src.length();
  for (int64_t i = 0; i < len;) {
    const unsigned char c = p[i];
    if (c < 0x80) {
      dst[i] = static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
      ++i;
    } else {
      dst[i] = static_cast<char>(c);
      dst[i + 1] = static_cast<char>(p[i + 1]);
      dst[i + 2] = static_cast<char>(p[i + 2]);
      i += 3;
    }
  }
}

// The "nothing to fold" fast path and the casedown-disabled path assign dst from
// src and therefore share the source bytes instead of copying them: the result is
// only valid while the source word buffer lives. Every caller (process_word)
// consumes it within the same parse step, so that holds.
int ObAddWord::casedown_word(const ObFTWord &src, ObFTWord &dst)
{
  int ret = OB_SUCCESS;
  char *buf = nullptr;
  bool has_upper = false;
  const ObString src_str = src.get_word().get_string();
  if (OB_UNLIKELY(src.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.casedown()
             && CHARSET_UTF8MB4 == ObCharset::charset_type_by_coll(word_meta_.get_collation_type())
             && casedown_fast_scan(src_str, has_upper)) {
    if (!has_upper) {
      // Nothing to fold, keep the source bytes and skip the copy altogether.
      dst = src;
    } else if (OB_ISNULL(buf = static_cast<char *>(allocator_.alloc(src_str.length())))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      casedown_fast_write(src_str, buf);
      ObFTWord tmp(src_str.length(), buf, word_meta_);
      dst = tmp;
    }
  } else if (flag_.casedown()) {
    ObString dst_str;
    if (OB_FAIL(ObCharset::tolower(
                    word_meta_.get_collation_type(),
                    src_str,
                    dst_str,
                    allocator_))) {
    } else {
      ObFTWord tmp(dst_str.length(), dst_str.ptr(), word_meta_);
      dst = tmp;
    }
  } else {
    dst = src;
  }
  return ret;
}

int ObAddWord::check_stopword(const ObFTWord &ft_word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(ft_word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.stopword()) {
    ObStopWordChecker *stop_word_checker = ObFTParseData::instance().stop_word_checker();
    if (OB_ISNULL(stop_word_checker)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(stop_word_checker->check_stopword(ft_word, is_stopword))) {
    }
  }
  return ret;
}

int ObAddWord::groupby_word(const ObFTWord &word, const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(word.empty() || word_freq <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!flag_.groupby_word()) {
    if (OB_FAIL(word_map_.set_refactored(word, 1/*word count*/))) {
    }
  } else {
    // One hash lookup instead of get_refactored + set_refactored: accumulate the frequency in
    // place when the word is already there, insert it with a count of one otherwise. The stored
    // key keeps the spelling of its first occurrence, exactly like the overwrite-based version.
    int64_t added_freq = word_freq;
    auto add_freq = [added_freq](auto &pair) { pair.second += added_freq; };
    if (OB_FAIL(word_map_.set_or_update(word, 1/*word count*/, add_freq))) {
    }
  }
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
