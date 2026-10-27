/*
 * Copyright (c) 2026 OceanBase.
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

// Local file scan -- value model shared by the CSV and JSONL providers.
//
// This translation unit owns the pieces that must behave identically for every
// text format: case insensitive name comparison, strict temporal literals,
// schema type widening, column name deduplication and the final coercion from
// a parsed draft into an ObLocalCell.

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_local_file_reader_priv.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "common/timezone/ob_time_convert.h"
#include "lib/oblog/ob_log.h"
#include "share/ob_errno.h"

namespace oceanbase
{
using namespace common;
namespace sql
{
namespace local_file
{
namespace
{

inline char ascii_lower(const char ch)
{
  return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

// Compares two byte ranges ignoring ASCII case.  Both ranges are caller owned
// and need not be NUL terminated.
bool same_bytes_no_case(const char *lhs, const size_t lhs_len,
                        const char *rhs, const size_t rhs_len)
{
  bool same = lhs_len == rhs_len;
  for (size_t i = 0; same && i < lhs_len; ++i) {
    same = ascii_lower(lhs[i]) == ascii_lower(rhs[i]);
  }
  return same;
}

} // namespace

bool name_equals_no_case(const std::string &left, const char *right)
{
  return OB_NOT_NULL(right)
      && same_bytes_no_case(left.data(), left.size(), right, std::strlen(right));
}

bool name_equals_no_case(const std::string &left, const std::string &right)
{
  return same_bytes_no_case(left.data(), left.size(), right.data(), right.size());
}

bool ends_with_no_case(const std::string &value, const char *suffix)
{
  const size_t suffix_len = std::strlen(suffix);
  return value.size() >= suffix_len
      && same_bytes_no_case(value.data() + value.size() - suffix_len, suffix_len,
                            suffix, suffix_len);
}

bool is_blank_text(const std::string &line)
{
  bool blank = true;
  for (size_t i = 0; blank && i < line.size(); ++i) {
    blank = 0 != std::isspace(static_cast<unsigned char>(line[i]));
  }
  return blank;
}

bool parse_iso_date(const std::string &text, int32_t &date_value)
{
  // Accepted shape: YYYY-MM-DD, nothing else.
  bool shape_ok = 10 == text.length() && '-' == text[4] && '-' == text[7];
  for (size_t i = 0; shape_ok && i < text.length(); ++i) {
    if (4 != i && 7 != i) {
      shape_ok = 0 != std::isdigit(static_cast<unsigned char>(text[i]));
    }
  }
  bool valid = false;
  if (shape_ok) {
    const ObString literal(static_cast<int32_t>(text.length()), text.data());
    valid = OB_SUCCESS == ObTimeConverter::str_to_date(literal, date_value, 0);
  }
  return valid;
}

bool parse_iso_datetime(const std::string &text, int64_t &datetime_value)
{
  // Accepted shape: YYYY-MM-DD[T ]HH:MM:SS[.fraction], the fraction is
  // forwarded verbatim to the time converter.
  const size_t length = text.length();
  bool shape_ok = length >= 19 && length <= 26
      && '-' == text[4] && '-' == text[7]
      && (' ' == text[10] || 'T' == text[10])
      && ':' == text[13] && ':' == text[16];
  for (size_t i = 0; shape_ok && i < length; ++i) {
    if (4 == i || 7 == i || 10 == i || 13 == i || 16 == i) {
      // separators already verified above
    } else if (19 == i) {
      shape_ok = '.' == text[i];
    } else {
      shape_ok = 0 != std::isdigit(static_cast<unsigned char>(text[i]));
    }
  }
  bool valid = false;
  if (shape_ok) {
    std::string normalized(text);
    normalized[10] = ' ';
    const ObString literal(static_cast<int32_t>(normalized.length()), normalized.data());
    const ObTimeConvertCtx cvrt_ctx(nullptr, false);
    valid = OB_SUCCESS == ObTimeConverter::str_to_datetime(literal, cvrt_ctx,
                                                          datetime_value, nullptr, 0);
  }
  return valid;
}

ObLocalColumnType merge_column_type(const ObLocalColumnType current,
                                    const ObLocalColumnType incoming)
{
  ObLocalColumnType merged = current;
  if (ObLocalColumnType::NULL_TYPE == current) {
    merged = incoming;
  } else if (ObLocalColumnType::NULL_TYPE == incoming || current == incoming) {
    // nothing to widen
  } else if ((ObLocalColumnType::BIGINT == current && ObLocalColumnType::DOUBLE == incoming)
             || (ObLocalColumnType::DOUBLE == current && ObLocalColumnType::BIGINT == incoming)) {
    merged = ObLocalColumnType::DOUBLE;
  } else if ((ObLocalColumnType::DATE == current && ObLocalColumnType::DATETIME == incoming)
             || (ObLocalColumnType::DATETIME == current && ObLocalColumnType::DATE == incoming)) {
    merged = ObLocalColumnType::DATETIME;
  } else {
    merged = ObLocalColumnType::VARCHAR;
  }
  return merged;
}

std::string make_unique_column_name(const std::string &source_name,
                                    const int64_t ordinal,
                                    const std::vector<ObLocalColumnSpec> &columns)
{
  const std::string base_name = source_name.empty()
                              ? "_c" + std::to_string(ordinal + 1)
                              : source_name;
  std::string candidate = base_name;
  for (int64_t suffix = 2;; ++suffix) {
    bool taken = false;
    for (size_t i = 0; !taken && i < columns.size(); ++i) {
      taken = name_equals_no_case(candidate, columns[i].column_name_);
    }
    if (!taken) {
      break;
    }
    candidate = base_name + "__" + std::to_string(suffix);
  }
  return candidate;
}

int draft_to_cell(const ValueDraft &draft, const ObLocalColumnType target_type,
                  ObLocalCell &cell)
{
  int ret = OB_SUCCESS;
  cell.reset();
  if (ObLocalColumnType::NULL_TYPE == draft.kind_) {
    // leave the cell null
  } else {
    cell.null_ = false;
    switch (target_type) {
      case ObLocalColumnType::BOOLEAN:
        cell.flag_ = draft.boolean_;
        break;
      case ObLocalColumnType::BIGINT:
        cell.i64_ = draft.integer_;
        break;
      case ObLocalColumnType::DOUBLE:
        cell.f64_ = ObLocalColumnType::BIGINT == draft.kind_
                  ? static_cast<double>(draft.integer_)
                  : draft.real_;
        break;
      case ObLocalColumnType::DATE:
        if (!parse_iso_date(draft.text_, cell.d32_)) {
          ret = OB_INVALID_DATA;
        }
        break;
      case ObLocalColumnType::DATETIME: {
        bool valid = false;
        if (ObLocalColumnType::DATE == draft.kind_) {
          valid = parse_iso_datetime(draft.text_ + " 00:00:00", cell.dt64_);
        } else {
          valid = parse_iso_datetime(draft.text_, cell.dt64_);
        }
        if (!valid) {
          ret = OB_INVALID_DATA;
        }
        break;
      }
      case ObLocalColumnType::VARCHAR:
        cell.text_ = draft.text_;
        break;
      case ObLocalColumnType::NULL_TYPE:
        cell.null_ = true;
        break;
      default:
        ret = OB_NOT_SUPPORTED;
        break;
    }
  }
  return ret;
}

} // namespace local_file
} // namespace sql
} // namespace oceanbase
