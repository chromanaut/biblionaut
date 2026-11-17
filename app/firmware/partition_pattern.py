#!/usr/bin/env python3
"""Writes the bytes the partition check expects to find in the fonts partition.

Each four bytes are a number worked out from where they are in the
partition, so that a byte read from the wrong place, or not written, is
found wherever it is. main/partition_check.cpp works out the same numbers.

usage: partition_pattern.py SIZE OUT
"""

import sys
from pathlib import Path

MASK = 0xFFFFFFFF


def word_at(offset: int) -> int:
    """The number at `offset` (a multiple of four), as partition_check.cpp has it."""
    x = offset ^ 0x6A09E667
    x ^= x >> 16
    x = (x * 0x7FEB352D) & MASK
    x ^= x >> 15
    x = (x * 0x846CA68B) & MASK
    x ^= x >> 16
    return x


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    size = int(sys.argv[1], 0)
    out = Path(sys.argv[2])
    with out.open("wb") as file:
        block = 65536
        for start in range(0, size, block):
            words = (word_at(offset) for offset in range(start, min(start + block, size), 4))
            file.write(b"".join(word.to_bytes(4, "little") for word in words))
    return 0


if __name__ == "__main__":
    sys.exit(main())
