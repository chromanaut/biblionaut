# Firmware entry point: app_main().
idf_component_register(
  SRCS "app_main.cpp"
  PRIV_REQUIRES bn_hw esp_rom esp_system freertos)

# bn_reader, bn_core and bn_core_runtime are defined by the project after
# components are registered; CMake resolves the names when it generates.
target_link_libraries(${COMPONENT_LIB} PRIVATE bn_reader bn_core)

# The operator new / delete replacements go into this component as objects,
# and app_main() references them, so they are always part of the image.
target_sources(${COMPONENT_LIB} PRIVATE $<TARGET_OBJECTS:bn_core_runtime>)

# A firmware that crashes on purpose (see BN_CRASH_TEST in the project file).
# The code for it, crash_test.cpp, is only part of such a build.
if(BN_CRASH_TEST)
  set(bn_crash_kinds fault abort oom stack hang)
  list(FIND bn_crash_kinds "${BN_CRASH_TEST}" bn_crash_kind)
  if(bn_crash_kind EQUAL -1)
    message(FATAL_ERROR "BN_CRASH_TEST must be one of: ${bn_crash_kinds} (or empty), not '${BN_CRASH_TEST}'")
  endif()
  message(WARNING "This firmware will crash on purpose (${BN_CRASH_TEST}) ten seconds after it starts. "
                  "Configure with -DBN_CRASH_TEST= for a normal build.")
  target_sources(${COMPONENT_LIB} PRIVATE crash_test.cpp)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    BN_CRASH_TEST=${bn_crash_kind} "BN_CRASH_TEST_NAME=\"${BN_CRASH_TEST}\"")
endif()

# A firmware that checks the panel's planes (see BN_PLANE_CHECK in the
# project file).
if(BN_PLANE_CHECK)
  message(WARNING "This firmware reads the panel's planes back whenever it comes to rest. "
                  "Configure with -DBN_PLANE_CHECK=OFF for a normal build.")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE BN_PLANE_CHECK=1)
endif()

# A firmware that checks the SD card (see BN_STORAGE_CHECK in the project
# file).
if(BN_STORAGE_CHECK)
  message(WARNING "This firmware writes to the SD card at start and at each press of Power. "
                  "Configure with -DBN_STORAGE_CHECK=OFF for a normal build.")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE BN_STORAGE_CHECK=1)
endif()

# A firmware that reads the books on the SD card (see BN_BOOK_CHECK in the
# project file).
if(BN_BOOK_CHECK)
  if(BN_STORAGE_CHECK)
    message(FATAL_ERROR "BN_BOOK_CHECK and BN_STORAGE_CHECK would get in each other's way on the card: "
                        "configure with one of them OFF.")
  endif()
  message(WARNING "This firmware reads every book on the SD card at start and at each press of Power. "
                  "Configure with -DBN_BOOK_CHECK=OFF for a normal build.")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE BN_BOOK_CHECK=1)
  if(BN_BOOK_CHECK_BRIEF)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BN_BOOK_CHECK_BRIEF=1)
  endif()
elseif(BN_BOOK_CHECK_BRIEF)
  message(FATAL_ERROR "BN_BOOK_CHECK_BRIEF is a kind of book check: configure with -DBN_BOOK_CHECK=ON as well, "
                      "or with -DBN_BOOK_CHECK_BRIEF=OFF.")
endif()

# A firmware that times the layout code (see BN_LAYOUT_CHECK in the project
# file).
if(BN_LAYOUT_CHECK)
  if(BN_STORAGE_CHECK OR BN_BOOK_CHECK)
    message(FATAL_ERROR "BN_LAYOUT_CHECK would be timed while BN_STORAGE_CHECK or BN_BOOK_CHECK took the card's "
                        "time: configure with the others OFF.")
  endif()
  message(WARNING "This firmware times the layout code at start and at each press of Power. "
                  "Configure with -DBN_LAYOUT_CHECK=OFF for a normal build.")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE BN_LAYOUT_CHECK=1)
endif()
