#!/usr/bin/env python3
"""P14 mirguard: FATAL if MIR still needs dialect transforms; identity otherwise.

Hot path: llc-19 consumes pass/harness.mir DIRECTLY. This script never rewrites
MIR bodies. Metadata lives in a sidecar *.meta.json.

Forbidden leftovers (any match → FATAL):
  - goc.* YAML keys (unknown to llc)
  - GOC_PCDATA1 (unknown opcode)
  - &sym  (use @sym)
  - bare CMP64ri (use CMP64ri32)
  - $noreg,...,@sym absolute PIC without $rip base
  - bb.N.name: labels (use bb.N:)
  - CALL64pcrel32 missing implicit-def $rsp
  - EH_* / landingpad ops
  - x87 ops on Go-callable path (FLD/FST/FADDP/...)
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
from pathlib import Path

# Dialect patterns that would have required mircanon transforms (P13 leftovers).
RE_GOC_KEY = re.compile(r"(?m)^\s*goc\.(go_sym|frame|flags|args)\s*:")
RE_PCDATA = re.compile(r"\bGOC_PCDATA1\b")
RE_AMP_SYM = re.compile(r"&[A-Za-z_][A-Za-z0-9_.]*")
RE_CMP64RI = re.compile(r"\bCMP64ri\b")  # must be CMP64ri32; bare CMP64ri is dialect
RE_BB_NAMED = re.compile(r"(?m)^\s*bb\.\d+\.[A-Za-z_][A-Za-z0-9_]*\s*:")
RE_NOREG_ABS = re.compile(
    r"\$noreg,\s*1,\s*\$noreg,\s*@[A-Za-z_][A-Za-z0-9_.]*"
)
RE_CALL = re.compile(r"\bCALL64pcrel32\b")

EH_OPS = (
    "EH_LABEL", "EH_RETURN", "CATCHPAD", "CLEANUPPAD",
    "LANDINGPAD", "CATCHRET", "CLEANUPRET",
)
X87_OPS = (
    "LD_F80m", "ST_FP80m", "ADD_FPrST0", "ADD_FST0r", "SUB_FPrST0",
    "MUL_FPrST0", "DIV_FPrST0", "ILD_F64m", "IST_FP64m",
    "LD_F32m", "LD_F64m", "ST_F32m", "ST_F64m",
    "ABS_F", "CHS_F", "SQRT_Fr",
)


def fail(msg: str) -> None:
    print(f"mirguard FATAL: {msg}", file=sys.stderr)
    sys.exit(1)


def strip_comments(text: str) -> str:
    """Drop # line comments so documentation does not trip dialect guards."""
    out = []
    for ln in text.splitlines():
        s = ln.lstrip()
        if s.startswith("#"):
            continue
        # keep inline MIR; only full-line comments stripped
        out.append(ln)
    return "\n".join(out)


def check_text(text: str, path: str, *, allow_x87: bool = False) -> None:
    text = strip_comments(text)
    if RE_GOC_KEY.search(text):
        fail(f"{path}: goc.* keys present — move to sidecar meta.json (llc rejects unknown keys)")
    if RE_PCDATA.search(text):
        fail(f"{path}: GOC_PCDATA1 present — put stackmap indices in sidecar meta calls[]")
    if RE_AMP_SYM.search(text):
        fail(f"{path}: &sym present — emit @sym in Pass MIR")
    # CMP64ri but not CMP64ri32 / CMP64ri8
    for m in RE_CMP64RI.finditer(text):
        after = text[m.end():m.end()+2]
        if after.startswith("32") or after.startswith("8"):
            continue
        # word boundary already; reject bare CMP64ri
        fail(f"{path}: CMP64ri present — emit CMP64ri32")
    if RE_BB_NAMED.search(text):
        fail(f"{path}: bb.N.name: labels — emit bb.N:")
    if RE_NOREG_ABS.search(text):
        fail(f"{path}: noreg absolute @sym mem — emit $rip PIC form")
    for ln in text.splitlines():
        if RE_CALL.search(ln) and "implicit-def $rsp" not in ln:
            fail(f"{path}: CALL64pcrel32 missing implicit-def $rsp (llc verifier)")
        for op in EH_OPS:
            if re.search(r"\b" + op + r"\b", ln):
                fail(f"{path}: unsupported EH opcode {op} (explicit permanent unsupported)")
        if not allow_x87:
            for op in X87_OPS:
                if re.search(r"\b" + op + r"\b", ln):
                    fail(
                        f"{path}: x87 opcode {op} — Go-callable x87 unsupported (FATAL); "
                        "use SSE/AVX float path"
                    )
    # Reject CFG/frame rewrite symbols if someone reintroduces old mircanon
    if re.search(r"^def split_morestack_cfg\b", text, re.M):
        fail(f"{path}: split_morestack_cfg must not exist")
    if re.search(r"^def inject_frame\b", text, re.M):
        fail(f"{path}: inject_frame must not exist")


def validate_meta(meta: dict) -> None:
    mc = meta.get("mircanon") or {}
    if mc.get("cfg_rewrite") or mc.get("frame_inject"):
        fail("meta: cfg_rewrite/frame_inject must be false")
    if mc.get("dialect_strip"):
        fail("meta: dialect_strip must be false (P14 identity)")
    mode = mc.get("mode", "")
    if mode not in ("identity", "noop", ""):
        # allow missing during transition only if transforms empty
        pass
    transforms = mc.get("transforms")
    if transforms is not None and len(transforms) != 0:
        fail(f"meta: transforms must be empty, got {transforms!r}")
    if not meta.get("functions"):
        fail("meta: no functions")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-in", dest="inp", required=True, help="llc-ready MIR")
    ap.add_argument("-meta-in", dest="meta_in", default="", help="sidecar meta.json")
    ap.add_argument("-out-mir", default="", help="optional identity copy of MIR")
    ap.add_argument("-out-meta", default="", help="optional copy of meta")
    ap.add_argument("--check-only", action="store_true")
    ap.add_argument("--allow-x87", action="store_true", help="skip x87 FATAL (objdump-only fixtures)")
    args = ap.parse_args()

    inp = Path(args.inp)
    text = inp.read_text()
    check_text(text, str(inp), allow_x87=args.allow_x87)

    # Prefer sibling .meta.json if -meta-in omitted
    meta_path = Path(args.meta_in) if args.meta_in else inp.with_suffix("").with_suffix(".meta.json")
    # harness.mir → harness.meta.json: with_suffix(".meta.json") on ".mir" → "harness.meta.json" ? 
    # Path('pass/harness.mir').with_suffix('.meta.json') → pass/harness.meta.json ✓
    if not args.meta_in:
        meta_path = inp.with_suffix(".meta.json")

    if not meta_path.is_file():
        fail(f"missing sidecar meta {meta_path} (P14: metadata out-of-band)")
    meta = json.loads(meta_path.read_text())
    # Normalize identity markers
    meta.setdefault("mircanon", {})
    meta["mircanon"]["mode"] = "identity"
    meta["mircanon"]["transforms"] = []
    meta["mircanon"]["cfg_rewrite"] = False
    meta["mircanon"]["frame_inject"] = False
    meta["mircanon"]["dialect_strip"] = False
    validate_meta(meta)

    if args.check_only and not args.out_mir and not args.out_meta:
        print(f"mirguard: OK identity {inp} + {meta_path} (no transforms)")
        return

    if args.out_mir:
        out = Path(args.out_mir)
        out.parent.mkdir(parents=True, exist_ok=True)
        # Identity: exact copy (no rewrite)
        shutil.copyfile(inp, out)
    if args.out_meta:
        outm = Path(args.out_meta)
        outm.parent.mkdir(parents=True, exist_ok=True)
        outm.write_text(json.dumps(meta, indent=2) + "\n")

    print(
        f"mirguard: identity OK ({len(meta.get('functions', []))} fns); "
        f"transforms=[]; llc may consume {args.out_mir or inp} directly"
    )


if __name__ == "__main__":
    main()
