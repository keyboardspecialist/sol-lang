cmake_minimum_required(VERSION 3.17)

project(sol VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_EXTENSIONS OFF)

option(SOL_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

add_library(sol_compiler STATIC
    src/diagnostic.c
    src/hir.c
    src/lexer.c
    src/parser.c
    src/source.c
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
    if(MSVC)
        target_compile_options(sol_frontend_tests PRIVATE /std:c17 /W4)
        target_compile_options(sol_sema_tests PRIVATE /std:c17 /W4)
    elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(sol_frontend_tests PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
        target_compile_options(sol_sema_tests PRIVATE -std=c17 -Wall -Wextra -Wpedantic)
    else()
        set_target_properties(sol_frontend_tests sol_sema_tests PROPERTIES
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
    endif()

    add_test(NAME frontend COMMAND sol_frontend_tests)
    add_test(NAME sema COMMAND sol_sema_tests)
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
endif()
