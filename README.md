# AMD 题外部公开实现参考（2026-10-28 抓取）

> 用途：给队友参考外部**公开**的 AMD 赛题（600072，seekdb × AMD hipVS / CAGRA 构建加速）实现。
> 所有来源均为 GitHub 公开仓库，附抓取时 commit / URL / 日期，见 `MANIFEST.md`。
>
> ⚠️ 只作技术参考：AMD 题面要求“实验结果必须来自自己的实现和运行记录”，评测/赛后核查按此执行；
> 建议学习思路后自己重写并自己跑 A/B 与 Recall 证据，不要直接整段复制进提交分支。

---

## 0. 一分钟结论

1. **官方基线不止 v1**：`zihaomu/seekdb` 有 `amd-oceanbase-official-contest-base` 的 v1–v5 五个分支/标签。
   题面冻结的是 v1（`08c76418`），但 v2–v5 是官方后续修复，且**都是 v1 的后代**——
   基于 v5 开发既满足“冻结基线后代”校验，又带上 Top-64/画质/离线 DDL 持久化等修复。
2. **正确的接入方式是基线自带的 GPU 路径**：`-DOB_BUILD_CUVS=ON` + `tools/hipvs_bridge/build.sh` 产出的
   `libseekdb_cuvs_bridge.so` + SQL `WITH (lib=cuvs)`；官方评测机按自己固定的 build 命令编译并跑自己的 harness。
3. **已抓到 2 份公开实现**（另一份 `weiqdguewbfu/seekdb@contest-optim` 在抓取时仓库已不再公开）：
   - `dyingtolife/seekdb` @ `amd-cagra-submit`：**基线 v5** + 2 项优化 + 完整技术文档（目前最完整）；
   - `pei-pei45/seekdb` @ `2026-competition-ecosystem`：**冻结 v1** + 2 项优化（更激进的 defer 方案）。
4. 唯一得分的队 `idmg123`（20.000，10-28 23:06）未找到公开仓库。

---

## 1. 官方基线 v1–v5（必读）

| 版本 | commit | 日期(CST) | 修复内容 |
|---|---|---|---|
| v1 | `08c76418a8e1fef3c1aae83010658ff56a4faa57` | 10-26 18:25 | 题面冻结 commit（tag `amd-cagra-contest-baseline-v1`） |
| v2 | `58ddbd7459` | 10-27 17:40 | 修 CAGRA search `itopk_size` 默认 64 导致 top-k>64 被截断 |
| v3 | `3d88d5bb59` | 10-27 19:52 | 用 NN-Descent 建高质量 CAGRA 图 |
| v4 | `ad3253a52f` | 10-27 20:42 | 256 维数据的 CAGRA 图质量 |
| v5 | `634888d9b0` | 10-28 10:21 | 离线 DDL 上真正构建并持久化 GPU CAGRA + 持久化冒烟测试（tag `amd-cagra-contest-baseline-v5`） |

v1→v5：8 文件 +578/−24，`src/oblib/lib/vector/ob_vsag_adaptor.cpp` 占 +367，
其余为 `tools/hipvs_bridge/seekdb_cuvs_bridge.c`(+56)、`ob_plugin_vector_index_adaptor.cpp`、
验证脚本 `run_gpu_smoke.sh`/`run_ddl_persistence_smoke.sh`、`test_persistence.cpp`、README。

差异全文：`exposed/baseline-v5/patches/v1-to-v5.diff`；v5 关键文件在 `exposed/baseline-v5/files/`。

官方基线自身用法（`docs/gpu-vector-index-hipvs-cuvs/README.md`）：

```bash
# 1) 先构建 bridge
tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so

# 2) 带 cuVS 构建 seekdb
./build.sh release -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so --make -j64

# 3) SQL 里按索引开启 GPU
CREATE TABLE items(
  id BIGINT PRIMARY KEY,
  embedding VECTOR(128),
  VECTOR INDEX idx_embedding(embedding)
    WITH (distance=l2, type=hnsw, lib=cuvs)
);
```

---

## 2. `dyingtolife/seekdb` @ `amd-cagra-submit`（bf0074456f，10-28 20:28 CST）

- 来源：<https://github.com/dyingtolife/seekdb/tree/amd-cagra-submit>
- 基线：`amd-cagra-contest-baseline-v5`（`634888d9b0`）
- 3 个提交、4 文件、+433/−21：
  - `perf(cuvs): reuse per-index device scratch in the hipVS bridge`
  - `perf(cuvs): overlap the CAGRA build with the CPU index load`
  - `docs(cuvs): document the contest optimisation and its runtime switches`

### 2.1 优化 1：bridge 的 per-index device scratch 池化（主要提升查询/减少固定开销）

- 基线每次 `knn_search` 都 `cuvsRMMAlloc` + `cuvsRMMFree` 3 个 device buffer（query / ids / distances）；
- 改成每个 index 保留 8 个 scratch slot 复用（per-index mutex；槽满时用私有 scratch 回退，不串行、不落 CPU）；
- 附带 `[seekdb_cuvs] build n=… ms=…` / `search nq=… topk=… ms=…` 日志（`OB_CUVS_LOG=0` 关闭），
  供 harness 归因 GPU 阶段并证明 GPU 图在计时窗口内建成；
- 构建参数开关（默认值与 v5 完全一致）：
  `OB_CUVS_NN_ITER`（默认 100）、`OB_CUVS_GRAPH_DEGREE`（128）、`OB_CUVS_INTERMEDIATE_DEGREE`（256）。

### 2.2 优化 2：CAGRA 构建与 CPU load 重叠（主要提升构建时间）

- 基线：GPU CAGRA 图在 DDL 最后 `ob_cuvs_serialize` 才建，CPU 建 VSAG 图的整段时间 GPU 全闲；
- 改为 `ob_cuvs_add` 中：
  - `buf_vecs_` / `buf_ids_` **几何扩容**（避免 `vector::insert` 反复重分配拷贝）；
  - 起 watcher 线程：缓冲 300 ms 不再增长（或达到几何增长点）就开始后台建 CAGRA，
    `ob_cuvs_serialize` 时图已就绪直接复用；
  - 安全措施：建图全程持 `g_ob_cuvs_mu`、每步重新查表（index drop 自动停止）、
    serving 仍要求 `built_n_ == buf_ids_.size()`（不会用旧图出结果）；
- 开关：`OB_CUVS_EAGER`（默认 1，0 关闭）、`OB_CUVS_SETTLE_MS`（300）、`OB_CUVS_FINAL_MS`（500）。

### 2.3 文档与复现

`exposed/dyingtolife/files/contest-optimization.md`（中文/英文混合的技术说明，含以下复现命令）：

```bash
tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so
./build.sh release -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=ON \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so --make -j64
# 测量：CREATE VECTOR INDEX -> 首条 ANN 查询返回；3 轮清缓存；同时核 Recall/QPS
```

> 注：文档提到“harness shipped with the submission”，但公开分支里只有上述 4 个文件，
> 测试脚本与成绩数据未公开。

文件：`exposed/dyingtolife/files/`；完整 diff：`exposed/dyingtolife/patches/v5-to-submit.diff`；
拆开的两个可选 diff：`v5-to-bridge-only.diff`、`v5-to-docs-only.diff`。

---

## 3. `pei-pei45/seekdb` @ `2026-competition-ecosystem`（9d0b421fb6，10-27 21:00 CST）

- 来源：<https://github.com/pei-pei45/seekdb/tree/2026-competition-ecosystem>
- 基线：**冻结 v1**（`08c76418a8`）
- 2 个提交、2 文件、+168/−14，无文档：
  - `fix(bridge): size CAGRA itopk by the requested top-k`（=官方 v2 修复，topk>64 场景）；
  - `feat(vector): defer the VSAG CPU graph while cuVS CAGRA is active`

### 3.1 思路：defer VSAG CPU 图（更激进，收益最大、风险也最高）

- 基线对 `lib=cuvs` 索引是 **CPU VSAG 图 + GPU CAGRA 图两套都建**；
- 该实现插入时**跳过 CPU VSAG 图**，只缓冲给 GPU CAGRA；查询优先走 cuVS；
- 只有需要 CPU 回退（filter / 删除 / GPU 图过期）时，才 `ob_cuvs_materialize()` 用缓冲一次性整体建 CPU 图，
  之后停止 defer；
- 顺带修了重复 vid bug：delta + snapshot 两次 `add_index` 会把同一行缓冲两遍（CAGRA 返回重复邻居），
  用 `id2slot_`（vid→row）去重/原地更新；
- 开关 `OB_CUVS_DEFER_VSAG=1` 才启用（默认关，基线行为不变）。

文件：`exposed/pei-pei45/files/`；完整 diff：`exposed/pei-pei45/patches/v1-to-branch.diff`。

---

## 4. 可借鉴点（按对官方 14 分构建性能的收益排序）

1. **defer/跳过 CPU VSAG 图**（pei 方案）：省掉基线里最大的一块 CPU 构建时间；但 filter/回退正确性要自己补
   （他们用 materialize + id 去重；官方硬门槛要求 CPU 路径仍可用、不能静默回退）。
2. **overlap CAGRA 构建与 CPU load**（dying 方案）：把 GPU 建图藏进 CPU 构建期，风险较低，收益中等。
3. **buf_vecs_/buf_ids_ 几何 reserve**：低风险、小收益，可单变量验证。
4. **bridge scratch 池化 + itopk 修复**：主要影响查询（2 分）与 Recall@100 正确性，建议必做 itopk 修复。
5. **NN-Descent/degree 参数调优**：用 Recall 换构建时间，注意硬门槛（Recall@10/@100 不得比同轮基线低 >0.005，
   且有未公布绝对下限）。
6. 记得用 **v5 基线**（不是 v1）打底，并把 v1 冻结 commit 作为祖先推进提交仓库，满足“冻结基线后代”校验。

---

## 5. 文件索引

```
README.md                       本文件
MANIFEST.md                     来源清单（repo/branch/commit/URL/抓取时间/许可）
exposed/baseline-v5/            官方 v1->v5 的 diff + v5 关键文件
  patches/v1-to-v5.diff
  files/ob_vsag_adaptor.cpp
  files/seekdb_cuvs_bridge.c
  files/seekdb_cuvs_bridge.h
  files/ob_plugin_vector_index_adaptor.cpp
  files/build.sh
  files/gpu-vector-docs-README.md
exposed/dyingtolife/           完整提交版参考
  patches/v5-to-submit.diff
  patches/v5-to-bridge-only.diff
  patches/v5-to-docs-only.diff
  files/ob_vsag_adaptor.cpp
  files/seekdb_cuvs_bridge.c
  files/build.sh
  files/contest-optimization.md
  compare-v5-to-submit.json
exposed/pei-pei45/             实验版参考
  patches/v1-to-branch.diff
  files/ob_vsag_adaptor.cpp
  files/seekdb_cuvs_bridge.c
  compare-v1-to-branch.json
```

### 怎么把 patch 打到临时工作区（只读参考，不影响提交分支）

```bash
git clone https://github.com/zihaomu/seekdb.git /tmp/seekdb-ref
cd /tmp/seekdb-ref

# 试 dyingtolife 的提交版（基于 v5）
git checkout -b try-dying 634888d9b037153fa143d2c913ba4b25503e1d62
git apply /path/to/exposed/dyingtolife/patches/v5-to-submit.diff

# 或试 pei-pei45 的实验版（基于 v1）
git checkout -b try-pei 08c76418a8e1fef3c1aae83010658ff56a4faa57
git apply /path/to/exposed/pei-pei45/patches/v1-to-branch.diff
```

构建（需要 AMD gfx1100 / ROCm 7.2.4 / `/opt/hipvs` 实例）：

```bash
tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so
./build.sh release -DOB_BUILD_CUVS=ON -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so --make -j64
```

---

## 6. 抓取说明 / 未抓到的

- 抓取日期：2026-10-28（深夜）；数据来自 GitHub API + `git fetch`（`--filter=blob:none` 部分克隆），
  文件内容与公开分支一致。
- `weiqdguewbfu/seekdb` 的 `contest-optim`（`c6e3d94724`）：抓取时账号已 404（转私有/删除），未抓到。
- `JIANG2331/seekdb@amd-cagra-submit`、`disguiser501/seekdb`、`magicones/seekdb`：没有 AMD 改动。
- 得分队 `idmg123`（AMD 20.000）：未找到公开仓库/账号。

---

## 7. 更新（2026-10-29）：外部仓库里的「风险信息」挖掘

新抓到两队 10-29 的更新（dyingtolife 的官方口径 benchmark harness、wiqdguewbfu 的编译/环境 notes 与 lossless 实现），
整理成 **\isk/RISK-INFO.md\**：
- 官方口径硬门槛/计分公式/计时窗口（dying 的 harness 复现版：4+14*clamp(build/2.0)+2*clamp(qps/1.5)）
- \gpu_build_in_window\ 检测方式、基线按冻结 v1 构建、1M/clustered 数据假设
- hipVS 版本线索与编译兼容坑（f96a663d / rocmds-26.03、dlpack 匿名枚举、cuvsResources_t 整数句柄）
- 强队从"降参数"转向"lossless 优化"的信号（对我们当前提交的 Recall 门槛风险提示）

详见 \isk/RISK-INFO.md\（原文素材在 \isk/dyingtolife/\、\isk/wiqdguewbfu/\）。
