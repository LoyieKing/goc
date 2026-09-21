#!/usr/bin/env python3
"""P14: mircanon is a no-op/identity shim → mirguard.

Dialect transforms deleted; Pass MIR is llc-ready. This file must NOT contain
rewrite helpers. Build FATALS if split_morestack_cfg / inject_frame / rewrite_* return.
"""
from __future__ import annotations

import re
import runpy
import sys
from pathlib import Path

_SRC = Path(__file__).read_text()
for name in (
    "split_morestack_cfg",
    "inject_frame",
    "rewrite_mem_globals",
    "normalize_opcodes",
    "rewrite_body",
    "extract_calls_and_strip",
):
    if re.search(rf"^def {name}\b", _SRC, re.M):
        print(f"mircanon FATAL: def {name} must not exist (P14 identity)", file=sys.stderr)
        sys.exit(1)

guard = Path(__file__).with_name("mirguard.py")
sys.argv[0] = str(guard)
runpy.run_path(str(guard), run_name="__main__")
