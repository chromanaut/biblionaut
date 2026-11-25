# Courier Prime (https://quoteunquoteapps.com/courierprime), a monospace
# typeface under the SIL Open Font License 1.1: the font files the tests
# give bn-font (tools/font) to draw, so that the font builder is tried on a
# real font.
#
# None of it is in this repository. CMake downloads the files below when it
# configures, from the typeface's own repository at the commit named, and
# checks each against its hash: the regular and the bold.
#
# BN_COURIER_PRIME_DIR is where the files are kept. A build that has them
# already can point another at them, which then downloads nothing.
#
# To move to another commit: change it and the hashes together. The hashes
# are the SHA-256 of the files as they are at that commit.
set(bn_courier_commit 7fd585a2dd4c1612c79b3308e300923d1c13df93)
set(bn_courier_url "https://raw.githubusercontent.com/quoteunquoteapps/CourierPrime/${bn_courier_commit}")
set(BN_COURIER_PRIME_DIR "${CMAKE_BINARY_DIR}/_deps/courier-prime" CACHE PATH
    "Where the Courier Prime font files are downloaded to")

# Downloads `from`, as `name`, unless it is there already with the hash it
# should have.
function(bn_courier_fetch from name hash)
  set(path "${BN_COURIER_PRIME_DIR}/${name}")
  if(EXISTS "${path}")
    file(SHA256 "${path}" have)
    if(have STREQUAL hash)
      return()
    endif()
  endif()
  message(STATUS "Downloading ${name} (Courier Prime at ${bn_courier_commit})")
  file(DOWNLOAD "${bn_courier_url}/${from}" "${path}" EXPECTED_HASH SHA256=${hash} STATUS status)
  list(GET status 0 code)
  if(NOT code EQUAL 0)
    list(GET status 1 why)
    message(FATAL_ERROR "Could not download ${bn_courier_url}/${from}: ${why}")
  endif()
endfunction()

bn_courier_fetch(fonts/ttf/CourierPrime-Regular.ttf CourierPrime-Regular.ttf
                 72f793376f8e2841656bf21d77a5de010f2929bd6956a22ee848ad0c7eb978af)
bn_courier_fetch(fonts/ttf/CourierPrime-Bold.ttf CourierPrime-Bold.ttf
                 ff1f38786c849d1c41fa8e447960abdb2bd75fdfb0cfcdeb524fad65a5af3638)
