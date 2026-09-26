#ifndef SOL_MIR_CONCRETE_H
#define SOL_MIR_CONCRETE_H

#include "sol/mir_linkage.h"

/* Unstable compiler-internal concrete-program owner. The owner is
   address-stable and non-copyable because every stage borrows its embedded
   predecessor. Only the source SolIr remains borrowed. */
typedef struct {
    SolMirProgramLimits program;
    SolMirPlanLimits plan;
    SolMirMaterializeLimits materialization;
    SolMirRepresentationLimits representation;
    SolMirLayoutLimits layout;
    SolMirOperationsLimits operations;
    SolMirLinkageLimits linkage;
} SolMirConcreteLimits;

typedef struct {
    SolMirProgram program;
    SolMirPlan plan;
    SolMirMaterialization materialization;
    SolMirRepresentation representation;
    SolMirLayout layout;
    SolMirOperations operations;
    SolMirLinkage linkage;
} SolMirConcreteProgram;

typedef struct {
    const SolIr *ir;
    const SolMirProgramRoot *roots;
    size_t root_count;
    const SolIrCallableId *approved_imports;
    size_t approved_import_count;
    const SolMirTargetDescriptor *target;
    /* NULL or a wholly zero value selects the defaults. A partially zero
       value is invalid. Inputs are copied or embedded during the build. */
    const SolMirConcreteLimits *limits;
} SolMirConcreteBuildRequest;

typedef enum {
    SOL_MIR_CONCRETE_BUILD_SUCCEEDED,
    SOL_MIR_CONCRETE_BUILD_INVALID_ARGUMENT,
    SOL_MIR_CONCRETE_BUILD_INVALID_IR,
    SOL_MIR_CONCRETE_BUILD_INVALID_TARGET,
    SOL_MIR_CONCRETE_BUILD_UNSUPPORTED_CLOSURE,
    SOL_MIR_CONCRETE_BUILD_EXPANDING_RECURSION,
    SOL_MIR_CONCRETE_BUILD_SYMBOL_COLLISION,
    SOL_MIR_CONCRETE_BUILD_RESOURCE_EXHAUSTED,
    SOL_MIR_CONCRETE_BUILD_ALLOCATION_FAILED,
    SOL_MIR_CONCRETE_BUILD_INTERNAL_FAILED,
} SolMirConcreteBuildOutcome;

void sol_mir_concrete_program_init(SolMirConcreteProgram *program);
void sol_mir_concrete_program_free(SolMirConcreteProgram *program);
SolMirConcreteLimits sol_mir_concrete_default_limits(void);
SolMirConcreteBuildOutcome sol_mir_concrete_program_build(
    const SolMirConcreteBuildRequest *request,
    SolMirConcreteProgram *program,
    SolDiagnostics *diagnostics
);
bool sol_mir_concrete_program_validate(
    const SolMirConcreteProgram *program,
    SolDiagnostics *diagnostics
);
#ifdef SOL_MIR_PLAN_TEST_HOOKS
void sol_mir_concrete_test_force_validation_allocation_failure(bool force);
size_t sol_mir_concrete_test_validation_allocation_attempts(void);
#endif
/* Unstable versionless diagnostics. Validation and buffering finish before
   the single caller-visible write. */
bool sol_mir_concrete_program_render(
    FILE *stream,
    const SolMirConcreteProgram *program
);

#endif
