/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <cstdint>
#include <vsag/vsag.h>
#include <vsag/constants.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <set>
#include <chrono>
#include <stdexcept>

template<class T> std::vector<T> read(const std::string &path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) throw std::runtime_error("open: " + path);
  auto bytes = f.tellg();
  std::vector<T> data(static_cast<size_t>(bytes) / sizeof(T));
  f.seekg(0); f.read(reinterpret_cast<char*>(data.data()), bytes);
  if (!f) throw std::runtime_error("read: " + path);
  return data;
}
int main(int argc, char **argv) {
 try {
  if (argc != 3) throw std::runtime_error("usage: native-smoke DIR cpu|gpu");
  std::string root = std::string(argv[1]) + "/";
  auto x = read<float>(root+"smoke.f32"), queries = read<float>(root+"smoke.queries");
  auto ids = read<int64_t>(root+"smoke.ids"), truth=read<int64_t>(root+"smoke.truth");
  auto made = vsag::Factory::CreateIndex("hnsw", R"({"dtype":"float32","metric_type":"l2","dim":128,"hnsw":{"max_degree":16,"ef_construction":200}})");
  if (!made.has_value()) throw std::runtime_error(made.error().message);
  auto index = made.value();
  auto start = std::chrono::steady_clock::now();
  if (std::string(argv[2]) == "cpu") {
    auto ds=vsag::Dataset::Make()->NumElements(ids.size())->Dim(128)->Ids(ids.data())->Float32Vectors(x.data())->Owner(false);
    auto built=index->Build(ds);
    if (!built.has_value() || !built.value().empty()) throw std::runtime_error("CPU build failed");
  } else {
    auto bytes=read<int8_t>(root+"smoke.hnsw");
    vsag::Binary graph; graph.size=bytes.size(); graph.data.reset(new int8_t[bytes.size()]);
    std::copy(bytes.begin(),bytes.end(),graph.data.get());
    vsag::BinarySet set; set.Set(vsag::HNSW_DATA,graph);
    auto imported=index->Deserialize(set);
    if (!imported.has_value()) throw std::runtime_error(imported.error().message);
  }
  double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
  if (index->GetNumElements()!=static_cast<int64_t>(ids.size())) throw std::runtime_error("count mismatch");
  for (size_t i=0;i<ids.size();++i) {
    auto d=index->CalcDistanceById(x.data()+128*i,ids[i]);
    if (!d.has_value() || d.value()!=0) throw std::runtime_error("VID/vector mismatch");
  }
  int hits=0;
  for(int q=0;q<100;++q) {
    auto ds=vsag::Dataset::Make()->NumElements(1)->Dim(128)->Float32Vectors(queries.data()+q*128)->Owner(false);
    auto result=index->KnnSearch(ds,10,R"({"hnsw":{"ef_search":512}})");
    if(!result.has_value() || result.value()->GetDim()!=10) throw std::runtime_error("query failed");
    std::set<int64_t> gt(truth.begin()+q*10,truth.begin()+(q+1)*10),got;
    for(int k=0;k<10;++k) {auto id=result.value()->GetIds()[k]; if(!got.insert(id).second) throw std::runtime_error("duplicate result"); hits+=gt.count(id);}
  }
  std::ofstream persisted(root+std::string(argv[2])+".native",std::ios::binary);
  auto saved=index->Serialize(persisted);
  if(!saved.has_value()) throw std::runtime_error(saved.error().message);
  std::cout<<"mode="<<argv[2]<<" build_or_import_seconds="<<seconds<<" rows="<<ids.size()<<" recall="<<hits<<"/1000\n";
  return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
