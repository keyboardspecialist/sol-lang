execute_process(
    COMMAND "${SOL_EXECUTABLE}" check --diagnostic-format=json "${SOL_SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)

if(NOT result EQUAL 1)
    message(FATAL_ERROR "expected compiler exit code 1, got ${result}: ${error}")
endif()

if(NOT output MATCHES "\"code\":\"${EXPECTED_CODE}\"")
    message(FATAL_ERROR "expected ${EXPECTED_CODE} JSON diagnostic, got: ${output}")
endif()
