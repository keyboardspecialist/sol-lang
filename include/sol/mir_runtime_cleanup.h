#ifndef SOL_MIR_RUNTIME_CLEANUP_H
#define SOL_MIR_RUNTIME_CLEANUP_H

#include "sol/mir_runtime_values.h"

#include <stdint.h>

typedef size_t SolMirRuntimeCleanupEventId;
typedef size_t SolMirRuntimeCleanupActionId;
typedef size_t SolMirRuntimeCleanupTransitionId;
typedef size_t SolMirRuntimeCleanupSupplementalSiteId;
typedef size_t SolMirRuntimeCleanupDropPathId;

/* The role names the P2 edge being described; it is not inferred from an
 * outcome.  In particular, PROPAGATE is reserved for RESUME_FAILURE. */
typedef enum {
    SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO,
    SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_TRUE,
    SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_FALSE,
    SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL,
    SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE,
    SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_SATISFIED,
    SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_VIOLATION,
    SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_FAILURE,
    SOL_MIR_RUNTIME_CLEANUP_EDGE_PROPAGATE_VALUE,
    SOL_MIR_RUNTIME_CLEANUP_EDGE_PROPAGATE_RESIDUAL,
    SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_SATISFIED,
    SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION,
    SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_FAILURE,
    SOL_MIR_RUNTIME_CLEANUP_EDGE_RETURN,
    SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE,
} SolMirRuntimeCleanupEdgeRole;

typedef enum {
    SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL,
    SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_ARITHMETIC,
    SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_INVOKE,
    SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_PANIC,
    SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_NO_MATCH,
    SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_UNREACHABLE,
    SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_ARITHMETIC,
    SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_INVOKE,
    SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_NO_MATCH,
    SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_RESULT,
    SOL_MIR_RUNTIME_CLEANUP_PRODUCER_SUPPLEMENTAL_ALLOCATION,
} SolMirRuntimeCleanupProducerKind;

typedef enum {
    SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE,
    SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL,
} SolMirRuntimeCleanupDropLiveness;

/* A transition site is interpreted in this namespace.  PENDING deliberately
 * has no site: the occurrence is authenticated at the producing transition
 * before it is resumed through this one. */
typedef enum {
    SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE,
    SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31,
    SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_SUPPLEMENTAL_P33,
    SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING,
} SolMirRuntimeCleanupFailureSource;

typedef enum {
    SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION,
    SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR,
    SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION,
    SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR,
    /* Compatibility names for the two fallible instruction classes. */
    SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_ARITHMETIC = SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION,
    SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_ARITHMETIC = SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION,
} SolMirRuntimeCleanupEventKind;

typedef enum { SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT,
    SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT } SolMirRuntimeCleanupOrigin;

typedef enum {
    SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK,
    SOL_MIR_RUNTIME_CLEANUP_ACTION_CHECK_CONTRACT,
    SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY,
    SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE,
    SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE,
    SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_REGION,
    /* Mirrors the executable HANDLER_EXIT marker after its lexical scope. */
    SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_HANDLER,
    SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT,
    SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER,
    SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE,
} SolMirRuntimeCleanupActionKind;

typedef enum { SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL,
    SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE,
    SOL_MIR_RUNTIME_CLEANUP_OUTCOME_EXIT } SolMirRuntimeCleanupOutcome;

enum {
    SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY = 1u,
    SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY = 2u,
    /* The root is MAYBE_INITIALIZED or contains a projected moved-out hole. */
    SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED = 4u,
};

typedef struct {
    SolMirRuntimeCleanupEventKind kind;
    SolMirRuntimeCleanupOrigin origin;
    size_t owner, block, operation;
    SolMirRuntimeSource source;
    SolMirRuntimeFailureSiteId inherited_failure_site;
    SolMirRuntimeCleanupSupplementalSiteId supplemental_site;
    SolMirRuntimeSlice actions;      /* all transition action records for event */
    SolMirRuntimeSlice transitions;  /* one record for each concrete outcome */
    SolMirRuntimeCleanupProducerKind producer;
    /* The producer captures its bounded failure detail before this event's
     * cleanup slice.  In particular this protects panic Text backing storage. */
    bool captures_failure_detail;
    /* This describes the ordinary producer shape only.  Capture validation is
     * per failure code: PANIC has PANIC_TEXT, HOST_ERROR has HOST_BYTES, and
     * every other code has identity-only NONE detail before cleanup. */
    SolMirRuntimeFailureDetailKind capture_detail_kind;
} SolMirRuntimeCleanupEvent;

typedef struct {
    SolMirRuntimeCleanupActionKind kind;
    unsigned flags;
    size_t target;
    SolMirRecipeId recipe;
    /* DROP_PLACE/DROP_PARAMETER only; otherwise NONE. */
    SolMirRuntimeCleanupDropPathId drop_path;
} SolMirRuntimeCleanupAction;

/* `place` is the exact authenticated materialized place (and hence its full
 * projection sequence). `root` is dropped minus every same-root path in the
 * action's contiguous antichain range.  The current materializer supplies one
 * path per action; the range form deliberately preserves the representation
 * for sibling-hole sets without borrowing mutable liveness state. */
typedef struct {
    SolMirMaterializedPlaceId root;
    SolMirMaterializedPlaceId place;
    SolMirRuntimeSlice holes;
    SolMirRecipeId recipe;
    SolMirRuntimeCleanupDropLiveness liveness;
} SolMirRuntimeCleanupDropPath;

typedef struct {
    SolMirRuntimeCleanupEventId event;
    SolMirRuntimeCleanupOutcome outcome;
    /* Concrete P2 edge for a continuation, or SOL_MIR_RUNTIME_NONE on exit. */
    size_t continuation;
    SolMirRuntimeSlice actions;
    bool primary_failure_wins;
    SolMirRuntimeCleanupEdgeRole edge_role;
    /* Named materialized P2 edge and its destination block.  Both are NONE
     * only for a terminal; failure identity remains in P3.1. */
    size_t source_edge;
    size_t destination;
    SolMirRuntimeCleanupFailureSource failure_source;
    /* A P3.1 or P3.3 site ID, interpreted by failure_source; NONE otherwise. */
    size_t failure_site;
    uint32_t failure_mask;
    SolContractClauseKind contract_phase;
    SolContractOutcomeKind contract_outcome;
} SolMirRuntimeCleanupTransition;

typedef struct {
    SolMirRuntimeCleanupEventId event;
    SolMirRuntimeSource source;
    uint32_t allowed_codes;
} SolMirRuntimeCleanupSupplementalSite;

/* Captured before cleanup.  Panic text is always NUL terminated; other detail
 * is a bounded 191-byte prefix and is never borrowed from runtime storage. */
typedef struct {
    SolMirRuntimeFailureCode code;
    SolMirRuntimeSource source;
    SolMirRuntimeFailureDetailKind kind;
    size_t length;
    uint8_t bytes[SOL_MIR_RUNTIME_HOST_DETAIL_MAX + 1];
} SolMirRuntimeCleanupDetail;

typedef struct { size_t max_events, max_actions, max_transitions, max_supplemental_sites;
    size_t max_drop_paths;
    size_t max_owned_bytes, max_build_scratch_bytes, max_build_work;
    size_t max_validation_scratch_bytes, max_validation_work; } SolMirRuntimeCleanupLimits;
typedef struct { size_t events, actions, transitions, supplemental_sites;
    size_t drop_paths;
    size_t owned_bytes, build_scratch_bytes, build_work;
    size_t validation_scratch_bytes, validation_work; } SolMirRuntimeCleanupUsage;

typedef struct {
    const SolMirRuntimeConventions *conventions;
    const SolMirRuntimeValues *values;
    SolMirRuntimeCleanupEvent *events; size_t event_count, event_capacity;
    SolMirRuntimeCleanupAction *actions; size_t action_count, action_capacity;
    SolMirRuntimeCleanupTransition *transitions; size_t transition_count, transition_capacity;
    SolMirRuntimeCleanupSupplementalSite *supplemental_sites;
    size_t supplemental_site_count, supplemental_site_capacity;
    SolMirRuntimeCleanupDropPath *drop_paths;
    size_t drop_path_count, drop_path_capacity;
    SolMirRuntimeCleanupLimits limits;
    SolMirRuntimeCleanupUsage usage;
} SolMirRuntimeCleanup;
typedef struct { const SolMirRuntimeConventions *conventions;
    const SolMirRuntimeValues *values;
    /* NULL or wholly zero selects defaults; partial zero is invalid. */
    const SolMirRuntimeCleanupLimits *limits; } SolMirRuntimeCleanupBuildRequest;
typedef enum { SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED,
    SOL_MIR_RUNTIME_CLEANUP_BUILD_INVALID_ARGUMENT,
    SOL_MIR_RUNTIME_CLEANUP_BUILD_INVALID_PREDECESSOR,
    SOL_MIR_RUNTIME_CLEANUP_BUILD_RESOURCE_EXHAUSTED,
    SOL_MIR_RUNTIME_CLEANUP_BUILD_ALLOCATION_FAILED,
    SOL_MIR_RUNTIME_CLEANUP_BUILD_INTERNAL_FAILED } SolMirRuntimeCleanupBuildOutcome;

void sol_mir_runtime_cleanup_init(SolMirRuntimeCleanup *cleanup);
void sol_mir_runtime_cleanup_free(SolMirRuntimeCleanup *cleanup);
SolMirRuntimeCleanupLimits sol_mir_runtime_cleanup_default_limits(void);
SolMirRuntimeCleanupBuildOutcome sol_mir_runtime_cleanup_build(const SolMirRuntimeCleanupBuildRequest *, SolMirRuntimeCleanup *, SolDiagnostics *);
bool sol_mir_runtime_cleanup_validate(const SolMirRuntimeCleanup *, SolDiagnostics *);
bool sol_mir_runtime_cleanup_render(FILE *, const SolMirRuntimeCleanup *);
/* Detail capture is event-owned: a caller cannot attach panic/host bytes to an
 * unrelated P3.1 site.  `edge_role` selects the producing transition. */
bool sol_mir_runtime_cleanup_capture_detail(const SolMirRuntimeCleanup *,
    SolMirRuntimeCleanupEventId, SolMirRuntimeCleanupEdgeRole,
    const SolMirRuntimeFailureRecord *, SolMirRuntimeCleanupDetail *);

#ifdef SOL_MIR_PLAN_TEST_HOOKS
/* Fully owned, bounded diagnostic identity.  `source` must be the exact
 * source triple owned by the P3.1/P3.3 site; detail bytes are never borrowed. */
typedef struct {
    SolMirRuntimeCleanupFailureSource failure_source;
    size_t site;
    SolMirRuntimeFailureCode code;
    SolMirRuntimeFailureDetailKind detail_kind;
    size_t detail_length;
    uint8_t detail_bytes[SOL_MIR_RUNTIME_HOST_DETAIL_MAX + 1];
    SolMirRuntimeSource source;
} SolMirRuntimeCleanupFailureOccurrence;

typedef struct {
    SolMirRuntimeCleanupEventId event;
    SolMirRuntimeCleanupEdgeRole edge_role;
    const SolMirRuntimeCleanupFailureOccurrence *produced;
    const SolMirRuntimeCleanupFailureOccurrence *pending;
    SolMirRuntimeCleanupDropLiveness liveness;
} SolMirRuntimeCleanupTraceRequest;

typedef struct {
    SolMirRuntimeCleanupEventId event;
    SolMirRuntimeCleanupEdgeRole edge_role;
    SolMirRuntimeCleanupOutcome outcome;
    SolMirRuntimeCleanupDropLiveness liveness;
    bool has_primary;
    SolMirRuntimeCleanupFailureOccurrence primary;
    size_t action_count;
    SolMirRuntimeCleanupAction *actions;
} SolMirRuntimeCleanupTrace;

/* Selects one concrete edge.  A producing failure needs `produced`; RESUME
 * needs `pending` and preserves it byte-for-byte.  Success/residual paths
 * reject both. */
bool sol_mir_runtime_cleanup_test_trace(const SolMirRuntimeCleanup *,
    const SolMirRuntimeCleanupTraceRequest *, SolMirRuntimeCleanupAction *, size_t,
    SolMirRuntimeCleanupTrace *);
bool sol_mir_runtime_cleanup_test_select(const SolMirRuntimeCleanup *,
    const SolMirRuntimeCleanupTraceRequest *, SolMirRuntimeCleanupAction *, size_t,
    SolMirRuntimeCleanupTrace *);

typedef struct {
    SolMirRuntimeCleanupEventId event;
    SolMirRuntimeCleanupEdgeRole edge_role;
    SolMirRuntimeCleanupFailureOccurrence occurrence;
    SolMirRuntimeCleanupDropLiveness liveness;
} SolMirRuntimeCleanupPrecedenceInput;

/* RESUME_FAILURE transports an already authenticated primary; it has no
 * second occurrence and cannot manufacture one. */
typedef struct {
    SolMirRuntimeCleanupEventId event;
    SolMirRuntimeCleanupEdgeRole edge_role;
    SolMirRuntimeCleanupDropLiveness liveness;
} SolMirRuntimeCleanupPrecedenceResume;

/* This test-only shape makes an attempted later producer explicit.  Runtime
 * failure cleanup is non-fallible and bypasses contract checks, so a checked,
 * reachable attempted-later occurrence is still rejected. */
typedef struct {
    SolMirRuntimeCleanupEventId event;
    SolMirRuntimeCleanupEdgeRole edge_role;
    SolMirRuntimeCleanupFailureOccurrence occurrence;
    SolMirRuntimeCleanupDropLiveness liveness;
} SolMirRuntimeCleanupAttemptedLater;

typedef enum {
    SOL_MIR_RUNTIME_CLEANUP_LATER_NOT_ATTEMPTED,
    SOL_MIR_RUNTIME_CLEANUP_LATER_SUPPRESSED_BY_PRIMARY,
} SolMirRuntimeCleanupLaterStatus;

/* A pending runtime failure bypasses normal writeback and success ensures.  A
 * reachable failure-outcome ensure may be authenticated as an attempted later
 * contract producer, but it is suppressed and cannot replace or augment the
 * primary failure. */
typedef struct {
    bool has_primary;
    SolMirRuntimeCleanupFailureOccurrence primary;
    SolMirRuntimeCleanupLaterStatus later_status;
    bool writeback_attempted;
    bool ensures_attempted;
    bool has_secondary;
} SolMirRuntimeCleanupPrecedenceResult;
bool sol_mir_runtime_cleanup_test_precedence(const SolMirRuntimeCleanup *,
    const SolMirRuntimeCleanupPrecedenceInput *first,
    const SolMirRuntimeCleanupPrecedenceResume *resume,
    SolMirRuntimeCleanupFailureOccurrence *primary);
bool sol_mir_runtime_cleanup_test_precedence_attempted_later(
    const SolMirRuntimeCleanup *, const SolMirRuntimeCleanupPrecedenceInput *,
    const SolMirRuntimeCleanupAttemptedLater *,
    size_t *reachability_scratch, size_t reachability_scratch_count);
bool sol_mir_runtime_cleanup_test_precedence_contract_later(
    const SolMirRuntimeCleanup *, const SolMirRuntimeCleanupPrecedenceInput *,
    const SolMirRuntimeCleanupAttemptedLater *,
    /* Caller-owned, allocation-free CFG workspace: at least twice the
     * materialized block count in size_t words. */
    size_t *reachability_scratch, size_t reachability_scratch_count,
    SolMirRuntimeCleanupPrecedenceResult *);
void sol_mir_runtime_cleanup_test_force_allocation_failure(bool);
void sol_mir_runtime_cleanup_test_force_build_scratch_failure(bool);
void sol_mir_runtime_cleanup_test_force_persistent_allocation_failure(bool);
void sol_mir_runtime_cleanup_test_force_validation_scratch_failure(bool);
/* A nonzero selector fails exactly that allocation attempt in the next build
 * or validation operation.  Cleanup has one build and one validation scratch
 * allocation; persistent arenas are selected in events/actions/transitions/
 * supplemental-sites order, skipping empty arenas. */
void sol_mir_runtime_cleanup_test_force_build_scratch_failure_attempt(size_t);
void sol_mir_runtime_cleanup_test_force_persistent_allocation_failure_attempt(size_t);
void sol_mir_runtime_cleanup_test_force_validation_scratch_failure_attempt(size_t);
size_t sol_mir_runtime_cleanup_test_build_scratch_attempts(void);
size_t sol_mir_runtime_cleanup_test_persistent_allocation_attempts(void);
size_t sol_mir_runtime_cleanup_test_validation_scratch_attempts(void);
bool sol_mir_runtime_cleanup_test_reconstruct_usage(const SolMirRuntimeConventions *,
    const SolMirRuntimeValues *, const SolMirRuntimeCleanupLimits *, SolMirRuntimeCleanupUsage *);
#endif
#endif
