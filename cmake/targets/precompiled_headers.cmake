# Precompiled headers for the targets with many sources.
#
# Parsing dominates a cold build: measured with -ftime-trace over 1011 translation units, the front end took 76% of the compile
# time, and windows.h, <chrono>, nlohmann/json and Boost.Log were among the most expensive headers. One
# precompiled header (src/precompiled.h) shared through target_precompile_headers removes most of that for the large targets.
#
# Small targets are left out: a precompiled header costs a few seconds of its own, which a target with a handful of sources
# does not earn back (the tests alone are ninety targets).
#
# With ccache, the compiler cache needs `sloppiness = pch_defines,time_macros`, and clang needs -fno-pch-timestamp (added
# below) so a cached precompiled header is not refused for a newer header file
# (https://ccache.dev/manual/latest.html#_precompiled_headers). CI sets the sloppiness.
if(NOT SUNSHINE_PRECOMPILED_HEADERS)
    return()
endif()

set(SUNSHINE_PCH_MINIMUM_SOURCES 8 CACHE STRING "Smallest number of C++ sources a target needs to get the precompiled header")

function(_sunshine_collect_targets directory result)
    get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_subdirectory IN LISTS _subdirectories)
        _sunshine_collect_targets("${_subdirectory}" _inner)
        list(APPEND _targets ${_inner})
    endforeach()
    set(${result} "${_targets}" PARENT_SCOPE)
endfunction()

function(_sunshine_apply_precompiled_headers)
    _sunshine_collect_targets("${CMAKE_SOURCE_DIR}" _all_targets)
    foreach(_target IN LISTS _all_targets)
        get_target_property(_type ${_target} TYPE)
        get_target_property(_source_dir ${_target} SOURCE_DIR)
        if(NOT _type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|OBJECT_LIBRARY)$" OR _source_dir MATCHES "_deps|third-party|googletest")
            continue()
        endif()
        get_target_property(_sources ${_target} SOURCES)
        set(_cxx_sources ${_sources})
        list(FILTER _cxx_sources INCLUDE REGEX "\\.cpp$")
        list(LENGTH _cxx_sources _count)
        if(_count LESS SUNSHINE_PCH_MINIMUM_SOURCES)
            continue()
        endif()
        message(STATUS "Precompiled header for ${_target} (${_count} C++ sources)")
        target_precompile_headers(${_target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SOURCE_DIR}/src/precompiled.h>")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            # The cached precompiled header must not be refused for a newer header file (see above).
            target_compile_options(${_target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-Xclang -fno-pch-timestamp>")
        endif()
    endforeach()
endfunction()

# After every target exists, tests included.
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _sunshine_apply_precompiled_headers)
