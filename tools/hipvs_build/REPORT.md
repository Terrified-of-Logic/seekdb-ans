# AMD 题实现与实测报告

## 2026-10-28 优化轮（当前实现）

日期：2026-10-28（Asia/Shanghai）。状态：实现完成，在稳定 AMD 实例（与 2026-10-27 同型号、同配额）上通过完整 CPU/GPU A/B、重开与故障回退验证；尚未提测。以下数据取代 2026-10-27 基线轮。

### 结果

| 数据量 / 维度 | 双方 ef_search | CPU 构建中位数 | hipVS 构建中位数 | 端到端加速 | CPU Recall@10 | GPU Recall@10（三轮范围） |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 20,000 × 128 | 512 | 5.385515 s | 1.473770 s | **3.654×** | 0.982 | 0.994–0.996 |
| 50,000 × 128 | 1024 | 16.175219 s | 2.093661 s | **7.726×** | 0.973 | 0.991–0.992 |
| 100,000 × 128（参考） | 1024 / 2048 | 39.96 s | 3.15 s | 12.7× | 0.927 | 0.938–0.939 |

20K/50K 各 3 轮，起点为已导入数据、索引尚不存在时发出 `CREATE VECTOR INDEX`，终点为首条可用 APPROXIMATE 查询；四组各通过 421 项脚本检查，重开 Recall 分别 0.982/0.994（20K）与 0.973/0.991（50K）。100K 只跑一轮，且 CPU/GPU 都未达到本地 0.95 线（提高 ef_search 到 2048 也基本不变，说明是 M=16/ef_construction=200 在该合成数据上的图质量上限），因此只作为“数据量增大时 GPU 优势扩大”的趋势参考，不作为合格质量结果。

与 2026-10-27 基线轮相比（同一机器形态、同一套测量脚本），20K 加速 3.228×→3.654×（+13.2%），50K 6.923×→7.726×（+11.6%），Recall 不降。

### 本轮改动

1. **helper 预启动与扫描重叠**：首批 staging 数据到达时即 `posix_spawn` helper，helper 完成 ROCm/hipVS 初始化后在 stdin 上等待；扫描结束后父进程写输入、发一个字节启动。初始化不再落在构建关键路径上（日志 `prestarted=1`，helper `wait_ms≈0.01`）。
2. **父进程线性校验**：原生序列化不保留 staging 行序，原实现对“第 i 行标签 == 第 i 个输入 id”的假设不成立。改为 `label → 输入行` 映射 + 逐行 vector 字节比较，并保留 3 个原生 `CalcDistanceById` 抽样探针；50K 校验 3 ms。
3. **helper mmap 标签重写 + 排序查重**：一次 `mmap` 重写全部 label，替代逐行 seek/read/seek；用排序代替 `std::set` 去重。
4. **helper 自绑核**：容器允许 CPU 数远大于 cgroup 配额时，未绑核的 host 侧 NN-Descent 记账会跨 NUMA 分散。实测 20K helper 未绑核 2.4 s、绑核/自绑核 0.60 s。helper 现在启动时自绑到至多 16 个允许 CPU。
5. **跳过退出析构**：图文件 `msync` 并关闭后 `_Exit(0)`，不把 cuVS/hnswlib 析构计入构建。
6. 新增 `HIPVS_HELPER_LOG` / `HIPVS_PARENT_TIMINGS` / `HIPVS_SNAPSHOT_FINISH` 阶段日志、可调参数（`HIPVS_CAGRA_*`、`HIPVS_SNAPSHOT_PREWARM`、`HIPVS_HNSW_THREADS`）、无 GPU 的协议测试 `test_fake_helper.py` 与标签重写单测 `test_hnsw_labels.cpp`。

### 阶段耗时（50K GPU，单轮典型值）

- helper：总 1.01 s = init 79 ms + load 19 ms + CAGRA 695 ms + HNSW 转换 212 ms + label 重写 3.5 ms。
- 父进程：写输入 10–18 ms、等待 helper 1.05–1.15 s、读回 17–19 ms、线性校验 3 ms、VSAG 导入 24 ms，合计 1.12–1.22 s。
- 端到端 2.09 s 中其余约 0.9 s 是扫描、快照序列化/发布与首查，与 CPU 路径共同承担。

### 验证

- 单测：`test_hnsw_labels.cpp` 通过（标签重写、重复标签、截断、id 数量不符四项拒绝路径）。
- 协议测试：`--helper test_fake_helper.py` 下父进程预启动、握手、写文件、线性校验、导入失败、显式 CPU 回退均按预期执行，查询 100/100 命中。
- 失败注入：`/bin/false` 明确记录 `HIPVS_CPU_FALLBACK reason=gpu_build_failed`，16 项检查通过、Recall 1.0，排除在 GPU 性能之外。
- 完整套件：20K/50K CPU/GPU 各 3 轮、重开不重复构图、过滤/DML/小表/NULL/空索引全部通过（每组 421 项）。
- 未绑核环境：20K GPU 端到端 1.597 s（新 helper 自绑核）与本机绑核结果相当。

### 环境与产物

- 实例：AMD EPYC 9334、cgroup 16 CPU、55 GiB，Radeon gfx1100 96 CU、ROCm 7.2.4。
- seekdb 版本 `5.7.25-OceanBase seekdb-v1.4.0.0`，二进制 SHA-256 `1844181e4a65b49448d1c1f2cfdd8d70d7b940705615b1c9531744e3b994e6b1`；helper SHA-256 `987ca3bdeb9e5a710ce234d1c938bd396703d587c3b549eaaed47838db33baf3`。
- 本轮全部源码改动、复现脚本和证据在 `tools/hipvs_build/`。

### 限制

- 未创建公开 PR、未上传评测平台、未取得官方评分；官方数据规模/目标倍数/Recall 门槛仍未公布。
- 100K 合成数据在 M=16/ef_construction=200 下 Recall 不足 0.95，属于索引参数与数据分布问题，CPU/GPU 同样受影响；未宣称该点合格。
- 低 niter / IVF_PQ 可继续缩短 helper（实测 50K niter=10/inter=32 端到端 2.01 s、Recall 0.983；IVF_PQ 2.72 s、Recall 0.975），但默认仍保持 NN-Descent niter=20，以不牺牲质量；参数说明见 `README.md`。
- 仅在单机 gfx1100、L2、float32、128 维和上述合成数据上验证。

---

日期：2026-10-27（Asia/Shanghai）。状态：**实现完成，真实 AMD 实例本地验证通过；尚未创建公开 PR 或进行比赛平台提测。**

## 结果

| 数据量 / 维度 | 双方 ef_search | CPU 构建中位数 | hipVS 构建中位数 | 端到端加速 | CPU Recall@10 | GPU Recall@10（三轮范围） |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 20,000 × 128 | 512 | 5.374261 s | 1.664880 s | **3.228×** | 0.982 | 0.991–0.994 |
| 50,000 × 128 | 1024 | 16.051240 s | 2.318539 s | **6.923×** | 0.973 | 0.989–0.995 |

CPU/GPU 使用同一最终二进制、同一批 float32 向量、相同 M=16 / ef_construction=200、相同 CPU 亲和性及内存配置。每组重新建表、导入原始数据、从不存在索引的状态构建三次。没有复用已有图。

每轮 100 条查询、K=10，完整分母为 1000；所有返回 ID 必须属于原始 BIGINT 文档 ID，且每条结果包含 10 个不同 ID。精确真值从原始向量直接计算。四组各通过 **421 项检查**，共 1684 项；另有故障回退 **16 项检查**。这些是脚本断言数量，不是比赛官方用例数或得分。

本次查阅的已保存官方题面没有写死最终目标倍数、统一评测数据规模及质量阈值。这里的 Recall ≥ 0.95 是补充本地验收线，不能据此声称已获 20 分或已经达到未公布的官方目标。

## 三轮原始计时

单位：秒；完整精度值及全部查询结果见 `evidence/` 下的 report/summary JSON。

| 组别 | 第 1 / 2 / 3 轮：CREATE 至首次查询成功 |
| --- | --- |
| 20,000 CPU | 5.396882270 / 5.374261169 / 5.373090051 |
| 20,000 GPU | 1.691212786 / 1.664880196 / 1.584821986 |
| 50,000 CPU | 16.051240101 / 15.916510485 / 16.119530622 |
| 50,000 GPU | 2.414453043 / 2.318539327 / 2.288430275 |

计时起点是已经完成原始数据导入、向量索引尚不存在时发出 CREATE VECTOR INDEX；终点是完成后首次 APPROXIMATE SQL 查询返回可用结果。包含数据扫描、staging 写入、helper 启动、CPU/GPU 传输、GPU 同步、GPU 构图、HNSW 转换、ID 写回、VSAG 原生导入、逐 VID/向量校验、原生序列化、发布和首次查询的重载。启动、数据生成、精确真值生成、原始数据导入均单独记录。

GPU helper 的单独耗时没有用作最终加速比，也没有采用预装 Notebook 的批量查询加速数字。

## 实现链路

1. 默认不设置 `SEEKDB_HIPVS_BUILD_HELPER`，仍走原 CPU 路径。显式设置可信 helper 的绝对路径后，仅普通 dense float32 / L2 HNSW 快照启用 GPU；增量句柄、量化索引及其他距离保留现有 CPU 行为。
2. 扫描数据时缓存原始向量与内部 VID，采用指数扩容，避免逐行输入导致二次复杂度拷贝。向量 staging 上限 1 GiB。
3. 发布快照键前以 posix_spawn 启动独立 helper。AMD hipVS CAGRA NN-Descent 完成底层图构建；CPU 仅完成稀疏上层层级转换。GPU 库在 helper 进程中加载，父数据库不链接 ROCm/hipVS。
4. 导出完整 HNSW，按实际标签重写原始 64 位 VID；检查布局、数量、标签唯一性。seekdb 通过 VSAG 原生 BinarySet 导入，逐 VID 校验对应原始未压缩向量，再使用现有快照序列化和查询链路。
5. 重载已落盘快照后关闭 staging 状态，正常重启查询不重复构图。
6. helper/GPU/导入失败时，在新的 CPU 索引对象上完整 Build，成功后再替换原图，记录 `HIPVS_CPU_FALLBACK reason=gpu_build_failed` 及原 GPU 错误。小快照 N≤4M 另记 `reason=small_snapshot`。回退没有 GPU 完成标记，不计入 GPU 成绩。

主要产品变更位于 `src/oblib/lib/vector/ob_vsag_adaptor.*`、`hipvs_snapshot.h`、`src/observer/vector_index/ob_plugin_vector_index_adaptor.cpp`；另登记了一个 Bazel 私有头文件。没有改动官方测试或评分资产。

## 验证范围与 GPU 证据

- 20K / 50K 各 CPU、GPU 三轮完整构建、原始文档 ID 和 Recall 检查。
- SQL 过滤结果与精确 SQL 对照，索引构建后插入、更新、删除，含 NULL 的小表、空索引查询。
- 四组均正常关闭并在原目录重启，全部查询继续通过；重启没有增加 GPU 构图标记。
- 每个正式 GPU 轮次恰有一个 `HIPVS_NATIVE_BUILD_COMPLETE fallback=false`；CPU 轮次为零。
- `/bin/false` 注入 helper 失败：明确记录 CPU 回退，1000 条数据完整、Recall=100/100，父进程继续提供 SQL 服务；该运行排除在 GPU 性能表之外。
- helper 单独测试：截断输入、重复 VID、GPU 不可见均返回失败。
- rocprofv3 原始 kernel / memory-copy CSV 证明 AMD GPU 真正执行了 NN-Descent local_join、CAGRA prune/sort 等构图内核。该分析单独运行，不混入正式性能计时。

## 环境、来源与可复现性

- 官方基线：`5bce9e06d1281347ad61344e4b01fab9158c58a2`（2026-competition）。
- 最终运行实现：`781ba1e14`；完整测试套件提交：`25c3f26d8e0d4374c510c298bfbbdd6c40bba5d2`。其后的私有头文件登记和报告不改变 CMake 产品代码。
- CPU：AMD EPYC 9334；容器 CPU 配额 16 核，两组均 taskset 0–15。
- GPU：AMD Radeon Graphics，gfx1100，96 CU，49136 MB GDDR6；ROCm 7.2.4。
- 容器内存上限：59055800320 字节；seekdb memory_limit=8G；datafile_size=2G、datafile_maxsize=4G、log_disk_size=2G。
- Python：3.12.3；NumPy：2.4.6；随机种子 20261027。
- seekdb SHA-256：`3f059a921f1af32fbf14d2e140496f33eeba767d2ccffb5723b4f8e2f01227ad`。
- helper SHA-256：`117a73e8396feb7a78cc48fb7c39fbbbd1a07be387c08a663af28389a6190612`。
- 详细产品源码、hipVS 库、输入文件哈希见 `evidence/manifest.json`、各组 `report.json`、`environment.json`。

独立目录从官方基线建立，保留了用户已有 AI/FTS 源码工作区。先完成独立全量 CMake 构建；后续仅重编译变化的向量 unity 单元，更新对应静态归档并重链完整程序。所有最终产品文件与编译输入逐字比较一致。

构建机 glibc 2.43 高于实例的 2.39，首次二进制无法启动；最终重链使用实例的 libm，remainder 依赖回到 GLIBC_2.2.5，实际实例 ldd 和全部 SQL 验证通过。归档源码没有携带 .git，所以编译版本行显示 `1-.`；来源以 Git 提交、源码哈希及完整构建日志证明，不能把空版本行当成代码基线。

Bazel 私有头文件已登记；没有运行完整 Bazel 构建。全量 header inventory 生成检查仍显示基线范围内的其他归属漂移，没有把无关大批量重生成结果混入 AMD 改动。已实际执行并通过的是 CMake 完整构建和上述真实 GPU/SQL 验证。

## 复现

依赖和构建命令见同目录 `README.md`。在有 ROCm / hipVS 的 Linux 实例中构建 helper 后执行：

```bash
PYTHON=python3 bash tools/hipvs_build/run_instance_suite.sh \
  "$PWD/build_release/src/observer/seekdb" \
  "$PWD/tools/hipvs_build/hipvs-build-helper" "$PWD/evidence/new-suite"
```

输出目录必须不存在，端口 2981 必须空闲。脚本只启动、正常关闭自己的实例。20K 双方 ef_search=512，50K 双方 ef_search=1024；原始向量、参数、数据哈希和完整结果一并输出。

当前用户 AMD 实例已部署的最终程序为 `/workspace/amd-contest-20261027/seekdb-amd-verified`，原始测试目录为 `/workspace/amd-contest-20261027/evidence/verified-suite`。所有测试数据库进程已在正常关闭流程中退出；云实例本身仍保留。

## 保留的负结果与限制

- 初版 staging 每批精确 reserve，20K 只有约 1.32×。指数扩容后性能显著改善；没有将旧数据冒充最终结果。
- 50K CPU、ef_search=512 的 Recall 为 0.939，未通过本地 0.95 线，该组没有作为合格性能结果。随后双方统一提高到 1024，再各测三轮，CPU 0.973、GPU 0.989–0.995。
- 故障注入发现上层 CREATE 可能不向 SQL 客户端传播底层序列化错误，因此改为完整、可审计的 CPU 回退，避免留下成功外观但不完整的索引。
- 仅实际验证了单机 gfx1100、上述合成数据、L2、128 维和参数；更大规模、多 GPU、高并发、大量快照同时构建及完整故障矩阵尚未验证。HNSW 二进制布局适配明确针对该 x86_64 依赖版本。
- 未创建公开 PR、未上传评测平台、未取得官方评分。

完整原始 server 日志、SQL 输入输出和两种规模原始向量存于用户工作区 `资料/amd/2026-10-27/原始证据/final-runtime-logs.tar.gz`；压缩包仅归档原始文件，没有重写日志。源码内 `evidence/` 保留可审阅的报告、完整查询 ID/真值、环境及哈希；GPU trace 另存 `helper-evidence.tar.gz`。
