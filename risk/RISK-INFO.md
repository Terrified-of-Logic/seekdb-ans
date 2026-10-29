# AMD 题：从其他参赛队公开仓库里挖到的「风险信息」

> 2026-10-29 下午抓取/整理。对象：别人公开的 AMD 赛题仓库/分支（我们自己的提交不在本文件讨论范围）。
> 素材全部来自 GitHub 公开仓库，已抓原文放在本目录 `risk/` 下。
> 一句话：**两份外部暴露物最有价值的信息是"评测口径 + 门槛检测方式 + 编译环境坑"**；
> 其中一条明确指向我们当前提交的最大风险——**降 Recall 换构建速度的路线，强队已经主动放弃，改成 lossless 优化**。

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
