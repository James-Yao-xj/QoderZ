#!/usr/bin/env bash
# Load footprint of `qoderz serve`: 400 KB text bodies, 100 requests, 8 at a time, driven by curl.
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="${QODERZ_PERF_BUILD_DIR:-$root/build-perf}"
jobs="${QODERZ_PERF_JOBS:-6}"
site="${QODERZ_PERF_SITE:-qoder-cn}"
requests="${QODERZ_PERF_REQUESTS:-100}"
concurrency="${QODERZ_PERF_CONCURRENCY:-8}"
body_text_bytes="${QODERZ_PERF_BODY_BYTES:-409600}"
bin="$build/qoderz"
stub="$build/tests/qoderz_stub_upstream"

log() { printf '%s\n' "$*" >&2; }
die() {
  printf 'measure-load: %s\n' "$*" >&2
  exit 1
}

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
  local pid="$1" rss pss anon threads fds
  rss="$(awk '/^VmRSS:/{print $2}' "/proc/$pid/status")"
  read -r pss anon < <(awk '/^Pss:/{p=$2} /^Anonymous:/{a=$2} END{print p+0, a+0}' \
    "/proc/$pid/smaps_rollup")
  threads="$(awk '/^Threads:/{print $2}' "/proc/$pid/status")"
  fds="$(find "/proc/$pid/fd" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)"
  printf '%s %s %s %s %s\n' "$rss" "$pss" "$anon" "$threads" "$fds"
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

url="http://127.0.0.1:$port/v1/chat/completions"
(
  seq 1 "$requests" | xargs -P "$concurrency" -I{} curl -s -o /dev/null \
    --max-time 120 -H 'Content-Type: application/json' \
    --data-binary "@$work/body.json" -w '%{http_code}\n' "$url"
) >"$work/codes.txt" &
load_pid=$!
while kill -0 "$load_pid" 2>/dev/null; do
  sleep 0.2
  record
done
wait "$load_pid" || true
load_pid=""
if ((${#rss_values[@]} == 0)); then
  record
fi

total="$(wc -l <"$work/codes.txt")"
ok="$(grep -c '^200$' "$work/codes.txt" || true)"
sleep 1
after="$(sample "$serve_pid")"

printf 'sample\tVmRSS_kB\tPss_kB\tAnonymous_kB\tthreads\tfds\n'
for index in "${!rss_values[@]}"; do
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$((index + 1))" "${rss_values[index]}" \
    "${pss_values[index]}" "${anon_values[index]}" "${thread_values[index]}" \
    "${fd_values[index]}"
done

printf '\nbefore\t%s\n' "$before"
printf 'after\t%s\n' "$after"
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
printf 'statuses\t%s\n' "$(sort "$work/codes.txt" | uniq -c | tr '\n' ' ')"
printf 'host\t%s\n' "$(uname -srm)"
printf 'listen\thttp://127.0.0.1:%s\n' "$port"
printf 'site\t%s\n' "$site"
grep -E '^  (threads|accounts|body|upstream):' "$work/serve.err" || true
