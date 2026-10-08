#!/usr/bin/env bash
# GOC_COLOR_REPORT=1 lists resolved pointer colors without changing the IR.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
PASS="$ROOT/frontend/color-escape/build/goc-color-escape"
SRC="$ROOT/frontend/color-escape/tests/color_report.c"
CLANG="${CLANG:-clang-19}"
export LD_LIBRARY_PATH="$(llvm-config-19 --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [[ ! -x "$PASS" ]]; then
  make -C "$ROOT/frontend/color-escape/pass"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

"$CLANG" -g -emit-llvm -S -O0 -Xclang -disable-O0-optnone \
  -I "$ROOT/include" -o "$TMP/in.ll" "$SRC"
"$CLANG" -emit-llvm -S -O0 -Xclang -disable-O0-optnone \
  -I "$ROOT/include" -o "$TMP/nog.ll" "$SRC"

"$PASS" "$TMP/in.ll" -o "$TMP/off.ll" >"$TMP/off.log" 2>&1
GOC_COLOR_REPORT=1 "$PASS" "$TMP/in.ll" -o "$TMP/on.ll" >"$TMP/on.log" 2>&1
GOC_COLOR_REPORT=1 "$PASS" "$TMP/in.ll" -o "$TMP/def.ll" \
  -default-ptr-color=cptr >"$TMP/def.log" 2>&1
"$PASS" "$TMP/in.ll" -o "$TMP/def-off.ll" \
  -default-ptr-color=cptr >"$TMP/def-off.log" 2>&1
GOC_COLOR_REPORT=1 "$PASS" "$TMP/nog.ll" -o "$TMP/nog-on.ll" >"$TMP/nog.log" 2>&1
"$PASS" "$TMP/nog.ll" -o "$TMP/nog-off.ll" >"$TMP/nog-off.log" 2>&1

diff -q "$TMP/off.ll" "$TMP/on.ll"
diff -q "$TMP/def-off.ll" "$TMP/def.ll"
diff -q "$TMP/nog-off.ll" "$TMP/nog-on.ll"

python3 - "$TMP/off.log" "$TMP/on.log" "$TMP/def.log" "$TMP/nog.log" << 'PY'
import sys

def rows(path):
    out = []
    for ln in open(path):
        if not ln.startswith("goc-color-report"):
            continue
        parts = ln.rstrip("\n").split("\t")
        if len(parts) != 7:
            raise SystemExit(f"{path}: expected 7 columns, got {parts}")
        out.append(parts)
    return out

off, on, default, nog = (rows(p) for p in sys.argv[1:])
if off:
    raise SystemExit("report printed while GOC_COLOR_REPORT is unset")
if not on or on[0] != ["goc-color-report", "kind", "name", "loc", "color", "source", "spelling"]:
    raise SystemExit(f"missing header: {on[:1]}")

def body(rs):
    return { (r[1], r[2]): r for r in rs[1:] }

got = body(on)
# loc is file:line or file:line:col. Match the declaration line.
expect = {
    ("field", "Node.bare"): ("sptr", "inferred", "T*", ":5"),
    ("field", "Node.cheap"): ("uptr", "inferred", "cptr", ":6"),
    ("field", "Node.enc"): ("uptr", "explicit", "uptr", ":7"),
    ("field", "Node.gp"): ("gptr", "explicit", "gptr", ":8"),
    ("field", "Node.ap"): ("sptr", "inferred", "auto_ptr", ":9"),
    ("global", "g_bare"): ("uptr", "inferred", "T*", ":18"),
    ("global", "g_explicit"): ("cptr", "explicit", "cptr", ":19"),
    ("param", "only_stack.out"): ("sptr", "inferred", "T*", ":22"),
    ("return", "ret_bare"): ("cptr", "inferred", "T*", ":24"),
    ("param", "ret_bare.p"): ("auto", "inferred", "T*", ":24"),
    ("return", "ret_cptr"): ("cptr", "explicit", "cptr", ":29"),
    ("param", "ret_cptr.p"): ("auto", "inferred", "T*", ":29"),
    ("local", "store_all.bare_local"): ("sptr", "inferred", "T*", ":36"),
    ("local", "store_all.sp"): ("sptr", "explicit", "sptr", ":37"),
    ("local", "store_all.ap"): ("sptr", "inferred", "auto_ptr", ":38"),
}
missing = []
for key, (color, source, spelling, line) in expect.items():
    row = got.get(key)
    if not row:
        missing.append(key)
        continue
    if (row[4], row[5], row[6]) != (color, source, spelling) or line not in row[3]:
        raise SystemExit(f"{key}: got {row[3:]} want {color} {source} {spelling} {line}")
if missing:
    raise SystemExit(f"missing rows: {missing}")
for key in got:
    if "hidden" in key[1] or key[1].startswith("Hide"):
        raise SystemExit(f"union field listed: {key}")

dgot = body(default)
for key in (("param", "ret_bare.p"), ("param", "ret_cptr.p")):
    row = dgot[key]
    if (row[4], row[5], row[6]) != ("cptr", "default-ptr-color", "T*"):
        raise SystemExit(f"default {key}: {row}")
# A proven stack formal and an explicit color stay put under the default.
for key, color, source, spelling in (
    (("param", "only_stack.out"), "sptr", "inferred", "T*"),
    (("local", "store_all.sp"), "sptr", "explicit", "sptr"),
    (("local", "store_all.bare_local"), "sptr", "inferred", "T*"),
    (("field", "Node.enc"), "uptr", "explicit", "uptr"),
    (("field", "Node.cheap"), "uptr", "inferred", "cptr"),
    (("global", "g_explicit"), "cptr", "explicit", "cptr"),
):
    row = dgot[key]
    if (row[4], row[5], row[6]) != (color, source, spelling):
        raise SystemExit(f"default kept {key}: got {row[4:]} want {color} {source} {spelling}")

# Without debug info the same colors are still listed, under IR names.
nb = body(nog)
if ("param", "only_stack.#0") not in nb or nb[("param", "only_stack.#0")][4] != "sptr":
    raise SystemExit(f"no-debug param missing: {list(nb)}")
if ("field", "Node.#1") not in nb or nb[("field", "Node.#1")][4:] != ["uptr", "inferred", "cptr"]:
    raise SystemExit("no-debug field color mismatch")
print("PASS color report")
PY
