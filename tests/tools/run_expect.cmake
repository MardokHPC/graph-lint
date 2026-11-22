# Runs one graphlint tool and asserts its exit code, its streams and its --json document.
#
# The gate verdicts are a contract on the exit code (0 pass/improvement, 1 regression,
# 2 suspect/error). A CTest that only greps stdout would still pass if the wrapper forgot
# to return the code, so this script checks the number first.
#
# Usage:
#   cmake -DTOOL=<path> -DEXPECTED=<int> "-DARGS=<arg;arg;...>"
#         [-DEXPECT_STDERR=<regex>] [-DEXPECT_STDOUT=<regex>] [-DEXPECT_STDOUT_EMPTY=ON]
#         [-DEXPECT_JSON=<path>] [-DEXPECT_JSON_CONTAINS=<substring>]
#         -P run_expect.cmake

if(NOT DEFINED TOOL OR NOT DEFINED EXPECTED)
    message(FATAL_ERROR "run_expect.cmake: TOOL and EXPECTED are required")
endif()

execute_process(
    COMMAND ${TOOL} ${ARGS}
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    RESULT_VARIABLE code
)

if(NOT code EQUAL EXPECTED)
    message(FATAL_ERROR
        "expected exit ${EXPECTED}, got ${code}\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
endif()

if(DEFINED EXPECT_STDERR AND NOT err MATCHES "${EXPECT_STDERR}")
    message(FATAL_ERROR
        "stderr did not match '${EXPECT_STDERR}'\n--- stderr ---\n${err}")
endif()

if(DEFINED EXPECT_STDOUT AND NOT out MATCHES "${EXPECT_STDOUT}")
    message(FATAL_ERROR
        "stdout did not match '${EXPECT_STDOUT}'\n--- stdout ---\n${out}")
endif()

if(EXPECT_STDOUT_EMPTY AND NOT out STREQUAL "")
    message(FATAL_ERROR "stdout was expected empty, got:\n${out}")
endif()

# The --json document is part of the contract (unreadable input still emits it), so its
# existence is asserted, not assumed.
if(DEFINED EXPECT_JSON)
    if(NOT EXISTS "${EXPECT_JSON}")
        message(FATAL_ERROR "expected ${EXPECT_JSON} to exist")
    endif()
    if(DEFINED EXPECT_JSON_CONTAINS)
        file(READ "${EXPECT_JSON}" json_text)
        string(FIND "${json_text}" "${EXPECT_JSON_CONTAINS}" found_at)
        if(found_at EQUAL -1)
            message(FATAL_ERROR
                "${EXPECT_JSON} does not contain '${EXPECT_JSON_CONTAINS}'\n--- file ---\n${json_text}")
        endif()
    endif()
endif()

message(STATUS "exit ${code} as expected")
