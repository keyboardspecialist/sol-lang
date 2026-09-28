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
#endif
