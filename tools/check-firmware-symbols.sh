#!/usr/bin/env bash
# Checks that the portable parts (core, epub, draw, layout), as built for the
# firmware, do not call into the C++ library's exception machinery.
#
# The firmware is built without exceptions, so a library function that
# "throws" (`.at()`, `string_view::substr()`, calling a `std::function`,
# growing a `std::vector` or `std::string`) ends in an abort: the device
# crashes and restarts. The portable parts parse files the user supplied, so
# they must report every failure to their caller instead; this looks for the
# tell-tale undefined symbols in their static libraries after a firmware
# build.
#
# The rest of the firmware (reader, hw, the entry point) is not checked. A
# throw there is still a crash, but that code may use the constructs above.
#
#   tools/idf build && tools/check-firmware-symbols.sh
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
build="${1:-$root/app/firmware/build}"

nm="$(find "$root/.esp/tools/tools/riscv32-esp-elf" -name 'riscv32-esp-elf-nm' -type f 2>/dev/null | head -n 1)"
if [ -z "$nm" ]; then
  echo "error: the ESP-IDF toolchain is not installed. Run tools/setup-idf.sh first." >&2
  exit 1
fi

# A part with no sources yet has no library; that is fine.
libraries=()
for part in core epub draw layout; do
  while IFS= read -r library; do
    libraries+=("$library")
  done < <(find "$build/bn/$part" -name '*.a' 2>/dev/null | sort)
done

if [ "${#libraries[@]}" -eq 0 ]; then
  echo "error: no portable-part libraries under $build/bn. Run tools/idf build first." >&2
  exit 1
fi

# std::__throw_*, the throw and catch entry points, and the unwinder.
pattern='std::__throw_|__cxa_throw|__cxa_rethrow|__cxa_allocate_exception|__cxa_begin_catch|__cxa_bad_|_Unwind_'

found="$("$nm" -C -A --undefined-only "${libraries[@]}" 2>/dev/null | grep -E "$pattern" || true)"
if [ -n "$found" ]; then
  echo "error: a portable part calls something that can throw:" >&2
  echo "$found" | sed "s|$build/||" >&2
  echo >&2
  echo "Each line is library:object: symbol. Find the call in that source file that can" >&2
  echo "throw (for example .at() or substr()) and use a form that cannot." >&2
  exit 1
fi

echo "No calls into exception support in the portable parts (libraries checked: ${#libraries[@]})."
