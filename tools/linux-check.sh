#!/usr/bin/env bash
# Runs CI's Linux host job here, in Docker, with GCC 14: the debug build
# with the address and undefined-behaviour sanitizers and its tests, then
# the optimised build without sanitizers and the same tests.
#
# Worth running before pushing anything that touches memory or arithmetic.
# GCC and the Linux C library catch things that the macOS build cannot, such
# as a null pointer handed to memset, and a finding fails the test.
#
#   tools/linux-check.sh                 # build and run every test
#   tools/linux-check.sh -R TestPattern  # arguments go to ctest
#
# The source tree is mounted read-only. The builds live in Docker volumes
# (biblionaut-linux-build and biblionaut-linux-build-release), so only the
# first run builds everything; remove a volume for a clean build.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
image="biblionaut-linux-check"

if ! docker info >/dev/null 2>&1; then
  echo "error: Docker is not running." >&2
  exit 1
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

exec docker run --rm \
  --volume "$root":/src:ro \
  --volume biblionaut-linux-build:/build \
  --volume biblionaut-linux-build-release:/build-release \
  --workdir /src \
  "$image" \
  bash -c 'cmake --preset host -B /build >/dev/null && cmake --build /build && ctest --test-dir /build --output-on-failure "$@" \
    && cmake --preset host-release -B /build-release >/dev/null && cmake --build /build-release \
    && ctest --test-dir /build-release --output-on-failure "$@"' \
  linux-check "$@"
