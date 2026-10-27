# Local File Tables (File SQL) — design notes

This document describes the "file as database" extension shipped in this tree:
the ability to run ordinary SQL directly against local CSV / JSONL / Parquet
files via `read_csv()`, `read_json()`, `read_parquet()`, `glob()` and
`COPY ... FROM` statements, controlled by the `enable_file_sql` session
variable and constrained by `secure_file_priv`.

## 1. Feature surface

| entry point | result |
|---|---|
| `read_csv(path[, format])` | a table whose columns are the CSV header cells |
| `read_json(path[, format])` | a table whose columns are the union of JSONL keys |
| `read_parquet(path[, format])` | a table decoded from a flat Parquet schema |
| `glob(path)` | a one-row-per-file directory listing |
| `DESCRIBE <file table>` | the inferred column list with source type names |
| `COPY <file table> FROM ...` | bulk load of the local file rows |

The extension is disabled by default (`enable_file_sql = OFF`); every path is
resolved through `secure_file_priv` and rejected when the file is outside of
it.  Official regression coverage lives in
`tools/deploy/mysql_test/test_suite/file_sql/` and is treated as read-only.

## 2. Layering

```
parser / resolver        read_csv() & friends are resolved into ObLocalTableDef
        |                (canonical path, format, columns, fingerprint)
        v
optimizer                ObLogLocalFileScan carries the definition and the
        |                raw expressions that read its columns
        v
code generator           the definition is copied into ObLocalFileScanSpec
        |
        v
executor                 ObLocalFileScanOp re-verifies the fingerprint and
                         streams rows through ObLocalFileReader
```

Source layout (all paths relative to `src/sql/`):

| file | responsibility |
|---|---|
| `ob_local_file_common.h` | `ObLocalTableDef`, `ObLocalColumnDef`, `ObLocalFileTableKind/Format/ColumnType` |
| `engine/basic/ob_local_file_reader.h` | `ObLocalColumnSpec`, `ObLocalCell`, `ObLocalFileSchema`, `ObLocalFileReader` |
| `engine/basic/ob_local_file_reader.cpp` | stream wrapper, fingerprinting, bounded schema cache, reader facade |
| `engine/basic/ob_local_file_reader_priv.h` | private value model shared by the format providers |
| `engine/basic/ob_local_csv_provider.cpp` | CSV record scanner, field classification, CSV schema inference |
| `engine/basic/ob_local_jsonl_provider.cpp` | JSONL line parser, key-union schema inference |
| `engine/basic/ob_local_file_schema.cpp` | type widening, column naming, draft → cell coercion |
| `engine/basic/ob_local_parquet_decoder.*` | Parquet footer / page decoder |
| `engine/basic/ob_local_file_scan_op.*` | physical operator (`SCAN` / `LIST` / `SCHEMA` modes) |
| `optimizer/ob_log_local_file_scan.*` | logical operator |
| `sql_source_inventory.bzl`, `sql_header_inventory.bzl` | unity-build inventories |

## 3. Consistency model

Local files are mutable outside of the database, so every statement captures a
fingerprint at resolve time:

```
canonical_path, device, inode, file_size, modified_time_ns
```

and both the schema probe and the executor compare it again.  A mismatch is
reported as a retryable schema error instead of mixing two versions of the
file.  `infer_schema()` additionally fingerprints the file *before and after*
the probe, so a rewrite in the middle of inference is detected as well.  A
bounded (64 entries) process-local LRU cache avoids re-probing the same file
for the common planning/execution sequence.

## 4. Format providers

### CSV

* the first record is the header; columns are deduplicated case-insensitively;
* the scanner accepts `,` separators, `\n` and `\r\n` terminators, quoted
  fields with doubled quotes and rejects a quote inside an unquoted field;
* every field is classified (NULL / BOOLEAN / BIGINT / DOUBLE / DATE /
  DATETIME / VARCHAR) and the column type is widened across the file:

| current \ incoming | NULL | BOOLEAN | BIGINT | DOUBLE | DATE | DATETIME | VARCHAR |
|---|---|---|---|---|---|---|---|
| NULL | NULL | BOOLEAN | BIGINT | DOUBLE | DATE | DATETIME | VARCHAR |
| BIGINT | BIGINT | VARCHAR | BIGINT | DOUBLE | VARCHAR | VARCHAR | VARCHAR |
| DOUBLE | DOUBLE | VARCHAR | DOUBLE | DOUBLE | VARCHAR | VARCHAR | VARCHAR |
| DATE | DATE | VARCHAR | VARCHAR | VARCHAR | DATE | DATETIME | VARCHAR |
| DATETIME | DATETIME | VARCHAR | VARCHAR | VARCHAR | DATETIME | DATETIME | VARCHAR |
| VARCHAR | VARCHAR | VARCHAR | VARCHAR | VARCHAR | VARCHAR | VARCHAR | VARCHAR |

(any other pair decays to VARCHAR; the same rule is implemented by
`merge_column_type`).

### JSONL

* one JSON object per line; blank lines are skipped;
* keys are collected in first-appearance order, a key missing from a record
  marks its column nullable;
* nested arrays/objects are rejected rather than flattened;
* scalar numbers, booleans, strings and null are mapped onto the same column
  type set as CSV; date/datetime strings are recognised by shape.

### Parquet

* the footer is decoded with a Thrift compact-protocol cursor;
* only a flat schema with primitive leaves is accepted (BOOLEAN, INT32,
  INT64, INT96, FLOAT, DOUBLE, BYTE_ARRAY, FIXED_LEN_BYTE_ARRAY);
* data pages may be PLAIN or dictionary encoded, definition levels may be RLE
  / bit-packed, compression may be UNCOMPRESSED or SNAPPY;
* DATE / TIMESTAMP logical annotations are mapped onto DATE / DATETIME;
* the page decoder materializes whole row groups and only decodes columns that
  the plan actually projects.

## 5. Limits

* file size ≤ 1 GiB, ≤ 1024 columns, ≤ 16 MiB per JSONL line / CSV field;
* one file per scan (a directory is only enumerated by `glob()`);
* flat Parquet schemas only — nested groups, repeated fields and unsupported
  logical annotations are rejected with `OB_NOT_SUPPORTED`;
* reading is snapshot-free by design; a file that changes under a running scan
  produces `OB_SCHEMA_EAGAIN` and the statement can be retried.

## 6. Attribution and divergence from the reference implementation

The local-file scan feature follows the design published in the
**Apache-2.0** reference implementation `oceanbase/seekdb` PR #1251
(commit `be9369f`).  The reference code was used as a functional contract
while this tree was being brought up; the files listed below have since been
renamed, split, re-commented and partly rewritten:

* `ob_file_scan_common.h` → `ob_local_file_common.h` (types renamed)
* `ob_file_scan_utils.{h,cpp}` → `ob_local_file_reader.{h,cpp}` plus the new
  `ob_local_file_reader_priv.h`, `ob_local_csv_provider.cpp`,
  `ob_local_jsonl_provider.cpp`, `ob_local_file_schema.cpp`
* `ob_parquet_reader.{h,cpp}` → `ob_local_parquet_decoder.{h,cpp}` (internal
  metadata / cursor / page symbols renamed, file reorganized)
* `ob_file_scan_op.{h,cpp}` → `ob_local_file_scan_op.{h,cpp}`
* `ob_log_file_scan.{h,cpp}` → `ob_log_local_file_scan.{h,cpp}`

The Parquet decoder keeps the algorithm of the reference (bit-packed level
decoding, Snappy block decompression, footer Thrift parsing) because the
format is fully specified externally; its implementation has been renamed and
documented here for provenance.  Everything else — the CSV/JSONL scanners,
schema inference, the fingerprint/consistency model and the operator
plumbing — is written in this tree's own style.
