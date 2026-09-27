#!/usr/bin/env bash
# Build-matrix benchmark for docs/perf-gap.md: the same QuickJS source built
# several ways, timed with scripts/bench-all.sh's pinned, interleaved method.
# Native binaries come from scripts/perfgap-build-natives.sh.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
PS="${PS:-/workspace/perf-study}"
OUT="${OUT:-$ROOT/docs/perf-gap/data/raw}"
mkdir -p "$OUT"
cat > "$OUT/engines.txt" <<MAP
ng-gcc-O2 $PS/native/b-gcc-O2/qjs -C --stack-size 16384
ng-clang-O2 $PS/native/b-O2/qjs -C --stack-size 16384
ng-clang-O3 $PS/native/b-O3/qjs -C --stack-size 16384
ng-clang-O3-gocflags $PS/native/b-gocfull/qjs -C --stack-size 16384
goc-ng $ROOT/build/qjs/qjscli --stack-size 16384
bellard-gcc-O2 $PS/bellard/quickjs-2026-06-04/qjs --stack-size 16M
bellard-gcc-O2-NDEBUG $PS/bellard/gcc-O2-NDEBUG/qjs --stack-size 16M
bellard-clang-O2 $PS/bellard/clang-O2/qjs --stack-size 16M
bellard-clang-O2-NDEBUG $PS/bellard/clang-O2-NDEBUG/qjs --stack-size 16M
bellard-clang-O3 $PS/bellard/clang-O3-NDEBUG/qjs --stack-size 16M
bellard-clang-O3-gocflags $PS/bellard/clang-O3-gocfull/qjs --stack-size 16M
goc-bellard $ROOT/build/qjs-bellard/qjscli --stack-size 16384
MAP
ENGINES="$(cut -d' ' -f1 "$OUT/engines.txt" | tr '\n' ' ')" \
ENGINE_MAP="$OUT/engines.txt" MICROCALL_PER_ENGINE=1 SKIP="${SKIP:-t262 qjs}" OUT="$OUT" \
  "$ROOT/scripts/bench-all.sh"
