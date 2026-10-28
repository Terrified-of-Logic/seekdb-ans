/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <vsag/constants.h>
#ifndef _WIN32
#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
extern char **environ;
#endif

namespace oceanbase { namespace common { namespace obvsag {

// A snapshot owns this staging buffer. Streaming/incremental handles never enable it.
//
// The GPU helper is spawned as soon as the first snapshot batch is staged and
// then blocks on a start byte until every vector has been written. ROCm/hipVS
// process initialization therefore overlaps the SQL snapshot scan instead of
// extending the critical path. Graph construction still happens entirely inside
// the measured window, and the helper only receives the staged vectors after
// the scan has finished.
class HipvsSnapshot {
public:
  std::atomic<bool> enabled{false};
  std::vector<float> vectors;
  std::vector<int64_t> ids;
  std::mutex mutex;

  HipvsSnapshot() = default;
  HipvsSnapshot(const HipvsSnapshot &) = delete;
  HipvsSnapshot &operator=(const HipvsSnapshot &) = delete;
  ~HipvsSnapshot() { cleanup(); }

  void configure(int dim, int m, int ef)
  {
    (void)dim; (void)ef;
    m_ = m;
    const char *prewarm = std::getenv("HIPVS_SNAPSHOT_PREWARM");
    prewarm_ = prewarm == nullptr || std::strcmp(prewarm, "0") != 0;
  }

  void reset()
  {
    enabled = false;
    cleanup();
    spawn_attempted_ = false;
    helper_prestarted_ = false;
    std::vector<float>().swap(vectors);
    std::vector<int64_t>().swap(ids);
  }

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
    // Start process/driver initialization now, while the scan is still running.
    // Snapshots small enough to stay on the CPU path never pay for a helper.
    if (count > 0 && prewarm_ && !spawn_attempted_ && m_ > 0 &&
        ids.size() + static_cast<uint64_t>(count) > static_cast<uint64_t>(4 * m_)) {
      spawn_attempted_ = true;
      if (start_helper() != OB_SUCCESS) {
        // Non-fatal: finish() retries synchronously and falls back to CPU on failure.
        cleanup();
      } else {
        helper_prestarted_ = true;
      }
    }
    for (uint64_t i = 0; i < static_cast<uint64_t>(count) * dim; ++i) {
      if (!std::isfinite(data[i])) { return OB_INVALID_ARGUMENT; }
    }
    try {
      // DDL can supply one vector per call. Exact-size reserve here would copy
      // the entire growing dataset per row and turn staging into quadratic work.
      if (total > vectors.capacity()) {
        vectors.reserve(std::min<uint64_t>(1ULL << 28,
            std::max<uint64_t>(total, std::max<uint64_t>(256, vectors.capacity() * 2))));
      }
      const uint64_t new_ids = ids.size() + count;
      if (new_ids > ids.capacity()) {
        ids.reserve(std::max<uint64_t>(new_ids, std::max<uint64_t>(16, ids.capacity() * 2)));
      }
      if (count) {
        vectors.insert(vectors.end(), data, data + static_cast<uint64_t>(count) * dim);
        ids.insert(ids.end(), labels, labels + count);
      }
    } catch (const std::bad_alloc &) { return OB_ALLOCATE_MEMORY_FAILED; }
    return OB_SUCCESS;
  }

  int finish(std::shared_ptr<vsag::Index> &index, int dim, int m, int ef)
  {
#ifdef _WIN32
    (void)index; (void)dim; (void)m; (void)ef;
    return OB_NOT_SUPPORTED;
#else
    if (!enabled) { return OB_SUCCESS; }
    if (ids.empty()) {
      enabled = false;
      cleanup();
      return OB_SUCCESS;
    }
    if (ids.size() <= static_cast<uint64_t>(4 * m)) {
      auto data = vsag::Dataset::Make()->NumElements(ids.size())->Dim(dim)
          ->Ids(ids.data())->Float32Vectors(vectors.data())->Owner(false);
      auto result = index->Add(data);
      if (!result.has_value() || !result.value().empty()) { return OB_ERR_VSAG_RETURN_ERROR; }
      LOG_INFO("HIPVS_CPU_FALLBACK reason=small_snapshot", "rows", ids.size(), K(m));
      reset();
      return OB_SUCCESS;
    }
    int ret = OB_SUCCESS;
    LOG_INFO("HIPVS_SNAPSHOT_FINISH", "rows", ids.size(),
             "prestarted", static_cast<int>(helper_prestarted_));
    if (OB_FAIL(run_build(index, dim, m, ef))) {
      cleanup();
      return ret;
    }
    LOG_INFO("HIPVS_NATIVE_BUILD_COMPLETE fallback=false", "rows", ids.size(), K(dim), K(m));
    reset();
    return OB_SUCCESS;
#endif
  }

private:
  static double now_ms()
  {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  int start_helper()
  {
#ifndef _WIN32
    if (helper_pid_ > 0) { return OB_SUCCESS; }
    const char *helper = std::getenv("SEEKDB_HIPVS_BUILD_HELPER");
    if (helper == nullptr || helper[0] != '/' || access(helper, X_OK) != 0) {
      return OB_INVALID_ARGUMENT;
    }
    char dir[] = "/tmp/seekdb-hipvs-XXXXXX";
    if (mkdtemp(dir) == nullptr) { return OB_IO_ERROR; }
    dir_ = dir;
    in_path_ = dir_ + "/vectors";
    out_path_ = dir_ + "/graph";
    log_path_ = dir_ + "/helper.log";
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
      cleanup();
      return OB_IO_ERROR;
    }
    // The child must not inherit the parent's write end, otherwise losing the
    // parent would leave the helper blocked forever instead of seeing EOF.
    bool cloexec_ok = true;
    for (int i = 0; i < 2; ++i) {
      const int flags = fcntl(sockets[i], F_GETFD);
      cloexec_ok = cloexec_ok && flags >= 0 &&
          fcntl(sockets[i], F_SETFD, flags | FD_CLOEXEC) == 0;
    }
    if (!cloexec_ok) {
      ::close(sockets[0]);
      ::close(sockets[1]);
      cleanup();
      return OB_IO_ERROR;
    }
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
      ::close(sockets[0]);
      ::close(sockets[1]);
      cleanup();
      return OB_IO_ERROR;
    }
    bool actions_ok = posix_spawn_file_actions_adddup2(&actions, sockets[0], STDIN_FILENO) == 0;
    actions_ok = actions_ok &&
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_path_.c_str(),
                                         O_WRONLY | O_CREAT | O_APPEND, 0600) == 0;
    actions_ok = actions_ok &&
        posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO) == 0;
    char *argv[] = {const_cast<char *>(helper), const_cast<char *>(in_path_.c_str()),
                    const_cast<char *>(out_path_.c_str()), const_cast<char *>("--wait-stdin"),
                    nullptr};
    pid_t pid = -1;
    int rc = actions_ok ? posix_spawn(&pid, helper, &actions, nullptr, argv, ::environ) : -1;
    posix_spawn_file_actions_destroy(&actions);
    ::close(sockets[0]);
    if (rc != 0) {
      ::close(sockets[1]);
      cleanup();
      return OB_IO_ERROR;
    }
    helper_pid_ = pid;
    start_fd_ = sockets[1];
    return OB_SUCCESS;
#else
    return OB_NOT_SUPPORTED;
#endif
  }

  int run_build(std::shared_ptr<vsag::Index> &index, int dim, int m, int ef)
  {
#ifndef _WIN32
    int ret = OB_SUCCESS;
    const double finish_begin = now_ms();
    if (helper_pid_ <= 0 && OB_FAIL(start_helper())) { return OB_ERR_VSAG_RETURN_ERROR; }
    // The helper may still be initializing; writing the payload does not block it.
    {
      uint64_t header[] = {0x315350564849ULL, ids.size(), static_cast<uint64_t>(dim),
                           static_cast<uint64_t>(m), static_cast<uint64_t>(ef)};
      std::ofstream out(in_path_, std::ios::binary);
      out.write(reinterpret_cast<const char *>(header), sizeof(header));
      out.write(reinterpret_cast<const char *>(ids.data()), ids.size() * sizeof(int64_t));
      out.write(reinterpret_cast<const char *>(vectors.data()), vectors.size() * sizeof(float));
      out.close();
      if (!out) {
        LOG_WARN("HIPVS_STAGING_WRITE_FAILED", "path", in_path_.c_str());
        return OB_IO_ERROR;
      }
    }
    const double write_done = now_ms();
    if (start_fd_ >= 0) {
      const char start = 'B';
      ssize_t sent = ::send(start_fd_, &start, 1, MSG_NOSIGNAL);
      ::close(start_fd_);
      start_fd_ = -1;
      if (sent != 1) {
        LOG_WARN("HIPVS_HELPER_START_FAILED", "rows", ids.size(), "errno", errno);
        return OB_ERR_VSAG_RETURN_ERROR;
      }
    } else {
      return OB_ERR_UNEXPECTED;
    }
    int status = 0;
    bool done = false;
    for (int i = 0; i < 12000; ++i) {
      pid_t waited = waitpid(helper_pid_, &status, WNOHANG);
      if (waited == helper_pid_) {
        done = true;
        helper_pid_ = -1;
        break;
      }
      if (waited < 0 && errno != EINTR) { break; }
      usleep(50000);
    }
    const double wait_done = now_ms();
    log_helper_output();
    if (!done) {
      LOG_WARN("HIPVS_BUILD_TIMEOUT", "rows", ids.size());
      return OB_TIMEOUT;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      LOG_WARN("HIPVS_BUILD_FAILED", K(status), "rows", ids.size());
      return OB_ERR_VSAG_RETURN_ERROR;
    }

    std::ifstream input(out_path_, std::ios::binary | std::ios::ate);
    const int64_t length = input.tellg();
    if (!input || length < 96 || static_cast<uint64_t>(length) > (1ULL << 33)) {
      return OB_IO_ERROR;
    }
    vsag::Binary graph;
    graph.size = length;
    graph.data.reset(new int8_t[length]);
    input.seekg(0);
    input.read(reinterpret_cast<char *>(graph.data.get()), length);
    if (!input) { return OB_IO_ERROR; }
    const double read_done = now_ms();

    // Validate the complete serialized graph without one random graph lookup per
    // row: each fixed-size record must carry the staged label and vector bytes.
    const uint64_t *h = reinterpret_cast<const uint64_t *>(graph.data.get());
    const uint64_t rows = h[2], row_size = h[3], label_offset = h[4], data_offset = h[5];
    if (rows != ids.size() || row_size != 2 * m * 4 + 4 + dim * 4 + 8 ||
        label_offset != 2 * m * 4 + 4 + dim * 4 || data_offset != 2 * m * 4 + 4 ||
        96 + rows * row_size > static_cast<uint64_t>(length)) {
      LOG_WARN("HIPVS_GRAPH_LAYOUT_MISMATCH", K(rows), K(row_size), K(label_offset), K(data_offset));
      return OB_ERR_UNEXPECTED;
    }
    // The native serializer does not preserve staged row order, so map each
    // stored label back to its input row before comparing the vector bytes.
    std::unordered_map<int64_t, size_t> id_to_row;
    id_to_row.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) { id_to_row.emplace(ids[i], i); }
    if (id_to_row.size() != ids.size()) {
      LOG_WARN("HIPVS_GRAPH_DUPLICATE_ID", "rows", ids.size());
      return OB_ERR_UNEXPECTED;
    }
    const char *base = reinterpret_cast<const char *>(graph.data.get());
    for (size_t row = 0; row < ids.size(); ++row) {
      const char *record = base + 96 + row * row_size;
      int64_t label = 0;
      std::memcpy(&label, record + label_offset, sizeof(label));
      const auto found = id_to_row.find(label);
      if (found == id_to_row.end()) {
        LOG_WARN("HIPVS_GRAPH_LABEL_MISMATCH", K(row), K(label));
        return OB_ERR_UNEXPECTED;
      }
      if (std::memcmp(record + data_offset, vectors.data() + found->second * dim,
                      static_cast<size_t>(dim) * sizeof(float)) != 0) {
        LOG_WARN("HIPVS_GRAPH_VECTOR_MISMATCH", K(row), "input_row", found->second);
        return OB_ERR_UNEXPECTED;
      }
    }
    const double validate_done = now_ms();

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
    // A few native lookups still prove that the imported graph resolves labels
    // to the staged vectors; the full byte check above covers every row.
    const size_t probes[] = {0, ids.size() / 2, ids.size() - 1};
    for (size_t probe : probes) {
      auto distance = index->CalcDistanceById(vectors.data() + probe * dim, ids[probe]);
      if (!distance.has_value() || !std::isfinite(distance.value()) || distance.value() != 0) {
        LOG_WARN("HIPVS_IMPORT_PROBE_FAILED", K(probe));
        return OB_ERR_UNEXPECTED;
      }
    }
    const double import_done = now_ms();
    LOG_INFO("HIPVS_PARENT_TIMINGS", "rows", ids.size(),
             "prestarted", static_cast<int>(helper_prestarted_),
             "write_ms", static_cast<int64_t>(write_done - finish_begin),
             "wait_ms", static_cast<int64_t>(wait_done - write_done),
             "read_ms", static_cast<int64_t>(read_done - wait_done),
             "validate_ms", static_cast<int64_t>(validate_done - read_done),
             "import_ms", static_cast<int64_t>(import_done - validate_done),
             "total_ms", static_cast<int64_t>(import_done - finish_begin));
    return OB_SUCCESS;
#else
    (void)index; (void)dim; (void)m; (void)ef;
    return OB_NOT_SUPPORTED;
#endif
  }

  void log_helper_output()
  {
#ifndef _WIN32
    std::ifstream in(log_path_);
    std::string line;
    int kept = 0;
    while (std::getline(in, line) && kept < 20) {
      if (!line.empty()) {
        LOG_INFO("HIPVS_HELPER_LOG", "line", line.c_str());
        ++kept;
      }
    }
#endif
  }

  void cleanup()
  {
#ifndef _WIN32
    if (start_fd_ >= 0) {
      ::close(start_fd_);
      start_fd_ = -1;
    }
    if (helper_pid_ > 0) {
      kill(helper_pid_, SIGKILL);
      int status = 0;
      while (waitpid(helper_pid_, &status, 0) < 0 && errno == EINTR) {}
      helper_pid_ = -1;
    }
    if (!dir_.empty()) {
      unlink(in_path_.c_str());
      unlink(out_path_.c_str());
      unlink(log_path_.c_str());
      rmdir(dir_.c_str());
      dir_.clear();
    }
#endif
  }

  int m_ = 0;
  bool prewarm_ = true;
  bool spawn_attempted_ = false;
  bool helper_prestarted_ = false;
#ifndef _WIN32
  pid_t helper_pid_ = -1;
  int start_fd_ = -1;
#endif
  std::string dir_;
  std::string in_path_;
  std::string out_path_;
  std::string log_path_;
};

}}}
