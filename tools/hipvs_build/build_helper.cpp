/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
// hipVS C API deliberately retains cuVS symbol names on AMD ROCm.
//
// The helper is normally spawned when the first snapshot batch arrives and is
// kept blocked on stdin until the parent has finished staging every vector.
// That lets ROCm/hipVS process initialization overlap the SQL snapshot scan;
// all graph construction still happens inside the measured build window.
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/hnsw.h>
#include <hip/hip_runtime.h>
#include <sched.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "hnsw_labels.h"

static double now_ms()
{
  return std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

static int env_int(const char *name, int fallback)
{
  const char *text = std::getenv(name);
  if (text == nullptr || *text == '\0') { return fallback; }
  char *end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || value < 0 || value > 1000000) { return fallback; }
  return static_cast<int>(value);
}

static void check(cuvsError_t result)
{
  if (result != CUVS_SUCCESS) { throw std::runtime_error(cuvsGetLastErrorText()); }
}

struct Resources {
  cuvsResources_t res{};
  cuvsCagraIndex_t graph = nullptr;
  cuvsCagraIndexParams_t params = nullptr;
  cuvsHnswIndex_t hnsw = nullptr;
  cuvsHnswIndexParams_t hp = nullptr;
  Resources() { check(cuvsResourcesCreate(&res)); }
  ~Resources() {
    if (hnsw) cuvsHnswIndexDestroy(hnsw);
    if (hp) cuvsHnswIndexParamsDestroy(hp);
    if (graph) cuvsCagraIndexDestroy(graph);
    if (params) cuvsCagraIndexParamsDestroy(params);
    cuvsResourcesDestroy(res);
  }
};

// Host-side NN-descent bookkeeping is memory-latency bound. When the container
// is allowed far more CPUs than its cgroup quota, unpinned worker threads get
// scattered across NUMA domains and the host phase becomes several times
// slower. Keep the process on a compact set of at most 16 allowed CPUs.
static int cgroup_cpu_quota()
{
  std::ifstream v2("/sys/fs/cgroup/cpu.max");
  std::string quota;
  std::string period;
  if (v2 >> quota >> period) {
    if (quota == "max") { return 0; }
    const long q = std::strtol(quota.c_str(), nullptr, 10);
    const long p = std::strtol(period.c_str(), nullptr, 10);
    if (q > 0 && p > 0) { return static_cast<int>(std::max(1L, q / p)); }
    return 0;
  }
  std::ifstream q1("/sys/fs/cgroup/cpu/cpu.cfs_quota_us");
  std::ifstream p1("/sys/fs/cgroup/cpu/cpu.cfs_period_us");
  long q = -1;
  long p = 100000;
  if (q1 >> q && p1 >> p && q > 0 && p > 0) {
    return static_cast<int>(std::max(1L, q / p));
  }
  return 0;
}

// Returns the effective CPU budget. Pinning only happens when the container is
// allowed more CPUs than its cgroup quota, which is the NUMA-scattering case.
static int compact_affinity()
{
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) { return 0; }
  const int allowed_count = CPU_COUNT(&allowed);
  const int quota = cgroup_cpu_quota();
  int target = allowed_count;
  if (quota > 0 && quota < target) { target = quota; }
  if (target >= allowed_count) { return allowed_count; }
  cpu_set_t chosen;
  CPU_ZERO(&chosen);
  int added = 0;
  for (int cpu = 0; cpu < CPU_SETSIZE && added < target; ++cpu) {
    if (CPU_ISSET(cpu, &allowed)) {
      CPU_SET(cpu, &chosen);
      ++added;
    }
  }
  if (added == target && sched_setaffinity(0, sizeof(chosen), &chosen) == 0) {
    return target;
  }
  return allowed_count;
}

int main(int argc, char **argv)
{
  const int cpu_budget = compact_affinity();
  const double process_start = now_ms();
  try {
    const bool wait_for_parent = argc == 4 && std::strcmp(argv[3], "--wait-stdin") == 0;
    if (argc != 3 && !wait_for_parent) {
      throw std::runtime_error("usage: hipvs-build-helper INPUT OUTPUT [--wait-stdin]");
    }
    int devices = 0;
    if (hipGetDeviceCount(&devices) != hipSuccess || devices < 1) {
      throw std::runtime_error("no AMD GPU");
    }
    hipDeviceProp_t prop {};
    if (hipGetDeviceProperties(&prop, 0) != hipSuccess) {
      throw std::runtime_error("GPU properties failed");
    }
    // Initialize the GPU runtime before the snapshot scan completes. This is
    // process/driver setup only; no vector or graph work is done here.
    Resources r;
    const double init_done = now_ms();
    double wait_ms = 0;
    if (wait_for_parent) {
      const double wait_begin = now_ms();
      char start = 0;
      ssize_t got = 0;
      do {
        got = ::read(STDIN_FILENO, &start, 1);
      } while (got < 0 && errno == EINTR);
      if (got != 1 || start != 'B') { throw std::runtime_error("parent did not start the build"); }
      wait_ms = now_ms() - wait_begin;
    }
    const double load_begin = now_ms();
    std::ifstream input(argv[1], std::ios::binary);
    uint64_t head[5] {};
    input.read(reinterpret_cast<char *>(head), sizeof(head));
    const uint64_t n = head[1], dim = head[2], m = head[3];
    if (!input || head[0] != 0x315350564849ULL || dim == 0 || dim > 4096 ||
        m < 8 || m > 64 || n <= 4 * m || n > (1ULL << 28) / dim || head[4] > 100000) {
      throw std::runtime_error("invalid input shape/parameters (N must exceed 4*M)");
    }
    std::vector<int64_t> ids(n);
    std::vector<float> data(n * dim);
    input.read(reinterpret_cast<char *>(ids.data()), ids.size() * sizeof(int64_t));
    input.read(reinterpret_cast<char *>(data.data()), data.size() * sizeof(float));
    if (!input || input.peek() != EOF) { throw std::runtime_error("truncated input"); }
    std::vector<int64_t> sorted_ids(ids);
    std::sort(sorted_ids.begin(), sorted_ids.end());
    if (std::adjacent_find(sorted_ids.begin(), sorted_ids.end()) != sorted_ids.end()) {
      throw std::runtime_error("duplicate VID");
    }
    for (float value : data) {
      if (!std::isfinite(value)) { throw std::runtime_error("nonfinite vector"); }
    }
    const double load_done = now_ms();

    check(cuvsCagraIndexCreate(&r.graph));
    check(cuvsCagraIndexParamsCreate(&r.params));
    r.params->metric = L2Expanded;
    r.params->graph_degree = 2 * m;
    // 4*m is the default intermediate degree; the override exists only for
    // controlled build-throughput experiments, never to weaken HNSW degree.
    r.params->intermediate_graph_degree =
        std::max<int>(r.params->graph_degree, env_int("HIPVS_CAGRA_INTERMEDIATE_DEGREE", 4 * m));
    const char *build_algo = std::getenv("HIPVS_CAGRA_BUILD_ALGO");
    r.params->build_algo = (build_algo != nullptr && std::strcmp(build_algo, "ivf_pq") == 0)
        ? IVF_PQ : NN_DESCENT;
    r.params->nn_descent_niter = env_int("HIPVS_CAGRA_NITER", 20);
    int64_t shape[] = {static_cast<int64_t>(n), static_cast<int64_t>(dim)};
    DLManagedTensor tensor {};
    tensor.dl_tensor.data = data.data();
    tensor.dl_tensor.device = {kDLCPU, 0};
    tensor.dl_tensor.ndim = 2;
    tensor.dl_tensor.dtype = {kDLFloat, 32, 1};
    tensor.dl_tensor.shape = shape;
    const double cagra_begin = now_ms();
    check(cuvsCagraBuild(r.res, r.params, &tensor, r.graph));
    if (hipDeviceSynchronize() != hipSuccess) { throw std::runtime_error("GPU sync failed"); }
    const double cagra_done = now_ms();

    check(cuvsHnswIndexCreate(&r.hnsw));
    check(cuvsHnswIndexParamsCreate(&r.hp));
    r.hp->hierarchy = CPU; // GPU builds the base graph; CPU constructs only the sparse upper levels.
    r.hp->ef_construction = static_cast<int>(head[4]);
    r.hp->num_threads = env_int("HIPVS_HNSW_THREADS",
        cpu_budget > 0 ? std::min(16, cpu_budget) : 16);
    check(cuvsHnswFromCagra(r.res, r.hp, r.graph, r.hnsw));
    check(cuvsHnswSerialize(r.res, argv[2], r.hnsw));
    const double serialize_done = now_ms();

    // Native hnswlib format on x86_64: 96-byte header, then fixed-size records
    // [links(2*M*4)][level(4)][vector(dim*4)][label(8)].
    const hipvs_layout::Layout layout = hipvs_layout::read_layout(argv[2]);
    if (layout.n != n || layout.row_size != 2 * m * 4 + 4 + dim * 4 + 8 ||
        layout.label_offset != 2 * m * 4 + 4 + dim * 4 ||
        layout.data_offset != 2 * m * 4 + 4) {
      throw std::runtime_error("unsupported HNSW serialization layout");
    }
    hipvs_layout::rewrite_labels(argv[2], layout, ids);
    const double rewrite_done = now_ms();

    const double seconds = (rewrite_done - process_start) / 1000.0;
    std::cerr << "HIPVS_GPU_BUILD_SUCCESS device=" << prop.gcnArchName << " rows=" << n
              << " dim=" << dim << " seconds=" << seconds
              << " init_ms=" << (init_done - process_start)
              << " wait_ms=" << wait_ms
              << " load_ms=" << (load_done - load_begin)
              << " cagra_ms=" << (cagra_done - cagra_begin)
              << " convert_ms=" << (serialize_done - cagra_done)
              << " rewrite_ms=" << (rewrite_done - serialize_done)
              << " fallback=false\n";
    // The graph file is already synced and closed. Skipping cuVS/hnswlib
    // teardown keeps destructor work out of the measured build.
    std::cerr.flush();
    std::_Exit(0);
  } catch (const std::exception &e) {
    std::cerr << "HIPVS_GPU_BUILD_FAILED fallback=false error=" << e.what() << '\n';
    return 1;
  }
}
