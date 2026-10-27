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

// Local file scan -- CSV provider.
//
// The CSV dialect understood here is intentionally small:
//   * one header line, the header cells become the SQL column names
//   * ',' separates fields, '\n' or "\r\n" terminates a record
//   * a field quoted with '"' may contain separators and newlines, an embedded
//     quote is written as "" (two quote characters)
//   * quotes are only allowed at the very beginning of a field
//
// Schema inference reads the whole file once and widens the type of every
// column while doing so; row streaming re-parses each record on demand.

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_local_file_reader_priv.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "lib/oblog/ob_log.h"
#include "share/ob_errno.h"

namespace oceanbase
{
namespace sql
{
namespace local_file
{
namespace
{

// Pushes the field collected so far and ends the current record.
void close_csv_record(std::vector<std::string> &fields, std::string &field)
{
  fields.push_back(field);
}

// strtoll() with the additional requirement that the whole text was consumed.
bool scan_integer_text(const std::string &text, int64_t &value)
{
  char *stop = nullptr;
  errno = 0;
  const long long parsed = std::strtoll(text.c_str(), &stop, 10);
  const bool ok = 0 == errno && stop == text.c_str() + text.size();
  if (ok) {
    value = static_cast<int64_t>(parsed);
  }
  return ok;
}

// strtod() with the same "whole text consumed" rule plus a finiteness check.
bool scan_double_text(const std::string &text, double &value)
{
  char *stop = nullptr;
  errno = 0;
  const double parsed = std::strtod(text.c_str(), &stop);
  const bool ok = 0 == errno && stop == text.c_str() + text.size() && std::isfinite(parsed);
  if (ok) {
    value = parsed;
  }
  return ok;
}

// The header cell of a UTF-8 CSV file may start with a BOM; it is not part of
// the column name.
void strip_utf8_bom(std::string &first_field)
{
  if (first_field.size() >= 3
      && static_cast<unsigned char>(first_field[0]) == 0xEF
      && static_cast<unsigned char>(first_field[1]) == 0xBB
      && static_cast<unsigned char>(first_field[2]) == 0xBF) {
    first_field.erase(0, 3);
  }
}

int build_csv_header(const std::vector<std::string> &field_names,
                     std::vector<ObLocalColumnSpec> &columns)
{
  int ret = OB_SUCCESS;
  for (size_t i = 0; OB_SUCC(ret) && i < field_names.size(); ++i) {
    std::string source_name = field_names[i];
    if (0 == i) {
      strip_utf8_bom(source_name);
    }
    ObLocalColumnSpec column;
    column.source_name_ = source_name;
    column.column_name_ = make_unique_column_name(source_name, static_cast<int64_t>(i), columns);
    columns.push_back(column);
    if (columns.size() > static_cast<size_t>(MAX_COLUMN_COUNT)) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("too many csv columns", K(ret), K(columns.size()));
    }
  }
  return ret;
}

int scan_csv_rows(std::istream &stream,
                  std::vector<ObLocalColumnSpec> &columns,
                  int64_t &row_count)
{
  int ret = OB_SUCCESS;
  std::vector<std::string> fields;
  while (OB_SUCC(ret)) {
    fields.clear();
    bool has_record = false;
    ret = read_csv_record(stream, fields, has_record);
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
      break;
    } else if (OB_FAIL(ret)) {
      LOG_WARN("failed to read csv record while inferring schema", K(ret), K(row_count));
    } else if (fields.size() > columns.size()) {
      ret = OB_INVALID_DATA;
      LOG_WARN("csv record has more fields than the header", K(ret),
               K(fields.size()), K(columns.size()));
    } else {
      ++row_count;
      for (size_t i = 0; OB_SUCC(ret) && i < columns.size(); ++i) {
        ValueDraft draft;
        if (i < fields.size() && OB_FAIL(classify_csv_text(fields[i], draft))) {
          LOG_WARN("failed to classify csv field", K(ret), K(i), K(row_count));
        } else {
          columns[i].nullable_ = columns[i].nullable_
              || ObLocalColumnType::NULL_TYPE == draft.kind_;
          columns[i].type_ = merge_column_type(columns[i].type_, draft.kind_);
          columns[i].max_length_ = std::max(columns[i].max_length_,
                                            static_cast<int64_t>(draft.text_.length()));
        }
      }
    }
  }
  return ret;
}

} // namespace

int read_csv_record(std::istream &stream, std::vector<std::string> &fields, bool &has_record)
{
  // Scanner states: at the beginning of a field, inside an unquoted field,
  // inside a quoted field and right after the closing quote of a quoted field.
  enum ScanState { FIELD_START, PLAIN, QUOTED, QUOTE_DONE };

  int ret = OB_SUCCESS;
  fields.clear();
  has_record = false;
  std::string field;
  ScanState state = FIELD_START;
  bool quoted_field = false;
  bool record_done = false;
  while (OB_SUCC(ret) && !record_done) {
    const int next = stream.get();
    if (std::char_traits<char>::eof() == next) {
      if (QUOTED == state) {
        ret = OB_INVALID_DATA;                 // unterminated quoted field
      } else if (!has_record && field.empty() && fields.empty()) {
        ret = OB_ITER_END;                     // nothing left in the stream
      } else {
        close_csv_record(fields, field);
        has_record = true;
      }
      break;
    }
    has_record = true;
    const char ch = static_cast<char>(next);
    if (QUOTED == state) {
      if ('"' != ch) {
        field.push_back(ch);
        if (field.size() > static_cast<size_t>(MAX_FIELD_BYTES)) {
          ret = OB_SIZE_OVERFLOW;
        }
      } else if ('"' == stream.peek()) {
        stream.get();
        field.push_back('"');
      } else {
        state = QUOTE_DONE;
      }
    } else if (QUOTE_DONE == state) {
      if (',' == ch) {
        close_csv_record(fields, field);
        field.clear();
        quoted_field = false;
        state = FIELD_START;
      } else if ('\n' == ch) {
        close_csv_record(fields, field);
        record_done = true;
      } else if ('\r' == ch) {
        if ('\n' == stream.peek()) {
          stream.get();
        }
        close_csv_record(fields, field);
        record_done = true;
      } else {
        ret = OB_INVALID_DATA;                 // trailing garbage after quote
      }
    } else if (FIELD_START == state && '"' == ch) {
      state = QUOTED;
      quoted_field = true;
    } else if (',' == ch) {
      close_csv_record(fields, field);
      field.clear();
      quoted_field = false;
      state = FIELD_START;
    } else if ('\n' == ch) {
      close_csv_record(fields, field);
      record_done = true;
    } else if ('\r' == ch) {
      if ('\n' == stream.peek()) {
        stream.get();
      }
      close_csv_record(fields, field);
      record_done = true;
    } else if ('"' == ch && !quoted_field) {
      ret = OB_INVALID_DATA;                   // quote inside a plain field
    } else {
      field.push_back(ch);
      if (field.size() > static_cast<size_t>(MAX_FIELD_BYTES)) {
        ret = OB_SIZE_OVERFLOW;
      }
      state = PLAIN;
    }
  }
  return ret;
}

int classify_csv_text(const std::string &text, ValueDraft &draft)
{
  int ret = OB_SUCCESS;
  draft = ValueDraft();
  draft.text_ = text;
  if (text.empty()) {
    draft.kind_ = ObLocalColumnType::NULL_TYPE;
  } else if (name_equals_no_case(text, "true") || name_equals_no_case(text, "false")) {
    draft.kind_ = ObLocalColumnType::BOOLEAN;
    draft.boolean_ = name_equals_no_case(text, "true");
  } else if (scan_integer_text(text, draft.integer_)) {
    draft.kind_ = ObLocalColumnType::BIGINT;
  } else if (scan_double_text(text, draft.real_)) {
    draft.kind_ = ObLocalColumnType::DOUBLE;
  } else {
    // Temporal values are recognised here but re-parsed when the cell is
    // materialized, so only the kind matters at classification time.
    int32_t date_value = 0;
    int64_t datetime_value = 0;
    if (parse_iso_date(text, date_value)) {
      draft.kind_ = ObLocalColumnType::DATE;
    } else if (parse_iso_datetime(text, datetime_value)) {
      draft.kind_ = ObLocalColumnType::DATETIME;
    } else {
      draft.kind_ = ObLocalColumnType::VARCHAR;
    }
  }
  return ret;
}

int infer_csv_schema(const std::string &path,
                     std::vector<ObLocalColumnSpec> &columns,
                     int64_t &row_count)
{
  int ret = OB_SUCCESS;
  std::unique_ptr<std::streambuf> stream_buffer;
  std::unique_ptr<std::istream> stream;
  std::vector<std::string> header_fields;
  columns.clear();
  row_count = 0;
  if (OB_FAIL(open_local_stream(path, stream_buffer, stream))) {
    LOG_WARN("failed to open csv file", K(ret), KCSTRING(path.c_str()));
  } else {
    bool has_record = false;
    ret = read_csv_record(*stream, header_fields, has_record);
    if (OB_ITER_END == ret) {
      ret = OB_INVALID_DATA;                   // an empty file has no header
      LOG_WARN("csv file is empty", K(ret), KCSTRING(path.c_str()));
    } else if (OB_FAIL(ret)) {
      LOG_WARN("failed to read csv header", K(ret), KCSTRING(path.c_str()));
    } else if (OB_FAIL(build_csv_header(header_fields, columns))) {
      LOG_WARN("failed to build csv columns", K(ret));
    } else if (OB_FAIL(scan_csv_rows(*stream, columns, row_count))) {
      LOG_WARN("failed to infer csv column types", K(ret));
    }
  }
  for (size_t i = 0; OB_SUCC(ret) && i < columns.size(); ++i) {
    columns[i].source_type_name_ = std::string("CSV/")
                                 + ObLocalFileSchema::column_type_name(columns[i].type_);
  }
  return ret;
}

} // namespace local_file


int ObLocalFileReader::get_next_csv_row(std::vector<ObLocalCell> &cells)
{
  int ret = OB_SUCCESS;
  std::vector<std::string> fields;
  bool has_record = false;
  cells.assign(column_specs_.size(), ObLocalCell());
  if (!header_consumed_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(local_file::read_csv_record(*input_stream_, fields, has_record))) {
    // OB_ITER_END is a normal end of scan; every other code is a parse error.
  } else if (fields.size() > column_specs_.size()) {
    ret = OB_INVALID_DATA;
    LOG_WARN("csv record exceeds the resolved column count", K(ret),
             K(fields.size()), K(column_specs_.size()));
  } else {
    ++rows_seen_;
    for (size_t i = 0; OB_SUCC(ret) && i < fields.size(); ++i) {
      if (projected_[i]) {
        local_file::ValueDraft draft;
        if (OB_FAIL(local_file::classify_csv_text(fields[i], draft))) {
          LOG_WARN("failed to classify csv field", K(ret), K(i), K(rows_seen_));
        } else if (OB_FAIL(local_file::draft_to_cell(draft, column_specs_[i].type_, cells[i]))) {
          LOG_WARN("failed to convert csv field", K(ret), K(i), K(rows_seen_));
        }
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
