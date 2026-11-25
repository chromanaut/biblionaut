#!/usr/bin/env bash
# Runs clang-tidy over the host build: every source file of ours is compiled
# with it looking on, and a finding is an error (.clang-tidy says which
# checks). CI runs this.
#
#   tools/check-tidy.sh                 # configure and build the `tidy` preset
#   tools/check-tidy.sh -DNAME=VALUE    # arguments go to the configuring
#
# It needs the clang-tidy of the LLVM version in tools/llvm-version, which
# the preset finds where Homebrew puts it; elsewhere, say where it is:
# -DBN_CLANG_TIDY_EXE=/path/to/clang-tidy.
#
# It takes every processor for some minutes, so it waits for its turn among
# the heavy checks on the machine (tools/queued says how).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"

if [[ ":${BN_QUEUE_HELD:-}:" != *":heavy:"* ]]; then
  exec "$root/tools/queued" "$0" "$@"
fi

cd "$root"
cmake --preset tidy "$@"
cmake --build --preset tidy
