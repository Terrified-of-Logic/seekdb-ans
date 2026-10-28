# AMD hipVS HNSW snapshot construction

This implementation targets official seekdb `2026-competition`, base commit
`5bce9e06d1281347ad61344e4b01fab9158c58a2`.

## Integration

`SEEKDB_HIPVS_BUILD_HELPER=/absolute/path/to/hipvs-build-helper` explicitly enables
the path for ordinary dense float32 L2 HNSW **snapshot handles only**, without
extra-info columns. Unset it for the unchanged CPU baseline. Incremental handles,
other metrics and quantized indexes keep their existing CPU implementation.

Snapshot scans append vectors and their internal VIDs into a bounded staging
buffer instead of calling VSAG `Add` for every batch. The helper is spawned on
the first staged batch and blocks on a one-byte start signal until the scan has
staged every vector, so ROCm/hipVS process initialization overlaps the scan
instead of extending the build. Before the snapshot key prefix is published,
seekdb finishes writing the input, releases the helper, imports its complete
graph through the native VSAG `BinarySet` interface, and validates the count and
every stored label/vector association. Existing native serialization, reload,
filters and incremental DML then apply to the result.

The serialized HNSW record order is not the staged row order, so the parent
validates records through a label-to-input-row map and a linear byte comparison
instead of one random graph lookup per row. The helper rewrites all labels
through a single `mmap` and skips library teardown with `_Exit` after the graph
is synced. It also reads the cgroup CPU quota and pins itself to a compact set of
that size when the allowed CPU set is larger, because unpinned host-side
NN-Descent bookkeeping then scatters across NUMA domains (measured 0.60 s pinned
versus 2.4 s unpinned for 20,000 rows). The sparse upper-layer build uses at most
16 threads, capped by the same budget.

The helper uses AMD hipVS CAGRA NN-Descent for the base graph, synchronizes the
GPU, converts to hierarchical HNSW using CPU upper-layer construction, serializes
the graph, and writes the original signed 64-bit VIDs into its label slots.
All original float32 vectors are retained. The native format adapter explicitly
checks the x86_64 hnswlib header/layout; it is not a generic cross-platform parser.
The hipVS package keeps the `cuvs*` C API names on AMD; these names do not imply CUDA.

GPU libraries load in the helper process only. The parent needs no ROCm headers,
GPU build flags or link dependencies. A helper/GPU/import failure builds a fresh
complete CPU graph and emits `HIPVS_CPU_FALLBACK reason=gpu_build_failed` with the
original GPU error; it is never credited as a successful GPU build. Small snapshots with `N <= 4*M`
explicitly emit `HIPVS_CPU_FALLBACK reason=small_snapshot` and use CPU construction.
These cases must be excluded from GPU performance claims. Staging is limited to
1 GiB of vector data. The helper has a 300-second timeout. The default tested
parameters are `M=16`, `ef_construction=200`, `ef_search=512`.

### Tuning and test switches

All switches are optional; unset values reproduce the default NN-Descent build
and quality.

| Variable | Default | Meaning |
| --- | --- | --- |
| `SEEKDB_HIPVS_BUILD_HELPER` | unset | absolute path to the trusted helper; unset keeps the CPU path |
| `HIPVS_SNAPSHOT_PREWARM` | `1` | start the helper on the first staged batch |
| `HIPVS_CAGRA_BUILD_ALGO` | `nn_descent` | `nn_descent` or `ivf_pq` |
| `HIPVS_CAGRA_NITER` | `20` | NN-Descent iterations |
| `HIPVS_CAGRA_INTERMEDIATE_DEGREE` | `4*M` | intermediate graph degree (must be >= 2*M) |
| `HIPVS_HNSW_THREADS` | `16` | CPU threads for the sparse upper-layer construction |

Lower `HIPVS_CAGRA_NITER`/`INTERMEDIATE_DEGREE` shorten the build and can reduce
recall; measure both against the official quality threshold before changing the
default. `eval_recall.cpp` reports Recall@10 directly from a serialized graph
without seekdb, and `test_fake_helper.py` exercises the parent protocol
(pre-start, handshake, file layout validation, explicit CPU fallback) without an
AMD GPU.

## Build

Build seekdb normally on Linux, following the repository dependency instructions:

```bash
bash build.sh release --init -DOB_MAX_UNITY_BATCH_SIZE=8 -DNEED_PARSER_CACHE=OFF
bash build.sh release -DOB_MAX_UNITY_BATCH_SIZE=8 -DNEED_PARSER_CACHE=OFF --make -j6
```

On the supplied gfx1100 / ROCm 7.2.4 instance (hipVS headers and libraries in
`/opt/hipvs`):

```bash
bash tools/hipvs_build/build.sh
export SEEKDB_HIPVS_BUILD_HELPER="$PWD/tools/hipvs_build/hipvs-build-helper"
```

The helper rpath locates hipVS under the chosen `HIPVS_HOME`. Its transitive ROCm
libraries must also resolve (`ldd tools/hipvs_build/hipvs-build-helper`).
When cross-building on a newer Linux distribution, link against a target-compatible
sysroot/libm or compile inside the target distribution. A binary linked on glibc
2.43 may otherwise require symbols unavailable on the supplied glibc 2.39 image.

## End-to-end reproduction

The complete tested suite is also available as one command:

```bash
PYTHON=python3 bash tools/hipvs_build/run_instance_suite.sh \
  "$PWD/build_release/src/observer/seekdb" \
  "$PWD/tools/hipvs_build/hipvs-build-helper" "$PWD/evidence/verified-suite"
```

Requires Python 3 + NumPy, the `mysql` CLI, `taskset`, and a free local port.
Each command owns a new data directory and starts/stops only its own server.
The output directory must not already exist. No source data is reused as an index.
The runner establishes the server's signal mask before exec and uses seekdb's
`SIGUSR1` prepare-stop path for normal close/reopen (not a forced SIGTERM exit).

```bash
python tools/hipvs_build/benchmark.py \
  --binary build_release/src/observer/seekdb --mode cpu \
  --out evidence/cpu-20k --rows 20000 --queries 100 --rounds 3
python tools/hipvs_build/benchmark.py \
  --binary build_release/src/observer/seekdb --mode gpu \
  --helper "$PWD/tools/hipvs_build/hipvs-build-helper" \
  --out evidence/gpu-20k --rows 20000 --queries 100 --rounds 3
python tools/hipvs_build/benchmark.py \
  --binary build_release/src/observer/seekdb --mode failure --helper /bin/false \
  --out evidence/helper-failure --rows 1000 --queries 10 --rounds 1
```

The timer begins immediately before `CREATE VECTOR INDEX` on an already imported
table and ends after its first successful approximate SQL query. It includes
the entire scan, staging I/O, GPU initialization/transfers/build/synchronization,
HNSW conversion, ID restoration, native import, validation, native serialization,
index publication and first-query reload. Data/oracle generation, raw table
import, server startup and later quality checks are recorded separately.

CPU and GPU use the same binary, generated float32 data, original BIGINT SQL IDs,
parameters, 16-CPU affinity and 8 GiB server memory setting. Each round has a fresh
table and index. The reported speedup is CPU median / GPU median from at least
three rounds. Oracle search computes distances directly from the input vectors;
the `.95` recall assertion is a supplemental local check, **not an official
published contest quality threshold**. Official dataset/quality/speed targets
must be checked separately when available.

The script verifies every query has K distinct valid original IDs, Recall@10,
SQL filtering, insert/update/delete, small and NULL vectors, empty indexes,
normal shutdown/reopen and no new GPU build after reopen. Failure mode requires
an explicit CPU fallback log and complete SQL Recall, and is excluded from GPU timings.
It requires one native
GPU completion marker for each timed GPU round and zero for CPU rounds. Evidence
includes raw SQL transcripts, server logs, individual result IDs, exact truth,
data/binary/helper SHA-256 and per-round timings.
For the 50K quality check, pass `--ef-search 1024` to both CPU and GPU commands.
The unchanged 512 query breadth did not pass the local CPU Recall floor at 50K.

## GPU kernel evidence

Profile separately from timed benchmark rounds:

```bash
mkdir native-smoke
cd native-smoke
python ../tools/hipvs_build/generate_smoke.py
rocprofv3 --kernel-trace --memory-copy-trace --stats --output-format csv \
  -d profile -- ../tools/hipvs_build/hipvs-build-helper smoke.input smoke.hnsw
```

The helper success line `HIPVS_GPU_BUILD_SUCCESS ... fallback=false` establishes
its completion; seekdb's `HIPVS_NATIVE_BUILD_COMPLETE fallback=false` establishes
successful native import. `HIPVS_HELPER_LOG` forwards the helper phases
(`init/load/cagra/convert/rewrite`) into the server log, and
`HIPVS_PARENT_TIMINGS` reports the parent's write/wait/read/validate/import
breakdown, so the end-to-end time can be attributed without a profiler. Retain
the kernel/memory-copy CSV files as independent GPU evidence. Neither helper-only
time nor the packaged Notebook's batch-query speedup is the contest's end-to-end
build speedup.

Unit test for the label rewrite (no GPU required):

```bash
g++ -std=c++17 -O2 tools/hipvs_build/test_hnsw_labels.cpp -o /tmp/test_hnsw_labels
/tmp/test_hnsw_labels
```

API references:
[AMD hipVS HNSW](https://rocm.docs.amd.com/projects/hipVS/en/latest/reference/cpp_api/neighbors_hnsw.html),
[VSAG source version](https://github.com/antgroup/vsag/tree/129b82c).

## Remaining scope

Only the recorded hardware/software/workloads are validated. Multi-GPU, concurrent
large snapshot builds, exhaustive failure injection, and larger data sizes need
additional testing. This work does not modify official scoring or test assets.
