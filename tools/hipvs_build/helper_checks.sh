#!/usr/bin/env bash
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
work=${1:?pass a fresh temporary output directory}
mkdir "$work"
cd "$work"
python "$here/generate_smoke.py"
"$here/hipvs-build-helper" smoke.input smoke.hnsw 2> valid.stderr
head -c 63 smoke.input > truncated.input
if "$here/hipvs-build-helper" truncated.input truncated.hnsw 2> truncated.stderr; then
  echo 'ERROR: truncated input accepted' >&2; exit 1
fi
if ROCR_VISIBLE_DEVICES='' "$here/hipvs-build-helper" smoke.input hidden.hnsw 2> hidden.stderr; then
  echo 'ERROR: hidden GPU accepted' >&2; exit 1
fi
python - <<'PY'
import struct
raw=bytearray(open('smoke.input','rb').read())
raw[48:56]=raw[40:48]
open('duplicate.input','wb').write(raw)
PY
if "$here/hipvs-build-helper" duplicate.input duplicate.hnsw 2> duplicate.stderr; then
  echo 'ERROR: duplicate VID accepted' >&2; exit 1
fi
printf 'valid_gpu=PASS\ntruncated_input=REJECTED\nhidden_gpu=REJECTED\nduplicate_vid=REJECTED\n' > summary.txt
cat summary.txt
