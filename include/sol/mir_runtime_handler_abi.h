#ifndef SOL_MIR_RUNTIME_HANDLER_ABI_H
#define SOL_MIR_RUNTIME_HANDLER_ABI_H

/* P3.5 freezes handler-frame descriptions.  It is deliberately not an
 * executor: the activation API below is a bounded test model only. */
#include "sol/mir_runtime_host_abi.h"

typedef size_t SolMirRuntimeHandlerFrameId;
typedef size_t SolMirRuntimeHandlerCleanupExitId;

typedef struct {
    /* Same-ID P2 concrete-handler identity. */
    SolMirMaterializedHandlerId handler;
    SolMirRuntimeSource source;
    SolMirMaterializedBindingId source_binding;
    SolMirMaterializedOperationKey source_operation;
    SolMirRuntimeSignatureId source_signature;
    SolMirOperationRootMatchRule root_match;
    SolMirMaterializedPlaceId authority_root;
    SolMirMaterializedEffectRowId effects;

    SolMirMaterializedBindingId provider_binding;
    SolMirMaterializedPlaceId provider_place;
    SolMirRecipeId provider_recipe;
    SolMirOperationAccessId provider_access;
    SolMirLinkageCallableId provider_internal;
    SolMirRuntimeSignatureId provider_signature;

    /* Exact P2 lexical delimiters, plus all corresponding P3.3 exits. */
    SolMirMaterializedInstructionId enter_marker;
    SolMirRuntimeSlice exit_markers;
    SolMirRuntimeSlice cleanup_exits;
    SolMirRuntimeHandlerFrameId parent;
} SolMirRuntimeHandlerFramePlan;

typedef struct {
    SolMirRuntimeHandlerFrameId frame;
    SolMirMaterializedInstructionId instruction;
    SolMirPlanInstanceId image;
    SolMirMaterializedBlockId block;
} SolMirRuntimeHandlerExitMarker;

typedef struct {
    SolMirRuntimeHandlerFrameId frame;
    SolMirRuntimeCleanupActionId action;
    SolMirRuntimeCleanupTransitionId transition;
} SolMirRuntimeHandlerCleanupExit;

typedef struct {
    size_t max_frames, max_marker_references, max_cleanup_exits;
    size_t max_stack_depth, max_test_work;
    size_t max_owned_bytes, max_build_scratch_bytes, max_build_work;
    size_t max_validation_scratch_bytes, max_validation_work;
} SolMirRuntimeHandlerAbiLimits;

typedef struct {
    size_t frames, marker_references, cleanup_exits;
    size_t stack_depth, test_work;
    size_t owned_bytes, build_scratch_bytes, build_work;
    size_t validation_scratch_bytes, validation_work;
} SolMirRuntimeHandlerAbiUsage;

typedef struct {
    const SolMirRuntimeConventions *conventions;
    const SolMirRuntimeValues *values;
    const SolMirRuntimeCleanup *cleanup;
    const SolMirRuntimeHostAbi *host_abi;
    SolMirRuntimeHandlerFramePlan *frames;
    size_t frame_count, frame_capacity;
    SolMirRuntimeHandlerExitMarker *exit_markers;
    size_t exit_marker_count, exit_marker_capacity;
    SolMirRuntimeHandlerCleanupExit *cleanup_exits;
    size_t cleanup_exit_count, cleanup_exit_capacity;
    SolMirRuntimeHandlerAbiLimits limits;
    SolMirRuntimeHandlerAbiUsage usage;
    uint64_t authentication;
} SolMirRuntimeHandlerAbi;

typedef struct {
    const SolMirRuntimeConventions *conventions;
    const SolMirRuntimeValues *values;
    const SolMirRuntimeCleanup *cleanup;
    const SolMirRuntimeHostAbi *host_abi;
    /* NULL or a wholly-zero value selects defaults. */
    const SolMirRuntimeHandlerAbiLimits *limits;
} SolMirRuntimeHandlerAbiBuildRequest;

typedef enum {
    SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_SUCCEEDED,
    SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_INVALID_ARGUMENT,
    SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_INVALID_PREDECESSOR,
    SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_UNSUPPORTED,
    SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_RESOURCE_EXHAUSTED,
    SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_ALLOCATION_FAILED,
    SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_INTERNAL_FAILED,
} SolMirRuntimeHandlerAbiBuildOutcome;

void sol_mir_runtime_handler_abi_init(SolMirRuntimeHandlerAbi *);
void sol_mir_runtime_handler_abi_free(SolMirRuntimeHandlerAbi *);
SolMirRuntimeHandlerAbiLimits sol_mir_runtime_handler_abi_default_limits(void);
SolMirRuntimeHandlerAbiBuildOutcome sol_mir_runtime_handler_abi_build(
    const SolMirRuntimeHandlerAbiBuildRequest *, SolMirRuntimeHandlerAbi *, SolDiagnostics *);
bool sol_mir_runtime_handler_abi_validate(const SolMirRuntimeHandlerAbi *, SolDiagnostics *);
/* Validation and buffering finish before the one caller-visible write. */
bool sol_mir_runtime_handler_abi_render(FILE *, const SolMirRuntimeHandlerAbi *);

#ifdef SOL_MIR_PLAN_TEST_HOOKS
typedef struct {
    SolMirRuntimeHandlerFrameId frame;
    SolMirRuntimeHandlerFrameId parent;
    /* Caller-supplied, opaque runtime receiver-root identity. */
    uint64_t root;
    /* Seal of the static authority/provenance and this activation root. */
    uint64_t identity;
} SolMirRuntimeHandlerActivation;
typedef struct {
    SolMirRuntimeHandlerActivation *frames;
    size_t count, capacity;
    /* [visible_count, count) is the temporarily hidden dispatch suffix. */
    size_t visible_count;
    /* Private-to-the-model integrity state; callers provide storage only. */
    uint64_t seal;
} SolMirRuntimeHandlerActivationStack;
typedef struct {
    SolMirMaterializedBindingId source_binding;
    SolMirMaterializedOperationKey operation;
    SolMirRuntimeSignatureId signature;
    uint64_t root;
} SolMirRuntimeHandlerDispatch;
typedef enum {
    SOL_MIR_RUNTIME_HANDLER_SELECT_INVALID,
    SOL_MIR_RUNTIME_HANDLER_SELECT_NO_MATCH,
    SOL_MIR_RUNTIME_HANDLER_SELECT_MATCH,
} SolMirRuntimeHandlerSelectOutcome;
typedef struct {
    SolMirRuntimeHandlerSelectOutcome outcome;
    size_t active_index;
    SolMirRuntimeHandlerFrameId frame;
    size_t hidden_count;
    uint64_t hidden_seal;
} SolMirRuntimeHandlerSelection;

void sol_mir_runtime_handler_abi_test_stack_init(SolMirRuntimeHandlerActivationStack *,
    SolMirRuntimeHandlerActivation *, size_t);
bool sol_mir_runtime_handler_abi_test_enter(const SolMirRuntimeHandlerAbi *,
    SolMirRuntimeHandlerActivationStack *, SolMirRuntimeHandlerFrameId,
    uint64_t runtime_root, bool provider_evaluation_succeeded);
SolMirRuntimeHandlerSelectOutcome sol_mir_runtime_handler_abi_test_select(
    const SolMirRuntimeHandlerAbi *, const SolMirRuntimeHandlerActivationStack *,
    const SolMirRuntimeHandlerDispatch *, SolMirRuntimeHandlerSelection *);
bool sol_mir_runtime_handler_abi_test_hide(const SolMirRuntimeHandlerAbi *,
    SolMirRuntimeHandlerActivationStack *,
    SolMirRuntimeHandlerSelection *);
bool sol_mir_runtime_handler_abi_test_restore(const SolMirRuntimeHandlerAbi *,
    SolMirRuntimeHandlerActivationStack *,
    const SolMirRuntimeHandlerSelection *, bool provider_succeeded);
bool sol_mir_runtime_handler_abi_test_exit_marker(const SolMirRuntimeHandlerAbi *,
    SolMirRuntimeHandlerActivationStack *, SolMirRuntimeHandlerFrameId,
    SolMirMaterializedInstructionId);
bool sol_mir_runtime_handler_abi_test_exit_cleanup(const SolMirRuntimeHandlerAbi *,
    SolMirRuntimeHandlerActivationStack *, SolMirRuntimeHandlerFrameId,
    SolMirRuntimeCleanupActionId);
void sol_mir_runtime_handler_abi_test_force_build_scratch_allocation_failure(bool);
void sol_mir_runtime_handler_abi_test_force_persistent_allocation_failure(bool);
void sol_mir_runtime_handler_abi_test_force_validation_scratch_failure(bool);
size_t sol_mir_runtime_handler_abi_test_allocation_attempts(void);
void sol_mir_runtime_handler_abi_test_reset_allocation_attempts(void);
bool sol_mir_runtime_handler_abi_test_predict_build_work(
    const SolMirRuntimeConventions *, const SolMirRuntimeCleanup *, size_t *);
#endif
#endif
