function(run_inspect name expected source)
    execute_process(
        COMMAND "${SOL_EXECUTABLE}" inspect "${source}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT result EQUAL expected)
        message(FATAL_ERROR "${name}: exit ${result}, expected ${expected}\n${output}${error}")
    endif()
    set("${name}_output" "${output}" PARENT_SCOPE)
    set("${name}_error" "${error}" PARENT_SCOPE)
    if(expected EQUAL 0)
        file(WRITE "${TEST_DIR}/${name}.json" "${output}")
        execute_process(
            COMMAND "${PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/validate_inspection.py"
                "${TEST_DIR}/${name}.json" "${SCHEMA_FILE}" ${ARGN}
            RESULT_VARIABLE validation_result
            OUTPUT_VARIABLE validation_output
            ERROR_VARIABLE validation_error
        )
        if(NOT validation_result EQUAL 0)
            message(FATAL_ERROR "${name}: validation failed\n${validation_output}${validation_error}")
        endif()
    endif()
endfunction()

file(MAKE_DIRECTORY "${TEST_DIR}")
run_inspect(single 0 "${SOURCE_FILE}")
run_inspect(repeat 0 "${SOURCE_FILE}")
if(NOT single_output STREQUAL repeat_output)
    message(FATAL_ERROR "inspection output is not deterministic")
endif()
foreach(required
    "\"schema\":\"sol.inspection\""
    "\"syntax\":{\"schema\":\"sol.inspection.syntax\""
    "\"hir\":{\"schema\":\"sol.inspection.hir\""
    "\"types\":{\"schema\":\"sol.inspection.types\""
    "\"effects\":{\"schema\":\"sol.inspection.effects\""
    "\"contracts\":{\"schema\":\"sol.inspection.contracts\""
    "\"diagnostics\":{\"schema\":\"sol.inspection.diagnostics\""
    "\"sourceBase64\":\"bW9kdWxl"
    "sem:1:[0-9a-f][0-9a-f][0-9a-f][0-9a-f]"
)
    if(NOT single_output MATCHES "${required}")
        message(FATAL_ERROR "inspection is missing ${required}: ${single_output}")
    endif()
endforeach()
string(REGEX MATCH "sem:1:[0-9a-f]+" semantic_id "${single_output}")
string(LENGTH "${semantic_id}" semantic_id_length)
if(NOT semantic_id_length EQUAL 38)
    message(FATAL_ERROR "semantic ID is not a versioned 128-bit ID: ${semantic_id}")
endif()
string(REGEX MATCHALL "\"schema\":\"sol.inspection\"" envelope_count "${single_output}")
list(LENGTH envelope_count envelope_length)
if(NOT envelope_length EQUAL 1 OR NOT single_output MATCHES "\"byteLength\":[0-9]+")
    message(FATAL_ERROR "single-file inspection envelope mismatch")
endif()

run_inspect(package 0 "${PACKAGE_DIR}")
run_inspect(package_slash 0 "${PACKAGE_DIR}/")
if(NOT package_output STREQUAL package_slash_output)
    message(FATAL_ERROR "trailing slash changes package inspection output")
endif()
if(NOT package_output MATCHES "\"kind\":\"directory\""
    OR NOT package_output MATCHES "services/read.sol"
    OR package_output MATCHES "${PACKAGE_DIR}/services/read.sol")
    message(FATAL_ERROR "package paths are not normalized: ${package_output}")
endif()

run_inspect(generic 0 "${GENERIC_FILE}" --require-generic)

run_inspect(compile_failure 1 "${INVALID_FILE}")
if(NOT compile_failure_output MATCHES "\"schema\":\"sol.diagnostic/1\""
    OR compile_failure_output MATCHES "sol.inspection")
    message(FATAL_ERROR "compile failure emitted an inspection object: ${compile_failure_output}")
endif()

run_inspect(load_failure 1 "${SOURCE_FILE}.missing")
if(NOT load_failure_output MATCHES "\"schema\":\"sol.cli-error/1\""
    OR NOT load_failure_output MATCHES "\"kind\":\"load\"")
    message(FATAL_ERROR "load failure mismatch: ${load_failure_output}")
endif()

execute_process(
    COMMAND "${SOL_EXECUTABLE}" inspect --diagnostic-format=json "${SOURCE_FILE}"
    RESULT_VARIABLE usage_result
    ERROR_VARIABLE usage_error
)
if(NOT usage_result EQUAL 2 OR NOT usage_error MATCHES "exactly one")
    message(FATAL_ERROR "inspection usage mismatch: ${usage_result} ${usage_error}")
endif()
