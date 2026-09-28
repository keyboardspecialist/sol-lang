#ifndef SOL_MIR_RUNTIME_HOST_ABI_H
#define SOL_MIR_RUNTIME_HOST_ABI_H

/* P3.4 is an authenticated description of the safe host boundary.  It is not
 * an executable ABI: all IDs below are local to an owner and opaque roots are
 * only configuration coordinates. */
#include "sol/mir_runtime_cleanup.h"

typedef size_t SolMirRuntimeHostAbiCapabilityId;
typedef size_t SolMirRuntimeHostAbiEntryRootId;
typedef size_t SolMirRuntimeHostAbiOperationId;
typedef size_t SolMirRuntimeHostAbiArgumentId;
typedef size_t SolMirRuntimeHostAbiFormalId;
typedef size_t SolMirRuntimeHostAbiShapeId;
typedef size_t SolMirRuntimeHostAbiShapeCaseId;
typedef size_t SolMirRuntimeHostAbiRequirementId;

typedef enum {
    SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT,
    SOL_MIR_RUNTIME_HOST_CAPABILITY_DERIVED,
    SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE,
} SolMirRuntimeHostCapabilitySource;

typedef struct {
    SolMirRecipeId recipe;
    SolMirRuntimeHostCapabilitySource source;
    /* This is a construction-instance arena, not a recipe census.  A
       construction record names its exact P2 construction and result; source
       coordinates describe the selected materialized operand, never an
       operand ordinal. */
    size_t source_construct;
    SolMirMaterializedValueId result;
    SolMirMaterializedTemporaryId temporary;
    SolMirMaterializedValueId source_value;
    SolMirMaterializedTemporaryId source_temporary;
    SolMirMaterializedPlaceId source_place;
    SolMirRuntimeHostAbiEntryRootId source_root;
    SolMirRuntimeHostAbiCapabilityId parent;
    SolMirRecipeId private_recipe;
    SolMirMaterializedValueId private_value;
} SolMirRuntimeHostCapabilityPlan;

typedef struct {
    SolMirRuntimeEntryId entry;
    size_t formal;
    SolMirRecipeId recipe;
    SolMirRuntimeSignatureId signature;
} SolMirRuntimeHostEntryRoot;

typedef enum {
    SOL_MIR_RUNTIME_HOST_ARGUMENT_INT64,
    SOL_MIR_RUNTIME_HOST_ARGUMENT_BOOL,
    SOL_MIR_RUNTIME_HOST_ARGUMENT_TEXT,
    SOL_MIR_RUNTIME_HOST_ARGUMENT_UNIT,
    SOL_MIR_RUNTIME_HOST_ARGUMENT_OPTION,
    SOL_MIR_RUNTIME_HOST_ARGUMENT_RESULT,
} SolMirRuntimeHostArgumentKind;

typedef struct {
    SolMirRecipeId recipe;
    SolMirRuntimeHostArgumentKind kind;
    SolAccessMode access;
    SolMirRuntimeSlice children;
} SolMirRuntimeHostArgument;

/* A callback formal owns exactly one recursive data-only shape.  Shape nodes
 * are deliberately separate from formals so two equal-looking formals retain
 * distinct roots and cases retain their semantic selection. */
typedef struct {
    SolMirRecipeId recipe;
    SolAccessMode access;
    SolMirRuntimeHostAbiShapeId shape;
} SolMirRuntimeHostFormal;
typedef struct {
    SolMirRecipeId recipe;
    SolMirRuntimeHostArgumentKind kind;
    SolAccessMode access;
    SolMirRuntimeSlice cases;
} SolMirRuntimeHostShape;
typedef struct {
    SolMirRuntimeHostAbiShapeId parent;
    size_t ordinal;
    SolMirRuntimeHostAbiShapeId child;
} SolMirRuntimeHostShapeCase;

typedef struct {
    SolMirLinkageHostRequirementId host;
    SolMirRuntimeImportId import_id;
    SolMirRuntimeSignatureId signature;
    SolMirRecipeId receiver;
    SolAccessMode receiver_access;
    SolMirRuntimeSlice formals;
    SolMirRecipeId result;
    SolMirRuntimeHostResultClass result_class;
    SolMirRecipeId result_plan;
    SolMirMaterializedEffectRowId effects;
    SolMirLinkageDigest identity;
    SolMirLinkageSymbol symbol;
    uint32_t failure_mask;
} SolMirRuntimeHostOperation;

typedef struct {
    SolMirRuntimeEntryId entry;
    SolMirRuntimeHostAbiEntryRootId root;
    SolMirRuntimeHostAbiOperationId operation;
    /* Exact P3.1 invoke site that establishes this authority relation. */
    size_t call;
    SolMirRuntimeCleanupEventId event;
    SolMirRuntimeCleanupTransitionId failure_transition;
} SolMirRuntimeHostRequirement;

typedef struct {
    size_t max_capabilities, max_entry_roots, max_operations, max_arguments;
    size_t max_formals, max_shapes, max_shape_cases;
    size_t max_requirements, max_grants;
    size_t max_owned_bytes, max_build_scratch_bytes, max_build_work;
    size_t max_validation_scratch_bytes, max_validation_work;
} SolMirRuntimeHostAbiLimits;
typedef struct {
    size_t capabilities, entry_roots, operations, arguments, requirements, grants;
    size_t formals, shapes, shape_cases;
    size_t owned_bytes, build_scratch_bytes, build_work;
    size_t validation_scratch_bytes, validation_work;
} SolMirRuntimeHostAbiUsage;

typedef struct {
    const SolMirRuntimeConventions *conventions;
    const SolMirRuntimeValues *values;
    const SolMirRuntimeCleanup *cleanup;
    SolMirRuntimeHostCapabilityPlan *capabilities;
    size_t capability_count, capability_capacity;
    SolMirRuntimeHostEntryRoot *entry_roots;
    size_t entry_root_count, entry_root_capacity;
    SolMirRuntimeHostOperation *operations;
    size_t operation_count, operation_capacity;
    SolMirRuntimeHostArgument *arguments;
    size_t argument_count, argument_capacity;
    SolMirRuntimeHostFormal *formals;
    size_t formal_count, formal_capacity;
    SolMirRuntimeHostShape *shapes;
    size_t shape_count, shape_capacity;
    SolMirRuntimeHostShapeCase *shape_cases;
    size_t shape_case_count, shape_case_capacity;
    SolMirRuntimeHostRequirement *requirements;
    size_t requirement_count, requirement_capacity;
    SolMirRuntimeHostAbiLimits limits;
    SolMirRuntimeHostAbiUsage usage;
    /* Set only after independent construction validation.  This is a
       field-wise seal over the header and every semantic owner arena; it does
       not depend on padding bytes and is recomputed by allocation-free
       preflight before a grant is trusted. */
    uint64_t authentication;
} SolMirRuntimeHostAbi;

typedef struct {
    const SolMirRuntimeConventions *conventions;
    const SolMirRuntimeValues *values;
    const SolMirRuntimeCleanup *cleanup;
    /* NULL or all zero selects defaults; partial zero is rejected. */
    const SolMirRuntimeHostAbiLimits *limits;
} SolMirRuntimeHostAbiBuildRequest;

typedef enum {
    SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED,
    SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_ARGUMENT,
    SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR,
    SOL_MIR_RUNTIME_HOST_ABI_BUILD_UNSUPPORTED,
    SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED,
    SOL_MIR_RUNTIME_HOST_ABI_BUILD_ALLOCATION_FAILED,
    SOL_MIR_RUNTIME_HOST_ABI_BUILD_INTERNAL_FAILED,
} SolMirRuntimeHostAbiBuildOutcome;

void sol_mir_runtime_host_abi_init(SolMirRuntimeHostAbi *owner);
void sol_mir_runtime_host_abi_free(SolMirRuntimeHostAbi *owner);
SolMirRuntimeHostAbiLimits sol_mir_runtime_host_abi_default_limits(void);
SolMirRuntimeHostAbiBuildOutcome sol_mir_runtime_host_abi_build(
    const SolMirRuntimeHostAbiBuildRequest *, SolMirRuntimeHostAbi *, SolDiagnostics *);
bool sol_mir_runtime_host_abi_validate(const SolMirRuntimeHostAbi *, SolDiagnostics *);
/* Validates first, buffers canonical semantic host-operation lines, then makes
 * exactly one caller-visible write. */
bool sol_mir_runtime_host_abi_render(FILE *, const SolMirRuntimeHostAbi *);

/* A binding is a fresh caller-selected opaque coordinate for exactly one entry
 * root.  Its value is never provided to a host callback. */
typedef struct { SolMirRuntimeHostAbiEntryRootId root; uint64_t opaque; }
    SolMirRuntimeHostRootBinding;
typedef struct { SolMirRuntimeHostAbiEntryRootId root;
    SolMirRuntimeHostAbiOperationId operation; } SolMirRuntimeHostGrant;
typedef struct {
    SolMirRuntimeEntryId entry;
    const SolMirRuntimeHostRootBinding *roots; size_t root_count;
    const SolMirRuntimeHostGrant *grants; size_t grant_count;
} SolMirRuntimeHostPreflightRequest;
typedef enum {
    SOL_MIR_RUNTIME_HOST_PREFLIGHT_SUCCEEDED,
    SOL_MIR_RUNTIME_HOST_PREFLIGHT_INVALID,
    SOL_MIR_RUNTIME_HOST_PREFLIGHT_UNSUPPORTED,
} SolMirRuntimeHostPreflightOutcome;
SolMirRuntimeHostPreflightOutcome sol_mir_runtime_host_abi_preflight(
    const SolMirRuntimeHostAbi *, const SolMirRuntimeHostPreflightRequest *);

#ifdef SOL_MIR_PLAN_TEST_HOOKS
typedef bool (*SolMirRuntimeHostTestCallback)(void *context,
    const SolMirRuntimeHostValue *const *arguments, size_t argument_count,
    const SolMirRuntimeHostValue **result, const uint8_t **detail, size_t *detail_length);
typedef struct {
    const SolMirRuntimeHostAbi *abi;
    const SolMirRuntimeHostPreflightRequest *preflight;
    SolMirRuntimeHostAbiEntryRootId root;
    SolMirRuntimeHostAbiOperationId operation;
    size_t call;
    const SolMirRuntimeHostValue *const *arguments; size_t argument_count;
    SolMirRuntimeHostTestCallback callback; void *context;
    size_t max_calls; size_t *calls;
    const SolMirRuntimeAllocationQuota *quota; SolMirRuntimeAllocationUsage *usage;
    const SolMirRuntimeHostTransferLimits *transfer_limits;
} SolMirRuntimeHostInvocationRequest;
typedef enum {
    SOL_MIR_RUNTIME_HOST_INVOKE_SUCCEEDED,
    SOL_MIR_RUNTIME_HOST_INVOKE_INVALID,
    SOL_MIR_RUNTIME_HOST_INVOKE_TRANSFER_LIMIT,
    SOL_MIR_RUNTIME_HOST_INVOKE_ALLOCATION_LIMIT,
    SOL_MIR_RUNTIME_HOST_INVOKE_ALLOCATION_FAILED,
    SOL_MIR_RUNTIME_HOST_INVOKE_CALL_LIMIT,
    SOL_MIR_RUNTIME_HOST_INVOKE_HOST_ERROR,
} SolMirRuntimeHostInvocationOutcome;
typedef struct {
    SolMirRuntimeHostInvocationOutcome outcome;
    SolMirRuntimeHostOwnedValue *value;
    SolMirRuntimeFailureRecord failure;
    /* Authenticated P3.3 CALL_FAILURE selection.  On HOST_ERROR this carries
       the selected disposition and the complete primary occurrence. */
    SolMirRuntimeCleanupTrace cleanup;
    uint8_t detail[SOL_MIR_RUNTIME_HOST_DETAIL_MAX];
} SolMirRuntimeHostInvocationResult;
SolMirRuntimeHostInvocationOutcome sol_mir_runtime_host_abi_test_invoke(
    const SolMirRuntimeHostInvocationRequest *, SolMirRuntimeHostInvocationResult *);
/* Test-only, allocation-free borrowed-view checker for any authenticated P2
 * recipe in this owner's representation.  It neither requires a host
 * operation/grant nor invokes a callback or transfer. */
bool sol_mir_runtime_host_abi_test_check_shape(const SolMirRuntimeHostAbi *,
    SolMirRecipeId, const SolMirRuntimeHostValue *,
    const SolMirRuntimeHostTransferLimits *);
void sol_mir_runtime_host_abi_test_force_persistent_allocation_failure(bool);
/* One-based persistent owner-arena allocation refusal; zero disables it. */
void sol_mir_runtime_host_abi_test_force_persistent_allocation_failure_attempt(size_t);
/* Fails the single bounded build-workspace allocation. */
void sol_mir_runtime_host_abi_test_force_build_scratch_allocation_failure(bool);
void sol_mir_runtime_host_abi_test_force_validation_scratch_failure(bool);
/* Counts this owner's allocator calls.  In particular, a preflight on an
 * authenticated immutable owner must leave this unchanged. */
size_t sol_mir_runtime_host_abi_test_allocation_attempts(void);
void sol_mir_runtime_host_abi_test_reset_allocation_attempts(void);
/* Last completed construction and independent-validation meter readings.  The
 * construction readings are deliberately retained separately so tests can
 * prove that the dry and persistent traversals did not drift. */
typedef struct {
    size_t census_work;
    size_t dry_work;
    size_t actual_work;
    size_t validation_audit_work;
    size_t validation_replay_work;
    size_t validation_work;
} SolMirRuntimeHostAbiWorkCensus;
SolMirRuntimeHostAbiWorkCensus sol_mir_runtime_host_abi_test_work_census(void);
#endif
#endif
