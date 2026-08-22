set(NOTEPADSAFE_BOOST_REGEX_VERSION "1.90.0")
set(NOTEPADSAFE_BOOST_REGEX_SOURCE_DIR
    "${CMAKE_SOURCE_DIR}/third_party/src/boost-regex-${NOTEPADSAFE_BOOST_REGEX_VERSION}"
    CACHE PATH "Pinned Boost.Regex source directory"
)

if(NOT EXISTS "${NOTEPADSAFE_BOOST_REGEX_SOURCE_DIR}/include/boost/regex.hpp")
    message(FATAL_ERROR
        "Pinned Boost.Regex ${NOTEPADSAFE_BOOST_REGEX_VERSION} source is missing at "
        "${NOTEPADSAFE_BOOST_REGEX_SOURCE_DIR}. Restore the vendored source tree."
    )
endif()

# Boost.Regex 1.90 provides its C++ API as a header-only standalone target.
# Keep the dependency isolated behind a project-owned target so product code
# never relies on a host-installed Boost tree or its auto-link behavior.
add_library(notepadsafe_boost_regex INTERFACE)
target_include_directories(notepadsafe_boost_regex INTERFACE
    "${NOTEPADSAFE_BOOST_REGEX_SOURCE_DIR}/include"
)
target_compile_definitions(notepadsafe_boost_regex INTERFACE
    BOOST_REGEX_STANDALONE
    BOOST_REGEX_NO_LIB
)
