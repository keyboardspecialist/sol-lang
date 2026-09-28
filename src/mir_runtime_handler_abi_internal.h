#ifndef SOL_MIR_RUNTIME_HANDLER_ABI_INTERNAL_H
#define SOL_MIR_RUNTIME_HANDLER_ABI_INTERNAL_H
#include "sol/mir_runtime_handler_abi.h"

typedef struct { size_t limit, used; bool exhausted; } SolMirRuntimeHandlerAbiWorkMeter;
static inline bool sol_mir_runtime_handler_abi_tick(SolMirRuntimeHandlerAbiWorkMeter *m) {
    if (!m || m->used == SIZE_MAX || m->used >= m->limit) {
        if (m) m->exhausted = true;
        return false;
    }
    ++m->used; return true;
}
SolMirRuntimeHandlerAbiBuildOutcome sol_mir_runtime_handler_abi_internal_validate(
    const SolMirRuntimeHandlerAbi *, SolDiagnostics *);
SolMirRuntimeHandlerAbiBuildOutcome
sol_mir_runtime_handler_abi_internal_measure_validation_work(
    const SolMirRuntimeHandlerAbi *, size_t *, SolDiagnostics *);
bool sol_mir_runtime_handler_abi_internal_seal_metered(
    const SolMirRuntimeHandlerAbi *, SolMirRuntimeHandlerAbiWorkMeter *, uint64_t *);
bool sol_mir_runtime_handler_abi_internal_seal_work(
    const SolMirRuntimeHandlerAbi *, size_t *);
void *sol_mir_runtime_handler_abi_internal_validation_scratch(size_t, size_t);
#endif
