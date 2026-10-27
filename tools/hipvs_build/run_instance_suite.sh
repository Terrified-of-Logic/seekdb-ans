#!/usr/bin/env bash
set -euo pipefail
binary=${1:?seekdb binary}
helper=${2:?hipVS helper}
out=${3:?fresh output root}
here=$(cd -- "$(dirname -- "$0")" && pwd)
python=${PYTHON:-python3}
mkdir "$out"
"$python" "$here/benchmark.py" --binary "$binary" --mode failure --helper /bin/false \
  --out "$out/failure" --rows 1000 --queries 10 --rounds 1 > "$out/failure.log" 2>&1
for size in 20000 50000; do
  ef=512
  if [ "$size" = 50000 ]; then ef=1024; fi
  for mode in cpu gpu; do
    "$python" "$here/benchmark.py" --binary "$binary" --mode "$mode" --helper "$helper" \
      --out "$out/$mode-$size" --rows "$size" --queries 100 --rounds 3 --ef-search "$ef" \
      > "$out/$mode-$size.log" 2>&1
    echo "$mode rows=$size ef_search=$ef PASS"
  done
done
