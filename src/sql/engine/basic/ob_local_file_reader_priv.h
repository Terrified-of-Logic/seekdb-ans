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

// Local file scan -- private interfaces of the format providers.
//
// The public entry points live in ob_local_file_reader.h.  This header is
// internal to the local-file scan implementation: it declares the tiny value
// objects and helpers exchanged between
//
//   ob_local_file_reader.cpp        reader facade, stream opening, fingerprints
//   ob_local_csv_provider.cpp       CSV record scanner + value classification
//   ob_local_jsonl_provider.cpp     JSONL line parser + key union
//   ob_local_file_schema.cpp        type merge / naming / cell coercion
//
// Nothing declared here may be used outside of this directory.

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_LOCAL_FILE_READER_PRIV_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_LOCAL_FILE_READER_PRIV_H_

#include <cstdint>
#include <istream>
#include <memory>
#include <string>
#include <vector>

#include "lib/ob_errno.h"
#include "sql/engine/basic/ob_local_file_reader.h"

namespace oceanbase
{
namespace sql
{
namespace local_file
{

// Safety limits applied while scanning local files.
constexpr int64_t MAX_FILE_BYTES = 1024L * 1024L * 1024L;
constexpr int64_t MAX_COLUMN_COUNT = 1024;
constexpr int64_t MAX_FIELD_BYTES = 16L * 1024L * 1024L;

// A source value classified before being coerced into the target column type.
// `text_` always keeps the original spelling so VARCHAR columns and errors can
// round trip unchanged.
struct ValueDraft
{
  ValueDraft()
    : kind_(ObLocalColumnType::NULL_TYPE), text_(), integer_(0), real_(0), boolean_(false)
  {}
  ObLocalColumnType kind_;
  std::string text_;
  int64_t integer_;
  double real_;
  bool boolean_;
};

// One "name: value" pair of a JSONL record, in document order.
struct JsonField
{
  JsonField() : name_(), value_() {}
  std::string name_;
  ValueDraft value_;
};

// Opens `path` as a regular file without following symlinks and, when asked,
// reports the identity of the opened descriptor.  Errors:
//   OB_FILE_NOT_OPENED -- open failed (missing file, EACCES, ...)
//   OB_INVALID_DATA    -- path is not a regular file
int open_local_stream(const std::string &path,
                      std::unique_ptr<std::streambuf> &buffer,
                      std::unique_ptr<std::istream> &stream,
                      uint64_t *opened_device = nullptr,
                      uint64_t *opened_inode = nullptr,
                      int64_t *opened_size = nullptr,
                      int64_t *opened_mtime_ns = nullptr);

// ASCII case-insensitive comparisons; the source data is not required to be
// valid UTF-8 and only bytes are touched.
bool name_equals_no_case(const std::string &left, const char *right);
bool name_equals_no_case(const std::string &left, const std::string &right);
bool ends_with_no_case(const std::string &value, const char *suffix);
bool is_blank_text(const std::string &line);

// Strict ISO date / datetime recognition.  Both return true only when the
// text matches the shape and the value is accepted by ObTimeConverter.
bool parse_iso_date(const std::string &text, int32_t &date_value);
bool parse_iso_datetime(const std::string &text, int64_t &datetime_value);

// Widening merge used by schema inference: NULL keeps the other side, numeric
// and temporal families widen to their wider member, everything else decays
// to VARCHAR.
ObLocalColumnType merge_column_type(ObLocalColumnType current, ObLocalColumnType incoming);

// Returns a column name that does not collide (case-insensitively) with the
// names already collected in `columns`.
std::string make_unique_column_name(const std::string &source_name,
                                    int64_t ordinal,
                                    const std::vector<ObLocalColumnSpec> &columns);

// Coerces a draft into the physical cell representation of `target_type`.
int draft_to_cell(const ValueDraft &draft, ObLocalColumnType target_type, ObLocalCell &cell);

// ---------------------------------------------------------------------------
// CSV provider
// ---------------------------------------------------------------------------
// Reads one comma separated record.  Returns OB_ITER_END at a clean EOF.  The
// scanner accepts quoted fields with doubled quotes, tolerates a trailing
// CRLF and rejects a quote inside an unquoted field.
int read_csv_record(std::istream &stream, std::vector<std::string> &fields, bool &has_record);
// Classifies one CSV field into the local value model.
int classify_csv_text(const std::string &text, ValueDraft &draft);
// Probes the CSV file and derives the column list plus the row count.
int infer_csv_schema(const std::string &path,
                     std::vector<ObLocalColumnSpec> &columns,
                     int64_t &row_count);

// ---------------------------------------------------------------------------
// JSONL provider
// ---------------------------------------------------------------------------
// Probes the JSONL file; the column order follows the first appearance of the
// keys and columns missing from a record are marked nullable.
int infer_jsonl_schema(const std::string &path,
                       std::vector<ObLocalColumnSpec> &columns,
                       int64_t &row_count);

} // namespace local_file
} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_LOCAL_FILE_READER_PRIV_H_
