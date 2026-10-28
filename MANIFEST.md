# 来源清单（MANIFEST）

抓取时间：2026-10-28（Asia/Shanghai）
抓取方式：GitHub REST API + `git fetch`（部分克隆 `--filter=blob:none`），文件内容来自对应公开 ref。
基础仓库许可：`oceanbase/seekdb` 为 Apache-2.0；本目录下的文件/补丁均为该仓库公开 fork/分支的派生内容，
保留原仓库许可与来源说明。

## 官方基线：zihaomu/seekdb

- 仓库：<https://github.com/zihaomu/seekdb>（public，oceanbase/seekdb fork）
- 分支/标签：
  | ref | commit | 说明 |
  |---|---|---|
  | `amd-oceanbase-official-contest-base` / tag `amd-cagra-contest-baseline-v1` | `08c76418a8e1fef3c1aae83010658ff56a4faa57` | 题面冻结 commit |
  | `amd-oceanbase-official-contest-base-v2` / tag `amd-cagra-contest-baseline-v2` | `58ddbd7459` | |
  | `amd-oceanbase-official-contest-base-v3` / tag `amd-cagra-contest-baseline-v3` | `3d88d5bb59` | |
  | `amd-oceanbase-official-contest-base-v4` / tag `amd-cagra-contest-baseline-v4` | `ad3253a52f` | |
  | `amd-oceanbase-official-contest-base-v5` / tag `amd-cagra-contest-baseline-v5` | `634888d9b037153fa143d2c913ba4b25503e1d62` | 抓取时的最新基线 |
- 抓取内容：`exposed/baseline-v5/`（v1→v5 diff、v5 关键文件）

## 竞争实现 1：dyingtolife/seekdb

- 仓库：<https://github.com/dyingtolife/seekdb>（public）
- 分支：`amd-cagra-submit`
- commit：`bf0074456f4858f0f08d22e7d234ae67fbbb431d`（2026-10-28 10:52 UTC push）
- 提交链（相对 v5）：
  - `6bd55ba6f9` perf(cuvs): reuse per-index device scratch in the hipVS bridge
  - `12aab9b773` perf(cuvs): overlap the CAGRA build with the CPU index load
  - `bf0074456f` docs(cuvs): document the contest optimisation and its runtime switches
- diff：`exposed/dyingtolife/patches/v5-to-submit.diff`
- 其他分支（仅含部分改动）：
  - `amd-cagra-bridge-only` = `6bd55ba6f9`
  - `amd-cagra-docs-only` = `9ee116eb96`
- 抓取内容：`exposed/dyingtolife/`

## 竞争实现 2：pei-pei45/seekdb

- 仓库：<https://github.com/pei-pei45/seekdb>（public）
- 分支：`2026-competition-ecosystem`
- commit：`9d0b421fb6851da395e7192b109be6b75af323b9`（2026-10-27 21:00 CST push）
- 提交链（相对 v1）：
  - `a5243f6e8e` fix(bridge): size CAGRA itopk by the requested top-k
  - `9d0b421fb6` feat(vector): defer the VSAG CPU graph while cuVS CAGRA is active
- diff：`exposed/pei-pei45/patches/v1-to-branch.diff`
- 抓取内容：`exposed/pei-pei45/`

## 已检查但未抓到 / 无内容

| 仓库/分支 | 状态 | 说明 |
|---|---|---|
| `weiqdguewbfu/seekdb@contest-optim`（`c6e3d94724`） | 抓取时账号 404 | 2026-10-28 白天列表中曾存在 `contest-optim` 分支，随后转私有/删除，无法获取内容 |
| `JIANG2331/seekdb@amd-cagra-submit` | public，但无 AMD 改动 | 分支与 `develop` 同为 `1e97231d1d` |
| `disguiser501/seekdb`、`magicones/seekdb` | public，无 AMD 改动 | 只有 `develop` |
| 得分队 `idmg123`（600072 AMD = 20.000） | 未找到公开仓库 | GitHub 用户/仓库搜索无匹配；无法确认其实现来源 |

## 其他参考

- 官方 GPU 后端文档：`zihaomu/seekdb` 的 `docs/gpu-vector-index-hipvs-cuvs/README.md`
  （已抓取为 `exposed/baseline-v5/files/gpu-vector-docs-README.md`）
- 官方 GPU 冒烟验证脚本（v5）：`docs/gpu-vector-index-hipvs-cuvs/validation/run_gpu_smoke.sh`、
  `run_ddl_persistence_smoke.sh`（在 v1→v5 diff 中新增/修改）
- 题面（本地工作区）：`03-材料/语雀题面原文/yuque-eco-01-AMD.md`、`_work/yuque_amd_赛题.txt`
- 算力申请说明（本地工作区）：`_work/yuque_amd_radeon.txt`
