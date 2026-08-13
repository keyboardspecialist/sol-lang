function(require_result actual expected context)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "${context}: expected exit ${expected}, got ${actual}")
    endif()
endfunction()

function(require_equal actual expected context)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files "${actual}" "${expected}"
        RESULT_VARIABLE comparison
    )
    if(NOT comparison EQUAL 0)
        message(FATAL_ERROR "${context}: files differ: ${actual} and ${expected}")
    endif()
endfunction()

function(require_no_transaction_artifacts directory context)
    file(GLOB_RECURSE artifacts
        "${directory}/*.solfmt-stage.*"
        "${directory}/*.solfmt-backup.*"
    )
    if(artifacts)
        list(JOIN artifacts ", " artifact_list)
        message(FATAL_ERROR "${context}: formatter transaction artifacts remain: ${artifact_list}")
    endif()
endfunction()

execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -rf "${TEST_DIR}" RESULT_VARIABLE setup_result)
require_result("${setup_result}" 0 "remove old test directory")
execute_process(COMMAND "${CMAKE_COMMAND}" -E make_directory "${TEST_DIR}" RESULT_VARIABLE setup_result)
require_result("${setup_result}" 0 "create test directory")

if(CASE STREQUAL "malformed_directory")
    set(input "${TEST_DIR}/package")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${FIXTURE_DIR}/malformed_directory" "${input}"
        RESULT_VARIABLE setup_result
    )
    require_result("${setup_result}" 0 "copy malformed directory fixture")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${input}/good.sol" "${TEST_DIR}/good.before" RESULT_VARIABLE setup_result)
    require_result("${setup_result}" 0 "snapshot valid file")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${input}/bad.sol" "${TEST_DIR}/bad.before" RESULT_VARIABLE setup_result)
    require_result("${setup_result}" 0 "snapshot malformed file")

    execute_process(
        COMMAND "${SOL_EXECUTABLE}" fmt "${input}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    require_result("${result}" 1 "format directory containing malformed source")
    require_equal("${input}/good.sol" "${TEST_DIR}/good.before" "valid file changed after directory failure")
    require_equal("${input}/bad.sol" "${TEST_DIR}/bad.before" "malformed file changed after directory failure")
    require_no_transaction_artifacts("${TEST_DIR}" "malformed directory cleanup")
    return()
endif()

if(CASE STREQUAL "symlink")
    set(target "${TEST_DIR}/target.sol")
    set(input "${TEST_DIR}/input.sol")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${FIXTURE_DIR}/unformatted.sol" "${target}" RESULT_VARIABLE setup_result)
    require_result("${setup_result}" 0 "copy symlink target")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${target}" "${TEST_DIR}/target.before" RESULT_VARIABLE setup_result)
    require_result("${setup_result}" 0 "snapshot symlink target")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E create_symlink "${target}" "${input}" RESULT_VARIABLE setup_result)
    require_result("${setup_result}" 0 "create formatter symlink operand")

    execute_process(
        COMMAND "${SOL_EXECUTABLE}" fmt "${input}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    require_result("${result}" 1 "format direct symlink operand")
    if(NOT error MATCHES "refusing to format symbolic link")
        message(FATAL_ERROR "missing symbolic link rejection diagnostic: ${error}")
    endif()
    require_equal("${target}" "${TEST_DIR}/target.before" "symlink target changed after rejection")
    if(NOT IS_SYMLINK "${input}")
        message(FATAL_ERROR "formatter replaced direct symlink operand")
    endif()
    return()
endif()

set(input "${TEST_DIR}/input.sol")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${FIXTURE_DIR}/unformatted.sol" "${input}" RESULT_VARIABLE setup_result)
require_result("${setup_result}" 0 "copy input fixture")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${input}" "${TEST_DIR}/input.before" RESULT_VARIABLE setup_result)
require_result("${setup_result}" 0 "snapshot input fixture")

if(CASE STREQUAL "stdout")
    execute_process(
        COMMAND "${SOL_EXECUTABLE}" fmt --stdout "${input}"
        RESULT_VARIABLE result
        OUTPUT_FILE "${TEST_DIR}/stdout.actual"
        ERROR_VARIABLE error
    )
    require_result("${result}" 0 "fmt --stdout")
    require_equal("${TEST_DIR}/stdout.actual" "${FIXTURE_DIR}/expected.sol" "fmt --stdout output")
    require_equal("${input}" "${TEST_DIR}/input.before" "fmt --stdout modified its input")
elseif(CASE STREQUAL "check")
    execute_process(
        COMMAND "${SOL_EXECUTABLE}" fmt --check "${input}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    require_result("${result}" 1 "fmt --check on unformatted input")
    string(FIND "${error}" "${input}: not formatted" diagnostic_position)
    if(diagnostic_position EQUAL -1)
        message(FATAL_ERROR "fmt --check did not identify ${input}: ${error}")
    endif()
    require_equal("${input}" "${TEST_DIR}/input.before" "fmt --check modified its input")
elseif(CASE STREQUAL "rewrite")
    execute_process(COMMAND "${SOL_EXECUTABLE}" fmt "${input}" RESULT_VARIABLE result ERROR_VARIABLE error)
    require_result("${result}" 0 "fmt rewrite")
    require_equal("${input}" "${FIXTURE_DIR}/expected.sol" "fmt rewrite output")
    require_no_transaction_artifacts("${TEST_DIR}" "successful rewrite cleanup")

    execute_process(COMMAND "${SOL_EXECUTABLE}" fmt --check "${input}" RESULT_VARIABLE result ERROR_VARIABLE error)
    require_result("${result}" 0 "fmt --check after rewrite")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${input}" "${TEST_DIR}/first-rewrite.sol" RESULT_VARIABLE setup_result)
    require_result("${setup_result}" 0 "snapshot first rewrite")
    execute_process(COMMAND "${SOL_EXECUTABLE}" fmt "${input}" RESULT_VARIABLE result ERROR_VARIABLE error)
    require_result("${result}" 0 "second fmt rewrite")
    require_equal("${input}" "${TEST_DIR}/first-rewrite.sol" "second rewrite was not byte-idempotent")
    require_no_transaction_artifacts("${TEST_DIR}" "idempotent rewrite cleanup")
elseif(CASE STREQUAL "exclusive_options")
    execute_process(
        COMMAND "${SOL_EXECUTABLE}" fmt --check --stdout "${input}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    require_result("${result}" 2 "fmt with mutually exclusive options")
    if(NOT error MATCHES "mutually exclusive")
        message(FATAL_ERROR "missing mutually exclusive options diagnostic: ${error}")
    endif()
    require_equal("${input}" "${TEST_DIR}/input.before" "invalid option usage modified its input")
else()
    message(FATAL_ERROR "unknown formatter CLI test case: ${CASE}")
endif()
