# The Wasm backend is deliberately opt-in.  Do not add fallback search paths:
# a configured backend must be the exact pair selected by its roots.
if(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR
        "SOL_ENABLE_WASM_BACKEND=ON does not support CMAKE_CROSSCOMPILING; "
        "the Binaryen/Wasmtime compatibility probe must run at configure time.")
endif()

foreach(sol_wasm_root SOL_BINARYEN_ROOT SOL_WASMTIME_ROOT)
    if(NOT ${sol_wasm_root} OR NOT IS_ABSOLUTE "${${sol_wasm_root}}"
        OR NOT IS_DIRECTORY "${${sol_wasm_root}}")
        message(FATAL_ERROR
            "SOL_ENABLE_WASM_BACKEND=ON requires ${sol_wasm_root} to name an "
            "existing absolute installation root (selected value: '${${sol_wasm_root}}').")
    endif()
endforeach()

# These discovery names are intentionally not user-configurable cache inputs.
# Clear them on every configure so a prior configure cannot mix dependency
# roots. A selected root may itself be a symlink, but every discovered item is
# checked below after resolving symlinks and must remain within that root.
foreach(sol_wasm_discovery
    SOL_BINARYEN_INCLUDE_DIR SOL_BINARYEN_LIBRARY SOL_BINARYEN_WASM_OPT
    SOL_WASMTIME_INCLUDE_DIR SOL_WASMTIME_LIBRARY)
    unset(${sol_wasm_discovery} CACHE)
    unset(${sol_wasm_discovery})
endforeach()
get_filename_component(SOL_BINARYEN_ROOT_REAL "${SOL_BINARYEN_ROOT}" REALPATH)
get_filename_component(SOL_WASMTIME_ROOT_REAL "${SOL_WASMTIME_ROOT}" REALPATH)

find_path(SOL_BINARYEN_INCLUDE_DIR binaryen-c.h
    PATHS "${SOL_BINARYEN_ROOT}/include" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_library(SOL_BINARYEN_LIBRARY NAMES binaryen
    PATHS "${SOL_BINARYEN_ROOT}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_program(SOL_BINARYEN_WASM_OPT NAMES wasm-opt
    PATHS "${SOL_BINARYEN_ROOT}/bin" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_path(SOL_WASMTIME_INCLUDE_DIR wasmtime.h
    PATHS "${SOL_WASMTIME_ROOT}/include" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_library(SOL_WASMTIME_LIBRARY NAMES wasmtime
    PATHS "${SOL_WASMTIME_ROOT}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

foreach(sol_wasm_required
    SOL_BINARYEN_INCLUDE_DIR SOL_BINARYEN_LIBRARY SOL_BINARYEN_WASM_OPT
    SOL_WASMTIME_INCLUDE_DIR SOL_WASMTIME_LIBRARY)
    if(NOT ${sol_wasm_required})
        message(FATAL_ERROR
            "SOL_ENABLE_WASM_BACKEND=ON could not find ${sol_wasm_required} "
            "under the selected roots: Binaryen='${SOL_BINARYEN_ROOT}', "
            "Wasmtime='${SOL_WASMTIME_ROOT}'. Searches use NO_DEFAULT_PATH.")
    endif()
endforeach()
if(NOT EXISTS "${SOL_WASMTIME_INCLUDE_DIR}/wasm.h")
    message(FATAL_ERROR
        "SOL_ENABLE_WASM_BACKEND=ON requires Wasmtime's wasm.h under the "
        "selected include root '${SOL_WASMTIME_INCLUDE_DIR}'.")
endif()

function(sol_wasm_require_within_root item root label)
    get_filename_component(item_real "${item}" REALPATH)
    file(RELATIVE_PATH item_relative "${root}" "${item_real}")
    if(IS_ABSOLUTE "${item_relative}" OR item_relative MATCHES "^\\.\\.(/|$)")
        message(FATAL_ERROR
            "SOL_ENABLE_WASM_BACKEND=ON rejected ${label}: selected path "
            "'${item}' resolves to '${item_real}', outside selected root "
            "'${root}'. Symlinked roots/items are accepted only when their "
            "resolved paths remain within the selected root.")
    endif()
endfunction()

sol_wasm_require_within_root("${SOL_BINARYEN_INCLUDE_DIR}"
    "${SOL_BINARYEN_ROOT_REAL}" "Binaryen include directory")
sol_wasm_require_within_root("${SOL_BINARYEN_INCLUDE_DIR}/binaryen-c.h"
    "${SOL_BINARYEN_ROOT_REAL}" "Binaryen header")
sol_wasm_require_within_root("${SOL_BINARYEN_LIBRARY}"
    "${SOL_BINARYEN_ROOT_REAL}" "Binaryen library")
sol_wasm_require_within_root("${SOL_BINARYEN_WASM_OPT}"
    "${SOL_BINARYEN_ROOT_REAL}" "Binaryen wasm-opt")
sol_wasm_require_within_root("${SOL_WASMTIME_INCLUDE_DIR}"
    "${SOL_WASMTIME_ROOT_REAL}" "Wasmtime include directory")
sol_wasm_require_within_root("${SOL_WASMTIME_INCLUDE_DIR}/wasmtime.h"
    "${SOL_WASMTIME_ROOT_REAL}" "Wasmtime header")
sol_wasm_require_within_root("${SOL_WASMTIME_INCLUDE_DIR}/wasm.h"
    "${SOL_WASMTIME_ROOT_REAL}" "Wasmtime wasm.h header")
sol_wasm_require_within_root("${SOL_WASMTIME_LIBRARY}"
    "${SOL_WASMTIME_ROOT_REAL}" "Wasmtime library")

execute_process(
    COMMAND "${SOL_BINARYEN_WASM_OPT}" --version
    RESULT_VARIABLE SOL_BINARYEN_WASM_OPT_STATUS
    OUTPUT_VARIABLE SOL_BINARYEN_WASM_OPT_VERSION
    ERROR_VARIABLE SOL_BINARYEN_WASM_OPT_ERROR
)
string(REGEX REPLACE "[\r\n]+$" "" SOL_BINARYEN_WASM_OPT_VERSION
    "${SOL_BINARYEN_WASM_OPT_VERSION}")
if(NOT SOL_BINARYEN_WASM_OPT_STATUS EQUAL 0
    OR NOT SOL_BINARYEN_WASM_OPT_VERSION STREQUAL "wasm-opt version 129")
    message(FATAL_ERROR
        "SOL_ENABLE_WASM_BACKEND=ON requires Binaryen 129. Selected wasm-opt "
        "'${SOL_BINARYEN_WASM_OPT}' returned '${SOL_BINARYEN_WASM_OPT_VERSION}' "
        "(status ${SOL_BINARYEN_WASM_OPT_STATUS}; stderr: ${SOL_BINARYEN_WASM_OPT_ERROR}).")
endif()

file(READ "${SOL_WASMTIME_INCLUDE_DIR}/wasmtime.h" SOL_WASMTIME_HEADER)
string(REGEX MATCH "#define[ \t]+WASMTIME_VERSION[ \t]+\\\"([^\\\"]+)\\\""
    SOL_WASMTIME_VERSION_MATCH "${SOL_WASMTIME_HEADER}")
set(SOL_WASMTIME_HEADER_VERSION "${CMAKE_MATCH_1}")
string(REGEX MATCH "#define[ \t]+WASMTIME_VERSION_MAJOR[ \t]+([0-9]+)"
    SOL_WASMTIME_MAJOR_MATCH "${SOL_WASMTIME_HEADER}")
set(SOL_WASMTIME_HEADER_MAJOR "${CMAKE_MATCH_1}")
string(REGEX MATCH "#define[ \t]+WASMTIME_VERSION_MINOR[ \t]+([0-9]+)"
    SOL_WASMTIME_MINOR_MATCH "${SOL_WASMTIME_HEADER}")
set(SOL_WASMTIME_HEADER_MINOR "${CMAKE_MATCH_1}")
string(REGEX MATCH "#define[ \t]+WASMTIME_VERSION_PATCH[ \t]+([0-9]+)"
    SOL_WASMTIME_PATCH_MATCH "${SOL_WASMTIME_HEADER}")
set(SOL_WASMTIME_HEADER_PATCH "${CMAKE_MATCH_1}")
if(NOT SOL_WASMTIME_HEADER_VERSION STREQUAL "49.0.1"
    OR NOT SOL_WASMTIME_HEADER_MAJOR STREQUAL "49"
    OR NOT SOL_WASMTIME_HEADER_MINOR STREQUAL "0"
    OR NOT SOL_WASMTIME_HEADER_PATCH STREQUAL "1")
    message(FATAL_ERROR
        "SOL_ENABLE_WASM_BACKEND=ON requires Wasmtime 49.0.1. Authoritative "
        "macros in '${SOL_WASMTIME_INCLUDE_DIR}/wasmtime.h' report "
        "version='${SOL_WASMTIME_HEADER_VERSION}', "
        "major='${SOL_WASMTIME_HEADER_MAJOR}', "
        "minor='${SOL_WASMTIME_HEADER_MINOR}', patch='${SOL_WASMTIME_HEADER_PATCH}'.")
endif()

get_filename_component(SOL_BINARYEN_LIBRARY_DIR "${SOL_BINARYEN_LIBRARY}" DIRECTORY)
get_filename_component(SOL_WASMTIME_LIBRARY_DIR "${SOL_WASMTIME_LIBRARY}" DIRECTORY)
set(SOL_WASM_PROBE_LINKER_FLAGS "")
if(APPLE OR UNIX)
    set(SOL_WASM_PROBE_LINKER_FLAGS
        "-Wl,-rpath,${SOL_BINARYEN_LIBRARY_DIR} -Wl,-rpath,${SOL_WASMTIME_LIBRARY_DIR}")
endif()
try_run(SOL_WASM_PROBE_RUN_RESULT SOL_WASM_PROBE_COMPILE_RESULT
    "${CMAKE_BINARY_DIR}/CMakeFiles/sol_wasm_dependency_probe"
    "${CMAKE_CURRENT_LIST_DIR}/SolWasmDependencyProbe.c"
    CMAKE_FLAGS
        "-DCMAKE_C_STANDARD=17"
        "-DCMAKE_EXE_LINKER_FLAGS:STRING=${SOL_WASM_PROBE_LINKER_FLAGS}"
    COMPILE_DEFINITIONS
        "-I${SOL_BINARYEN_INCLUDE_DIR}"
        "-I${SOL_WASMTIME_INCLUDE_DIR}"
    LINK_LIBRARIES "${SOL_BINARYEN_LIBRARY}" "${SOL_WASMTIME_LIBRARY}"
    COMPILE_OUTPUT_VARIABLE SOL_WASM_PROBE_COMPILE_OUTPUT
    RUN_OUTPUT_VARIABLE SOL_WASM_PROBE_RUN_OUTPUT
)
if(NOT SOL_WASM_PROBE_COMPILE_RESULT)
    message(FATAL_ERROR
        "SOL_ENABLE_WASM_BACKEND=ON compatibility probe could not compile/link "
        "against Binaryen '${SOL_BINARYEN_LIBRARY}' and Wasmtime "
        "'${SOL_WASMTIME_LIBRARY}'. Compiler output:\n${SOL_WASM_PROBE_COMPILE_OUTPUT}")
endif()
if(NOT SOL_WASM_PROBE_RUN_RESULT EQUAL 0)
    message(FATAL_ERROR
        "SOL_ENABLE_WASM_BACKEND=ON compatibility probe failed while running "
        "the Binaryen serialize + Wasmtime validate/instantiate/call phase "
        "(exit '${SOL_WASM_PROBE_RUN_RESULT}'). Selected libraries: Binaryen "
        "'${SOL_BINARYEN_LIBRARY}', Wasmtime '${SOL_WASMTIME_LIBRARY}'. "
        "Probe output:\n${SOL_WASM_PROBE_RUN_OUTPUT}")
endif()

add_library(sol_binaryen_dependency UNKNOWN IMPORTED GLOBAL)
set_target_properties(sol_binaryen_dependency PROPERTIES
    IMPORTED_LOCATION "${SOL_BINARYEN_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SOL_BINARYEN_INCLUDE_DIR}")
add_library(sol_wasmtime_dependency UNKNOWN IMPORTED GLOBAL)
set_target_properties(sol_wasmtime_dependency PROPERTIES
    IMPORTED_LOCATION "${SOL_WASMTIME_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SOL_WASMTIME_INCLUDE_DIR}")
