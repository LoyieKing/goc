# shellcheck shell=bash
# arm64_skip_old_go NAME: exit 0 with a SKIP line when the go in PATH is
# older than go1.21. goc does not support arm64 there: the runtime does not
# adjust saved frame pointers when it copies a stack, and goc's arm64 code
# addresses locals from the frame pointer (docs/multi-go-version.md).
arm64_skip_old_go() {
  local v m
  v="$(go env GOVERSION)"
  m="${v#go1.}"
  m="${m%%[!0-9]*}"
  if [[ -n "$m" ]] && (( m < 21 )); then
    echo "SKIP $1: goc supports arm64 from go1.21 ($v)"
    exit 0
  fi
}
