/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <vector>
#include <vsag/constants.h>
#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
extern char **environ;
#endif

namespace oceanbase { namespace common { namespace obvsag {

// A snapshot owns this staging buffer. Streaming/incremental handles never enable it.
class HipvsSnapshot {
public:
  std::atomic<bool> enabled{false};
  std::vector<float> vectors;
  std::vector<int64_t> ids;
  std::mutex mutex;

  int append(const float *data, const int64_t *labels, int count, int dim)
  {
    if (count < 0 || dim <= 0 || (count && (!data || !labels))) {
      return OB_INVALID_ARGUMENT;
    }
    const uint64_t total = (ids.size() + static_cast<uint64_t>(count)) * dim;
    // Bound staging memory independently of GPU availability; never silently drop rows.
    if (total > (1ULL << 28) || total > vectors.max_size()) {
      return OB_ALLOCATE_MEMORY_FAILED;
    }
    for (uint64_t i = 0; i < static_cast<uint64_t>(count) * dim; ++i) {
      if (!std::isfinite(data[i])) { return OB_INVALID_ARGUMENT; }
    }
    try {
      vectors.reserve(total);
      ids.reserve(ids.size() + count);
      if (count) {
        vectors.insert(vectors.end(), data, data + static_cast<uint64_t>(count) * dim);
        ids.insert(ids.end(), labels, labels + count);
      }
    } catch (const std::bad_alloc &) { return OB_ALLOCATE_MEMORY_FAILED; }
    return OB_SUCCESS;
  }

  int finish(std::shared_ptr<vsag::Index> &index, int dim, int m, int ef)
  {
    int ret = OB_SUCCESS;
#ifdef _WIN32
    return OB_NOT_SUPPORTED;
#else
    if (!enabled) { return OB_SUCCESS; }
    if (ids.empty()) { enabled = false; return OB_SUCCESS; }
    if (ids.size() <= static_cast<uint64_t>(4 * m)) {
      auto data = vsag::Dataset::Make()->NumElements(ids.size())->Dim(dim)
          ->Ids(ids.data())->Float32Vectors(vectors.data())->Owner(false);
      auto result = index->Add(data);
      if (!result.has_value() || !result.value().empty()) { return OB_ERR_VSAG_RETURN_ERROR; }
      LOG_INFO("HIPVS_CPU_FALLBACK reason=small_snapshot", "rows", ids.size(), K(m));
      enabled = false;
      std::vector<float>().swap(vectors);
      std::vector<int64_t>().swap(ids);
      return OB_SUCCESS;
    }
    const char *helper = std::getenv("SEEKDB_HIPVS_BUILD_HELPER");
    if (!helper || helper[0] != '/' || access(helper, X_OK) != 0) {
      return OB_INVALID_ARGUMENT;
    }
    if (std::set<int64_t>(ids.begin(), ids.end()).size() != ids.size()) {
      return OB_INVALID_ARGUMENT;
    }
    char dir[] = "/tmp/seekdb-hipvs-XXXXXX";
    if (!mkdtemp(dir)) { return OB_IO_ERROR; }
    struct Cleanup {
      std::string in, out, dir;
      ~Cleanup() { unlink(in.c_str()); unlink(out.c_str()); rmdir(dir.c_str()); }
    } files{std::string(dir) + "/vectors", std::string(dir) + "/graph", dir};
    uint64_t header[] = {0x315350564849ULL, ids.size(), static_cast<uint64_t>(dim),
                         static_cast<uint64_t>(m), static_cast<uint64_t>(ef)};
    {
      std::ofstream out(files.in, std::ios::binary);
      out.write(reinterpret_cast<const char *>(header), sizeof(header));
      out.write(reinterpret_cast<const char *>(ids.data()), ids.size() * sizeof(int64_t));
      out.write(reinterpret_cast<const char *>(vectors.data()), vectors.size() * sizeof(float));
      out.close();
      if (!out) { return OB_IO_ERROR; }
    }
    char *argv[] = {const_cast<char *>(helper), const_cast<char *>(files.in.c_str()),
                    const_cast<char *>(files.out.c_str()), nullptr};
    pid_t pid;
    int rc = posix_spawn(&pid, helper, nullptr, nullptr, argv, ::environ);
    if (rc != 0) { return OB_IO_ERROR; }
    int status = 0;
    bool done = false;
    for (int i = 0; i < 6000; ++i) {
      pid_t waited = waitpid(pid, &status, WNOHANG);
      if (waited == pid) { done = true; break; }
      if (waited < 0 && errno != EINTR) { break; }
      usleep(50000);
    }
    if (!done) {
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
      return OB_TIMEOUT;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      LOG_WARN("HIPVS_BUILD_FAILED no CPU fallback", K(status), K(pid));
      return OB_ERR_VSAG_RETURN_ERROR;
    }
    // Import the complete native graph; do not perform CPU Build/Add first.
    std::ifstream input(files.out, std::ios::binary | std::ios::ate);
    const int64_t length = input.tellg();
    if (!input || length < 96 || static_cast<uint64_t>(length) > (1ULL << 32)) {
      return OB_IO_ERROR;
    }
    vsag::Binary graph;
    graph.size = length;
    graph.data.reset(new int8_t[length]);
    input.seekg(0);
    input.read(reinterpret_cast<char *>(graph.data.get()), length);
    if (!input) { return OB_IO_ERROR; }
    vsag::BinarySet set;
    set.Set(vsag::HNSW_DATA, graph);
    auto imported = index->Deserialize(set);
    if (!imported.has_value()) {
      LOG_WARN("HIPVS_IMPORT_FAILED", "reason", imported.error().message.c_str());
      return OB_ERR_VSAG_RETURN_ERROR;
    }
    if (index->GetNumElements() != static_cast<int64_t>(ids.size())) {
      return OB_ERR_UNEXPECTED;
    }
    // Validate every original VID and its corresponding uncompressed vector.
    for (size_t i = 0; i < ids.size(); ++i) {
      auto distance = index->CalcDistanceById(vectors.data() + i * dim, ids[i]);
      if (!distance.has_value() || !std::isfinite(distance.value()) || distance.value() != 0) {
        return OB_ERR_UNEXPECTED;
      }
    }
    LOG_INFO("HIPVS_NATIVE_BUILD_COMPLETE fallback=false", "rows", ids.size(), K(dim), K(m));
    enabled = false;
    std::vector<float>().swap(vectors);
    std::vector<int64_t>().swap(ids);
    return OB_SUCCESS;
#endif
  }
};
}}}
