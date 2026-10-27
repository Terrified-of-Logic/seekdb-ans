# Evidence provenance

The four CPU/GPU directories and `failure/` contain copied measurement reports,
complete returned-ID lists and exact-oracle ID lists from the verified instance
suite. `summary.json` is a derived aggregate that checks all run states, data and
binary hashes, quality checks, and per-round GPU completion marker counts.

`explain.txt` is a presentation copy with trailing display whitespace removed.
Its original exact SQL output remains in the full SQL transcript archive.
`environment.json` records the actual instance configuration; `manifest.json`
records source/build provenance. `helper-evidence.tar.gz` contains original GPU
kernel/memory-copy CSVs and the helper negative-test outputs.

The complete original server logs, SQL inputs/outputs and raw data are retained
separately as `final-runtime-logs.tar.gz` (not included in this source tree):

SHA-256: `7f1b6e6b98f984d1db47aab495ec931a9cdbde3220ee69d722442b1761293924`.

See `../REPORT.md` for all values, failed earlier configurations and limitations.
