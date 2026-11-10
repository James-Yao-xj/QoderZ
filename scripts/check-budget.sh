#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
warn_threshold=600
fail_threshold=800

count_effective_lines() {
  awk '
    BEGIN { in_comment = 0; count = 0 }
    {
      line = $0

      if (in_comment) {
        close_pos = index(line, "*/")
        if (close_pos == 0) {
          next
        }
        line = substr(line, close_pos + 2)
        in_comment = 0
      }

      while ((open_pos = index(line, "/*")) > 0) {
        rest = substr(line, open_pos + 2)
        close_pos = index(rest, "*/")
        if (close_pos == 0) {
          line = substr(line, 1, open_pos - 1)
          in_comment = 1
          break
        }
        line = substr(line, 1, open_pos - 1) substr(line, open_pos + close_pos + 3)
      }

      sub(/\/\/.*$/, "", line)
      gsub(/^[ \t\r]+/, "", line)
      gsub(/[ \t\r]+$/, "", line)
      if (line != "") {
        count++
      }
    }
    END { print count }
  ' "$1"
}

mapfile -t files < <(
  find "$root" -type f \
    \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' -o -name '*.hpp' -o -name '*.h' \) \
    -not -path "$root/build/*" \
    -not -path "$root/build-*/*" \
    -not -path "$root/result/*" \
    -not -path "$root/result-*/*" \
    -not -path "$root/.git/*" \
    -not -path "$root/.direnv/*" \
    | sort
)

if ((${#files[@]} == 0)); then
  printf 'budget: no code files found under %s\n' "$root" >&2
  exit 1
fi

max_lines=0
max_file=""
warnings=0
failures=0

for file in "${files[@]}"; do
  lines="$(count_effective_lines "$file")"
  relative="${file#"$root/"}"

  if ((lines > max_lines)); then
    max_lines=$lines
    max_file=$relative
  fi

  if ((lines > fail_threshold)); then
    printf 'ERROR  %s: %d effective lines (> %d)\n' "$relative" "$lines" "$fail_threshold" >&2
    failures=$((failures + 1))
  elif ((lines > warn_threshold)); then
    printf 'WARN   %s: %d effective lines (> %d, needs justification)\n' \
      "$relative" "$lines" "$warn_threshold" >&2
    warnings=$((warnings + 1))
  fi
done

printf 'budget: checked %d code files; max: %s (%d effective lines)\n' \
  "${#files[@]}" "$max_file" "$max_lines"

if ((failures > 0)); then
  printf 'budget: %d file(s) exceed the hard limit of %d lines\n' \
    "$failures" "$fail_threshold" >&2
  exit 1
fi

if ((warnings > 0)); then
  printf 'budget: %d file(s) exceed the 600-line target; justify or split them\n' \
    "$warnings" >&2
fi
