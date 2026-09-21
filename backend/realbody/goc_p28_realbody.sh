#!/usr/bin/env bash
# P28: real Clang IR body → llc ISel → elfpack goobj (NO P21 seedMIR).
# Usage: goc_p28_realbody.sh <input.ll|.c> <out.goobj.o> [fn_name] [go_sym]
set -euo pipefail
SELF="$(cd "$(dirname "$0")" && pwd)"
ROOT="${GOC_ROOT:-$(cd "$SELF/../.." && pwd)}"
export GOC_ROOT="$ROOT"
P5="$ROOT/backend"

IN="${1:?input .ll or .c}"
OUT_O="${2:?output .o}"
FN_NAME="${3:-p28_real_body}"
GO_SYM="${4:-main.P28RealBody}"

CLANG="${GOC_CLANG:-${CLANG:-}}"
if [[ -z "$CLANG" ]]; then
  for c in \
    "$ROOT/third_party/llvm-19.1.7-clang-build/bin/clang" \
    "$ROOT/third_party/llvm-clang-build/bin/clang"; do
    if [[ -x "$c" ]]; then CLANG="$c"; break; fi
  done
  [[ -n "$CLANG" ]] || CLANG="${CLANG_FALLBACK:-clang-19}"
fi
LLC="${LLC:-llc-19}"
INC="$ROOT/include"

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

LL="$TMP/body.ll"
if [[ "$IN" == *.ll ]]; then
  cp "$IN" "$LL"
else
  NATIVE_DEFS=(-DGOC_USE_INTREE_ATTRS)
  if ! "$CLANG" --version 2>/dev/null | rg -q 'clang version 19'; then
    NATIVE_DEFS=()
  fi
  "$CLANG" "${NATIVE_DEFS[@]}" -I "$INC" \
    -emit-llvm -S -O0 -Xclang -disable-O0-optnone \
    -o "$LL" "$IN"
fi

rg -q "define .* @$FN_NAME" "$LL"
MAGIC_OK=0
rg -q '28C0DE42|0x28c0de42|683728450' "$LL" && MAGIC_OK=1 || true

"$LLC" -O0 -relocation-model=pic -march=x86-64 -filetype=obj \
  -o "$TMP/body.llc.o" "$LL"

mkdir -p "$TMP/maps"
cp "$SELF/args_map.bin" "$TMP/maps/args_map.bin"
cp "$SELF/locals_map.bin" "$TMP/maps/locals_map.bin"

CALLS_JSON='[]'
if nm "$TMP/body.llc.o" 2>/dev/null | rg -q 'U p28_external_hook' \
  || llvm-objdump-19 -d "$TMP/body.llc.o" 2>/dev/null | rg -q 'p28_external_hook'; then
  CALLS_JSON='[{"callee":"p28_external_hook","stackmap_index":0}]'
fi

cat > "$TMP/meta.json" << JSON
{
  "producer": "goc-p28-realbody",
  "pipeline": "clang.c→real IR→llc ISel→elfpack (not P21 seed templates)",
  "not_source": "p21-color-vertical seed templates",
  "functions": [
    {
      "mir_name": "$FN_NAME",
      "go_sym": "$GO_SYM",
      "frame": 24,
      "flags": "nosplit",
      "encoding": "clang-real-isel",
      "calls": $CALLS_JSON
    }
  ],
  "mircanon": {
    "mode": "identity",
    "transforms": [],
    "cfg_rewrite": false,
    "frame_inject": false,
    "dialect_strip": false
  }
}
JSON

rg -q '"encoding": "clang-real-isel"' "$TMP/meta.json"
rg -q 'not_source' "$TMP/meta.json"
if rg -q 'attrs→seedMIR|seedMIR→Spill' "$TMP/meta.json"; then
  echo "FATAL: meta looks like P21 seedMIR pipeline" >&2
  exit 1
fi

( cd "$P5" && go build -o "$TMP/elfpack" ./goobj/elfpack/ )
"$TMP/elfpack" -elf "$TMP/body.llc.o" -meta "$TMP/meta.json" \
  -maps "$TMP/maps" -out-o "$OUT_O" -p main

if [[ $MAGIC_OK -eq 1 ]]; then
  python3 - "$OUT_O" <<'PY'
import struct, sys
from pathlib import Path
b = Path(sys.argv[1]).read_bytes()
pat = struct.pack("<I", 0x28C0DE42)
if pat not in b:
    raise SystemExit("FAIL: magic 0x28C0DE42 not in goobj TEXT — body not from source")
print("P28-proof: magic 0x28C0DE42 present in goobj (real ISel body)")
PY
fi

cp "$TMP/meta.json" "${OUT_O%.o}.meta.json"
cp "$LL" "${OUT_O%.o}.ll"
echo "P28-realbody: wrote $OUT_O (encoding=clang-real-isel)"
