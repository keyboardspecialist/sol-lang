#ifndef SOL_MIR_CONCRETE_INTERNAL_H
#define SOL_MIR_CONCRETE_INTERNAL_H

#include "sol/mir_concrete.h"

bool sol_mir_concrete_internal_validation_requirements(
    const SolMirConcreteProgram *program,
    size_t *work,
    size_t *scratch_bytes
);

bool sol_mir_concrete_internal_validate_measured(
    const SolMirConcreteProgram *program,
    SolDiagnostics *diagnostics,
    size_t exact_work_limit,
    size_t exact_scratch_limit,
    size_t *measured_work,
    size_t *measured_scratch_bytes
);

#endif
