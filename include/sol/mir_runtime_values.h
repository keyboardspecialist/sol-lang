#ifndef SOL_MIR_RUNTIME_VALUES_H
#define SOL_MIR_RUNTIME_VALUES_H

#include "sol/mir_runtime_conventions.h"

typedef size_t SolMirRuntimeRecipeOperationId;

typedef struct {
    SolMirRecipeId recipe;
    uint32_t demanded_operations;
    SolMirRuntimeImportId create_import;
    SolMirRuntimeImportId copy_import;
    SolMirRuntimeImportId drop_import;
    SolMirRuntimeImportId equal_import;
} SolMirRuntimeRecipeOperations;

typedef struct {
    size_t max_records;
    size_t max_owned_bytes;
    size_t max_build_scratch_bytes;
    size_t max_build_work;
    size_t max_validation_scratch_bytes;
    size_t max_validation_work;
} SolMirRuntimeValuesLimits;

typedef struct {
    size_t records;
    size_t owned_bytes;
    size_t build_scratch_bytes;
    size_t build_work;
    size_t validation_scratch_bytes;
    size_t validation_work;
} SolMirRuntimeValuesUsage;

/* Unstable target-neutral operation-demand inventory. conventions is borrowed,
   authenticated, address-stable, immutable, and must outlive this owner. This
   owner defines no allocation plans or executable value operations. */
typedef struct {
    const SolMirRuntimeConventions *conventions;
    SolMirRuntimeRecipeOperations *recipe_operations;
    size_t recipe_operation_count;
    size_t recipe_operation_capacity;
    SolMirRuntimeValuesLimits limits;
    SolMirRuntimeValuesUsage usage;
} SolMirRuntimeValues;

typedef struct {
    const SolMirRuntimeConventions *conventions;
    /* NULL or wholly zero selects defaults. Partial zero is invalid. */
    const SolMirRuntimeValuesLimits *limits;
} SolMirRuntimeValuesBuildRequest;

typedef enum {
    SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED,
    SOL_MIR_RUNTIME_VALUES_BUILD_INVALID_ARGUMENT,
    SOL_MIR_RUNTIME_VALUES_BUILD_INVALID_CONVENTIONS,
    SOL_MIR_RUNTIME_VALUES_BUILD_UNSUPPORTED,
    SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED,
    SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED,
    SOL_MIR_RUNTIME_VALUES_BUILD_INTERNAL_FAILED,
} SolMirRuntimeValuesBuildOutcome;

void sol_mir_runtime_values_init(SolMirRuntimeValues *values);
void sol_mir_runtime_values_free(SolMirRuntimeValues *values);
SolMirRuntimeValuesLimits sol_mir_runtime_values_default_limits(void);
SolMirRuntimeValuesBuildOutcome sol_mir_runtime_values_build(
    const SolMirRuntimeValuesBuildRequest *request,
    SolMirRuntimeValues *values,
    SolDiagnostics *diagnostics
);
bool sol_mir_runtime_values_validate(
    const SolMirRuntimeValues *values,
    SolDiagnostics *diagnostics
);
/* Validation and buffering complete before the single caller-visible write. */
bool sol_mir_runtime_values_render(FILE *stream,
    const SolMirRuntimeValues *values);

#ifdef SOL_MIR_PLAN_TEST_HOOKS
void sol_mir_runtime_values_test_force_allocation_failure(bool force);
void sol_mir_runtime_values_test_force_validation_allocation_failure(bool force);
size_t sol_mir_runtime_values_test_validation_allocation_attempts(void);
bool sol_mir_runtime_values_test_reconstruct_usage(
    const SolMirRuntimeConventions *conventions,
    const SolMirRuntimeValuesLimits *limits,
    SolMirRuntimeValuesUsage *usage
);
#endif

#endif
