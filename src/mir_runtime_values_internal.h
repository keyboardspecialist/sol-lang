#ifndef SOL_MIR_RUNTIME_VALUES_INTERNAL_H
#define SOL_MIR_RUNTIME_VALUES_INTERNAL_H

#include "sol/mir_runtime_values.h"

SolMirRuntimeValuesBuildOutcome sol_mir_runtime_values_internal_preflight(
    const SolMirRuntimeConventions *conventions,
    const SolMirRuntimeValuesLimits *limits,
    SolMirRuntimeValuesUsage *usage,
    SolDiagnostics *diagnostics
);

SolMirRuntimeValuesBuildOutcome sol_mir_runtime_values_internal_validate(
    const SolMirRuntimeValues *values,
    SolDiagnostics *diagnostics
);

#endif
