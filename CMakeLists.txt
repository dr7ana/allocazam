cmake_minimum_required(VERSION 3.22)
set(LANGS C CXX)

project(allocazam
    DESCRIPTION "fast C++23 allocator and memory pool"
    LANGUAGES ${LANGS}
)

set(CMAKE_CXX_STANDARD 23)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_library(liballocazam
    lib/allocazam.cpp
)

target_include_directories(liballocazam PUBLIC lib)
target_compile_features(liballocazam PUBLIC cxx_std_23)


add_subdirectory(bench)
add_subdirectory(tests)
