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

// Local file scan -- JSONL provider.
//
// Every non blank line must hold exactly one JSON object.  The column set of
// the table is the ordered union of the keys seen while probing the file; a
// key that is absent from a record simply leaves that column NULL.  Nested
// arrays and objects are rejected because the local table only exposes scalar
// columns.

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_local_file_reader_priv.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_parse.h"
#include "lib/allocator/page_arena.h"
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

// Maps one JSON scalar node onto the local value model.  The original text
// spelling is preserved for every scalar so that a later VARCHAR coercion is
// lossless.
int json_scalar_to_draft(const ObIJsonBase &node, ValueDraft &draft)
{
  int ret = OB_SUCCESS;
  draft = ValueDraft();
  switch (node.json_type()) {
    case ObJsonNodeType::J_NULL:
      draft.kind_ = ObLocalColumnType::NULL_TYPE;
      break;
    case ObJsonNodeType::J_BOOLEAN:
      draft.kind_ = ObLocalColumnType::BOOLEAN;
      draft.boolean_ = node.get_boolean();
      draft.text_ = draft.boolean_ ? "true" : "false";
      break;
    case ObJsonNodeType::J_INT:
    case ObJsonNodeType::J_OINT:
      draft.kind_ = ObLocalColumnType::BIGINT;
      draft.integer_ = node.get_int();
      draft.text_ = std::to_string(draft.integer_);
      break;
    case ObJsonNodeType::J_UINT:
    case ObJsonNodeType::J_OLONG: {
      const uint64_t wide = node.get_uint();
      if (wide <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        draft.kind_ = ObLocalColumnType::BIGINT;
        draft.integer_ = static_cast<int64_t>(wide);
      } else {
        draft.kind_ = ObLocalColumnType::DOUBLE;
        draft.real_ = static_cast<double>(wide);
      }
      draft.text_ = std::to_string(wide);
      break;
    }
    case ObJsonNodeType::J_DOUBLE:
    case ObJsonNodeType::J_ODOUBLE:
    case ObJsonNodeType::J_OFLOAT:
    case ObJsonNodeType::J_DECIMAL:
    case ObJsonNodeType::J_ODECIMAL: {
      double real = 0;
      if (OB_FAIL(node.to_double(real)) || !std::isfinite(real)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("jsonl number is not representable as double", K(ret));
      } else {
        char buffer[64];
        const int length = std::snprintf(buffer, sizeof(buffer), "%.17g", real);
        draft.kind_ = ObLocalColumnType::DOUBLE;
        draft.real_ = real;
        draft.text_.assign(buffer, length > 0 ? static_cast<size_t>(length) : 0);
      }
      break;
    }
    case ObJsonNodeType::J_STRING: {
      draft.text_.assign(node.get_data(), node.get_data_length());
      int32_t date_value = 0;
      int64_t datetime_value = 0;
      if (parse_iso_date(draft.text_, date_value)) {
        draft.kind_ = ObLocalColumnType::DATE;
      } else if (parse_iso_datetime(draft.text_, datetime_value)) {
        draft.kind_ = ObLocalColumnType::DATETIME;
      } else {
        draft.kind_ = ObLocalColumnType::VARCHAR;
      }
      break;
    }
    case ObJsonNodeType::J_ARRAY:
    case ObJsonNodeType::J_OBJECT:
      ret = OB_NOT_SUPPORTED;                  // nested values are not surfaced
      break;
    default:
      ret = OB_NOT_SUPPORTED;
      break;
  }
  return ret;
}

// Parses one line into the ordered field list of the record.  Duplicate keys
// are rejected by the parser itself (JSN_UNIQUE_FLAG).
int parse_json_line(const std::string &line, std::vector<JsonField> &fields)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("LocalJsonLine");
  ObIJsonBase *root = nullptr;
  fields.clear();
  const ObString json_text(static_cast<int32_t>(line.length()), line.data());
  const uint32_t parse_flags = ObJsonParser::JSN_STRICT_FLAG | ObJsonParser::JSN_UNIQUE_FLAG;
  if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator,
                                               json_text,
                                               ObJsonInType::JSON_TREE,
                                               ObJsonInType::JSON_TREE,
                                               root,
                                               parse_flags,
                                               100))) {
    LOG_WARN("failed to parse jsonl record", K(ret));
  } else if (OB_ISNULL(root) || ObJsonNodeType::J_OBJECT != root->json_type()) {
    ret = OB_INVALID_DATA;
    LOG_WARN("jsonl line is not a json object", K(ret));
  } else {
    for (uint64_t i = 0; OB_SUCC(ret) && i < root->element_count(); ++i) {
      ObString key;
      ObIJsonBase *child = nullptr;
      JsonField field;
      if (OB_FAIL(root->get_object_value(i, key, child))) {
        LOG_WARN("failed to read json object field", K(ret), K(i));
      } else if (OB_ISNULL(child)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(json_scalar_to_draft(*child, field.value_))) {
        LOG_WARN("unsupported jsonl field value", K(ret), K(key));
      } else {
        field.name_.assign(key.ptr(), key.length());
        fields.push_back(field);
      }
    }
  }
  return ret;
}

} // namespace

int infer_jsonl_schema(const std::string &path,
                       std::vector<ObLocalColumnSpec> &columns,
                       int64_t &row_count)
{
  int ret = OB_SUCCESS;
  std::unique_ptr<std::streambuf> stream_buffer;
  std::unique_ptr<std::istream> stream;
  std::string line;
  columns.clear();
  row_count = 0;
  if (OB_FAIL(open_local_stream(path, stream_buffer, stream))) {
    LOG_WARN("failed to open jsonl file", K(ret), KCSTRING(path.c_str()));
  }
  while (OB_SUCC(ret) && std::getline(*stream, line)) {
    if (line.length() > static_cast<size_t>(MAX_FIELD_BYTES)) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("jsonl line is too long", K(ret), K(line.length()));
      break;
    }
    if (!line.empty() && '\r' == line.back()) {
      line.pop_back();
    }
    if (is_blank_text(line)) {
      continue;
    }
    std::vector<JsonField> fields;
    if (OB_FAIL(parse_json_line(line, fields))) {
      LOG_WARN("failed to infer jsonl record", K(ret), K(row_count));
    } else {
      ++row_count;
      std::vector<bool> seen(columns.size(), false);
      for (size_t field_idx = 0; OB_SUCC(ret) && field_idx < fields.size(); ++field_idx) {
        int64_t column_idx = -1;
        for (size_t i = 0; -1 == column_idx && i < columns.size(); ++i) {
          if (columns[i].source_name_ == fields[field_idx].name_) {
            column_idx = static_cast<int64_t>(i);
          }
        }
        if (column_idx < 0) {
          ObLocalColumnSpec column;
          column.source_name_ = fields[field_idx].name_;
          column.column_name_ = make_unique_column_name(column.source_name_,
                                                        static_cast<int64_t>(columns.size()),
                                                        columns);
          column.nullable_ = row_count > 1;      // absent from earlier records
          column.type_ = fields[field_idx].value_.kind_;
          column.max_length_ = static_cast<int64_t>(fields[field_idx].value_.text_.length());
          columns.push_back(column);
          seen.push_back(true);
          if (columns.size() > static_cast<size_t>(MAX_COLUMN_COUNT)) {
            ret = OB_SIZE_OVERFLOW;
            LOG_WARN("too many jsonl columns", K(ret), K(columns.size()));
          }
        } else {
          ObLocalColumnSpec &column = columns[column_idx];
          seen[column_idx] = true;
          column.nullable_ = column.nullable_
              || ObLocalColumnType::NULL_TYPE == fields[field_idx].value_.kind_;
          column.type_ = merge_column_type(column.type_, fields[field_idx].value_.kind_);
          column.max_length_ = std::max(column.max_length_,
                                        static_cast<int64_t>(
                                          fields[field_idx].value_.text_.length()));
        }
      }
      for (size_t i = 0; i < seen.size(); ++i) {
        if (!seen[i]) {
          columns[i].nullable_ = true;          // this record omits the key
        }
      }
    }
  }
  if (OB_SUCC(ret) && !stream->eof() && stream->fail()) {
    ret = OB_IO_ERROR;
  }
  for (size_t i = 0; OB_SUCC(ret) && i < columns.size(); ++i) {
    columns[i].source_type_name_ = std::string("JSON/")
                                 + ObLocalFileSchema::column_type_name(columns[i].type_);
  }
  return ret;
}

} // namespace local_file


int ObLocalFileReader::get_next_jsonl_row(std::vector<ObLocalCell> &cells)
{
  int ret = OB_SUCCESS;
  std::string line;
  bool found = false;
  while (!found && std::getline(*input_stream_, line)) {
    ++rows_seen_;
    if (line.length() > static_cast<size_t>(local_file::MAX_FIELD_BYTES)) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("jsonl line is too long", K(ret), K(rows_seen_));
      break;
    }
    if (!line.empty() && '\r' == line.back()) {
      line.pop_back();
    }
    found = !local_file::is_blank_text(line);
  }
  if (OB_FAIL(ret)) {
  } else if (!found) {
    ret = OB_ITER_END;
  } else {
    std::vector<local_file::JsonField> fields;
    cells.assign(column_specs_.size(), ObLocalCell());
    if (OB_FAIL(local_file::parse_json_line(line, fields))) {
      LOG_WARN("failed to parse jsonl row", K(ret), K(rows_seen_));
    } else {
      for (size_t field_idx = 0; OB_SUCC(ret) && field_idx < fields.size(); ++field_idx) {
        for (size_t column_idx = 0; column_idx < column_specs_.size(); ++column_idx) {
          if (fields[field_idx].name_ == column_specs_[column_idx].source_name_) {
            if (projected_[column_idx]
                && OB_FAIL(local_file::draft_to_cell(fields[field_idx].value_,
                                         column_specs_[column_idx].type_,
                                         cells[column_idx]))) {
              LOG_WARN("failed to convert jsonl field", K(ret),
                       K(field_idx), K(rows_seen_));
            }
            break;
          }
        }
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
