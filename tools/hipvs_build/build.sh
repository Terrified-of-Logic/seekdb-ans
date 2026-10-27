#!/usr/bin/env bash
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
hipvs=${HIPVS_HOME:-/opt/hipvs}
"${HIPCC:-/opt/rocm/bin/hipcc}" -x hip -std=c++17 -O3 \
  "$here/build_helper.cpp" -I"$hipvs/include" -I"$hipvs/include/cuvs" \
  -L"$hipvs/lib" -Wl,-rpath,"$hipvs/lib" -lcuvs_c -o "$here/hipvs-build-helper"
