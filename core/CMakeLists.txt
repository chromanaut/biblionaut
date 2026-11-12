# core: shared foundation for every other part. Depends on nothing.
add_library(bn_core STATIC
  src/arena.cpp
  src/byte_source.cpp
  src/metrics.cpp
  src/trace.cpp)
target_include_directories(bn_core PUBLIC include)
target_link_libraries(bn_core PRIVATE bn_options)

# Global operator new / delete replacements and the heap-failure hook. An
# OBJECT library, because replacement operators must be linked into the final
# executable unconditionally; a static archive member would only be pulled in
# if something happened to reference it first. Every executable links this.
add_library(bn_core_runtime OBJECT
  src/runtime/new_delete.cpp)
target_include_directories(bn_core_runtime PUBLIC include)
target_link_libraries(bn_core_runtime PRIVATE bn_options)
