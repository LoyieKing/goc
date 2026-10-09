#!/usr/bin/env bash
# Pack a complete linux/amd64 goc release: the driver, the patched clang,
# opt/llc/llvm-mc/llvm-objdump, the passes, and prebuilt elfpack and goc-lower.
# Usage: scripts/pack-release.sh DEST
# DEST is a directory, or a path ending in .tar / .tar.gz.
set -euo pipefail
SELF="$(cd "$(dirname "$0")" && pwd)"
ROOT="${GOC_ROOT:-$(cd "$SELF/.." && pwd)}"
export GOC_ROOT="$ROOT"
# shellcheck source=goc-product-lib.sh
source "$SELF/goc-product-lib.sh"

dest="${1:?pack-release: need DEST}"
ver="$(tr -d '[:space:]' <"$ROOT/VERSION")"
name="goc-${ver}-linux-amd64"

clang="$(goc_resolve_clang)"
goc_export_tools
[[ -x "$LLC" ]] || goc_die "goc-llc missing ($LLC). Run scripts/build-passes.sh"
[[ -x "${GOC_COLOR_ESCAPE:-}" ]] || goc_die "goc-color-escape missing. Run scripts/build-passes.sh"
[[ -f "$GOC_STACKMAP" ]] || goc_die "GocStackMap.so missing ($GOC_STACKMAP)"
[[ -n "${OPT:-}" && -x "$OPT" ]] || goc_die "opt missing. Run scripts/build-clang.sh"

copy_tool() {
  local name="$1" src="${2:-}"
  if [[ -z "$src" ]]; then
    src="$(goc_beside "$name" 2>/dev/null || true)"
  fi
  [[ -n "$src" && -e "$src" ]] || goc_die "missing $name (run scripts/build-clang.sh)"
  cp -a "$(readlink -f "$src")" "$stage/bin/$name"
  chmod a+rx "$stage/bin/$name"
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
stage="$work/$name"
mkdir -p "$stage/bin" "$stage/lib" "$stage/passes" "$stage/cmd" \
  "$stage/scripts" "$stage/include" "$stage/examples/hello" \
  "$stage/backend/realbody" "$stage/backend/tools"

cp -a "$ROOT/cmd/goc" "$stage/cmd/goc"
chmod a+rx "$stage/cmd/goc"
ln -sfn ../cmd/goc "$stage/bin/goc"
cp -a "$ROOT/VERSION" "$stage/VERSION"
for s in goc-flags.sh goc-go.sh goc-check.sh goc-product-lib.sh goc-toolchain.sh pack-release.sh; do
  cp -a "$ROOT/scripts/$s" "$stage/scripts/$s"
  chmod a+rx "$stage/scripts/$s"
done
cp -a "$ROOT/include/goc.h" "$ROOT/include/goc_uptr.h" "$stage/include/"
cp -a "$ROOT/examples/hello/go.mod" "$ROOT/examples/hello/main.go" \
  "$ROOT/examples/hello/add.c" "$stage/examples/hello/"
cp -a "$ROOT/backend/realbody/goc_p28_realbody.sh" \
  "$ROOT/backend/realbody/args_map.bin" \
  "$ROOT/backend/realbody/locals_map.bin" \
  "$stage/backend/realbody/"
chmod a+rx "$stage/backend/realbody/goc_p28_realbody.sh"
cp -a "$ROOT/backend/tools/toolexec_pack_goobj.sh" "$stage/backend/tools/"
chmod a+rx "$stage/backend/tools/toolexec_pack_goobj.sh"

cp -a "$(readlink -f "$clang")" "$stage/bin/clang-19"
ln -sfn clang-19 "$stage/bin/clang"
chmod a+rx "$stage/bin/clang-19"
libdir="$(goc_clang_libdir "$clang")"
[[ -e "$libdir/libLLVM.so.19.1" ]] || goc_die "no libLLVM.so.19.1 in $libdir"
cp -a "$libdir/libLLVM.so.19.1" "$stage/lib/libLLVM.so.19.1"
ln -sfn libLLVM.so.19.1 "$stage/lib/libLLVM.so"
# build-clang.sh sets LLVM_LINK_LLVM_DYLIB, so clang links libclang-cpp.
if [[ -e "$libdir/libclang-cpp.so.19.1" ]]; then
  cp -a "$libdir/libclang-cpp.so.19.1" "$stage/lib/libclang-cpp.so.19.1"
  ln -sfn libclang-cpp.so.19.1 "$stage/lib/libclang-cpp.so"
fi
[[ -d "$libdir/clang" ]] || goc_die "no clang resource dir in $libdir"
cp -a "$libdir/clang" "$stage/lib/clang"

copy_tool opt "$OPT"
copy_tool llc
copy_tool llvm-mc
copy_tool llvm-objdump
cp -a "$LLC" "$stage/passes/goc-llc"
cp -a "$GOC_COLOR_ESCAPE" "$stage/passes/goc-color-escape"
cp -a "$GOC_STACKMAP" "$stage/passes/GocStackMap.so"
chmod a+rx "$stage/passes/goc-llc" "$stage/passes/goc-color-escape"

( cd "$ROOT/backend" && go build -o "$stage/bin/elfpack" ./goobj/elfpack/ )
chmod a+rx "$stage/bin/elfpack"
( cd "$ROOT/backend" && go build -o "$stage/bin/goc-lower" ./goobj/goclower/ )
chmod a+rx "$stage/bin/goc-lower"

# Drop the absolute build-machine directory from RUNPATH. $ORIGIN/../lib
# is already the first entry, and it is the one the unpacked tree uses.
# patchelf can also add that entry to the passes. Without it, the python
# fallback only shortens an existing $ORIGIN/../lib:<abs> string; the
# driver still prepends lib/ onto LD_LIBRARY_PATH for the passes.
rpath_origin() {
  local bin="$1"
  if command -v patchelf >/dev/null 2>&1; then
    patchelf --set-rpath '$ORIGIN/../lib' "$bin"
    return
  fi
  python3 - "$bin" <<'PY'
import sys
path = sys.argv[1]
new = b"$ORIGIN/../lib"
data = bytearray(open(path, "rb").read())
marker = b"$ORIGIN/../lib:"
idx = 0
n = 0
while True:
    i = data.find(marker, idx)
    if i < 0:
        break
    end = data.find(b"\0", i)
    if end < 0 or not data[i:end].startswith(new):
        sys.exit("pack-release: unexpected RUNPATH in %s" % path)
    data[i:i + len(new)] = new
    data[i + len(new):end] = b"\0" * (end - (i + len(new)))
    n += 1
    idx = end + 1
if n == 0 and data.find(new + b"\0") < 0:
    sys.exit("pack-release: no RUNPATH to shorten in %s" % path)
open(path, "wb").write(data)
PY
}
for bin in "$stage/bin/clang-19" "$stage/bin/opt" "$stage/bin/llc" \
           "$stage/bin/llvm-mc" "$stage/bin/llvm-objdump" \
           "$stage/lib/libclang-cpp.so.19.1"; do
  [[ -f "$bin" ]] || continue
  rpath_origin "$bin"
done
if command -v patchelf >/dev/null 2>&1; then
  for bin in "$stage/passes/goc-llc" "$stage/passes/goc-color-escape"; do
    patchelf --set-rpath '$ORIGIN/../lib' "$bin"
  done
fi

# Assertion builds embed the absolute source path. Overwrite this checkout's
# prefix with a same-length generic one so the archive does not carry it.
python3 - "$stage" "$ROOT" <<'PY'
import os, sys
stage, root = sys.argv[1], sys.argv[2]
old = root.encode()
base = b"/src/goc-release"
if len(base) > len(old):
    sys.exit("pack-release: checkout path is shorter than the replacement")
new = base + b"/" * (len(old) - len(base))
nfiles = 0
for dirpath, _, files in os.walk(stage):
    for name in files:
        path = os.path.join(dirpath, name)
        if os.path.islink(path):
            continue
        data = open(path, "rb").read()
        if old not in data:
            continue
        open(path, "wb").write(data.replace(old, new))
        nfiles += 1
print("pack-release: rewrote the checkout path in %d files" % nfiles, file=sys.stderr)
PY

cat >"$stage/README.md" <<EOF
# goc ${ver}

linux/amd64. This archive is the whole compiler: \`goc\` and the patched Clang.

## Install

Need Go 1.24 or newer.

\`\`\`bash
tar -xzf ${name}.tar.gz
cd ${name}
export PATH="\$PWD/bin:\$PATH"
goc version
goc check
\`\`\`

\`goc check\` prints \`PASS goc check\` and the sample program prints \`hello 42\`.

## Compile a program

\`\`\`bash
goc go examples/hello -o /tmp/hello
/tmp/hello
\`\`\`

C is \`int\`. The Go declaration is a body-less \`int32\`. This path does not call libc. See the repository \`docs/quickstart.md\`.

Building this archive from source is \`docs/build-from-source.md\`.
EOF

echo "pack-release: staged $stage" >&2
case "$dest" in
  *.tar.gz|*.tar)
    mkdir -p "$(dirname "$dest")"
    # dirname of a bare filename is '.'; make the path absolute for tar -C.
    abs="$(cd "$(dirname "$dest")" && pwd)/$(basename "$dest")"
    if [[ "$dest" == *.tar.gz ]]; then
      tar -C "$work" -czf "$abs" "$name"
    else
      tar -C "$work" -cf "$abs" "$name"
    fi
    echo "pack-release: wrote $abs" >&2
    ;;
  *)
    rm -rf "$dest"
    mkdir -p "$(dirname "$dest")"
    mv "$stage" "$dest"
    trap - EXIT
    rm -rf "$work"
    echo "pack-release: wrote $dest" >&2
    ;;
esac
