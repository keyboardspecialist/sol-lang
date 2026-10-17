#ifndef SOL_REPRESENTED_TEST_PROGRESS_H
#define SOL_REPRESENTED_TEST_PROGRESS_H
#include "test_progress.h"
SOL_TEST_VALUE_HOOK(SolWasmRepresentedResult, sol_wasm_represented_build)
SOL_TEST_VALUE_HOOK(SolWasmRepresentedResult, sol_wasm_represented_validate)
SOL_TEST_VALUE_HOOK(SolWasmRepresentedResult, sol_wasm_represented_test_call_catalog)
#define sol_wasm_represented_build(...) SOL_TEST_VALUE(sol_wasm_represented_build, __VA_ARGS__)
#define sol_wasm_represented_validate(...) SOL_TEST_VALUE(sol_wasm_represented_validate, __VA_ARGS__)
#define sol_wasm_represented_test_call_catalog(...) SOL_TEST_VALUE(sol_wasm_represented_test_call_catalog, __VA_ARGS__)
static inline void sol_test_represented_fault(size_t attempt) {
    sol_test_progress_hook("represented-allocation", attempt);
    sol_wasm_represented_test_fail_allocation_after(attempt);
}
#define sol_wasm_represented_test_fail_allocation_after sol_test_represented_fault
#endif
