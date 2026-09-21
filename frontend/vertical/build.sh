#!/usr/bin/env bash
# P21: color.ll → bridge → seed MIR (not harness.mir) → Spill/Maps/WB →
#      Go-frame printMIR → llc → elfpack goobj
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
DOCS="$(cd "$ROOT/.." && pwd)"
cd "$ROOT"
OUT="$ROOT/build/p21-out"
P5="$DOCS/p5-machinepass-goobj"
mkdir -p "$OUT" "$DOCS/bin" "$DOCS/p21-archive"

LLC="${LLC:-llc-19}"
CLANG="${CLANG:-clang-19}"
BRIDGE_BIN="$DOCS/p18-color-bridge/build/goc-color-bridge"
COLOR_ESC="$DOCS/p17-frontend/build/goc-color-escape"

echo "=== P21: ensure P18 bridge binary ==="
if [[ ! -x "$BRIDGE_BIN" ]]; then
  make -C "$DOCS/p18-color-bridge/pass" all
fi
test -x "$BRIDGE_BIN"

echo "=== P21: build vertical driver ==="
make -C pass all
DRIVER="$ROOT/build/goc-p21-vertical"
test -x "$DRIVER"

echo "=== P21: bridge color fixtures ==="
for f in fixtures/p21_gptr_store.color.ll fixtures/p21_sptr_across_call.color.ll; do
  base="$(basename "$f" .color.ll)"
  "$BRIDGE_BIN" "$f" -o "$OUT/${base}.bridged.ll" --recipe "$OUT/${base}.bridge.txt"
done
rg -q 'need_wb=1' "$OUT/p21_gptr_store.bridge.txt"
rg -q 'need_spill_maps=1' "$OUT/p21_sptr_across_call.bridge.txt"
rg -q 'goc-store-gptr' "$OUT/p21_gptr_store.bridged.ll"
rg -q 'goc-spill-gptrs' "$OUT/p21_sptr_across_call.bridged.ll"
echo "PASS P21-bridge (gptr→WB attrs, sptr→maps attrs)"

echo "=== P21: C → color → bridge (frontend vertical) ==="
INCDIR="$DOCS/p17-frontend/include"
if [[ ! -x "$COLOR_ESC" ]]; then
  make -C "$DOCS/p17-frontend/pass" all
fi
for src in tests/p21_ok_gptr_store.c tests/p21_ok_sptr_call.c; do
  base="$(basename "$src" .c)"
  "$CLANG" -emit-llvm -S -O0 -Xclang -disable-O0-optnone -I "$INCDIR" \
    -o "$OUT/${base}.ll" "$src"
  "$COLOR_ESC" "$OUT/${base}.ll" -o "$OUT/${base}.color.ll"
  "$BRIDGE_BIN" "$OUT/${base}.color.ll" -o "$OUT/${base}.bridged.ll" \
    --recipe "$OUT/${base}.bridge.txt"
done
rg -q 'need_wb=1' "$OUT/p21_ok_gptr_store.bridge.txt"
rg -q 'need_spill_maps=1' "$OUT/p21_ok_sptr_call.bridge.txt"
echo "PASS P21-vertical-fe (C→color→bridge)"

echo "=== P21: machine vertical (seed≠harness.mir) → printMIR ==="
"$DRIVER" --outdir "$OUT/machine" \
  "$OUT/p21_gptr_store.bridged.ll" \
  "$OUT/p21_sptr_across_call.bridged.ll" \
  | tee "$OUT/machine-driver.log"

rg -q 'PASS-P21-VERTICAL' "$OUT/machine-driver.log"
test -f "$OUT/machine/vertical.mir"
test -f "$OUT/machine/vertical.meta.json"
test -f "$OUT/machine/p21.recipe.txt"
test -f "$OUT/machine/p21.analysis.mir"

# Must NOT be the P5 harness body catalog
if rg -q 'goc_checked_add|goc_hold_live|goc_store_gptr' "$OUT/machine/vertical.mir"; then
  echo "FAIL: vertical.mir must not contain P5 harness TEXT names" >&2
  exit 1
fi
rg -q 'name:            p21_gptr_store' "$OUT/machine/vertical.mir"
rg -q 'name:            p21_sptr_across_call' "$OUT/machine/vertical.mir"
rg -q 'not_source.: .p5-machinepass-goobj/pass/harness.mir' "$OUT/machine/vertical.meta.json" \
  || rg -q 'pass/harness.mir' "$OUT/machine/vertical.meta.json"
rg -q 'color_fp' "$OUT/machine/vertical.meta.json"
rg -q 'gcWriteBarrier2|runtime.gcWriteBarrier2' "$OUT/machine/vertical.mir"
rg -q 'external_safepoint' "$OUT/machine/vertical.mir"

# Color fingerprints in recipe / analysis
rg -q 'color_wb=gptr_store|wb=gcWriteBarrier2|color_driven=1' "$OUT/machine/p21.recipe.txt"
rg -q 'fn p21_gptr_store .*wb=1' "$OUT/machine/p21.recipe.txt"
rg -q 'fn p21_sptr_across_call .*spill_maps=1' "$OUT/machine/p21.recipe.txt"
rg -q 'gcWriteBarrier2|color_wb' "$OUT/machine/p21.analysis.mir"
test -f "$OUT/machine/p21_sptr_across_call/maps.txt"
rg -q 'color_driven 1' "$OUT/machine/p21_sptr_across_call/maps.txt"
rg -q 'locals_source liveintervals' "$OUT/machine/p21_sptr_across_call/maps.txt"
echo "PASS P21-vertical-mir (color-seeded → Spill/Maps/WB → printMIR; ≠harness.mir)"

echo "=== P21: mirguard + llc + elfpack goobj ==="
# Root maps required by elfpack mustRead — copy color-driven sptr maps
cp "$OUT/machine/p21_sptr_across_call/args_map.bin" "$OUT/machine/args_map.bin"
cp "$OUT/machine/p21_sptr_across_call/locals_map.bin" "$OUT/machine/locals_map.bin"

python3 "$P5/goobj/llvmmc/mirguard.py" \
  -in "$OUT/machine/vertical.mir" \
  -meta-in "$OUT/machine/vertical.meta.json" \
  -out-mir "$OUT/machine/vertical.canon.mir" \
  -out-meta "$OUT/machine/vertical.meta.json"
if ! cmp -s "$OUT/machine/vertical.mir" "$OUT/machine/vertical.canon.mir"; then
  echo "FAIL: mirguard rewrote vertical.mir" >&2
  diff -u "$OUT/machine/vertical.mir" "$OUT/machine/vertical.canon.mir" | head -40 >&2 || true
  exit 1
fi

"$LLC" -O0 -relocation-model=pic -march=x86-64 -filetype=obj \
  -o "$OUT/machine/vertical.llc.o" "$OUT/machine/vertical.mir"

( cd "$P5" && go build -o "$OUT/elfpack" ./goobj/elfpack/ )
"$OUT/elfpack" \
  -elf "$OUT/machine/vertical.llc.o" \
  -meta "$OUT/machine/vertical.meta.json" \
  -maps "$OUT/machine" \
  -out-o "$OUT/p21_funcs.o" \
  -p main

test -f "$OUT/p21_funcs.o"
go tool nm "$OUT/p21_funcs.o" | tee "$OUT/nm.txt"
rg -q 'main\.P21GptrStoreWB' "$OUT/nm.txt"
rg -q 'main\.P21SptrAcrossCall' "$OUT/nm.txt"
rg -q 'gclocals\.p21SptrLocals' "$OUT/nm.txt"
rg -q 'gclocals\.p21SptrArgs' "$OUT/nm.txt"
# Must not claim P5 harness symbols as the color vertical product
if rg -q 'main\.GocHoldLive|main\.StoreGptrWB|main\.GocCheckedAdd' "$OUT/nm.txt"; then
  echo "FAIL: p21 goobj must not be the P5 harness object" >&2
  exit 1
fi
echo "PASS P21-goobj (TEXT+maps from color vertical; nm fingerprints)"

# Fingerprint linkage: bridge → recipe → meta → goobj
python3 - <<'PY'
from pathlib import Path
out = Path("build/p21-out")
bridge_g = (out / "p21_gptr_store.bridge.txt").read_text()
bridge_s = (out / "p21_sptr_across_call.bridge.txt").read_text()
recipe = (out / "machine/p21.recipe.txt").read_text()
meta = (out / "machine/vertical.meta.json").read_text()
nm = (out / "nm.txt").read_text()
assert "need_wb=1" in bridge_g
assert "need_spill_maps=1" in bridge_s
assert "wb=1" in recipe and "p21_gptr_store" in recipe
assert "spill_maps=1" in recipe and "p21_sptr_across_call" in recipe
assert "color_fp" in meta and "p21_gptr_store" in meta
assert "P21GptrStoreWB" in nm and "P21SptrAcrossCall" in nm
assert "p21SptrLocals" in nm
print("P21 fingerprint chain: bridge→recipe→meta→nm OK")
PY
echo "PASS P21-fingerprint (bridge→recipe→meta→goobj nm)"

{
  echo "P21 results ($(date '+%Y-%m-%d %H:%M %Z'))"
  echo "PASS P21-bridge (gptr→WB attrs, sptr→maps attrs)"
  echo "PASS P21-vertical-fe (C→color→bridge)"
  echo "PASS P21-vertical-mir (color-seeded → Spill/Maps/WB → printMIR; ≠harness.mir)"
  echo "PASS P21-goobj (TEXT+maps from color vertical; nm fingerprints)"
  echo "PASS P21-fingerprint (bridge→recipe→meta→goobj nm)"
  echo "PASS frontend/vertical (color.ll→goobj proven slice)"
} | tee "$OUT/PASS_LINES.txt" | tee "$DOCS/p21-archive/PASS-LINES.txt"

echo ""
echo "PASS frontend/vertical (color.ll→goobj proven slice)"
echo "Artifact: $OUT/p21_funcs.o"
