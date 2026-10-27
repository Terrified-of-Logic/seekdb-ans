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

// Local file scan -- physical operator.
//
// ObLocalFileScanOp is the leaf operator of a local file statement.  It has
// three modes, selected by the table kind resolved during planning:
//
//   SCAN    stream the file rows through ObLocalFileReader,
//   LIST    enumerate a directory with readdir() for glob() style queries,
//   SCHEMA  emit one DESCRIBE row per inferred column.
//
// Every mode re-validates the fingerprint stored in the spec before producing
// rows, so a file that was rewritten between planning and execution yields a
// retryable schema error instead of silently mixed data.

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_LOCAL_FILE_SCAN_OP_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_LOCAL_FILE_SCAN_OP_H_

#include <dirent.h>
#include <vector>

#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_local_file_reader.h"

namespace oceanbase
{
namespace sql
{
// Serialized description of the local file table attached to the plan.
class ObLocalFileScanSpec : public ObOpSpec
{
  OB_UNIS_VERSION_V(1);
public:
  ObLocalFileScanSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type), kind_(static_cast<int32_t>(ObLocalFileTableKind::INVALID)),
      file_path_(), secure_file_priv_(), file_format_(static_cast<int32_t>(ObLocalFileFormat::INVALID)),
      device_(0), inode_(0), file_size_(0), modified_time_ns_(0),
      file_column_names_(alloc), file_column_types_(alloc),
      source_column_names_(alloc), source_original_names_(alloc),
      source_type_names_(alloc), source_column_types_(alloc), source_column_nullable_(alloc),
      output_column_idxs_(alloc), column_exprs_(alloc)
  {}

  int32_t kind_;
  common::ObString file_path_;
  common::ObString secure_file_priv_;
  int32_t file_format_;
  uint64_t device_;
  uint64_t inode_;
  int64_t file_size_;
  int64_t modified_time_ns_;
  common::ObFixedArray<common::ObString, common::ObIAllocator> file_column_names_;
  common::ObFixedArray<int32_t, common::ObIAllocator> file_column_types_;
  common::ObFixedArray<common::ObString, common::ObIAllocator> source_column_names_;
  common::ObFixedArray<common::ObString, common::ObIAllocator> source_original_names_;
  common::ObFixedArray<common::ObString, common::ObIAllocator> source_type_names_;
  common::ObFixedArray<int32_t, common::ObIAllocator> source_column_types_;
  common::ObFixedArray<int8_t, common::ObIAllocator> source_column_nullable_;
  common::ObFixedArray<int64_t, common::ObIAllocator> output_column_idxs_;
  common::ObFixedArray<ObExpr *, common::ObIAllocator> column_exprs_;
};

class ObLocalFileScanOp : public ObOperator
{
public:
  ObLocalFileScanOp(ObExecContext &ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(ctx, spec, input), reader_(), row_cells_(), batch_rows_(), dir_handle_(nullptr),
      describe_cursor_(0)
  {}
  virtual ~ObLocalFileScanOp() {}

  virtual int inner_open() override;
  virtual int inner_get_next_row() override;
  virtual int inner_get_next_batch(const int64_t max_row_cnt) override;
  virtual int inner_rescan() override;
  virtual int inner_close() override;
  virtual void destroy() override { ObOperator::destroy(); }

private:
  int open_scanner();
  int open_listing();
  int recheck_fingerprint();
  int next_scan_row();
  int next_describe_row();
  int next_listing_row();
  int emit_cells();
  int store_datum(const ObLocalCell &cell, ObLocalColumnType type, ObDatum &datum);
  const ObLocalFileScanSpec &file_spec() const
  { return static_cast<const ObLocalFileScanSpec &>(spec_); }

private:
  ObLocalFileReader reader_;
  std::vector<ObLocalCell> row_cells_;
  std::vector<std::vector<ObLocalCell> > batch_rows_;
  DIR *dir_handle_;
  int64_t describe_cursor_;
};
} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_LOCAL_FILE_SCAN_OP_H_
