include_guard(GLOBAL)

# ci-lto-experiment-20261117: link-time optimisation for the GCC/Clang comparison. The mode comes from the environment of the CI job
# (SUNSHINE_LTO = gcc-lto | thinlto; anything else leaves the build untouched), so no workflow invocation line changes. Windows
# (MinGW) Release builds only. Not meant for the upstream branch.
if(WIN32 AND "$ENV{SUNSHINE_LTO}" MATCHES "^(gcc-lto|thinlto)$" AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set(_lto_mode "$ENV{SUNSHINE_LTO}")
    if(_lto_mode STREQUAL "gcc-lto" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # Slim LTO objects need the plugin-aware archiver; -flto=auto spreads the LTRANS partitions over all processors.
        find_program(_lto_ar NAMES gcc-ar REQUIRED)
        find_program(_lto_ranlib NAMES gcc-ranlib REQUIRED)
        set(CMAKE_AR "${_lto_ar}" CACHE FILEPATH "archiver with the LTO plugin" FORCE)
        set(CMAKE_RANLIB "${_lto_ranlib}" CACHE FILEPATH "ranlib with the LTO plugin" FORCE)
        foreach(_lang C CXX)
            string(APPEND CMAKE_${_lang}_FLAGS_RELEASE " -flto=auto")
        endforeach()
        foreach(_kind EXE SHARED MODULE)
            string(APPEND CMAKE_${_kind}_LINKER_FLAGS_RELEASE " -flto=auto")
        endforeach()
    elseif(_lto_mode STREQUAL "thinlto" AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # Fat objects keep GNU ar able to index the static libraries (the pinned Clang set has no llvm-ar); lld takes the
        # bitcode section for ThinLTO. ICF "safe" folds only functions whose address is not significant.
        foreach(_lang C CXX)
            string(APPEND CMAKE_${_lang}_FLAGS_RELEASE " -flto=thin -ffat-lto-objects")
        endforeach()
        foreach(_kind EXE SHARED MODULE)
            string(APPEND CMAKE_${_kind}_LINKER_FLAGS_RELEASE " -flto=thin -Wl,--fat-lto-objects -Wl,--icf=safe")
        endforeach()
    else()
        message(FATAL_ERROR "SUNSHINE_LTO=${_lto_mode} does not match the compiler ${CMAKE_CXX_COMPILER_ID}")
    endif()
    message(STATUS "LTO experiment: ${_lto_mode}")
endif()

# Require the selected standard rather than silently accepting an older dialect.
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(SUNSHINE_TESTS_ENABLE_COVERAGE "Enable gcov coverage instrumentation for tests" OFF)

add_library(sunshine_project_options INTERFACE)
target_compile_features(sunshine_project_options INTERFACE cxx_std_23)
if(MINGW)
    target_link_options(sunshine_project_options INTERFACE -static)
    target_compile_options(sunshine_project_options INTERFACE $<$<COMPILE_LANGUAGE:RC>:--use-temp-file>)
    # fs-gnu-ld-gc-sections-20261116: discard unreferenced Release sections after
    # each function/data item is emitted separately.
    target_compile_options(sunshine_project_options INTERFACE
            "$<$<CONFIG:Release>:-ffunction-sections;-fdata-sections>")
    target_link_options(sunshine_project_options INTERFACE
            $<$<CONFIG:Release>:LINKER:--gc-sections>)
    # fs-llvm-clang-cli-20261116 and fs-llvm-lld-20261116: Clang's MinGW
    # release executable keeps the same 2 MiB reserve as the GCC baseline.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_link_options(sunshine_project_options INTERFACE -Wl,--stack,2097152)
    endif()
endif()

# Keep per-configuration source policy in one place for both host variants.
function(sunshine_configure_debug_sources test_directory)
    if(WIN32)
        set_source_files_properties("${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
                DIRECTORY "${CMAKE_SOURCE_DIR}" "${test_directory}"
                PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>")
    endif()
endfunction()

add_library(sunshine_coverage INTERFACE)
if(SUNSHINE_TESTS_ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "SUNSHINE_TESTS_ENABLE_COVERAGE requires GCC or Clang")
    endif()
    target_compile_options(sunshine_coverage INTERFACE
            $<$<COMPILE_LANGUAGE:C,CXX>:--coverage;-g;-O0>)
    target_link_options(sunshine_coverage INTERFACE --coverage)
    if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # Instrumented Beast templates can exceed the standard COFF section limit.
        target_compile_options(sunshine_coverage INTERFACE $<$<COMPILE_LANGUAGE:CXX>:-Wa,-mbig-obj>)
    endif()
endif()
