cmake_minimum_required(VERSION 3.17)

project(sol VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_EXTENSIONS OFF)

option(SOL_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

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
    src/contract.c
    src/diagnostic.c
    src/effectcheck.c
    src/hir.c
    src/lexer.c
    src/parser.c
    src/source.c
    src/syntax.c
    src/typecheck.c
)

target_include_directories(sol_compiler PUBLIC include)

add_executable(sol src/main.c)
target_link_libraries(sol PRIVATE sol_compiler)

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

include(CTest)
enable_testing()

if(BUILD_TESTING)
    add_executable(sol_frontend_tests tests/frontend_tests.c)
    target_link_libraries(sol_frontend_tests PRIVATE sol_compiler)
    add_executable(sol_sema_tests tests/sema_tests.c)
    target_link_libraries(sol_sema_tests PRIVATE sol_compiler)
    add_executable(sol_typecheck_tests tests/typecheck_tests.c)
    target_link_libraries(sol_typecheck_tests PRIVATE sol_compiler)
    add_executable(sol_effectcheck_tests tests/effectcheck_tests.c)
    target_link_libraries(sol_effectcheck_tests PRIVATE sol_compiler)
    add_executable(sol_contract_tests tests/contract_tests.c)
    target_link_libraries(sol_contract_tests PRIVATE sol_compiler)
    if(MSVC)
        target_compile_options(sol_frontend_tests PRIVATE /std:c17 /W4)
        target_compile_options(sol_sema_tests PRIVATE /std:c17 /W4)
        target_compile_options(sol_typecheck_tests PRIVATE /std:c17 /W4)
        target_compile_options(sol_effectcheck_tests PRIVATE /std:c17 /W4)
        target_compile_options(sol_contract_tests PRIVATE /std:c17 /W4)
    elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(sol_frontend_tests PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
        target_compile_options(sol_sema_tests PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
        target_compile_options(sol_typecheck_tests PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
        target_compile_options(sol_effectcheck_tests PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
        target_compile_options(sol_contract_tests PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
    else()
        set_target_properties(sol_frontend_tests sol_sema_tests sol_typecheck_tests sol_effectcheck_tests sol_contract_tests PROPERTIES
            C_STANDARD 17
            C_STANDARD_REQUIRED ON
            C_EXTENSIONS OFF
        )
    endif()

    if(SOL_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(sol_frontend_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(sol_frontend_tests PRIVATE -fsanitize=address,undefined)
        target_compile_options(sol_sema_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(sol_sema_tests PRIVATE -fsanitize=address,undefined)
        target_compile_options(sol_typecheck_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(sol_typecheck_tests PRIVATE -fsanitize=address,undefined)
        target_compile_options(sol_effectcheck_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(sol_effectcheck_tests PRIVATE -fsanitize=address,undefined)
        target_compile_options(sol_contract_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(sol_contract_tests PRIVATE -fsanitize=address,undefined)
    endif()

    add_test(NAME frontend COMMAND sol_frontend_tests)
    add_test(NAME sema COMMAND sol_sema_tests)
    add_test(NAME typecheck COMMAND sol_typecheck_tests)
    add_test(NAME effectcheck COMMAND sol_effectcheck_tests)
    add_test(NAME contract COMMAND sol_contract_tests)
    add_test(NAME cli_valid COMMAND sol check ${CMAKE_CURRENT_SOURCE_DIR}/tests/valid.sol)
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
endif()
