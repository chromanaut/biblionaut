# Copies the fonts image IMAGE to OUT for flashing, if it is one and the
# fonts partition, of PARTITION_BYTES, has room for it:
#
#   cmake -DIMAGE=... -DOUT=... -DPARTITION_BYTES=... -P fonts_image.cmake
#
# PARTITION_BYTES is as ESP-IDF's partition tool prints a size: in hex,
# with 0x before it.
file(SIZE "${IMAGE}" bn_image_bytes)
math(EXPR bn_partition_bytes "${PARTITION_BYTES}")
if(bn_image_bytes GREATER bn_partition_bytes)
  message(FATAL_ERROR "The fonts image ${IMAGE} is ${bn_image_bytes} bytes, and the fonts partition has "
                      "${bn_partition_bytes}.")
endif()
# "BNFN", as hex.
file(READ "${IMAGE}" bn_image_magic LIMIT 4 HEX)
if(NOT bn_image_magic STREQUAL "424e464e")
  message(FATAL_ERROR "${IMAGE} is not a fonts image: it does not begin with BNFN. bn-font makes one.")
endif()
file(COPY_FILE "${IMAGE}" "${OUT}" ONLY_IF_DIFFERENT)
message(STATUS "The fonts image: ${bn_image_bytes} bytes of the partition's ${bn_partition_bytes}.")
