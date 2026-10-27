"""Owned-instance SQL build-to-query benchmark. Never operates on an existing server.

The measured interval includes CREATE VECTOR INDEX and its first usable ANN query.
Raw import, startup and exact-oracle generation are outside that interval.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import statistics
import subprocess
import time

import numpy as np


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def literal(vector):
    return "'[" + ",".join(format(float(x), ".9g") for x in vector) + "]'"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--helper", type=Path)
    ap.add_argument("--mode", choices=["cpu", "gpu", "failure"], required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--rows", type=int, default=20000)
    ap.add_argument("--queries", type=int, default=100)
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--port", type=int, default=2981)
    ap.add_argument("--affinity", default="0-15")
    args = ap.parse_args()
    if args.rows < 100 or args.queries < 2 or args.rounds < 1:
        ap.error("rows>=100, queries>=2 and rounds>=1 are required")
    binary = args.binary.resolve(strict=True)
    helper = args.helper.resolve(strict=True) if args.helper else None
    if args.mode != "cpu" and helper is None:
        ap.error("--helper is required for GPU/failure modes")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    base = out / "instance"
    base.mkdir()
    env = os.environ.copy()
    env.pop("SEEKDB_HIPVS_BUILD_HELPER", None)
    env.update(OMP_NUM_THREADS="16", OPENBLAS_NUM_THREADS="16")
    if helper is not None and args.mode != "cpu":
        env["SEEKDB_HIPVS_BUILD_HELPER"] = str(helper)
    mysql = ["mysql", "--protocol=TCP", "-h127.0.0.1", f"-P{args.port}", "-uroot", "-N", "-B"]
    prefix = "SET ob_query_timeout=300000000; SET ob_trx_timeout=300000000; SET ob_hnsw_ef_search=512; "
    transcript = open(out / "sql-transcript.jsonl", "w", buffering=1)
    server = None
    handle = None
    report = {"mode": args.mode, "rows": args.rows, "dim": 128, "queries": args.queries,
              "k": 10, "seed": 20261027, "m": 16, "ef_construction": 200, "ef_search": 512,
              "affinity": args.affinity, "binary_sha256": digest(binary),
              "helper_sha256": digest(helper) if helper else None,
              "rounds": [], "checks": [], "state": "running"}

    def save():
        (out / "report.json").write_text(json.dumps(report, indent=2))

    def sql(query, db=True, check=True):
        command = prefix + ("USE amdbench; " if db else "") + query
        r = subprocess.run(mysql, input=command, text=True, capture_output=True, timeout=330)
        transcript.write(json.dumps({"sql": query, "rc": r.returncode, "stdout": r.stdout, "stderr": r.stderr}) + "\n")
        if check and r.returncode:
            raise RuntimeError(f"SQL failed: {query[:180]}\n{r.stderr}")
        return r

    def start(reopen=False):
        nonlocal server, handle
        # The socket check is read-only. Refuse to attach to any other running database.
        import socket
        with socket.socket() as sock:
            if sock.connect_ex(("127.0.0.1", args.port)) == 0:
                raise RuntimeError("requested port already in use")
        handle = open(out / ("reopen.stdout" if reopen else "server.stdout"), "w")
        server = subprocess.Popen(["taskset", "-c", args.affinity, str(binary), f"--base-dir={base}",
                    f"--port={args.port}", "--nodaemon", "--log-level=INFO",
                    "--parameter", "memory_limit=8G", "--parameter", "datafile_size=2G",
                    "--parameter", "datafile_maxsize=4G", "--parameter", "log_disk_size=2G"],
                    cwd=base, env=env, stdout=handle, stderr=subprocess.STDOUT, start_new_session=True,
                    # Block before exec so constructor-created library threads also
                    # inherit seekdb's sigwait mask (rather than default termination).
                    preexec_fn=lambda: signal.pthread_sigmask(signal.SIG_BLOCK,{signal.SIGUSR1,signal.SIGTERM}))
        began = time.monotonic()
        for _ in range(180):
            if server.poll() is not None:
                raise RuntimeError(f"owned server exited {server.returncode}")
            probe = subprocess.run(mysql + ["-e", "SELECT 1"], capture_output=True, timeout=5)
            if probe.returncode == 0:
                report["reopen_seconds" if reopen else "startup_seconds"] = time.monotonic()-began
                return
            time.sleep(1)
        raise RuntimeError("owned server startup timed out")

    def stop():
        nonlocal server, handle
        if server is not None:
            # seekdb's ObSignalHandle uses SIGUSR1 for prepare_stop/set_stop.
            # SIGTERM can terminate the process rather than perform a normal close.
            server.send_signal(signal.SIGUSR1)
            try:
                rc = server.wait(timeout=150)
            except subprocess.TimeoutExpired:
                os.killpg(server.pid, signal.SIGKILL)
                server.wait()
                raise RuntimeError("owned server failed graceful shutdown")
            server = None
            if rc != 0:
                raise RuntimeError(f"owned server shutdown code {rc}")
        if handle:
            handle.close()
            handle = None

    def rows(query):
        return [line.split("\t") for line in sql(query).stdout.splitlines() if line]

    def ann(table, vector, condition=""):
        return rows(f"SELECT id FROM {table} {condition} ORDER BY l2_distance(v,{vector}) APPROXIMATE LIMIT 10")

    def assert_check(name, passed):
        report["checks"].append({"name": name, "passed": bool(passed)})
        save()
        if not passed:
            raise AssertionError(name)

    def native_markers():
        return sum(path.read_text(errors="replace").count("HIPVS_NATIVE_BUILD_COMPLETE fallback=false")
                   for path in (base/"log").glob("seekdb.log*") if path.is_file())

    def wait_top_id(table, vector, expected, present=True):
        deadline=time.monotonic()+45
        while time.monotonic()<deadline:
            answer=[int(x[0]) for x in ann(table,vector)]
            if ((answer and answer[0]==expected) if present else expected not in answer):
                return True
            time.sleep(.25)
        return False

    rng = np.random.default_rng(20261027)
    vectors = rng.standard_normal((args.rows, 128)).astype(np.float32)
    queries = rng.standard_normal((args.queries, 128)).astype(np.float32)
    ids = np.arange(args.rows, dtype=np.int64)*17+10_000_000_000
    valid_ids=set(ids.tolist())
    literals = [literal(v) for v in vectors]
    ql = [literal(q) for q in queries]
    vectors.tofile(out / "base.f32")
    queries.tofile(out / "queries.f32")
    ids.tofile(out / "ids.i64")
    began = time.monotonic()
    truth = []
    for q in queries:
        d = ((vectors.astype(np.float64)-q.astype(np.float64))**2).sum(axis=1)
        truth.append(ids[np.argsort(d, kind="stable")[:10]].tolist())
    report["oracle_seconds"] = time.monotonic()-began
    report["data_sha256"] = {name: digest(out/name) for name in ["base.f32", "queries.f32", "ids.i64"]}
    (out / "truth.json").write_text(json.dumps(truth))
    save()
    try:
        start()
        report["version"] = sql("SELECT version()", db=False).stdout.strip()
        sql("CREATE DATABASE amdbench", db=False)
        for round_id in range(args.rounds):
            table = f"vectors_r{round_id}"
            sql(f"CREATE TABLE {table}(id BIGINT PRIMARY KEY, v VECTOR(128))")
            began = time.monotonic()
            for offset in range(0, args.rows, 250):
                batch = ",".join(f"({ids[i]},{literals[i]})" for i in range(offset,min(offset+250,args.rows)))
                sql(f"INSERT INTO {table} VALUES {batch}")
            record = {"round": round_id, "import_seconds": time.monotonic()-began}
            create = (f"CREATE VECTOR INDEX vi ON {table}(v) WITH "
                      "(distance=l2,type=hnsw,lib=vsag,m=16,ef_construction=200,ef_search=512)")
            before=native_markers()
            began = time.monotonic()
            result = sql(create, check=args.mode!="failure")
            record["create_seconds"] = time.monotonic()-began
            if args.mode == "failure":
                assert_check("helper_failure_rejected",result.returncode!=0)
                assert_check("parent_alive_after_helper_failure",rows(f"SELECT count(*) FROM {table}")[0][0]==str(args.rows))
                report["rounds"].append(record)
                break
            first = ann(table, ql[0])
            record["build_to_first_query_seconds"] = time.monotonic()-began
            record["native_gpu_build_markers"]=native_markers()-before
            assert_check(f"round{round_id}_backend_evidence",record["native_gpu_build_markers"]==(1 if args.mode=="gpu" else 0))
            assert_check(f"round{round_id}_first_query_usable",len(first)==10)
            got, hits = [], 0
            for query_id, vector in enumerate(ql):
                answer = [int(x[0]) for x in ann(table, vector)]
                assert_check(f"round{round_id}_query{query_id}_valid_ids",
                             len(answer)==10 and len(set(answer))==10 and set(answer).issubset(valid_ids))
                hits += len(set(answer)&set(truth[query_id]))
                got.append(answer)
            record.update(recall_hits=hits, recall_denominator=args.queries*10,
                          recall_at_10=hits/(args.queries*10))
            (out / f"round{round_id}-results.json").write_text(json.dumps(got))
            assert_check(f"round{round_id}_recall_local_floor_095",record["recall_at_10"]>=.95)
            assert_check(f"round{round_id}_base_count",rows(f"SELECT count(*) FROM {table}")[0][0]==str(args.rows))
            report["rounds"].append(record)
            save()
            print(json.dumps(record),flush=True)
        if args.mode != "failure":
            table=f"vectors_r{args.rounds-1}"
            filtered=[int(x[0]) for x in ann(table,ql[0],"WHERE MOD(id,2)=0")]
            exact=[int(x[0]) for x in rows(f"SELECT id FROM {table} WHERE MOD(id,2)=0 ORDER BY l2_distance(v,{ql[0]}) LIMIT 10")]
            assert_check("filtered_ANN_matches_SQL_exact",filtered==exact)
            plan=sql(f"EXPLAIN SELECT id FROM {table} ORDER BY l2_distance(v,{ql[0]}) APPROXIMATE LIMIT 10").stdout
            (out/"explain.txt").write_text(plan)
            assert_check("ANN_plan_uses_vector_index","VECTOR" in plan.upper())
            sentinel=900_000_000_000_000_001
            sql(f"INSERT INTO {table} VALUES({sentinel},{ql[0]})")
            assert_check("insert_after_build",wait_top_id(table,ql[0],sentinel))
            sql(f"UPDATE {table} SET v={ql[1]} WHERE id={sentinel}")
            assert_check("update_after_build",wait_top_id(table,ql[1],sentinel))
            sql(f"DELETE FROM {table} WHERE id={sentinel}")
            assert_check("delete_after_build",wait_top_id(table,ql[1],sentinel,present=False))
            sql("CREATE TABLE small_case(id BIGINT PRIMARY KEY,v VECTOR(128))")
            sql("INSERT INTO small_case VALUES "+",".join(f"({ids[i]},{literals[i]})" for i in range(32))+",(-1,NULL)")
            sql("CREATE VECTOR INDEX vi ON small_case(v) WITH(distance=l2,type=hnsw,lib=vsag,m=16,ef_construction=200)")
            small_answer=[int(x[0]) for x in ann("small_case",literals[0])]
            assert_check("small_snapshot_and_null",len(small_answer)==10 and small_answer[0]==int(ids[0]) and -1 not in small_answer)
            sql("CREATE TABLE empty_case(id BIGINT PRIMARY KEY,v VECTOR(128), VECTOR INDEX vi(v) WITH(distance=l2,type=hnsw))")
            assert_check("empty_index_query",ann("empty_case",ql[0])==[])
            before_reopen=native_markers()
            stop()
            start(reopen=True)
            hits=0
            for qid,q in enumerate(ql):
                answer=[int(x[0]) for x in ann(table,q)]
                assert_check(f"reopen_query{qid}_valid",len(answer)==10 and len(set(answer))==10)
                hits+=len(set(answer)&set(truth[qid]))
            report["reopen_recall_at_10"]=hits/(args.queries*10)
            assert_check("reopen_recall_local_floor_095",report["reopen_recall_at_10"]>=.95)
            assert_check("reopen_does_not_rebuild_GPU_graph",native_markers()==before_reopen)
            report["median_build_to_first_query_seconds"]=statistics.median(x["build_to_first_query_seconds"] for x in report["rounds"])
        stop()
        report["state"]="passed"
    except BaseException as exc:
        report["state"]="failed"
        report["error"]=repr(exc)
        raise
    finally:
        try:
            stop()
        finally:
            transcript.close()
            save()
    print(json.dumps({k:v for k,v in report.items() if k not in ["checks","rounds"]}),flush=True)


if __name__ == "__main__":
    main()
