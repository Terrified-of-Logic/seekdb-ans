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

// Local file scan -- reader facade.
//
// This translation unit contains the parts of the subsystem that are format
// independent:
//   * a bounded streambuf that keeps the raw file handle inside our control,
//   * file / directory fingerprints (canonical path, device, inode, size and
//     modification time) that are captured at resolve time and re-checked at
//     execution time,
//   * a tiny process-local schema cache so repeated DESCRIBE / SCAN statements
//     do not re-read the same file,
//   * the ObLocalFileSchema helpers and the ObLocalFileReader lifecycle.
//
// CSV and JSONL record parsing is delegated to their providers; Parquet is
// delegated to ObLocalParquetDecoder.

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_local_file_reader_priv.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <mutex>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <stdlib.h>
#else
#include <limits.h>
#include <stdlib.h>
#endif

#include "lib/oblog/ob_log.h"
#include "share/ob_errno.h"
#include "sql/engine/basic/ob_local_parquet_decoder.h"

namespace oceanbase
{
using namespace common;
namespace sql
{
namespace local_file
{
namespace
{

// A streambuf reading straight from a validated file descriptor.  The buffer
// is intentionally modest (64 KiB): local tables are scanned sequentially and
// a bigger window only adds pressure to the block cache.
class LocalInputBuf : public std::streambuf
{
public:
  explicit LocalInputBuf(const int fd) : fd_(fd), window_()
  {
    setg(window_, window_, window_);
  }
  virtual ~LocalInputBuf()
  {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

protected:
  virtual int_type underflow() override
  {
    if (gptr() < egptr()) {
      return traits_type::to_int_type(*gptr());
    }
    const ssize_t received = read(fd_, window_, sizeof(window_));
    if (received <= 0) {
      return traits_type::eof();
    }
    setg(window_, window_, window_ + received);
    return traits_type::to_int_type(*gptr());
  }

private:
  int fd_;
  char window_[64 * 1024];
};

// Extracts the [device, inode, size, mtime_ns] identity of a stat result.
void stat_identity(const struct stat &st,
                   uint64_t &device,
                   uint64_t &inode,
                   int64_t &file_size,
                   int64_t &modified_time_ns)
{
  device = static_cast<uint64_t>(st.st_dev);
  inode = static_cast<uint64_t>(st.st_ino);
  file_size = static_cast<int64_t>(st.st_size);
#if defined(__APPLE__)
  modified_time_ns = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000L
                   + static_cast<int64_t>(st.st_mtimespec.tv_nsec);
#elif defined(_WIN32)
  modified_time_ns = static_cast<int64_t>(st.st_mtime) * 1000000000L;
#else
  modified_time_ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000L
                   + static_cast<int64_t>(st.st_mtim.tv_nsec);
#endif
}

// Schema cache entry: the full fingerprint is the key, so any rewrite of the
// file invalidates the entry implicitly.
struct LocalSchemaCacheEntry
{
  std::string path_;
  ObLocalFileFormat format_;
  uint64_t device_;
  uint64_t inode_;
  int64_t size_;
  int64_t mtime_ns_;
  std::vector<ObLocalColumnSpec> columns_;
  int64_t row_count_;
};

// Bounded LRU of inferred schemas.  Inference is cheap for small files but the
// same file is often probed several times by one statement (planning, EXPLAIN,
// execution), so caching it avoids repeated IO.
class LocalSchemaCache
{
public:
  bool lookup(const std::string &path,
              const ObLocalFileFormat format,
              const uint64_t device,
              const uint64_t inode,
              const int64_t size,
              const int64_t mtime_ns,
              std::vector<ObLocalColumnSpec> &columns,
              int64_t &row_count)
  {
    bool hit = false;
    std::lock_guard<std::mutex> guard(lock_);
    for (auto it = entries_.begin(); !hit && it != entries_.end(); ++it) {
      if (matches(*it, path, format, device, inode, size, mtime_ns)) {
        columns = it->columns_;
        row_count = it->row_count_;
        entries_.splice(entries_.begin(), entries_, it);
        hit = true;
      }
    }
    return hit;
  }

  void store(const std::string &path,
             const ObLocalFileFormat format,
             const uint64_t device,
             const uint64_t inode,
             const int64_t size,
             const int64_t mtime_ns,
             const std::vector<ObLocalColumnSpec> &columns,
             const int64_t row_count)
  {
    LocalSchemaCacheEntry entry;
    entry.path_ = path;
    entry.format_ = format;
    entry.device_ = device;
    entry.inode_ = inode;
    entry.size_ = size;
    entry.mtime_ns_ = mtime_ns;
    entry.columns_ = columns;
    entry.row_count_ = row_count;
    std::lock_guard<std::mutex> guard(lock_);
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (matches(*it, path, format, device, inode, size, mtime_ns)) {
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
    entries_.push_front(entry);
    while (entries_.size() > MAX_CACHED_SCHEMAS) {
      entries_.pop_back();
    }
  }

private:
  static bool matches(const LocalSchemaCacheEntry &entry,
                      const std::string &path,
                      const ObLocalFileFormat format,
                      const uint64_t device,
                      const uint64_t inode,
                      const int64_t size,
                      const int64_t mtime_ns)
  {
    return entry.path_ == path && entry.format_ == format
        && entry.device_ == device && entry.inode_ == inode
        && entry.size_ == size && entry.mtime_ns_ == mtime_ns;
  }

  static constexpr size_t MAX_CACHED_SCHEMAS = 64;
  std::mutex lock_;
  std::list<LocalSchemaCacheEntry> entries_;
};

LocalSchemaCache &local_schema_cache()
{
  static LocalSchemaCache cache;
  return cache;
}

// Shared implementation of the file / directory fingerprint getters.
int resolve_path_identity(const std::string &path,
                          const bool expect_directory,
                          std::string &canonical_path,
                          uint64_t &device,
                          uint64_t &inode,
                          int64_t &file_size,
                          int64_t &modified_time_ns)
{
  int ret = OB_SUCCESS;
  struct stat path_stat;
  if (OB_FAIL(ObLocalFileSchema::canonicalize_path(path, canonical_path))) {
    LOG_WARN("failed to canonicalize local file path", K(ret), K(path));
  } else if (0 != stat(canonical_path.c_str(), &path_stat)) {
    ret = OB_FILE_NOT_OPENED;
  } else if (expect_directory ? !S_ISDIR(path_stat.st_mode) : !S_ISREG(path_stat.st_mode)) {
    ret = OB_INVALID_DATA;
  } else {
    stat_identity(path_stat, device, inode, file_size, modified_time_ns);
  }
  return ret;
}

} // namespace

int open_local_stream(const std::string &path,
                      std::unique_ptr<std::streambuf> &buffer,
                      std::unique_ptr<std::istream> &stream,
                      uint64_t *opened_device,
                      uint64_t *opened_inode,
                      int64_t *opened_size,
                      int64_t *opened_mtime_ns)
{
  int ret = OB_SUCCESS;
  struct stat file_stat;
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    ret = OB_FILE_NOT_OPENED;
  } else if (0 != fstat(fd, &file_stat) || !S_ISREG(file_stat.st_mode)) {
    close(fd);
    ret = OB_INVALID_DATA;
  } else {
    uint64_t device = 0;
    uint64_t inode = 0;
    int64_t file_size = 0;
    int64_t mtime_ns = 0;
    stat_identity(file_stat, device, inode, file_size, mtime_ns);
    if (OB_NOT_NULL(opened_device)) {
      *opened_device = device;
    }
    if (OB_NOT_NULL(opened_inode)) {
      *opened_inode = inode;
    }
    if (OB_NOT_NULL(opened_size)) {
      *opened_size = file_size;
    }
    if (OB_NOT_NULL(opened_mtime_ns)) {
      *opened_mtime_ns = mtime_ns;
    }
    buffer.reset(new (std::nothrow) LocalInputBuf(fd));
    if (!buffer) {
      close(fd);
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      stream.reset(new (std::nothrow) std::istream(buffer.get()));
      if (OB_ISNULL(stream.get())) {
        buffer.reset();
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
    }
  }
  return ret;
}

} // namespace local_file

// The public facade lives in the sql namespace; the helpers above are pulled
// in explicitly so the method bodies below stay short.

int ObLocalFileSchema::parse_format(const std::string &format_name, ObLocalFileFormat &format)
{
  int ret = OB_SUCCESS;
  if (format_name.empty() || local_file::name_equals_no_case(format_name, "auto")) {
    format = ObLocalFileFormat::AUTO;
  } else if (local_file::name_equals_no_case(format_name, "csv")) {
    format = ObLocalFileFormat::CSV;
  } else if (local_file::name_equals_no_case(format_name, "jsonl")) {
    format = ObLocalFileFormat::JSONL;
  } else if (local_file::name_equals_no_case(format_name, "parquet")) {
    format = ObLocalFileFormat::PARQUET;
  } else {
    format = ObLocalFileFormat::INVALID;
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

int ObLocalFileSchema::detect_format(const std::string &path, ObLocalFileFormat &format)
{
  int ret = OB_SUCCESS;
  if (local_file::ends_with_no_case(path, ".csv")) {
    format = ObLocalFileFormat::CSV;
  } else if (local_file::ends_with_no_case(path, ".jsonl")) {
    format = ObLocalFileFormat::JSONL;
  } else if (local_file::ends_with_no_case(path, ".parquet")) {
    format = ObLocalFileFormat::PARQUET;
  } else {
    format = ObLocalFileFormat::INVALID;
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

int ObLocalFileSchema::canonicalize_path(const std::string &path, std::string &canonical_path)
{
  int ret = OB_SUCCESS;
#ifdef _WIN32
  char resolved_path[_MAX_PATH];
  if (nullptr == _fullpath(resolved_path, path.c_str(), sizeof(resolved_path))) {
    ret = OB_FILE_NOT_OPENED;
  } else {
    canonical_path.assign(resolved_path);
  }
#else
  char resolved_path[PATH_MAX];
  if (nullptr == realpath(path.c_str(), resolved_path)) {
    ret = OB_FILE_NOT_OPENED;
  } else {
    canonical_path.assign(resolved_path);
  }
#endif
  return ret;
}

int ObLocalFileSchema::get_file_fingerprint(const std::string &path,
                                            std::string &canonical_path,
                                            int64_t &file_size,
                                            int64_t &modified_time_ns)
{
  uint64_t device = 0;
  uint64_t inode = 0;
  return get_file_fingerprint(path, canonical_path, device, inode, file_size, modified_time_ns);
}

int ObLocalFileSchema::get_file_fingerprint(const std::string &path,
                                            std::string &canonical_path,
                                            uint64_t &device,
                                            uint64_t &inode,
                                            int64_t &file_size,
                                            int64_t &modified_time_ns)
{
  return local_file::resolve_path_identity(path, false, canonical_path, device, inode,
                               file_size, modified_time_ns);
}

int ObLocalFileSchema::get_directory_fingerprint(const std::string &path,
                                                 std::string &canonical_path,
                                                 uint64_t &device,
                                                 uint64_t &inode,
                                                 int64_t &modified_time_ns)
{
  int64_t ignored_size = 0;
  return local_file::resolve_path_identity(path, true, canonical_path, device, inode,
                               ignored_size, modified_time_ns);
}

int ObLocalFileSchema::infer_schema(const std::string &path,
                                    ObLocalFileFormat requested_format,
                                    std::vector<ObLocalColumnSpec> &columns,
                                    int64_t &row_count,
                                    std::string &canonical_path,
                                    ObLocalFileFormat &actual_format,
                                    int64_t &file_size,
                                    int64_t &modified_time_ns)
{
  uint64_t device = 0;
  uint64_t inode = 0;
  return infer_schema(path, requested_format, columns, row_count, canonical_path,
                      actual_format, device, inode, file_size, modified_time_ns);
}

int ObLocalFileSchema::infer_schema(const std::string &path,
                                    ObLocalFileFormat requested_format,
                                    std::vector<ObLocalColumnSpec> &columns,
                                    int64_t &row_count,
                                    std::string &canonical_path,
                                    ObLocalFileFormat &actual_format,
                                    uint64_t &device,
                                    uint64_t &inode,
                                    int64_t &file_size,
                                    int64_t &modified_time_ns)
{
  int ret = OB_SUCCESS;
  uint64_t initial_device = 0;
  uint64_t initial_inode = 0;
  int64_t initial_file_size = 0;
  int64_t initial_modified_time_ns = 0;
  std::string verified_path;
  uint64_t verified_device = 0;
  uint64_t verified_inode = 0;
  bool served_from_cache = false;
  if (OB_FAIL(get_file_fingerprint(path, canonical_path, initial_device, initial_inode,
                                   initial_file_size, initial_modified_time_ns))) {
    LOG_WARN("failed to fingerprint local file", K(ret), K(path));
  } else if (initial_file_size > local_file::MAX_FILE_BYTES) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("local file exceeds the size limit", K(ret),
             K(initial_file_size), K(local_file::MAX_FILE_BYTES));
  } else if (ObLocalFileFormat::AUTO == requested_format
             && OB_FAIL(detect_format(canonical_path, actual_format))) {
    LOG_WARN("failed to detect local file format", K(ret), K(canonical_path));
  } else {
    if (ObLocalFileFormat::AUTO != requested_format) {
      actual_format = requested_format;
    }
    served_from_cache = local_file::local_schema_cache().lookup(
        canonical_path, actual_format, initial_device, initial_inode,
        initial_file_size, initial_modified_time_ns, columns, row_count);
    if (!served_from_cache) {
      switch (actual_format) {
        case ObLocalFileFormat::CSV:
          ret = local_file::infer_csv_schema(canonical_path, columns, row_count);
          break;
        case ObLocalFileFormat::JSONL:
          ret = local_file::infer_jsonl_schema(canonical_path, columns, row_count);
          break;
        case ObLocalFileFormat::PARQUET:
          ret = ObLocalParquetDecoder::infer_schema(canonical_path, columns, row_count);
          break;
        default:
          ret = OB_NOT_SUPPORTED;
          break;
      }
    }
  }
  if (OB_SUCC(ret)) {
    // The file must not move or change while it is being probed, otherwise the
    // inferred schema would describe a different file than the executor sees.
    if (OB_FAIL(get_file_fingerprint(path, verified_path, verified_device, verified_inode,
                                     file_size, modified_time_ns))) {
      LOG_WARN("failed to re-fingerprint local file", K(ret), K(path));
    } else if (verified_path != canonical_path
               || verified_device != initial_device
               || verified_inode != initial_inode
               || file_size != initial_file_size
               || modified_time_ns != initial_modified_time_ns) {
      ret = OB_EAGAIN;
      LOG_WARN("local file changed while its schema was inferred", K(ret), K(path));
    } else {
      device = verified_device;
      inode = verified_inode;
      if (!served_from_cache) {
        local_file::local_schema_cache().store(canonical_path, actual_format, device, inode,
                                   file_size, modified_time_ns, columns, row_count);
      }
    }
  }
  return ret;
}

const char *ObLocalFileSchema::format_name(const ObLocalFileFormat format)
{
  const char *name = "invalid";
  switch (format) {
    case ObLocalFileFormat::AUTO: name = "auto"; break;
    case ObLocalFileFormat::CSV: name = "csv"; break;
    case ObLocalFileFormat::JSONL: name = "jsonl"; break;
    case ObLocalFileFormat::PARQUET: name = "parquet"; break;
    default: break;
  }
  return name;
}

const char *ObLocalFileSchema::column_type_name(const ObLocalColumnType type)
{
  const char *name = "INVALID";
  switch (type) {
    case ObLocalColumnType::NULL_TYPE: name = "NULL"; break;
    case ObLocalColumnType::BOOLEAN: name = "BOOLEAN"; break;
    case ObLocalColumnType::BIGINT: name = "BIGINT"; break;
    case ObLocalColumnType::DOUBLE: name = "DOUBLE"; break;
    case ObLocalColumnType::VARCHAR: name = "VARCHAR"; break;
    case ObLocalColumnType::DATE: name = "DATE"; break;
    case ObLocalColumnType::DATETIME: name = "DATETIME"; break;
    default: break;
  }
  return name;
}

ObLocalFileReader::ObLocalFileReader()
  : source_path_(),
    format_(ObLocalFileFormat::INVALID),
    column_specs_(),
    projected_(),
    stream_buffer_(),
    input_stream_(),
    rows_seen_(0),
    header_consumed_(false),
    expected_dev_(0),
    expected_ino_(0),
    expected_size_(0),
    expected_mtime_ns_(0),
    end_verified_(false),
    parquet_decoder_()
{}

ObLocalFileReader::~ObLocalFileReader()
{
  close();
}

int ObLocalFileReader::open(const std::string &path,
                            const ObLocalFileFormat format,
                            const std::vector<ObLocalColumnSpec> &columns,
                            const int64_t expected_file_size,
                            const int64_t expected_modified_time_ns)
{
  return open(path, format, columns, 0, 0, expected_file_size, expected_modified_time_ns);
}

int ObLocalFileReader::open(const std::string &path,
                            const ObLocalFileFormat format,
                            const std::vector<ObLocalColumnSpec> &columns,
                            const uint64_t expected_device,
                            const uint64_t expected_inode,
                            const int64_t expected_file_size,
                            const int64_t expected_modified_time_ns)
{
  std::vector<int64_t> projected_column_idxs;
  projected_column_idxs.reserve(columns.size());
  for (size_t i = 0; i < columns.size(); ++i) {
    projected_column_idxs.push_back(static_cast<int64_t>(i));
  }
  return open(path, format, columns, expected_device, expected_inode,
              expected_file_size, expected_modified_time_ns, projected_column_idxs);
}

int ObLocalFileReader::open(const std::string &path,
                            const ObLocalFileFormat format,
                            const std::vector<ObLocalColumnSpec> &columns,
                            const uint64_t expected_device,
                            const uint64_t expected_inode,
                            const int64_t expected_file_size,
                            const int64_t expected_modified_time_ns,
                            const std::vector<int64_t> &projected_column_idxs)
{
  int ret = OB_SUCCESS;
  source_path_ = path;
  format_ = format;
  column_specs_ = columns;
  projected_.assign(columns.size(), false);
  for (size_t i = 0; OB_SUCC(ret) && i < projected_column_idxs.size(); ++i) {
    const int64_t column_idx = projected_column_idxs[i];
    if (column_idx < 0 || column_idx >= static_cast<int64_t>(columns.size())) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      projected_[column_idx] = true;
    }
  }
  expected_dev_ = expected_device;
  expected_ino_ = expected_inode;
  expected_size_ = expected_file_size;
  expected_mtime_ns_ = expected_modified_time_ns;
  if (OB_SUCC(ret)) {
    ret = open_inner();
  }
  return ret;
}

int ObLocalFileReader::open_inner()
{
  int ret = OB_SUCCESS;
  close();
  std::string actual_path;
  uint64_t actual_device = 0;
  uint64_t actual_inode = 0;
  int64_t actual_file_size = 0;
  int64_t actual_modified_time_ns = 0;
  if (OB_FAIL(ObLocalFileSchema::get_file_fingerprint(source_path_, actual_path,
                                                      actual_device, actual_inode,
                                                      actual_file_size,
                                                      actual_modified_time_ns))) {
    LOG_WARN("failed to fingerprint local file before scan", K(ret), K(source_path_));
  } else if (actual_path != source_path_
             || (0 != expected_dev_ && actual_device != expected_dev_)
             || (0 != expected_ino_ && actual_inode != expected_ino_)
             || actual_file_size != expected_size_
             || actual_modified_time_ns != expected_mtime_ns_) {
    ret = OB_SCHEMA_EAGAIN;
    LOG_WARN("local file changed after schema inference", K(ret), K(source_path_));
  }
  if (OB_SUCC(ret)) {
    if (ObLocalFileFormat::PARQUET == format_) {
      parquet_decoder_.reset(new (std::nothrow) ObLocalParquetDecoder());
      if (!parquet_decoder_) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(parquet_decoder_->open(source_path_, column_specs_,
                                                expected_dev_, expected_ino_,
                                                expected_size_, expected_mtime_ns_,
                                                projected_))) {
        LOG_WARN("failed to open parquet decoder", K(ret), K(source_path_));
      }
    } else {
      uint64_t opened_device = 0;
      uint64_t opened_inode = 0;
      int64_t opened_size = 0;
      int64_t opened_mtime_ns = 0;
      if (OB_FAIL(local_file::open_local_stream(source_path_, stream_buffer_, input_stream_,
                                    &opened_device, &opened_inode,
                                    &opened_size, &opened_mtime_ns))) {
        LOG_WARN("failed to open local file for scan", K(ret), K(source_path_));
      } else if ((0 != expected_dev_ && opened_device != expected_dev_)
                 || (0 != expected_ino_ && opened_inode != expected_ino_)
                 || opened_size != expected_size_
                 || opened_mtime_ns != expected_mtime_ns_) {
        ret = OB_SCHEMA_EAGAIN;
        LOG_WARN("opened file does not match the inferred fingerprint", K(ret));
        input_stream_.reset();
        stream_buffer_.reset();
      }
    }
  }
  rows_seen_ = 0;
  header_consumed_ = false;
  if (OB_FAIL(ret)) {
  } else if (ObLocalFileFormat::CSV == format_) {
    std::vector<std::string> header_fields;
    bool has_record = false;
    if (OB_FAIL(local_file::read_csv_record(*input_stream_, header_fields, has_record))) {
      LOG_WARN("failed to read the csv header", K(ret), K(source_path_));
    } else if (header_fields.size() != column_specs_.size()) {
      ret = OB_INVALID_DATA;
      LOG_WARN("csv header does not match the resolved columns", K(ret),
               K(header_fields.size()), K(column_specs_.size()));
    } else {
      header_consumed_ = true;
    }
  }
  return ret;
}

int ObLocalFileReader::verify_end_of_file()
{
  int ret = OB_SUCCESS;
  std::string actual_path;
  uint64_t actual_device = 0;
  uint64_t actual_inode = 0;
  int64_t actual_size = 0;
  int64_t actual_mtime = 0;
  const int verify_ret = ObLocalFileSchema::get_file_fingerprint(
      source_path_, actual_path, actual_device, actual_inode, actual_size, actual_mtime);
  if (OB_SUCCESS != verify_ret
      || actual_path != source_path_
      || (0 != expected_dev_ && actual_device != expected_dev_)
      || (0 != expected_ino_ && actual_inode != expected_ino_)
      || actual_size != expected_size_
      || actual_mtime != expected_mtime_ns_) {
    ret = OB_SCHEMA_EAGAIN;
    LOG_WARN("local file changed while it was being scanned", K(ret), K(source_path_));
  } else {
    end_verified_ = true;
  }
  return ret;
}

int ObLocalFileReader::get_next_row(std::vector<ObLocalCell> &cells)
{
  int ret = OB_SUCCESS;
  switch (format_) {
    case ObLocalFileFormat::CSV:
      ret = get_next_csv_row(cells);
      break;
    case ObLocalFileFormat::JSONL:
      ret = get_next_jsonl_row(cells);
      break;
    case ObLocalFileFormat::PARQUET:
      ret = get_next_parquet_row(cells);
      break;
    default:
      ret = OB_NOT_SUPPORTED;
      break;
  }
  if (OB_ITER_END == ret && !end_verified_) {
    const int verify_ret = verify_end_of_file();
    if (OB_SUCCESS != verify_ret) {
      ret = verify_ret;
    }
  }
  return ret;
}

int ObLocalFileReader::get_next_parquet_row(std::vector<ObLocalCell> &cells)
{
  int ret = OB_SUCCESS;
  if (!parquet_decoder_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(parquet_decoder_->get_next_row(cells))) {
  } else {
    ++rows_seen_;
  }
  return ret;
}

int ObLocalFileReader::rescan()
{
  return open_inner();
}

void ObLocalFileReader::close()
{
  input_stream_.reset();
  stream_buffer_.reset();
  parquet_decoder_.reset();
  rows_seen_ = 0;
  header_consumed_ = false;
  end_verified_ = false;
}

} // namespace sql
} // namespace oceanbase
