#!/usr/bin/env bash
# Prove the linux/arm64 backend links. This does not execute the ELF:
# qemu-aarch64 is not required and is not invoked.
#
#   1. leaf add, linked from a Go caller
#   2. callit → leaf, the call reloc resolves
#   3. return of &global (ADRP+ADD or ADRP+LDR)
#   4. morestack prologue (ldr from x28, cmp, b.ls)
#   5. --goabi 9-int thunk stores x8, llvm-mc assembles it, link if it can
#      (MORESTACK + NO_NOSPLIT; stack maps stay off)
#   6. default amd64 goc build of a tiny C file still works
#   plus: a linux/amd64 host TU is refused; runtime/uptr is compiled for arm64
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}"
export GOC_ROOT="$ROOT"
GOC="$ROOT/cmd/goc"
export PATH="${HOME}/tools/LLVM-19.1.7-Linux-X64/bin:${PATH}"
export LD_LIBRARY_PATH="$(llvm-config-19 --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*" >&2; exit 1; }

# A caller's environment must not leak into a case.
clean_env() {
  env -u GOC_ARCH -u GOC_FRAMEADDR_MODE -u GOC_SPTR_MAPS -u GOC_CSR_ADJUST \
      -u GOC_MORESTACK -u GOC_NO_NOSPLIT -u GOC_CRESERVE -u GOC_KEEP_TMP \
      -u GOC_FIXED_G "$@"
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
  local obj
  for obj in "$@"; do
    go tool pack r "$dir/combined.a" "$obj"
  done
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
clean_env "$GOC" build "$TMP/leaf.c" -o "$TMP/add_amd64.o" --all
header_has "$TMP/add_amd64.o" "linux amd64"
go tool nm "$TMP/add_amd64.o" | rg -q 'main\.add' || fail "amd64 object has no main.add"
pass "amd64 goc build (header linux amd64, main.add)"

echo "== arm64 leaf + call + &global =="
clean_env GOC_ARCH=arm64 "$GOC" build "$TMP/leaf.c" -o "$TMP/leaf_arm.o" --all
header_has "$TMP/leaf_arm.o" "linux arm64"
go tool nm "$TMP/leaf_arm.o" | rg -q 'main\.add' || fail "arm64 object has no main.add"
go tool nm "$TMP/leaf_arm.o" | rg -q 'main\.callit' || fail "arm64 object has no main.callit"
go tool nm "$TMP/leaf_arm.o" | rg -q 'main\.aglob' || fail "arm64 object has no main.aglob"
link_arm64 "$TMP/caller.go" "$TMP/leaf.elf" "$TMP/leaf_arm.o"
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
callit = bodies.get("main.callit", "")
if "main.add" not in callit and "add(" not in callit:
    sys.exit("callit disassembly does not name add:\n" + callit[:800])
aglob = bodies.get("main.aglob", "")
low = aglob.lower()
if "adrp" not in low and "adr " not in low:
    sys.exit("aglob disassembly has no ADRP:\n" + aglob[:800])
print("callit names add; aglob has adrp")
PY
pass "arm64 leaf linked; callit→add resolved; aglob has ADRP"

echo "== arm64 morestack prologue =="
clean_env GOC_ARCH=arm64 GOC_MORESTACK=1 GOC_NO_NOSPLIT=1 \
  "$GOC" build "$TMP/leaf.c" -o "$TMP/ms.o" --all
link_arm64 "$TMP/caller.go" "$TMP/ms.elf" "$TMP/ms.o"
GOARCH=arm64 go tool objdump "$TMP/ms.elf" >"$TMP/ms.dis"
python3 - "$TMP/ms.dis" <<'PY'
import sys
text = open(sys.argv[1]).read().split("TEXT ")
body = ""
for chunk in text[1:]:
    name, _, rest = chunk.partition("\n")
    if name.split("(")[0].strip() == "main.callit":
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
# ABIInternal register arguments need the split stub (same as the amd64
# goabi build). Stack maps stay off: arm64 has no frame-address repair.
clean_env GOC_ARCH=arm64 GOC_CRESERVE=16 GOC_MORESTACK=1 GOC_NO_NOSPLIT=1 \
  GOC_KEEP_TMP="$KEEP" \
  "$GOC" build "$TMP/sum9.c" -o "$TMP/sum9.o" --all --goabi
# The thunk is assembled by llvm-mc inside realbody. The x8 store is the
# 9th Go argument (X8) moving to the AAPCS stack.
thunk="$(find "$KEEP" -name 'thunks.s' -print -quit)"
[[ -n "$thunk" ]] || fail "thunks.s was not kept"
rg -n 'str[[:space:]]+x8' "$thunk" || fail "thunk does not store x8"
echo "thunk store:"
rg -n 'x8|stp|bl' "$thunk"
link_arm64 "$TMP/sum9.go" "$TMP/sum9.elf" "$TMP/sum9.o"
# nm of a linked ELF is large. rg -q would SIGPIPE it, and pipefail would
# turn that into a false failure.
GOARCH=arm64 go tool nm "$TMP/sum9.elf" >"$TMP/sum9.nm"
rg -q 'main\.sum9$' "$TMP/sum9.nm" || fail "linked ELF has no main.sum9"
pass "arm64 goabi thunk stores x8, assembled, linked"

echo "== refuse an unported amd64 host =="
if clean_env GOC_ARCH=arm64 "$GOC" build "$ROOT/tests/qjs/_qjs_cli_host.c" \
    -o "$TMP/host.o" --all >"$TMP/host.log" 2>&1; then
  fail "arm64 build of the QuickJS host should have been refused"
fi
rg -q 'not ported' "$TMP/host.log" || { cat "$TMP/host.log" >&2; fail "refusal did not say the TU is not ported"; }
pass "arm64 refuses the linux/amd64 QuickJS host"

echo "== runtime/uptr for aarch64 =="
# -ffreestanding keeps clang on its own stdint/stddef. The aarch64 target
# has no glibc multiarch headers on this amd64 host, and this TU does not
# use libc.
clean_env GOC_ARCH=arm64 "$GOC" build "$ROOT/runtime/uptr/goc_uptr_runtime.c" \
  -o "$TMP/uptr.o" --all -ffreestanding \
  -DGOC_UPTR_FREESTANDING -DGOC_UPTR_HAVE_TLS -I"$ROOT/runtime/uptr"
header_has "$TMP/uptr.o" "linux arm64"
go tool nm "$TMP/uptr.o" | rg -q 'goc_runtime_getg' || fail "uptr object has no goc_runtime_getg"
# The body must read x28, not the amd64 FS load. Check the kept assembly via objdump of the goobj text.
GOARCH=arm64 go tool objdump "$TMP/uptr.o" >"$TMP/uptr.dis" || true
if rg -q 'fs:-8|FS' "$TMP/uptr.dis"; then
  fail "uptr disassembly still mentions FS"
fi
if ! rg -qi 'r28|x28' "$TMP/uptr.dis"; then
  echo "uptr disassembly (first 80 lines):" >&2
  head -80 "$TMP/uptr.dis" >&2
  fail "uptr disassembly does not read x28/r28"
fi
pass "runtime/uptr arm64 reads x28"

echo "check_arm64: all cases passed (not executed under qemu)"
