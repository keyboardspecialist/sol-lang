function(run_case name)
    execute_process(
        COMMAND "${SOL_EXECUTABLE}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${name}: exit ${result}\n${output}${error}")
    endif()
    set("${name}_output" "${output}" PARENT_SCOPE)
    set("${name}_error" "${error}" PARENT_SCOPE)
endfunction()

run_case(format fmt --check "${PACKAGE_DIR}")
if(NOT format_output STREQUAL "" OR NOT format_error STREQUAL "")
    message(FATAL_ERROR "formatter output mismatch: ${format_output}${format_error}")
endif()

run_case(check check "${PACKAGE_DIR}")
if(NOT check_output MATCHES "checked .*: 3 files, 23 declarations"
    OR NOT check_error STREQUAL "")
    message(FATAL_ERROR "check output mismatch: ${check_output}${check_error}")
endif()

run_case(test test --diagnostic-format=json "${PACKAGE_DIR}")
if(NOT test_output MATCHES "\"schema\":\"sol.test-results\""
    OR NOT test_output MATCHES "\"version\":1"
    OR NOT test_output MATCHES "\"label\":\"contracted ownership core\""
    OR NOT test_output MATCHES "\"label\":\"typed error path\""
    OR NOT test_output MATCHES "\"total\":4"
    OR NOT test_output MATCHES "\"passed\":4"
    OR NOT test_output MATCHES "\"failed\":0"
    OR NOT test_error STREQUAL "")
    message(FATAL_ERROR "test output mismatch: ${test_output}${test_error}")
endif()

run_case(effects effects --diagnostic-format=json "${PACKAGE_DIR}")
if(NOT effects_output MATCHES "\"schema\":\"sol.effects\""
    OR NOT effects_output MATCHES "\"name\":\"compute\""
    OR NOT effects_output MATCHES "\"name\":\"fail_runtime\""
    OR NOT effects_output MATCHES "\"name\":\"launch\""
    OR NOT effects_output MATCHES "\"name\":\"panic\""
    OR NOT effects_output MATCHES "\"target\":\"fail_runtime\""
    OR NOT effects_output MATCHES "\"name\":\"console.write\""
    OR NOT effects_output MATCHES "\"name\":\"configuration.read\""
    OR NOT effects_error STREQUAL "")
    message(FATAL_ERROR "effects output mismatch: ${effects_output}${effects_error}")
endif()

run_case(inspect inspect "${PACKAGE_DIR}")
if(NOT inspect_output MATCHES "\"schema\":\"sol.inspection\""
    OR NOT inspect_output MATCHES "\"version\":3"
    OR NOT inspect_output MATCHES "conformance.domain"
    OR NOT inspect_output MATCHES "conformance.core"
    OR NOT inspect_output MATCHES "conformance.main"
    OR NOT inspect_output MATCHES "conformance.compute.v1"
    OR NOT inspect_output MATCHES "requires"
    OR NOT inspect_output MATCHES "snapshots"
    OR NOT inspect_error STREQUAL "")
    message(FATAL_ERROR "inspection output mismatch: ${inspect_output}${inspect_error}")
endif()

run_case(run run --config=mode=e6 "${PACKAGE_DIR}" -- fixture)
if(NOT run_output STREQUAL "E6 ok\n" OR NOT run_error STREQUAL "")
    message(FATAL_ERROR "run output mismatch: ${run_output}${run_error}")
endif()

execute_process(
    COMMAND "${SOL_EXECUTABLE}" run --config=mode=panic "${PACKAGE_DIR}"
    RESULT_VARIABLE failure_result
    OUTPUT_VARIABLE failure_output
    ERROR_VARIABLE failure_error
)
if(NOT failure_result EQUAL 1
    OR NOT failure_output STREQUAL ""
    OR NOT failure_error MATCHES "SOL-RUNTIME-PANIC"
    OR NOT failure_error MATCHES "conformance failure")
    message(FATAL_ERROR "runtime failure mismatch: ${failure_output}${failure_error}")
endif()
