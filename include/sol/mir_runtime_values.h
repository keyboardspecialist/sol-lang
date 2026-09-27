#ifndef SOL_MIR_RUNTIME_VALUES_H
#define SOL_MIR_RUNTIME_VALUES_H

#include "sol/mir_runtime_conventions.h"

#include <stdint.h>

typedef size_t SolMirRuntimeRecipeOperationId;

typedef struct {
    SolMirRecipeId recipe;
    uint32_t demanded_operations;
    SolMirRuntimeImportId create_import;
    SolMirRuntimeImportId copy_import;
    SolMirRuntimeImportId drop_import;
    SolMirRuntimeImportId equal_import;
} SolMirRuntimeRecipeOperations;

typedef enum {
    SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE,
    SOL_MIR_RUNTIME_ALLOCATION_PLAN_FIXED_OBJECT,
    SOL_MIR_RUNTIME_ALLOCATION_PLAN_TEXT,
} SolMirRuntimeAllocationPlanKind;

/* One same-ID outer-object allocation description per concrete recipe. */
typedef struct {
    SolMirRecipeId recipe;
    SolMirRuntimeAllocationPlanKind kind;
    uint64_t object_size;
    uint64_t object_alignment;
} SolMirRuntimeAllocationPlan;

/* Static ownership descriptions only: P3.2c does not execute moves or drops. */
typedef enum {
    SOL_MIR_RUNTIME_OWNERSHIP_UNREACHABLE,
    SOL_MIR_RUNTIME_OWNERSHIP_LEAF,
    SOL_MIR_RUNTIME_OWNERSHIP_TEXT,
    SOL_MIR_RUNTIME_OWNERSHIP_PRODUCT,
    SOL_MIR_RUNTIME_OWNERSHIP_SUM,
    SOL_MIR_RUNTIME_OWNERSHIP_WRAPPER,
    SOL_MIR_RUNTIME_OWNERSHIP_CALLABLE,
    SOL_MIR_RUNTIME_OWNERSHIP_CAPABILITY,
} SolMirRuntimeOwnershipClass;

typedef enum {
    SOL_MIR_RUNTIME_OWNED_EDGE_FIELD,
    SOL_MIR_RUNTIME_OWNED_EDGE_BACKING,
    SOL_MIR_RUNTIME_OWNED_EDGE_PRIVATE_SOURCE,
    SOL_MIR_RUNTIME_OWNED_EDGE_CAPTURED_RECEIVER,
} SolMirRuntimeOwnedEdgeKind;

/* producer is SOL_MIR_RUNTIME_NONE except for CAPTURED_RECEIVER edges. */
typedef struct {
    SolMirRuntimeOwnedEdgeKind kind;
    SolMirRecipeId recipe;
    size_t ordinal;
    size_t producer;
} SolMirRuntimeOwnedEdge;

/* A SUM plan indexes one of these records for every source variant, including
   empty and unreachable variants. */
typedef struct {
    size_t ordinal;
    size_t semantic_tag;
    SolMirRuntimeSlice edges;
} SolMirRuntimeOwnershipVariant;

typedef struct {
    SolMirRecipeId recipe;
    SolMirRuntimeOwnershipClass classification;
    SolMirRuntimeSlice edges;
    SolMirRuntimeSlice variants;
} SolMirRuntimeOwnershipPlan;

/* Runtime allocation quotas are cumulative and distinct from build limits. */
typedef struct {
    uint64_t max_requests;
    uint64_t max_bytes;
} SolMirRuntimeAllocationQuota;

typedef struct {
    uint64_t requests;
    uint64_t bytes;
} SolMirRuntimeAllocationUsage;

/* Text length is meaningful only for a TEXT allocation plan. */
typedef struct {
    SolMirRecipeId recipe;
    uint64_t text_length;
} SolMirRuntimeAllocationRequest;

typedef struct {
    uint64_t requests;
    uint64_t bytes;
} SolMirRuntimeAllocationDemand;

typedef enum {
    SOL_MIR_RUNTIME_ALLOCATION_SUCCEEDED,
    SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT,
    SOL_MIR_RUNTIME_ALLOCATION_LIMIT,
    /* Reserved for a later physical allocator; this checker never returns it. */
    SOL_MIR_RUNTIME_ALLOCATION_FAILED,
} SolMirRuntimeAllocationOutcome;

typedef struct {
    size_t max_records;
    size_t max_allocation_plans;
    size_t max_ownership_plans;
    size_t max_ownership_variants;
    size_t max_owned_edges;
    size_t max_owned_bytes;
    size_t max_build_scratch_bytes;
    size_t max_build_work;
    size_t max_validation_scratch_bytes;
    size_t max_validation_work;
} SolMirRuntimeValuesLimits;

typedef struct {
    size_t records;
    size_t allocation_plans;
    size_t ownership_plans;
    size_t ownership_variants;
    size_t owned_edges;
    size_t owned_bytes;
    size_t build_scratch_bytes;
    size_t build_work;
    size_t validation_scratch_bytes;
    size_t validation_work;
} SolMirRuntimeValuesUsage;

/* Unstable target-parameterized operation-demand and allocation-plan inventory.
   conventions is borrowed, authenticated, address-stable, immutable, and must
   outlive this owner. Plans describe allocation only, not allocation execution. */
typedef struct {
    const SolMirRuntimeConventions *conventions;
    SolMirRuntimeRecipeOperations *recipe_operations;
    size_t recipe_operation_count;
    size_t recipe_operation_capacity;
    SolMirRuntimeAllocationPlan *allocation_plans;
    size_t allocation_plan_count;
    size_t allocation_plan_capacity;
    SolMirRuntimeOwnershipPlan *ownership_plans;
    size_t ownership_plan_count;
    size_t ownership_plan_capacity;
    SolMirRuntimeOwnershipVariant *ownership_variants;
    size_t ownership_variant_count;
    size_t ownership_variant_capacity;
    SolMirRuntimeOwnedEdge *owned_edges;
    size_t owned_edge_count;
    size_t owned_edge_capacity;
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

/* O(1), allocation-free, non-mutating quota preflight. It validates its owner,
   request, quota, and current cumulative usage without revalidating predecessors.
   On success demand receives the requests and bytes to charge; destruction never
   refunds usage. demand must not overlap values or any input; rejected calls leave
   it unchanged. This pure checker never returns ALLOCATION_FAILED. */
SolMirRuntimeAllocationOutcome sol_mir_runtime_values_check_allocation(
    const SolMirRuntimeValues *values,
    const SolMirRuntimeAllocationRequest *request,
    const SolMirRuntimeAllocationQuota *quota,
    const SolMirRuntimeAllocationUsage *usage,
    SolMirRuntimeAllocationDemand *demand
);
/* Returns false for INVALID_ARGUMENT or an unknown outcome. */
bool sol_mir_runtime_allocation_outcome_failure(
    SolMirRuntimeAllocationOutcome outcome,
    SolMirRuntimeFailureCode *failure
);

#ifdef SOL_MIR_PLAN_TEST_HOOKS
void sol_mir_runtime_values_test_force_allocation_failure(bool force);
/* One-based persistent-arena allocation attempt; zero disables this hook. */
void sol_mir_runtime_values_test_force_persistent_allocation_failure(
    size_t attempt
);
void sol_mir_runtime_values_test_force_validation_allocation_failure(bool force);
size_t sol_mir_runtime_values_test_validation_allocation_attempts(void);
size_t sol_mir_runtime_values_test_ownership_count_scans(void);
void sol_mir_runtime_values_test_reverse_captured_fragments(bool reverse);
void sol_mir_runtime_values_test_force_captured_digest_collision(bool force);
bool sol_mir_runtime_values_test_reconstruct_usage(
    const SolMirRuntimeConventions *conventions,
    const SolMirRuntimeValuesLimits *limits,
    SolMirRuntimeValuesUsage *usage
);
#endif

#endif
