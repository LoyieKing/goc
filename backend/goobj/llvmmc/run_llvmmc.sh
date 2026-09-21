#!/usr/bin/env bash
# P15: MIR → LLVM MC (llc) → goobj pack. Instruction encoding is LLVM's.
# Hot path: Pass printMIR → build/pass-out/harness.mir; llc DIRECT (mirguard identity).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
IN_MIR="${1:-build/pass-out/harness.mir}"
OUT_DIR="${2:-build/pass-out}"
OUT_O="${3:-build/goobj/goc_funcs.o}"
MAPS="${4:-build/pass-out}"
META_IN="${5:-}"
mkdir -p "$OUT_DIR" "$(dirname "$OUT_O")"

LLC="${LLC:-llc-19}"
command -v "$LLC" >/dev/null || { echo "FATAL: $LLC not found (need LLVM 19 llc)"; exit 1; }

# Resolve sidecar meta next to MIR when not passed
if [[ -z "$META_IN" ]]; then
  if [[ -f "${IN_MIR%.mir}.meta.json" ]]; then
    META_IN="${IN_MIR%.mir}.meta.json"
  elif [[ -f pass/harness.meta.json && "$IN_MIR" == *harness.mir ]]; then
    META_IN=pass/harness.meta.json
  else
    echo "FATAL: no sidecar meta for $IN_MIR (expected ${IN_MIR%.mir}.meta.json)" >&2
    exit 1
  fi
fi

echo "llvmmc: mirguard identity check $IN_MIR + $META_IN"
python3 goobj/llvmmc/mirguard.py \
  -in "$IN_MIR" \
  -meta-in "$META_IN" \
  -out-mir "$OUT_DIR/harness.canon.mir" \
  -out-meta "$OUT_DIR/harness.meta.json"

# Prove identity: out MIR must match input
if ! cmp -s "$IN_MIR" "$OUT_DIR/harness.canon.mir"; then
  echo "FATAL: mirguard rewrote MIR (P14 requires identity/no-op)" >&2
  diff -u "$IN_MIR" "$OUT_DIR/harness.canon.mir" | head -40 >&2 || true
  exit 1
fi

echo "llvmmc: $LLC DIRECT on $IN_MIR (X86 AsmPrinter / MCCodeEmitter) → ELF"
"$LLC" -O0 -relocation-model=pic -march=x86-64 -filetype=obj \
  -o "$OUT_DIR/harness.llc.o" "$IN_MIR"

echo "llvmmc: elfpack ELF → goobj (+ FUNCDATA maps)"
go build -o build/goobj/elfpack ./goobj/elfpack/
./build/goobj/elfpack \
  -elf "$OUT_DIR/harness.llc.o" \
  -meta "$OUT_DIR/harness.meta.json" \
  -maps "$MAPS" \
  -out-o "$OUT_O" \
  -p main

echo "llvmmc: OK encoding=llvm-llc-mc (direct llc, mircanon identity) → $OUT_O"
