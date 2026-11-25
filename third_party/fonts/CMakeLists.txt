# The font files of the presets (tools/font/presets): the typefaces a fonts
# image can be made of without anyone bringing fonts of their own. Each is
# under the SIL Open Font License 1.1, and THIRD_PARTY.md has what that asks
# of a firmware that is given out with an image made of them.
#
# None of them is in this repository, and none is downloaded when a build is
# configured: a file is downloaded when something that needs it is built
# (the `fonts` target, tools/font/CMakeLists.txt), from its typeface's own
# repository at the commit named, and checked against its hash.
#
# BN_FONTS_DIR is where they are kept, each family in a folder of its own. A
# build that has them already can point another at them.
#
# What this gives whoever adds it: for each family below, a list of its
# files as they are once downloaded, BN_FONT_FILES_<family>.
#
# To move a family to another commit: change the commit and the hashes of
# its files together. The hashes are the SHA-256 of the files as they are
# at that commit.
set(BN_FONTS_DIR "${CMAKE_BINARY_DIR}/_deps/fonts" CACHE PATH "Where the presets' font files are downloaded to")

# One file of `family`, downloaded to its folder when it is needed.
function(bn_font_file family url hash)
  get_filename_component(name "${url}" NAME)
  set(path "${BN_FONTS_DIR}/${family}/${name}")
  add_custom_command(
    OUTPUT "${path}"
    COMMAND ${CMAKE_COMMAND} "-DURL=${url}" "-DOUT=${path}" "-DHASH=${hash}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/fetch.cmake"
    DEPENDS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/fetch.cmake"
    COMMENT "Downloading ${family}/${name}"
    VERBATIM)
  set(files ${BN_FONT_FILES_${family}})
  list(APPEND files "${path}")
  set(BN_FONT_FILES_${family} "${files}" PARENT_SCOPE)
endfunction()


# Noto Serif, Noto Sans and Noto Sans Mono.
set(bn_noto_url "https://raw.githubusercontent.com/notofonts/notofonts.github.io/d14cc173a70dc57962ed2d4ad278fc9df83bf9f3")
bn_font_file(noto "${bn_noto_url}/fonts/NotoSerif/unhinted/ttf/NotoSerif-Regular.ttf"
             a15cfbbc1539d707115111d672d590a3d70d4f74b4c0a315956da20ae19a14e1)
bn_font_file(noto "${bn_noto_url}/fonts/NotoSerif/unhinted/ttf/NotoSerif-Italic.ttf"
             c4b3c971741ecdb40f5a443bce754e8fe91efe761b6ea10c92be7c3597cdadc4)
bn_font_file(noto "${bn_noto_url}/fonts/NotoSerif/unhinted/ttf/NotoSerif-Bold.ttf"
             24ad531e6b05ddad8c3d89572d2c93eb86a6b74e652ce7ee3c3e171de68e84c3)
bn_font_file(noto "${bn_noto_url}/fonts/NotoSerif/unhinted/ttf/NotoSerif-BoldItalic.ttf"
             1bc4f86502eaa368718f6192bee022ea9a703d5af1c18b7d291212657b63074a)
bn_font_file(noto "${bn_noto_url}/fonts/NotoSans/unhinted/ttf/NotoSans-Regular.ttf"
             f3961a9cde016d41a4879aecda1474d3a36d6bf54fa0e4643de029cc2248b0e8)
bn_font_file(noto "${bn_noto_url}/fonts/NotoSans/unhinted/ttf/NotoSans-Italic.ttf"
             678288f868807d4d64a6f3b51466871d117d915780381ce9d0ed4b3bcbd06d37)
bn_font_file(noto "${bn_noto_url}/fonts/NotoSans/unhinted/ttf/NotoSans-Bold.ttf"
             87cb2d84472a7d66da659ee47b6cdb9552326e8c128245231f191b6ac72529d9)
bn_font_file(noto "${bn_noto_url}/fonts/NotoSans/unhinted/ttf/NotoSans-BoldItalic.ttf"
             3d367743f371f28671d2764e911a53d7c20ec9b6aa8791d059e7090389fc52a5)
bn_font_file(noto "${bn_noto_url}/fonts/NotoSansMono/unhinted/ttf/NotoSansMono-Regular.ttf"
             87f8ce0522a6c99b743ee5fc75b4073cfdd575639119672828b7b9944b65b4f4)
bn_font_file(noto "${bn_noto_url}/fonts/NotoSansMono/unhinted/ttf/NotoSansMono-Bold.ttf"
             1100772b2f79c102402a1011df5e2226517d0f40ac553a25fb12fdbad8c11b85)

# IBM Plex Serif, Sans and Mono.
set(bn_plex_url "https://raw.githubusercontent.com/IBM/plex/ba3013d72a8419a22c758cc51c44e04ac7ea565b")
bn_font_file(plex "${bn_plex_url}/packages/plex-serif/fonts/complete/ttf/IBMPlexSerif-Regular.ttf"
             6c490ccd38e856c674e0d163f3f40742a75058b7b935998dbfbc1745ba54e474)
bn_font_file(plex "${bn_plex_url}/packages/plex-serif/fonts/complete/ttf/IBMPlexSerif-Italic.ttf"
             d28aa63cf618439712d6b459c5649af1d6d789cb911994f2357f1ce44128f163)
bn_font_file(plex "${bn_plex_url}/packages/plex-serif/fonts/complete/ttf/IBMPlexSerif-Bold.ttf"
             5fa66833dc80adb6252c793adb555716ec10833d2b1e5b0235b68c72cd69adf0)
bn_font_file(plex "${bn_plex_url}/packages/plex-serif/fonts/complete/ttf/IBMPlexSerif-BoldItalic.ttf"
             e33c9d0a692bb26c9c1ece4a35fd2a351317f5a60009d0e9945ae30d0297bf42)
bn_font_file(plex "${bn_plex_url}/packages/plex-sans/fonts/complete/ttf/IBMPlexSans-Regular.ttf"
             b47ebd72b5ce53b60f909033ff84d6fc12358cc147e2981f8c321ad3f36246b6)
bn_font_file(plex "${bn_plex_url}/packages/plex-sans/fonts/complete/ttf/IBMPlexSans-Italic.ttf"
             fbcfd6c96fb6188d3d65498e6f8e9719c2a3c0c2932e09255b01bd6d7c78ca12)
bn_font_file(plex "${bn_plex_url}/packages/plex-sans/fonts/complete/ttf/IBMPlexSans-Bold.ttf"
             22f59d358e8b9b9fb38ac6ee26a7ea7263ffa1c89a9056c101acff5add91f5d6)
bn_font_file(plex "${bn_plex_url}/packages/plex-sans/fonts/complete/ttf/IBMPlexSans-BoldItalic.ttf"
             701f8c2227fac11c2b010196b205307c30d7bd582b91a806461e7e963656cbe0)
bn_font_file(plex "${bn_plex_url}/packages/plex-mono/fonts/complete/ttf/IBMPlexMono-Regular.ttf"
             7c6fbddca4b700be918f5f6183d9bd4464fa427fe435f0b480d77fe2bb8c5a43)
bn_font_file(plex "${bn_plex_url}/packages/plex-mono/fonts/complete/ttf/IBMPlexMono-Bold.ttf"
             74e5eedcfa4596497d34e19023cabdabd3a8c852b903007a5654a59591a72ffb)

# Source Serif 4, Source Sans 3 and Source Code Pro.
set(bn_source_serif_url "https://raw.githubusercontent.com/adobe-fonts/source-serif/80d3f8894c09c937bebfa9011247d2e1c79fd6f4")
bn_font_file(source "${bn_source_serif_url}/TTF/SourceSerif4-Regular.ttf"
             e5a4ee6a3d87bb9024796be390c6771e2a0eb1883dae25effaf57ca01668e24b)
bn_font_file(source "${bn_source_serif_url}/TTF/SourceSerif4-It.ttf"
             9d2950a8f1da66e21502c35d646a1d2148e79f9ea43fd2158cf02f5232e7f430)
bn_font_file(source "${bn_source_serif_url}/TTF/SourceSerif4-Bold.ttf"
             7cf4f4e1ad74f45058d5bc61716b82560442fbdcd9d3654d2dea96bf6c683d86)
bn_font_file(source "${bn_source_serif_url}/TTF/SourceSerif4-BoldIt.ttf"
             7b215b37f8873f5579f3f8d2ded3ca7c588e2f435cd996605ebfc5befe2cd5eb)
set(bn_source_sans_url "https://raw.githubusercontent.com/adobe-fonts/source-sans/87b37a2daaed80fcb8e8ccb0085c4d72ddade12e")
bn_font_file(source "${bn_source_sans_url}/TTF/SourceSans3-Regular.ttf"
             4644c81b86ec9caaa76b634889968ed3c4f4f52f054855933acc7c2b21e53b0f)
bn_font_file(source "${bn_source_sans_url}/TTF/SourceSans3-It.ttf"
             192afd78f0f54a3c69eaf02d43f4d9a821e9d6110e41d3d25d61a7385cd580e4)
bn_font_file(source "${bn_source_sans_url}/TTF/SourceSans3-Bold.ttf"
             9214b9d95e4231c609802815c2646c98174e2102d0d37f88978a7f8e71006e6a)
bn_font_file(source "${bn_source_sans_url}/TTF/SourceSans3-BoldIt.ttf"
             7978291fc1bf314db887e0366853b33c5cf2e964c7b95cfb9ce403a6ec46a842)
set(bn_source_code_url "https://raw.githubusercontent.com/adobe-fonts/source-code-pro/803b7e23ec97ae58b6232ea76519a76d428ba268")
bn_font_file(source "${bn_source_code_url}/TTF/SourceCodePro-Regular.ttf"
             74bd80d3e42a08517cd7e1108ba3d86f2da29ac0f3065be95e0357956ab9db37)
bn_font_file(source "${bn_source_code_url}/TTF/SourceCodePro-Bold.ttf"
             b2095e0d657e6d28dc32444a9dacabab0c9241d0bf39d96371756cc9bdbc3a5f)

# Courier Prime.
set(bn_courier_url "https://raw.githubusercontent.com/quoteunquoteapps/CourierPrime/7fd585a2dd4c1612c79b3308e300923d1c13df93")
bn_font_file(courier-prime "${bn_courier_url}/fonts/ttf/CourierPrime-Regular.ttf"
             72f793376f8e2841656bf21d77a5de010f2929bd6956a22ee848ad0c7eb978af)
bn_font_file(courier-prime "${bn_courier_url}/fonts/ttf/CourierPrime-Bold.ttf"
             ff1f38786c849d1c41fa8e447960abdb2bd75fdfb0cfcdeb524fad65a5af3638)

# A target for each family, so that the downloads above are part of the
# build (bn_font_files_noto and so on); and the lists, for whoever added
# this directory.
foreach(family noto plex source courier-prime)
  add_custom_target(bn_font_files_${family} DEPENDS ${BN_FONT_FILES_${family}})
  set(BN_FONT_FILES_${family} "${BN_FONT_FILES_${family}}" PARENT_SCOPE)
endforeach()
