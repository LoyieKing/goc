#!/usr/bin/env bash
# goc toolchain status | pack DEST
# DEST is a directory, or a path ending in .tar / .tar.gz.
set -euo pipefail
SELF="$(cd "$(dirname "$0")" && pwd)"
ROOT="${GOC_ROOT:-$(cd "$SELF/.." && pwd)}"
export GOC_ROOT="$ROOT"
# shellcheck source=goc-flags.sh
source "$SELF/goc-flags.sh"
# shellcheck source=goc-product-lib.sh
source "$SELF/goc-product-lib.sh"

usage() {
  cat <<'EOF'
usage: goc toolchain status [flags]
       goc toolchain pack DEST [flags]

status prints the clang, opt, llc and passes goc go will use.
pack copies a relocatable toolchain into DEST (or DEST.tar / DEST.tar.gz):
  bin/clang, bin/opt, lib/libLLVM.so.19.1, lib/clang/, passes/
Unpack over ~/.goc/toolchain or third_party/goc-toolchain. The repo still
supplies cmd/goc, the realbody scripts and include/goc.h.
EOF
  echo
  goc_flags_help
}

cmd_status() {
  local libdir flags=()
  libdir="$(goc_clang_libdir "$GOC_CLANG")"
  goc_append_resolved_flags flags
  cat <<EOF
clang: $GOC_CLANG
libdir: $libdir
opt: $OPT
llc: $LLC
color-escape: ${GOC_COLOR_ESCAPE:-<built on first goc build>}
stackmap: $GOC_STACKMAP
goc go: ${flags[*]}
arch: ${GOC_ARCH:-amd64}
EOF
}

copy_opt() {
  local dest="$1" src
  if [[ -n "${OPT:-}" && -x "$OPT" ]]; then
    src="$OPT"
  else
    src="$(command -v opt-19 || true)"
  fi
  [[ -n "$src" && -x "$src" ]] || goc_die "opt-19 not found; cannot pack bin/opt"
  cp -a "$(readlink -f "$src")" "$dest/bin/opt"
  chmod a+rx "$dest/bin/opt"
}

cmd_pack() {
  local dest="${1:?goc toolchain pack: need DEST}"
  local tar=""
  case "$dest" in
    *.tar.gz) tar="$dest"; dest="${dest%.tar.gz}" ;;
    *.tar) tar="$dest"; dest="${dest%.tar}" ;;
  esac
  local clang bin libdir
  clang="$(readlink -f "$GOC_CLANG")"
  bin="$(dirname "$clang")"
  libdir="$(goc_clang_libdir "$clang")"
  [[ -e "$libdir/libLLVM.so.19.1" ]] || goc_die "no libLLVM.so.19.1 in $libdir"
  rm -rf "$dest"
  mkdir -p "$dest/bin" "$dest/lib" "$dest/passes"
  cp -a "$clang" "$dest/bin/clang-19"
  ln -sfn clang-19 "$dest/bin/clang"
  # clang looks up lib/clang/<ver> relative to the real binary.
  if [[ -d "$libdir/clang" ]]; then
    cp -a "$libdir/clang" "$dest/lib/clang"
  fi
  cp -a "$libdir/libLLVM.so.19.1" "$dest/lib/libLLVM.so.19.1"
  ln -sfn libLLVM.so.19.1 "$dest/lib/libLLVM.so"
  copy_opt "$dest"
  cp -a "$LLC" "$dest/passes/goc-llc"
  cp -a "$GOC_COLOR_ESCAPE" "$dest/passes/goc-color-escape"
  cp -a "$GOC_STACKMAP" "$dest/passes/GocStackMap.so"
  chmod a+rx "$dest/bin/clang-19" "$dest/passes/goc-llc" "$dest/passes/goc-color-escape"
  cat >"$dest/README" <<EOF
goc toolchain. linux/amd64. Produced from:
  clang: $clang
  opt:   $(readlink -f "$dest/bin/opt")

Unpack this directory onto ~/.goc/toolchain or third_party/goc-toolchain
inside a goc checkout. Then: ./cmd/goc check
EOF
  echo "goc toolchain: packed $dest" >&2
  if [[ -n "$tar" ]]; then
    local parent base
    parent="$(cd "$(dirname "$dest")" && pwd)"
    base="$(basename "$dest")"
    case "$tar" in
      *.tar.gz) tar -C "$parent" -czf "$tar" "$base" ;;
      *) tar -C "$parent" -cf "$tar" "$base" ;;
    esac
    echo "goc toolchain: wrote $tar" >&2
  fi
}

goc_isolate_product_env
cmd=""
dest=""
while [[ $# -gt 0 ]]; do
  if goc_consume_flag "$1" "${2:-}"; then
    shift "$GOC_FLAG_SHIFT"
    continue
  fi
  case "$1" in
    status) cmd=status; shift ;;
    pack)
      cmd=pack
      shift
      [[ $# -gt 0 && "$1" != -* ]] || goc_die "goc toolchain pack: need DEST"
      dest="$1"
      shift
      ;;
    -h|--help|help) usage; exit 0 ;;
    *) goc_die "unknown toolchain command: $1" ;;
  esac
done
[[ -n "$cmd" ]] || cmd=status
goc_product_defaults
goc_require_tools
case "$cmd" in
  status) cmd_status ;;
  pack) cmd_pack "$dest" ;;
esac
