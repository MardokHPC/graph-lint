cmake_minimum_required(VERSION 3.20)

project(graphlint
    VERSION 0.1.0
    DESCRIPTION "Topology linter for CUDA graphs"
    HOMEPAGE_URL "https://github.com/MardokHPC/graph-lint"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(GRAPHLINT_BUILD_TESTS "Build the host test suite (fetches GoogleTest)" ON)
option(GRAPHLINT_WITH_CUDA "Build the cudaGraph_t conversion in src/cuda_source.cpp" OFF)
# Fetches cuda-timer for tools/bench.cu; arrives in M7.
option(GRAPHLINT_BUILD_BENCH "Build the instantiate/launch benchmark (needs a GPU)" OFF)

if(GRAPHLINT_WITH_CUDA)
    message(FATAL_ERROR
        "CUDA source lands in M2: src/cuda_source.cpp is the only translation unit "
        "that includes <cuda_runtime.h>, and it does not exist yet. Configure with "
        "-DGRAPHLINT_WITH_CUDA=OFF (the default) to build and test the host-only core.")
endif()

if(GRAPHLINT_BUILD_BENCH)
    message(FATAL_ERROR
        "GRAPHLINT_BUILD_BENCH=ON is not buildable yet: tools/bench.cu and its pinned "
        "cuda-timer FetchContent arrive in M7. Configure with -DGRAPHLINT_BUILD_BENCH=OFF.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Needed before the exported target names the include subdirectory.
include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Warnings: fatal everywhere, CXX only.
#
# One generator expression per flag: a genex result is not split on spaces by
# every generator, so passing "-Wall -Wextra" as one string can reach the
# compiler as a single malformed option.
# ---------------------------------------------------------------------------
function(graphlint_apply_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>>:-Wall>
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>>:-Wextra>
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>>:-Wpedantic>
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>>:-Werror>
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/W4>
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/WX>
    )
endfunction()

# Same flags for the host code inside a .cu, forwarded to the host compiler
# behind nvcc. -Wpedantic is absent on purpose: nvcc's generated #line directives
# trip it, and with -Werror every .cu would fail for a warning that is not ours.
function(graphlint_apply_cuda_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/W4>
            $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/WX>
        )
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # One -Xcompiler with a comma-separated list: nvcc forwards each element
        # to the host compiler as a separate argument.
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-Wall,-Wextra,-Werror>
        )
    else()
        message(WARNING
            "graphlint: unknown CUDA host compiler '${CMAKE_CXX_COMPILER_ID}'; "
            "building the .cu files without fatal host warnings")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# INTERFACE target (header-only)
#
#   add_subdirectory(graph-lint) / FetchContent -> graphlint::graphlint
#   find_package(graphlint CONFIG)              -> graphlint::graphlint
# ---------------------------------------------------------------------------
add_library(graphlint INTERFACE)
add_library(graphlint::graphlint ALIAS graphlint)

target_include_directories(graphlint INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(graphlint INTERFACE cxx_std_17)

# ---------------------------------------------------------------------------
# Tests (host-only: no CUDA toolkit, no GPU, no nvcc)
# ---------------------------------------------------------------------------
if(GRAPHLINT_BUILD_TESTS)
    enable_testing()

    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    # Pinned to a tag: a moving branch would make the suite's verdict depend on
    # somebody else's commit.
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    )
    # Keep `cmake --install` clean and skip gmock (unused).
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    # GoogleTest is not ours: SYSTEM keeps its headers outside /W4 /WX and
    # -Werror, which gate this repository's code only.
    foreach(graphlint_gtest_target IN ITEMS gtest gtest_main)
        if(TARGET ${graphlint_gtest_target})
            get_target_property(graphlint_gtest_include
                ${graphlint_gtest_target} INTERFACE_INCLUDE_DIRECTORIES)
            if(graphlint_gtest_include)
                set_target_properties(${graphlint_gtest_target} PROPERTIES
                    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${graphlint_gtest_include}")
            endif()
        endif()
    endforeach()

    # Registers target + ctest entry; the wrappers below add the warning set.
    function(graphlint_configure_test name main_file)
        add_executable(${name} ${ARGN} ${main_file})
        target_link_libraries(${name} PRIVATE graphlint::graphlint GTest::gtest)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    # Host-only suite: CXX flags, no device to check, never skipped.
    function(graphlint_add_test name)
        graphlint_configure_test(${name} ${PROJECT_SOURCE_DIR}/tests/test_main.cpp ${ARGN})
        graphlint_apply_warnings(${name})
    endfunction()

    # Device suite: the .cu files in ARGN go through nvcc (no -Wpedantic), the
    # host TUs keep /W4 /WX; 77 means "no card here", ctest reports SKIPPED.
    function(graphlint_add_device_test name)
        graphlint_configure_test(${name} ${PROJECT_SOURCE_DIR}/tests/test_main_device.cpp ${ARGN})
        graphlint_apply_warnings(${name})
        graphlint_apply_cuda_warnings(${name})
        set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
    endfunction()

    graphlint_add_test(graphlint_test_config tests/test_config.cpp)
    # The header must not drift from project(VERSION ...); the test compares them.
    # Asserts are forced on so the failure path is exercised in a Release build.
    target_compile_definitions(graphlint_test_config PRIVATE
        GRAPHLINT_TEST_CMAKE_VERSION="${PROJECT_VERSION}"
        GRAPHLINT_ENABLE_ASSERTS=1)

    graphlint_add_test(graphlint_test_graph tests/test_graph.cpp)
    graphlint_add_test(graphlint_test_json tests/test_json.cpp)
    # dump_graph validates its input when asserts are on: every emitted document
    # is then checked against the model invariants, not only against its bytes.
    target_compile_definitions(graphlint_test_graph PRIVATE GRAPHLINT_ENABLE_ASSERTS=1)
    # Fixtures are read from the source tree: mixed.json must stay the golden bytes.
    target_compile_definitions(graphlint_test_json PRIVATE
        GRAPHLINT_ENABLE_ASSERTS=1
        GRAPHLINT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/graphs")
endif()

# ---------------------------------------------------------------------------
# Install + export for find_package(graphlint CONFIG)
# ---------------------------------------------------------------------------
include(CMakePackageConfigHelpers)

install(DIRECTORY include/graphlint
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)

install(TARGETS graphlint EXPORT graphlintTargets)

install(EXPORT graphlintTargets
    FILE graphlintTargets.cmake
    NAMESPACE graphlint::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/graphlint
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/graphlint-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/graphlint-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/graphlint
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/graphlint-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
    ARCH_INDEPENDENT
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/graphlint-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/graphlint-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/graphlint
)
