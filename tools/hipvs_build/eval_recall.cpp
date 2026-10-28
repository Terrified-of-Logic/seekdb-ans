// Evaluate Recall@K of a serialized native HNSW graph without seekdb.
#include "hnswlib/hnswlib.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <queue>
#include <string>
#include <vector>

static std::vector<float> read_floats(const char *path, size_t count)
{
  std::vector<float> out(count);
  std::ifstream in(path, std::ios::binary);
  in.read(reinterpret_cast<char *>(out.data()), count * sizeof(float));
  if (!in) { std::fprintf(stderr, "bad file %s\n", path); std::exit(2); }
  return out;
}

static std::vector<int64_t> read_ids(const char *path, size_t count)
{
  std::vector<int64_t> out(count);
  std::ifstream in(path, std::ios::binary);
  in.read(reinterpret_cast<char *>(out.data()), count * sizeof(int64_t));
  if (!in) { std::fprintf(stderr, "bad file %s\n", path); std::exit(2); }
  return out;
}

int main(int argc, char **argv)
{
  if (argc != 8) {
    std::fprintf(stderr, "usage: %s INDEX QUERIES TRUTH DIM NQ K EF\n", argv[0]);
    return 2;
  }
  const char *index_path = argv[1];
  const size_t dim = std::strtoul(argv[4], nullptr, 10);
  const size_t nq = std::strtoul(argv[5], nullptr, 10);
  const size_t k = std::strtoul(argv[6], nullptr, 10);
  const size_t ef = std::strtoul(argv[7], nullptr, 10);
  hnswlib::L2Space<float> space(dim);
  hnswlib::HierarchicalNSW<float> index(&space, index_path);
  index.setEf(ef);
  const std::vector<float> queries = read_floats(argv[2], nq * dim);
  const std::vector<int64_t> truth = read_ids(argv[3], nq * k);
  size_t hits = 0;
  for (size_t q = 0; q < nq; ++q) {
    const auto result = index.searchKnn(queries.data() + q * dim, k);
    std::vector<int64_t> got;
    auto queue = result;
    while (!queue.empty()) { got.push_back(queue.top().second); queue.pop(); }
    for (const int64_t label : got) {
      for (size_t t = 0; t < k; ++t) {
        if (truth[q * k + t] == label) { ++hits; break; }
      }
    }
  }
  std::printf("nq=%zu k=%zu ef=%zu recall=%.4f\n", nq, k, ef, double(hits) / double(nq * k));
  return 0;
}
