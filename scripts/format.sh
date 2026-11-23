#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
mode="fix"

usage() {
  cat <<'USAGE'
用法: scripts/format.sh [--check|--fix]

  --check  只检查，不修改文件（CI/提交前使用）
  --fix    就地格式化（默认）
  -h, --help
USAGE
}

case "${1:-}" in
  "")
    ;;
  --check)
    mode="check"
    ;;
  --fix)
    mode="fix"
    ;;
  -h | --help)
    usage
    exit 0
    ;;
  *)
    printf 'format: unknown option: %s\n' "$1" >&2
    usage >&2
    exit 2
    ;;
esac

if ! command -v clang-format >/dev/null 2>&1; then
  printf 'format: clang-format not found; run this inside nix develop\n' >&2
  exit 1
fi

# Keep this loop compatible with the Bash 3.2 shipped with macOS.
files=()
while IFS= read -r file; do
  files+=("$file")
done < <(
  find "$root/src" "$root/tests" -type f \
    \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' -o -name '*.hpp' -o -name '*.h' \) \
    | sort
)

if ((${#files[@]} == 0)); then
  printf 'format: no C++ files found under src/ and tests/\n' >&2
  exit 1
fi

style="file:$root/.clang-format"

if [[ "$mode" == "check" ]]; then
  failed=0
  for file in "${files[@]}"; do
    if ! clang-format --dry-run --Werror --style="$style" "$file"; then
      failed=1
    fi
  done
  if ((failed != 0)); then
    printf 'format: check failed; run scripts/format.sh --fix\n' >&2
    exit 1
  fi
  printf 'format: check passed (%d files)\n' "${#files[@]}"
else
  clang-format -i --style="$style" "${files[@]}"
  printf 'format: formatted %d files\n' "${#files[@]}"
fi
