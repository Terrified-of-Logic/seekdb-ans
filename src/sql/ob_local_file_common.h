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

// Local file table -- the descriptor shared by the resolver, the optimizer and
// the executor.
//
// A statement such as
//
//   SELECT * FROM read_csv('/data/users.csv');
//
// is resolved into an ObLocalTableDef: the file is canonicalized, its kind
// (single file scan / directory listing / schema probe) is recorded, the
// format is fixed, and the visible column list plus the source column list are
// materialized.  The definition also carries the file fingerprint captured at
// resolve time, which the executor uses to detect a concurrent rewrite of the
// data before it returns a single row.

#ifndef OCEANBASE_SQL_OB_LOCAL_FILE_COMMON_H_
#define OCEANBASE_SQL_OB_LOCAL_FILE_COMMON_H_

#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_print_utils.h"

namespace oceanbase
{
namespace sql
{

// How the local file table is consumed by a statement.
enum class ObLocalFileTableKind : int32_t
{
  SCAN = 0,   // SELECT from the file contents
  LIST,       // glob() style directory listing
  SCHEMA,     // DESCRIBE of the inferred columns
  INVALID
};

// Physical encoding of the file.
enum class ObLocalFileFormat : int32_t
{
  AUTO = 0,   // decided from the file extension at resolve time
  CSV,
  JSONL,
  PARQUET,
  INVALID
};

// The SQL-visible type of an inferred column.
enum class ObLocalColumnType : int32_t
{
  NULL_TYPE = 0,
  BOOLEAN,
  BIGINT,
  DOUBLE,
  VARCHAR,
  DATE,
  DATETIME,
  INVALID
};

// One column of a local file table.  `source_name_` is the key inside the
// file, `column_name_` is the deduplicated visible name, and
// `source_type_name_` is the format specific type spelling reported by
// DESCRIBE (for example "CSV/DOUBLE" or "INT64/TIMESTAMP_MICROS").
struct ObLocalColumnDef
{
  ObLocalColumnDef()
    : source_name_(), column_name_(), source_type_name_(),
      type_(ObLocalColumnType::NULL_TYPE), nullable_(false), max_length_(0)
  {}

  common::ObString source_name_;
  common::ObString column_name_;
  common::ObString source_type_name_;
  ObLocalColumnType type_;
  bool nullable_;
  int64_t max_length_;
  TO_STRING_KV(K_(source_name), K_(column_name), K_(source_type_name),
               K_(type), K_(nullable), K_(max_length));
};

// Complete description of a local file table.
struct ObLocalTableDef
{
  ObLocalTableDef()
    : kind_(ObLocalFileTableKind::INVALID),
      canonical_path_(),
      secure_file_priv_(),
      format_(ObLocalFileFormat::INVALID),
      columns_(),
      source_columns_(),
      estimated_rows_(0),
      device_(0),
      inode_(0),
      file_size_(0),
      modified_time_ns_(0)
  {}

  ObLocalFileTableKind kind_;
  common::ObString canonical_path_;   // realpath() of the target
  common::ObString secure_file_priv_; // value in effect when resolved
  ObLocalFileFormat format_;
  common::ObSEArray<ObLocalColumnDef, 16> columns_;        // visible columns
  common::ObSEArray<ObLocalColumnDef, 16> source_columns_; // probe result
  int64_t estimated_rows_;
  uint64_t device_;
  uint64_t inode_;
  int64_t file_size_;
  int64_t modified_time_ns_;
  TO_STRING_KV(K_(kind), K_(canonical_path), K_(secure_file_priv), K_(format), K_(columns),
               K_(source_columns), K_(estimated_rows), K_(device), K_(inode), K_(file_size),
               K_(modified_time_ns));
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_OB_LOCAL_FILE_COMMON_H_
