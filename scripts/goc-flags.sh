# Command-line settings for the goc driver.
# Source this file. Each recognized flag exports the value the passes,
# realbody and the converter already read. A flag overrides an inherited value.
#
# On success GOC_FLAG_SHIFT is 1 or 2. On an unknown flag the function
# returns 1 and shifts nothing.

goc_die() { echo "goc: $*" >&2; exit 1; }

GOC_FLAG_SEEN="${GOC_FLAG_SEEN:-}"

goc_flag_mark() { GOC_FLAG_SEEN="${GOC_FLAG_SEEN:-} $1"; }

goc_flag_seen() { [[ " ${GOC_FLAG_SEEN:-} " == *" $1 "* ]]; }

# Drop compile settings inherited from the environment. `goc go`, `goc check`,
# and `goc toolchain` call this before parsing flags, then apply their defaults.
# `goc build` does not: repository scripts still pass the same settings as
# variables, and a flag on that command overrides them.
goc_isolate_product_env() {
  unset GOC_OPT_LEVEL GOC_DEFAULT_PTR_COLOR GOC_INLINE_DYNALLOC GOC_OPT_EXTRA \
    GOC_LLC_EXTRA GOC_COLOR_REPORT GOC_ARCH GOC_IR_RENAMES GOC_LIBCALL_IMPL \
    GOC_KEEP_TMP GOC_PKG
}

goc_need_arg() {
  [[ -n "${2:-}" ]] || goc_die "$1 needs a value"
  printf '%s' "$2"
}

goc_consume_flag() {
  GOC_FLAG_SHIFT=0
  case "$1" in
    -O0|-O1|-O2|-O3)
      export GOC_OPT_LEVEL="${1#-O}"
      goc_flag_mark opt
      GOC_FLAG_SHIFT=1
      ;;
    --default-ptr-color)
      export GOC_DEFAULT_PTR_COLOR="$(goc_need_arg --default-ptr-color "${2:-}")"
      goc_flag_mark default-ptr-color
      GOC_FLAG_SHIFT=2
      ;;
    --default-ptr-color=*)
      export GOC_DEFAULT_PTR_COLOR="${1#--default-ptr-color=}"
      [[ -n "$GOC_DEFAULT_PTR_COLOR" ]] || goc_die "--default-ptr-color needs a value"
      goc_flag_mark default-ptr-color
      GOC_FLAG_SHIFT=1
      ;;
    --no-default-ptr-color)
      unset GOC_DEFAULT_PTR_COLOR
      goc_flag_mark default-ptr-color
      GOC_FLAG_SHIFT=1
      ;;
    --fast-stack-alloca)
      export GOC_INLINE_DYNALLOC=1
      goc_flag_mark fast-stack-alloca
      GOC_FLAG_SHIFT=1
      ;;
    --no-fast-stack-alloca)
      export GOC_INLINE_DYNALLOC=0
      goc_flag_mark fast-stack-alloca
      GOC_FLAG_SHIFT=1
      ;;
    --color-report)
      export GOC_COLOR_REPORT=1
      goc_flag_mark color-report
      GOC_FLAG_SHIFT=1
      ;;
    --no-color-report)
      unset GOC_COLOR_REPORT
      goc_flag_mark color-report
      GOC_FLAG_SHIFT=1
      ;;
    --arch)
      local a
      a="$(goc_need_arg --arch "${2:-}")"
      case "$a" in
        amd64|arm64) export GOC_ARCH="$a" ;;
        *) goc_die "--arch wants amd64 or arm64" ;;
      esac
      goc_flag_mark arch
      GOC_FLAG_SHIFT=2
      ;;
    --arch=*)
      local a="${1#--arch=}"
      case "$a" in
        amd64|arm64) export GOC_ARCH="$a" ;;
        *) goc_die "--arch wants amd64 or arm64" ;;
      esac
      goc_flag_mark arch
      GOC_FLAG_SHIFT=1
      ;;
    --keep-tmp)
      export GOC_KEEP_TMP="$(mktemp -d)"
      echo "goc: keeping temps in $GOC_KEEP_TMP" >&2
      goc_flag_mark keep-tmp
      GOC_FLAG_SHIFT=1
      ;;
    --keep-tmp=*)
      export GOC_KEEP_TMP="${1#--keep-tmp=}"
      [[ -n "$GOC_KEEP_TMP" ]] || goc_die "--keep-tmp needs a directory"
      mkdir -p "$GOC_KEEP_TMP"
      goc_flag_mark keep-tmp
      GOC_FLAG_SHIFT=1
      ;;
    --opt-extra)
      export GOC_OPT_EXTRA="$(goc_need_arg --opt-extra "${2:-}")"
      goc_flag_mark opt-extra
      GOC_FLAG_SHIFT=2
      ;;
    --opt-extra=*)
      export GOC_OPT_EXTRA="${1#--opt-extra=}"
      goc_flag_mark opt-extra
      GOC_FLAG_SHIFT=1
      ;;
    --llc-extra)
      export GOC_LLC_EXTRA="$(goc_need_arg --llc-extra "${2:-}")"
      goc_flag_mark llc-extra
      GOC_FLAG_SHIFT=2
      ;;
    --llc-extra=*)
      export GOC_LLC_EXTRA="${1#--llc-extra=}"
      goc_flag_mark llc-extra
      GOC_FLAG_SHIFT=1
      ;;
    --pkg)
      export GOC_PKG="$(goc_need_arg --pkg "${2:-}")"
      goc_flag_mark pkg
      GOC_FLAG_SHIFT=2
      ;;
    --pkg=*)
      export GOC_PKG="${1#--pkg=}"
      [[ -n "$GOC_PKG" ]] || goc_die "--pkg needs a package path"
      goc_flag_mark pkg
      GOC_FLAG_SHIFT=1
      ;;
    --ir-renames)
      export GOC_IR_RENAMES="$(goc_need_arg --ir-renames "${2:-}")"
      goc_flag_mark ir-renames
      GOC_FLAG_SHIFT=2
      ;;
    --ir-renames=*)
      export GOC_IR_RENAMES="${1#--ir-renames=}"
      goc_flag_mark ir-renames
      GOC_FLAG_SHIFT=1
      ;;
    --libcall-impl)
      export GOC_LIBCALL_IMPL="$(goc_need_arg --libcall-impl "${2:-}")"
      goc_flag_mark libcall-impl
      GOC_FLAG_SHIFT=2
      ;;
    --libcall-impl=*)
      export GOC_LIBCALL_IMPL="${1#--libcall-impl=}"
      goc_flag_mark libcall-impl
      GOC_FLAG_SHIFT=1
      ;;
    *)
      return 1
      ;;
  esac
  return 0
}

# Fill any product switch the user did not pass. Call after goc_isolate_product_env
# and after flags have been parsed.
goc_product_defaults() {
  goc_flag_seen opt || export GOC_OPT_LEVEL=3
  goc_flag_seen default-ptr-color || export GOC_DEFAULT_PTR_COLOR=cptr
  goc_flag_seen fast-stack-alloca || export GOC_INLINE_DYNALLOC=1
}

# Append the resolved compile switches to the named array.
goc_append_resolved_flags() {
  local -n _dest=$1
  _dest+=(-O"${GOC_OPT_LEVEL:-0}")
  if [[ -n "${GOC_DEFAULT_PTR_COLOR:-}" ]]; then
    _dest+=(--default-ptr-color "$GOC_DEFAULT_PTR_COLOR")
  elif goc_flag_seen default-ptr-color; then
    _dest+=(--no-default-ptr-color)
  fi
  if [[ "${GOC_INLINE_DYNALLOC:-0}" == 1 ]]; then
    _dest+=(--fast-stack-alloca)
  else
    _dest+=(--no-fast-stack-alloca)
  fi
  [[ "${GOC_COLOR_REPORT:-}" == 1 ]] && _dest+=(--color-report)
  [[ -n "${GOC_ARCH:-}" ]] && _dest+=(--arch "$GOC_ARCH")
  [[ -n "${GOC_OPT_EXTRA:-}" ]] && _dest+=(--opt-extra "$GOC_OPT_EXTRA")
  [[ -n "${GOC_LLC_EXTRA:-}" ]] && _dest+=(--llc-extra "$GOC_LLC_EXTRA")
  [[ -n "${GOC_IR_RENAMES:-}" ]] && _dest+=(--ir-renames "$GOC_IR_RENAMES")
  [[ -n "${GOC_LIBCALL_IMPL:-}" ]] && _dest+=(--libcall-impl "$GOC_LIBCALL_IMPL")
  if [[ -n "${GOC_KEEP_TMP:-}" ]]; then
    _dest+=(--keep-tmp="$GOC_KEEP_TMP")
  fi
  if [[ -n "${GOC_PKG:-}" ]]; then
    _dest+=(--pkg "$GOC_PKG")
  fi
  return 0
}

goc_flags_help() {
  cat <<'EOF'
Flags (a flag overrides a same-named environment variable on `goc build`):
The patched clang is bin/clang in a release, or
third_party/llvm-19.1.7-clang-build/bin/clang in a checkout.
  -O0|-O1|-O2|-O3        LLVM IR optimization level
  --default-ptr-color COLOR
                         Color of an unannotated T* (cptr, sptr, uptr, gptr)
  --no-default-ptr-color Leave that color unset; each pointer is inferred
  --fast-stack-alloca    alloca bumps a 16 MiB pool per object, inline.
                         Much faster. The pool cursor is a plain global,
                         so this is single-goroutine only. Concurrent
                         calls are not thread-safe.
  --no-fast-stack-alloca alloca is one malloc/free per call
  --color-report         Print the pointer-color table; does not change colors
  --no-color-report
  --arch amd64|arm64     `goc go` accepts amd64 only
  --keep-tmp             Keep the temporary directory
  --keep-tmp=DIR
  --opt-extra STRING     Extra flags for opt
  --llc-extra STRING     Extra flags for llc
  --pkg PATH             Go symbol prefix (default: main, or the import path)
  --ir-renames SPEC      Space-separated old:new IR renames
  --libcall-impl SPEC    Compiler-emitted libcall implementations

`goc go` and `goc check` ignore inherited GOC_* settings and start from:
  -O3 --default-ptr-color cptr --fast-stack-alloca
Each Go→C thunk frame is that signature's stack arguments, rounded up to 16,
plus the saved frame pointer.
`goc build` emits every defined function and a Go entry for each. It stays
at -O0 and still honors inherited GOC_*. A flag wins. Morestack, splittable
frames, and sptr maps (amd64 and arm64) are always on for `goc go` and
`goc build`.
Clang arguments go after `--`.
EOF
}
