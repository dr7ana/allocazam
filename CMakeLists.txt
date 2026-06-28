cmake_minimum_required(VERSION 3.22)
set(LANGS C CXX)


set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)


project(allocazam
    VERSION 0.1.0
    DESCRIPTION "fast C++23 allocator and memory pool"
    LANGUAGES ${LANGS}
)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(ALLOCAZAM_IS_TOPLEVEL_PROJECT TRUE)
else()
    set(ALLOCAZAM_IS_TOPLEVEL_PROJECT FALSE)
endif()

include(build_opts)

set(CMAKE_CXX_STANDARD 23)

set(warning_flags -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-function -Werror=vla -Wno-deprecated-declaration)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    list(APPEND warning_flags -Wno-unknown-warning-option)
endif()

if(ALLOCAZAM_WARNINGS_AS_ERRORS)
    list(APPEND warning_flags -Werror)
endif()

add_library(liballocazam
    lib/allocazam.cpp
)

if(ALLOCAZAM_USE_LIBCXX)
    target_link_libraries(liballocazam PUBLIC "-lc++abi")
endif()

target_include_directories(liballocazam PUBLIC lib)
target_compile_features(liballocazam PUBLIC cxx_std_23)

if(ALLOCAZAM_ARM64)
    target_link_libraries(liballocazam PUBLIC atomic)
endif()

if(ALLOCAZAM_BUILD_TESTS)
    add_subdirectory(bench)
    add_subdirectory(tests)
endif()
