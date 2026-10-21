#!/usr/bin/env bash
# Ten-engine V8-v7 comparison for docs/benchmark/README.md ("多引擎 V8-v7 对比").
#
# Engines: goc-ng, goc-bellard, native ng, native Bellard (gcc and clang),
# liustack's C patch on Bellard 2026-06-04 built natively (gcc and clang, the
# same upstream Makefile), github.com/liustack/quickjs and modernc.org/quickjs
# (both ccgo-translated Bellard QuickJS, pure Go, via bench/ccgo), and Goja.
#
# Rounds are split over the CPUs in $CPUS (one lane each, odd/even rounds);
# in every round each engine runs once, one at a time on that lane's CPU, and
# the start engine rotates. bench/report/v8multi.py takes medians and draws
# the charts.
#
# Env: OUT (raw dir, default /tmp/v8multi), CPUS (default "1 5"), ROUNDS (8),
#      GOC GOCB NG BELLARD BELLARD_CLANG GOJA V8JS as in bench/all.sh,
#      BELLARD_TAR (quickjs-2026-06-04.tar.xz), SKIP_BUILD=1 to reuse builds.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PS=/workspace/perf-study
OUT="${OUT:-/tmp/v8multi}"; CPUS="${CPUS:-1 5}"; ROUNDS="${ROUNDS:-8}"
GOC="${GOC:-$ROOT/build/qjs/qjscli}"; GOCB="${GOCB:-$ROOT/build/qjs-bellard/qjscli}"
NG="${NG:-$PS/native/b-O2/qjs}"; BELLARD="${BELLARD:-$PS/bellard/quickjs-2026-06-04/qjs}"
BELLARD_CLANG="${BELLARD_CLANG:-$PS/bellard/clang-O2/qjs}"; GOJA="${GOJA:-$PS/goja-cli/gojacli}"
V8JS="${V8JS:-$PS/js/v8.js}"; BELLARD_TAR="${BELLARD_TAR:-$PS/bellard/quickjs-2026-06-04.tar.xz}"
B="$OUT/build"; mkdir -p "$OUT/v8" "$B"

if [[ -z "${SKIP_BUILD:-}" ]]; then
  (cd "$ROOT/bench/ccgo" && go build -o "$B/qjs-liustack" ./liustack && go build -o "$B/qjs-modernc" ./modernc)
  patch_file="$(cd "$ROOT/bench/ccgo" && go list -m -f '{{.Dir}}' github.com/liustack/quickjs)/lib/internal/patch/quickjs.diff"
  for cc in gcc clang; do
    rm -rf "$B/c-$cc"; mkdir -p "$B/c-$cc"
    tar xf "$BELLARD_TAR" -C "$B/c-$cc" --strip-components=1
    (cd "$B/c-$cc" && patch -p1 -s < "$patch_file")
    if [[ $cc == gcc ]]; then make -C "$B/c-$cc" -j4 qjs >/dev/null
    else make -C "$B/c-$cc" -j4 CONFIG_CLANG=y CC=clang-19 qjs >/dev/null; fi
  done
fi

cat > "$OUT/engines.txt" <<EOT
goc-ng $GOC --stack-size 16384 --script
goc-bellard $GOCB --stack-size 16384 --script
ng $NG -C --stack-size 16384
bellard $BELLARD --stack-size 16M
bellard-clang $BELLARD_CLANG --stack-size 16M
liustack-c-gcc $B/c-gcc/qjs --stack-size 16M
liustack-c-clang $B/c-clang/qjs --stack-size 16M
liustack-go $B/qjs-liustack
modernc-go $B/qjs-modernc
goja $GOJA
EOT
{
  echo "date: $(date '+%Y-%m-%d %H:%M:%S %Z')"
  echo "host: $(uname -srm), $(nproc) vCPU, $(lscpu | sed -n 's/^Model name: *//p'), lanes on CPUs $CPUS"
  echo "goc commit: $(git -C "$ROOT" rev-parse HEAD)"
  while read -r n p _; do echo "$n: sha256=$(sha256sum "$p" | cut -c1-16)"; done < "$OUT/engines.txt"
} > "$OUT/env.txt"

lane() {  # lane CPU ROUND...
  local cpu=$1 r i n line name c s; shift
  mapfile -t L < "$OUT/engines.txt"; n=${#L[@]}
  for r in "$@"; do for ((i = 0; i < n; i++)); do
    line="${L[$(( (i + r) % n ))]}"; name=${line%% *}; c=${line#* }
    s=$(date +%s.%N)
    taskset -c "$cpu" $c "$V8JS" > "$OUT/v8/$name-r$r.txt" 2>&1 || true
    awk -v a="$s" -v b="$(date +%s.%N)" 'BEGIN{printf "WALL %.2f\n", b-a}' >> "$OUT/v8/$name-r$r.txt"
    echo "cpu$cpu r$r $name $(grep SCORE "$OUT/v8/$name-r$r.txt")"
  done; done
}
read -r -a C <<< "$CPUS"; k=${#C[@]}
for ((j = 0; j < k; j++)); do
  rs=(); for ((r = j + 1; r <= ROUNDS; r += k)); do rs+=("$r"); done
  lane "${C[$j]}" "${rs[@]}" &
done
wait
python3 "$ROOT/bench/report/v8multi.py" "$OUT/v8"
