<!-- 官方 AMD 题面（语雀），抓取时间 2026-10-29；content_updated_at=10/28/2026 15:44:11 -->

| 项目 | 说明 |
| --- | --- |
| 项目 | seekdb × AMD hipVS |
| 任务 | 使用 hipVS 加速 HNSW 索引构建 |
| 性能目标 | 在结果正确、检索质量达标的前提下，构建加速效果达到大赛公布的预期 |
| 提交产物 | 实现代码、可复现的测试方法及 CPU/GPU 构建性能对比报告 |
| 总分 | 20 |


## 一、题目背景
向量索引构建需要完成大量距离计算、近邻选择和图结构处理。随着向量规模和维度增长，索引构建时间会直接影响数据导入效率和检索服务的准备时间。

本题要求参赛者结合 AMD Radeon GPU 与 hipVS/cuVS，在已经完成 GPU 接入的 seekdb 比赛基线上继续优化 CAGRA 向量索引构建链路，并在保证结果正确、检索质量和系统稳定性的前提下取得端到端性能提升。

开始前，请阅读 [AMD Radeon 使用和部署 seekdb](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/lcuknwgl955v8ou7)，完成算力平台、ROCm 和 seekdb 实验环境的准备。

> **资料链接提醒：**该链接可能需要特定账号权限。如果链接失效或无法访问，请联系比赛组织方获取新的部署文档或开通访问权限。
>

## 二、比赛基线
为了帮助同学们更好地完成赛题，减少 AMD GPU 环境适配和 hipVS/cuVS 接入所需的前置工作，比赛组织方已经准备了一份支持 AMD Radeon 设备、并完成 CAGRA 基本构建与查询链路接入的 seekdb 比赛基线。

参赛者不需要从零接入 AMD GPU，也不需要从 seekdb 上游 `master` 开始开发。请统一从以下比赛专用分支开始：

```latex
https://github.com/zihaomu/seekdb/tree/amd-oceanbase-official-contest-base-v5
```

基线仓库：

```latex
https://github.com/zihaomu/seekdb
```

比赛专用分支：

```latex
amd-oceanbase-official-contest-base-v5
```

冻结 Tag：

```latex
amd-cagra-contest-baseline-v5
```

冻结 commit：

```latex
634888d9b037153fa143d2c913ba4b25503e1d62
```

参赛代码必须基于该 commit 开发。建议先 fork 比赛仓库，再创建自己的参赛分支：

```bash
git clone https://github.com/<你的账号或团队>/seekdb.git
cd seekdb

git remote add contest-base https://github.com/zihaomu/seekdb.git
git fetch contest-base amd-oceanbase-official-contest-base-v5 --tags

git switch -c amd-cagra-submit \
  contest-base/amd-oceanbase-official-contest-base-v5
```

评测系统会检查参赛 commit 是否为冻结基线的后代。

> **提醒：报名登记的答题仓库地址只接受 **`https://<用户名>:<令牌>@github.com/<所有者>/<仓库>`** 这一种格式**，其他任何格式都会直接判为评测失败（`failMsg = 4`）。令牌的创建方法见 4.1。
>

### 2.1 V5 基线说明
V5 是当前唯一有效的比赛基线。与 V1～V4 相比，V5 修复了正式评测路径上的关键问题：

+ 在“先导入数据、再执行 `CREATE VECTOR INDEX ... lib=cuvs`”的离线 DDL 路径上，V5 会真正在 GPU 上构建 CAGRA 图；V1～V4 在这条路径上实际构建的是 CPU VSAG HNSW，GPU 基本空闲；
+ 构建完成后，V5 将 CAGRA 图连同 ID 与原始向量一起持久化为 GPU 快照，查询时直接重载该快照并在 GPU 上检索；
+ 基线本身在两个正式数据集上均满足全部 Recall 门槛。

**基于 V1～V4 开发的代码请先合入或变基到 V5 冻结 commit。** 不是 `634888d9b` 后代的提交会在评测开始前被拒绝（`failMsg = 8`）。

V5 冻结基线在正式评测环境（AMD Radeon PRO W7900D，ROCm 7.2.4，24 核 CPU 槽）上 10 次运行的参考结果：

| 数据集 | 构建时间（含第一条查询） | Recall@10 / @100 | nq=4096 查询 QPS（Top-10 / Top-100） |
| --- | ---: | ---: | ---: |
| SIFT250K | 20～27 s（中位数约 23 s） | 约 1.000 / 1.000 | 约 147 / 143 |
| Hidden250K | 29～35 s（中位数约 30 s） | 约 0.985 / 0.914 | 约 87 / 85 |


以上数值仅供参考，本地机器上的绝对时间会有所不同；同一套代码在评测机上不同轮次之间也存在约 5%～15% 的波动。正式成绩始终与同轮运行的冻结基线配对比较，不使用上表的绝对值。

## 三、优化任务
参赛者需要优化 seekdb 中的 AMD GPU CAGRA 索引构建路径：

```latex
评测输入：float32
距离类型：L2
索引配置：type=hnsw, lib=cuvs
```

参赛者可以自行选择优化方案和实现路径。以下方向仅供参考，不构成限制：

+ 优化建图算法、并行方式或批量处理流程；
+ 优化数据布局以及 CPU/GPU 之间的数据传输；
+ 优化显存分配、缓存复用和索引生命周期；
+ 优化索引持久化、重载或建图完成后的批量查询。

参赛者也可以探索其他方案。赛题只要求保持规定的 SQL 使用方式、数据语义和结果正确性，并按照本文的统一测试方法获得可验证的性能提升。

### 3.1 可修改范围
+ **可以修改**：答题仓库（基于 V5 的 seekdb 仓库）中的全部源码，包括 seekdb 内核、CMake 文件，以及 `tools/hipvs_bridge/` 下的 hipVS bridge。
+ **不能修改**：hipVS/cuVS 库本身。评测时 hipVS 是组织方固定的版本（commit `f96a663d0a4441d6f5344bd3ef552b71ae7b370b`），以只读方式提供，选手无法替换；对 hipVS/cuVS 源码的修改不会进入评测。如果要改进 CAGRA 的建图或检索实现，相关代码必须放在答题仓库中，并在下文的统一编译流程里编译生成。
+ 评测输入数据、评测 SQL、索引参数和测试程序由组织方固定，不能修改。

评测数据和对外 SQL 接口固定使用 FP32。参赛者可以在内部使用 FP16、混合精度或其他数值优化，但不能修改评测输入数据或改变对外 FP32 语义，并且必须通过相同的 ID 正确性和 Recall 门槛。

原有 `lib=vsag` CPU 路径必须继续正常工作：`lib=vsag` 索引的 SQL 建表、建索引和查询仍然可用即可，不约束 `lib=cuvs` 的内部实现，也不要求 `lib=cuvs` 索引额外构建 CPU VSAG 索引。

### 3.2 统一编译方式
评测编译环境：ROCm 7.2.4、GCC 13.3、CMake 3.31、Rust 1.97，hipVS 位于 `/opt/hipvs`，ROCm 位于 `/opt/rocm`。编译环境**不联网**。

评测系统在答题仓库根目录依次执行以下步骤，选手可以在本地按同样方式复现：

```bash
# 1. 编译 hipVS bridge（脚本位于答题仓库）
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh <输出目录>/libseekdb_cuvs_bridge.so

# 2. 编译 seekdb
cmake -S . -B build_release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON \
  -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=<输出目录>/libseekdb_cuvs_bridge.so
cmake --build build_release --target seekdb
```

编译约束：

+ 上述命令和参数固定，不能追加或修改编译选项；可以修改仓库内的 CMake 文件和 `tools/hipvs_bridge/build.sh`，但最终必须能用上述命令编译成功，并产出 `build_release/src/observer/seekdb` 和 bridge 动态库；
+ 第三方依赖使用组织方冻结的 V5 版本：仓库中的 `deps/3rd` 会被替换为冻结版本，Rust 依赖只能使用冻结的离线 vendor 目录。**不能新增需要联网下载的依赖**；确需新增的代码请以源码形式直接放入仓库；
+ 编译限制为最多 64 核、128 GB 内存、2 小时，编译时间不计入性能成绩。

## 四、云端统一评测流程
正式成绩全部由比赛组织方在统一的 AMD GPU 云端服务器上产生。

参赛者在比赛平台提测时，评测系统使用报名时绑定的 seekdb 答题仓库。提测时指定了分支或 commit 的，按指定版本检出；未指定的，使用仓库默认分支的最新提交。评测系统会锁定实际检出的完整 commit，后续编译和测试始终使用同一个代码版本，并在成绩中回传该 commit。

### 4.1 答题仓库地址格式（必须）
答题仓库保持私有，评测系统通过报名时登记的仓库地址中携带的只读令牌拉取代码。**报名登记的仓库地址必须是以下格式，否则评测直接失败（**`failMsg = 4`**）：**

```latex
https://<GitHub 用户名>:<只读令牌>@github.com/<仓库所有者>/<仓库名>
```

> **只接受 **`https://<用户名>:<令牌>@github.com/<所有者>/<仓库>`** 这一种格式。** 地址不符合该格式时，评测系统不会尝试拉取代码，直接回传评测失败（`failMsg = 4`），本次提测不计分。
>

令牌请按以下要求创建（GitHub → Settings → Developer settings → Personal access tokens → Fine-grained tokens）：

+ **Repository access** 只选择本赛题的答题仓库；
+ **Permissions** 只授予 `Contents: Read-only`，不要授予任何写权限；
+ 有效期覆盖比赛及成绩处理期（比赛结束后 11 天），比赛结束后请自行撤销；
+ 不要把令牌写入代码、提交记录、Issue 或聊天群。评测系统只用它拉取代码，不会写入日志或回传。

以下地址会被拒绝：不带令牌的 `https://github.com/...`、SSH 地址 `git@github.com:...`、非 GitHub 地址、仓库内的子路径（如 `.../tree/main`）。

每次正式评测依次执行：

1. 拉取并检出上述 commit；
2. 检查该 commit 是否为 V5 冻结 commit 的后代；
3. 使用组织方固定的编译环境和构建命令，从源码重新编译 seekdb 和 hipVS bridge。编译环境**不联网**，只能使用基线已有的第三方依赖；
4. 检查候选二进制、动态库和 GPU 后端是否正确生成；
5. **GPU 路径抽检**：评测会使用外部 profiler 抽检 GPU CAGRA 路径，未通过的参赛版本不进入正式测试；
6. **正式测试**：在相同 GPU、软件环境、数据集和索引参数下，每个数据集运行一组“冻结基线 + 参赛版本”配对，两个数据集交换先后顺序（见 6.4）；
7. 自动检查结果正确性、Recall、构建与查询阶段的 GPU 使用、CPU 回退、查询回归和资源使用；
8. 根据测试结果自动计算并回传成绩。

一次完整评测的 GPU 阶段总时长上限为 15 分钟；冻结基线本身约需 10 分钟。

组织方使用固定测试程序进行评测，不以参赛者自带的测试脚本、预编译二进制、自报耗时或性能报告作为正式评分依据。源码编译时间不计入性能成绩。

参赛者可以在本地进行功能和性能测试，但本地结果仅用于开发参考，最终成绩以云端统一评测结果为准。

## 五、分数结构
总分为 20 分：

| 项目 | 分数 | 权重 |
| --- | ---: | ---: |
| 正确性与硬门槛 | 4 分 | 20% |
| CAGRA 构建性能 | 14 分 | 70% |
| 批量查询性能 | 2 分 | 10% |
| 总分 | 20 分 | 100% |


评分结构为：

```latex
4 + 14 + 2
```

4 分属于二元硬门槛：

+ 全部硬门槛通过：获得 4 分，并继续计算性能分；
+ 任一硬门槛失败：本次总分为 0。

## 六、构建性能测试规则
正式计分使用两个 250K 数据集：

| 数据集 | 规模与维度 | 权重 |
| --- | --- | ---: |
| SIFT250K | 250K × 128 | 40% |
| Hidden250K | 250K × 256 | 60% |


输入均为 FP32 + L2。Hidden250K 的内容、顺序和 ID 不公开。

### 6.1 对比对象
正式评分比较参赛版本和冻结比赛基线：

```latex
构建加速比
= 冻结比赛基线端到端构建时间
/ 参赛版本端到端构建时间
```

主要评分指标不是简单的 CPU 与 GPU 对比，而是参赛版本相对冻结基线的提升。

### 6.2 计时起点
开始计时时：

+ 原始向量表已经通过 `LOAD DATA` 导入，表上没有向量索引；
+ 目标向量索引不存在；
+ 没有可复用的 CAGRA 图、索引文件或进程内缓存。

计时从执行以下语句开始（离线 DDL 路径）：

```sql
CREATE VECTOR INDEX idx_items ON items(v)
  WITH (distance=l2, type=hnsw, lib=cuvs, m=16, ef_construction=200, ef_search=64);
```

查询统一使用：

```sql
SELECT id FROM items ORDER BY l2_distance(v, <query>) APPROXIMATE LIMIT <k>;
```

以上 SQL 和索引参数由评测程序固定，参赛者不能修改。

### 6.3 计时终点
结束计时时必须满足：

+ 索引创建完成；
+ 数据扫描与 CPU/GPU 传输完成；
+ CAGRA 图构建与 GPU 同步完成；
+ 索引已经可以查询；
+ 第一条固定查询完成并返回合法结果。

第一条查询必须计入构建时间，防止将惰性建图延迟到计时范围之外。

Git clone、源码编译、数据集生成、原始数据导入和服务启动时间不计入构建性能。

### 6.4 重复测试
每个正式数据集对冻结基线和参赛版本各运行 1 次，共 2 轮。两个数据集交换运行顺序：

```latex
数据集 A：baseline -> candidate
数据集 B：candidate -> baseline
```

每轮都会清除索引和缓存，从相同数据状态重新开始。交换顺序用于降低 GPU 温度、时钟和先后顺序造成的偏差。

如果检测到 GPU 状态、系统负载、profiler 或结果采样异常，该数据集的本次配对作废并重新运行，不直接处罚参赛者。多个数据集之间按权重计算综合构建加速比。

## 七、查询性能测试规则
索引建成后，评测系统使用固定 `nq=4096`，分别执行 Top-10 和 Top-100 查询，记录：

+ QPS；
+ 近似 P95 延迟；
+ Recall@10 和 Recall@100；
+ 返回 ID 的正确性。

查询性能用于计算 2 分查询性能分。

本节所说的 `baseline`，统一指比赛冻结基线：

```latex
Tag：amd-cagra-contest-baseline-v5
Commit：634888d9b037153fa143d2c913ba4b25503e1d62
```

它不是 CPU VSAG 路径，也不是参赛者自行选择的修改前版本。评测系统会在同一轮、同一台服务器和相同测试条件下运行该冻结 commit，并与参赛版本进行比较。

任何正式数据集上的综合查询吞吐都不得低于同轮冻结基线的 90%：

```latex
candidate_query_qps / baseline_query_qps >= 0.90
```

吞吐门槛和查询性能分只统计 `nq=4096` 的 Top-10 与 Top-100 两组：每个数据集先分别计算两组的 QPS 比，再取几何平均。`nq=4096` 是 4096 条单行近似查询 SQL，不是专门的批量查询接口。

`nq=1` 的单条查询只做诊断记录，不计入 Recall 门槛、吞吐门槛和查询性能分。计时终点的“第一条固定查询”只检查返回结果是否合法，其耗时计入构建时间。

## 八、硬门槛
参赛版本必须同时满足：

1. **能够完成评测**：代码可以从源码编译，并在规定时间和云端资源范围内完成全部测试；必须通过 GPU 路径抽检；
2. **结果正确**：索引和查询结果完整，返回 ID 合法且符合数据语义；每条查询必须返回 `k` 个互不重复的 ID，否则判为 `result_integrity` 失败；
3. **检索质量达标**：SIFT250K Recall@10/100 不低于 `0.95`，Hidden250K Recall@10 不低于 `0.90`、Recall@100 不低于 `0.85`，并且相比同轮冻结基线下降不超过 `0.005`；
4. **真实使用 GPU 建图和查询**：索引构建和查询必须由 GPU 上的 hipVS/cuVS CAGRA 完成，不能回退到 VSAG/CPU，也不能以其他方式代替 GPU 计算。评测会通过多种方式核验，未通过判为硬门槛失败（`gpu_path`）；
5. **无明显功能或性能回归**：基线已有的基本索引功能仍可使用，并满足上一节规定的查询性能下限（只统计 `nq=4096` 的 Top-10/Top-100）。“基本功能”按对外 SQL 行为判定，指 `lib=vsag` 索引的 SQL 建表、建索引和查询仍然可用，不约束 `lib=cuvs` 的内部实现；
6. **资源使用不超限**：显存峰值不超过同轮冻结基线的 `1.25` 倍，并满足下表的云端资源配额。

云端资源配额：

| 阶段 | CPU | 内存 | 显存 | 时间上限 |
| --- | ---: | ---: | ---: | ---: |
| 源码编译 | 最多 64 核 | 128 GB | — | 2 小时 |
| seekdb 评测进程 | 24 核 | 96 GB | 45 GB，且不超过基线 1.25 倍 | 整轮 GPU 评测 15 分钟 |


每次评测独占一张 AMD Radeon PRO W7900D。

## 九、性能计分
基于 V5 冻结基线标定：

```latex
综合构建加速达到 3.0x：获得全部 14 分构建分
综合查询加速达到 1.5x：获得全部 2 分查询分
```

基线与满分线之间采用连续计分，不是只有“通过”和“不通过”两档。计分公式：

```latex
综合构建加速比 = SIFT250K 构建加速比^0.4 × Hidden250K 构建加速比^0.6     （加权几何平均）
综合查询加速比 = SIFT250K 查询加速比^0.4 × Hidden250K 查询加速比^0.6

构建性能分 = 14 × clamp( ln(综合构建加速比) / ln(3.0), 0, 1 )
查询性能分 =  2 × clamp( ln(综合查询加速比) / ln(1.5), 0, 1 )
```

其中 `clamp(x, 0, 1)` 表示把 x 限制在 0～1 之间；每个数据集的查询加速比是 Top-10 与 Top-100 两组 QPS 比的几何平均。

分数随加速比按**对数**增长，不是线性的。例如综合构建加速 1.2x 约得 2.3 分、1.5x 约得 5.2 分、2.0x 约得 8.8 分、3.0x 及以上得满分 14 分；加速比不超过 1.0x 时性能分为 0。

最终得分：

```latex
全部硬门槛通过：
总分 = 4 + 构建性能分 + 查询性能分

任一硬门槛失败：
总分 = 0
```

最终得分范围为 `0～20`，最多保留三位小数。

冻结基线本身的综合构建加速和查询加速均为 `1.0x`，因此直接提交未修改的基线只能获得 4 分。

## 十、评测结果说明
比赛平台上的每次提测会显示以下两类结果之一。

**评测完成（taskStatus = 2）**：通过时显示得分和加速比，例如 `build=1.427x, query=1.083x, gates=pass`；未通过时得 0 分，并列出未通过的门槛：

| 门槛代码 | 含义 |
| --- | --- |
| `<数据集>:recall@10_minimum` / `recall@100_minimum` | Recall 低于绝对下限 |
| `<数据集>:recall@10_baseline_delta` / `recall@100_baseline_delta` | Recall 比同轮基线下降超过 `0.005` |
| `<数据集>:query_regression` | 查询吞吐低于同轮基线的 90% |
| `gpu_path` | 未通过 GPU CAGRA 路径核验 |
| `result_integrity` | 查询返回的 ID 不足 `k` 个或有重复 |
| `relative_vram_limit` / `physical_vram_limit` / `host_memory_limit` | 资源超限 |
| `cpu_path` / `cpu_fallback_observed` / `index_integrity` / `stability` | CPU 路径、回退、索引完整性或稳定性检查失败 |


**评测失败（taskStatus = 3）**：无法完成本次评测，不计分，原因代码如下：

| failMsg | 含义 |
| ---: | --- |
| 1 | 指定分支不存在 |
| 2 | 指定 commit 不存在 |
| 3 | 编译失败，或未通过 GPU 路径抽检 |
| 4 | git clone 失败：仓库地址不是 `https://<用户名>:<令牌>@github.com/<所有者>/<仓库>` 格式，或令牌无效、过期、没有该仓库的读取权限 |
| 5 | git clone 超时 |
| 6 | 检出指定版本失败 |
| 7 | CMake 失败 |
| 8 | 验证失败，例如提交不是 V5 冻结 commit 的后代 |


因评测机自身原因（GPU 被占用、服务异常等）中断的评测会自动重跑，不计入参赛者的失败。

## 十一、禁止行为
以下行为会导致成绩无效或计 0 分：

+ 不基于 V5 冻结 commit 提交（基于 V1～V4 的代码需先合入 V5）；
+ 使用预编译 seekdb 代替源码构建；
+ 复用上一轮 CAGRA 图、索引文件或缓存；
+ 减少测试数据，或者修改评测输入的精度和内容；
+ 通过降低 Recall 换取性能；
+ 使用 VSAG/CPU 回退结果冒充 GPU CAGRA；
+ 在构建或查询阶段用 CPU 计算替代 GPU；
+ 针对评测流程的不同阶段采用不同的实现路径；
+ 将首次查询中的惰性建图移出计时范围；
+ 针对隐藏数据硬编码；
+ 读取、推测或伪造评测数据和结果。

## 十二、参考资料
+ [AMD Radeon 使用和部署 seekdb](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/lcuknwgl955v8ou7)
+ [比赛基线仓库](https://github.com/zihaomu/seekdb/tree/amd-oceanbase-official-contest-base-v5)
+ [seekdb × hipVS 实验 Notebook](https://github.com/zihaomu/notebook/blob/main/seekdb_hipvs_notebook/seekdb_hipvs_batch_demo_executed.ipynb)

Notebook 中约 `12.65x` 的结果是批量查询加速，不是索引构建加速，不能直接作为本赛题的构建成绩。

