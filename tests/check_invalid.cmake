execute_process(
    COMMAND "${SOL_EXECUTABLE}" check --diagnostic-format=json "${SOL_SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)

if(NOT result EQUAL 1)
    message(FATAL_ERROR "expected compiler exit code 1, got ${result}: ${error}")
endif()

if(NOT output MATCHES "\"code\":\"SOL-PARSE-004\"")
    message(FATAL_ERROR "expected SOL-PARSE-004 JSON diagnostic, got: ${output}")
endif()
