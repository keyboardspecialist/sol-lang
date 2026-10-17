#ifndef SOL_NATIVE_TEST_PROGRESS_H
#define SOL_NATIVE_TEST_PROGRESS_H

/* Test-translation-unit adapters only. No production object sees these macros.
 * Every original operation/result/short circuit is retained, evaluated once.
 * Type-preserving adapters report completion AFTER the operation returns. */
#ifndef SOL_MIR_PLAN_TEST_HOOKS
#define SOL_MIR_PLAN_TEST_HOOKS 1
#endif
#include "test_progress.h"
#include "sol/mir_runtime_lowered_program.h"
#include "sol/package.h"

SOL_TEST_VALUE_HOOK(bool, sol_hir_lower_scoped)
SOL_TEST_VALUE_HOOK(bool, sol_type_check)
SOL_TEST_VALUE_HOOK(bool, sol_effect_check)
SOL_TEST_VALUE_HOOK(bool, sol_contract_lower)
SOL_TEST_VALUE_HOOK(bool, sol_ir_lower_scoped)
#define sol_hir_lower_scoped(...) SOL_TEST_VALUE(sol_hir_lower_scoped, __VA_ARGS__)
#define sol_type_check(...) SOL_TEST_VALUE(sol_type_check, __VA_ARGS__)
#define sol_effect_check(...) SOL_TEST_VALUE(sol_effect_check, __VA_ARGS__)
#define sol_contract_lower(...) SOL_TEST_VALUE(sol_contract_lower, __VA_ARGS__)
#define sol_ir_lower_scoped(...) SOL_TEST_VALUE(sol_ir_lower_scoped, __VA_ARGS__)

SOL_TEST_VALUE_HOOK(SolMirConcreteBuildOutcome, sol_mir_concrete_program_build)
SOL_TEST_VALUE_HOOK(SolMirRuntimeConventionsBuildOutcome, sol_mir_runtime_conventions_build)
SOL_TEST_VALUE_HOOK(SolMirRuntimeValuesBuildOutcome, sol_mir_runtime_values_build)
SOL_TEST_VALUE_HOOK(SolMirRuntimeCleanupBuildOutcome, sol_mir_runtime_cleanup_build)
SOL_TEST_VALUE_HOOK(SolMirRuntimeHostAbiBuildOutcome, sol_mir_runtime_host_abi_build)
SOL_TEST_VALUE_HOOK(SolMirRuntimeHandlerAbiBuildOutcome, sol_mir_runtime_handler_abi_build)
SOL_TEST_VALUE_HOOK(SolMirRuntimeLoweredProgramBuildOutcome, sol_mir_runtime_lowered_program_build)
SOL_TEST_VALUE_HOOK(bool, sol_mir_runtime_conventions_validate)
SOL_TEST_VALUE_HOOK(bool, sol_mir_runtime_values_validate)
SOL_TEST_VALUE_HOOK(bool, sol_mir_runtime_cleanup_validate)
SOL_TEST_VALUE_HOOK(bool, sol_mir_runtime_host_abi_validate)
SOL_TEST_VALUE_HOOK(bool, sol_mir_runtime_handler_abi_validate)
SOL_TEST_VALUE_HOOK(bool, sol_mir_runtime_lowered_program_validate)
SOL_TEST_VALUE_HOOK(bool, sol_mir_runtime_conventions_render)
SOL_TEST_VALUE_HOOK(bool, sol_mir_runtime_values_render)
SOL_TEST_VALUE_HOOK(bool, sol_mir_runtime_cleanup_render)
SOL_TEST_VALUE_HOOK(bool, sol_mir_runtime_host_abi_render)
SOL_TEST_VALUE_HOOK(bool, sol_mir_runtime_handler_abi_render)
SOL_TEST_VALUE_HOOK(bool, sol_mir_runtime_lowered_program_render)
SOL_TEST_VALUE_HOOK(bool, sol_mir_runtime_cleanup_test_trace)
SOL_TEST_VALUE_HOOK(bool, sol_mir_runtime_cleanup_test_precedence)
SOL_TEST_VALUE_HOOK(bool, sol_mir_runtime_cleanup_test_precedence_attempted_later)
SOL_TEST_VALUE_HOOK(bool, sol_mir_runtime_cleanup_test_precedence_contract_later)

#define sol_mir_concrete_program_build(...) SOL_TEST_VALUE(sol_mir_concrete_program_build, __VA_ARGS__)
#define sol_mir_runtime_conventions_build(...) SOL_TEST_VALUE(sol_mir_runtime_conventions_build, __VA_ARGS__)
#define sol_mir_runtime_values_build(...) SOL_TEST_VALUE(sol_mir_runtime_values_build, __VA_ARGS__)
#define sol_mir_runtime_cleanup_build(...) SOL_TEST_VALUE(sol_mir_runtime_cleanup_build, __VA_ARGS__)
#define sol_mir_runtime_host_abi_build(...) SOL_TEST_VALUE(sol_mir_runtime_host_abi_build, __VA_ARGS__)
#define sol_mir_runtime_handler_abi_build(...) SOL_TEST_VALUE(sol_mir_runtime_handler_abi_build, __VA_ARGS__)
#define sol_mir_runtime_lowered_program_build(...) SOL_TEST_VALUE(sol_mir_runtime_lowered_program_build, __VA_ARGS__)
#define sol_mir_runtime_conventions_validate(...) SOL_TEST_VALUE(sol_mir_runtime_conventions_validate, __VA_ARGS__)
#define sol_mir_runtime_values_validate(...) SOL_TEST_VALUE(sol_mir_runtime_values_validate, __VA_ARGS__)
#define sol_mir_runtime_cleanup_validate(...) SOL_TEST_VALUE(sol_mir_runtime_cleanup_validate, __VA_ARGS__)
#define sol_mir_runtime_host_abi_validate(...) SOL_TEST_VALUE(sol_mir_runtime_host_abi_validate, __VA_ARGS__)
#define sol_mir_runtime_handler_abi_validate(...) SOL_TEST_VALUE(sol_mir_runtime_handler_abi_validate, __VA_ARGS__)
#define sol_mir_runtime_lowered_program_validate(...) SOL_TEST_VALUE(sol_mir_runtime_lowered_program_validate, __VA_ARGS__)
#define sol_mir_runtime_conventions_render(...) SOL_TEST_VALUE(sol_mir_runtime_conventions_render, __VA_ARGS__)
#define sol_mir_runtime_values_render(...) SOL_TEST_VALUE(sol_mir_runtime_values_render, __VA_ARGS__)
#define sol_mir_runtime_cleanup_render(...) SOL_TEST_VALUE(sol_mir_runtime_cleanup_render, __VA_ARGS__)
#define sol_mir_runtime_host_abi_render(...) SOL_TEST_VALUE(sol_mir_runtime_host_abi_render, __VA_ARGS__)
#define sol_mir_runtime_handler_abi_render(...) SOL_TEST_VALUE(sol_mir_runtime_handler_abi_render, __VA_ARGS__)
#define sol_mir_runtime_lowered_program_render(...) SOL_TEST_VALUE(sol_mir_runtime_lowered_program_render, __VA_ARGS__)
#define sol_mir_runtime_cleanup_test_trace(...) SOL_TEST_VALUE(sol_mir_runtime_cleanup_test_trace, __VA_ARGS__)
#define sol_mir_runtime_cleanup_test_precedence(...) SOL_TEST_VALUE(sol_mir_runtime_cleanup_test_precedence, __VA_ARGS__)
#define sol_mir_runtime_cleanup_test_precedence_attempted_later(...) SOL_TEST_VALUE(sol_mir_runtime_cleanup_test_precedence_attempted_later, __VA_ARGS__)
#define sol_mir_runtime_cleanup_test_precedence_contract_later(...) SOL_TEST_VALUE(sol_mir_runtime_cleanup_test_precedence_contract_later, __VA_ARGS__)

static inline bool sol_test_load_directory(SolPackage *p, const char *directory,
    SolDiagnostics *d, char *error, size_t size) {
    sol_test_progress_fixture(directory);
    sol_test_progress_operation_begin("package-load");
    bool result = sol_package_load_directory(p, directory, d, error, size);
    sol_test_progress_operation_end();
    return result;
}
#define sol_package_load_directory sol_test_load_directory

/* These void operations are still expressions and execute exactly once. */
#define SOL_TEST_TEARDOWN(name, ...) \
    (sol_test_progress_operation_at(__func__, "teardown:" #name), \
     name(__VA_ARGS__), sol_test_progress_operation_end())
#define sol_mir_runtime_lowered_program_free(...) SOL_TEST_TEARDOWN(sol_mir_runtime_lowered_program_free, __VA_ARGS__)
#define sol_mir_runtime_cleanup_free(...) SOL_TEST_TEARDOWN(sol_mir_runtime_cleanup_free, __VA_ARGS__)
#define sol_mir_concrete_program_free(...) SOL_TEST_TEARDOWN(sol_mir_concrete_program_free, __VA_ARGS__)

#define SOL_TEST_ARM_HOOK(type, name) \
    static inline void sol_test_arm_##name(type value) { \
        sol_test_progress_hook(#name, (size_t)value); name(value); \
    }
SOL_TEST_ARM_HOOK(size_t, sol_mir_runtime_cleanup_test_force_build_scratch_failure_attempt)
SOL_TEST_ARM_HOOK(size_t, sol_mir_runtime_cleanup_test_force_persistent_allocation_failure_attempt)
SOL_TEST_ARM_HOOK(size_t, sol_mir_runtime_cleanup_test_force_validation_scratch_failure_attempt)
SOL_TEST_ARM_HOOK(size_t, sol_mir_runtime_lowered_program_test_fail_build_scratch_allocation_after)
SOL_TEST_ARM_HOOK(size_t, sol_mir_runtime_lowered_program_test_fail_persistent_allocation_after)
#define sol_mir_runtime_cleanup_test_force_build_scratch_failure_attempt sol_test_arm_sol_mir_runtime_cleanup_test_force_build_scratch_failure_attempt
#define sol_mir_runtime_cleanup_test_force_persistent_allocation_failure_attempt sol_test_arm_sol_mir_runtime_cleanup_test_force_persistent_allocation_failure_attempt
#define sol_mir_runtime_cleanup_test_force_validation_scratch_failure_attempt sol_test_arm_sol_mir_runtime_cleanup_test_force_validation_scratch_failure_attempt
#define sol_mir_runtime_lowered_program_test_fail_build_scratch_allocation_after sol_test_arm_sol_mir_runtime_lowered_program_test_fail_build_scratch_allocation_after
#define sol_mir_runtime_lowered_program_test_fail_persistent_allocation_after sol_test_arm_sol_mir_runtime_lowered_program_test_fail_persistent_allocation_after

#endif
