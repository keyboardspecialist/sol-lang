#ifndef SOL_MIR_RUNTIME_CONVENTIONS_INTERNAL_H
#define SOL_MIR_RUNTIME_CONVENTIONS_INTERNAL_H

#include "sol/mir_runtime_conventions.h"

bool sol_mir_runtime_conventions_internal_validation_requirements(
    const SolMirRuntimeConventions *owner,
    size_t *work,
    size_t *scratch,
    SolDiagnostics *diagnostics
);

#endif
