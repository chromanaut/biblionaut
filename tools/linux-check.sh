#!/usr/bin/env bash
# Runs CI's two Linux host jobs here, in Docker, with GCC 14: the debug
# build with the address and undefined-behaviour sanitizers and its tests,
# then the optimised build without sanitizers and the same tests. As in CI,
# the tests run one to a core.
#
# Worth running before pushing anything that touches memory or arithmetic.
# GCC and the Linux C library catch things that the macOS build cannot, such
# as a null pointer handed to memset, and a finding fails the test.
#
#   tools/linux-check.sh                 # build and run every test
#   tools/linux-check.sh -R TestPattern  # arguments go to ctest
#
# The source tree is mounted read-only. The builds live in two Docker
# volumes, so only the first run builds everything. They are named for the
# checkout's directory, biblionaut-linux-build and
# biblionaut-linux-build-release for a checkout in `biblionaut`, and a run
# says which they are: remove one for a clean build, and both when the
# checkout is done with.
#
# Each checkout has volumes of its own because a build is only good for the
# tree it was made from. Ninja builds again what is older than its source,
# and a file one checkout last changed a week ago is older than what was
# built from another checkout yesterday: given that build, nothing would be
# built, and the tests run would be the other checkout's. So a volume also
# records which checkout it was built from, and is emptied before it is
# used for any other.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
image="biblionaut-linux-check"
# Docker takes letters, digits, "_", "." and "-" in a volume's name, and a
# letter or a digit first.
checkout="$(basename "$root")"
checkout="${checkout//[^a-zA-Z0-9_.-]/-}"
[[ "$checkout" =~ ^[a-zA-Z0-9] ]] || checkout="checkout$checkout"
volume="$checkout-linux-build"

if ! docker info >/dev/null 2>&1; then
  echo "error: Docker is not running." >&2
  exit 1
fi

# One heavy check at a time on a machine: this waits for its turn
# (tools/queued says how), and carries on from here when it has it.
if [[ ":${BN_QUEUE_HELD:-}:" != *":heavy:"* ]]; then
  exec "$root/tools/queued" "$0" "$@"
fi

# The image is CI's Ubuntu release with the same compiler. Docker keeps it
# after the first build.
docker build --quiet --tag "$image" - >/dev/null <<'DOCKERFILE'
FROM ubuntu:24.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates cmake ninja-build g++-14 python3 zip zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*
ENV CC=gcc-14 CXX=g++-14
DOCKERFILE

echo "linux-check: the builds are in the Docker volumes $volume and $volume-release." >&2
exec docker run --rm \
  --volume "$root":/src:ro \
  --volume "$volume":/build \
  --volume "$volume-release":/build-release \
  --env BN_CHECKOUT="$root" \
  --workdir /src \
  "$image" \
  bash -c 'for build in /build /build-release; do
      if [ "$(cat "$build/.checkout" 2>/dev/null)" != "$BN_CHECKOUT" ]; then
        find "$build" -mindepth 1 -delete && printf "%s\n" "$BN_CHECKOUT" > "$build/.checkout" || exit 1
      fi
    done
    cmake --preset host -B /build >/dev/null && cmake --build /build \
    && ctest --test-dir /build --output-on-failure --parallel "$(nproc)" "$@" \
    && cmake --preset host-release -B /build-release >/dev/null && cmake --build /build-release \
    && ctest --test-dir /build-release --output-on-failure --parallel "$(nproc)" "$@"' \
  linux-check "$@"
