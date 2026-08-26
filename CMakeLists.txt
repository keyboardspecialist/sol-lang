cmake_minimum_required(VERSION 3.17)

project(sol VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_EXTENSIONS OFF)

option(SOL_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(SOL_ENABLE_WERROR "Treat compiler warnings as errors" OFF)
option(SOL_BUILD_FUZZERS "Build Clang libFuzzer targets" OFF)

if(SOL_ENABLE_WERROR)
    if(MSVC)
        add_compile_options(/WX)
    elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(-Werror)
    endif()
endif()

if(SOL_BUILD_FUZZERS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND UNIX)
        include(CheckCSourceCompiles)
        set(SOL_SAVED_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
        set(CMAKE_REQUIRED_FLAGS
            "${CMAKE_REQUIRED_FLAGS} -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer")
        check_c_source_compiles(
            "#include <stddef.h>\n#include <stdint.h>\nint LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { (void)data; (void)size; return 0; }"
            SOL_HAS_LIBFUZZER
        )
        set(CMAKE_REQUIRED_FLAGS "${SOL_SAVED_REQUIRED_FLAGS}")
    endif()
    if(NOT SOL_HAS_LIBFUZZER)
        message(WARNING "SOL_BUILD_FUZZERS requires a usable Clang libFuzzer runtime; fuzz targets are disabled")
        set(SOL_BUILD_FUZZERS OFF)
    endif()
endif()

find_program(TYPST_EXECUTABLE typst)
if(TYPST_EXECUTABLE)
    add_custom_target(manual
        COMMAND ${TYPST_EXECUTABLE} compile
            ${CMAKE_CURRENT_SOURCE_DIR}/docs/specification.typ
            ${CMAKE_CURRENT_SOURCE_DIR}/Sol_Programming_Language_Design_Specification_v0.2.pdf
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/docs/specification.typ
        COMMENT "Building Sol Design Specification v0.2"
        VERBATIM
    )
else()
    add_custom_target(manual
        COMMAND ${CMAKE_COMMAND} -E echo
            "Typst was not found. Install Typst 0.15.1, reconfigure CMake, or run: typst compile docs/specification.typ Sol_Programming_Language_Design_Specification_v0.2.pdf"
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM
    )
endif()

add_library(sol_compiler STATIC
    src/compilation.c
    src/contract.c
    src/diagnostic.c
    src/effectcheck.c
    src/effects.c
    src/formatter.c
    src/hir.c
    src/inspection.c
    src/ir.c
    src/interpreter.c
    src/lexer.c
    src/ownership.c
    src/package.c
    src/parser.c
    src/resource.c
    src/source.c
    src/syntax.c
    src/typecheck.c
)

target_include_directories(sol_compiler PUBLIC include)

if(SOL_BUILD_FUZZERS)
    target_compile_options(sol_compiler PRIVATE
        -fsanitize=fuzzer-no-link,address,undefined
        -fno-omit-frame-pointer
    )
endif()

add_executable(sol src/main.c)
target_link_libraries(sol PRIVATE sol_compiler)

if(SOL_BUILD_FUZZERS)
    target_link_options(sol PRIVATE -fsanitize=address,undefined)
endif()

if(MSVC)
    target_compile_options(sol_compiler PRIVATE /std:c17 /W4)
    target_compile_options(sol PRIVATE /std:c17 /W4)
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(sol_compiler PRIVATE
        -std=c17
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
    )
    target_compile_options(sol PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
else()
    set_target_properties(sol_compiler sol PROPERTIES
        C_STANDARD 17
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )
endif()

if(SOL_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(sol_compiler PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(sol_compiler PRIVATE -fsanitize=address,undefined)
    target_compile_options(sol PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(sol PRIVATE -fsanitize=address,undefined)
endif()

if(SOL_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

include(CTest)
enable_testing()

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(sol_frontend_tests tests/frontend_tests.c)
    target_link_libraries(sol_frontend_tests PRIVATE sol_compiler)
    add_executable(sol_compilation_tests tests/compilation_tests.c)
    target_link_libraries(sol_compilation_tests PRIVATE sol_compiler)
    target_compile_definitions(sol_compilation_tests PRIVATE
        SOL_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_executable(sol_formatter_tests tests/formatter_tests.c)
    target_link_libraries(sol_formatter_tests PRIVATE sol_compiler)
    add_executable(sol_sema_tests tests/sema_tests.c)
    target_link_libraries(sol_sema_tests PRIVATE sol_compiler)
    add_executable(sol_typecheck_tests tests/typecheck_tests.c)
    target_link_libraries(sol_typecheck_tests PRIVATE sol_compiler)
    add_executable(sol_effectcheck_tests tests/effectcheck_tests.c)
    target_link_libraries(sol_effectcheck_tests PRIVATE sol_compiler)
    add_executable(sol_contract_tests tests/contract_tests.c)
    target_link_libraries(sol_contract_tests PRIVATE sol_compiler)
    add_executable(sol_package_tests tests/package_tests.c)
    target_link_libraries(sol_package_tests PRIVATE sol_compiler)
    target_compile_definitions(sol_package_tests PRIVATE
        SOL_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_executable(sol_ir_tests tests/ir_tests.c)
    target_link_libraries(sol_ir_tests PRIVATE sol_compiler)
    add_executable(sol_interpreter_tests tests/interpreter_tests.c)
    target_link_libraries(sol_interpreter_tests PRIVATE sol_compiler)
    add_executable(sol_inspection_tests tests/inspection_tests.c)
    target_link_libraries(sol_inspection_tests PRIVATE sol_compiler)
    add_executable(sol_test_declaration_tests tests/test_declaration_tests.c)
    target_link_libraries(sol_test_declaration_tests PRIVATE sol_compiler)
    if(UNIX)
        add_executable(sol_cli_io_helper tests/cli_io_helper.c)
        set(SOL_CLI_IO_HELPER $<TARGET_FILE:sol_cli_io_helper>)
    else()
        set(SOL_CLI_IO_HELPER "")
    endif()
    target_compile_definitions(sol_interpreter_tests PRIVATE
        SOL_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_definitions(sol_ir_tests PRIVATE
        SOL_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    if(MSVC)
        target_compile_options(sol_frontend_tests PRIVATE /std:c17 /W4)
        target_compile_options(sol_compilation_tests PRIVATE /std:c17 /W4)
        target_compile_options(sol_formatter_tests PRIVATE /std:c17 /W4)
        target_compile_options(sol_sema_tests PRIVATE /std:c17 /W4)
        target_compile_options(sol_typecheck_tests PRIVATE /std:c17 /W4)
        target_compile_options(sol_effectcheck_tests PRIVATE /std:c17 /W4)
        target_compile_options(sol_contract_tests PRIVATE /std:c17 /W4)
        target_compile_options(sol_package_tests PRIVATE /std:c17 /W4)
        target_compile_options(sol_ir_tests PRIVATE /std:c17 /W4)
        target_compile_options(sol_interpreter_tests PRIVATE /std:c17 /W4)
        target_compile_options(sol_inspection_tests PRIVATE /std:c17 /W4)
        target_compile_options(sol_test_declaration_tests PRIVATE /std:c17 /W4)
        if(UNIX)
            target_compile_options(sol_cli_io_helper PRIVATE /std:c17 /W4)
        endif()
    elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(sol_frontend_tests PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
        target_compile_options(sol_compilation_tests PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
        target_compile_options(sol_formatter_tests PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
        target_compile_options(sol_sema_tests PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
        target_compile_options(sol_typecheck_tests PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
        target_compile_options(sol_effectcheck_tests PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
        target_compile_options(sol_contract_tests PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
        target_compile_options(sol_package_tests PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
        target_compile_options(sol_ir_tests PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
        target_compile_options(sol_interpreter_tests PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
        target_compile_options(sol_inspection_tests PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
        target_compile_options(sol_test_declaration_tests PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
        if(UNIX)
            target_compile_options(sol_cli_io_helper PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
        endif()
    else()
        set_target_properties(sol_frontend_tests sol_compilation_tests sol_formatter_tests sol_sema_tests sol_typecheck_tests sol_effectcheck_tests sol_contract_tests sol_package_tests sol_ir_tests sol_interpreter_tests sol_inspection_tests sol_test_declaration_tests PROPERTIES
            C_STANDARD 17
            C_STANDARD_REQUIRED ON
            C_EXTENSIONS OFF
        )
    endif()

    if(SOL_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(sol_frontend_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(sol_frontend_tests PRIVATE -fsanitize=address,undefined)
        target_compile_options(sol_compilation_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(sol_compilation_tests PRIVATE -fsanitize=address,undefined)
        target_compile_options(sol_formatter_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(sol_formatter_tests PRIVATE -fsanitize=address,undefined)
        target_compile_options(sol_sema_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(sol_sema_tests PRIVATE -fsanitize=address,undefined)
        target_compile_options(sol_typecheck_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(sol_typecheck_tests PRIVATE -fsanitize=address,undefined)
        target_compile_options(sol_effectcheck_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(sol_effectcheck_tests PRIVATE -fsanitize=address,undefined)
        target_compile_options(sol_contract_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(sol_contract_tests PRIVATE -fsanitize=address,undefined)
        target_compile_options(sol_package_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(sol_package_tests PRIVATE -fsanitize=address,undefined)
        target_compile_options(sol_ir_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(sol_ir_tests PRIVATE -fsanitize=address,undefined)
        target_compile_options(sol_interpreter_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(sol_interpreter_tests PRIVATE -fsanitize=address,undefined)
        target_compile_options(sol_inspection_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(sol_inspection_tests PRIVATE -fsanitize=address,undefined)
        target_compile_options(sol_test_declaration_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(sol_test_declaration_tests PRIVATE -fsanitize=address,undefined)
    endif()

    add_test(NAME frontend COMMAND sol_frontend_tests)
    add_test(NAME compilation COMMAND sol_compilation_tests)
    add_test(NAME formatter COMMAND sol_formatter_tests)
    add_test(NAME sema COMMAND sol_sema_tests)
    add_test(NAME typecheck COMMAND sol_typecheck_tests)
    add_test(NAME effectcheck COMMAND sol_effectcheck_tests)
    add_test(NAME contract COMMAND sol_contract_tests)
    add_test(NAME package COMMAND sol_package_tests)
    add_test(NAME ir COMMAND sol_ir_tests)
    add_test(NAME interpreter COMMAND sol_interpreter_tests)
    add_test(NAME inspection COMMAND sol_inspection_tests)
    add_test(NAME test_declarations COMMAND sol_test_declaration_tests)
    add_test(NAME cli_valid COMMAND sol check ${CMAKE_CURRENT_SOURCE_DIR}/tests/valid.sol)
    add_test(NAME cli_mutable COMMAND sol check ${CMAKE_CURRENT_SOURCE_DIR}/tests/mutable.sol)
    add_test(
        NAME cli_package_valid
        COMMAND sol check ${CMAKE_CURRENT_SOURCE_DIR}/tests/packages/valid
    )
    add_test(
        NAME cli_package_mutable
        COMMAND sol check ${CMAKE_CURRENT_SOURCE_DIR}/tests/packages/mutable
    )
    add_test(
        NAME cli_package_private_import
        COMMAND ${CMAKE_COMMAND}
            -DSOL_EXECUTABLE=$<TARGET_FILE:sol>
            -DSOL_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/packages/private_import
            -DEXPECTED_CODE=SOL-RESOLVE-010
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_invalid.cmake
    )
    add_test(
        NAME cli_package_missing_import
        COMMAND ${CMAKE_COMMAND}
            -DSOL_EXECUTABLE=$<TARGET_FILE:sol>
            -DSOL_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/packages/missing_import
            -DEXPECTED_CODE=SOL-RESOLVE-008
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_invalid.cmake
    )
    add_test(
        NAME cli_package_duplicate
        COMMAND ${CMAKE_COMMAND}
            -DSOL_EXECUTABLE=$<TARGET_FILE:sol>
            -DSOL_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/packages/duplicate
            -DEXPECTED_CODE=SOL-RESOLVE-001
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_invalid.cmake
    )
    add_test(
        NAME cli_invalid
        COMMAND ${CMAKE_COMMAND}
            -DSOL_EXECUTABLE=$<TARGET_FILE:sol>
            -DSOL_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid.sol
            -DEXPECTED_CODE=SOL-PARSE-004
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_invalid.cmake
    )
    add_test(
        NAME cli_unresolved
        COMMAND ${CMAKE_COMMAND}
            -DSOL_EXECUTABLE=$<TARGET_FILE:sol>
            -DSOL_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/unresolved.sol
            -DEXPECTED_CODE=SOL-RESOLVE-002
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_invalid.cmake
    )
    add_test(
        NAME cli_type_error
        COMMAND ${CMAKE_COMMAND}
            -DSOL_EXECUTABLE=$<TARGET_FILE:sol>
            -DSOL_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid_type.sol
            -DEXPECTED_CODE=SOL-TYPE-002
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_invalid.cmake
    )
    add_test(
        NAME cli_effect_error
        COMMAND ${CMAKE_COMMAND}
            -DSOL_EXECUTABLE=$<TARGET_FILE:sol>
            -DSOL_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid_effect.sol
            -DEXPECTED_CODE=SOL-EFFECT-002
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_invalid.cmake
    )
    add_test(
        NAME cli_authority_error
        COMMAND ${CMAKE_COMMAND}
            -DSOL_EXECUTABLE=$<TARGET_FILE:sol>
            -DSOL_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid_authority.sol
            -DEXPECTED_CODE=SOL-EFFECT-010
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_invalid.cmake
    )
    add_test(
        NAME cli_ownership_error
        COMMAND ${CMAKE_COMMAND}
            -DSOL_EXECUTABLE=$<TARGET_FILE:sol>
            -DSOL_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid_ownership.sol
            -DEXPECTED_CODE=SOL-OWNERSHIP-001
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_invalid.cmake
    )
    add_test(
        NAME cli_borrow_error
        COMMAND ${CMAKE_COMMAND}
            -DSOL_EXECUTABLE=$<TARGET_FILE:sol>
            -DSOL_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid_borrow.sol
            -DEXPECTED_CODE=SOL-OWNERSHIP-002
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_invalid.cmake
    )
    add_test(
        NAME cli_region_error
        COMMAND ${CMAKE_COMMAND}
            -DSOL_EXECUTABLE=$<TARGET_FILE:sol>
            -DSOL_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid_region.sol
            -DEXPECTED_CODE=SOL-REGION-001
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_invalid.cmake
    )

    foreach(formatter_cli_case stdout check rewrite malformed_directory symlink exclusive_options)
        add_test(
            NAME formatter_cli_${formatter_cli_case}
            COMMAND ${CMAKE_COMMAND}
                -DCASE=${formatter_cli_case}
                -DSOL_EXECUTABLE=$<TARGET_FILE:sol>
                -DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/formatter_cli
                -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/tests/formatter_cli/${formatter_cli_case}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/formatter_cli/test_formatter_cli.cmake
        )
        set_tests_properties(formatter_cli_${formatter_cli_case} PROPERTIES LABELS formatter_cli)
    endforeach()
    add_test(
        NAME test_cli
        COMMAND ${CMAKE_COMMAND}
            -DSOL_EXECUTABLE=$<TARGET_FILE:sol>
            -DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli
            -DRUN_FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_cli/console.sol
            -DRUN_NONZERO_FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_cli/exit_42.sol
            -DCLI_IO_HELPER=${SOL_CLI_IO_HELPER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli/test_test_cli.cmake
    )
    set_tests_properties(test_cli PROPERTIES LABELS test_cli)
    add_test(
        NAME run_cli
        COMMAND ${CMAKE_COMMAND}
            -DSOL_EXECUTABLE=$<TARGET_FILE:sol>
            -DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/run_cli
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_cli/test_run_cli.cmake
    )
    set_tests_properties(run_cli PROPERTIES LABELS run_cli)
    add_test(
        NAME effects_cli
        COMMAND ${CMAKE_COMMAND}
            -DSOL_EXECUTABLE=$<TARGET_FILE:sol>
            -DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/valid.sol
            -DPACKAGE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/packages/valid
            -DSUBSTITUTION_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/effects_cli/substitution.sol
            -DINVALID_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid.sol
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/effects_cli/test_effects_cli.cmake
    )
    add_test(
        NAME inspection_cli
        COMMAND ${CMAKE_COMMAND}
            -DSOL_EXECUTABLE=$<TARGET_FILE:sol>
            -DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/valid.sol
            -DPACKAGE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/packages/valid
            -DGENERIC_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/inspection_cli/generic.sol
            -DTUPLE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/inspection_cli/tuple.sol
            -DMUTABLE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/mutable.sol
            -DINVALID_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid.sol
            -DCLI_IO_HELPER=${SOL_CLI_IO_HELPER}
            -DPYTHON_EXECUTABLE=${Python3_EXECUTABLE}
            -DSCHEMA_FILE=${CMAKE_CURRENT_SOURCE_DIR}/schemas/sol-inspection-3.schema.json
            -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/tests/inspection_cli
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/inspection_cli/test_inspection_cli.cmake
    )
    set_tests_properties(inspection_cli PROPERTIES LABELS inspection_cli)
endif()
