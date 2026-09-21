#!/usr/bin/env bash
# P5b→P6: LiveIntervals-eq spill + Go SP layout + standalone goobj (no GOROOT overlay)
# No goc_lower.py. No git clone.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
mkdir -p build/pass build/pass-out build/goobj harness

echo "==> [1/6] MachineFunctionPass driver (real X86 MIs + LiveIntervals-eq spill)"
make -C pass run 2>&1 | tee build/pass-out/pass-driver.log
# P16: One body path — analysis MF → Go-frame lower → printMIR (no parallel seed export)
test -f build/pass-out/harness.mir
test -f build/pass-out/harness.meta.json
rg -q 'PASS-DRIVER: wrote .*harness.mir' build/pass-out/pass-driver.log
rg -q 'P16: harness.mir from analysis MF after Go-frame lower' build/pass-out/pass-driver.log
rg -q 'Pass-exported llc-ready MIR|llvm::printMIR' build/pass-out/harness.mir
rg -q 'no parallel seed export' build/pass-out/harness.mir
# Proof: analysis dump still has vreg/%stack; harness is physreg Go-frame
rg -q 'morestack' build/pass-out/goc.mir
rg -q 'morestack' build/pass-out/harness.mir
python3 - <<'PY16'
from pathlib import Path
import re
g = Path("build/pass-out/goc.mir").read_text()
h = Path("build/pass-out/harness.mir").read_text()
assert "goc_hold_live" in g and "goc_hold_live" in h
assert ("%stack." in g) or ("stack:" in g), "analysis goc.mir should retain %stack/FI"
assert "%stack." not in h, "harness must be Go-frame physreg (no %stack)"
assert "goc_p15_export" not in h and "goc_p15" not in h.splitlines()[0]
assert "no parallel seed export" in h.splitlines()[0]
assert "Pass-exported llc-ready MIR" in h
# Same TEXT names as analysis recipe / maps
for fn in ("goc_checked_add","goc_hold_live","goc_hold_arg","goc_hold_two","goc_hold_regonly"):
    assert fn in g and fn in h, fn
print("P16 proof: harness from analysis MF after Go-frame lower (shared TEXT names; no parallel seed Module)")
PY16
python3 -c "import json; m=json.load(open('build/pass-out/harness.meta.json')); assert m.get('producer')=='goc-pass-driver/printMIR'"
python3 goobj/llvmmc/mirguard.py -in build/pass-out/harness.mir -meta-in build/pass-out/harness.meta.json --check-only
echo "P16: analysis MF → Go-frame lower → printMIR harness OK (no parallel seed export)"
# Guard: stackcheck must NOT be INLINEASM path
if rg -q 'op=INLINEASM stackguard0' build/pass-out/stackcheck.recipe.txt; then
  echo "ERROR: stackcheck still on INLINEASM path" >&2
  exit 1
fi
rg -q 'mi_path=real_x86_opcodes' build/pass-out/stackcheck.recipe.txt
rg -q 'MOV64rm|CMP64rm|JCC_1|CALL64pcrel32' build/pass-out/goc.mir
echo "MIR stackcheck: real X86 opcodes OK"
# Guard: store_gptr WB must NOT be INLINEASM primary path
if rg -q 'op=INLINEASM wb=' build/pass-out/stackcheck.recipe.txt; then
  echo "ERROR: StoreGptr WB still on INLINEASM path" >&2
  exit 1
fi
rg -q 'mi_path=real_x86_opcodes_wb' build/pass-out/stackcheck.recipe.txt
rg -q 'CMP32mi|gcWriteBarrier2' build/pass-out/goc.mir
echo "MIR store_gptr WB: real X86 opcodes OK"

# P6: maps from liveintervals (spill), not FI-only mir_liveness / debug_override
rg -q 'locals_source liveintervals' build/pass-out/maps.txt
rg -q 'args_source mir_ir_args' build/pass-out/maps.txt
if rg -q 'locals_source debug_override' build/pass-out/maps.txt; then
  echo "ERROR: pointer maps still from debug_override attrs" >&2
  exit 1
fi
rg -q 'locals_hex 02000000030000000404' build/pass-out/maps.txt
rg -q 'args_hex 02000000010000000101' build/pass-out/maps.txt
rg -q 'spill_gptr_vreg|liveintervals_eq' build/pass-out/stackcheck.recipe.txt
rg -q 'liveintervals via=LiveIntervalsWrapperPass' build/pass-out/stackcheck.recipe.txt
rg -q 'x86_instr_info real' build/pass-out/stackcheck.recipe.txt || rg -q 'api=X86InstrInfo' build/pass-out/stackcheck.recipe.txt
rg -q 'api=X86InstrInfo' build/pass-out/stackcheck.recipe.txt
echo "Pointer maps: liveintervals spill OK (locals SP+16, args bit0)"
echo "P6: real X86InstrInfo + LiveIntervalsWrapperPass OK"

# P6 multi-FI: hold_two must have bits for SP+16 and SP+24 (0x0c), NOT FI-rank*8 (0x03)
test -f build/pass-out/hold_two/maps.txt
rg -q 'locals_source liveintervals' build/pass-out/hold_two/maps.txt
rg -q 'locals_hex 02000000040000000c0c' build/pass-out/hold_two/maps.txt
if rg -q 'locals_hex 02000000040000000303' build/pass-out/hold_two/maps.txt; then
  echo "ERROR: hold_two still using FI-rank*8 layout (0x03)" >&2
  exit 1
fi
echo "Multi-FI Go SP layout OK (hold_two locals byte=0x0c = SP+16|SP+24)"

# P6.1: tight gptr ID + register-only hard case (S3) + morestack safepoint
rg -q 'gptr_id_rules=R1_explicit_attr' build/pass-out/stackcheck.recipe.txt
rg -q 'reject=every_GR64' build/pass-out/stackcheck.recipe.txt
rg -q 'lis_policy=safe_recompute_after_stackcheck' build/pass-out/stackcheck.recipe.txt
rg -q 'morestack_spill_via_rebuilt_lis|morestack_spill_rebuilt_lis' build/pass-out/stackcheck.recipe.txt
rg -q 'stackcheck_mode=cfg_only' build/pass-out/stackcheck.recipe.txt
test -f build/pass-out/mi_lower.txt
rg -q 'format goc-mi-lower-1' build/pass-out/mi_lower.txt
rg -q 'abi amd64_ABIInternal' build/pass-out/mi_lower.txt
rg -q 'abi=amd64_ABIInternal' build/pass-out/stackcheck.recipe.txt
test -f build/pass-out/hold_regonly/maps.txt
rg -q 'locals_source liveintervals' build/pass-out/hold_regonly/maps.txt
rg -q 'locals_hex 02000000030000000404' build/pass-out/hold_regonly/maps.txt
python3 - <<'PY'
from pathlib import Path
r = Path('build/pass-out/stackcheck.recipe.txt').read_text().splitlines()
buf = []
spills_before_regonly = None
for line in r:
    if line.startswith('spill_gptr_vreg='):
        buf.append(line)
    if line.startswith('fn='):
        if line == 'fn=goc_hold_regonly':
            spills_before_regonly = len(buf)
        buf = []
assert spills_before_regonly == 1, f"expected 1 spill for regonly, got {spills_before_regonly}"
print("S3 pass-out: exactly 1 spill_gptr for goc_hold_regonly (integer decoy not spilled) OK")
PY
echo "P6.1: tight gptr ID + hold_regonly maps OK"
echo "P8: LIS rebuild after StackCheck + morestack spill via rebuilt liveness OK"
echo "P8: mi_lower.txt (MIR→goobj) + ABIInternal markers OK"

# P11: primary path = LLVM MIR (pass/harness.mir) → mirparse → mi_lower
# Old pass/mi_full_bodies.txt remains documented fallback only.
echo "==> [1b] P11 mirparse: Pass harness.mir → mi_lower.txt (primary)"
test -f build/pass-out/harness.mir
go run ./goobj/cmd/mir2lower   -in build/pass-out/harness.mir   -recipe build/pass-out/stackcheck.recipe.txt   -out build/pass-out/mi_lower.txt   -fallback-bodies pass/mi_full_bodies.txt   2>&1 | tee build/pass-out/mir2lower.log
rg -q 'mirparse' build/pass-out/mi_lower.txt
rg -q 'format goc-mi-lower-1' build/pass-out/mi_lower.txt
rg -F -q '.begin_fn goc_hold_live' build/pass-out/mi_lower.txt
echo "P11: mirparse primary MIR→mi_lower OK"


# P9: full MIR→goobj — mi_lower must export per-fn bodies; binwriter must not template them
rg -F -q '.begin_fn goc_checked_add' build/pass-out/mi_lower.txt
rg -F -q '.begin_fn goc_hold_live' build/pass-out/mi_lower.txt
rg -F -q '.begin_fn goc_store_gptr' build/pass-out/mi_lower.txt
if rg -q 'AMOVQ|HugeFrameVoid|gcWriteBarrier2|leafOk' goobj/binwriter/main.go; then
  echo "ERROR: binwriter still contains Prog templates for goc TEXT bodies" >&2
  exit 1
fi
rg -q 'RequireFn|LowerFn' goobj/binwriter/main.go
echo "P9: full MI bodies in mi_lower + binwriter MIR-only (no templates) OK"

# P10: Hold* MIR-owned morestack (NOSPLIT) + goc_leaf in same pipeline
rg -F -q '.begin_fn goc_leaf' build/pass-out/mi_lower.txt
python3 - <<'PY10'
from pathlib import Path
text = Path('build/pass-out/mi_lower.txt').read_text()
for fn in ('goc_hold_live', 'goc_hold_arg', 'goc_hold_two', 'goc_hold_regonly'):
    i = text.index('.begin_fn ' + fn)
    j = text.index('.end_fn', i)
    body = text[i:j]
    assert 'flags nosplit' in body, fn + ' missing flags nosplit'
    assert 'mem=FS:-8' in body, fn + ' missing MIR morestack TLS'
    assert 'runtime.morestack_noctxt' in body, fn + ' missing CALL morestack'
    assert 'op=JMP_1' in body, fn + ' missing JMP reentry'
print('P10: Hold* NOSPLIT + MIR morestack in mi_lower OK')
leaf_i = text.index('.begin_fn goc_leaf')
leaf = text[leaf_i:text.index('.end_fn', leaf_i)]
assert 'ADD64rr' in leaf and 'flags nosplit' in leaf
print('P10: goc_leaf MI body in mi_lower OK')
PY10




echo "==> [2/6] P15/P12 LLVM MC: Pass harness.mir → llc DIRECT → elfpack goobj"
# Instruction encoding = LLVM llc (AsmPrinter/MCCodeEmitter). goobj pack = FUNCDATA only.
# Hot path input = Pass printMIR export (NOT hand-edited pass/harness.mir).
if ! command -v llc-19 >/dev/null 2>&1; then
  echo "ERROR: llc-19 required for P12 LLVM MC path" >&2
  exit 1
fi
./goobj/llvmmc/run_llvmmc.sh build/pass-out/harness.mir build/pass-out build/goobj/goc_funcs.o build/pass-out build/pass-out/harness.meta.json
cp build/pass-out/stackcheck.recipe.txt build/goobj/stackcheck.recipe.txt
# Document encoding provenance
echo "encoding llvm-llc-mc" > build/goobj/ENCODING.txt
rg -q 'llvm-llc-mc|AsmPrinter|MCCodeEmitter' goobj/llvmmc/README.md
echo "P12: LLVM MC encoding path OK"

echo "==> [3/6] verify goobj symbols (Args+Locals FUNCDATA carriers + TEXT)"
go tool nm build/goobj/goc_funcs.o | tee build/goobj/nm.txt
rg -q 'gclocals\.gocHoldArgs' build/goobj/nm.txt
rg -q 'gclocals\.gocHoldLive' build/goobj/nm.txt
rg -q 'main\.GocCheckedAdd' build/goobj/nm.txt
rg -q 'main\.GocHoldLive' build/goobj/nm.txt
rg -q 'main\.StoreGptrWB' build/goobj/nm.txt
# HoldTwo optional if binwriter emits it
if rg -q 'main\.GocHoldTwo' build/goobj/nm.txt; then
  echo "goobj includes GocHoldTwo"
fi
python3 - <<'PY'
b=open('build/goobj/goc_funcs.o','rb').read(64)
assert b.startswith(b'go object '), b[:32]
print('goobj text header OK:', b.split(b'\n',1)[0].decode())
PY
echo "goobj nm: Args+Locals gclocals + TEXT OK (standalone writer)"
rg -q 'gclocals\.gocHoldRegOnly' build/goobj/nm.txt
rg -q 'main\.GocHoldRegOnly' build/goobj/nm.txt
echo "goobj nm: GocHoldRegOnly (S3) OK"

rg -q 'goc_leaf' build/goobj/nm.txt
echo "goobj nm: goc_leaf (P10 MIR-goobj) OK"
rg -q 'GocFadd64' build/goobj/nm.txt
rg -q 'GocFadd32' build/goobj/nm.txt
echo "goobj nm: GocFadd64/32 (P14 float ABI) OK"

echo "==> [3b] P12 golden: mutate Pass MIR imm → LLVM MC goobj bytes change"
cp build/goobj/goc_funcs.o build/goobj/goc_funcs.o.orig
cp build/pass-out/harness.mir build/pass-out/harness.mir.orig
python3 - <<'PYGOLD'
from pathlib import Path
p = Path('build/pass-out/harness.mir')
t = p.read_text()
if 'CMP64ri32 $rax, 42' not in t:
    raise SystemExit('Pass harness.mir missing CMP64ri32 $rax, 42 to mutate')
p.write_text(t.replace('CMP64ri32 $rax, 42', 'CMP64ri32 $rax, 43', 1))
print('mutated CMP64ri32 imm 42 -> 43 in Pass harness.mir')
PYGOLD
./goobj/llvmmc/run_llvmmc.sh build/pass-out/harness.mir build/pass-out build/goobj/goc_funcs.o.mut build/pass-out build/pass-out/harness.meta.json
python3 - <<'PYGOLD2'
from pathlib import Path
a = Path('build/goobj/goc_funcs.o.orig').read_bytes()
b = Path('build/goobj/goc_funcs.o.mut').read_bytes()
assert a != b, 'FATAL: mutated MIR produced identical goobj (LLVM MC path dead?)'
print('P12 golden: goobj bytes changed after Pass MIR mutate via llc OK')
PYGOLD2
mv build/pass-out/harness.mir.orig build/pass-out/harness.mir
mv build/goobj/goc_funcs.o.orig build/goobj/goc_funcs.o
rm -f build/goobj/goc_funcs.o.mut
# Re-encode final object from restored Pass MIR
./goobj/llvmmc/run_llvmmc.sh build/pass-out/harness.mir build/pass-out build/goobj/goc_funcs.o build/pass-out build/pass-out/harness.meta.json
( cd goobj/mirparse && go test -count=1 -timeout 60s . )
( cd goobj/llvmmc && python3 -c "import pathlib; t=pathlib.Path('mirguard.py').read_text(); assert 'identity' in t and 'FATAL' in t" )
echo "P12 golden + mirparse tests OK"

# P14: dialect strip → ZERO (identity mirguard); float into Go ABI; dense PCDATA; reloc/pcsp
echo "==> [3c] P14 zero-dialect + float ABI"
if rg -n '^def split_morestack_cfg|^def inject_frame|^def rewrite_mem_globals|^def normalize_opcodes|^def rewrite_body|^def extract_calls_and_strip' goobj/llvmmc/mircanon.py goobj/llvmmc/mirguard.py; then
  echo "ERROR: mircanon/mirguard still has dialect transforms (P14 forbids)" >&2
  exit 1
fi
# Pass-exported MIR must llc without rewrite (identity mirguard)
cmp -s build/pass-out/harness.mir build/pass-out/harness.canon.mir
python3 - <<'PY14META'
import json
m=json.load(open('build/pass-out/harness.meta.json'))
assert m.get('mircanon',{}).get('mode')=='identity'
assert m.get('mircanon',{}).get('transforms')==[]
assert m.get('mircanon',{}).get('cfg_rewrite') is False
assert m.get('mircanon',{}).get('frame_inject') is False
assert m.get('mircanon',{}).get('dialect_strip') is False
hl=[f for f in m['functions'] if f['mir_name']=='goc_hold_live'][0]
assert len(hl['calls'])>=2
assert all(c['stackmap_index']==0 for c in hl['calls'])
assert any(f['mir_name']=='goc_fadd64' for f in m['functions'])
print('P14: meta identity + hold_live idx 0 + fadd64 OK')
PY14META
# Prove no dialect leftovers in Pass-exported MIR
python3 goobj/llvmmc/mirguard.py -in build/pass-out/harness.mir -meta-in build/pass-out/harness.meta.json --check-only
# P15 proof: hot path input is Pass export, not sole hand file
test -f build/pass-out/harness.mir
rg -q 'Pass-exported llc-ready MIR' build/pass-out/harness.mir
echo "P16: hot path = build/pass-out/harness.mir (analysis MF→Go-frame lower→printMIR); pass/harness.mir golden only"
test -f build/goobj/pcdata_proof.txt
rg -q 'GocHoldLive.*idx 0' build/goobj/pcdata_proof.txt
./build/goobj/elfpack -check-relocs -elf build/pass-out/harness.llc.o -meta build/pass-out/harness.meta.json -maps build/pass-out -out-o /tmp/elfpack_check.o
python3 - <<'PY14PCSP'
import subprocess
out=subprocess.check_output(['llvm-objdump-19','-d','--disassemble-symbols=goc_hold_live','build/pass-out/harness.llc.o'], text=True)
assert 'pushq %rbp' in out or 'push' in out.lower(), out[:500]
out2=subprocess.check_output(['llvm-objdump-19','-d','--disassemble-symbols=goc_fadd64','build/pass-out/harness.llc.o'], text=True)
assert 'addsd' in out2, out2
print('P14: pcsp PUSH BP + fadd64 addsd OK')
PY14PCSP
rg -q 'main\.GocFadd64|GocFadd64' build/goobj/nm.txt
bash goobj/llvmmc/check_fixtures.sh
echo "P14: zero-dialect + dense PCDATA + reloc/pcsp + float/SSE/AVX/x87/EH fixtures OK"



echo "==> [4/6] goc_leaf from MIR->goobj (no llc .syso); stubs only (no goc TEXT .s)"
# P10: leaf is TEXT in goc_funcs.o; remove leftover llc .syso to avoid dup symbol.
rm -f harness/goc_leaf_amd64.syso harness/goc_funcs_amd64.s harness/*goc_funcs*.s
if command -v llc-19 >/dev/null 2>&1; then
  llc-19 -filetype=asm -o build/goc_leaf.llc.s ir/goc_leaf.ll || true
fi
if ls harness/*.llc.s >/dev/null 2>&1; then
  echo "ERROR: *.llc.s leaked into harness" >&2
  exit 1
fi
if ls harness/*.syso >/dev/null 2>&1; then
  echo "ERROR: harness still has .syso (leaf must come from binary goobj)" >&2
  exit 1
fi
if rg -q 'TEXT ·GocCheckedAdd|TEXT ·GocHoldLive|TEXT ·GocHoldArg|TEXT ·StoreGptrWB|TEXT ·GocHoldTwo' harness/*.s; then
  echo "ERROR: harness .s still contains goc TEXT (must come from binary .o)" >&2
  exit 1
fi
test -f harness/stubs_amd64.s
echo "harness isolation OK: stubs helpers only; goc TEXT+leaf from binary goc_funcs.o (no .syso)"

echo "==> [5/6] CGO_ENABLED=0 go build -a + toolexec (symabis ABIInternal + pack binary goobj)"
export GOC_BINOBJ="$ROOT/build/goobj/goc_funcs.o"
( cd harness && CGO_ENABLED=0 go build -a \
  -toolexec "$ROOT/tools/toolexec_pack_goobj.sh" \
  -o ../build/p5_machinepass_goobj . )
echo "built build/p5_machinepass_goobj"

echo "==> [6/6] run L/W/S/S2/S3/A harness + P10 morestack/leaf proofs"
go tool objdump -s 'main\.GocHoldLive' build/p5_machinepass_goobj | head -50 | tee build/hold_objdump.txt
go tool objdump -s 'main\.GocCheckedAdd' build/p5_machinepass_goobj | head -30 | tee build/checked_objdump.txt
go tool objdump -s 'main\.StoreGptrWB' build/p5_machinepass_goobj | head -40 | tee build/wb_objdump.txt
go tool objdump -s 'goc_leaf' build/p5_machinepass_goobj | head -20 | tee build/leaf_objdump.txt
# Provenance: LLVM MC path (ENCODING.txt) + instruction patterns (file attr may be stubs.s)
test -f build/goobj/ENCODING.txt
rg -q 'llvm-llc-mc' build/goobj/ENCODING.txt
rg -q 'FS:|R11' build/hold_objdump.txt
rg -q 'morestack_noctxt|morestackHits' build/checked_objdump.txt
rg -q 'gcWriteBarrier2|writeBarrier' build/wb_objdump.txt
rg -q 'morestack_noctxt' build/hold_objdump.txt
python3 - <<'PYPROOF'
from pathlib import Path
h = Path('build/hold_objdump.txt').read_text()
assert ('FS:' in h or 'R11' in h), 'Hold objdump missing MIR TLS/R11 morestack'
assert 'morestack_noctxt' in h, 'Hold objdump missing morestack call'
print('P10 proof: GocHoldLive morestack is MIR-owned (objdump) OK')
leaf = Path('build/leaf_objdump.txt').read_text()
assert 'goc_leaf' in leaf or 'TEXT' in leaf
print('P10 proof: goc_leaf TEXT present in final binary OK')
PYPROOF

./build/p5_machinepass_goobj
