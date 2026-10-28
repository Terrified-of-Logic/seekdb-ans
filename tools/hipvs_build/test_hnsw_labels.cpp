/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
// Standalone test for the native-HNSW label rewrite used by the hipVS helper.
// It needs no GPU, ROCm or cuVS: build and run with
//   g++ -std=c++17 -O2 tools/hipvs_build/test_hnsw_labels.cpp -o /tmp/test_hnsw_labels
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

#include "hnsw_labels.h"

namespace {

constexpr uint64_t kHeaderBytes = 96;

std::string make_file(const std::string &path, uint64_t n, uint64_t m, uint64_t dim,
                      const std::vector<uint64_t> &labels, bool truncate)
{
  const uint64_t row_size = 2 * m * 4 + 4 + dim * 4 + 8;
  const uint64_t label_offset = 2 * m * 4 + 4 + dim * 4;
  const uint64_t data_offset = 2 * m * 4 + 4;
  FILE *f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) { throw std::runtime_error("cannot create test file"); }
  uint64_t header[12] = {0, 0, n, row_size, label_offset, data_offset};
  std::fwrite(header, sizeof(header), 1, f);
  std::vector<char> record(row_size, 0);
  for (uint64_t i = 0; i < n; ++i) {
    std::fill(record.begin(), record.end(), 0);
    const uint32_t level = 0;
    std::memcpy(record.data(), &level, sizeof(level));
    for (uint64_t d = 0; d < dim; ++d) {
      const float value = static_cast<float>(i * dim + d) * 0.5f;
      std::memcpy(record.data() + data_offset + d * sizeof(float), &value, sizeof(value));
    }
    const uint64_t label = labels[i];
    std::memcpy(record.data() + label_offset, &label, sizeof(label));
    std::fwrite(record.data(), record.size(), 1, f);
  }
  std::fclose(f);
  if (truncate) {
    if (::truncate(path.c_str(), static_cast<off_t>(kHeaderBytes + (n - 1) * row_size + 1)) != 0) {
      throw std::runtime_error("truncate failed");
    }
  }
  return path;
}

std::vector<int64_t> read_labels(const std::string &path, const hipvs_layout::Layout &layout,
                                 uint64_t dim, const std::vector<uint64_t> &labels)
{
  FILE *f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) { throw std::runtime_error("cannot reopen test file"); }
  std::fseek(f, static_cast<long>(kHeaderBytes), SEEK_SET);
  std::vector<char> record(layout.row_size);
  std::vector<int64_t> result;
  for (uint64_t row = 0; row < layout.n; ++row) {
    if (std::fread(record.data(), record.size(), 1, f) != 1) {
      throw std::runtime_error("short read");
    }
    int64_t label = 0;
    std::memcpy(&label, record.data() + layout.label_offset, sizeof(label));
    result.push_back(label);
    float first = 0;
    std::memcpy(&first, record.data() + layout.data_offset, sizeof(first));
    if (first != static_cast<float>(row * dim) * 0.5f) {
      throw std::runtime_error("vector bytes changed");
    }
  }
  std::fclose(f);
  return result;
}

int fail(const char *what)
{
  std::fprintf(stderr, "FAIL: %s\n", what);
  return 1;
}

} // namespace

int main()
{
  const std::string path = "/tmp/hipvs-labels-test.bin";
  const uint64_t n = 1000, m = 16, dim = 4;
  std::vector<int64_t> ids(n);
  for (uint64_t i = 0; i < n; ++i) { ids[i] = static_cast<int64_t>(i * 17 + 10000000000LL); }
  // Permuted label slots exercise label-driven (not row-driven) rewriting.
  std::vector<uint64_t> labels(n);
  for (uint64_t i = 0; i < n; ++i) { labels[i] = (i * 37 + 11) % n; }

  make_file(path, n, m, dim, labels, false);
  const hipvs_layout::Layout layout = hipvs_layout::read_layout(path.c_str());
  if (layout.n != n || layout.row_size != 2 * m * 4 + 4 + dim * 4 + 8 ||
      layout.label_offset != 2 * m * 4 + 4 + dim * 4 || layout.data_offset != 2 * m * 4 + 4) {
    return fail("layout header");
  }
  hipvs_layout::rewrite_labels(path.c_str(), layout, ids);
  const std::vector<int64_t> rewritten = read_labels(path, layout, dim, labels);
  for (uint64_t row = 0; row < n; ++row) {
    if (rewritten[row] != ids[labels[row]]) { return fail("rewritten label"); }
  }

  // A duplicate point id must be rejected instead of silently corrupting labels.
  std::vector<uint64_t> duplicate(n, 0);
  make_file(path, n, m, dim, duplicate, false);
  bool threw = false;
  try {
    const hipvs_layout::Layout bad = hipvs_layout::read_layout(path.c_str());
    hipvs_layout::rewrite_labels(path.c_str(), bad, ids);
  } catch (const std::exception &) { threw = true; }
  if (!threw) { return fail("duplicate label not rejected"); }

  // A truncated record area must be rejected.
  make_file(path, n, m, dim, labels, true);
  threw = false;
  try {
    const hipvs_layout::Layout bad = hipvs_layout::read_layout(path.c_str());
    hipvs_layout::rewrite_labels(path.c_str(), bad, ids);
  } catch (const std::exception &) { threw = true; }
  if (!threw) { return fail("truncated graph not rejected"); }

  // A mismatched id vector must be rejected.
  make_file(path, n, m, dim, labels, false);
  threw = false;
  try {
    const hipvs_layout::Layout bad = hipvs_layout::read_layout(path.c_str());
    std::vector<int64_t> fewer(ids.begin(), ids.end() - 1);
    hipvs_layout::rewrite_labels(path.c_str(), bad, fewer);
  } catch (const std::exception &) { threw = true; }
  if (!threw) { return fail("id count mismatch not rejected"); }

  std::remove(path.c_str());
  std::printf("PASS: label rewrite, duplicate, truncation and count checks\n");
  return 0;
}
