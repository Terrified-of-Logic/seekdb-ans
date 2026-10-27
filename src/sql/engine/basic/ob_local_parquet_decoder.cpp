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

// Local file scan -- Parquet decoder implementation.
//
// Layout of this file, top to bottom:
//   1. Thrift compact protocol cursor and the metadata descriptors it fills,
//   2. footer parsing (file metadata, schema elements, row groups, columns),
//   3. primitive value / level decoding helpers,
//   4. Snappy block decompression,
//   5. the page level decoder that stitches dictionary pages and data pages
//      into ObLocalCell vectors.
//
// Only a flat schema of primitive leaves is accepted: the resolver refuses
// nested groups, repeated fields and unsupported logical annotations instead
// of silently flattening them.
//
// Attribution: the decoding algorithm follows the Apache-2.0 reference
// implementation from oceanbase/seekdb PR #1251 (commit be9369f); this file
// has been renamed, reorganized and re-commented.  See
// docs/file_sql_design.md for the details of the divergence.
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_local_parquet_decoder.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

#include "lib/oblog/ob_log.h"
#include "share/ob_errno.h"

namespace oceanbase
{
namespace sql
{
namespace
{
enum ThriftFieldType : uint8_t
{
  TF_STOP = 0, TF_TRUE = 1, TF_FALSE = 2, TF_BYTE = 3, TF_I16 = 4,
  TF_I32 = 5, TF_I64 = 6, TF_DOUBLE = 7, TF_BINARY = 8, TF_LIST = 9,
  TF_SET = 10, TF_MAP = 11, TF_STRUCT = 12
};

enum ParquetPhysicalType : int32_t
{
  PHY_BOOLEAN = 0, PHY_INT32 = 1, PHY_INT64 = 2, PHY_INT96 = 3,
  PHY_FLOAT = 4, PHY_DOUBLE = 5, PHY_BYTE_ARRAY = 6, PHY_FIXED_LEN_BYTE_ARRAY = 7
};
enum ParquetPageEncoding : int32_t
{
  ENC_PLAIN = 0, ENC_PLAIN_DICTIONARY = 2, ENC_RLE = 3, ENC_BIT_PACKED = 4,
  ENC_RLE_DICTIONARY = 8
};
enum ParquetCompression : int32_t { CMP_UNCOMPRESSED = 0, CMP_SNAPPY = 1 };
enum ParquetPageKind : int32_t
{
  PGK_DATA_PAGE = 0, PGK_INDEX_PAGE = 1, PGK_DICTIONARY_PAGE = 2, PGK_DATA_PAGE_V2 = 3
};
enum ParquetLogicalKind : int32_t
{
  LOG_NONE = 0, LOG_STRING, LOG_DATE, LOG_TIMESTAMP_MILLIS, LOG_TIMESTAMP_MICROS,
  LOG_TIMESTAMP_NANOS, LOG_INTEGER, LOG_UNSUPPORTED
};

struct ThriftCursor
{
  ThriftCursor(const uint8_t *data, const size_t size) : data_(data), size_(size), pos_(0) {}
  bool read_byte(uint8_t &value)
  {
    const bool ok = pos_ < size_;
    if (ok) value = data_[pos_++];
    return ok;
  }
  bool consume_uvarint(uint64_t &value)
  {
    value = 0;
    int shift = 0;
    uint8_t byte = 0;
    bool ok = true;
    do {
      ok = shift < 64 && read_byte(byte);
      if (ok) {
        value |= static_cast<uint64_t>(byte & 0x7f) << shift;
        shift += 7;
      }
    } while (ok && 0 != (byte & 0x80));
    return ok;
  }
  bool read_i64(int64_t &value)
  {
    uint64_t encoded = 0;
    const bool ok = consume_uvarint(encoded);
    if (ok) value = static_cast<int64_t>((encoded >> 1) ^ (~(encoded & 1) + 1));
    return ok;
  }
  bool read_i32(int32_t &value)
  {
    int64_t wide = 0;
    const bool ok = read_i64(wide) && wide >= std::numeric_limits<int32_t>::min()
                                      && wide <= std::numeric_limits<int32_t>::max();
    if (ok) value = static_cast<int32_t>(wide);
    return ok;
  }
  bool read_binary(std::string &value)
  {
    uint64_t length = 0;
    const bool ok = consume_uvarint(length) && length <= size_ - pos_;
    if (ok) {
      value.assign(reinterpret_cast<const char *>(data_ + pos_), static_cast<size_t>(length));
      pos_ += static_cast<size_t>(length);
    }
    return ok;
  }
  bool read_field(int16_t &last_id, int16_t &field_id, uint8_t &type)
  {
    uint8_t header = 0;
    bool ok = read_byte(header);
    type = header & 0x0f;
    if (ok && TF_STOP != type) {
      const int16_t delta = static_cast<int16_t>(header >> 4);
      if (0 != delta) {
        field_id = last_id + delta;
      } else {
        int64_t id = 0;
        ok = read_i64(id) && id > 0 && id <= std::numeric_limits<int16_t>::max();
        field_id = static_cast<int16_t>(id);
      }
      if (ok) last_id = field_id;
    }
    return ok;
  }
  bool read_list_header(uint8_t &element_type, uint64_t &size)
  {
    uint8_t header = 0;
    bool ok = read_byte(header);
    element_type = header & 0x0f;
    size = header >> 4;
    if (ok && 15 == size) ok = consume_uvarint(size);
    return ok;
  }
  bool skip(uint8_t type)
  {
    bool ok = true;
    uint8_t byte = 0;
    uint64_t size = 0;
    switch (type) {
      case TF_TRUE: case TF_FALSE: break;
      case TF_BYTE: ok = read_byte(byte); break;
      case TF_I16: case TF_I32: case TF_I64: ok = consume_uvarint(size); break;
      case TF_DOUBLE:
        ok = size_ - pos_ >= sizeof(double); if (ok) pos_ += sizeof(double); break;
      case TF_BINARY:
        ok = consume_uvarint(size) && size <= size_ - pos_;
        if (ok) pos_ += static_cast<size_t>(size);
        break;
      case TF_LIST: case TF_SET: {
        uint8_t element_type = 0;
        ok = read_list_header(element_type, size);
        for (uint64_t i = 0; ok && i < size; ++i) ok = skip(element_type);
        break;
      }
      case TF_MAP: {
        ok = consume_uvarint(size);
        uint8_t types = 0;
        if (ok && size > 0) ok = read_byte(types);
        for (uint64_t i = 0; ok && i < size; ++i) {
          ok = skip(types >> 4) && skip(types & 0x0f);
        }
        break;
      }
      case TF_STRUCT: {
        int16_t last = 0, id = 0;
        uint8_t field_type = 0;
        do {
          ok = read_field(last, id, field_type);
          if (ok && TF_STOP != field_type) ok = skip(field_type);
        } while (ok && TF_STOP != field_type);
        break;
      }
      default: ok = false; break;
    }
    return ok;
  }
  const uint8_t *data_;
  size_t size_;
  size_t pos_;
};

bool read_compact_integer(ThriftCursor &cursor, const uint8_t type, int64_t &value)
{
  return (TF_I16 == type || TF_I32 == type || TF_I64 == type) && cursor.read_i64(value);
}

struct ParquetFieldMeta
{
  ParquetFieldMeta() : physical_type_(-1), type_length_(0), repetition_(0), name_(),
                    num_children_(0), converted_type_(-1), scale_(0), precision_(0),
                    logical_annotation_(LOG_NONE), integer_bit_width_(0), integer_signed_(true) {}
  int32_t physical_type_;
  int32_t type_length_;
  int32_t repetition_;
  std::string name_;
  int32_t num_children_;
  int32_t converted_type_;
  int32_t scale_;
  int32_t precision_;
  int32_t logical_annotation_;
  int32_t integer_bit_width_;
  bool integer_signed_;
};

bool parse_timestamp_unit(ThriftCursor &cursor, int32_t &annotation)
{
  bool ok = true, found = false;
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  do {
    ok = cursor.read_field(last, id, type);
    if (!ok || TF_STOP == type) {
    } else if (TF_STRUCT == type && id >= 1 && id <= 3) {
      ok = cursor.skip(type);
      if (ok) {
        annotation = 1 == id ? LOG_TIMESTAMP_MILLIS
                   : 2 == id ? LOG_TIMESTAMP_MICROS : LOG_TIMESTAMP_NANOS;
        found = true;
      }
    } else {
      ok = cursor.skip(type);
    }
  } while (ok && TF_STOP != type);
  return ok && found;
}

bool parse_timestamp_logical(ThriftCursor &cursor, int32_t &annotation)
{
  bool ok = true, found_unit = false;
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  do {
    ok = cursor.read_field(last, id, type);
    if (!ok || TF_STOP == type) {
    } else if (2 == id && TF_STRUCT == type) {
      ok = parse_timestamp_unit(cursor, annotation);
      found_unit = ok;
    } else {
      ok = cursor.skip(type);
    }
  } while (ok && TF_STOP != type);
  return ok && found_unit;
}

bool parse_integer_logical(ThriftCursor &cursor, ParquetFieldMeta &element)
{
  bool ok = true, found_width = false, found_signed = false;
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  do {
    ok = cursor.read_field(last, id, type);
    if (!ok || TF_STOP == type) {
    } else if (1 == id && TF_BYTE == type) {
      uint8_t width = 0;
      ok = cursor.read_byte(width);
      if (ok) { element.integer_bit_width_ = width; found_width = true; }
    } else if (2 == id && (TF_TRUE == type || TF_FALSE == type)) {
      element.integer_signed_ = TF_TRUE == type;
      found_signed = true;
    } else {
      ok = cursor.skip(type);
    }
  } while (ok && TF_STOP != type);
  return ok && found_width && found_signed;
}

bool parse_logical_kind(ThriftCursor &cursor, ParquetFieldMeta &element)
{
  bool ok = true, found = false;
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  do {
    ok = cursor.read_field(last, id, type);
    if (!ok || TF_STOP == type) {
    } else if (TF_STRUCT != type) {
      ok = cursor.skip(type);
    } else if (1 == id || 6 == id) {
      ok = cursor.skip(type);
      if (ok) {
        element.logical_annotation_ = 1 == id ? LOG_STRING : LOG_DATE;
        found = true;
      }
    } else if (8 == id) {
      ok = parse_timestamp_logical(cursor, element.logical_annotation_);
      found = ok;
    } else if (9 == id) {
      element.logical_annotation_ = LOG_INTEGER;
      ok = parse_integer_logical(cursor, element);
      found = ok;
    } else {
      element.logical_annotation_ = LOG_UNSUPPORTED;
      ok = cursor.skip(type);
      found = ok;
    }
  } while (ok && TF_STOP != type);
  return ok && found;
}

struct ParquetLeafMeta
{
  ParquetLeafMeta() : physical_type_(-1), codec_(CMP_UNCOMPRESSED), num_values_(0),
                 total_compressed_size_(0), data_page_offset_(-1), dictionary_page_offset_(-1) {}
  int32_t physical_type_;
  std::vector<std::string> file_path_;
  int32_t codec_;
  int64_t num_values_;
  int64_t total_compressed_size_;
  int64_t data_page_offset_;
  int64_t dictionary_page_offset_;
};

struct ParquetGroupMeta
{
  ParquetGroupMeta() : columns_(), num_rows_(0) {}
  std::vector<ParquetLeafMeta> columns_;
  int64_t num_rows_;
};

struct ParquetFileMeta
{
  std::vector<ParquetFieldMeta> schema_;
  std::vector<ParquetGroupMeta> row_groups_;
  int64_t num_rows_ = 0;
};

bool parse_field_meta(ThriftCursor &cursor, ParquetFieldMeta &element)
{
  bool ok = true;
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  do {
    ok = cursor.read_field(last, id, type);
    int64_t value = 0;
    if (!ok || TF_STOP == type) {
    } else if (1 == id && read_compact_integer(cursor, type, value)) {
      element.physical_type_ = static_cast<int32_t>(value);
    } else if (2 == id && read_compact_integer(cursor, type, value)) {
      element.type_length_ = static_cast<int32_t>(value);
    } else if (3 == id && read_compact_integer(cursor, type, value)) {
      element.repetition_ = static_cast<int32_t>(value);
    } else if (4 == id && TF_BINARY == type) {
      ok = cursor.read_binary(element.name_);
    } else if (5 == id && read_compact_integer(cursor, type, value)) {
      element.num_children_ = static_cast<int32_t>(value);
    } else if (6 == id && read_compact_integer(cursor, type, value)) {
      element.converted_type_ = static_cast<int32_t>(value);
    } else if (7 == id && read_compact_integer(cursor, type, value)) {
      element.scale_ = static_cast<int32_t>(value);
    } else if (8 == id && read_compact_integer(cursor, type, value)) {
      element.precision_ = static_cast<int32_t>(value);
    } else if (10 == id && TF_STRUCT == type) {
      ok = parse_logical_kind(cursor, element);
    } else {
      ok = cursor.skip(type);
    }
  } while (ok && TF_STOP != type);
  return ok && !element.name_.empty();
}

bool parse_leaf_meta(ThriftCursor &cursor, ParquetLeafMeta &meta)
{
  bool ok = true;
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  do {
    ok = cursor.read_field(last, id, type);
    int64_t value = 0;
    if (!ok || TF_STOP == type) {
    } else if (1 == id && read_compact_integer(cursor, type, value)) {
      meta.physical_type_ = static_cast<int32_t>(value);
    } else if (3 == id && TF_LIST == type) {
      uint8_t element_type = 0; uint64_t count = 0;
      ok = cursor.read_list_header(element_type, count) && TF_BINARY == element_type;
      for (uint64_t i = 0; ok && i < count; ++i) {
        std::string path;
        ok = cursor.read_binary(path);
        if (ok) meta.file_path_.push_back(path);
      }
    } else if (4 == id && read_compact_integer(cursor, type, value)) {
      meta.codec_ = static_cast<int32_t>(value);
    } else if (5 == id && read_compact_integer(cursor, type, value)) {
      meta.num_values_ = value;
    } else if (7 == id && read_compact_integer(cursor, type, value)) {
      meta.total_compressed_size_ = value;
    } else if (9 == id && read_compact_integer(cursor, type, value)) {
      meta.data_page_offset_ = value;
    } else if (11 == id && read_compact_integer(cursor, type, value)) {
      meta.dictionary_page_offset_ = value;
    } else {
      ok = cursor.skip(type);
    }
  } while (ok && TF_STOP != type);
  return ok && meta.physical_type_ >= 0 && meta.num_values_ >= 0
            && meta.data_page_offset_ >= 0 && !meta.file_path_.empty();
}

bool parse_leaf_chunk(ThriftCursor &cursor, ParquetLeafMeta &meta)
{
  bool ok = true, found = false;
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  do {
    ok = cursor.read_field(last, id, type);
    if (!ok || TF_STOP == type) {
    } else if (3 == id && TF_STRUCT == type) {
      ok = parse_leaf_meta(cursor, meta); found = ok;
    } else {
      ok = cursor.skip(type);
    }
  } while (ok && TF_STOP != type);
  return ok && found;
}

bool parse_group_meta(ThriftCursor &cursor, ParquetGroupMeta &group)
{
  bool ok = true;
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  do {
    ok = cursor.read_field(last, id, type);
    int64_t value = 0;
    if (!ok || TF_STOP == type) {
    } else if (1 == id && TF_LIST == type) {
      uint8_t element_type = 0; uint64_t count = 0;
      ok = cursor.read_list_header(element_type, count) && TF_STRUCT == element_type;
      for (uint64_t i = 0; ok && i < count; ++i) {
        ParquetLeafMeta meta;
        ok = parse_leaf_chunk(cursor, meta);
        if (ok) group.columns_.push_back(meta);
      }
    } else if (3 == id && read_compact_integer(cursor, type, value)) {
      group.num_rows_ = value;
    } else {
      ok = cursor.skip(type);
    }
  } while (ok && TF_STOP != type);
  return ok && group.num_rows_ >= 0 && !group.columns_.empty();
}

bool parse_footer(const uint8_t *data, const size_t size, ParquetFileMeta &meta)
{
  ThriftCursor cursor(data, size);
  bool ok = true;
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  do {
    ok = cursor.read_field(last, id, type);
    int64_t value = 0;
    if (!ok || TF_STOP == type) {
    } else if (2 == id && TF_LIST == type) {
      uint8_t element_type = 0; uint64_t count = 0;
      ok = cursor.read_list_header(element_type, count) && TF_STRUCT == element_type && count <= 1025;
      for (uint64_t i = 0; ok && i < count; ++i) {
        ParquetFieldMeta element;
        ok = parse_field_meta(cursor, element);
        if (ok) meta.schema_.push_back(element);
      }
    } else if (3 == id && read_compact_integer(cursor, type, value)) {
      meta.num_rows_ = value;
    } else if (4 == id && TF_LIST == type) {
      uint8_t element_type = 0; uint64_t count = 0;
      ok = cursor.read_list_header(element_type, count) && TF_STRUCT == element_type && count <= 1000000;
      for (uint64_t i = 0; ok && i < count; ++i) {
        ParquetGroupMeta group;
        ok = parse_group_meta(cursor, group);
        if (ok) meta.row_groups_.push_back(group);
      }
    } else {
      ok = cursor.skip(type);
    }
  } while (ok && TF_STOP != type);
  return ok && meta.num_rows_ >= 0 && meta.schema_.size() >= 2;
}

uint32_t decode_u32_le(const uint8_t *data)
{
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8)
       | (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}
uint64_t decode_u64_le(const uint8_t *data)
{
  return static_cast<uint64_t>(decode_u32_le(data)) | (static_cast<uint64_t>(decode_u32_le(data + 4)) << 32);
}

int pread_loop(const int fd, const int64_t offset, uint8_t *data, const size_t size)
{
  int ret = OB_SUCCESS;
  size_t done = 0;
  while (OB_SUCC(ret) && done < size) {
    const ssize_t count = pread(fd, data + done, size - done, offset + done);
    if (count > 0) done += static_cast<size_t>(count);
    else ret = OB_IO_ERROR;
  }
  return ret;
}

int read_parquet_footer(const int fd, const int64_t file_size, ParquetFileMeta &meta)
{
  int ret = OB_SUCCESS;
  uint8_t magic[4];
  uint8_t trailer[8];
  if (file_size < 12 || OB_FAIL(pread_loop(fd, 0, magic, sizeof(magic)))
      || OB_FAIL(pread_loop(fd, file_size - 8, trailer, sizeof(trailer)))) {
    if (OB_SUCC(ret)) ret = OB_INVALID_DATA;
  } else if (0 != memcmp(magic, "PAR1", 4) || 0 != memcmp(trailer + 4, "PAR1", 4)) {
    ret = OB_INVALID_DATA;
  } else {
    const uint32_t metadata_size = decode_u32_le(trailer);
    if (metadata_size > 64U * 1024U * 1024U
        || static_cast<int64_t>(metadata_size) > file_size - 12) {
      ret = OB_SIZE_OVERFLOW;
    } else {
      std::vector<uint8_t> bytes(metadata_size);
      if (OB_FAIL(pread_loop(fd, file_size - 8 - metadata_size, bytes.data(), bytes.size()))) {
        LOG_WARN("failed to read parquet footer", K(ret));
      } else if (!parse_footer(bytes.data(), bytes.size(), meta)) {
        ret = OB_INVALID_DATA;
      }
    }
  }
  return ret;
}

bool column_name_equal_ci(const std::string &left, const std::string &right)
{
  bool equal = left.size() == right.size();
  for (size_t i = 0; equal && i < left.size(); ++i) {
    equal = std::tolower(static_cast<unsigned char>(left[i]))
         == std::tolower(static_cast<unsigned char>(right[i]));
  }
  return equal;
}

std::string unique_column_name(const std::string &source, const std::vector<ObLocalColumnSpec> &columns)
{
  std::string base = source.empty() ? "_c" + std::to_string(columns.size() + 1) : source;
  std::string result = base;
  for (int64_t suffix = 2;; ++suffix) {
    bool duplicate = false;
    for (size_t i = 0; !duplicate && i < columns.size(); ++i) {
      duplicate = column_name_equal_ci(result, columns[i].column_name_);
    }
    if (!duplicate) break;
    result = base + "__" + std::to_string(suffix);
  }
  return result;
}

int build_columns_from_footer(const ParquetFileMeta &meta, std::vector<ObLocalColumnSpec> &columns)
{
  int ret = OB_SUCCESS;
  columns.clear();
  if (meta.schema_.empty() || meta.schema_[0].num_children_ <= 0
      || meta.schema_[0].num_children_ != static_cast<int32_t>(meta.schema_.size() - 1)) {
    ret = OB_NOT_SUPPORTED; // MVP supports a flat primitive schema.
  }
  for (size_t i = 1; OB_SUCC(ret) && i < meta.schema_.size(); ++i) {
    const ParquetFieldMeta &element = meta.schema_[i];
    ObLocalColumnSpec column;
    if (element.num_children_ > 0 || 2 == element.repetition_) {
      ret = OB_NOT_SUPPORTED;
    } else if (LOG_UNSUPPORTED == element.logical_annotation_
               || (LOG_INTEGER == element.logical_annotation_
                   && !element.integer_signed_ && 64 == element.integer_bit_width_)) {
      ret = OB_NOT_SUPPORTED;
    } else if (PHY_BOOLEAN == element.physical_type_) {
      column.type_ = ObLocalColumnType::BOOLEAN;
      column.source_type_name_ = "BOOLEAN";
    } else if (PHY_INT32 == element.physical_type_) {
      column.type_ = (6 == element.converted_type_ || LOG_DATE == element.logical_annotation_)
                   ? ObLocalColumnType::DATE : ObLocalColumnType::BIGINT;
      column.source_type_name_ = ObLocalColumnType::DATE == column.type_ ? "INT32/DATE" : "INT32";
    } else if (PHY_INT64 == element.physical_type_) {
      column.type_ = (9 == element.converted_type_ || 10 == element.converted_type_
                      || LOG_TIMESTAMP_MILLIS == element.logical_annotation_
                      || LOG_TIMESTAMP_MICROS == element.logical_annotation_
                      || LOG_TIMESTAMP_NANOS == element.logical_annotation_)
                   ? ObLocalColumnType::DATETIME : ObLocalColumnType::BIGINT;
      if (LOG_TIMESTAMP_NANOS == element.logical_annotation_) {
        column.source_type_name_ = "INT64/TIMESTAMP_NANOS";
      } else if (10 == element.converted_type_ || LOG_TIMESTAMP_MICROS == element.logical_annotation_) {
        column.source_type_name_ = "INT64/TIMESTAMP_MICROS";
      } else if (9 == element.converted_type_ || LOG_TIMESTAMP_MILLIS == element.logical_annotation_) {
        column.source_type_name_ = "INT64/TIMESTAMP_MILLIS";
      } else {
        column.source_type_name_ = "INT64";
      }
    } else if (PHY_INT96 == element.physical_type_) {
      column.type_ = ObLocalColumnType::DATETIME;
      column.source_type_name_ = "INT96";
    } else if (PHY_FLOAT == element.physical_type_ || PHY_DOUBLE == element.physical_type_) {
      column.type_ = ObLocalColumnType::DOUBLE;
      column.source_type_name_ = PHY_FLOAT == element.physical_type_ ? "FLOAT" : "DOUBLE";
    } else if (PHY_BYTE_ARRAY == element.physical_type_
               || PHY_FIXED_LEN_BYTE_ARRAY == element.physical_type_) {
      column.type_ = ObLocalColumnType::VARCHAR;
      column.max_length_ = std::max<int32_t>(element.type_length_, 65535);
      if (PHY_FIXED_LEN_BYTE_ARRAY == element.physical_type_) {
        column.source_type_name_ = "FIXED_LEN_BYTE_ARRAY(" + std::to_string(element.type_length_) + ")";
      } else if (0 == element.converted_type_ || LOG_STRING == element.logical_annotation_) {
        column.source_type_name_ = "BYTE_ARRAY/UTF8";
      } else {
        column.source_type_name_ = "BYTE_ARRAY";
      }
    } else {
      ret = OB_NOT_SUPPORTED;
    }
    if (OB_SUCC(ret)) {
      column.source_name_ = element.name_;
      column.column_name_ = unique_column_name(element.name_, columns);
      column.nullable_ = 1 == element.repetition_;
      columns.push_back(column);
    }
  }
  for (size_t group_idx = 0; OB_SUCC(ret) && group_idx < meta.row_groups_.size(); ++group_idx) {
    const ParquetGroupMeta &group = meta.row_groups_[group_idx];
    if (group.columns_.size() != columns.size()
        || group.num_rows_ > 1024L * 1024L
        || (!columns.empty()
            && group.num_rows_ > 1024L * 1024L / static_cast<int64_t>(columns.size()))) {
      ret = OB_NOT_SUPPORTED;
    } else {
      for (size_t column_idx = 0; OB_SUCC(ret) && column_idx < columns.size(); ++column_idx) {
        const ParquetLeafMeta &chunk = group.columns_[column_idx];
        const ParquetFieldMeta &schema = meta.schema_[column_idx + 1];
        if (chunk.physical_type_ != schema.physical_type_
            || chunk.file_path_.size() != 1 || chunk.file_path_[0] != schema.name_) {
          ret = OB_INVALID_DATA;
        }
      }
    }
  }
  int64_t total_rows = 0;
  for (size_t i = 0; OB_SUCC(ret) && i < meta.row_groups_.size(); ++i) {
    if (meta.row_groups_[i].num_rows_ > meta.num_rows_ - total_rows) ret = OB_INVALID_DATA;
    else total_rows += meta.row_groups_[i].num_rows_;
  }
  if (OB_SUCC(ret) && total_rows != meta.num_rows_) ret = OB_INVALID_DATA;
  return ret;
}

int open_with_fingerprint(const std::string &path, const uint64_t expected_device,
                  const uint64_t expected_inode, const int64_t expected_size,
                  const int64_t expected_mtime_ns, int &fd)
{
  int ret = OB_SUCCESS;
  fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  struct stat st;
  if (fd < 0 || 0 != fstat(fd, &st) || !S_ISREG(st.st_mode)) {
    if (fd >= 0) ::close(fd);
    fd = -1;
    ret = OB_FILE_NOT_OPENED;
  } else {
    const int64_t mtime_ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000L
                           + static_cast<int64_t>(st.st_mtim.tv_nsec);
    if ((0 != expected_device && static_cast<uint64_t>(st.st_dev) != expected_device)
        || (0 != expected_inode && static_cast<uint64_t>(st.st_ino) != expected_inode)
        || (expected_size >= 0 && static_cast<int64_t>(st.st_size) != expected_size)
        || (expected_mtime_ns >= 0 && mtime_ns != expected_mtime_ns)) {
      ::close(fd); fd = -1; ret = OB_SCHEMA_EAGAIN;
    }
  }
  return ret;
}

struct ParquetPageHeader
{
  ParquetPageHeader()
    : type_(-1), uncompressed_size_(-1), compressed_size_(-1), num_values_(-1),
      encoding_(-1), definition_encoding_(-1), repetition_encoding_(-1),
      num_nulls_(0), num_rows_(-1), definition_length_(0), repetition_length_(0),
      is_compressed_(true)
  {}
  int32_t type_;
  int32_t uncompressed_size_;
  int32_t compressed_size_;
  int32_t num_values_;
  int32_t encoding_;
  int32_t definition_encoding_;
  int32_t repetition_encoding_;
  int32_t num_nulls_;
  int32_t num_rows_;
  int32_t definition_length_;
  int32_t repetition_length_;
  bool is_compressed_;
};

bool parse_data_page_one(ThriftCursor &cursor, ParquetPageHeader &header)
{
  bool ok = true;
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  do {
    ok = cursor.read_field(last, id, type);
    int64_t value = 0;
    if (!ok || TF_STOP == type) {
    } else if (1 == id && read_compact_integer(cursor, type, value)) {
      header.num_values_ = static_cast<int32_t>(value);
    } else if (2 == id && read_compact_integer(cursor, type, value)) {
      header.encoding_ = static_cast<int32_t>(value);
    } else if (3 == id && read_compact_integer(cursor, type, value)) {
      header.definition_encoding_ = static_cast<int32_t>(value);
    } else if (4 == id && read_compact_integer(cursor, type, value)) {
      header.repetition_encoding_ = static_cast<int32_t>(value);
    } else {
      ok = cursor.skip(type);
    }
  } while (ok && TF_STOP != type);
  return ok && header.num_values_ >= 0 && header.encoding_ >= 0;
}

bool parse_dict_page(ThriftCursor &cursor, ParquetPageHeader &header)
{
  bool ok = true;
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  do {
    ok = cursor.read_field(last, id, type);
    int64_t value = 0;
    if (!ok || TF_STOP == type) {
    } else if (1 == id && read_compact_integer(cursor, type, value)) {
      header.num_values_ = static_cast<int32_t>(value);
    } else if (2 == id && read_compact_integer(cursor, type, value)) {
      header.encoding_ = static_cast<int32_t>(value);
    } else {
      ok = cursor.skip(type);
    }
  } while (ok && TF_STOP != type);
  return ok && header.num_values_ >= 0 && header.encoding_ >= 0;
}

bool parse_data_page_two(ThriftCursor &cursor, ParquetPageHeader &header)
{
  bool ok = true;
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  do {
    ok = cursor.read_field(last, id, type);
    int64_t value = 0;
    if (!ok || TF_STOP == type) {
    } else if (1 == id && read_compact_integer(cursor, type, value)) {
      header.num_values_ = static_cast<int32_t>(value);
    } else if (2 == id && read_compact_integer(cursor, type, value)) {
      header.num_nulls_ = static_cast<int32_t>(value);
    } else if (3 == id && read_compact_integer(cursor, type, value)) {
      header.num_rows_ = static_cast<int32_t>(value);
    } else if (4 == id && read_compact_integer(cursor, type, value)) {
      header.encoding_ = static_cast<int32_t>(value);
    } else if (5 == id && read_compact_integer(cursor, type, value)) {
      header.definition_length_ = static_cast<int32_t>(value);
    } else if (6 == id && read_compact_integer(cursor, type, value)) {
      header.repetition_length_ = static_cast<int32_t>(value);
    } else if (7 == id && (TF_TRUE == type || TF_FALSE == type)) {
      header.is_compressed_ = TF_TRUE == type;
    } else {
      ok = cursor.skip(type);
    }
  } while (ok && TF_STOP != type);
  return ok && header.num_values_ >= 0 && header.num_rows_ >= 0 && header.encoding_ >= 0
         && header.definition_length_ >= 0 && header.repetition_length_ >= 0;
}

bool parse_page_header_block(const uint8_t *data, const size_t size,
                       ParquetPageHeader &header, size_t &consumed)
{
  ThriftCursor cursor(data, size);
  bool ok = true;
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  do {
    ok = cursor.read_field(last, id, type);
    int64_t value = 0;
    if (!ok || TF_STOP == type) {
    } else if (1 == id && read_compact_integer(cursor, type, value)) {
      header.type_ = static_cast<int32_t>(value);
    } else if (2 == id && read_compact_integer(cursor, type, value)) {
      header.uncompressed_size_ = static_cast<int32_t>(value);
    } else if (3 == id && read_compact_integer(cursor, type, value)) {
      header.compressed_size_ = static_cast<int32_t>(value);
    } else if (5 == id && TF_STRUCT == type) {
      ok = parse_data_page_one(cursor, header);
    } else if (7 == id && TF_STRUCT == type) {
      ok = parse_dict_page(cursor, header);
    } else if (8 == id && TF_STRUCT == type) {
      ok = parse_data_page_two(cursor, header);
    } else {
      ok = cursor.skip(type);
    }
  } while (ok && TF_STOP != type);
  consumed = cursor.pos_;
  return ok && header.type_ >= 0 && header.uncompressed_size_ >= 0
         && header.compressed_size_ >= 0;
}

bool consume_uvarint(const uint8_t *data, const size_t size, size_t &pos, uint64_t &value)
{
  value = 0;
  int shift = 0;
  uint8_t byte = 0;
  bool ok = true;
  do {
    ok = pos < size && shift < 64;
    if (ok) {
      byte = data[pos++];
      value |= static_cast<uint64_t>(byte & 0x7f) << shift;
      shift += 7;
    }
  } while (ok && 0 != (byte & 0x80));
  return ok;
}

int uncompress_snappy_block(const uint8_t *data, const size_t size,
                      const size_t expected_size, std::vector<uint8_t> &output)
{
  int ret = OB_SUCCESS;
  size_t pos = 0;
  uint64_t declared_size = 0;
  output.clear();
  if (!consume_uvarint(data, size, pos, declared_size)
      || declared_size != expected_size || declared_size > 64U * 1024U * 1024U) {
    ret = OB_INVALID_DATA;
  } else {
    output.reserve(static_cast<size_t>(declared_size));
  }
  while (OB_SUCC(ret) && pos < size && output.size() < declared_size) {
    const uint8_t tag = data[pos++];
    const uint8_t kind = tag & 0x03;
    size_t length = 0;
    size_t offset = 0;
    if (0 == kind) {
      const uint8_t encoded = tag >> 2;
      if (encoded < 60) {
        length = static_cast<size_t>(encoded) + 1;
      } else {
        const size_t length_bytes = static_cast<size_t>(encoded) - 59;
        if (length_bytes > 4 || length_bytes > size - pos) {
          ret = OB_INVALID_DATA;
        } else {
          uint32_t length_minus_one = 0;
          for (size_t i = 0; i < length_bytes; ++i) {
            length_minus_one |= static_cast<uint32_t>(data[pos++]) << (8 * i);
          }
          length = static_cast<size_t>(length_minus_one) + 1;
        }
      }
      if (OB_SUCC(ret) && (length > size - pos || length > declared_size - output.size())) {
        ret = OB_INVALID_DATA;
      } else if (OB_SUCC(ret)) {
        output.insert(output.end(), data + pos, data + pos + length);
        pos += length;
      }
    } else {
      if (1 == kind) {
        length = 4 + ((tag >> 2) & 0x07);
        if (pos >= size) ret = OB_INVALID_DATA;
        else offset = (static_cast<size_t>(tag & 0xe0) << 3) | data[pos++];
      } else if (2 == kind) {
        length = 1 + (tag >> 2);
        if (size - pos < 2) ret = OB_INVALID_DATA;
        else { offset = static_cast<size_t>(data[pos]) | (static_cast<size_t>(data[pos + 1]) << 8); pos += 2; }
      } else {
        length = 1 + (tag >> 2);
        if (size - pos < 4) ret = OB_INVALID_DATA;
        else { offset = decode_u32_le(data + pos); pos += 4; }
      }
      if (OB_SUCC(ret) && (0 == offset || offset > output.size()
                           || length > declared_size - output.size())) {
        ret = OB_INVALID_DATA;
      }
      for (size_t i = 0; OB_SUCC(ret) && i < length; ++i) {
        output.push_back(output[output.size() - offset]);
      }
    }
  }
  if (OB_SUCC(ret) && (output.size() != declared_size || pos != size)) ret = OB_INVALID_DATA;
  return ret;
}

int decode_rle_hybrid(const uint8_t *data, const size_t size, const uint8_t bit_width,
                  const size_t value_count, std::vector<uint32_t> &values)
{
  int ret = OB_SUCCESS;
  size_t pos = 0;
  values.clear();
  if (bit_width > 32) ret = OB_NOT_SUPPORTED;
  while (OB_SUCC(ret) && values.size() < value_count) {
    uint64_t header = 0;
    if (!consume_uvarint(data, size, pos, header) || 0 == header) {
      ret = OB_INVALID_DATA;
    } else if (0 == (header & 1)) {
      const uint64_t run_length = header >> 1;
      const size_t byte_width = (bit_width + 7) / 8;
      uint32_t value = 0;
      if (run_length > value_count - values.size() || byte_width > size - pos) {
        ret = OB_INVALID_DATA;
      } else {
        for (size_t i = 0; i < byte_width; ++i) value |= static_cast<uint32_t>(data[pos++]) << (8 * i);
        values.insert(values.end(), static_cast<size_t>(run_length), value);
      }
    } else {
      const uint64_t groups = header >> 1;
      if (groups > (std::numeric_limits<size_t>::max() / 8)) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        const size_t run_values = static_cast<size_t>(groups) * 8;
        const size_t run_bytes = (run_values * bit_width + 7) / 8;
        if (run_bytes > size - pos) {
          ret = OB_INVALID_DATA;
        } else {
          for (size_t i = 0; i < run_values && values.size() < value_count; ++i) {
            uint32_t value = 0;
            for (uint8_t bit = 0; bit < bit_width; ++bit) {
              const size_t absolute_bit = i * bit_width + bit;
              value |= ((data[pos + absolute_bit / 8] >> (absolute_bit % 8)) & 1U) << bit;
            }
            values.push_back(value);
          }
          pos += run_bytes;
        }
      }
    }
  }
  return ret;
}

int decode_definition_levels(const uint8_t *data, const size_t size, const size_t value_count,
                  const bool length_prefixed, std::vector<uint32_t> &levels, size_t &consumed)
{
  int ret = OB_SUCCESS;
  consumed = 0;
  if (length_prefixed) {
    if (size < 4) ret = OB_INVALID_DATA;
    else {
      const uint32_t encoded_size = decode_u32_le(data);
      if (encoded_size > size - 4) ret = OB_INVALID_DATA;
      else if (OB_FAIL(decode_rle_hybrid(data + 4, encoded_size, 1, value_count, levels))) {}
      else consumed = 4 + encoded_size;
    }
  } else if (OB_FAIL(decode_rle_hybrid(data, size, 1, value_count, levels))) {
  } else {
    consumed = size;
  }
  return ret;
}

int decode_plain_cells(const uint8_t *data, const size_t size, const size_t value_count,
                        const ParquetFieldMeta &schema, std::vector<ObLocalCell> &values,
                        size_t &consumed)
{
  int ret = OB_SUCCESS;
  consumed = 0;
  values.assign(value_count, ObLocalCell());
  for (size_t i = 0; OB_SUCC(ret) && i < value_count; ++i) {
    ObLocalCell &cell = values[i];
    cell.null_ = false;
    switch (schema.physical_type_) {
      case PHY_BOOLEAN: {
        const size_t byte = i / 8;
        if (byte >= size) ret = OB_INVALID_DATA;
        else cell.flag_ = 0 != ((data[byte] >> (i % 8)) & 1);
        consumed = (value_count + 7) / 8;
        break;
      }
      case PHY_INT32:
        if (size - consumed < 4) ret = OB_INVALID_DATA;
        else {
          const int32_t value = static_cast<int32_t>(decode_u32_le(data + consumed));
          if (6 == schema.converted_type_ || LOG_DATE == schema.logical_annotation_) cell.d32_ = value;
          else cell.i64_ = value;
          consumed += 4;
        }
        break;
      case PHY_INT64:
        if (size - consumed < 8) ret = OB_INVALID_DATA;
        else {
          const int64_t value = static_cast<int64_t>(decode_u64_le(data + consumed));
          if (9 == schema.converted_type_ || LOG_TIMESTAMP_MILLIS == schema.logical_annotation_) {
            if (value > std::numeric_limits<int64_t>::max() / 1000L
                || value < std::numeric_limits<int64_t>::min() / 1000L) ret = OB_SIZE_OVERFLOW;
            else cell.dt64_ = value * 1000L;
          } else if (10 == schema.converted_type_ || LOG_TIMESTAMP_MICROS == schema.logical_annotation_) {
            cell.dt64_ = value;
          } else if (LOG_TIMESTAMP_NANOS == schema.logical_annotation_) {
            cell.dt64_ = value / 1000L;
          }
          else cell.i64_ = value;
          consumed += 8;
        }
        break;
      case PHY_INT96:
        if (size - consumed < 12) ret = OB_INVALID_DATA;
        else {
          const uint64_t nanos_of_day = decode_u64_le(data + consumed);
          const int64_t julian_day = decode_u32_le(data + consumed + 8);
          if (nanos_of_day >= 86400ULL * 1000000000ULL) {
            ret = OB_INVALID_DATA;
          } else {
            cell.dt64_ = (julian_day - 2440588L) * 86400L * 1000000L
                                 + static_cast<int64_t>(nanos_of_day / 1000L);
            consumed += 12;
          }
        }
        break;
      case PHY_FLOAT:
        if (size - consumed < 4) ret = OB_INVALID_DATA;
        else {
          const uint32_t bits = decode_u32_le(data + consumed);
          float value = 0;
          memcpy(&value, &bits, sizeof(value));
          cell.f64_ = value;
          consumed += 4;
        }
        break;
      case PHY_DOUBLE:
        if (size - consumed < 8) ret = OB_INVALID_DATA;
        else {
          const uint64_t bits = decode_u64_le(data + consumed);
          memcpy(&cell.f64_, &bits, sizeof(bits));
          consumed += 8;
        }
        break;
      case PHY_BYTE_ARRAY: {
        if (size - consumed < 4) ret = OB_INVALID_DATA;
        else {
          const uint32_t length = decode_u32_le(data + consumed);
          consumed += 4;
          if (length > 16U * 1024U * 1024U || length > size - consumed) ret = OB_SIZE_OVERFLOW;
          else {
            cell.text_.assign(reinterpret_cast<const char *>(data + consumed), length);
            consumed += length;
          }
        }
        break;
      }
      case PHY_FIXED_LEN_BYTE_ARRAY:
        if (schema.type_length_ <= 0 || static_cast<size_t>(schema.type_length_) > size - consumed) {
          ret = OB_INVALID_DATA;
        } else {
          cell.text_.assign(reinterpret_cast<const char *>(data + consumed), schema.type_length_);
          consumed += schema.type_length_;
        }
        break;
      default: ret = OB_NOT_SUPPORTED; break;
    }
  }
  return ret;
}

int uncompress_page(const int32_t codec, const uint8_t *data, const size_t compressed_size,
                    const size_t uncompressed_size, std::vector<uint8_t> &output)
{
  int ret = OB_SUCCESS;
  if (CMP_UNCOMPRESSED == codec) {
    if (compressed_size != uncompressed_size) ret = OB_INVALID_DATA;
    else output.assign(data, data + compressed_size);
  } else if (CMP_SNAPPY == codec) {
    ret = uncompress_snappy_block(data, compressed_size, uncompressed_size, output);
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

} // namespace

class ObLocalParquetDecoder::DecoderState
{
public:
  DecoderState() : file_fd_(-1), file_bytes_(0), file_path_(), footer_(), columns_(), projected_(),
           group_cursor_(0), row_cursor_(0), decoded_columns_() {}
  ~DecoderState() { close(); }
  void close()
  {
    if (file_fd_ >= 0) ::close(file_fd_);
    file_fd_ = -1;
    file_bytes_ = 0;
    footer_ = ParquetFileMeta();
    columns_.clear();
    projected_.clear();
    decoded_columns_.clear();
    group_cursor_ = 0;
    row_cursor_ = 0;
  }
  int decode_current_group();
  int decode_leaf_column(const ParquetGroupMeta &group, const size_t column_idx,
                    std::vector<ObLocalCell> &values);

  int file_fd_;
  int64_t file_bytes_;
  std::string file_path_;
  ParquetFileMeta footer_;
  std::vector<ObLocalColumnSpec> columns_;
  std::vector<bool> projected_;
  size_t group_cursor_;
  int64_t row_cursor_;
  std::vector<std::vector<ObLocalCell> > decoded_columns_;
};

int ObLocalParquetDecoder::DecoderState::decode_current_group()
{
  int ret = OB_SUCCESS;
  row_cursor_ = 0;
  decoded_columns_.assign(columns_.size(), std::vector<ObLocalCell>());
  if (group_cursor_ >= footer_.row_groups_.size()) {
    ret = OB_ITER_END;
  } else {
    const ParquetGroupMeta &group = footer_.row_groups_[group_cursor_];
    for (size_t i = 0; OB_SUCC(ret) && i < columns_.size(); ++i) {
      if (!projected_[i]) {
        // The row group cursor is driven by metadata; no bytes from this column
        // chunk are read when the column is not referenced by the SQL plan.
      } else if (OB_FAIL(decode_leaf_column(group, i, decoded_columns_[i]))) {
        LOG_WARN("failed to decode parquet column", K(ret), K(i), K(group_cursor_));
      } else if (decoded_columns_[i].size() != static_cast<size_t>(group.num_rows_)) {
        ret = OB_INVALID_DATA;
      }
    }
  }
  return ret;
}

int ObLocalParquetDecoder::DecoderState::decode_leaf_column(const ParquetGroupMeta &group, const size_t column_idx,
                                         std::vector<ObLocalCell> &values)
{
  int ret = OB_SUCCESS;
  values.clear();
  if (column_idx >= group.columns_.size() || column_idx + 1 >= footer_.schema_.size()) {
    ret = OB_INVALID_DATA;
  } else {
    const ParquetLeafMeta &column = group.columns_[column_idx];
    const ParquetFieldMeta &schema = footer_.schema_[column_idx + 1];
    int64_t offset = column.dictionary_page_offset_ >= 0
                   ? std::min(column.dictionary_page_offset_, column.data_page_offset_)
                   : column.data_page_offset_;
    const int64_t chunk_end = offset + column.total_compressed_size_;
    std::vector<ObLocalCell> dictionary;
    if (offset < 4 || column.total_compressed_size_ < 0 || chunk_end < offset
        || chunk_end > file_bytes_ || column.num_values_ != group.num_rows_) {
      ret = OB_INVALID_DATA;
    }
    while (OB_SUCC(ret) && offset < chunk_end
           && values.size() < static_cast<size_t>(column.num_values_)) {
      const size_t available = static_cast<size_t>(chunk_end - offset);
      const size_t header_probe = std::min<size_t>(available, 1024U * 1024U);
      std::vector<uint8_t> header_bytes(header_probe);
      ParquetPageHeader header;
      size_t header_size = 0;
      if (OB_FAIL(pread_loop(file_fd_, offset, header_bytes.data(), header_bytes.size()))) {
      } else if (!parse_page_header_block(header_bytes.data(), header_bytes.size(), header, header_size)) {
        ret = OB_INVALID_DATA;
      } else if (header.compressed_size_ > 64 * 1024 * 1024
                 || header.uncompressed_size_ > 64 * 1024 * 1024
                 || header.num_values_ > 1024 * 1024
                 || header_size > available
                 || static_cast<size_t>(header.compressed_size_) > available - header_size) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        std::vector<uint8_t> payload(static_cast<size_t>(header.compressed_size_));
        if (!payload.empty()
            && OB_FAIL(pread_loop(file_fd_, offset + header_size, payload.data(), payload.size()))) {
        } else {
          offset += static_cast<int64_t>(header_size + payload.size());
          if (PGK_INDEX_PAGE == header.type_) {
            // Index pages do not contain row values.
          } else if (PGK_DICTIONARY_PAGE == header.type_) {
            std::vector<uint8_t> plain;
            size_t consumed = 0;
            if ((ENC_PLAIN != header.encoding_ && ENC_PLAIN_DICTIONARY != header.encoding_)
                || !dictionary.empty()) {
              ret = OB_NOT_SUPPORTED;
            } else if (OB_FAIL(uncompress_page(column.codec_, payload.data(), payload.size(),
                                               header.uncompressed_size_, plain))) {
            } else if (OB_FAIL(decode_plain_cells(plain.data(), plain.size(), header.num_values_,
                                                   schema, dictionary, consumed))) {
            } else if (consumed != plain.size()) {
              ret = OB_INVALID_DATA;
            }
          } else if (PGK_DATA_PAGE == header.type_ || PGK_DATA_PAGE_V2 == header.type_) {
            const bool v2 = PGK_DATA_PAGE_V2 == header.type_;
            std::vector<uint8_t> page;
            const uint8_t *definition_data = nullptr;
            size_t definition_size = 0;
            const uint8_t *encoded_values = nullptr;
            size_t encoded_values_size = 0;
            if (!v2) {
              if (OB_FAIL(uncompress_page(column.codec_, payload.data(), payload.size(),
                                          header.uncompressed_size_, page))) {
              } else {
                definition_data = page.data();
                definition_size = page.size();
              }
            } else if (header.repetition_length_ != 0
                       || header.definition_length_ + header.repetition_length_ > header.compressed_size_
                       || header.definition_length_ + header.repetition_length_ > header.uncompressed_size_) {
              ret = OB_NOT_SUPPORTED;
            } else {
              definition_data = payload.data() + header.repetition_length_;
              definition_size = header.definition_length_;
              const size_t value_compressed_size = payload.size() - header.repetition_length_
                                                - header.definition_length_;
              const size_t value_uncompressed_size = header.uncompressed_size_
                                                   - header.repetition_length_
                                                   - header.definition_length_;
              if (header.is_compressed_) {
                if (OB_FAIL(uncompress_page(column.codec_,
                                            payload.data() + header.repetition_length_
                                                           + header.definition_length_,
                                            value_compressed_size, value_uncompressed_size, page))) {
                } else {
                  encoded_values = page.data();
                  encoded_values_size = page.size();
                }
              } else if (value_compressed_size != value_uncompressed_size) {
                ret = OB_INVALID_DATA;
              } else {
                encoded_values = payload.data() + header.repetition_length_ + header.definition_length_;
                encoded_values_size = value_compressed_size;
              }
            }

            std::vector<uint32_t> levels;
            size_t level_bytes = 0;
            const bool optional = 1 == schema.repetition_;
            if (OB_SUCC(ret) && optional) {
              if (!v2 && ENC_RLE != header.definition_encoding_) {
                ret = OB_NOT_SUPPORTED;
              } else if (OB_FAIL(decode_definition_levels(definition_data, definition_size,
                                               header.num_values_, !v2, levels, level_bytes))) {
              }
            } else if (OB_SUCC(ret)) {
              levels.assign(static_cast<size_t>(header.num_values_), 1);
            }
            if (OB_SUCC(ret) && !v2) {
              if (level_bytes > page.size()) ret = OB_INVALID_DATA;
              else {
                encoded_values = page.data() + level_bytes;
                encoded_values_size = page.size() - level_bytes;
              }
            }
            size_t present_count = 0;
            for (size_t i = 0; OB_SUCC(ret) && i < levels.size(); ++i) {
              if (levels[i] > 1) ret = OB_INVALID_DATA;
              else if (1 == levels[i]) ++present_count;
            }
            if (OB_SUCC(ret) && v2
                && present_count != static_cast<size_t>(header.num_values_ - header.num_nulls_)) {
              ret = OB_INVALID_DATA;
            }
            std::vector<ObLocalCell> present_values;
            size_t consumed = 0;
            if (OB_SUCC(ret) && ENC_PLAIN == header.encoding_) {
              ret = decode_plain_cells(encoded_values, encoded_values_size, present_count,
                                        schema, present_values, consumed);
            } else if (OB_SUCC(ret)
                       && (ENC_RLE_DICTIONARY == header.encoding_
                           || ENC_PLAIN_DICTIONARY == header.encoding_)) {
              if (0 == present_count) {
                // An all-null page may legally use dictionary encoding without
                // containing either dictionary values or dictionary indices.
              } else if (dictionary.empty() || 0 == encoded_values_size) {
                ret = OB_INVALID_DATA;
              } else {
                const uint8_t bit_width = encoded_values[0];
                std::vector<uint32_t> indices;
                if (OB_FAIL(decode_rle_hybrid(encoded_values + 1, encoded_values_size - 1,
                                          bit_width, present_count, indices))) {
                } else {
                  for (size_t i = 0; OB_SUCC(ret) && i < indices.size(); ++i) {
                    if (indices[i] >= dictionary.size()) ret = OB_INVALID_DATA;
                    else present_values.push_back(dictionary[indices[i]]);
                  }
                }
              }
            } else if (OB_SUCC(ret)) {
              ret = OB_NOT_SUPPORTED;
            }
            if (OB_SUCC(ret) && ENC_PLAIN == header.encoding_ && consumed != encoded_values_size) {
              ret = OB_INVALID_DATA;
            }
            if (OB_SUCC(ret)) {
              size_t present_idx = 0;
              for (size_t i = 0; i < levels.size(); ++i) {
                if (0 == levels[i]) values.push_back(ObLocalCell());
                else values.push_back(present_values[present_idx++]);
              }
            }
          } else {
            ret = OB_NOT_SUPPORTED;
          }
        }
      }
    }
    if (OB_SUCC(ret) && values.size() != static_cast<size_t>(column.num_values_)) {
      ret = OB_INVALID_DATA;
    }
  }
  return ret;
}

ObLocalParquetDecoder::ObLocalParquetDecoder() : state_(new (std::nothrow) DecoderState()) {}
ObLocalParquetDecoder::~ObLocalParquetDecoder() {}

int ObLocalParquetDecoder::infer_schema(const std::string &path,
                                  std::vector<ObLocalColumnSpec> &columns,
                                  int64_t &row_count)
{
  int ret = OB_SUCCESS;
  int fd = -1;
  struct stat st;
  ParquetFileMeta meta;
  if (OB_FAIL(open_with_fingerprint(path, 0, 0, -1, -1, fd))) {
    LOG_WARN("failed to open parquet file", K(ret));
  } else if (0 != fstat(fd, &st)) {
    ret = OB_IO_ERROR;
  } else if (OB_FAIL(read_parquet_footer(fd, st.st_size, meta))) {
    LOG_WARN("failed to read parquet metadata", K(ret));
  } else if (OB_FAIL(build_columns_from_footer(meta, columns))) {
    LOG_WARN("unsupported parquet schema", K(ret));
  } else {
    row_count = meta.num_rows_;
  }
  if (fd >= 0) ::close(fd);
  return ret;
}

int ObLocalParquetDecoder::open(const std::string &path,
                          const std::vector<ObLocalColumnSpec> &columns,
                          const uint64_t expected_device,
                          const uint64_t expected_inode,
                          const int64_t expected_file_size,
                          const int64_t expected_modified_time_ns,
                          const std::vector<bool> &projected_columns)
{
  int ret = OB_SUCCESS;
  if (!state_) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    state_->close();
    state_->file_path_ = path;
    state_->columns_ = columns;
    state_->projected_ = projected_columns;
    if (projected_columns.size() != columns.size()) {
      ret = OB_INVALID_ARGUMENT;
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(open_with_fingerprint(path, expected_device, expected_inode, expected_file_size,
                              expected_modified_time_ns, state_->file_fd_))) {
      LOG_WARN("failed to open verified parquet file", K(ret));
    } else {
      struct stat st;
      if (0 != fstat(state_->file_fd_, &st)) {
        ret = OB_IO_ERROR;
      } else {
        state_->file_bytes_ = st.st_size;
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(read_parquet_footer(state_->file_fd_, state_->file_bytes_, state_->footer_))) {
      LOG_WARN("failed to read parquet metadata", K(ret));
    } else {
      std::vector<ObLocalColumnSpec> actual_columns;
      if (OB_FAIL(build_columns_from_footer(state_->footer_, actual_columns))) {
      } else if (actual_columns.size() != columns.size()) {
        ret = OB_SCHEMA_EAGAIN;
      } else {
        for (size_t i = 0; OB_SUCC(ret) && i < columns.size(); ++i) {
          if (actual_columns[i].source_name_ != columns[i].source_name_
              || actual_columns[i].type_ != columns[i].type_) {
            ret = OB_SCHEMA_EAGAIN;
          }
        }
      }
    }
  }
  if (OB_FAIL(ret) && state_) state_->close();
  return ret;
}

int ObLocalParquetDecoder::get_next_row(std::vector<ObLocalCell> &cells)
{
  int ret = OB_SUCCESS;
  if (!state_ || state_->file_fd_ < 0) {
    ret = OB_NOT_INIT;
  } else {
    while (OB_SUCC(ret)
           && (state_->decoded_columns_.empty()
               || state_->row_cursor_ >= state_->footer_.row_groups_[state_->group_cursor_].num_rows_)) {
      if (!state_->decoded_columns_.empty()) {
        ++state_->group_cursor_;
      }
      if (state_->group_cursor_ >= state_->footer_.row_groups_.size()) {
        ret = OB_ITER_END;
      } else if (OB_FAIL(state_->decode_current_group())) {
        LOG_WARN("failed to load parquet row group", K(ret), K(state_->group_cursor_));
      }
    }
    if (OB_SUCC(ret)) {
      cells.assign(state_->columns_.size(), ObLocalCell());
      for (size_t i = 0; i < state_->columns_.size(); ++i) {
        if (state_->projected_[i]) {
          cells[i] = state_->decoded_columns_[i][state_->row_cursor_];
        }
      }
      ++state_->row_cursor_;
    }
  }
  return ret;
}

int ObLocalParquetDecoder::rescan()
{
  int ret = OB_SUCCESS;
  if (!state_ || state_->file_fd_ < 0) ret = OB_NOT_INIT;
  else {
    state_->group_cursor_ = 0;
    state_->row_cursor_ = 0;
    state_->decoded_columns_.clear();
  }
  return ret;
}

void ObLocalParquetDecoder::close()
{
  if (state_) state_->close();
}

} // namespace sql
} // namespace oceanbase
