set(NOTEPADSAFE_PCRE2_VERSION "10.47")
set(NOTEPADSAFE_PCRE2_SOURCE_DIR
    "${CMAKE_SOURCE_DIR}/third_party/src/pcre2-${NOTEPADSAFE_PCRE2_VERSION}"
    CACHE PATH "Pinned PCRE2 source directory"
)

if(NOT EXISTS "${NOTEPADSAFE_PCRE2_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Pinned PCRE2 ${NOTEPADSAFE_PCRE2_VERSION} source is missing at "
        "${NOTEPADSAFE_PCRE2_SOURCE_DIR}. Restore the vendored source tree."
    )
endif()

# The application consumes only the 8-bit static engine. Keeping JIT disabled
# makes match/depth/heap limits and cancellation callouts deterministic.
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2_8 ON CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2_16 OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2_32 OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_JIT OFF CACHE BOOL "" FORCE)
set(PCRE2_SHOW_REPORT OFF CACHE BOOL "" FORCE)

add_subdirectory(
    "${NOTEPADSAFE_PCRE2_SOURCE_DIR}"
    "${CMAKE_BINARY_DIR}/_deps/pcre2-build"
    EXCLUDE_FROM_ALL
)

add_library(notepadsafe_pcre2 INTERFACE)
target_link_libraries(notepadsafe_pcre2 INTERFACE pcre2-8-static)
