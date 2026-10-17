#!/usr/bin/env bash
# Prove the linux/arm64 backend links, and run the ELFs under qemu-aarch64
# when it is installed (ARM64_REQUIRE_QEMU=1 makes a missing qemu a failure).
#
#   1. leaf add, linked from a Go caller
#   2. callit → leaf, the call reloc resolves
#   3. return of &global (ADRP+ADD or ADRP+LDR)
#   4. morestack prologue (ldr from x28, cmp, b.ls), emitted by goc-llc's
#      AArch64 stack-check hook; the .gocobj passes goc-objdump verify
#   5. 9-int Go entry stores x8, llvm-mc assembles it, link if it can
#   6. default amd64 goc build of a tiny C file still works
#   plus: a linux/amd64 host TU is refused; the uptr helpers run on arm64
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}"
# shellcheck source=goversion.sh
source "$ROOT/test/e2e/arm64/goversion.sh"
arm64_skip_old_go "arm64"
export GOC_ROOT="$ROOT"
GOC="$ROOT/cmd/goc"

TMP="$(mktemp -d)"
[[ -n "${ARM64_KEEP:-}" ]] || trap 'rm -rf "$TMP"' EXIT; echo "tmp=$TMP"
pass() { echo "PASS $*"; }
QEMU="$(command -v qemu-aarch64 || command -v qemu-aarch64-static || true)"
if [[ -z "$QEMU" && -n "${ARM64_REQUIRE_QEMU:-}" ]]; then
  echo "FAIL qemu-aarch64 not found" >&2; exit 1
fi
RAN=0
# run_arm64 ELF EXPECTED: run under qemu, compare stdout+stderr.
run_arm64() {
  [[ -n "$QEMU" ]] || return 0
  local got
  got="$(timeout 60 "$QEMU" "$1" 2>&1)" || { echo "$got" >&2; fail "$1 exited non-zero under qemu"; }
  [[ "$got" == "$2" ]] || { printf 'want:\n%s\ngot:\n%s\n' "$2" "$got" >&2; fail "$1 output under qemu"; }
  RAN=$((RAN + 1))
  echo "qemu: $(basename "$1") ok"
}
fail() { echo "FAIL $*" >&2; exit 1; }

# A caller's environment must not leak into a case.
clean_env() {
  env -u GOC_ARCH -u GOC_KEEP_TMP "$@"
}

header_has() {
  python3 - "$1" "$2" <<'PY'
import sys
b = open(sys.argv[1], "rb").read()
needle = sys.argv[2].encode()
i = b.find(b"go object ")
got = b[i:i + 64] if i >= 0 else b[:64]
if needle not in got:
    raise SystemExit("header %r does not contain %r" % (got, sys.argv[2]))
print(got.decode("ascii", "replace"))
PY
}

# The host linker is linux_amd64/link. `go tool link` under GOARCH=arm64 looks
# for a linux_arm64 tool that this machine does not have, and the linker accepts
# exactly one archive. Compile the Go caller with the host compile tool, pack
# the goc goobj into that archive, and invoke the host linker with GOARCH=arm64
# so the goobj header matches the arm64 runtime.
link_arm64() {
  local gosrc="$1" elf="$2"
  shift 2
  local dir="$TMP/link.$$"
  rm -rf "$dir"
  mkdir -p "$dir"
  cp "$gosrc" "$dir/main.go"
  if [[ ! -f "$TMP/importcfg.link" ]]; then
    local hdir="$TMP/hello" work
    mkdir -p "$hdir"
    printf 'package main\nfunc main() { println("hi") }\n' >"$hdir/main.go"
    (
      cd "$hdir"
      go mod init hello >/dev/null
      GOARCH=arm64 go build -x -work -o "$hdir/hi.elf" .
    ) >"$hdir/log" 2>&1 || {
      echo "link: arm64 hello build failed" >&2
      cat "$hdir/log" >&2
      return 1
    }
    work="$(sed -n 's/^WORK=//p' "$hdir/log" | head -1)"
    if [[ -z "$work" || ! -f "$work/b001/importcfg.link" ]]; then
      echo "link: no importcfg.link from $work" >&2
      cat "$hdir/log" >&2
      return 1
    fi
    # Drop the hello package itself; the caller is package main.
    grep -v '^packagefile hello=' "$work/b001/importcfg.link" >"$TMP/importcfg.link"
  fi
  GOARCH=arm64 GOOS=linux go tool compile -p main -o "$dir/main.o" "$dir/main.go"
  cp "$dir/main.o" "$dir/combined.a"
  # The linker skips a short archive member unless its name ends in .o.
  # goc's file on disk is .goobj; the member it loads is .o.
  local obj base stage
  stage="$(mktemp -d)"
  trap 'rm -rf "$stage"' RETURN
  for obj in "$@"; do
    base="$(basename "$obj")"
    case "$base" in
      *.goobj) base="${base%.goobj}.o" ;;
    esac
    cp "$obj" "$stage/$base"
    go tool pack r "$dir/combined.a" "$stage/$base"
  done
  rm -rf "$stage"
  GOARCH=arm64 GOOS=linux GOROOT="$(go env GOROOT)" \
    "$(go env GOTOOLDIR)/link" \
    -o "$elf" -importcfg "$TMP/importcfg.link" -buildmode=exe -extld=gcc \
    "$dir/combined.a"
}

cat >"$TMP/leaf.c" <<'EOF'
int add(int a, int b) { return a + b; }
int callit(int a) { return add(a, 1); }
int glob = 7;
int *aglob(void) { return &glob; }
EOF

cat >"$TMP/caller.go" <<'EOF'
package main

func add(a, b int32) int32
func callit(a int32) int32
func aglob() *int32

func main() {
	println(add(20, 22))
	println(callit(3))
	println(aglob())
}
EOF

echo "== amd64 regression =="
clean_env "$GOC" build "$TMP/leaf.c" -o "$TMP/add_amd64.goobj"
header_has "$TMP/add_amd64.goobj" "linux amd64"
go tool nm "$TMP/add_amd64.goobj" | rg -q 'main\.add' || fail "amd64 object has no main.add"
pass "amd64 goc build (header linux amd64, main.add)"

echo "== arm64 leaf + call + &global =="
clean_env GOC_ARCH=arm64 "$GOC" build "$TMP/leaf.c" -o "$TMP/leaf_arm.goobj"
header_has "$TMP/leaf_arm.goobj" "linux arm64"
go tool nm "$TMP/leaf_arm.goobj" | rg -q 'main\.add' || fail "arm64 object has no main.add"
go tool nm "$TMP/leaf_arm.goobj" | rg -q 'main\.callit' || fail "arm64 object has no main.callit"
go tool nm "$TMP/leaf_arm.goobj" | rg -q 'main\.aglob' || fail "arm64 object has no main.aglob"
link_arm64 "$TMP/caller.go" "$TMP/leaf.elf" "$TMP/leaf_arm.goobj"
GOARCH=arm64 go tool objdump "$TMP/leaf.elf" >"$TMP/leaf.dis"
# Go's objdump prints Plan 9 syntax. The body and the resolved call both
# have to show up; a zero reloc would not name the callee.
rg -q 'TEXT main\.add' "$TMP/leaf.dis" || fail "objdump has no TEXT main.add"
rg -q 'TEXT main\.callit' "$TMP/leaf.dis" || fail "objdump has no TEXT main.callit"
rg -q 'TEXT main\.aglob' "$TMP/leaf.dis" || fail "objdump has no TEXT main.aglob"
rg -n 'main\.(add|callit|aglob)' "$TMP/leaf.dis" | head -40
# callit must refer to add (the CALL26 reloc was applied).
python3 - "$TMP/leaf.dis" <<'PY'
import sys
text = open(sys.argv[1]).read().split("TEXT ")
bodies = {}
for chunk in text[1:]:
    name, _, rest = chunk.partition("\n")
    bodies[name.split("(")[0].strip()] = rest
# The Go entry is the thunk. The C call and the ADRP live in the .impl body.
callit = bodies.get("main.callit.impl", "")
if "main.add" not in callit and "add(" not in callit:
    sys.exit("callit.impl disassembly does not name add:\n" + callit[:800])
aglob = bodies.get("main.aglob.impl", "")
low = aglob.lower()
if "adrp" not in low and "adr " not in low:
    sys.exit("aglob.impl disassembly has no ADRP:\n" + aglob[:800])
print("callit.impl names add; aglob.impl has adrp")
PY
run_arm64 "$TMP/leaf.elf" "$(printf '42\n4\n0x')$(GOARCH=arm64 go tool nm "$TMP/leaf.elf" | awk '$3=="main.glob"{sub(/^0+/,"",$1); print $1}')"
pass "arm64 leaf linked; callit→add resolved; aglob has ADRP"

echo "== arm64 morestack prologue =="
clean_env GOC_ARCH=arm64 \
  "$GOC" build "$TMP/leaf.c" -o "$TMP/ms.goobj"
link_arm64 "$TMP/caller.go" "$TMP/ms.elf" "$TMP/ms.goobj"
GOARCH=arm64 go tool objdump "$TMP/ms.elf" >"$TMP/ms.dis"
python3 - "$TMP/ms.dis" <<'PY'
import sys
text = open(sys.argv[1]).read().split("TEXT ")
body = ""
for chunk in text[1:]:
    name, _, rest = chunk.partition("\n")
    # The C body carries the morestack prologue. The thunk starts with stp.
    if name.split("(")[0].strip() == "main.callit.impl":
        body = rest
        break
head = "\n".join(body.splitlines()[:12])
low = head.lower()
# ldr x16, [x28, #16] / Go syntax references R28 and a compare/branch.
if "r28" not in low and "x28" not in low:
    sys.exit("morestack prologue does not read g (x28/r28):\n" + head)
if "cmp" not in low and "subs" not in low:
    sys.exit("morestack prologue has no compare:\n" + head)
print(head)
PY
# The tables describe that check: goc-objdump verify requires SP_DELTA 0 and
# an unsafe point at pc 0 for every function flagged needs_stack_check.
[[ -f "$TMP/ms.gocobj" ]] || fail "no side .gocobj next to ms.goobj"
( cd "$ROOT" && "${GOC_HOST_GO:-go}" run ./tools/goc-objdump verify "$TMP/ms.gocobj" ) || fail "arm64 .gocobj does not verify"
( cd "$ROOT" && "${GOC_HOST_GO:-go}" run ./tools/goc-objdump dump "$TMP/ms.gocobj" ) | rg -q 'ARM64_CALL26 runtime\.morestack_noctxt' \
  || fail "tables list no morestack call"
run_arm64 "$TMP/ms.elf" "$(printf '42\n4\n0x')$(GOARCH=arm64 go tool nm "$TMP/ms.elf" | awk '$3=="main.glob"{sub(/^0+/,"",$1); print $1}')"
pass "arm64 morestack prologue reads g and compares"

echo "== arm64 goabi 9th integer argument =="
cat >"$TMP/sum9.c" <<'EOF'
long sum9(long a, long b, long c, long d, long e, long f, long g, long h, long i) {
  return a + b + c + d + e + f + g + h + i;
}
EOF
cat >"$TMP/sum9.go" <<'EOF'
package main

func sum9(a, b, c, d, e, f, g, h, i int64) int64

func main() {
	println(sum9(1, 2, 3, 4, 5, 6, 7, 8, 9))
}
EOF
KEEP="$TMP/goabi-tmp"
mkdir -p "$KEEP"
# The 9th Go argument moves through x8 into the AAPCS stack slot.
# Stack maps stay off: arm64 has no frame-address repair.
clean_env GOC_ARCH=arm64 \
  GOC_KEEP_TMP="$KEEP" \
  "$GOC" build "$TMP/sum9.c" -o "$TMP/sum9.goobj"
# The thunk is assembled by llvm-mc inside realbody. The x8 store is the
# 9th Go argument (X8) moving to the AAPCS stack.
thunk="$(find "$KEEP" -name 'thunks.s' -print -quit)"
[[ -n "$thunk" ]] || fail "thunks.s was not kept"
rg -n 'str[[:space:]]+x8' "$thunk" || fail "thunk does not store x8"
echo "thunk store:"
rg -n 'x8|stp|bl' "$thunk"
link_arm64 "$TMP/sum9.go" "$TMP/sum9.elf" "$TMP/sum9.goobj"
# nm of a linked ELF is large. rg -q would SIGPIPE it, and pipefail would
# turn that into a false failure.
GOARCH=arm64 go tool nm "$TMP/sum9.elf" >"$TMP/sum9.nm"
rg -q 'main\.sum9$' "$TMP/sum9.nm" || fail "linked ELF has no main.sum9"
run_arm64 "$TMP/sum9.elf" "45"
pass "arm64 goabi thunk stores x8, assembled, linked"

echo "== refuse an unported amd64 host =="
if clean_env GOC_ARCH=arm64 "$GOC" build "$ROOT/test/e2e/qjs/host/cli_host.c" \
    -o "$TMP/host.goobj" >"$TMP/host.log" 2>&1; then
  fail "arm64 build of the QuickJS host should have been refused"
fi
rg -q 'not ported' "$TMP/host.log" || { cat "$TMP/host.log" >&2; fail "refusal did not say the TU is not ported"; }
pass "arm64 refuses the linux/amd64 QuickJS host"

echo "== arm64 uptr helpers (compiler bodies, x28) =="
# goc-uptr-inline gives every goc_uptr_* / goc_stack_* helper an inline body;
# no runtime object is linked. On arm64 g is x28.
cat >"$TMP/uptr.c" <<'EOF'
#include "goc_uptr.h"

static int global_word = 3;

int uptrcheck(void) {
  int local = 5;
  uintptr_t lo = goc_stack_lo(), hi = goc_stack_hi();
  if (!(lo < (uintptr_t)&local && (uintptr_t)&local < hi))
    return 1;
  goc_uptr u = goc_uptr_from_ptr(&local);
  if (!goc_uptr_is_stack_encoded(u) || goc_uptr_decode(u) != (void *)&local)
    return 2;
  goc_uptr c = goc_uptr_from_ptr(&global_word);
  if ((void *)c != (void *)&global_word || goc_uptr_decode(c) != (void *)&global_word)
    return 3;
  goc_uptr e = goc_uptr_from_sptr((goc_sptr)&local);
  if (goc_uptr_as_sptr(e) != (void *)&local)
    return 4;
  if (goc_uptr_as_cptr(goc_uptr_from_cptr((goc_cptr)&global_word)) != (void *)&global_word)
    return 5;
  return 0;
}
EOF
cat >"$TMP/uptr.go" <<'EOF'
package main

func uptrcheck() int32

func main() {
	println(uptrcheck())
}
EOF
clean_env GOC_ARCH=arm64 "$GOC" build "$TMP/uptr.c" -o "$TMP/uptr.goobj" -ffreestanding
header_has "$TMP/uptr.goobj" "linux arm64"
if go tool nm "$TMP/uptr.goobj" |
    rg -q ' U .*\bgoc_(uptr_(from|as)_[a-z_]+|uptr_decode|uptr_require_cptr|stack_(hi|lo)|dyn(alloc|release))$'; then
  fail "uptr object still references an out-of-line helper"
fi
# The body must read x28, not the amd64 FS load. Go's objdump spells an
# arm64 float-pair store FSTPQ; that is not the segment register.
GOARCH=arm64 go tool objdump "$TMP/uptr.goobj" >"$TMP/uptr.dis" || true
if rg -q 'fs:-8|\bFS\b' "$TMP/uptr.dis"; then
  fail "uptr disassembly still mentions the amd64 FS segment"
fi
if ! rg -qi 'r28|x28' "$TMP/uptr.dis"; then
  echo "uptr disassembly (first 80 lines):" >&2
  head -80 "$TMP/uptr.dis" >&2
  fail "uptr disassembly does not read x28/r28"
fi
link_arm64 "$TMP/uptr.go" "$TMP/uptr.elf" "$TMP/uptr.goobj"
run_arm64 "$TMP/uptr.elf" "0"
pass "arm64 uptr helpers inline, read x28, round-trip on the goroutine stack"

echo "arm64: all cases passed ($RAN ELFs run under qemu)"
