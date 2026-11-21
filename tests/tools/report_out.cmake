# Runs graphlint_report twice: --out <file> and plain stdout. The contract is that --out leaves
# stdout empty, the file carries the same bytes, and an emitter reproduces its committed artifact.
#
# Usage:
#   cmake -DTOOL=<path> -DGRAPH=<graph.json> -DFORMAT=<json|dot|mermaid> -DOUT=<file>
#         [-DEXPECTED_FILE=<artifact>] -P report_out.cmake

if(NOT DEFINED TOOL OR NOT DEFINED GRAPH OR NOT DEFINED FORMAT OR NOT DEFINED OUT)
    message(FATAL_ERROR "report_out.cmake: TOOL, GRAPH, FORMAT and OUT are required")
endif()

execute_process(
    COMMAND ${TOOL} ${GRAPH} --format ${FORMAT} --out ${OUT}
    OUTPUT_VARIABLE redirected_stdout
    ERROR_VARIABLE redirected_stderr
    RESULT_VARIABLE redirected_code)

if(NOT redirected_code EQUAL 0)
    message(FATAL_ERROR "--out exited ${redirected_code}\n--- stderr ---\n${redirected_stderr}")
endif()
if(NOT redirected_stdout STREQUAL "")
    message(FATAL_ERROR "stdout was not empty with --out:\n${redirected_stdout}")
endif()

set(stdout_file "${OUT}.stdout")
execute_process(
    COMMAND ${TOOL} ${GRAPH} --format ${FORMAT}
    OUTPUT_FILE "${stdout_file}"
    ERROR_VARIABLE piped_stderr
    RESULT_VARIABLE piped_code)

if(NOT piped_code EQUAL 0)
    message(FATAL_ERROR "stdout run exited ${piped_code}\n--- stderr ---\n${piped_stderr}")
endif()

file(READ "${OUT}" redirected_hex HEX)
file(READ "${stdout_file}" piped_hex HEX)
if(NOT redirected_hex STREQUAL piped_hex)
    message(FATAL_ERROR "--out bytes differ from the bytes written to stdout")
endif()

if(DEFINED EXPECTED_FILE AND NOT "${EXPECTED_FILE}" STREQUAL "")
    file(READ "${EXPECTED_FILE}" expected_hex HEX)
    if(NOT redirected_hex STREQUAL expected_hex)
        message(FATAL_ERROR "--out differs from ${EXPECTED_FILE}")
    endif()
endif()

message(STATUS "${FORMAT}: --out equals stdout, artifact checked")
