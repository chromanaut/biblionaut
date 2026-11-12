# Sets BN_VERSION_STRING, for example "0.1.0-dev+5e068ca". The commit is read
# when CMake configures, so it goes stale until the next configure.
set(BN_VERSION_BASE "0.1.0")

find_package(Git QUIET)
set(_bn_commit "unknown")
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
    WORKING_DIRECTORY "${BN_ROOT}"
    OUTPUT_VARIABLE _bn_commit_out
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _bn_commit_result)
  if(_bn_commit_result EQUAL 0 AND _bn_commit_out)
    set(_bn_commit "${_bn_commit_out}")
  endif()
endif()
set(BN_VERSION_STRING "${BN_VERSION_BASE}-dev+${_bn_commit}")
message(STATUS "Biblionaut version: ${BN_VERSION_STRING}")
