cmake_minimum_required(VERSION 3.22)
set(LANGS C CXX)

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

option(ALLOCAZAM_BUILD_TESTS "Build allocazam test suite" ${ALLOCAZAM_IS_TOPLEVEL_PROJECT})

set(CMAKE_CXX_STANDARD 23)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_library(liballocazam
    lib/allocazam.cpp
)

target_include_directories(liballocazam PUBLIC lib)
target_compile_features(liballocazam PUBLIC cxx_std_23)

if(ALLOCAZAM_BUILD_TESTS)
    add_subdirectory(bench)
    add_subdirectory(tests)
endif()
