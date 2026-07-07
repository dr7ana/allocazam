set(allocazam_compiler_name "${CMAKE_CXX_COMPILER_ID}")
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" allocazam_system_processor)

set(ALLOCAZAM_ARM64 FALSE)
set(ALLOCAZAM_X86_64 FALSE)

if(allocazam_system_processor STREQUAL "x86_64")
    set(CMAKE_TARGET_ARCHITECTURE "x86_64" CACHE STRING "target architecture" FORCE)
    set(ALLOCAZAM_X86_64 TRUE)
else()
    if(allocazam_system_processor STREQUAL "aarch64")
        set(CMAKE_TARGET_ARCHITECTURE "arm64" CACHE STRING "target architecture" FORCE)
    elseif(allocazam_system_processor MATCHES "^arm")
        set(CMAKE_TARGET_ARCHITECTURE "arm" CACHE STRING "target architecture" FORCE)
    else()
        message(FATAL_ERROR "allocazam not supported on on this architecture -- what exactly are you using?")
    endif()
    set(ALLOCAZAM_ARM64 TRUE)
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C>:-fsigned-char>"
        "$<$<COMPILE_LANGUAGE:CXX>:-fsigned-char>")
endif()

set(ALLOCAZAM_DEBUG_BUILD FALSE)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(ALLOCAZAM_DEBUG_BUILD TRUE)
endif()

option(ALLOCAZAM_BUILD_TESTS "Build allocazam test suite" ${ALLOCAZAM_IS_TOPLEVEL_PROJECT})
option(ALLOCAZAM_WARNINGS_AS_ERRORS "treat all warnings as errors. turn off for development, on for release" ${ALLOCAZAM_DEBUG_BUILD})
option(ALLOCAZAM_USE_LIBCXX "build C++ targets with libc++ instead of libstdc++ when using clang" OFF)
set(ALLOCAZAM_SANITIZER "" CACHE STRING "compile and link everything with -fsanitize=<value> (e.g. thread, address)")

if(ALLOCAZAM_SANITIZER)
    add_compile_options(-fsanitize=${ALLOCAZAM_SANITIZER} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${ALLOCAZAM_SANITIZER})
endif()

if(ALLOCAZAM_USE_LIBCXX)
    if(NOT allocazam_compiler_name MATCHES "Clang")
        message(FATAL_ERROR "ALLOCAZAM_USE_LIBCXX requires clang; compiler is ${allocazam_compiler_name}")
    endif()

    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>")
    add_link_options("$<$<LINK_LANGUAGE:CXX>:-stdlib=libc++>" "$<$<LINK_LANGUAGE:CXX>:-fuse-ld=lld>")
endif()

