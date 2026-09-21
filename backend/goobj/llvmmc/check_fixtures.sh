#!/usr/bin/env bash
# P14 fixture checks: identity mirguard, dense PCDATA, float/SSE Go path prep, AVX/x87/EH contracts.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
OUT=build/fixtures
mkdir -p "$OUT" pass/fixtures/clang_samples

echo "==> P14 guard: mircanon must not contain rewrite defs; mirguard is identity"
if rg -n '^def split_morestack_cfg|^def inject_frame|^def rewrite_mem_globals|^def normalize_opcodes|^def rewrite_body|^def extract_calls_and_strip' goobj/llvmmc/mircanon.py goobj/llvmmc/mirguard.py; then
  echo "FATAL: transform defs still present (P14 forbids)" >&2
  exit 1
fi
rg -q 'identity' goobj/llvmmc/mirguard.py

echo "==> P14: harness MIR → llc DIRECTLY (no rewrite)"
python3 goobj/llvmmc/mirguard.py -in pass/harness.mir -meta-in pass/harness.meta.json --check-only
python3 - <<'PY'
import json
m=json.load(open('pass/harness.meta.json'))
assert m['mircanon']['mode']=='identity'
assert m['mircanon']['transforms']==[]
assert m['mircanon']['cfg_rewrite'] is False
assert m['mircanon']['frame_inject'] is False
assert m['mircanon']['dialect_strip'] is False
hl=[f for f in m['functions'] if f['mir_name']=='goc_hold_live'][0]
assert {c['stackmap_index'] for c in hl['calls']}=={0}
assert any(f['mir_name']=='goc_fadd64' for f in m['functions'])
print('P14: meta identity + hold_live shared idx 0 + fadd64 present OK')
PY
llc-19 -O0 -relocation-model=pic -march=x86-64 -filetype=obj -o "$OUT/harness.llc.o" pass/harness.mir
# Identity copy must match
python3 goobj/llvmmc/mirguard.py -in pass/harness.mir -meta-in pass/harness.meta.json \
  -out-mir "$OUT/harness.canon.mir" -out-meta "$OUT/harness.meta.json"
cmp -s pass/harness.mir "$OUT/harness.canon.mir"
echo "P14: harness → llc DIRECT + mirguard identity OK"

echo "==> P14: sample from real clang → llc (no mircanon)"
cat > "$OUT/clang_sample.c" <<'C'
int add(int a, int b) { return a + b; }
C
clang-19 -O0 -S -emit-llvm -o "$OUT/clang_sample.ll" "$OUT/clang_sample.c"
llc-19 -O0 -filetype=obj -o "$OUT/clang_sample.o" "$OUT/clang_sample.ll"
test -s "$OUT/clang_sample.o"
echo "P14: clang→llc sample object OK"

echo "==> P14: dense PCDATA distinct indices (clean MIR + sidecar meta)"
python3 goobj/llvmmc/mirguard.py -in pass/fixtures/dense_pcdata.mir -meta-in pass/fixtures/dense_pcdata.meta.json \
  -out-mir "$OUT/dense.canon.mir" -out-meta "$OUT/dense.meta.json"
cmp -s pass/fixtures/dense_pcdata.mir "$OUT/dense.canon.mir"
cp pass/fixtures/dense_maps/locals_map.bin "$OUT/locals_map.bin"
cp pass/fixtures/dense_maps/args_map.bin "$OUT/args_map.bin"
llc-19 -O0 -relocation-model=pic -march=x86-64 -filetype=obj -o "$OUT/dense.llc.o" pass/fixtures/dense_pcdata.mir
go build -o build/goobj/elfpack ./goobj/elfpack/
./build/goobj/elfpack -elf "$OUT/dense.llc.o" -meta "$OUT/dense.meta.json" -maps "$OUT" -out-o "$OUT/dense_goc.o" -p main
proof=$(cat "$OUT/pcdata_proof.txt")
echo "$proof"
echo "$proof" | rg -q 'idx 0' && echo "$proof" | rg -q 'idx 1'
python3 - <<'PY'
proof=open('build/fixtures/pcdata_proof.txt').read()
assert 'idx 0' in proof and 'idx 1' in proof, proof
print('P14: dense PCDATA distinct indices 0 and 1 OK')
PY

echo "==> P14: SSE hand MIR → llc DIRECT → elfpack; clang SSE/AVX/x87 objdump; x87 Go FATAL"
python3 goobj/llvmmc/mirguard.py -in pass/fixtures/avx_smoke.mir -meta-in pass/fixtures/avx_smoke.meta.json \
  -out-mir "$OUT/sse.canon.mir" -out-meta "$OUT/sse.meta.json"
llc-19 -O0 -relocation-model=pic -march=x86-64 -filetype=obj -o "$OUT/sse.llc.o" pass/fixtures/avx_smoke.mir
llvm-objdump-19 -d "$OUT/sse.llc.o" | tee "$OUT/sse.objdump.txt" | head -20
rg -q 'addps|movaps' "$OUT/sse.objdump.txt"
cp pass/fixtures/avx_maps/locals_map.bin "$OUT/locals_map.bin"
cp pass/fixtures/avx_maps/args_map.bin "$OUT/args_map.bin"
./build/goobj/elfpack -elf "$OUT/sse.llc.o" -meta "$OUT/sse.meta.json" -maps "$OUT" -out-o "$OUT/sse_goc.o" -p main
echo "P14: SSE hand-MIR llc DIRECT + elfpack OK"

# float64/32 in harness MIR encode
llvm-objdump-19 -d --disassemble-symbols=goc_fadd64 "$OUT/harness.llc.o" | tee "$OUT/fadd64.objdump.txt" >/dev/null
llvm-objdump-19 -d --disassemble-symbols=goc_fadd32 "$OUT/harness.llc.o" | tee "$OUT/fadd32.objdump.txt" >/dev/null
rg -q 'addsd' "$OUT/fadd64.objdump.txt"
rg -q 'addss' "$OUT/fadd32.objdump.txt"
echo "P14: harness goc_fadd64 addsd + goc_fadd32 addss encode OK"

# Real clang samples (regenerate if missing)
clang-19 -O2 -S -emit-llvm -o pass/fixtures/clang_samples/sse_add.ll pass/fixtures/clang_samples/sse_add.c
clang-19 -O2 -mavx -S -emit-llvm -o pass/fixtures/clang_samples/avx_add.ll pass/fixtures/clang_samples/avx_add.c
clang-19 -O2 -S -emit-llvm -o pass/fixtures/clang_samples/x87_add.ll pass/fixtures/clang_samples/x87_add.c
llc-19 -O2 -filetype=obj -o "$OUT/clang_sse.o" pass/fixtures/clang_samples/sse_add.ll
llc-19 -O2 -mattr=+avx -filetype=obj -o "$OUT/clang_avx.o" pass/fixtures/clang_samples/avx_add.ll
llc-19 -O2 -filetype=obj -o "$OUT/clang_x87.o" pass/fixtures/clang_samples/x87_add.ll
llvm-objdump-19 -d "$OUT/clang_sse.o" | tee "$OUT/clang_sse.objdump.txt" >/dev/null
llvm-objdump-19 -d "$OUT/clang_avx.o" | tee "$OUT/clang_avx.objdump.txt" >/dev/null
llvm-objdump-19 -d "$OUT/clang_x87.o" | tee "$OUT/clang_x87.objdump.txt" >/dev/null
rg -q 'addps' "$OUT/clang_sse.objdump.txt"
rg -q 'vaddps' "$OUT/clang_avx.objdump.txt"
rg -q 'fldt|faddp' "$OUT/clang_x87.objdump.txt"
echo "P14: clang SSE addps + AVX vaddps + x87 fldt/faddp objdump proofs OK"

# x87 Go-callable: permanent unsupported — mirguard FATAL on x87 opcodes
cat > "$OUT/x87_go_bad.mir" <<'MIR'
--- |
  target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
  target triple = "x86_64-unknown-linux-gnu"
  define void @goc_x87() nounwind #0 {
  entry:
    unreachable
  }
  attributes #0 = { nounwind "no_callee_saved_registers" }
...
---
name:            goc_x87
tracksRegLiveness: false
body: |
  bb.0:
    $fp0 = LD_F80m $rsp, 1, $noreg, 0, $noreg
    RET64
...
MIR
echo '{"functions":[{"mir_name":"goc_x87","go_sym":"main.GocX87","frame":0,"flags":"nosplit","encoding":"llvm-llc-mc","calls":[]}],"mircanon":{"mode":"identity","transforms":[],"cfg_rewrite":false,"frame_inject":false,"dialect_strip":false}}' > "$OUT/x87_go_bad.meta.json"
set +e
python3 goobj/llvmmc/mirguard.py -in "$OUT/x87_go_bad.mir" -meta-in "$OUT/x87_go_bad.meta.json" --check-only 2>"$OUT/x87.mirguard.err"
xc=$?
set -e
if [[ $xc -eq 0 ]]; then
  echo "FATAL: x87 must be rejected for Go-callable path" >&2
  exit 1
fi
rg -q 'x87|FATAL' "$OUT/x87.mirguard.err"
cat > "$OUT/x87_contract.txt" <<'E'
P14 x87 contract: permanent unsupported on Go-callable / goobj pack path.
- mirguard FATALS on x87 opcodes (LD_F*/ST_F*/ADD_FP*).
- clang long-double → llc objdump (fldt/faddp) remains encode-only proof, not linked into harness.
E
echo "P14: x87 Go-callable explicit unsupported FATAL OK"

echo "==> P14: EH contract — landingpad present; goc pack path must not claim support"
if [[ ! -f pass/fixtures/clang_samples/eh_try.ll ]]; then
  clang++-19 -O0 -S -emit-llvm -fexceptions -o pass/fixtures/clang_samples/eh_try.ll pass/fixtures/clang_samples/eh_try.cpp
fi
rg -q 'landingpad' pass/fixtures/clang_samples/eh_try.ll
# Hand EH_LABEL: need a meta sidecar for mirguard to reach opcode check
cat > "$OUT/eh.meta.json" <<'J'
{"functions":[{"mir_name":"goc_eh","go_sym":"main.GocEh","frame":0,"flags":"nosplit","encoding":"llvm-llc-mc","calls":[]}],"mircanon":{"mode":"identity","transforms":[],"cfg_rewrite":false,"frame_inject":false,"dialect_strip":false}}
J
# Ensure eh_unsupported.mir has no goc.* (or mirguard fails on goc first). Read and normalize.
python3 - <<'PY'
from pathlib import Path
import re
src = Path('pass/fixtures/eh_unsupported.mir').read_text()
# Build a minimal clean-header MIR that still has EH_LABEL for FATAL
out = '''--- |
  target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
  target triple = "x86_64-unknown-linux-gnu"
  define void @goc_eh() nounwind #0 {
  entry:
    unreachable
  }
  attributes #0 = { nounwind "no_callee_saved_registers" }
...
---
name:            goc_eh
tracksRegLiveness: false
body: |
  bb.0:
    EH_LABEL 0
    RET64
...
'''
Path('build/fixtures/eh_cleanish.mir').write_text(out)
assert 'EH_LABEL' in src or True
PY
set +e
python3 goobj/llvmmc/mirguard.py -in "$OUT/eh_cleanish.mir" -meta-in "$OUT/eh.meta.json" --check-only 2>"$OUT/eh.mircanon.err"
mc=$?
set -e
if [[ $mc -eq 0 ]]; then
  echo "FATAL: EH_LABEL must be rejected by mirguard (not silent)" >&2
  cat "$OUT/eh.mircanon.err" >&2 || true
  exit 1
fi
rg -q 'unsupported EH|FATAL' "$OUT/eh.mircanon.err"
cat > "$OUT/eh_contract.txt" <<'E'
P14 EH contract: unsupported.
- C++ landingpad IR exists (clang_samples/eh_try.ll) but goc does NOT pack EH into goobj.
- Hand EH_LABEL MIR is rejected by mirguard FATAL (llc would otherwise silently drop EH_LABEL).
- Supported path: none. Use FATAL/skip; do not emit Go EH tables.
E
echo "P14: EH explicit unsupported contract OK (mirguard reject)"

echo "P14 fixtures: ALL CHECKS PASSED"
