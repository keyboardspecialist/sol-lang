function(run_effects name expected format source)
    set(arguments effects)
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

run_effects(human 0 human "${SOURCE_FILE}")
if(NOT human_output MATCHES "effects: E"
    OR NOT human_output MATCHES "callback <dynamic>.*effects: E"
    OR NOT human_output MATCHES "clock.read<clock>"
    OR NOT human_output MATCHES "capability Clock.now effects: clock.read<clock>"
    OR NOT human_output MATCHES "authority: random, provider"
    OR NOT human_output MATCHES "capability Clock.now")
    message(FATAL_ERROR "human effects output mismatch: ${human_output}")
endif()

run_effects(json 0 json "${SOURCE_FILE}")
run_effects(json_repeat 0 json "${SOURCE_FILE}")
string(FIND "${json_output}" "\"authority\":{\"kind\":\"local\",\"name\":\"clock\"}" local_authority)
string(FIND "${json_output}" "\"authority\":[\"random\",\"provider\"]" callable_authority)
if(NOT json_output STREQUAL json_repeat_output
    OR NOT json_output MATCHES "\"schema\":\"sol.effects\""
    OR NOT json_output MATCHES "\"effect_parameter\":\"E\""
    OR NOT json_output MATCHES "\"kind\":\"callback\",\"target\":null,\"dynamic\":true"
    OR local_authority LESS 0 OR callable_authority LESS 0)
    message(FATAL_ERROR "JSON effects output mismatch: ${json_output}")
endif()

run_effects(package 0 json "${PACKAGE_DIR}")
if(NOT package_output MATCHES "services/read.sol"
    OR NOT package_output MATCHES "\"kind\":\"capability\"")
    message(FATAL_ERROR "package effects output mismatch: ${package_output}")
endif()

run_effects(substitution 0 human "${SUBSTITUTION_FILE}")
if(NOT substitution_output MATCHES "function consume effects: service.read<actual>"
    OR NOT substitution_output MATCHES "capability Reader.copy effects: service.read<left>, service.read<right>")
    message(FATAL_ERROR "call authority substitution mismatch: ${substitution_output}")
endif()
string(REGEX MATCH "capability Reader.both effects: service.read<reader>[^,]" collapsed_row
    "${substitution_output}")
if("${collapsed_row}" STREQUAL "")
    message(FATAL_ERROR "collapsed call row was not deduplicated: ${substitution_output}")
endif()

run_effects(compile_failure 1 json "${INVALID_FILE}")
if(NOT compile_failure_output MATCHES "\"schema\":\"sol.diagnostic/1\""
    OR compile_failure_output MATCHES "sol.effects")
    message(FATAL_ERROR "effects compile failure mismatch: ${compile_failure_output}")
endif()

run_effects(load_failure 1 json "${SOURCE_FILE}.missing")
if(NOT load_failure_output MATCHES "\"schema\":\"sol.cli-error/1\""
    OR NOT load_failure_output MATCHES "\"kind\":\"load\"")
    message(FATAL_ERROR "effects load failure mismatch: ${load_failure_output}")
endif()

execute_process(
    COMMAND "${SOL_EXECUTABLE}" effects --bad "${SOURCE_FILE}"
    RESULT_VARIABLE usage_result
    ERROR_VARIABLE usage_error
)
if(NOT usage_result EQUAL 2 OR NOT usage_error MATCHES "unknown option")
    message(FATAL_ERROR "effects usage mismatch: ${usage_result} ${usage_error}")
endif()
