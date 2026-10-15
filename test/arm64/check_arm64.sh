#!/usr/bin/env bash
# linux/arm64 status check.
#
# The table-driven pipeline (goc-llc emits the stack check and the .goc.*
# tables; gocobj2goobj only converts tables) is amd64-only for now. arm64 needs
# an AArch64 stack-check hook in goc-llc (llvm/lib/Target/AArch64), AArch64
# relocations in GocObjTables and gocobj2goobj, and AAPCS64 thunk tables. Until
# then this script proves:
#   1. the default amd64 build of a tiny C file works
#   2. GOC_ARCH=arm64 is refused with a clear message, not miscompiled
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}"
export GOC_ROOT="$ROOT"
GOC="$ROOT/cmd/goc"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*" >&2; exit 1; }

# A caller's environment must not leak into a case.
clean_env() {
  env -u GOC_ARCH -u GOC_FRAMEADDR_MODE -u GOC_CSR_ADJUST -u GOC_KEEP_TMP \
      -u GOC_FIXED_G "$@"
}

cat >"$TMP/leaf.c" <<'C'
int add(int a, int b) { return a + b; }
C

echo "== amd64 regression =="
clean_env "$GOC" build "$TMP/leaf.c" -o "$TMP/add_amd64.goobj"
head -c 64 "$TMP/add_amd64.goobj" | grep -aq "linux amd64" || fail "amd64 header"
go tool nm "$TMP/add_amd64.goobj" | rg -q 'main\.add' || fail "amd64 object has no main.add"
pass "amd64 goc build (header linux amd64, main.add)"

echo "== arm64 refused =="
if clean_env GOC_ARCH=arm64 "$GOC" build "$TMP/leaf.c" -o "$TMP/leaf_arm.goobj" \
    >"$TMP/arm.log" 2>&1; then
  fail "GOC_ARCH=arm64 build succeeded; the AArch64 hook is not implemented"
fi
rg -q 'not supported' "$TMP/arm.log" || { cat "$TMP/arm.log" >&2; fail "arm64 refusal message"; }
[[ ! -s "$TMP/leaf_arm.goobj" ]] || fail "arm64 left an object"
pass "arm64 refused (AArch64 stack-check hook pending)"
