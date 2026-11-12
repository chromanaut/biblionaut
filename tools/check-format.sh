#!/usr/bin/env bash
# Checks that every C++ source file is formatted the way clang-format would
# format it (.clang-format at the repository root).
#
#   tools/check-format.sh         report files that differ, exit 1 if any
#   tools/check-format.sh --fix   rewrite them in place
#
# clang-format's output changes between major versions, so the major version
# is pinned in tools/llvm-version. Set CLANG_FORMAT to use a specific binary.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
major="$(tr -d '[:space:]' < "$root/tools/llvm-version")"

major_of() {
  "$1" --version 2>/dev/null | sed -n 's/.*version \([0-9][0-9]*\)\..*/\1/p' | head -n 1
}

clang_format=""
found=""
for candidate in "${CLANG_FORMAT:-}" "clang-format-$major" "/opt/homebrew/opt/llvm@$major/bin/clang-format" \
  "/opt/homebrew/opt/llvm/bin/clang-format" "/usr/local/opt/llvm@$major/bin/clang-format" clang-format; do
  [ -n "$candidate" ] || continue
  command -v "$candidate" >/dev/null 2>&1 || continue
  version="$(major_of "$candidate")"
  if [ "$version" = "$major" ]; then
    clang_format="$candidate"
    break
  fi
  found="$found $candidate($version)"
done

if [ -z "$clang_format" ]; then
  echo "error: clang-format $major is required (tools/llvm-version).${found:+ Found:$found.}" >&2
  exit 1
fi

cd "$root"
files=()
while IFS= read -r -d '' file; do
  [ -f "$file" ] && files+=("$file")
done < <(git ls-files -z --cached --others --exclude-standard -- '*.h' '*.cpp')

if [ "${#files[@]}" -eq 0 ]; then
  echo "No C++ sources found."
  exit 0
fi

if [ "${1:-}" = "--fix" ]; then
  "$clang_format" -i "${files[@]}"
  echo "Formatted ${#files[@]} files."
else
  "$clang_format" --dry-run --Werror "${files[@]}"
  echo "${#files[@]} files are formatted correctly."
fi
