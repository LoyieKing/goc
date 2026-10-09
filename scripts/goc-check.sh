#!/usr/bin/env bash
# Install self-check: build and run examples/hello. Flags are forwarded to goc go.
set -euo pipefail
SELF="$(cd "$(dirname "$0")" && pwd)"
ROOT="${GOC_ROOT:-$(cd "$SELF/.." && pwd)}"
export GOC_ROOT="$ROOT"
# shellcheck source=goc-flags.sh
source "$SELF/goc-flags.sh"

# Reject a package path. A flag's value (the path after --clang) is not one.
scan=("$@")
while [[ ${#scan[@]} -gt 0 ]]; do
  if goc_consume_flag "${scan[0]}" "${scan[1]:-}"; then
    if [[ "$GOC_FLAG_SHIFT" -eq 2 ]]; then
      scan=("${scan[@]:2}")
    else
      scan=("${scan[@]:1}")
    fi
    continue
  fi
  case "${scan[0]}" in
    -h|--help)
      echo "usage: goc check [flags]"
      echo "Builds a copy of examples/hello and requires stdout \"hello 42\"."
      echo
      goc_flags_help
      exit 0
      ;;
    *) goc_die "goc check takes flags only (got ${scan[0]})" ;;
  esac
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cp "$ROOT/examples/hello/go.mod" "$ROOT/examples/hello/main.go" "$ROOT/examples/hello/add.c" "$TMP/"
# The trailing -o wins over an -o in "$@".
bash "$SELF/goc-go.sh" "$TMP" "$@" -o "$TMP/hello"
got="$("$TMP/hello")"
[[ "$got" == "hello 42" ]] || goc_die "examples/hello printed $(printf %q "$got"), want \"hello 42\""
echo "PASS goc check"
