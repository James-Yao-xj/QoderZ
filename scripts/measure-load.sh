#!/usr/bin/env bash
# Load footprint of `qoderz serve`: 400 KB text bodies, 100 requests, 8 at a time, driven by curl.
set -euo pipefail

if [[ "$(uname -s)" != Linux ]]; then
  printf 'This benchmark requires Linux procfs metrics. Build and test with scripts/build.sh on macOS.\n' >&2
  exit 1
fi

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="${QODERZ_PERF_BUILD_DIR:-$root/build-perf}"
jobs="${QODERZ_PERF_JOBS:-6}"
site="${QODERZ_PERF_SITE:-qoder-cn}"
requests="${QODERZ_PERF_REQUESTS:-100}"
concurrency="${QODERZ_PERF_CONCURRENCY:-8}"
body_text_bytes="${QODERZ_PERF_BODY_BYTES:-409600}"
sample_seconds="${QODERZ_PERF_SAMPLE_SECONDS:-0.05}"
cooldown_seconds="${QODERZ_PERF_COOLDOWN_SECONDS:-6}"
bin="${QODERZ_PERF_BINARY:-$build/qoderz}"
stub="$build/tests/qoderz_stub_upstream"

log() { printf '%s\n' "$*" >&2; }
die() {
  printf 'measure-load: %s\n' "$*" >&2
  exit 1
}

require_positive_integer() {
  local name="$1" value="$2"
  [[ "$value" =~ ^[1-9][0-9]*$ ]] || die "$name must be a positive integer"
}
require_positive_seconds() {
  local name="$1" value="$2"
  [[ "$value" =~ ^[0-9]+([.][0-9]+)?$ ]] || die "$name must be a positive decimal number"
  awk -v value="$value" 'BEGIN { exit !(value > 0) }' || die "$name must be greater than zero"
}
require_nonnegative_seconds() {
  local name="$1" value="$2"
  [[ "$value" =~ ^[0-9]+([.][0-9]+)?$ ]] || die "$name must be a non-negative decimal number"
  awk -v value="$value" 'BEGIN { exit !(value >= 0) }' || die "$name must be non-negative"
}

require_positive_integer QODERZ_PERF_REQUESTS "$requests"
require_positive_integer QODERZ_PERF_CONCURRENCY "$concurrency"
require_positive_integer QODERZ_PERF_BODY_BYTES "$body_text_bytes"
require_positive_seconds QODERZ_PERF_SAMPLE_SECONDS "$sample_seconds"
require_nonnegative_seconds QODERZ_PERF_COOLDOWN_SECONDS "$cooldown_seconds"

if [[ "${1:-}" != "--no-build" ]]; then
  log "measure-load: building Release into $build"
  cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON >/dev/null
  cmake --build "$build" --parallel "$jobs" >/dev/null
fi

[[ -x "$bin" ]] || die "missing $bin; drop --no-build to build it"
[[ -x "$stub" ]] || die "missing $stub; BUILD_TESTING=ON builds the stub upstream"
command -v curl >/dev/null 2>&1 || die "curl is required"
command -v jq >/dev/null 2>&1 || die "jq is required"

work="$(mktemp -d "${TMPDIR:-/tmp}/qoderz-load.XXXXXX")"
serve_pid=""
stub_pid=""
load_pid=""

cleanup() {
  local pid
  [[ -n "$load_pid" ]] && kill -TERM "$load_pid" 2>/dev/null || true
  for pid in "$serve_pid" "$stub_pid"; do
    [[ -n "$pid" ]] || continue
    kill -TERM "$pid" 2>/dev/null || true
  done
  sleep 0.2 2>/dev/null || true
  for pid in "$serve_pid" "$stub_pid"; do
    [[ -n "$pid" ]] || continue
    kill -KILL "$pid" 2>/dev/null || true
  done
  rm -rf "$work"
}
trap cleanup EXIT INT TERM

cat >"$work/credentials.json" <<JSON
{"version":1,
 "accounts":{"$site/perf":{"site":"$site","name":"perf","vpc":"",
   "access_token":"perf-access-token-not-real",
   "refresh_token":"perf-refresh-token-not-real",
   "expires_at":4102444800000,"uid":"uid-perf","display_name":"Perf",
   "email":"perf@example.invalid","account_id":"perf@example.invalid",
   "machine_id":"perf-machine-not-real","device_token":"","device_refresh_token":"",
   "device_chat":false,"pat_auth":false,"pat":"","created_at":0}},
 "defaults":{"$site":"$site/perf"}}
JSON
chmod 600 "$work/credentials.json"

"$stub" --listen http://127.0.0.1:0 >"$work/stub.out" 2>"$work/stub.err" &
stub_pid=$!
origin=""
for _ in $(seq 1 100); do
  origin="$(sed -n 's/^stub-upstream listening on //p' "$work/stub.out" 2>/dev/null | head -1)"
  [[ -n "$origin" ]] && break
  sleep 0.1
done
if [[ -z "$origin" ]]; then
  cat "$work/stub.err" >&2 || true
  die "the stub upstream did not start"
fi

QODERZ_UPSTREAM="$origin" HOME="$work" XDG_CONFIG_HOME="$work/xdg" \
  "$bin" serve --listen http://127.0.0.1:0 --site "$site" \
  --credentials "$work/credentials.json" >"$work/serve.out" 2>"$work/serve.err" &
serve_pid=$!
port=""
for _ in $(seq 1 100); do
  port="$(sed -n 's/^ *listen: *http:\/\/[^:]*:\([0-9][0-9]*\)$/\1/p' "$work/serve.err" 2>/dev/null | head -1)"
  [[ -n "$port" ]] && break
  sleep 0.1
done
if [[ -z "$port" ]]; then
  cat "$work/serve.err" >&2 || true
  die "the gateway did not start"
fi

code=""
for _ in $(seq 1 100); do
  code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 1 "http://127.0.0.1:$port/healthz" || true)"
  [[ "$code" == "200" ]] && break
  sleep 0.1
done
[[ "$code" == "200" ]] || die "the gateway never answered /healthz"

head -c "$body_text_bytes" /dev/zero | tr '\0' 'a' >"$work/text.bin"
# `--rawfile` reads the text straight from disk: a 400 KB argument would exceed Linux's 128 KiB
# per-argument limit (MAX_ARG_STRLEN) and fail with "Argument list too long".
jq -n --rawfile text "$work/text.bin" \
  '{model:"stub-model",messages:[{role:"user",content:$text}]}' >"$work/body.json"
body_bytes="$(wc -c <"$work/body.json")"

sample() {
  local pid="$1" rss pss anon threads fds fd_path
  rss="$(awk '/^VmRSS:/{print $2}' "/proc/$pid/status")"
  read -r pss anon < <(awk '/^Pss:/{p=$2} /^Anonymous:/{a=$2} END{print p+0, a+0}' \
    "/proc/$pid/smaps_rollup")
  threads="$(awk '/^Threads:/{print $2}' "/proc/$pid/status")"
  fds=0
  for fd_path in "/proc/$pid/fd/"*; do
    [[ -e "$fd_path" ]] || continue
    ((fds += 1))
  done
  printf '%s %s %s %s %s\n' "$rss" "$pss" "$anon" "$threads" "$fds"
}

proc_cpu_ticks() {
  awk '{sub(/^.*\) /, ""); print $12 + $13; exit}' "/proc/$1/stat"
}

proc_vmhwm_kb() {
  awk '/^VmHWM:/{print $2; exit}' "/proc/$1/status"
}

median() {
  sort -n | awk '{value[NR]=$1} END{
    if (NR == 0) { print "n/a"; exit }
    if (NR % 2 == 1) { print value[(NR + 1) / 2] }
    else { printf "%.1f\n", (value[NR / 2] + value[NR / 2 + 1]) / 2 }
  }'
}

rss_values=()
pss_values=()
anon_values=()
thread_values=()
fd_values=()

record() {
  local rss pss anon threads fds
  read -r rss pss anon threads fds <<<"$(sample "$serve_pid")"
  rss_values+=("$rss")
  pss_values+=("$pss")
  anon_values+=("$anon")
  thread_values+=("$threads")
  fd_values+=("$fds")
}

before="$(sample "$serve_pid")"
cpu_start_ticks="$(proc_cpu_ticks "$serve_pid")"
wall_start_ns="$(date +%s%N)"

url="http://127.0.0.1:$port/v1/chat/completions"
(
  seq 1 "$requests" | xargs -P "$concurrency" -I{} curl -s -o /dev/null \
    --max-time 120 -H 'Content-Type: application/json' \
    --data-binary "@$work/body.json" -w '%{http_code}\n' "$url"
) >"$work/codes.txt" &
load_pid=$!
while kill -0 "$load_pid" 2>/dev/null; do
  sleep "$sample_seconds"
  record
done
load_status=0
if wait "$load_pid"; then
  load_status=0
else
  load_status=$?
fi
load_pid=""
load_end_ns="$(date +%s%N)"
cpu_end_ticks="$(proc_cpu_ticks "$serve_pid")"
vmhwm_kb="$(proc_vmhwm_kb "$serve_pid")"
if ((${#rss_values[@]} == 0)); then
  record
fi

cpu_ticks=$((cpu_end_ticks - cpu_start_ticks))
if ((cpu_ticks < 0)); then
  cpu_ticks=0
fi
clock_ticks="$(getconf CLK_TCK)"
cpu_ms="$(awk -v ticks="$cpu_ticks" -v hz="$clock_ticks" 'BEGIN { printf "%.1f", ticks * 1000 / hz }')"
wall_ms="$(awk -v start="$wall_start_ns" -v end="$load_end_ns" \
  'BEGIN { printf "%.0f", (end - start) / 1000000 }')"

total="$(awk 'END {print NR + 0}' "$work/codes.txt")"
ok="$(awk '$1 == 200 {count += 1} END {print count + 0}' "$work/codes.txt")"
# Keep the status summary format stable while using awk for counting.
statuses="$(sort "$work/codes.txt" | uniq -c | tr '\n' ' ')"
load_failed=0
if ((load_status != 0 || total != requests || ok != requests)); then
  load_failed=1
fi
sleep "$cooldown_seconds"
after="$(sample "$serve_pid")"

printf 'sample\tVmRSS_kB\tPss_kB\tAnonymous_kB\tthreads\tfds\n'
for index in "${!rss_values[@]}"; do
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$((index + 1))" "${rss_values[index]}" \
    "${pss_values[index]}" "${anon_values[index]}" "${thread_values[index]}" \
    "${fd_values[index]}"
done

printf '\nbefore\t%s\n' "$before"
printf 'after\t%s\n' "$after"
printf 'VmHWM_kB\t%s\n' "$vmhwm_kb"
printf 'cpu_ms\t%s\n' "$cpu_ms"
printf 'wall_ms\t%s\n' "$wall_ms"
printf 'median\t%s\t%s\t%s\t%s\t%s\n' \
  "$(printf '%s\n' "${rss_values[@]}" | median)" \
  "$(printf '%s\n' "${pss_values[@]}" | median)" \
  "$(printf '%s\n' "${anon_values[@]}" | median)" \
  "$(printf '%s\n' "${thread_values[@]}" | median)" \
  "$(printf '%s\n' "${fd_values[@]}" | median)"

printf '\nbody_bytes\t%s\n' "$body_bytes"
printf 'concurrency\t%s\n' "$concurrency"
printf 'requests\t%s\n' "$total"
printf 'successful\t%s\n' "$ok"
printf 'statuses\t%s\n' "$statuses"
printf 'host\t%s\n' "$(uname -srm)"
printf 'listen\thttp://127.0.0.1:%s\n' "$port"
printf 'site\t%s\n' "$site"
awk '/^  (threads|accounts|body|upstream):/' "$work/serve.err" || true

((load_failed == 0)) || die "load failed: expected $requests HTTP 200 responses, got $ok successful responses (curl status $load_status)"
