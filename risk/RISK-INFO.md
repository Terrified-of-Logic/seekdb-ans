# AMD 题：从其他参赛队公开仓库里挖到的「风险信息」

> 2026-10-29 下午抓取/整理。对象：别人公开的 AMD 赛题仓库/分支（我们自己的提交不在本文件讨论范围）。
> 素材全部来自 GitHub 公开仓库，已抓原文放在本目录 `risk/` 下。
> 一句话：**两份外部暴露物最有价值的信息是"评测口径 + 门槛检测方式 + 编译环境坑"**；
> 其中一条明确指向我们当前提交的最大风险——**降 Recall 换构建速度的路线，强队已经主动放弃，改成 lossless 优化**。
>
> ⚠️ **重要更正（2026-10-29）**：官方语雀 AMD 题面已于 **2026-10-28 23:44 CST** 更新，**唯一有效基线改为 V5**
> （并给出两数据集、明确 Recall 阈值、3.0x 构建满分线、nq=4096 查询口径、固定编译命令、failMsg 表）。
> 本文第 2 节（依据外部 harness 和旧版文档）里"官方基线可能是 v1 / 2.0x 满分线 / batch 查询"等说法**已过时**，
> 以文末 **第 7 节「官方题面 V5 更正」** 为准。

---

## 1. 这次新挖到的暴露物（10-29 更新）

| 来源 | 分支/内容 | 为什么重要 |
|---|---|---|
| **`dyingtolife/seekdb`**（10-29 还在更新） | `amd-cagra-bench-tools`：**附带完整评测 harness**（`tools/contest/` 6 个脚本）+ 优化代码 | 它自称"尽量复现官方测量合约"，把官方门槛、计分公式、计时口径、GPU 证据检测方式都写进了代码 |
| | `amd-cagra-redo-v1`：**重做的 lossless 版**（复用 device scratch + 预建图） | 说明他们在第一版之后换了路线（从调参转向不降质量的优化） |
| | `amd-cagra-submit`：被重写的新提交版（+428/−56 bridge、+184/−22 adaptor、build.sh +5） | 他们最终提交的东西也以"参数调优 + 显存复用"为主 |
| **`wiqdguewbfu/seekdb`**（仓库一度 404，10-29 恢复） | `amd-contest-entry`：v1 冻结基线 + `OPTIMIZATION_NOTES.md` + H2D 直传/显存池/参数缓存/复用 big-stack worker | 明确写了**官方 hipVS 版本线索 + 编译兼容坑**；并声明"默认优化全部 lossless，不降 Recall" |
| `JIANG2331/seekdb` | `amd-cagra-submit`：只有一个 `AMD_CONTEST_SUBMIT.md` 占位（写明团队 repo `Respawn-ID/seekdb-2026`、基线 v5） | 没代码，不会得分；说明有队在 v5 上做 |
| `zihaomu/seekdb` | 仍是最新 v5（无 v6）；`develop` 停更 | 官方基线没再更新 |

---

## 2. 从 dyingtolife 的 harness 里读到的「官方评测口径」（风险核心）

`tools/contest/run_bench.py` / `report.py` / `build_all.sh` / `setup_cloud_run.sh` 声称复现官方测量合约，可信度较高的信息：

### 2.1 硬门槛（他们建模了 6 条，任一失败=0）

| 门槛 | 他们的判定 | 备注 |
|---|---|---|
| `ids_valid` | 候选返回的 id 全在 `[0,n)` | 结果正确性 |
| `recall@10` | 候选 ≥ 基线 − 0.005 | 注意基线取哪版很关键（见 2.3） |
| `recall@100` | 候选 ≥ 基线 − 0.005 | 同 |
| **`gpu_build_in_window`** | **`CREATE INDEX` 期间（首条查询之前）bridge 日志里出现 `[seekdb_cuvs] build n=… ms=…`** | 基线是惰性建图→DDL 期间 0 次；eager 候选应 ≥1 次。**这条是"GPU 真实参与建图"的门槛** |
| `query_throughput` | 候选/基线 **单条查询 QPS ≥ 0.90** | 他们用单条 QPS 建模；题面写"综合查询吞吐"，是否含批量未知 |
| `cpu_vsag_path` | 用候选二进制跑 `lib=vsag` 也必须正常返回 | CPU 路径不能被破坏 |

### 2.2 计分公式（他们按公开口径实现）

```
total = 4（硬门槛全过）
      + 14 × clamp01((build_speedup − 1) / (2.0 − 1))     # 构建 2.0x 封顶
      + 2  × clamp01((query_qps_ratio − 1) / (1.5 − 1))   # 查询 1.5x 封顶
```

### 2.3 计时/对照口径（直接影响"加速比"）

- 计时窗口 = `CREATE VECTOR INDEX` + **首条成功 ANN 查询**；表已导入、索引不存在、**每轮 drop caches**；每版本 **3 轮取中位数**。
- **基线 = 冻结 v1**：`build_all.sh` 注释与 `setup_cloud_run.sh` 都写 `BASE_REF=amd-cagra-contest-baseline-v1`；候选与基线**同机、同 flags、从源码编译**，bridge 也从各自源码重建。
- 索引参数默认 `m=16,ef_construction=200,ef_search=200`。
- 数据集假设：smoke **200K×128**，full **1M×128**；`gen_dataset.py` 默认 **clustered 分布**（"更接近真实 embedding、对图索引更难"）。
- 环境：官方 Contest image（AMD Radeon Cloud `seekdb-hipvs` workspace）；`/opt/hipvs`、`/opt/rocm`、gfx1100。

### 2.4 对我们的风险映射（重点）

1. **对照基线口径可能不同**：我们队友的 10x 是对 **v5** 基线测的；官方很可能拿 **v1** 冻结基线做性能对照（外部两队都这么建 harness）。v1 和 v5 的 CAGRA 建图行为不同（v3 才改为 NN-Descent 高质量图、v5 才在离线 DDL 上建并持久化 GPU 图）→ **官方真实加速比可能低于自测值**。这不是 0 分风险，但直接决定 14 分里能拿多少；应用 v1 基线重跑 A/B 再估分。
2. **`gpu_build_in_window` 门槛**：我们提交基于 v5，离线 DDL 会建 GPU 图，bridge 也会打印 `[seekdb_cuvs] build n=…`（我们 bridge 里是无条件 stderr 打印，前缀匹配）→ 这条**大概率能过**；但要确认评测的编译选项/日志检测方式（例如是否用 `OB_BUILD_CUVS_TRACE`/`OBCUVS02` 等标记）。
3. **query_throughput 口径不确定**：他们用"单条 QPS ≥90%"，题面写"批量查询用于计算 2 分"+"综合查询吞吐不得低于 90%"。我们只测了单条 SQL，**没有 batch_knn 数据**；如果官方按批量/综合吞吐判 gate，存在掉门槛=0 的风险，同时 2 分查询分也没底。
4. **数据集规模/分布**：队友调参假设"Hidden250K / SIFT 分布"；外部 harness 按 200K smoke + **1M full + clustered 合成分布**测，说明 hidden 数据的规模与分布仍是猜的。250K SIFT 上的 Recall 余量不能直接外推到 1M/聚类数据。

---

## 3. 从 wiqdguewbfu 的 OPTIMIZATION_NOTES 里读到的「编译/环境坑」（可致 0）

原文关键信息：
- 基线：**冻结 v1**（`08c76418`）。
- 官方 hipVS 版本线索：**`hipVS f96a663d (release/rocmds-26.03)`**。
- 兼容性坑（他们专门修了）：
  - **dlpack v1.x 的匿名枚举** → 参数要传**整数**（不能传枚举值）；
  - **`cuvsResources_t` 是整数 handle**（不是指针）；
  - 用 `hipcc -fsyntax-only -std=c++17 -Wall -Wextra -D__HIP_PLATFORM_AMD__` 验证零告警。
- 他们的优化（全部**默认 lossless**，结果/图质量不变，只在 env 开关里留降质量参数给云端标定）：
  - build 使用 `kDLCUDA` 显式异步 H2D（`SEEKDB_CUVS_H2D_SYNC=1` 可强制同步）；
  - grow-only device buffer pool（**首条查询——官方计时包含它——零设备分配**）；
  - `cuvsCagraSearchParams_t` 按 handle 缓存；
  - 复用 32MB 大栈 GPU worker 线程（省 ~55.5µs/次 dispatch，失败自动回退基线 per-job 线程）。

**对我们的风险**：
1. 我们提交的 bridge 新增了 `enum cuvsCagraSearchAlgo` / `AUTO` / `MULTI_CTA` 等写法 → 如果评测机 hipVS 头（f96a663d/rocmds-26.03）与队友本地版本不一致，**bridge 编译失败 → 硬门槛"可编译"失败=0**。这是当前最具体的"编译 0 分"风险，建议在官方同版本镜像上做一次 `-fsyntax-only` 验证。
2. 他们把"首条查询的设备分配"也算进风险，说明计时窗口对首查路径敏感；我们的改动没做 device buffer pool（仍沿用基线 per-search alloc），首查会多几微秒~毫秒级，相对秒级构建可忽略，但说明"计时口径"的细节值得注意。

---

## 4. 竞争队的策略信号（对我们当前提交最关键的一条）

| 队伍 | 默认是否降 CAGRA 图质量 | 打法 |
|---|---|---|
| `wiqdguewbfu` | **不降**（"defaults preserve frozen-baseline graph quality, Recall unchanged"） | H2D 直传、显存池、参数缓存、复用 worker（lossless） |
| `dyingtolife` 第一版 | 降（tune CAGRA build params + eager build） | 参数调优路线 |
| `dyingtolife` 10-29 重做版 `amd-cagra-redo-v1` | **新分支改成 lossless**（reuse device scratch + prebuild CAGRA，未提降参数） | 疑似发现参数路线风险后换打法 |
| **我们当前提交 `e7221f8b`** | **降**（dim≤128: degree 32 / inter 64 / niter 10，对比基线 128/256/100） | 用 Recall 余量换 10x 构建加速 |

→ 外部信息给出的最强信号：**强队在往"lossless 优化"收敛，把降质量参数放到 env opt-in（评测机不设置）**。
我们这条 pending 的 0 分风险主要就落在 **Recall@10/@100 门槛**（hidden 数据未知）和 **查询吞吐口径**上。

---

## 5. 建议的复核清单（基于外部信息，按优先级）

1. **用 v1 冻结基线重跑 A/B**：用 `risk/dyingtolife/contest-tools/` 里的 harness（`build_all.sh` 按 v1 建基线、候选按当前 pending commit 建），得到接近官方的 gates 与估算分；尤其跑 `--drop-caches`、200K 与 1M 两档。
2. **补测 batch_knn QPS**：题面 2 分来自批量查询、90% 门槛可能按综合吞吐；确认 width=32/AUTO 下批量没有掉过 90%。
3. **编译兼容验证**：在官方 hipVS（f96a663d/rocmds-26.03）镜像上 `hipcc -fsyntax-only -std=c++17 -Wall -Wextra -D__HIP_PLATFORM_AMD__` 编一次我们的 `seekdb_cuvs_bridge.c`（新增的 `enum cuvsCagraSearchAlgo`/`AUTO` 是重点）。
4. **Recall 余量评估**：在 clustered 合成数据（`gen_dataset.py` 默认分布）和 1M 规模上，用我们默认参数测 Recall@10/@100 vs 基线，看是否仍 ≥ 基线−0.005；
5. **如果上面任一条有风险**：准备回退版（默认参数恢复基线图质量，只保留 lossless 优化：H2D、显存池、参数缓存、预建图/worker 复用），这正好是外部两队验证过的方向。

---

## 6. 文件索引（全部来自公开仓库，已落盘）

```
risk/
├── RISK-INFO.md（本文件）
├── dyingtolife/
│   ├── contest-tools/            ← 官方口径复现 harness（重点看 run_bench.py、report.py、build_all.sh）
│   │   ├── run_bench.py          ← 计时窗口/首查/3 轮/recall/GPU build 证据检测
│   │   ├── report.py             ← 门槛清单 + 计分公式（4+14+2）
│   │   ├── build_all.sh          ← 基线=v1 冻结、候选同 flags、bridge 从源码重建
│   │   ├── setup_cloud_run.sh    ← 官方 Contest image/环境/1M full/数据集
│   │   ├── gen_dataset.py        ← clustered 分布、200K/1M、float32/L2、gt10/100
│   │   └── run_all.sh
│   ├── bridge.c                  ← 他们 10-29 重做版 bridge（device scratch 复用等）
│   └── ob_vsag_adaptor.cpp       ← 对应 adaptor
└── wiqdguewbfu/
    ├── OPTIMIZATION_NOTES.md     ← 编译/env 坑 + lossless 优化说明（重点）
    ├── seekdb_cuvs_bridge.c      ← H2D 直传/显存池/参数缓存实现
    └── ob_vsag_adaptor.cpp       ← 复用 big-stack worker 实现
```

> 注意：这些都是**其他队的实现与本地测量口径**，不是官方评分器；官方正式成绩仍以组织方云端评测为准。
> 参考思路可以，但 AMD 题面要求"实验结果必须来自自己的实现和运行记录"，不要直接照抄代码进提交。

---

## 7. 官方题面 V5 更正（2026-10-29 抓取，content_updated_at=2026-10-28T15:44:11Z = 23:44 CST）

> 原文已存 `official-AMD-doc-20261029.md`。本节是官方最新口径，**与本文前面基于旧版文档/外部 harness 的推断冲突时，以本节为准**。

### 7.1 基线唯一有效版本 = V5

- 分支 `amd-oceanbase-official-contest-base-v5`，Tag `amd-cagra-contest-baseline-v5`，冻结 commit `634888d9b037153fa143d2c913ba4b25503e1d62`。
- "参赛代码必须基于该 commit 开发"；**不是 V5 后代的提交会在评测开始前被拒绝（failMsg = 8）**；基于 V1–V4 的代码必须先合入/变基到 V5。
- V5 修复的关键点：在"先导入数据、再 `CREATE VECTOR INDEX ... lib=cuvs`"的**离线 DDL 路径上真正在 GPU 上构建 CAGRA**（V1–V4 实际构建的是 CPU VSAG HNSW、GPU 空闲）；构建后把 CAGRA 图 + ID + 原始向量持久化为 GPU 快照，查询时重载；基线本身在正式数据集上满足所有 Recall 门槛。
- 官方参考成绩（V5 基线在正式评测环境 AMD Radeon PRO W7900D / ROCm 7.2.4 / 24 核 CPU 槽上 10 次）：

| 数据集 | 构建时间（含首查） | Recall@10/@100 | nq=4096 QPS（Top-10 / Top-100） |
|---|---|---|---|
| SIFT250K | 20–27 s（中位约 23 s） | 约 1.000 / 1.000 | 约 147 / 143 |
| Hidden250K | 29–35 s（中位约 30 s） | 约 0.985 / 0.914 | 约 87 / 85 |

### 7.2 数据集、计时与查询口径

- 正式计分用两个 **250K** 数据集：**SIFT250K = 250K×128（权重 40%）**、**Hidden250K = 250K×256（权重 60%）**；FP32 + L2；Hidden250K 内容/顺序/ID 不公开。
- 计时起点 SQL 固定：`CREATE VECTOR INDEX idx_items ON items(v) WITH (distance=l2, type=hnsw, lib=cuvs, m=16, ef_construction=200, ef_search=64)`；**终点包含第一条固定查询**（防止把惰性建图移出计时）。
- 每个数据集 baseline/candidate **各跑 1 次、共 2 轮**，两个数据集交换先后顺序（A: baseline→candidate；B: candidate→baseline）；每轮清索引和缓存；异常配对作废重跑。
- **查询是 nq=4096 条单行近似查询 SQL（Top-10 / Top-100 两组），不是 `dbms_vector.batch_knn` 批量接口**；nq=1 只做诊断。→ 本文第 2.4 节"批量查询口径风险"作废；队友的"4096 single-row SQL 1.58x QPS"正是官方口径。
- 硬门槛（任一失败 = 0）：
  1. 可编译、通过 GPU 路径抽检、在资源/时间限制内完成；
  2. 结果正确：每条查询必须返回 k 个**互不重复**的 ID（result_integrity）；
  3. 检索质量：**SIFT250K Recall@10/100 ≥ 0.95**；**Hidden250K Recall@10 ≥ 0.90、Recall@100 ≥ 0.85**；且各项比同轮冻结基线下降 **≤ 0.005**；
  4. 建图和查询都必须真实由 GPU hipVS/cuVS CAGRA 完成（外部 profiler 抽检，gpu_path）；
  5. `lib=vsag` CPU 路径仍可用；查询吞吐 ≥ 同轮基线 90%（nq=4096 Top-10/Top-100 几何平均）；
  6. 资源：显存峰值 ≤ 基线 1.25 倍；CPU 24 核 / 内存 96 GB / 显存 45 GB / GPU 阶段 15 分钟。
- 计分（**不是本文第 2.2 节的 2.0x 线性口径**）：

```
综合构建加速比 = SIFT250K^0.4 × Hidden250K^0.6   （加权几何平均）
构建性能分 = 14 × clamp( ln(综合构建加速比) / ln(3.0), 0, 1 )    ← 3.0x 满分
查询性能分 =  2 × clamp( ln(综合查询加速比) / ln(1.5), 0, 1 )    ← 1.5x 满分
总分 = 4（硬门槛全过）+ 构建性能分 + 查询性能分；任一硬门槛挂 = 0
```

对数增长：1.2x≈2.3 分、1.5x≈5.2 分、2.0x≈8.8 分、3.0x 及以上 = 14 分；直接提交未修改的基线只拿 4 分。

### 7.3 编译/提测要求（新增，直接决定能不能开始评测）

- 固定编译环境：ROCm 7.2.4、GCC 13.3、CMake 3.31、Rust 1.97；hipVS 固定 commit **`f96a663d0a4441d6f5344bd3ef552b71ae7b370b`**（只读 `/opt/hipvs`）；**编译环境不联网**，3rd 依赖 / Rust vendor 均为冻结版本，不能新增需联网下载的依赖。
- 固定编译命令（不能追加/修改编译选项；但**可以改仓库内 CMake 文件和 `tools/hipvs_bridge/build.sh`**，只要最终能用该命令编出 seekdb + bridge）：

```bash
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh <outdir>/libseekdb_cuvs_bridge.so

cmake -S . -B build_release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=<outdir>/libseekdb_cuvs_bridge.so
cmake --build build_release --target seekdb
```

- **答题仓库地址必须是 `https://<用户名>:<只读令牌>@github.com/<所有者>/<仓库>`**，否则直接 failMsg=4；答题仓库保持私有；令牌 Fine-grained、只给 `Contents: Read-only`、有效期覆盖赛程。
- failMsg 对照：1 分支不存在 / 2 commit 不存在 / 3 编译失败或未通过 GPU 路径抽检 / 4 clone 失败（地址格式/令牌） / 5 clone 超时 / 6 检出失败 / 7 CMake 失败 / 8 验证失败（如非 V5 后代）。

### 7.4 对本文前面风险结论的修正（以官方 V5 口径重排）

| 之前判断 | 更正后 |
|---|---|
| "官方基线可能是 v1，要按 v1 重跑 A/B" | **作废**。官方唯一基线 = V5；外部 dying 的 harness 用 v1 是旧口径。A/B 应按 **V5 基线**（参考构建 20–27 s / 29–35 s） |
| "批量查询接口 batch_knn 是风险" | **作废**。官方查询 = nq=4096 条单行 SQL；队友的"4096 single-row SQL 1.58x QPS"正是官方口径 |
| "构建 2.0x 满分" | **作废**。官方 **3.0x** 满分、对数计分（2.0x 只有 8.8 分） |
| "提交 e7221f8b 的结构/基线风险" | **已消除**：它基于 V5 冻结 commit（其后代），当前 pending 记录已带 tokenized URL |
| 首次失败 failMsg=4 | **真实原因**：当时答题仓库地址是普通 HTTPS 链接，不是带只读令牌的格式 → clone 失败（不是基线/ancestry 问题） |
| 队友 dim 分档（≤128 / >128） | **正好对上官方两数据集**：SIFT250K=128 维（40%）、Hidden250K=256 维（60%） |
| 最大 0 分风险 | 仍是 **Hidden250K（256 维、60% 权重）的 Recall 门槛**：基线 R@10/100 ≈0.985/0.914，候选需 ≥0.90/0.85 且比基线降 ≤0.005；队友只有 256d 代理数据（nytimes）验证，官方 Hidden250K 不公开 |
| "改 bridge 有编译风险" | **表述需修正**：改 bridge 本身是官方允许且预期的路径（见 7.5）；真正要确认的只是"新增的 cuVS C API 写法与固定 hipVS 版本 f96a663d 兼容"。我们提交的 e7221f8b 里唯一的新 API 是 `enum cuvsCagraSearchAlgo` + `sp.algo`，其余（`cuvsCagraSearchParamsCreate` / `itopk_size` / `search_width` / `build_algo`）都是基线 v5 已用 API；且队友在官方同款 AMD 实例上编译并实测过 t33/t43 → 风险低。另需注意 GPU profiler 抽检（建图和查询都必须在 GPU 完成） |

### 7.5 补充说明：为什么"改 bridge"不是风险

官方题面 3.1 原文：

> **可以修改**：答题仓库（基于 V5 的 seekdb 仓库）中的全部源码，包括 seekdb 内核、CMake 文件，以及 `tools/hipvs_bridge/` 下的 hipVS bridge。
> **不能修改**：hipVS/cuVS 库本身。评测时 hipVS 是组织方固定的版本（commit `f96a663d...`），以只读方式提供，选手无法替换；对 hipVS/cuVS 源码的修改不会进入评测。如果要改进 CAGRA 的建图或检索实现，相关代码必须放在答题仓库中，并在下文的统一编译流程里编译生成。

3.2 原文：

> 上述命令和参数固定，不能追加或修改编译选项；可以修改仓库内的 CMake 文件和 `tools/hipvs_bridge/build.sh`，但最终必须能用上述命令编译成功，并产出 `build_release/src/observer/seekdb` 和 bridge 动态库。

所以：

- **改 bridge 是这道题的标准/预期做法**（CAGRA 实现只能通过仓库内的 bridge 改，不能改 hipVS 库），本身零风险；
- 需要遵守的约束只有：不提交预编译 `.so`（要由评测按固定命令从源码编译）、不新增需要联网的依赖、固定命令必须能编过；
- 我们提交的 `e7221f8b` 只改了 `seekdb_cuvs_bridge.c`（没有提交 .so、没改 build.sh/CMake、没加依赖）；
- 唯一"新增 API"是 `enum cuvsCagraSearchAlgo` + `sp.algo = AUTO`（基线 v5 bridge 没用过；其余 `cuvsCagraSearchParamsCreate` / `itopk_size` / `search_width` / `build_algo` 都是基线已用 API）。队友在 AMD 实例上编译并用 t33/t43 实测过这条路径，实例就是 Radeon Cloud 的官方 contest image（/opt/hipvs、ROCm 7.2.4）→ 兼容性风险低；花 5 分钟用官方固定命令在评测镜像上重编一次即可完全排除。

### 7.6 修正后的建议

1. 其它先不动（当前 pending 那条就是基于 V5 + token URL，结构上已符合官方要求），等判分；
2. 判回后按官方参考值核算：构建综合加速 = SIFT^0.4 × Hidden^0.6，目标 3.0x 拿满 14；Hidden250K（256d、60%）是关键短板；
3. 若判 0/低分：优先核查 Hidden250K Recall 门槛与 GPU 抽检；必要时把 256d 的 niter/degree 回退到基线附近、把加速让给 lossless 优化；
4. 编译兼容：在官方同版本 hipVS（f96a663d / ROCm 7.2.4）按固定命令完整编译一次，确认 bridge 无错误；
5. 注意"禁止行为"：不得通过降低 Recall 换性能（Recall 门槛已明确）、不得对评测不同阶段用不同实现路径、不得复用 CAGRA 图/缓存。
