# Runs one graphlint tool twice, the second process under another locale and timezone, and
# compares the bytes: the document is a function of its inputs, never of the machine's locale.
#
# Usage:
#   cmake -DTOOL=<path> "-DARGS=<arg;arg;...>" -DOUT=<prefix> -P report_determinism.cmake

if(NOT DEFINED TOOL OR NOT DEFINED ARGS OR NOT DEFINED OUT)
    message(FATAL_ERROR "report_determinism.cmake: TOOL, ARGS and OUT are required")
endif()

execute_process(
    COMMAND ${TOOL} ${ARGS}
    OUTPUT_FILE "${OUT}.a"
    ERROR_VARIABLE first_stderr
    RESULT_VARIABLE first_code)
if(NOT first_code EQUAL 0)
    message(FATAL_ERROR "first run exited ${first_code}\n--- stderr ---\n${first_stderr}")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env LC_ALL=de_DE.UTF-8 TZ=Asia/Tokyo ${TOOL} ${ARGS}
    OUTPUT_FILE "${OUT}.b"
    ERROR_VARIABLE second_stderr
    RESULT_VARIABLE second_code)
if(NOT second_code EQUAL 0)
    message(FATAL_ERROR "second run exited ${second_code}\n--- stderr ---\n${second_stderr}")
endif()

file(READ "${OUT}.a" first_hex HEX)
file(READ "${OUT}.b" second_hex HEX)
if(NOT first_hex STREQUAL second_hex)
    message(FATAL_ERROR "the two processes disagree on the bytes")
endif()

message(STATUS "two locales, one document")
