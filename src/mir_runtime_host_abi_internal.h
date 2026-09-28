#ifndef SOL_MIR_RUNTIME_HOST_ABI_INTERNAL_H
#define SOL_MIR_RUNTIME_HOST_ABI_INTERNAL_H
#include "sol/mir_runtime_host_abi.h"

/* This meter is deliberately operation based.  A caller must tick before it
 * examines a queue item, follows an edge, compares a record, or emits one; a
 * rejected tick performs no persistent-owner allocation. */
typedef struct {
    size_t limit;
    size_t used;
} SolMirRuntimeHostAbiWorkMeter;
static inline bool sol_mir_runtime_host_abi_work_tick(
    SolMirRuntimeHostAbiWorkMeter *meter) {
    if (!meter || meter->used == SIZE_MAX || meter->used >= meter->limit)
        return false;
    ++meter->used;
    return true;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
extern _Thread_local size_t sol_mir_runtime_host_abi_internal_allocation_attempts;
extern _Thread_local SolMirRuntimeHostAbiWorkCensus
    sol_mir_runtime_host_abi_internal_work_census;
#endif
SolMirRuntimeHostAbiBuildOutcome sol_mir_runtime_host_abi_internal_validate(
    const SolMirRuntimeHostAbi *, SolDiagnostics *);
bool sol_mir_runtime_host_abi_internal_measure_validation_work(
    const SolMirRuntimeHostAbi *, size_t *);
#ifdef SOL_MIR_PLAN_TEST_HOOKS
void sol_mir_runtime_host_abi_internal_record_validation_work(size_t audit,
    size_t validation);
#endif
#endif
