function(run_case name expected)
    execute_process(
        COMMAND "${SOL_EXECUTABLE}" run ${ARGN}
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

run_case(unit 0 "${FIXTURE_DIR}/unit.sol")
if(NOT unit_output STREQUAL "" OR NOT unit_error STREQUAL "")
    message(FATAL_ERROR "unit output mismatch: ${unit_output}${unit_error}")
endif()

run_case(exit_42 42 "${FIXTURE_DIR}/exit_42.sol")
run_case(exit_256 1 "${FIXTURE_DIR}/exit_256.sol")
if(NOT exit_256_error MATCHES "SOL-RUN-002")
    message(FATAL_ERROR "exit boundary mismatch: ${exit_256_output}${exit_256_error}")
endif()
run_case(exit_negative 1 "${FIXTURE_DIR}/exit_negative.sol")
if(NOT exit_negative_error MATCHES "SOL-RUN-002")
    message(FATAL_ERROR "negative exit boundary mismatch")
endif()

run_case(console 0 "${FIXTURE_DIR}/console.sol")
if(NOT console_output STREQUAL "hello world\n" OR NOT console_error STREQUAL "")
    message(FATAL_ERROR "console mismatch: ${console_output}${console_error}")
endif()

run_case(arguments 2 "${FIXTURE_DIR}/arguments.sol" -- first second)
run_case(arguments_empty 100 "${FIXTURE_DIR}/arguments.sol")
run_case(arguments_count 3 "${FIXTURE_DIR}/arguments_count.sol" -- one two three)
run_case(configuration 7 --config=mode=test "${FIXTURE_DIR}/configuration.sol")
run_case(configuration_missing 1 "${FIXTURE_DIR}/configuration.sol")
run_case(configuration_empty 1 --config=mode= "${FIXTURE_DIR}/configuration.sol")
run_case(dashed_argument 100 "${FIXTURE_DIR}/arguments.sol" -- --bad)

run_case(no_entry 1 "${FIXTURE_DIR}/no_entry.sol")
if(NOT no_entry_error MATCHES "SOL-RUN-001")
    message(FATAL_ERROR "missing entry mismatch: ${no_entry_output}${no_entry_error}")
endif()

run_case(panic 1 "${FIXTURE_DIR}/panic.sol")
if(NOT panic_error MATCHES "SOL-RUNTIME-PANIC" OR NOT panic_error MATCHES "boom")
    message(FATAL_ERROR "panic mismatch: ${panic_output}${panic_error}")
endif()

run_case(unsupported 1 "${FIXTURE_DIR}/unsupported.sol")
if(NOT unsupported_output STREQUAL "" OR NOT unsupported_error MATCHES "SOL-RUN-003")
    message(FATAL_ERROR "unsupported host mismatch: ${unsupported_output}${unsupported_error}")
endif()

run_case(lookalike_console 1 "${FIXTURE_DIR}/lookalike_console.sol")
if(NOT lookalike_console_output STREQUAL ""
    OR NOT lookalike_console_error MATCHES "SOL-RUN-003")
    message(FATAL_ERROR "lookalike console escaped preflight: ${lookalike_console_output}${lookalike_console_error}")
endif()

run_case(package 9 "${FIXTURE_DIR}/package")

run_case(json_console 0 --diagnostic-format=json "${FIXTURE_DIR}/console.sol")
if(NOT json_console_output MATCHES "\"schema\":\"sol.run-result\""
    OR NOT json_console_output MATCHES "\"outcome\":\"returned\""
    OR NOT json_console_output MATCHES "\"data\":\"aGVsbG8gd29ybGQK\"")
    message(FATAL_ERROR "JSON console mismatch: ${json_console_output}${json_console_error}")
endif()

run_case(json_exit 42 --diagnostic-format=json "${FIXTURE_DIR}/exit_42.sol")
if(NOT json_exit_output MATCHES "\"outcome\":\"returned\""
    OR NOT json_exit_output MATCHES "\"exit_status\":42")
    message(FATAL_ERROR "JSON nonzero return mismatch: ${json_exit_output}${json_exit_error}")
endif()

run_case(json_one 0 --diagnostic-format=json "${FIXTURE_DIR}/console_one.sol")
if(NOT json_one_output MATCHES "\"data\":\"YQ==\"")
    message(FATAL_ERROR "one-byte base64 mismatch: ${json_one_output}")
endif()
run_case(json_two 0 --diagnostic-format=json "${FIXTURE_DIR}/console_two.sol")
if(NOT json_two_output MATCHES "\"data\":\"YWI=\"")
    message(FATAL_ERROR "two-byte base64 mismatch: ${json_two_output}")
endif()

run_case(json_panic 1 --diagnostic-format=json "${FIXTURE_DIR}/panic.sol")
if(NOT json_panic_output MATCHES "\"outcome\":\"runtime_error\""
    OR NOT json_panic_output MATCHES "SOL-RUNTIME-PANIC")
    message(FATAL_ERROR "JSON panic mismatch: ${json_panic_output}${json_panic_error}")
endif()

run_case(json_no_entry 1 --diagnostic-format=json "${FIXTURE_DIR}/no_entry.sol")
if(NOT json_no_entry_output MATCHES "application_boundary_error"
    OR NOT json_no_entry_output MATCHES "SOL-RUN-001")
    message(FATAL_ERROR "JSON missing entry mismatch: ${json_no_entry_output}${json_no_entry_error}")
endif()

run_case(compile_failure 1 "${FIXTURE_DIR}/compile_failure.sol")
if(NOT compile_failure_error MATCHES "SOL-TYPE-004")
    message(FATAL_ERROR "compile failure mismatch: ${compile_failure_output}${compile_failure_error}")
endif()

run_case(json_compile_failure 1 --diagnostic-format=json "${FIXTURE_DIR}/compile_failure.sol")
if(NOT json_compile_failure_output MATCHES "sol.diagnostic/1"
    OR json_compile_failure_output MATCHES "sol.run-result")
    message(FATAL_ERROR "JSON compile failure mismatch: ${json_compile_failure_output}${json_compile_failure_error}")
endif()

run_case(duplicate_config 2 --config=mode=a --config=mode=b "${FIXTURE_DIR}/unit.sol")
if(NOT duplicate_config_error MATCHES "duplicate")
    message(FATAL_ERROR "duplicate config usage mismatch")
endif()

run_case(unknown_option 2 --bad "${FIXTURE_DIR}/unit.sol")
if(NOT unknown_option_error MATCHES "unknown option")
    message(FATAL_ERROR "unknown option mismatch")
endif()
run_case(malformed_config 2 --config==value "${FIXTURE_DIR}/unit.sol")
if(NOT malformed_config_error MATCHES "non-empty KEY=VALUE")
    message(FATAL_ERROR "malformed config mismatch")
endif()
run_case(early_delimiter 2 -- "${FIXTURE_DIR}/unit.sol")
if(NOT early_delimiter_error MATCHES "requires a source path")
    message(FATAL_ERROR "early delimiter mismatch")
endif()
run_case(extra_operand 2 "${FIXTURE_DIR}/unit.sol" "${FIXTURE_DIR}/exit_42.sol")
if(NOT extra_operand_error MATCHES "accepts one source")
    message(FATAL_ERROR "extra operand mismatch")
endif()

run_case(missing_path 1 "${FIXTURE_DIR}/missing.sol")
if(NOT missing_path_error MATCHES "cannot inspect" OR NOT missing_path_output STREQUAL "")
    message(FATAL_ERROR "missing path mismatch: ${missing_path_output}${missing_path_error}")
endif()
