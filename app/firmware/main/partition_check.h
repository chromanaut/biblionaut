#pragma once

// The partition check: a firmware that finds out on a device whether the
// data partition for fonts in the partition table (partitions.csv) is
// there, and what reading fonts out of flash costs. It is chosen when
// configuring the build:
//
//   tools/idf -DBN_PARTITION_CHECK=ON build flash
//
// Flashing it fills the fonts partition with numbers worked out from where
// each is (partition_pattern.py), in place of any fonts image. At start,
// before the application, it says on the console:
//
//   - how large the flash chip says it is, and how large the build takes it
//     to be;
//   - the partitions the table has;
//   - whether the whole of the fonts partition can be mapped into the
//     processor's address space at once, and if not, the most of it that
//     can be, and how many pages of that space were free;
//   - how long mapping and unmapping a window of it takes;
//   - how fast the whole partition is read through windows of 1 MB, and
//     whether every word in it is the number it should be;
//   - how long a read of 64 bytes takes at places all over a large window,
//     where the cache seldom has them, and over and over in a small one,
//     where it does.
//
// Then the application starts as usual. In a normal build this does nothing
// and partition_check.cpp is not compiled, so the entry point calls it
// unconditionally.

namespace bn::firmware {

#ifdef BN_PARTITION_CHECK

void RunPartitionCheck() noexcept;

#else

inline void RunPartitionCheck() noexcept {}

#endif

}  // namespace bn::firmware
