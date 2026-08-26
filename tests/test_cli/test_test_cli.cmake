function(run_test name expected source format)
    set(arguments test)
    if(NOT "${format}" STREQUAL "")
        list(APPEND arguments "--diagnostic-format=${format}")
    endif()
    list(APPEND arguments "${source}")
    execute_process(
        COMMAND "${SOL_EXECUTABLE}" ${arguments}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT result EQUAL expected)
        message(FATAL_ERROR "${name}: exit ${result}, expected ${expected}\n${output}${error}")
    endif()
    set("${name}_output" "${output}" PARENT_SCOPE)
    set("${name}_error" "${error}" PARENT_SCOPE)
endfunction()

function(run_cli name expected)
    execute_process(
        COMMAND "${SOL_EXECUTABLE}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT result EQUAL expected)
        message(FATAL_ERROR "${name}: exit ${result}, expected ${expected}\n${output}${error}")
    endif()
    set("${name}_output" "${output}" PARENT_SCOPE)
    set("${name}_error" "${error}" PARENT_SCOPE)
endfunction()

run_test(pass 0 "${FIXTURE_DIR}/pass.sol" human)
if(NOT pass_output MATCHES "PASS \"truth\"" OR NOT pass_output MATCHES "2 tests, 2 passed, 0 failed")
    message(FATAL_ERROR "pass output mismatch: ${pass_output}")
endif()

run_test(false 1 "${FIXTURE_DIR}/false.sol" human)
if(NOT false_output MATCHES "evaluated to false")
    message(FATAL_ERROR "false output mismatch: ${false_output}")
endif()

run_test(runtime 1 "${FIXTURE_DIR}/runtime.sol" human)
if(NOT runtime_output MATCHES "integer division by zero")
    message(FATAL_ERROR "runtime output mismatch: ${runtime_output}")
endif()

run_test(contract_failure 1 "${FIXTURE_DIR}/contract_failure.sol" human)
if(NOT contract_failure_output MATCHES "requires contract was not satisfied")
    message(FATAL_ERROR "contract test output mismatch: ${contract_failure_output}")
endif()

run_test(cross_file_runtime 1 "${FIXTURE_DIR}/cross_file_runtime" human)
if(NOT cross_file_runtime_output MATCHES "cross-file runtime"
    OR NOT cross_file_runtime_output MATCHES "helper.sol:[0-9]+: integer division by zero")
    message(FATAL_ERROR "cross-file runtime output mismatch: ${cross_file_runtime_output}")
endif()

run_test(mixed 1 "${FIXTURE_DIR}/mixed" human)
string(FIND "${mixed_output}" "a pass" a_pass)
string(FIND "${mixed_output}" "a false" a_false)
string(FIND "${mixed_output}" "b runtime" b_runtime)
string(FIND "${mixed_output}" "b pass" b_pass)
if(a_pass LESS 0 OR NOT a_pass LESS a_false OR NOT a_false LESS b_runtime
    OR NOT b_runtime LESS b_pass OR NOT mixed_output MATCHES "4 tests, 2 passed, 2 failed")
    message(FATAL_ERROR "mixed order/output mismatch: ${mixed_output}")
endif()

run_test(compile_failure 1 "${FIXTURE_DIR}/compile_failure.sol" human)
if(NOT compile_failure_error MATCHES "SOL-TYPE-004" OR NOT compile_failure_output STREQUAL "")
    message(FATAL_ERROR "compile failure mismatch: ${compile_failure_output}${compile_failure_error}")
endif()

run_test(no_tests 0 "${FIXTURE_DIR}/no_tests.sol" human)
if(NOT no_tests_output MATCHES "0 tests, 0 passed, 0 failed")
    message(FATAL_ERROR "no-tests output mismatch: ${no_tests_output}")
endif()

run_test(same_labels 0 "${FIXTURE_DIR}/same_labels" human)
if(NOT same_labels_output MATCHES "2 tests, 2 passed, 0 failed")
    message(FATAL_ERROR "cross-module labels mismatch: ${same_labels_output}")
endif()

run_test(json 0 "${FIXTURE_DIR}/pass.sol" json)
if(NOT json_output MATCHES "\"schema\":\"sol.test-results\""
    OR NOT json_output MATCHES "\"version\":1"
    OR NOT json_output MATCHES "\"status\":\"passed\"")
    message(FATAL_ERROR "JSON output mismatch: ${json_output}")
endif()

run_test(non_ascii 0 "${FIXTURE_DIR}/non_ascii_é.sol" json)
string(FIND "${non_ascii_output}" "\"label\":\"café\"" semantic_non_ascii)
string(FIND "${non_ascii_output}" "non_ascii_é.sol" semantic_path)
if(semantic_non_ascii LESS 0 OR semantic_path LESS 0)
    message(FATAL_ERROR "non-ASCII JSON byte escaping mismatch: ${non_ascii_output}")
endif()

run_test(json_compile_failure 1 "${FIXTURE_DIR}/compile_failure.sol" json)
if(NOT json_compile_failure_output MATCHES "\"schema\":\"sol.diagnostic/1\""
    OR json_compile_failure_output MATCHES "sol.test-results")
    message(FATAL_ERROR "JSON compilation diagnostic mismatch: ${json_compile_failure_output}")
endif()

set(missing "${FIXTURE_DIR}/does-not-exist.sol")
foreach(command check test)
    set(human_name "missing_${command}_human")
    run_cli("${human_name}" 1 "${command}" "${missing}")
    string(REGEX MATCHALL "\n" human_newlines "${${human_name}_error}")
    list(LENGTH human_newlines human_line_count)
    if(NOT "${${human_name}_output}" STREQUAL ""
        OR NOT "${${human_name}_error}" MATCHES "cannot inspect"
        OR "${${human_name}_error}" MATCHES "out of memory"
        OR NOT human_line_count EQUAL 1)
        message(FATAL_ERROR "${command} human load error mismatch: ${${human_name}_output}${${human_name}_error}")
    endif()
    set(json_name "missing_${command}_json")
    run_cli("${json_name}" 1 "${command}"
        --diagnostic-format=json "${missing}")
    if(NOT "${${json_name}_error}" STREQUAL ""
        OR NOT "${${json_name}_output}" MATCHES "\"schema\":\"sol.cli-error/1\""
        OR NOT "${${json_name}_output}" MATCHES "\"kind\":\"load\""
        OR "${${json_name}_output}" MATCHES "^\\[\\]$")
        message(FATAL_ERROR "${command} JSON load error mismatch: ${${json_name}_output}${${json_name}_error}")
    endif()
endforeach()

execute_process(
    COMMAND "${SOL_EXECUTABLE}" test --bad "${FIXTURE_DIR}/pass.sol"
    RESULT_VARIABLE usage_result
    OUTPUT_VARIABLE usage_output
    ERROR_VARIABLE usage_error
)
if(NOT usage_result EQUAL 2 OR NOT usage_error MATCHES "unknown option")
    message(FATAL_ERROR "usage mismatch: ${usage_result} ${usage_output}${usage_error}")
endif()

execute_process(
    COMMAND "${SOL_EXECUTABLE}" fmt --check "${FIXTURE_DIR}/pass.sol"
    RESULT_VARIABLE format_result
    OUTPUT_VARIABLE format_output
    ERROR_VARIABLE format_error
)
if(NOT format_result EQUAL 0)
    message(FATAL_ERROR "test declaration formatter mismatch: ${format_output}${format_error}")
endif()

if(UNIX AND NOT "${CLI_IO_HELPER}" STREQUAL "")
    execute_process(
        COMMAND "${CLI_IO_HELPER}" "${SOL_EXECUTABLE}" "${FIXTURE_DIR}/pass.sol"
            "${FIXTURE_DIR}/non_ascii_é.sol" "${RUN_FIXTURE}" "${RUN_NONZERO_FIXTURE}"
        RESULT_VARIABLE io_result
    )
    if(NOT io_result EQUAL 0)
        message(FATAL_ERROR "CLI stdout/invalid-byte helper failed: ${io_result}")
    endif()
endif()
