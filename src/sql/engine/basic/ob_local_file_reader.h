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

// Local file scan: schema probing and row streaming.
//
// A "local file table" is resolved once by the resolver: the file is
// fingerprinted (canonical path + device/inode + size + modification time),
// a format provider inspects the file and reports the column list, and the
// resulting description is frozen into the local table definition.  At
// execution time the physical operator re-opens the very same file, verifies
// that the fingerprint is unchanged, and streams rows through the reader
// defined below.
//
// Support matrix of this module:
//   * CSV     -- header driven, comma separated, optional double quoting
//   * JSONL   -- one JSON object per line, the column set is the union of keys
//   * Parquet -- flat primitive schemas only, decoded by ObLocalParquetDecoder
//
// Format specific parsing lives in ob_local_csv_provider.cpp and
// ob_local_jsonl_provider.cpp; the value model shared by both is declared in
// ob_local_file_reader_priv.h.  See docs/file_sql_design.md for the overall
// design and the upstream attribution.

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_LOCAL_FILE_READER_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_LOCAL_FILE_READER_H_

#include <istream>
#include <memory>
#include <string>
#include <vector>

#include "lib/ob_errno.h"
#include "sql/ob_local_file_common.h"

namespace oceanbase
{
namespace sql
{
class ObLocalParquetDecoder;

// One column of a local file as inferred from the file contents.
struct ObLocalColumnSpec
{
  ObLocalColumnSpec()
    : source_name_(),
      column_name_(),
      source_type_name_(),
      type_(ObLocalColumnType::NULL_TYPE),
      nullable_(false),
      max_length_(0)
  {}
  std::string source_name_;      // key in the source document
  std::string column_name_;      // SQL visible name (deduplicated)
  std::string source_type_name_; // human readable source type, e.g. "CSV/DOUBLE"
  ObLocalColumnType type_;
  bool nullable_;
  int64_t max_length_;
};

// A single decoded cell.  Exactly one of the typed fields below is meaningful,
// selected by the column type of the owning table.
struct ObLocalCell
{
  ObLocalCell() { reset(); }
  void reset()
  {
    null_ = true;
    i64_ = 0;
    f64_ = 0;
    flag_ = false;
    d32_ = 0;
    dt64_ = 0;
    text_.clear();
  }
  bool null_;
  int64_t i64_;
  double f64_;
  bool flag_;
  int32_t d32_;
  int64_t dt64_;
  std::string text_;
};

// Stateless schema helpers.  Every function here is pure except the bounded
// process wide schema cache hidden behind infer_schema().
class ObLocalFileSchema
{
public:
  // "auto" / "csv" / "jsonl" / "parquet" -> enum.  An empty name means auto.
  static int parse_format(const std::string &format_name, ObLocalFileFormat &format);
  // Extension based detection, used only when the caller asked for auto.
  static int detect_format(const std::string &path, ObLocalFileFormat &format);
  // realpath(); the result is what gets stored in the table definition.
  static int canonicalize_path(const std::string &path, std::string &canonical_path);
  static int get_file_fingerprint(const std::string &path,
                                  std::string &canonical_path,
                                  int64_t &file_size,
                                  int64_t &modified_time_ns);
  static int get_file_fingerprint(const std::string &path,
                                  std::string &canonical_path,
                                  uint64_t &device,
                                  uint64_t &inode,
                                  int64_t &file_size,
                                  int64_t &modified_time_ns);
  static int get_directory_fingerprint(const std::string &path,
                                       std::string &canonical_path,
                                       uint64_t &device,
                                       uint64_t &inode,
                                       int64_t &modified_time_ns);
  // Inspect the file and describe its columns.  The file is fingerprinted
  // before and after probing; a concurrent modification yields OB_EAGAIN.
  static int infer_schema(const std::string &path,
                          ObLocalFileFormat requested_format,
                          std::vector<ObLocalColumnSpec> &columns,
                          int64_t &row_count,
                          std::string &canonical_path,
                          ObLocalFileFormat &actual_format,
                          int64_t &file_size,
                          int64_t &modified_time_ns);
  static int infer_schema(const std::string &path,
                          ObLocalFileFormat requested_format,
                          std::vector<ObLocalColumnSpec> &columns,
                          int64_t &row_count,
                          std::string &canonical_path,
                          ObLocalFileFormat &actual_format,
                          uint64_t &device,
                          uint64_t &inode,
                          int64_t &file_size,
                          int64_t &modified_time_ns);
  static const char *format_name(ObLocalFileFormat format);
  static const char *column_type_name(ObLocalColumnType type);
};

// Sequential row source for one local file.  The reader owns the file
// descriptor / decoder and can be rewound with rescan().
class ObLocalFileReader
{
public:
  ObLocalFileReader();
  ~ObLocalFileReader();

  // The two shorter overloads simply carry no device/inode information; the
  // third one also receives the projected column list.
  int open(const std::string &path,
           ObLocalFileFormat format,
           const std::vector<ObLocalColumnSpec> &columns,
           int64_t expected_file_size,
           int64_t expected_modified_time_ns);
  int open(const std::string &path,
           ObLocalFileFormat format,
           const std::vector<ObLocalColumnSpec> &columns,
           uint64_t expected_device,
           uint64_t expected_inode,
           int64_t expected_file_size,
           int64_t expected_modified_time_ns);
  int open(const std::string &path,
           ObLocalFileFormat format,
           const std::vector<ObLocalColumnSpec> &columns,
           uint64_t expected_device,
           uint64_t expected_inode,
           int64_t expected_file_size,
           int64_t expected_modified_time_ns,
           const std::vector<int64_t> &projected_column_idxs);
  // Fills one row; OB_ITER_END signals a clean end of file.
  int get_next_row(std::vector<ObLocalCell> &cells);
  int rescan();
  void close();
  int64_t rows_seen() const { return rows_seen_; }

private:
  int open_inner();
  int verify_end_of_file();
  int get_next_csv_row(std::vector<ObLocalCell> &cells);
  int get_next_jsonl_row(std::vector<ObLocalCell> &cells);
  int get_next_parquet_row(std::vector<ObLocalCell> &cells);

private:
  std::string source_path_;
  ObLocalFileFormat format_;
  std::vector<ObLocalColumnSpec> column_specs_;
  std::vector<bool> projected_;
  std::unique_ptr<std::streambuf> stream_buffer_;
  std::unique_ptr<std::istream> input_stream_;
  int64_t rows_seen_;
  bool header_consumed_;
  uint64_t expected_dev_;
  uint64_t expected_ino_;
  int64_t expected_size_;
  int64_t expected_mtime_ns_;
  bool end_verified_;
  std::unique_ptr<ObLocalParquetDecoder> parquet_decoder_;
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_LOCAL_FILE_READER_H_
