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
# Fetches cuda-timer for tools/bench.cu; needs nvcc and a GPU to run.
option(GRAPHLINT_BUILD_BENCH "Build the instantiate/launch benchmark (needs a GPU)" OFF)
# Two knobs on purpose: the library needs cudart only, nvcc and a GPU are needed
# only to regenerate graphs/*.json and to run the suite that opens a handle.
option(GRAPHLINT_BUILD_DEVICE_TESTS "Build tests/device: demo_graph and the GPU suite" OFF)
# examples/three_lines.cu captures a graph: it needs nvcc, a card and the reader.
option(GRAPHLINT_BUILD_EXAMPLES "Build examples/ (needs CUDA, nvcc and a GPU to run)" OFF)

if(GRAPHLINT_BUILD_DEVICE_TESTS AND NOT GRAPHLINT_WITH_CUDA)
    message(FATAL_ERROR
        "GRAPHLINT_BUILD_DEVICE_TESTS=ON needs GRAPHLINT_WITH_CUDA=ON: the device "
        "suite reads graphs through src/cuda_source.cpp. Configure with "
        "-DGRAPHLINT_WITH_CUDA=ON, or turn the device tests off.")
endif()

if(GRAPHLINT_BUILD_DEVICE_TESTS AND NOT GRAPHLINT_BUILD_TESTS)
    message(FATAL_ERROR
        "GRAPHLINT_BUILD_DEVICE_TESTS=ON is a test suite: configure with "
        "-DGRAPHLINT_BUILD_TESTS=ON as well.")
endif()

if(GRAPHLINT_BUILD_BENCH AND NOT GRAPHLINT_WITH_CUDA)
    message(FATAL_ERROR
        "GRAPHLINT_BUILD_BENCH=ON needs GRAPHLINT_WITH_CUDA=ON: tools/bench.cu reads every "
        "graph back through src/cuda_source.cpp, so the number is the graph the linter "
        "sees. Configure with -DGRAPHLINT_WITH_CUDA=ON, or turn the benchmark off.")
endif()

if(GRAPHLINT_BUILD_EXAMPLES AND NOT GRAPHLINT_WITH_CUDA)
    message(FATAL_ERROR
        "GRAPHLINT_BUILD_EXAMPLES=ON needs GRAPHLINT_WITH_CUDA=ON: examples/three_lines.cu "
        "reads its captured graph through src/cuda_source.cpp. Configure with "
        "-DGRAPHLINT_WITH_CUDA=ON, or turn the examples off.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Only used once enable_language(CUDA) runs; the demo's host half is C++17 too.
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

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
# CUDA reader (src/cuda_source.cpp)
#
# Host-only C++: CUDAToolkit for the headers and cudart, no CUDA language and no
# nvcc. This is the promise README makes: a host job can build the reader.
# ---------------------------------------------------------------------------
if(GRAPHLINT_WITH_CUDA)
    find_package(CUDAToolkit REQUIRED)

    add_library(graphlint_cuda STATIC src/cuda_source.cpp)
    add_library(graphlint::cuda ALIAS graphlint_cuda)
    # The installed target must answer to the same name the build does: without
    # this the package would export graphlint::graphlint_cuda and no alias.
    set_target_properties(graphlint_cuda PROPERTIES EXPORT_NAME cuda)
    # PUBLIC: cuda_source.hpp includes <cuda_runtime_api.h> and the library's
    # users are the ones supplying the handle and the runtime.
    target_link_libraries(graphlint_cuda PUBLIC graphlint::graphlint CUDA::cudart)
    graphlint_apply_warnings(graphlint_cuda)
endif()

# ---------------------------------------------------------------------------
# CLIs (host-only: they read JSON; CUDA only fills the environment, never the core)
# ---------------------------------------------------------------------------
add_executable(graphlint_report tools/report.cpp tools/report_core.hpp)
target_link_libraries(graphlint_report PRIVATE graphlint::graphlint)
target_include_directories(graphlint_report PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
graphlint_apply_warnings(graphlint_report)

add_executable(graphlint_gate tools/gate.cpp)
target_link_libraries(graphlint_gate PRIVATE graphlint::graphlint)
graphlint_apply_warnings(graphlint_gate)

# Only the report CLI writes `environment`: the gate reads it out of the two documents.
if(GRAPHLINT_WITH_CUDA)
    target_compile_definitions(graphlint_report PRIVATE GRAPHLINT_CLI_WITH_CUDA=1)
    target_link_libraries(graphlint_report PRIVATE graphlint_cuda)
endif()

# ---------------------------------------------------------------------------
# Benchmark (tools/bench.cu)
#
# cuda-timer is header-only: its sources are populated and its CMakeLists is never
# added, so its install rules (headers, package config, CPack) cannot reach our
# package. The INTERFACE target below is ours and is one include directory wide.
# ---------------------------------------------------------------------------
if(GRAPHLINT_BUILD_BENCH)
    if(NOT CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES 75 CACHE STRING
            "Architecture of the benchmark kernels; override for another card")
    endif()
    enable_language(CUDA)

    include(FetchContent)
    FetchContent_Declare(cuda_timer
        GIT_REPOSITORY https://github.com/MardokHPC/cuda-timer.git
        GIT_TAG v1.2.2
        GIT_SHALLOW TRUE
        # include/ holds no CMakeLists.txt: populated, never added as a subdirectory.
        SOURCE_SUBDIR include
    )
    FetchContent_MakeAvailable(cuda_timer)

    add_library(graphlint_bench_timer INTERFACE)
    target_include_directories(graphlint_bench_timer INTERFACE ${cuda_timer_SOURCE_DIR}/include)

    add_executable(bench tools/bench.cu)
    target_link_libraries(bench PRIVATE graphlint_cuda CUDA::cudart graphlint_bench_timer)
    graphlint_apply_cuda_warnings(bench)

    # The producer of docs/bench/weights.json: one node at a time on the exec. dump_graph
    # validates the model in this build, so a graph the report would refuse never reaches disk.
    add_executable(weights tools/weights.cu)
    target_link_libraries(weights PRIVATE graphlint_cuda CUDA::cudart graphlint_bench_timer)
    target_compile_definitions(weights PRIVATE GRAPHLINT_ENABLE_ASSERTS=1)
    graphlint_apply_cuda_warnings(weights)
endif()

# ---------------------------------------------------------------------------
# Examples (examples/three_lines.cu): nvcc for the noop kernel, cudart for the
# capture, graphlint_cuda for the reader. The target runs on a GPU, not here.
# ---------------------------------------------------------------------------
if(GRAPHLINT_BUILD_EXAMPLES)
    if(NOT CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES 75 CACHE STRING
            "Architecture of the example kernels; override for another card")
    endif()
    enable_language(CUDA)

    add_executable(example_three_lines examples/three_lines.cu)
    target_link_libraries(example_three_lines PRIVATE graphlint_cuda CUDA::cudart)
    graphlint_apply_cuda_warnings(example_three_lines)
endif()

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

    graphlint_add_test(graphlint_test_analysis tests/test_analysis.cpp)
    # The four committed artifacts are read from the source tree: the numbers M0 measured
    # and M2 regenerated must stay true on a runner without a GPU.
    target_compile_definitions(graphlint_test_analysis PRIVATE
        GRAPHLINT_ENABLE_ASSERTS=1
        GRAPHLINT_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/graphs")

    graphlint_add_test(graphlint_test_rules tests/test_rules.cpp)
    # The four committed artifacts are read from the source tree: the finding sets M4 derives
    # must stay the ones docs/rules.md fixes, on a runner without a GPU.
    target_compile_definitions(graphlint_test_rules PRIVATE
        GRAPHLINT_ENABLE_ASSERTS=1
        GRAPHLINT_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/graphs")

    # -----------------------------------------------------------------------
    # emit_artifacts is a host generator, not a product: it rewrites the DOT and
    # Mermaid next to graphs/*.json, so the artifact tests and the README figure
    # are regenerable on a machine with no GPU. Built only with the tests.
    # -----------------------------------------------------------------------
    add_executable(emit_artifacts tools/emit_artifacts.cpp)
    target_link_libraries(emit_artifacts PRIVATE graphlint::graphlint)
    target_compile_definitions(emit_artifacts PRIVATE
        GRAPHLINT_ENABLE_ASSERTS=1
        GRAPHLINT_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/graphs")
    graphlint_apply_warnings(emit_artifacts)

    graphlint_add_test(graphlint_test_emitters tests/test_emitters.cpp)
    # Fixtures of the pinned examples, the four artifacts, and the repo root, where
    # README.md holds the figure the emitter has to reproduce.
    target_compile_definitions(graphlint_test_emitters PRIVATE
        GRAPHLINT_ENABLE_ASSERTS=1
        GRAPHLINT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/graphs"
        GRAPHLINT_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/graphs"
        GRAPHLINT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

    # The second process of docs/emitters.md section 6: regenerates the eight artifacts in
    # a scratch directory under another locale and timezone, then compares them with the
    # committed bytes one by one.
    add_test(NAME artifacts_locale
        COMMAND ${CMAKE_COMMAND}
            -DEMIT=$<TARGET_FILE:emit_artifacts>
            -DGRAPH_DIR=${CMAKE_CURRENT_SOURCE_DIR}/graphs
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/artifacts-locale
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_artifacts.cmake)
    set_tests_properties(artifacts_locale PROPERTIES
        ENVIRONMENT "LC_ALL=de_DE.UTF-8;TZ=Asia/Tokyo")

    graphlint_add_test(graphlint_test_report tests/test_report.cpp)
    # The pinned report document, the fixtures the gate reads, and the committed
    # artifacts the summary CLI is run over.
    target_compile_definitions(graphlint_test_report PRIVATE
        GRAPHLINT_ENABLE_ASSERTS=1
        GRAPHLINT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/reports"
        GRAPHLINT_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/graphs")
    # The CLI body lives in tools/report_core.hpp, outside the installed header set.
    target_include_directories(graphlint_test_report PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    graphlint_add_test(graphlint_test_gate tests/test_gate.cpp)
    # The four verdicts, the nine metrics and the threshold boundaries, on documents built in memory.
    target_compile_definitions(graphlint_test_gate PRIVATE
        GRAPHLINT_ENABLE_ASSERTS=1
        GRAPHLINT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/reports")
    set(graphlint_report_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/reports")

    graphlint_add_test(graphlint_test_weights tests/test_weights.cpp)
    # The table's loader, the fingerprint and the critical path on the six graph fixtures,
    # plus the committed ns table the CLI cases read.
    target_compile_definitions(graphlint_test_weights PRIVATE
        GRAPHLINT_ENABLE_ASSERTS=1
        GRAPHLINT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/graphs"
        GRAPHLINT_WEIGHTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/weights")

    # The benchmark artifact is opt-in: `build-device.bat bench` writes it on a GPU box,
    # and this entry skips when the file is absent. Nothing here asserts a timing.
    graphlint_add_test(graphlint_test_bench tests/test_bench_report.cpp)
    target_compile_definitions(graphlint_test_bench PRIVATE
        GRAPHLINT_BENCH_JSON="${CMAKE_CURRENT_SOURCE_DIR}/docs/bench/bench.json")
    set_tests_properties(graphlint_test_bench PROPERTIES SKIP_RETURN_CODE 77)

    # The closed loop, read on a runner with no GPU: the measured graph and the table bound to
    # it by fingerprint, through the same loader and critical path the report uses. Opt-in too.
    graphlint_add_test(graphlint_test_weights_artifact tests/test_weights_artifact.cpp)
    target_compile_definitions(graphlint_test_weights_artifact PRIVATE
        GRAPHLINT_ENABLE_ASSERTS=1
        GRAPHLINT_MEASURED_JSON="${CMAKE_CURRENT_SOURCE_DIR}/docs/bench/measured.json"
        GRAPHLINT_WEIGHTS_JSON="${CMAKE_CURRENT_SOURCE_DIR}/docs/bench/weights.json")
    set_tests_properties(graphlint_test_weights_artifact PROPERTIES SKIP_RETURN_CODE 77)

    # Exit codes are the contract of docs/report.md section 4, so the assertions are numbers
    # and the stderr line that names the failed comparison, never stdout alone.
    function(graphlint_add_gate_case name expected args stderr)
        add_test(NAME ${name} COMMAND ${CMAKE_COMMAND}
            -DTOOL=$<TARGET_FILE:graphlint_gate>
            -DEXPECTED=${expected}
            "-DARGS=${args}"
            "-DEXPECT_STDERR=${stderr}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/run_expect.cmake)
    endfunction()

    graphlint_add_gate_case(gate_pass_same_document 0
        "--baseline;${graphlint_report_dir}/baseline.json;--current;${graphlint_report_dir}/baseline.json"
        pass)
    graphlint_add_gate_case(gate_threshold_exceeded 1
        "--baseline;${graphlint_report_dir}/baseline.json;--current;${graphlint_report_dir}/depth_plus_one.json"
        "depth grew 1 > 0")
    graphlint_add_gate_case(gate_real_regression 1
        "--baseline;${graphlint_report_dir}/baseline.json;--current;${graphlint_report_dir}/host_node.json"
        "findings_severe grew 1 > 0")
    graphlint_add_gate_case(gate_other_source 2
        "--baseline;${graphlint_report_dir}/baseline.json;--current;${graphlint_report_dir}/other_source.json"
        "different source kind: built vs capture")
    graphlint_add_gate_case(gate_weights_incompatible 2
        "--baseline;${graphlint_report_dir}/baseline.json;--current;${graphlint_report_dir}/ns_weights.json"
        "different weight_kind: levels vs ns")
    # docs/weights.md section 5: each threshold in the wrong unit is exit 2, and the sentence
    # names the unit; the pair in ns decides on path_ns only when asked.
    graphlint_add_gate_case(gate_ns_threshold_on_levels 2
        "--baseline;${graphlint_report_dir}/baseline.json;--current;${graphlint_report_dir}/baseline.json;--max-path-increase-ns;0"
        "max-path-increase-ns is a nanosecond threshold")
    graphlint_add_gate_case(gate_depth_threshold_on_ns 2
        "--baseline;${graphlint_report_dir}/ns_baseline.json;--current;${graphlint_report_dir}/ns_baseline.json;--max-depth-increase;0"
        "max-depth-increase is a level count")
    graphlint_add_gate_case(gate_ns_regression 1
        "--baseline;${graphlint_report_dir}/ns_baseline.json;--current;${graphlint_report_dir}/ns_path_plus_one.json;--max-path-increase-ns;0"
        "path_ns grew 1 > 0")
    graphlint_add_gate_case(gate_ns_without_threshold 0
        "--baseline;${graphlint_report_dir}/ns_baseline.json;--current;${graphlint_report_dir}/ns_path_plus_one.json"
        "all metrics inside their thresholds")
    graphlint_add_gate_case(gate_ns_within_threshold 0
        "--baseline;${graphlint_report_dir}/ns_baseline.json;--current;${graphlint_report_dir}/ns_path_plus_one.json;--max-path-increase-ns;1"
        "all metrics inside their thresholds")
    graphlint_add_gate_case(gate_ns_improvement 0
        "--baseline;${graphlint_report_dir}/ns_path_plus_one.json;--current;${graphlint_report_dir}/ns_baseline.json;--max-path-increase-ns;0"
        "path_ns fell 1 > 0")
    # A document is emitted even when the input cannot be read, so the --json file is opened,
    # not assumed.
    function(graphlint_add_gate_json_case name expected args stderr json_path json_contains)
        add_test(NAME ${name} COMMAND ${CMAKE_COMMAND}
            -DTOOL=$<TARGET_FILE:graphlint_gate>
            -DEXPECTED=${expected}
            "-DARGS=${args}"
            "-DEXPECT_STDERR=${stderr}"
            "-DEXPECT_JSON=${json_path}"
            "-DEXPECT_JSON_CONTAINS=${json_contains}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/run_expect.cmake)
    endfunction()

    graphlint_add_gate_json_case(gate_unreadable_input 2
        "--baseline;${graphlint_report_dir}/baseline.json;--current;${graphlint_report_dir}/truncated.json;--json;${CMAKE_CURRENT_BINARY_DIR}/unreadable-gate.json"
        "[0-9]+:[0-9]+"
        ${CMAKE_CURRENT_BINARY_DIR}/unreadable-gate.json
        "\"verdict\": \"suspect\"")
    graphlint_add_gate_json_case(gate_missing_input_writes_suspect 2
        "--quiet;--baseline;${CMAKE_CURRENT_BINARY_DIR}/does-not-exist.json;--current;${graphlint_report_dir}/baseline.json;--json;${CMAKE_CURRENT_BINARY_DIR}/missing-input.json"
        "graphlint_gate: suspect"
        ${CMAKE_CURRENT_BINARY_DIR}/missing-input.json
        "\"verdict\": \"suspect\"")

    # --quiet silences the document, never the verdict line.
    add_test(NAME gate_quiet_keeps_the_verdict COMMAND ${CMAKE_COMMAND}
        -DTOOL=$<TARGET_FILE:graphlint_gate>
        -DEXPECTED=0
        "-DARGS=--quiet;--baseline;${graphlint_report_dir}/baseline.json;--current;${graphlint_report_dir}/baseline.json"
        "-DEXPECT_STDERR=graphlint_gate: pass"
        -DEXPECT_STDOUT_EMPTY=ON
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/run_expect.cmake)

    # A --json path that cannot be opened is exit 2, and the verdict sentence still reaches
    # stderr: the line is the contract's fixed one, not a success-only courtesy.
    add_test(NAME gate_json_unwritable_keeps_the_verdict COMMAND ${CMAKE_COMMAND}
        -DTOOL=$<TARGET_FILE:graphlint_gate>
        -DEXPECTED=2
        "-DARGS=--quiet;--baseline;${graphlint_report_dir}/baseline.json;--current;${graphlint_report_dir}/baseline.json;--json;${CMAKE_CURRENT_BINARY_DIR}/no-such-directory/gate.json"
        "-DEXPECT_STDERR=graphlint_gate: pass"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/run_expect.cmake)

    add_test(NAME report_summary COMMAND ${CMAKE_COMMAND}
        -DTOOL=$<TARGET_FILE:graphlint_report>
        -DEXPECTED=0
        "-DARGS=--format;summary;${CMAKE_CURRENT_SOURCE_DIR}/graphs/built.json"
        "-DEXPECT_STDERR=^$"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/run_expect.cmake)

    # Usage and input failures the contract names: a cap below one and a report where a graph
    # belongs both end at 2, with the detail on stderr.
    add_test(NAME report_max_nodes_zero COMMAND ${CMAKE_COMMAND}
        -DTOOL=$<TARGET_FILE:graphlint_report>
        -DEXPECTED=2
        "-DARGS=--max-nodes;0;${CMAKE_CURRENT_SOURCE_DIR}/graphs/built.json"
        "-DEXPECT_STDERR=--max-nodes must be >= 1"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/run_expect.cmake)
    add_test(NAME report_rejects_report_json COMMAND ${CMAKE_COMMAND}
        -DTOOL=$<TARGET_FILE:graphlint_report>
        -DEXPECTED=2
        "-DARGS=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/reports/baseline.json"
        "-DEXPECT_STDERR=unknown key \"analysis\""
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/run_expect.cmake)

    # --template: the starting document a hand-written graph begins from. The bytes are
    # compared with the committed file, and a second call must refuse to overwrite it.
    add_test(NAME report_template COMMAND ${CMAKE_COMMAND}
        -DTOOL=$<TARGET_FILE:graphlint_report>
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/template.json
        "-DEXPECTED_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/graphs/template.json"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/report_template.cmake)

    # The template is a graph the linter reads clean, or the starting point would be a trap.
    add_test(NAME report_template_lints_clean COMMAND ${CMAKE_COMMAND}
        -DTOOL=$<TARGET_FILE:graphlint_report>
        -DEXPECTED=0
        "-DARGS=--format;summary;${CMAKE_CURRENT_SOURCE_DIR}/tests/data/graphs/template.json"
        "-DEXPECT_STDERR=^$"
        "-DEXPECT_STDOUT=findings: 0"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/run_expect.cmake)

    # --weights: a table bound to the graph turns the summary into a nanosecond path, and a
    # refused table is exit 2 with the position of the fault.
    add_test(NAME report_weights_summary COMMAND ${CMAKE_COMMAND}
        -DTOOL=$<TARGET_FILE:graphlint_report>
        -DEXPECTED=0
        "-DARGS=--format;summary;${CMAKE_CURRENT_SOURCE_DIR}/tests/data/graphs/diamond.json;--weights;${CMAKE_CURRENT_SOURCE_DIR}/tests/data/weights/diamond.json"
        "-DEXPECT_STDERR=^$"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/run_expect.cmake)
    add_test(NAME report_weights_refused COMMAND ${CMAKE_COMMAND}
        -DTOOL=$<TARGET_FILE:graphlint_report>
        -DEXPECTED=2
        "-DARGS=--format;summary;${CMAKE_CURRENT_SOURCE_DIR}/tests/data/graphs/diamond.json;--weights;${CMAKE_CURRENT_SOURCE_DIR}/tests/data/weights/unit_us.json"
        "-DEXPECT_STDERR=unit .us. is not"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/run_expect.cmake)
    # Two processes, one under another locale and timezone, must write the same bytes.
    add_test(NAME report_ns_determinism COMMAND ${CMAKE_COMMAND}
        -DTOOL=$<TARGET_FILE:graphlint_report>
        "-DARGS=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/graphs/diamond.json;--weights;${CMAKE_CURRENT_SOURCE_DIR}/tests/data/weights/diamond.json;--format;json"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/ns-determinism"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/report_determinism.cmake)

    # --out leaves stdout empty and writes the same bytes; the emitters also reproduce the
    # committed graphs/*.dot and *.mmd, so the CLI stays tied to the files the README shows.
    function(graphlint_add_report_out_case name graph format expected_file)
        add_test(NAME ${name} COMMAND ${CMAKE_COMMAND}
            -DTOOL=$<TARGET_FILE:graphlint_report>
            -DGRAPH=${CMAKE_CURRENT_SOURCE_DIR}/${graph}
            -DFORMAT=${format}
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/${name}.out
            "-DEXPECTED_FILE=${expected_file}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/report_out.cmake)
    endfunction()

    graphlint_add_report_out_case(report_json_out graphs/built.json json "")
    graphlint_add_report_out_case(
        report_dot_matches_artifact graphs/pingpong.json dot
        ${CMAKE_CURRENT_SOURCE_DIR}/graphs/pingpong.dot)
    graphlint_add_report_out_case(
        report_mermaid_matches_artifact graphs/pingpong.json mermaid
        ${CMAKE_CURRENT_SOURCE_DIR}/graphs/pingpong.mmd)

    # -----------------------------------------------------------------------
    # demo_check reads graphs/*.json with no CUDA at all: it is registered here,
    # in the host-only block, so the free CI job validates the committed
    # artifacts. demo_graph, which regenerates them, needs a GPU (below).
    # -----------------------------------------------------------------------
    graphlint_add_test(demo_check tests/device/demo_check.cpp)
    target_compile_definitions(demo_check PRIVATE
        GRAPHLINT_ENABLE_ASSERTS=1
        GRAPHLINT_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/graphs")

    if(GRAPHLINT_BUILD_DEVICE_TESTS)
        if(NOT CMAKE_CUDA_ARCHITECTURES)
            set(CMAKE_CUDA_ARCHITECTURES 75 CACHE STRING
                "Architecture of the demo kernels; override for another card")
        endif()
        enable_language(CUDA)

        add_executable(demo_graph tests/device/demo_graph.cu)
        target_link_libraries(demo_graph PRIVATE graphlint_cuda)
        target_compile_definitions(demo_graph PRIVATE
            GRAPHLINT_ENABLE_ASSERTS=1
            GRAPHLINT_GRAPH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/graphs")
        graphlint_apply_cuda_warnings(demo_graph)

        # Device code needs nvcc (the kernel), the main is host-only and returns 77
        # without a card, which ctest reports as SKIPPED.
        graphlint_add_device_test(graphlint_test_cuda tests/device/test_cuda_source.cu)
        target_link_libraries(graphlint_test_cuda PRIVATE graphlint_cuda)
        target_compile_definitions(graphlint_test_cuda PRIVATE GRAPHLINT_ENABLE_ASSERTS=1)
    endif()

    # The README example, only when it was built: exit 0 is the assertion, 77 is "no card
    # here" and ctest reports SKIPPED. --out keeps the temp file out of the source tree.
    if(GRAPHLINT_BUILD_EXAMPLES)
        add_test(NAME example_three_lines
            COMMAND example_three_lines --out ${CMAKE_CURRENT_BINARY_DIR}/example_three_lines.json)
        set_tests_properties(example_three_lines PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

# ---------------------------------------------------------------------------
# Install + export for find_package(graphlint CONFIG)
# ---------------------------------------------------------------------------
include(CMakePackageConfigHelpers)

# cuda_source.hpp and the reader ship together: the header declares from_cuda and
# graphlint::cuda is the target that defines it. A host-only package has neither.
install(DIRECTORY include/graphlint
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp" PATTERN "cuda_source.hpp" EXCLUDE
)

if(GRAPHLINT_WITH_CUDA)
    install(TARGETS graphlint_cuda EXPORT graphlintTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES include/graphlint/cuda_source.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/graphlint)
endif()

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

# ARCH_INDEPENDENT says the package is headers only. A STATIC reader next to them
# makes that untrue, so the flag follows the CUDA switch.
if(GRAPHLINT_WITH_CUDA)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/graphlint-config-version.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion
    )
else()
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/graphlint-config-version.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion
        ARCH_INDEPENDENT
    )
endif()

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/graphlint-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/graphlint-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/graphlint
)
