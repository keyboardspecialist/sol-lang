if(NOT DEFINED SOL_SOURCE_DIR OR NOT DEFINED SOL_CMAKE_GENERATOR
    OR NOT DEFINED SOL_SHARED_LIBRARY_PREFIX
    OR NOT DEFINED SOL_SHARED_LIBRARY_SUFFIX OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "Wasm backend rejection test is missing its configured inputs")
endif()

function(write_wasm_opt root version)
    file(MAKE_DIRECTORY "${root}/bin" "${root}/include" "${root}/lib")
    file(WRITE "${root}/include/binaryen-c.h" "/* controlled test fixture */\n")
    file(WRITE "${root}/lib/${SOL_SHARED_LIBRARY_PREFIX}binaryen${SOL_SHARED_LIBRARY_SUFFIX}" "")
    file(WRITE "${root}/bin/wasm-opt"
        "#!/bin/sh\nprintf 'wasm-opt version ${version}\\n'\n")
    execute_process(COMMAND /bin/chmod 755 "${root}/bin/wasm-opt"
        RESULT_VARIABLE chmod_result)
    if(NOT chmod_result EQUAL 0)
        message(FATAL_ERROR "could not make deterministic fake wasm-opt executable")
    endif()
endfunction()

function(write_wasmtime_header root version major minor patch)
    file(MAKE_DIRECTORY "${root}/include" "${root}/lib")
    file(WRITE "${root}/include/wasmtime.h"
        "#define WASMTIME_VERSION \"${version}\"\n"
        "#define WASMTIME_VERSION_MAJOR ${major}\n"
        "#define WASMTIME_VERSION_MINOR ${minor}\n"
        "#define WASMTIME_VERSION_PATCH ${patch}\n")
    file(WRITE "${root}/include/wasm.h" "/* controlled test fixture */\n")
    file(WRITE "${root}/lib/${SOL_SHARED_LIBRARY_PREFIX}wasmtime${SOL_SHARED_LIBRARY_SUFFIX}" "")
endfunction()

function(expect_rejection name binaryen_root wasmtime_root required_text)
    set(build_dir "${TEST_DIR}/${name}-build")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${SOL_SOURCE_DIR}" -B "${build_dir}"
            -G "${SOL_CMAKE_GENERATOR}"
            -DSOL_ENABLE_WASM_BACKEND=ON
            -DSOL_BINARYEN_ROOT=${binaryen_root}
            -DSOL_WASMTIME_ROOT=${wasmtime_root}
            ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
    )
    if(result EQUAL 0)
        message(FATAL_ERROR "${name}: configuration unexpectedly succeeded")
    endif()
    set(combined "${output}\n${errors}")
    string(FIND "${combined}" "${required_text}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "${name}: missing required diagnostic '${required_text}':\n${combined}")
    endif()
endfunction()

file(REMOVE_RECURSE "${TEST_DIR}")
file(MAKE_DIRECTORY "${TEST_DIR}")

set(binaryen_good "${TEST_DIR}/binaryen-good")
set(binaryen_wrong "${TEST_DIR}/binaryen-wrong")
set(wasmtime_good "${TEST_DIR}/wasmtime-good")
set(wasmtime_wrong "${TEST_DIR}/wasmtime-wrong")
write_wasm_opt("${binaryen_good}" 129)
write_wasm_opt("${binaryen_wrong}" 128)
write_wasmtime_header("${wasmtime_good}" "49.0.1" 49 0 1)
write_wasmtime_header("${wasmtime_wrong}" "49.0.0" 49 0 0)

expect_rejection(missing_binaryen "${TEST_DIR}/missing-binaryen" "${wasmtime_good}"
    "SOL_BINARYEN_ROOT")
expect_rejection(missing_wasmtime "${binaryen_good}" "${TEST_DIR}/missing-wasmtime"
    "SOL_WASMTIME_ROOT")
expect_rejection(wrong_binaryen "${binaryen_wrong}" "${wasmtime_good}"
    "requires Binaryen 129")
expect_rejection(wrong_wasmtime "${binaryen_good}" "${wasmtime_wrong}"
    "requires Wasmtime 49.0.1")

# A root symlink is permitted, but an artifact symlink escaping that root is
# not: containment is checked after canonicalizing both paths.
set(symlink_binaryen "${TEST_DIR}/symlink-binaryen")
set(symlink_outside "${TEST_DIR}/symlink-outside")
write_wasm_opt("${symlink_binaryen}" 129)
file(REMOVE_RECURSE "${symlink_binaryen}/include")
file(MAKE_DIRECTORY "${symlink_outside}/include")
file(WRITE "${symlink_outside}/include/binaryen-c.h" "/* outside root */\n")
file(CREATE_LINK "${symlink_outside}/include" "${symlink_binaryen}/include" SYMBOLIC)
expect_rejection(symlink_escape "${symlink_binaryen}" "${wasmtime_good}"
    "rejected Binaryen include directory")

# Regression: all discovery variables are supplied from an outside root, but
# the selected root's own wrong wasm-opt must win and reject at version check.
set(cache_selected "${TEST_DIR}/cache-selected")
set(cache_outside "${TEST_DIR}/cache-outside")
write_wasm_opt("${cache_selected}" 128)
write_wasm_opt("${cache_outside}" 129)
expect_rejection(cache_root_bypass "${cache_selected}" "${wasmtime_good}"
    "requires Binaryen 129"
    -DSOL_BINARYEN_INCLUDE_DIR:PATH=${cache_outside}/include
    -DSOL_BINARYEN_LIBRARY:FILEPATH=${cache_outside}/lib/${SOL_SHARED_LIBRARY_PREFIX}binaryen${SOL_SHARED_LIBRARY_SUFFIX}
    -DSOL_BINARYEN_WASM_OPT:FILEPATH=${cache_outside}/bin/wasm-opt
    -DSOL_WASMTIME_INCLUDE_DIR:PATH=${wasmtime_wrong}/include
    -DSOL_WASMTIME_LIBRARY:FILEPATH=${wasmtime_wrong}/lib/${SOL_SHARED_LIBRARY_PREFIX}wasmtime${SOL_SHARED_LIBRARY_SUFFIX})
