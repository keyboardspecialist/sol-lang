#ifndef SOL_SCALAR_TEST_PROGRESS_H
#define SOL_SCALAR_TEST_PROGRESS_H
#include "test_progress.h"
SOL_TEST_VALUE_HOOK(SolWasmScalarResult, sol_wasm_scalar_build_scalar)
SOL_TEST_VALUE_HOOK(SolWasmScalarResult, sol_wasm_scalar_validate_scalar)
SOL_TEST_VALUE_HOOK(SolWasmScalarResult, sol_wasm_scalar_test_call_catalog)
#define sol_wasm_scalar_build_scalar(...) SOL_TEST_VALUE(sol_wasm_scalar_build_scalar, __VA_ARGS__)
#define sol_wasm_scalar_validate_scalar(...) SOL_TEST_VALUE(sol_wasm_scalar_validate_scalar, __VA_ARGS__)
#define sol_wasm_scalar_test_call_catalog(...) SOL_TEST_VALUE(sol_wasm_scalar_test_call_catalog, __VA_ARGS__)
static inline void sol_test_scalar_fault(size_t attempt) {
    sol_test_progress_hook("scalar-allocation", attempt);
    sol_wasm_scalar_test_fail_allocation_after(attempt);
}
#define sol_wasm_scalar_test_fail_allocation_after sol_test_scalar_fault
#endif
