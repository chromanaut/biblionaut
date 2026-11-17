# TeX's hyphenation patterns for American English, made into the table that
# layout/hyphenator.h reads, and compiled in (hyphenation/american_english.h).
#
# None of the patterns are in this repository. CMake downloads the two files
# below when it configures, from the sources of the hyph-utf8 package at the
# commit named, and checks each against its hash. They are the package's
# plain-text form of hyph-en-us.tex, whose header gives the terms: the
# patterns, and the words written out with their breaks. bn-hyphen packs
# them into a table as part of the build, bytes.cmake writes the table out
# as numbers, and american_english.cpp has them as an array.
#
# Whoever adds this directory says which bn-hyphen packs them, in
# BN_HYPHEN: a target or a path. BN_HYPHEN_DEPENDS names anything that has
# to be built first. The host build gives the bn-hyphen it builds (tools/);
# the firmware build compiles one for the computer it is building on
# (app/firmware/CMakeLists.txt).
#
# BN_HYPH_EN_US_DIR is where the two files are kept. A build that has them
# already can point another at them, which then downloads nothing.
#
# To move to another commit: change it and both hashes together. The
# hashes are the SHA-256 of the files as they are at that commit.
if(NOT BN_HYPHEN)
  message(FATAL_ERROR "third_party/hyph-en-us needs BN_HYPHEN: the bn-hyphen that packs the patterns.")
endif()

# Tagged CTAN-2024.12.31.
set(bn_hyph_commit 29e003ea905f6740a0d66e114db9f60b26d07516)
set(bn_hyph_url
    "https://raw.githubusercontent.com/hyphenation/tex-hyphen/${bn_hyph_commit}/hyph-utf8/tex/generic/hyph-utf8/patterns/txt")
set(BN_HYPH_EN_US_DIR "${CMAKE_BINARY_DIR}/_deps/hyph-en-us" CACHE PATH
    "Where the American English hyphenation patterns are downloaded to")

# Downloads `name` unless it is there already with the hash it should have.
function(bn_hyph_fetch name hash)
  set(path "${BN_HYPH_EN_US_DIR}/${name}")
  if(EXISTS "${path}")
    file(SHA256 "${path}" have)
    if(have STREQUAL hash)
      return()
    endif()
  endif()
  message(STATUS "Downloading ${name} (hyph-utf8 at ${bn_hyph_commit})")
  file(DOWNLOAD "${bn_hyph_url}/${name}" "${path}" EXPECTED_HASH SHA256=${hash} STATUS status)
  list(GET status 0 code)
  if(NOT code EQUAL 0)
    list(GET status 1 why)
    message(FATAL_ERROR "Could not download ${bn_hyph_url}/${name}: ${why}")
  endif()
endfunction()

bn_hyph_fetch(hyph-en-us.pat.txt 0f57318b878b132547ae92db39a6e1d1cf2a05d9008874955d6ecb910007a463)
bn_hyph_fetch(hyph-en-us.hyp.txt 5e785c27e151f878c312991d937b5582615b77737ab30bac413d5967d6aa21da)

# The fewest letters before a break and after one are the file's own
# (hyphenmins: 2 and 3).
set(bn_hyph_table "${CMAKE_CURRENT_BINARY_DIR}/american_english.bnhy")
set(bn_hyph_bytes "${CMAKE_CURRENT_BINARY_DIR}/american_english_bytes.inc")
add_custom_command(
  OUTPUT "${bn_hyph_table}" "${bn_hyph_bytes}"
  COMMAND ${BN_HYPHEN} pack "${BN_HYPH_EN_US_DIR}/hyph-en-us.pat.txt"
          --exceptions "${BN_HYPH_EN_US_DIR}/hyph-en-us.hyp.txt" --left 2 --right 3 --out "${bn_hyph_table}"
  COMMAND ${CMAKE_COMMAND} "-DIN=${bn_hyph_table}" "-DOUT=${bn_hyph_bytes}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/bytes.cmake"
  DEPENDS ${BN_HYPHEN} ${BN_HYPHEN_DEPENDS}
          "${BN_HYPH_EN_US_DIR}/hyph-en-us.pat.txt" "${BN_HYPH_EN_US_DIR}/hyph-en-us.hyp.txt"
          "${CMAKE_CURRENT_SOURCE_DIR}/bytes.cmake"
  COMMENT "Packing the American English hyphenation patterns"
  VERBATIM)

add_library(bn_hyph_en_us STATIC american_english.cpp "${bn_hyph_bytes}")
target_include_directories(bn_hyph_en_us PUBLIC include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(bn_hyph_en_us PRIVATE bn_options)
# The table as a file, for whatever wants to hand the same one to bn-lines
# or bn-hyphen (the end-to-end tests do).
set_target_properties(bn_hyph_en_us PROPERTIES BN_TABLE_FILE "${bn_hyph_table}")
