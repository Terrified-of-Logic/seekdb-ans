/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
// Native hnswlib serialization layout used by the supplied hipVS/hnswlib
// dependency on x86_64. Kept in a small header so the label rewrite can be
// unit-tested without ROCm or a GPU.
#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace hipvs_layout {

struct Layout {
  uint64_t n = 0;
  uint64_t row_size = 0;
  uint64_t label_offset = 0;
  uint64_t data_offset = 0;
};

// Reads the 96-byte header and returns the four fields used below. The caller
// checks them against the expected (n, m, dim) shape before touching records.
inline Layout read_layout(const char *path)
{
  const int fd = ::open(path, O_RDONLY);
  if (fd < 0) { throw std::runtime_error("cannot open serialized graph"); }
  uint64_t header[6] {};
  ssize_t got = 0;
  do {
    got = ::read(fd, header, sizeof(header));
  } while (got < 0 && errno == EINTR);
  ::close(fd);
  if (got != static_cast<ssize_t>(sizeof(header))) {
    throw std::runtime_error("short serialized graph header");
  }
  Layout layout;
  layout.n = header[2];
  layout.row_size = header[3];
  layout.label_offset = header[4];
  layout.data_offset = header[5];
  return layout;
}

// Rewrites every record's label slot from the hnswlib point id to the original
// 64-bit document id. The dependency writes fixed-size records, so one mmap
// replaces three file operations per row (seek/read/seek).
inline void rewrite_labels(const char *path, const Layout &layout,
                           const std::vector<int64_t> &ids)
{
  if (layout.n != ids.size() || layout.row_size == 0 ||
      layout.label_offset + sizeof(int64_t) > layout.row_size ||
      layout.data_offset >= layout.row_size) {
    throw std::runtime_error("inconsistent HNSW layout");
  }
  const int fd = ::open(path, O_RDWR);
  if (fd < 0) { throw std::runtime_error("cannot open serialized graph"); }
  struct stat info {};
  if (::fstat(fd, &info) != 0 || static_cast<uint64_t>(info.st_size) < 96) {
    ::close(fd);
    throw std::runtime_error("cannot stat serialized graph");
  }
  const uint64_t length = static_cast<uint64_t>(info.st_size);
  if (length < 96 + layout.n * layout.row_size) {
    ::close(fd);
    throw std::runtime_error("serialized graph is truncated");
  }
  void *mapping = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    ::close(fd);
    throw std::runtime_error("cannot mmap serialized graph");
  }
  char *base = static_cast<char *>(mapping);
  std::vector<uint8_t> seen((layout.n + 7) / 8, 0);
  for (uint64_t row = 0; row < layout.n; ++row) {
    char *record = base + 96 + row * layout.row_size;
    uint64_t label = 0;
    std::memcpy(&label, record + layout.label_offset, sizeof(label));
    if (label >= layout.n || (seen[label >> 3] & (1U << (label & 7))) != 0) {
      ::munmap(mapping, length);
      ::close(fd);
      throw std::runtime_error("invalid GPU label mapping");
    }
    seen[label >> 3] |= static_cast<uint8_t>(1U << (label & 7));
    const int64_t original = ids[static_cast<size_t>(label)];
    std::memcpy(record + layout.label_offset, &original, sizeof(original));
  }
  if (::msync(mapping, length, MS_SYNC) != 0) {
    ::munmap(mapping, length);
    ::close(fd);
    throw std::runtime_error("cannot flush rewritten labels");
  }
  ::munmap(mapping, length);
  if (::close(fd) != 0) { throw std::runtime_error("cannot close serialized graph"); }
}

} // namespace hipvs_layout
