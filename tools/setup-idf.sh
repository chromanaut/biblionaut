#!/usr/bin/env bash
# Installs the pinned ESP-IDF version, and the compiler and Python environment
# it needs, into .esp/ inside this repository. Nothing is written outside the
# repository; delete .esp/ to remove it all.
#
# To move to another ESP-IDF version: change IDF_VERSION, delete .esp/esp-idf,
# and run this again.
set -euo pipefail

IDF_VERSION="v6.0.1"

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
idf_dir="$root/.esp/esp-idf"
export IDF_TOOLS_PATH="$root/.esp/tools"

if [ -d "$idf_dir/.git" ]; then
  found="$(git -C "$idf_dir" describe --tags --exact-match 2>/dev/null || echo unknown)"
  if [ "$found" != "$IDF_VERSION" ]; then
    echo "error: $idf_dir holds ESP-IDF $found, but $IDF_VERSION is wanted." >&2
    echo "Delete .esp/esp-idf and run this script again." >&2
    exit 1
  fi
  echo "ESP-IDF $IDF_VERSION is already cloned."
else
  mkdir -p "$root/.esp"
  git clone -b "$IDF_VERSION" --recursive --depth 1 --shallow-submodules \
    https://github.com/espressif/esp-idf.git "$idf_dir"
fi

"$idf_dir/install.sh" esp32c3

echo
echo "Done. Build the firmware with: tools/idf build"
