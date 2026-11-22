# Asserts --template: the committed file is the source of truth for the starting document,
# a second run refuses to overwrite it, and the refusal leaves the bytes untouched.
#
# Usage:
#   cmake -DTOOL=<path> -DOUT=<scratch file> -DEXPECTED_FILE=<committed template.json>
#         -P report_template.cmake

if(NOT DEFINED TOOL OR NOT DEFINED OUT OR NOT DEFINED EXPECTED_FILE)
    message(FATAL_ERROR "report_template.cmake: TOOL, OUT and EXPECTED_FILE are required")
endif()

# The build dir is reused: a leftover from the previous run would make the first call
# fail as a refusal instead of exercising the writer.
file(REMOVE "${OUT}")

execute_process(
    COMMAND ${TOOL} --template ${OUT}
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    RESULT_VARIABLE code
)

if(NOT code EQUAL 0)
    message(FATAL_ERROR "--template exited ${code}\n--- stderr ---\n${err}")
endif()
if(NOT out STREQUAL "")
    message(FATAL_ERROR "--template wrote to stdout:\n${out}")
endif()

file(READ "${OUT}" generated_hex HEX)
file(READ "${EXPECTED_FILE}" expected_hex HEX)
if(NOT generated_hex STREQUAL expected_hex)
    message(FATAL_ERROR "--template differs from ${EXPECTED_FILE}")
endif()

# The file exists now: a second call must refuse, at exit 2, without touching the bytes.
execute_process(
    COMMAND ${TOOL} --template ${OUT}
    OUTPUT_VARIABLE refused_stdout
    ERROR_VARIABLE refused_stderr
    RESULT_VARIABLE refused_code
)

if(NOT refused_code EQUAL 2)
    message(FATAL_ERROR "overwrite exited ${refused_code}, expected 2")
endif()
if(NOT refused_stdout STREQUAL "")
    message(FATAL_ERROR "the refusal wrote to stdout:\n${refused_stdout}")
endif()
if(NOT refused_stderr MATCHES "refusing to overwrite")
    message(FATAL_ERROR "stderr did not explain the refusal\n--- stderr ---\n${refused_stderr}")
endif()

file(READ "${OUT}" after_hex HEX)
if(NOT after_hex STREQUAL generated_hex)
    message(FATAL_ERROR "the refused run modified ${OUT}")
endif()

message(STATUS "template matches ${EXPECTED_FILE}, overwrite refused")
