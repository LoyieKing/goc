#!/usr/bin/env bash
# P6 standalone binary goobj writer: plain `go build` against goobj/enc (vendored).
# Does NOT use a GOROOT overlay or import cmd/internal from the system tree.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/goobj/gocbinwrite"
mkdir -p "$(dirname "$BIN")"
NEED_BUILD=0
if [[ ! -x "$BIN" ]]; then NEED_BUILD=1; fi
if [[ -x "$BIN" && "$ROOT/goobj/binwriter/main.go" -nt "$BIN" ]]; then NEED_BUILD=1; fi
if [[ -x "$BIN" && "$ROOT/goobj/mirlower/lower.go" -nt "$BIN" ]]; then NEED_BUILD=1; fi
if [[ "$NEED_BUILD" -eq 1 ]]; then
  ( cd "$ROOT" && go build -o "$BIN" ./goobj/binwriter/ )
fi
exec "$BIN" "$@"
