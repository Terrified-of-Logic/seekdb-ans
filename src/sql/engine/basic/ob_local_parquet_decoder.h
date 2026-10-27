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

// Local file scan -- Parquet decoder.
//
// A trimmed down, read only Parquet reader that covers the subset of the
// format needed by local file tables:
//   * the footer is parsed with a small Thrift compact protocol cursor,
//   * flat primitive schemas only (BOOLEAN / INT32 / INT64 / INT96 / FLOAT /
//     DOUBLE / BYTE_ARRAY / FIXED_LEN_BYTE_ARRAY),
//   * PLAIN and dictionary encoded data pages, RLE / bit packed definition
//     levels, UNCOMPRESSED and SNAPPY compression.
//
// Descriptor, page and metadata symbols are internal to the implementation
// file.  The public interface below only exposes the operations the reader
// facade needs.
//
// Attribution: the decoding approach follows the Apache-2.0 reference
// implementation published in oceanbase/seekdb PR #1251; the implementation
// here has been reorganized and is documented in docs/file_sql_design.md.

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_LOCAL_PARQUET_DECODER_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_LOCAL_PARQUET_DECODER_H_

#include <memory>
#include <string>
#include <vector>

#include "lib/ob_errno.h"
#include "sql/engine/basic/ob_local_file_reader.h"

namespace oceanbase
{
namespace sql
{
class ObLocalParquetDecoder
{
public:
  ObLocalParquetDecoder();
  ~ObLocalParquetDecoder();

  // Reads only the footer and describes the flat schema of the file.
  static int infer_schema(const std::string &path,
                          std::vector<ObLocalColumnSpec> &columns,
                          int64_t &row_count);
  // Opens the file and verifies it still matches the fingerprint captured when
  // the schema was inferred.
  int open(const std::string &path,
           const std::vector<ObLocalColumnSpec> &columns,
           uint64_t expected_device,
           uint64_t expected_inode,
           int64_t expected_file_size,
           int64_t expected_modified_time_ns,
           const std::vector<bool> &projected_columns);
  int get_next_row(std::vector<ObLocalCell> &cells);
  int rescan();
  void close();

private:
  class DecoderState;
  std::unique_ptr<DecoderState> state_;
};
} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_LOCAL_PARQUET_DECODER_H_
