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

// Local file scan -- physical operator implementation.
//
// The operator keeps one row (or a small batch of rows) in an intermediate
// ObLocalCell vector and only converts the projected cells into datums at the
// very end.  That keeps the format providers free of any expression engine
// dependency and makes the projection mask the only place where the plan
// layout is consulted.

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_local_file_scan_op.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
using namespace common;
namespace sql
{
OB_SERIALIZE_MEMBER((ObLocalFileScanSpec, ObOpSpec), kind_, file_path_, secure_file_priv_, file_format_,
                    device_, inode_, file_size_, modified_time_ns_,
                    file_column_names_, file_column_types_, source_column_names_,
                    source_original_names_, source_type_names_, source_column_types_,
                    source_column_nullable_, output_column_idxs_, column_exprs_);

int ObLocalFileScanOp::open_scanner()
{
  int ret = OB_SUCCESS;
  std::vector<ObLocalColumnSpec> columns;
  std::vector<int64_t> projected_column_idxs;
  if (file_spec().file_column_names_.count() != file_spec().file_column_types_.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file column name and type arrays differ in size", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < file_spec().file_column_names_.count(); ++i) {
    const ObString &name = file_spec().file_column_names_.at(i);
    ObLocalColumnSpec column;
    column.source_name_.assign(name.ptr(), name.length());
    column.column_name_ = column.source_name_;
    column.type_ = static_cast<ObLocalColumnType>(file_spec().file_column_types_.at(i));
    columns.push_back(column);
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < file_spec().output_column_idxs_.count(); ++i) {
    projected_column_idxs.push_back(file_spec().output_column_idxs_.at(i));
  }
  if (OB_SUCC(ret) && OB_FAIL(reader_.open(
        std::string(file_spec().file_path_.ptr(), file_spec().file_path_.length()),
        static_cast<ObLocalFileFormat>(file_spec().file_format_), columns,
        file_spec().device_, file_spec().inode_, file_spec().file_size_,
        file_spec().modified_time_ns_, projected_column_idxs))) {
    LOG_WARN("failed to open the local file scanner", K(ret), K(file_spec().file_path_));
  }
  return ret;
}

int ObLocalFileScanOp::open_listing()
{
  int ret = OB_SUCCESS;
  std::string canonical_path;
  uint64_t device = 0;
  uint64_t inode = 0;
  int64_t modified_time_ns = 0;
  const std::string path(file_spec().file_path_.ptr(), file_spec().file_path_.length());
  if (OB_FAIL(ObLocalFileSchema::get_directory_fingerprint(
        path, canonical_path, device, inode, modified_time_ns))) {
    LOG_WARN("failed to fingerprint the listed directory", K(ret));
  } else if (canonical_path != path || device != file_spec().device_
             || inode != file_spec().inode_) {
    ret = OB_SCHEMA_EAGAIN;
    LOG_WARN("listed directory changed after resolution", K(ret));
  } else {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat directory_stat;
    if (fd < 0) {
      ret = OB_FILE_NOT_OPENED;
    } else if (0 != fstat(fd, &directory_stat)
               || static_cast<uint64_t>(directory_stat.st_dev) != file_spec().device_
               || static_cast<uint64_t>(directory_stat.st_ino) != file_spec().inode_
               || (static_cast<int64_t>(directory_stat.st_mtim.tv_sec) * 1000000000L
                   + static_cast<int64_t>(directory_stat.st_mtim.tv_nsec))
                  != file_spec().modified_time_ns_) {
      ::close(fd);
      ret = OB_SCHEMA_EAGAIN;
      LOG_WARN("opened directory does not match the resolved fingerprint", K(ret));
    } else if (OB_ISNULL(dir_handle_ = fdopendir(fd))) {
      ::close(fd);
      ret = OB_FILE_NOT_OPENED;
      LOG_WARN("failed to open the directory stream", K(ret));
    }
  }
  return ret;
}

int ObLocalFileScanOp::recheck_fingerprint()
{
  int ret = OB_SUCCESS;
  std::string canonical_path;
  uint64_t device = 0;
  uint64_t inode = 0;
  int64_t file_size = 0;
  int64_t modified_time_ns = 0;
  const std::string path(file_spec().file_path_.ptr(), file_spec().file_path_.length());
  if (OB_FAIL(ObLocalFileSchema::get_file_fingerprint(
        path, canonical_path, device, inode, file_size, modified_time_ns))) {
    LOG_WARN("failed to re-fingerprint the file table", K(ret));
  } else if (canonical_path != path || device != file_spec().device_
             || inode != file_spec().inode_ || file_size != file_spec().file_size_
             || modified_time_ns != file_spec().modified_time_ns_) {
    ret = OB_SCHEMA_EAGAIN;
    LOG_WARN("file table changed after schema resolution", K(ret));
  }
  return ret;
}

int ObLocalFileScanOp::inner_open()
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = ctx_.get_my_session();
  ObString current_secure_file_priv;
  bool enable_file_sql = false;
  describe_cursor_ = 0;
  const ObLocalFileTableKind kind = static_cast<ObLocalFileTableKind>(file_spec().kind_);
  if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(session->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL,
                                               enable_file_sql))) {
    LOG_WARN("failed to read enable_file_sql at execution time", K(ret));
  } else if (!enable_file_sql) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("file sql was disabled after resolution", K(ret));
  } else if (OB_FAIL(session->get_secure_file_priv(current_secure_file_priv))) {
    LOG_WARN("failed to read the current secure_file_priv", K(ret));
  } else if (current_secure_file_priv != file_spec().secure_file_priv_) {
    ret = OB_SCHEMA_EAGAIN;
    LOG_WARN("secure_file_priv changed after file sql resolution", K(ret));
  } else if (ObLocalFileTableKind::SCAN == kind) {
    ret = open_scanner();
  } else if (ObLocalFileTableKind::LIST == kind) {
    ret = open_listing();
  } else if (ObLocalFileTableKind::SCHEMA == kind) {
    ret = recheck_fingerprint();
  } else {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int ObLocalFileScanOp::next_scan_row()
{
  return reader_.get_next_row(row_cells_);
}

int ObLocalFileScanOp::next_describe_row()
{
  int ret = OB_SUCCESS;
  const int64_t source_column_count = file_spec().source_column_names_.count();
  if (describe_cursor_ >= source_column_count) {
    // The last row is only produced once the file is guaranteed unchanged, so
    // an incomplete DESCRIBE cannot be mistaken for a complete one.
    if (OB_FAIL(recheck_fingerprint())) {
    } else {
      ret = OB_ITER_END;
    }
  } else if (source_column_count != file_spec().source_column_types_.count()
             || source_column_count != file_spec().source_original_names_.count()
             || source_column_count != file_spec().source_type_names_.count()
             || source_column_count != file_spec().source_column_nullable_.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("describe column arrays differ in size", K(ret), K(source_column_count));
  } else {
    const ObLocalColumnType type = static_cast<ObLocalColumnType>(
      file_spec().source_column_types_.at(describe_cursor_));
    row_cells_.assign(6, ObLocalCell());
    row_cells_[0].null_ = false;
    row_cells_[0].i64_ = describe_cursor_ + 1;
    row_cells_[1].null_ = false;
    row_cells_[1].text_.assign(file_spec().source_column_names_.at(describe_cursor_).ptr(),
                               file_spec().source_column_names_.at(describe_cursor_).length());
    row_cells_[2].null_ = false;
    row_cells_[2].text_.assign(file_spec().source_original_names_.at(describe_cursor_).ptr(),
                               file_spec().source_original_names_.at(describe_cursor_).length());
    row_cells_[3].null_ = false;
    row_cells_[3].text_ = ObLocalFileSchema::column_type_name(type);
    row_cells_[4].null_ = false;
    row_cells_[4].text_.assign(file_spec().source_type_names_.at(describe_cursor_).ptr(),
                               file_spec().source_type_names_.at(describe_cursor_).length());
    row_cells_[5].null_ = false;
    row_cells_[5].flag_ = 0 != file_spec().source_column_nullable_.at(describe_cursor_);
    ++describe_cursor_;
  }
  return ret;
}

int ObLocalFileScanOp::next_listing_row()
{
  int ret = OB_SUCCESS;
  struct dirent *entry = nullptr;
  struct stat entry_stat;
  while (OB_SUCC(ret) && OB_NOT_NULL(entry = readdir(dir_handle_))) {
    if (0 == strcmp(entry->d_name, ".") || 0 == strcmp(entry->d_name, "..")) {
      continue;
    }
    if (0 != fstatat(dirfd(dir_handle_), entry->d_name, &entry_stat, AT_SYMLINK_NOFOLLOW)) {
      continue;
    }
    const std::string directory_path(file_spec().file_path_.ptr(),
                                     file_spec().file_path_.length());
    const std::string full_path = directory_path + "/" + entry->d_name;
    ObLocalFileFormat format = ObLocalFileFormat::INVALID;
    const bool queryable = S_ISREG(entry_stat.st_mode)
                           && OB_SUCCESS == ObLocalFileSchema::detect_format(full_path, format);
    if (1 == file_spec().file_column_types_.count()) {
      // glob() compatibility mode: a single VARCHAR column with the full path.
      row_cells_.assign(1, ObLocalCell());
      row_cells_[0].null_ = false;
      row_cells_[0].text_ = full_path;
    } else {
      row_cells_.assign(6, ObLocalCell());
      row_cells_[0].null_ = false;
      row_cells_[0].text_ = entry->d_name;
      row_cells_[1].null_ = false;
      row_cells_[1].text_ = full_path;
      row_cells_[2].null_ = false;
      row_cells_[2].text_ = queryable ? ObLocalFileSchema::format_name(format) : "unknown";
      row_cells_[3].null_ = false;
      row_cells_[3].i64_ = static_cast<int64_t>(entry_stat.st_size);
      row_cells_[4].null_ = false;
      row_cells_[4].dt64_ = static_cast<int64_t>(entry_stat.st_mtime) * 1000000L;
      row_cells_[5].null_ = false;
      row_cells_[5].flag_ = queryable;
    }
    break;
  }
  if (OB_SUCC(ret) && OB_ISNULL(entry)) {
    struct stat directory_stat;
    if (0 != fstat(dirfd(dir_handle_), &directory_stat)
        || static_cast<uint64_t>(directory_stat.st_dev) != file_spec().device_
        || static_cast<uint64_t>(directory_stat.st_ino) != file_spec().inode_
        || (static_cast<int64_t>(directory_stat.st_mtim.tv_sec) * 1000000000L
            + static_cast<int64_t>(directory_stat.st_mtim.tv_nsec))
           != file_spec().modified_time_ns_) {
      ret = OB_SCHEMA_EAGAIN;
      LOG_WARN("directory changed while it was being listed", K(ret));
    } else {
      ret = OB_ITER_END;
    }
  }
  return ret;
}

int ObLocalFileScanOp::emit_cells()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (file_spec().column_exprs_.count() != file_spec().output_column_idxs_.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("column expression and output index arrays differ in size", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < file_spec().column_exprs_.count(); ++i) {
    const int64_t column_idx = file_spec().output_column_idxs_.at(i);
    ObExpr *expr = file_spec().column_exprs_.at(i);
    if (OB_ISNULL(expr) || column_idx < 0
        || column_idx >= static_cast<int64_t>(row_cells_.size())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
      const ObLocalColumnType type = static_cast<ObLocalColumnType>(
        file_spec().file_column_types_.at(column_idx));
      OZ (store_datum(row_cells_.at(column_idx), type, datum));
      OX (expr->set_evaluated_projected(eval_ctx_));
    }
  }
  return ret;
}

int ObLocalFileScanOp::store_datum(const ObLocalCell &cell,
                                   const ObLocalColumnType type,
                                   ObDatum &datum)
{
  int ret = OB_SUCCESS;
  if (cell.null_ || ObLocalColumnType::NULL_TYPE == type) {
    datum.set_null();
  } else if (ObLocalColumnType::BOOLEAN == type) {
    datum.set_int(cell.flag_ ? 1 : 0);
  } else if (ObLocalColumnType::BIGINT == type) {
    datum.set_int(cell.i64_);
  } else if (ObLocalColumnType::DOUBLE == type) {
    datum.set_double(cell.f64_);
  } else if (ObLocalColumnType::DATE == type) {
    datum.set_date(cell.d32_);
  } else if (ObLocalColumnType::DATETIME == type) {
    datum.set_datetime(cell.dt64_);
  } else if (ObLocalColumnType::VARCHAR == type) {
    datum.set_string(cell.text_.data(), cell.text_.length());
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

int ObLocalFileScanOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ctx_.check_status())) {
    LOG_WARN("file scan was interrupted", K(ret));
  } else {
    const ObLocalFileTableKind kind = static_cast<ObLocalFileTableKind>(file_spec().kind_);
    if (ObLocalFileTableKind::SCAN == kind) {
      ret = next_scan_row();
    } else if (ObLocalFileTableKind::SCHEMA == kind) {
      ret = next_describe_row();
    } else if (ObLocalFileTableKind::LIST == kind) {
      ret = next_listing_row();
    } else {
      ret = OB_ERR_UNEXPECTED;
    }
    if (OB_SUCC(ret)) {
      ret = emit_cells();
    }
  }
  return ret;
}

int ObLocalFileScanOp::inner_get_next_batch(const int64_t max_row_cnt)
{
  int ret = OB_SUCCESS;
  bool reached_end = false;
  const int64_t batch_limit = std::min(max_row_cnt, file_spec().max_batch_size_);
  batch_rows_.clear();
  clear_evaluated_flag();
  clear_datum_eval_flag();
  while (OB_SUCC(ret) && !reached_end
         && static_cast<int64_t>(batch_rows_.size()) < batch_limit) {
    if (OB_FAIL(ctx_.check_status())) {
      LOG_WARN("file batch scan was interrupted", K(ret));
    } else {
      const ObLocalFileTableKind kind = static_cast<ObLocalFileTableKind>(file_spec().kind_);
      if (ObLocalFileTableKind::SCAN == kind) {
        ret = next_scan_row();
      } else if (ObLocalFileTableKind::SCHEMA == kind) {
        ret = next_describe_row();
      } else if (ObLocalFileTableKind::LIST == kind) {
        ret = next_listing_row();
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
        reached_end = true;
      } else if (OB_SUCC(ret)) {
        batch_rows_.push_back(row_cells_);
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < file_spec().column_exprs_.count(); ++i) {
    const int64_t column_idx = file_spec().output_column_idxs_.at(i);
    ObExpr *expr = file_spec().column_exprs_.at(i);
    if (OB_ISNULL(expr) || column_idx < 0
        || column_idx >= file_spec().file_column_types_.count()) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObDatum *datums = expr->locate_batch_datums(eval_ctx_);
      const int64_t datum_count = expr->is_batch_result()
                                ? static_cast<int64_t>(batch_rows_.size())
                                : std::min<int64_t>(1, batch_rows_.size());
      for (int64_t row_idx = 0; OB_SUCC(ret) && row_idx < datum_count; ++row_idx) {
        if (column_idx >= static_cast<int64_t>(batch_rows_.at(row_idx).size())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(store_datum(batch_rows_.at(row_idx).at(column_idx),
                                       static_cast<ObLocalColumnType>(
                                         file_spec().file_column_types_.at(column_idx)),
                                       datums[row_idx]))) {
          LOG_WARN("failed to write a file batch cell", K(ret), K(row_idx), K(column_idx));
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    brs_.size_ = batch_rows_.size();
    brs_.end_ = reached_end;
  }
  return ret;
}

int ObLocalFileScanOp::inner_rescan()
{
  int ret = inner_close();
  if (OB_SUCC(ret)) {
    ret = inner_open();
  }
  return ret;
}

int ObLocalFileScanOp::inner_close()
{
  reader_.close();
  row_cells_.clear();
  batch_rows_.clear();
  describe_cursor_ = 0;
  if (OB_NOT_NULL(dir_handle_)) {
    closedir(dir_handle_);
    dir_handle_ = nullptr;
  }
  return OB_SUCCESS;
}
} // namespace sql
} // namespace oceanbase
