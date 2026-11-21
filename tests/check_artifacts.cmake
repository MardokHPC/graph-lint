# docs/emitters.md section 6: a second process, not a second call. ctest sets LC_ALL and
# TZ on this entry, so a locale-dependent byte or a warmed static shows up as a red test.
if(NOT DEFINED EMIT OR NOT DEFINED GRAPH_DIR OR NOT DEFINED OUT)
    message(FATAL_ERROR "artifacts_locale needs -DEMIT, -DGRAPH_DIR and -DOUT")
endif()

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

# emit_artifacts reads and writes one directory: give the copy the four JSON as input.
foreach(name built captured pingpong chain)
    file(COPY "${GRAPH_DIR}/${name}.json" DESTINATION "${OUT}")
endforeach()

execute_process(
    COMMAND "${EMIT}" --out "${OUT}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE report
    ERROR_VARIABLE errors
)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "emit_artifacts exited ${status}: ${errors}")
endif()
message(STATUS "${report}")

foreach(name built captured pingpong chain)
    foreach(extension dot mmd)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E compare_files
                    "${GRAPH_DIR}/${name}.${extension}" "${OUT}/${name}.${extension}"
            RESULT_VARIABLE differs
        )
        if(NOT differs EQUAL 0)
            message(FATAL_ERROR
                "${name}.${extension}: a second process differs from ${GRAPH_DIR}")
        endif()
    endforeach()
endforeach()

message(STATUS "artifacts_locale: 8 files identical to ${GRAPH_DIR}")
