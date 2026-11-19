#ifndef SOL_MIR_RUNTIME_CLEANUP_INTERNAL_H
#define SOL_MIR_RUNTIME_CLEANUP_INTERNAL_H
#include "sol/mir_runtime_cleanup.h"
SolMirRuntimeCleanupBuildOutcome sol_mir_runtime_cleanup_internal_validate(
    const SolMirRuntimeCleanup *cleanup, SolDiagnostics *diagnostics);
bool sol_mir_runtime_cleanup_internal_reconstruct(const SolMirRuntimeConventions *,
    const SolMirRuntimeValues *, const SolMirRuntimeCleanupLimits *,
    SolMirRuntimeCleanupUsage *);
void *sol_mir_runtime_cleanup_internal_validation_scratch_allocate(size_t);
void sol_mir_runtime_cleanup_internal_validation_scratch_free(void *);
bool sol_mir_runtime_cleanup_internal_alias_work(const SolMirRuntimeCleanup *,
    size_t *);
#ifdef SOL_MIR_PLAN_TEST_HOOKS
size_t sol_mir_runtime_cleanup_internal_test_storage_size(void);
size_t sol_mir_runtime_cleanup_internal_test_independent_storage_size(void);
typedef enum {
    SOL_MIR_RUNTIME_CLEANUP_TEST_ROW_EVENT = 1,
    SOL_MIR_RUNTIME_CLEANUP_TEST_ROW_ACTION,
    SOL_MIR_RUNTIME_CLEANUP_TEST_ROW_DROP_PATH,
    SOL_MIR_RUNTIME_CLEANUP_TEST_ROW_TRANSITION,
    SOL_MIR_RUNTIME_CLEANUP_TEST_ROW_SUPPLEMENTAL_SITE,
    SOL_MIR_RUNTIME_CLEANUP_TEST_ROW_USAGE,
} SolMirRuntimeCleanupTestRowKind;
/* Test bridge only. Operands must point to readable, correctly aligned rows
 * of the selected kind; this compares row values, not owner authentication. */
bool sol_mir_runtime_cleanup_internal_test_row_equal(unsigned,
    const void *, const void *);
#endif
#endif
