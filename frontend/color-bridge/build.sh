#!/usr/bin/env bash
# P18: color.ll → bridge attrs → P5 Spill/Maps/WB proofs (+ keep P17 green)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
DOCS="$(cd "$ROOT/.." && pwd)"
cd "$ROOT"
OUT="$ROOT/build/p18-out"
mkdir -p "$OUT" "$DOCS/bin"

echo "=== P18: ensure P17 goldens still green ==="
"$DOCS/p17-frontend/build.sh" | tee "$OUT/p17-rerun.log"
rg -q 'PASS 01_ok_outparam_stack' "$OUT/p17-rerun.log"
rg -q 'PASS 06_ok_gptr_only' "$OUT/p17-rerun.log"

echo "=== P18: build bridge + driver ==="
make -C pass all

BRIDGE="$ROOT/build/goc-color-bridge"
DRIVER="$ROOT/build/goc-p18-driver"
test -x "$BRIDGE"
test -x "$DRIVER"

if [[ -x "$DOCS/bin/goc" ]]; then
  cat > "$DOCS/bin/goc-p18" << 'DRV'
#!/usr/bin/env bash
set -euo pipefail
SELF="$(cd "$(dirname "$0")" && pwd)"
exec "$SELF/goc" bridge "$@"
DRV
  chmod +x "$DOCS/bin/goc-p18"
fi

echo "=== P18: bridge fixtures ==="
for f in fixtures/p18_gptr_store.color.ll \
         fixtures/p18_sptr_across_call.color.ll \
         fixtures/p18_cptr_only.color.ll; do
  base="$(basename "$f" .color.ll)"
  "$BRIDGE" "$f" -o "$OUT/${base}.bridged.ll" --recipe "$OUT/${base}.bridge.txt"
  echo "bridged $base"
done

# Also bridge a real P17 golden (gptr-only)
P17_GPTR="$DOCS/p17-frontend/build/test-out/06_ok_gptr_only.color.ll"
if [[ -f "$P17_GPTR" ]]; then
  "$BRIDGE" "$P17_GPTR" -o "$OUT/06_ok_gptr_only.bridged.ll" \
    --recipe "$OUT/06_ok_gptr_only.bridge.txt"
fi

echo "=== P18: verify bridge recipe decisions ==="
rg -q 'need_wb=1' "$OUT/p18_gptr_store.bridge.txt"
rg -q 'need_spill_maps=1' "$OUT/p18_sptr_across_call.bridge.txt"
rg -q 'cptr_only=1' "$OUT/p18_cptr_only.bridge.txt"
rg -q 'need_wb=0' "$OUT/p18_cptr_only.bridge.txt"
# Bridged IR must carry attrs
rg -q 'goc-store-gptr' "$OUT/p18_gptr_store.bridged.ll"
rg -q 'goc-spill-gptrs' "$OUT/p18_sptr_across_call.bridged.ll"
rg -q 'goc-color-cptr-only' "$OUT/p18_cptr_only.bridged.ll"
# cptr must NOT get store-gptr
if rg -q 'goc-store-gptr' "$OUT/p18_cptr_only.bridged.ll"; then
  echo "FAIL: cptr_only should not have goc-store-gptr" >&2
  exit 1
fi
echo "PASS P18-bridge (gptr→WB attrs, sptr→maps attrs, cptr→no WB)"

echo "=== P18: vertical C → P17 → bridge (optional path) ==="
CLANG="${CLANG:-clang-19}"
INCDIR="$DOCS/p17-frontend/include"
COLOR_ESC="$DOCS/p17-frontend/build/goc-color-escape"
for src in tests/p18_ok_gptr_store.c tests/p18_ok_sptr_call.c tests/p18_ok_cptr_only.c; do
  base="$(basename "$src" .c)"
  "$CLANG" -emit-llvm -S -O0 -Xclang -disable-O0-optnone -I "$INCDIR" \
    -o "$OUT/${base}.ll" "$src"
  "$COLOR_ESC" "$OUT/${base}.ll" -o "$OUT/${base}.color.ll"
  "$BRIDGE" "$OUT/${base}.color.ll" -o "$OUT/${base}.bridged.ll" \
    --recipe "$OUT/${base}.bridge.txt"
done
rg -q 'need_wb=1' "$OUT/p18_ok_gptr_store.bridge.txt"
rg -q 'need_spill_maps=1' "$OUT/p18_ok_sptr_call.bridge.txt"
rg -q 'cptr_only=1' "$OUT/p18_ok_cptr_only.bridge.txt"
echo "PASS P18-vertical-fe (C→color→bridge)"

echo "=== P18: machine Spill/Maps/WB via P5 passes ==="
"$DRIVER" --outdir "$OUT/machine" \
  "$OUT/p18_gptr_store.bridged.ll" \
  "$OUT/p18_sptr_across_call.bridged.ll" \
  "$OUT/p18_cptr_only.bridged.ll" \
  | tee "$OUT/machine-driver.log"

rg -q 'PASS-P18-DRIVER' "$OUT/machine-driver.log"
test -f "$OUT/machine/p18.recipe.txt"
test -f "$OUT/machine/p18.mir"

# gptr → WB in recipe/MIR
rg -q 'color_wb=gptr_store|gcWriteBarrier2|wb=gcWriteBarrier2' "$OUT/machine/p18.recipe.txt" \
  || rg -q 'gcWriteBarrier2|CMP32mi|writeBarrier' "$OUT/machine/p18.mir"
rg -q 'fn p18_gptr_store .* wb=1' "$OUT/machine/p18.recipe.txt"
echo "PASS P18-WB (gptr store → ExpandStoreGptr / gcWriteBarrier2)"

# sptr → spill + maps
rg -q 'fn p18_sptr_across_call .* spill_maps=1' "$OUT/machine/p18.recipe.txt"
rg -q 'spill_gptr_vreg|color_spill=1|color_driven=1' "$OUT/machine/p18.recipe.txt"
test -f "$OUT/machine/p18_sptr_across_call/maps.txt"
rg -q 'color_driven 1' "$OUT/machine/p18_sptr_across_call/maps.txt"
rg -q 'locals_source liveintervals' "$OUT/machine/p18_sptr_across_call/maps.txt"
echo "PASS P18-maps (sptr across CALL → Locals maps)"

# cptr → no WB
rg -q 'fn p18_cptr_only .* cptr_only=1' "$OUT/machine/p18.recipe.txt"
rg -q 'fn p18_cptr_only .* wb=0' "$OUT/machine/p18.recipe.txt"
if rg -q 'fn=p18_cptr_only' "$OUT/machine/p18.recipe.txt"; then
  echo "FAIL: WB expand should not target cptr_only" >&2
  exit 1
fi
echo "PASS P18-cptr (no spurious Go WB)"

# Summary PASS lines
{
  echo "P18 results ($(date '+%Y-%m-%d %H:%M %Z'))"
  echo "PASS P18-bridge (gptr→WB attrs, sptr→maps attrs, cptr→no WB)"
  echo "PASS P18-vertical-fe (C→color→bridge)"
  echo "PASS P18-WB (gptr store → ExpandStoreGptr / gcWriteBarrier2)"
  echo "PASS P18-maps (sptr across CALL → Locals maps)"
  echo "PASS P18-cptr (no spurious Go WB)"
  echo "PASS frontend/color-bridge (P17-green + color→maps/WB)"
} | tee "$OUT/PASS_LINES.txt"

echo ""
echo "PASS frontend/color-bridge (P17-green + color→maps/WB)"
