set(SOL_FUZZ_TARGETS
    sol_parser_fuzzer
    sol_package_fuzzer
    sol_ir_fuzzer
)

add_executable(sol_parser_fuzzer parser_fuzzer.c)
add_executable(sol_package_fuzzer package_fuzzer.c)
add_executable(sol_ir_fuzzer ir_fuzzer.c)

foreach(target IN LISTS SOL_FUZZ_TARGETS)
    target_link_libraries(${target} PRIVATE sol_compiler)
    target_compile_options(${target} PRIVATE
        -std=c17
        -Wall
        -Wextra
        -Wpedantic
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
endforeach()
