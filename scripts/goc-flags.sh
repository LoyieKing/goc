# Command-line settings for the goc driver.
# Source this file. Each recognized flag exports the value the passes,
# realbody, and elfpack already read. A flag overrides an inherited value.
#
# GOC_FLAG_MODE=driver accepts only --root. --clang and --toolchain are
# rejected in every mode: the patched clang ships next to goc.
# GOC_FLAG_MODE=all (default) accepts every switch below.
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
  unset GOC_OPT_LEVEL GOC_DEFAULT_PTR_COLOR GOC_NO_NOSPLIT GOC_MORESTACK \
    GOC_SPTR_MAPS GOC_INLINE_DYNALLOC GOC_FIXED_G GOC_OPT_EXTRA \
    GOC_LLC_EXTRA GOC_COLOR_REPORT GOC_CSR_ADJUST GOC_FRAMEADDR_MODE \
    GOC_ARCH GOC_LLC_OPT GOC_IR_RENAMES GOC_LIBCALL_IMPL GOC_KEEP_TMP \
    LLC OPT GOC_COLOR_ESCAPE GOC_STACKMAP GOC_PKG
}

goc_need_arg() {
  [[ -n "${2:-}" ]] || goc_die "$1 needs a value"
  printf '%s' "$2"
}

goc_consume_flag() {
  local mode="${GOC_FLAG_MODE:-all}"
  GOC_FLAG_SHIFT=0
  if [[ "$mode" == driver ]]; then
    case "$1" in
      --clang|--clang=*|--toolchain|--toolchain=*|--root|--root=*) ;;
      *) return 1 ;;
    esac
  fi
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
    --ptr-color|--ptr-color=*|--no-ptr-color)
      goc_die "the flag is --default-ptr-color"
      ;;
    --morestack|--no-morestack|--splittable|--nosplit|--no-nosplit|--sptr-maps|--no-sptr-maps)
      goc_die "morestack, splittable frames, and sptr maps are always on when linking with Go. There is no switch."
      ;;
    --creserve|--creserve=*)
      goc_die "each Go→C thunk is sized from its signature. There is no --creserve."
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
    --inline-dynalloc|--no-inline-dynalloc)
      goc_die "the flag is --fast-stack-alloca"
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
    --clang|--clang=*|--toolchain|--toolchain=*)
      goc_die "goc ships its patched clang. There is no --clang or --toolchain."
      ;;
    --root)
      local p
      p="$(goc_need_arg --root "${2:-}")"
      [[ -d "$p" ]] || goc_die "--root is not a directory: $p"
      ROOT="$(cd "$p" && pwd)"
      export GOC_ROOT="$ROOT"
      goc_flag_mark root
      GOC_FLAG_SHIFT=2
      ;;
    --root=*)
      local p="${1#--root=}"
      [[ -d "$p" ]] || goc_die "--root is not a directory: $p"
      ROOT="$(cd "$p" && pwd)"
      export GOC_ROOT="$ROOT"
      goc_flag_mark root
      GOC_FLAG_SHIFT=1
      ;;
    --llc)
      local p
      p="$(goc_need_arg --llc "${2:-}")"
      export LLC="$p"
      goc_flag_mark llc
      GOC_FLAG_SHIFT=2
      ;;
    --llc=*)
      export LLC="${1#--llc=}"
      [[ -n "$LLC" ]] || goc_die "--llc needs a path"
      goc_flag_mark llc
      GOC_FLAG_SHIFT=1
      ;;
    --opt-bin)
      local p
      p="$(goc_need_arg --opt-bin "${2:-}")"
      export OPT="$p"
      goc_flag_mark opt-bin
      GOC_FLAG_SHIFT=2
      ;;
    --opt-bin=*)
      export OPT="${1#--opt-bin=}"
      [[ -n "$OPT" ]] || goc_die "--opt-bin needs a path"
      goc_flag_mark opt-bin
      GOC_FLAG_SHIFT=1
      ;;
    --color-escape)
      local p
      p="$(goc_need_arg --color-escape "${2:-}")"
      export GOC_COLOR_ESCAPE="$p"
      goc_flag_mark color-escape
      GOC_FLAG_SHIFT=2
      ;;
    --color-escape=*)
      export GOC_COLOR_ESCAPE="${1#--color-escape=}"
      [[ -n "$GOC_COLOR_ESCAPE" ]] || goc_die "--color-escape needs a path"
      goc_flag_mark color-escape
      GOC_FLAG_SHIFT=1
      ;;
    --stackmap)
      local p
      p="$(goc_need_arg --stackmap "${2:-}")"
      export GOC_STACKMAP="$p"
      goc_flag_mark stackmap
      GOC_FLAG_SHIFT=2
      ;;
    --stackmap=*)
      export GOC_STACKMAP="${1#--stackmap=}"
      [[ -n "$GOC_STACKMAP" ]] || goc_die "--stackmap needs a path"
      goc_flag_mark stackmap
      GOC_FLAG_SHIFT=1
      ;;
    --frameaddr)
      local m
      m="$(goc_need_arg --frameaddr "${2:-}")"
      case "$m" in
        gep|asm) export GOC_FRAMEADDR_MODE="$m" ;;
        *) goc_die "--frameaddr wants gep or asm" ;;
      esac
      goc_flag_mark frameaddr
      GOC_FLAG_SHIFT=2
      ;;
    --frameaddr=*)
      local m="${1#--frameaddr=}"
      case "$m" in
        gep|asm) export GOC_FRAMEADDR_MODE="$m" ;;
        *) goc_die "--frameaddr wants gep or asm" ;;
      esac
      goc_flag_mark frameaddr
      GOC_FLAG_SHIFT=1
      ;;
    --fixed-g)
      export GOC_FIXED_G=1
      goc_flag_mark fixed-g
      GOC_FLAG_SHIFT=1
      ;;
    --no-fixed-g)
      export GOC_FIXED_G=0
      goc_flag_mark fixed-g
      GOC_FLAG_SHIFT=1
      ;;
    --csr-adjust)
      export GOC_CSR_ADJUST=1
      goc_flag_mark csr-adjust
      GOC_FLAG_SHIFT=1
      ;;
    --no-csr-adjust)
      export GOC_CSR_ADJUST=0
      goc_flag_mark csr-adjust
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
    --llc-opt)
      local n
      n="$(goc_need_arg --llc-opt "${2:-}")"
      [[ "$n" =~ ^[0-3]$ ]] || goc_die "--llc-opt wants 0, 1, 2, or 3"
      export GOC_LLC_OPT="$n"
      goc_flag_mark llc-opt
      GOC_FLAG_SHIFT=2
      ;;
    --llc-opt=*)
      local n="${1#--llc-opt=}"
      [[ "$n" =~ ^[0-3]$ ]] || goc_die "--llc-opt wants 0, 1, 2, or 3"
      export GOC_LLC_OPT="$n"
      goc_flag_mark llc-opt
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
  # Required to run on a Go stack. Not flags.
  export GOC_NO_NOSPLIT=1
  export GOC_MORESTACK=1
  export GOC_SPTR_MAPS=1
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
  [[ -n "${LLC:-}" ]] && _dest+=(--llc "$LLC")
  [[ -n "${OPT:-}" ]] && _dest+=(--opt-bin "$OPT")
  [[ -n "${GOC_COLOR_ESCAPE:-}" ]] && _dest+=(--color-escape "$GOC_COLOR_ESCAPE")
  [[ -n "${GOC_STACKMAP:-}" ]] && _dest+=(--stackmap "$GOC_STACKMAP")
  [[ "${GOC_COLOR_REPORT:-}" == 1 ]] && _dest+=(--color-report)
  [[ -n "${GOC_ARCH:-}" ]] && _dest+=(--arch "$GOC_ARCH")
  [[ -n "${GOC_FRAMEADDR_MODE:-}" ]] && _dest+=(--frameaddr "$GOC_FRAMEADDR_MODE")
  [[ "${GOC_FIXED_G:-0}" == 1 ]] && _dest+=(--fixed-g)
  [[ "${GOC_CSR_ADJUST:-0}" == 1 ]] && _dest+=(--csr-adjust)
  [[ -n "${GOC_OPT_EXTRA:-}" ]] && _dest+=(--opt-extra "$GOC_OPT_EXTRA")
  [[ -n "${GOC_LLC_EXTRA:-}" ]] && _dest+=(--llc-extra "$GOC_LLC_EXTRA")
  [[ -n "${GOC_LLC_OPT:-}" ]] && _dest+=(--llc-opt "$GOC_LLC_OPT")
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
  --root DIR             Repository or release root
The patched clang is bin/clang in a release, or
third_party/llvm-19.1.7-clang-build/bin/clang in a checkout.
There is no --clang and no --toolchain.
  -O0|-O1|-O2|-O3        LLVM IR optimization level
  --default-ptr-color COLOR
                         Color of an unannotated T* (cptr, sptr, uptr, gptr)
  --no-default-ptr-color Leave that color unset; each pointer is inferred
  --fast-stack-alloca    Inline the alloca pool bump. Much faster.
                         The pool cursor is process-global, so this is
                         single-threaded only. Concurrent calls are not
                         thread-safe.
  --no-fast-stack-alloca Leave alloca as a call to goc_dynalloc
  --color-report         Print the pointer-color table; does not change colors
  --no-color-report
  --arch amd64|arm64     `goc go` accepts amd64 only
  --llc PATH             llc binary
  --opt-bin PATH         LLVM opt binary
  --color-escape PATH    goc-color-escape pass
  --stackmap PATH        GocStackMap.so
  --frameaddr gep|asm    Frame-address mode (arm64 forces asm)
  --fixed-g              Reserve r14 for g (off unless this flag is passed)
  --no-fixed-g
  --csr-adjust           Adjust frame addresses kept in callee-saved registers
  --no-csr-adjust
  --keep-tmp             Keep the temporary directory
  --keep-tmp=DIR
  --opt-extra STRING     Extra flags for opt
  --llc-extra STRING     Extra flags for llc
  --llc-opt 0|1|2|3      llc optimization level, when it differs from -O
  --pkg PATH             Go symbol prefix (default: main, or the import path)
  --ir-renames SPEC      Space-separated old:new IR renames
  --libcall-impl SPEC    Compiler-emitted libcall implementations

`goc go` and `goc check` ignore inherited GOC_* settings and start from:
  -O3 --default-ptr-color cptr --fast-stack-alloca
Each Go→C thunk frame is that signature's stack arguments, rounded up to 16,
plus the saved frame pointer.
Morestack, splittable frames, and sptr maps are always on for `goc go`.
`goc build --goabi` turns on morestack and splittable frames. amd64 also
records sptr maps. There is no flag to disable them. arm64 cannot record
sptr maps. `goc build` without `--goabi` stays at -O0 with those three off,
unless an inherited variable sets them.
Clang arguments go after `--`.
EOF
}
