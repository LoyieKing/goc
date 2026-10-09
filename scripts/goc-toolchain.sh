#!/usr/bin/env bash
# goc toolchain status | pack DEST
# DEST is a directory, or a path ending in .tar / .tar.gz.
# pack writes a complete goc release: driver, patched clang, passes, elfpack.
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
pack writes a complete linux/amd64 release (goc plus the patched clang).
DEST is a directory, or a path ending in .tar / .tar.gz.
EOF
  echo
  goc_flags_help
}

cmd_status() {
  local clang libdir flags=()
  clang="$(goc_resolve_clang)"
  libdir="$(goc_clang_libdir "$clang")"
  goc_append_resolved_flags flags
  cat <<EOF
clang: $clang
libdir: $libdir
opt: $OPT
llc: $LLC
color-escape: ${GOC_COLOR_ESCAPE:-<built on first goc build>}
stackmap: $GOC_STACKMAP
goc go: ${flags[*]}
arch: ${GOC_ARCH:-amd64}
EOF
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
case "$cmd" in
  status)
    goc_require_tools
    cmd_status
    ;;
  pack)
    exec "$SELF/pack-release.sh" "$dest"
    ;;
esac
