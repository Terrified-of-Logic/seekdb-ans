/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
// hipVS C API deliberately retains cuVS symbol names on AMD ROCm.
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/hnsw.h>
#include <hip/hip_runtime.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>

static void check(cuvsError_t result) {
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
int main(int argc, char **argv) {
  try {
    if (argc != 3) throw std::runtime_error("usage: hipvs-build-helper INPUT OUTPUT");
    const auto start = std::chrono::steady_clock::now();
    std::ifstream input(argv[1], std::ios::binary);
    uint64_t head[5]{};
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
    if (!input || input.peek() != EOF || std::set<int64_t>(ids.begin(), ids.end()).size() != n)
      throw std::runtime_error("truncated input or duplicate VID");
    for (float value : data) if (!std::isfinite(value)) throw std::runtime_error("nonfinite vector");
    int devices = 0;
    if (hipGetDeviceCount(&devices) != hipSuccess || devices < 1) throw std::runtime_error("no AMD GPU");
    hipDeviceProp_t prop{};
    if (hipGetDeviceProperties(&prop, 0) != hipSuccess) throw std::runtime_error("GPU properties failed");
    Resources r;
    check(cuvsCagraIndexCreate(&r.graph));
    check(cuvsCagraIndexParamsCreate(&r.params));
    r.params->metric = L2Expanded;
    r.params->graph_degree = 2 * m;
    r.params->intermediate_graph_degree = 4 * m;
    r.params->build_algo = NN_DESCENT;
    r.params->nn_descent_niter = 20;
    int64_t shape[] = {static_cast<int64_t>(n), static_cast<int64_t>(dim)};
    DLManagedTensor tensor{};
    tensor.dl_tensor.data = data.data();
    tensor.dl_tensor.device = {kDLCPU, 0};
    tensor.dl_tensor.ndim = 2;
    tensor.dl_tensor.dtype = {kDLFloat, 32, 1};
    tensor.dl_tensor.shape = shape;
    check(cuvsCagraBuild(r.res, r.params, &tensor, r.graph));
    if (hipDeviceSynchronize() != hipSuccess) throw std::runtime_error("GPU sync failed");
    check(cuvsHnswIndexCreate(&r.hnsw));
    check(cuvsHnswIndexParamsCreate(&r.hp));
    r.hp->hierarchy = CPU; // GPU builds base graph; CPU constructs only sparse upper levels.
    r.hp->ef_construction = head[4];
    r.hp->num_threads = 16;
    check(cuvsHnswFromCagra(r.res, r.hp, r.graph, r.hnsw));
    check(cuvsHnswSerialize(r.res, argv[2], r.hnsw));
    // Native hnswlib format on x86_64: six uint64, int32 level, uint32 entry,
    // three uint64 degrees, double multiplier, uint64 ef => 96-byte header.
    std::fstream output(argv[2], std::ios::binary | std::ios::in | std::ios::out);
    uint64_t h[6]{};
    output.read(reinterpret_cast<char *>(h), sizeof(h));
    if (!output || h[2] != n || h[3] != 2*m*4+4+dim*4+8 ||
        h[4] != 2*m*4+4+dim*4 || h[5] != 2*m*4+4) {
      throw std::runtime_error("unsupported HNSW serialization layout");
    }
    std::vector<bool> seen(n, false);
    for (uint64_t row = 0; row < n; ++row) {
      const uint64_t offset = 96 + row * h[3] + h[4];
      uint64_t label = n;
      output.seekg(offset);
      output.read(reinterpret_cast<char *>(&label), sizeof(label));
      if (!output || label >= n || seen[label]) throw std::runtime_error("invalid GPU label mapping");
      seen[label] = true;
      output.seekp(offset);
      output.write(reinterpret_cast<const char *>(&ids[label]), sizeof(int64_t));
      if (!output) throw std::runtime_error("VID rewrite failed");
    }
    output.close();
    if (!output) throw std::runtime_error("output flush failed");
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::cerr << "HIPVS_GPU_BUILD_SUCCESS device=" << prop.gcnArchName << " rows=" << n
              << " dim=" << dim << " seconds=" << seconds << " fallback=false\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "HIPVS_GPU_BUILD_FAILED fallback=false error=" << e.what() << '\n';
    return 1;
  }
}
