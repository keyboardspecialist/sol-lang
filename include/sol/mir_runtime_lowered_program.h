#ifndef SOL_MIR_RUNTIME_LOWERED_PROGRAM_H
#define SOL_MIR_RUNTIME_LOWERED_PROGRAM_H

/* P3.6 is a static, backend-independent closure.  These rows name exact
 * predecessor coordinates only; none selects a physical ABI or executes. */
#include "sol/mir_runtime_handler_abi.h"

typedef enum {
    SOL_MIR_RUNTIME_LOWERED_PRESENT,
    SOL_MIR_RUNTIME_LOWERED_NOT_DEMANDED,
    SOL_MIR_RUNTIME_LOWERED_ERASED,
} SolMirRuntimeLoweredState;
typedef enum {
    SOL_MIR_RUNTIME_LOWERED_EXECUTABLE,
    SOL_MIR_RUNTIME_LOWERED_CONTROL_OR_MARKER,
} SolMirRuntimeLoweredExecution;
typedef enum {
    SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,
    SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL,
    SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER,
    SOL_MIR_RUNTIME_LOWERED_CLASS_ERASED,
} SolMirRuntimeLoweredRuntimeClass;
typedef enum {
    SOL_MIR_RUNTIME_LOWERED_PLAN_NONE,
    SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,
    SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL,
    SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC,
    SOL_MIR_RUNTIME_LOWERED_PLAN_CONSTRUCT,
    SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN,
    SOL_MIR_RUNTIME_LOWERED_PLAN_SNAPSHOT,
    SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE,
    SOL_MIR_RUNTIME_LOWERED_PLAN_PROPAGATION,
    SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE,
    SOL_MIR_RUNTIME_LOWERED_PLAN_HANDLER,
    SOL_MIR_RUNTIME_LOWERED_PLAN_IMPORT_SNAPSHOT,
    SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP,
} SolMirRuntimeLoweredPlanFamily;
typedef enum {
    SOL_MIR_RUNTIME_LOWERED_SEMANTIC_ACCESS,
    SOL_MIR_RUNTIME_LOWERED_SEMANTIC_CONSTRUCT,
    SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PATTERN_TEST,
    SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PATTERN_EXTRACTION,
    SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PROPAGATION,
    SOL_MIR_RUNTIME_LOWERED_SEMANTIC_ARITHMETIC,
    SOL_MIR_RUNTIME_LOWERED_SEMANTIC_SNAPSHOT,
    SOL_MIR_RUNTIME_LOWERED_SEMANTIC_CALLABLE,
    SOL_MIR_RUNTIME_LOWERED_SEMANTIC_HANDLER,
    SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PREDICATE,
    SOL_MIR_RUNTIME_LOWERED_SEMANTIC_IMPORT_SNAPSHOT,
} SolMirRuntimeLoweredSemanticArena;
enum {
    SOL_MIR_RUNTIME_LOWERED_FACILITY_CALL = 1u << 0,
    SOL_MIR_RUNTIME_LOWERED_FACILITY_SIGNATURE = 1u << 1,
    SOL_MIR_RUNTIME_LOWERED_FACILITY_IMPORT = 1u << 2,
    SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE = 1u << 3,
    SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE = 1u << 4,
    SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION = 1u << 5,
    SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY = 1u << 6,
    SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY = 1u << 7,
    SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP = 1u << 8,
    SOL_MIR_RUNTIME_LOWERED_FACILITY_HOST_RESULT = 1u << 9,
    SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP = 1u << 10,
    SOL_MIR_RUNTIME_LOWERED_FACILITY_EVENT = 1u << 11,
    SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE = 1u << 12,
    SOL_MIR_RUNTIME_LOWERED_FACILITY_HOST_GRANT = 1u << 13,
    SOL_MIR_RUNTIME_LOWERED_FACILITY_HANDLER_FRAME = 1u << 14,
};
#define SOL_MIR_RUNTIME_LOWERED_NONE SIZE_MAX
typedef struct {
    SolMirRuntimeLoweredRuntimeClass runtime_class;
    SolMirRuntimeLoweredPlanFamily plan_family;
    uint32_t facilities;
} SolMirRuntimeLoweredDemandDescriptor;
typedef enum {
    SOL_MIR_RUNTIME_LOWERED_CLEANUP_EVENT,
    SOL_MIR_RUNTIME_LOWERED_CLEANUP_ACTION,
    SOL_MIR_RUNTIME_LOWERED_CLEANUP_TRANSITION,
    SOL_MIR_RUNTIME_LOWERED_CLEANUP_SUPPLEMENTAL_SITE,
    SOL_MIR_RUNTIME_LOWERED_CLEANUP_DROP_PATH,
} SolMirRuntimeLoweredCleanupKind;

typedef struct { SolMirRuntimeLoweredState state; SolMirRuntimeLoweredExecution execution; SolMirPlanInstanceId image; SolMirMaterializedInstructionId instruction; SolMirMaterializedBlockId block; SolMirInstructionKind kind; SolMirRuntimeLoweredRuntimeClass runtime_class; SolMirRuntimeLoweredPlanFamily plan_family; uint32_t facilities; size_t plan; SolMirRuntimeSlice demanded_recipes; SolMirRuntimeCleanupEventId cleanup_event; SolMirRuntimeFailureSiteId failure_site; SolMirRuntimeCleanupEventId step_cleanup_event; SolMirRuntimeFailureSiteId step_failure_site; } SolMirRuntimeLoweredImageInstruction;
typedef struct { SolMirRuntimeLoweredState state; SolMirPlanInstanceId image; SolMirMaterializedBlockId block; SolMirRuntimeSlice incoming_edges; SolMirRuntimeSlice outgoing_edges; } SolMirRuntimeLoweredImageBlock;
typedef struct { SolMirRuntimeLoweredState state; SolMirPlanInstanceId image; SolMirMaterializedEdgeId edge; SolMirMaterializedBlockId target; SolMirMaterializedBlockId source; size_t ordinal; } SolMirRuntimeLoweredImageEdge;
typedef struct { SolMirRuntimeLoweredState state; SolMirPlanInstanceId image; SolMirMaterializedBlockId block; SolMirMaterializedEdgeId edge; size_t ordinal; } SolMirRuntimeLoweredImageBlockEdge;
typedef struct { SolMirRuntimeLoweredState state; SolMirRuntimeLoweredExecution execution; SolMirPlanInstanceId image; SolMirMaterializedBlockId block; SolMirTerminatorKind kind; SolMirRuntimeLoweredRuntimeClass runtime_class; SolMirRuntimeLoweredPlanFamily plan_family; uint32_t facilities; size_t plan; SolMirRuntimeSlice demanded_recipes; SolMirRuntimeCallId call; SolMirRuntimeCleanupEventId cleanup_event; SolMirRuntimeFailureSiteId failure_site; SolMirRuntimeCleanupEventId step_cleanup_event; SolMirRuntimeFailureSiteId step_failure_site; SolMirRuntimeCleanupEventId pre_operation_cleanup_event; SolMirRuntimeCleanupSupplementalSiteId pre_operation_supplemental_site; } SolMirRuntimeLoweredImageTerminator;
typedef struct { SolMirRuntimeLoweredState state; SolMirPredicateBodyId body; SolMirPredicateOwnerKind owner_kind; SolMirPlanInstanceId image; SolMirMaterializedImportId import_id; SolMirPlanContextId context; SolContractClauseKind phase; SolContractOutcomeKind outcome; SolMirRuntimeSlice blocks; SolMirPredicateBlockId entry; SolMirRecipeId output_recipe; SolMirRecipeId refinement_self_recipe; } SolMirRuntimeLoweredPredicateBody;
typedef struct { SolMirRuntimeLoweredState state; SolMirPredicateBodyId body; SolMirPredicateInstructionId instruction; SolMirPredicateBlockId block; SolMirPredicateInstructionKind kind; SolMirRuntimeLoweredRuntimeClass runtime_class; SolMirRuntimeLoweredPlanFamily plan_family; uint32_t facilities; size_t plan; SolMirRuntimeSlice demanded_recipes; SolMirRuntimeCleanupEventId cleanup_event; SolMirRuntimeFailureSiteId failure_site; SolMirRuntimeCleanupEventId step_cleanup_event; SolMirRuntimeFailureSiteId step_failure_site; } SolMirRuntimeLoweredPredicateInstruction;
typedef struct { SolMirRuntimeLoweredState state; SolMirPredicateBodyId body; SolMirPredicateBlockId block; SolMirRuntimeSlice incoming_edges; SolMirRuntimeSlice outgoing_edges; } SolMirRuntimeLoweredPredicateBlock;
typedef struct { SolMirRuntimeLoweredState state; SolMirPredicateBodyId body; SolMirPredicateEdgeId edge; SolMirPredicateBlockId target; SolMirPredicateBlockId source; size_t ordinal; } SolMirRuntimeLoweredPredicateEdge;
typedef struct { SolMirRuntimeLoweredState state; SolMirPredicateBodyId body; SolMirPredicateBlockId block; SolMirPredicateEdgeId edge; size_t ordinal; } SolMirRuntimeLoweredPredicateBlockEdge;
typedef struct { SolMirRuntimeLoweredState state; SolMirPredicateBodyId body; SolMirPredicateBlockId block; SolMirPredicateTerminatorKind kind; SolMirRuntimeLoweredRuntimeClass runtime_class; SolMirRuntimeLoweredPlanFamily plan_family; uint32_t facilities; size_t plan; SolMirRuntimeSlice demanded_recipes; SolMirRuntimeCallId call; SolMirRuntimeCleanupEventId cleanup_event; SolMirRuntimeFailureSiteId failure_site; SolMirRuntimeCleanupEventId step_cleanup_event; SolMirRuntimeFailureSiteId step_failure_site; } SolMirRuntimeLoweredPredicateTerminator;
typedef struct { SolMirRuntimeLoweredState state; SolMirRuntimeLoweredSemanticArena arena; SolMirRuntimeLoweredRuntimeClass runtime_class; SolMirRuntimeLoweredPlanFamily plan_family; size_t plan; uint32_t facilities; SolMirMaterializedProducerKind producer_kind; size_t producer; } SolMirRuntimeLoweredSemanticPlan;
typedef struct { SolMirRuntimeLoweredState state; SolMirRuntimeCallId call; SolMirRuntimeSignatureId signature; SolMirRuntimeCallOwnerKind owner_kind; SolMirPlanInstanceId image; SolMirPredicateBodyId body; size_t block; SolIrCallKind call_kind; SolMirRuntimeCallTargetKind target_kind; SolMirLinkageCallableId internal; SolMirLinkageHostRequirementId host; SolMirLinkageTableId table; SolMirRuntimeValueRef callee; SolMirRuntimeSlice operands; SolMirRuntimeValueRef result; size_t normal_edge; size_t failure_edge; SolMirRuntimeSlice writebacks; SolMirRuntimeImportId import_id; SolMirRuntimeImportId bound_environment_import; SolMirRuntimeEntryId entry; SolMirRuntimeFailureSiteId failure_site; } SolMirRuntimeLoweredCall;
typedef struct { SolMirRuntimeLoweredState state; SolMirRuntimeSignatureId signature; } SolMirRuntimeLoweredSignature;
typedef struct { SolMirRuntimeLoweredState state; SolMirRuntimeImportId import_id; } SolMirRuntimeLoweredImport;
typedef struct { SolMirRuntimeLoweredState state; SolMirRecipeId recipe; uint32_t demanded_operations; SolMirRuntimeRecipeOperationId operations; SolMirRuntimeImportId create_import; SolMirRuntimeImportId copy_import; SolMirRuntimeImportId drop_import; SolMirRuntimeImportId equal_import; uint32_t facilities; SolMirRuntimeAllocationPlanKind allocation; SolMirRuntimeCopyClass copy; SolMirRuntimeEqualityClass equality; SolMirRuntimeOwnershipClass ownership; SolMirRuntimeHostResultClass host_result; } SolMirRuntimeLoweredRecipe;
typedef struct { SolMirRuntimeLoweredState state; SolMirRecipeId recipe; SolMirRuntimeRecipeOperationId operations; uint32_t facilities; uint32_t demanded_facilities; SolMirRuntimeAllocationPlanKind allocation; SolMirRuntimeCopyClass copy; SolMirRuntimeEqualityClass equality; SolMirRuntimeOwnershipClass ownership; SolMirRuntimeHostResultClass host_result; } SolMirRuntimeLoweredValuePlan;
/* One authenticated P2 executable consumer to P3.2 recipe/value-plan use.
 * `ordinal` preserves source order, including repeated formal recipes. */
typedef struct { SolMirRuntimeLoweredState state; SolMirRecipeId recipe; size_t value_plan; uint32_t facilities; size_t ordinal; } SolMirRuntimeLoweredRecipeDemand;
typedef struct {
    SolMirRuntimeLoweredState state; SolMirRuntimeLoweredCleanupKind kind; size_t record;
    SolMirRuntimeCleanupEventId event; SolMirRuntimeCleanupEventKind event_kind;
    SolMirRuntimeCleanupOrigin origin; SolMirRuntimeCleanupPhase phase;
    size_t owner, block, operation, semantic_site;
    SolMirRuntimeCleanupProducerKind producer; SolMirRuntimeFailureSiteId inherited_failure_site;
    SolMirRuntimeCleanupSupplementalSiteId supplemental_site;
    SolMirRuntimeSlice actions, transitions;
    bool captures_failure_detail; SolMirRuntimeFailureDetailKind capture_detail_kind;
    SolMirRuntimeCleanupTransitionId transition; SolMirRuntimeCleanupActionId action;
    SolMirRuntimeCleanupActionKind action_kind; unsigned action_flags; size_t target;
    SolMirRecipeId recipe; SolMirRuntimeCleanupDropPathId drop_path;
    SolMirRuntimeHandlerFrameId frame;
    size_t continuation, source_edge, destination;
    bool primary_failure_wins; SolMirRuntimeCleanupFailureSource failure_source;
    size_t failure_site; uint32_t failure_mask;
    SolMirRuntimeCleanupEdgeRole edge_role; SolMirRuntimeCleanupOutcome outcome;
    SolContractClauseKind contract_phase; SolContractOutcomeKind contract_outcome;
    SolMirMaterializedPlaceId drop_root, drop_place; SolMirRuntimeSlice drop_holes;
    SolMirRuntimeCleanupDropLiveness drop_liveness;
} SolMirRuntimeLoweredCleanupFailure;
typedef struct { SolMirRuntimeLoweredState state; SolMirRuntimeHostAbiRequirementId requirement; SolMirRuntimeEntryId entry; SolMirRuntimeHostAbiEntryRootId root; SolMirRuntimeHostAbiOperationId operation; SolMirRuntimeCallId call; SolMirRuntimeCleanupEventId event; SolMirRuntimeCleanupTransitionId transition; SolMirRuntimeImportId import_id; SolMirRuntimeSignatureId signature; } SolMirRuntimeLoweredHostRequirement;
typedef struct { SolMirRuntimeLoweredState state; SolMirRuntimeEntryId entry; SolMirRuntimeHostAbiEntryRootId root; SolMirRuntimeHostAbiOperationId operation; SolMirRuntimeSlice incidences; } SolMirRuntimeLoweredHostGrant;
typedef struct { SolMirRuntimeLoweredState state; SolMirRuntimeHostAbiRequirementId requirement; size_t grant; } SolMirRuntimeLoweredHostIncidence;
typedef struct { SolMirRuntimeLoweredState state; SolMirRuntimeHandlerFrameId frame; SolMirMaterializedHandlerId handler; SolMirRuntimeHandlerFrameId parent; SolMirMaterializedBindingId source_binding; SolMirMaterializedOperationKey source_operation; SolMirRuntimeSignatureId source_signature; SolMirOperationRootMatchRule root_match; SolMirMaterializedPlaceId root; SolMirMaterializedEffectRowId effects; SolMirMaterializedBindingId provider_binding; SolMirMaterializedPlaceId provider_place; SolMirRecipeId provider_recipe; SolMirOperationAccessId provider_access; SolMirLinkageCallableId provider; SolMirRuntimeSignatureId signature; SolMirMaterializedInstructionId enter; } SolMirRuntimeLoweredHandlerFrame;
typedef struct { SolMirRuntimeLoweredState state; SolMirRuntimeHandlerFrameId frame; SolMirMaterializedInstructionId marker; SolMirPlanInstanceId image; SolMirMaterializedBlockId block; } SolMirRuntimeLoweredHandlerMarker;
typedef struct { SolMirRuntimeLoweredState state; SolMirRuntimeHandlerFrameId frame; SolMirRuntimeCleanupActionId action; SolMirRuntimeCleanupTransitionId transition; bool alternative_one_pop; } SolMirRuntimeLoweredHandlerExit;

/* Only executable semantic plan arenas.  Supporting nodes/operands remain
 * owned by P2 and are reached through their exact parent plan. */
#define SOL_MIR_RUNTIME_LOWERED_SEMANTIC_ARENAS(X) \
 X(access_plans, SOL_MIR_RUNTIME_LOWERED_SEMANTIC_ACCESS, SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE) \
 X(constructors, SOL_MIR_RUNTIME_LOWERED_SEMANTIC_CONSTRUCT, SOL_MIR_RUNTIME_LOWERED_PLAN_CONSTRUCT) \
 X(pattern_tests, SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PATTERN_TEST, SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN) \
 X(pattern_extractions, SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PATTERN_EXTRACTION, SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN) \
 X(propagations, SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PROPAGATION, SOL_MIR_RUNTIME_LOWERED_PLAN_PROPAGATION) \
 X(arithmetic, SOL_MIR_RUNTIME_LOWERED_SEMANTIC_ARITHMETIC, SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC) \
 X(snapshots, SOL_MIR_RUNTIME_LOWERED_SEMANTIC_SNAPSHOT, SOL_MIR_RUNTIME_LOWERED_PLAN_SNAPSHOT) \
 X(callables, SOL_MIR_RUNTIME_LOWERED_SEMANTIC_CALLABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE) \
 X(handlers, SOL_MIR_RUNTIME_LOWERED_SEMANTIC_HANDLER, SOL_MIR_RUNTIME_LOWERED_PLAN_HANDLER) \
 X(predicates, SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PREDICATE, SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE) \
 X(import_snapshots, SOL_MIR_RUNTIME_LOWERED_SEMANTIC_IMPORT_SNAPSHOT, SOL_MIR_RUNTIME_LOWERED_PLAN_IMPORT_SNAPSHOT)

#define SOL_MIR_RUNTIME_LOWERED_TABLES(X) \
 X(image_instructions, SolMirRuntimeLoweredImageInstruction, image_instruction) \
  X(image_blocks, SolMirRuntimeLoweredImageBlock, image_block) \
  X(image_edges, SolMirRuntimeLoweredImageEdge, image_edge) \
  X(image_incoming_edges, SolMirRuntimeLoweredImageBlockEdge, image_incoming_edge) \
  X(image_outgoing_edges, SolMirRuntimeLoweredImageBlockEdge, image_outgoing_edge) \
 X(image_terminators, SolMirRuntimeLoweredImageTerminator, image_terminator) \
 X(predicate_bodies, SolMirRuntimeLoweredPredicateBody, predicate_body) \
 X(predicate_instructions, SolMirRuntimeLoweredPredicateInstruction, predicate_instruction) \
  X(predicate_blocks, SolMirRuntimeLoweredPredicateBlock, predicate_block) \
  X(predicate_edges, SolMirRuntimeLoweredPredicateEdge, predicate_edge) \
  X(predicate_incoming_edges, SolMirRuntimeLoweredPredicateBlockEdge, predicate_incoming_edge) \
  X(predicate_outgoing_edges, SolMirRuntimeLoweredPredicateBlockEdge, predicate_outgoing_edge) \
 X(predicate_terminators, SolMirRuntimeLoweredPredicateTerminator, predicate_terminator) \
 X(semantic_plans, SolMirRuntimeLoweredSemanticPlan, semantic_plan) \
 X(calls, SolMirRuntimeLoweredCall, call) X(signatures, SolMirRuntimeLoweredSignature, signature) \
  X(imports, SolMirRuntimeLoweredImport, import) X(recipes, SolMirRuntimeLoweredRecipe, recipe) \
  X(value_plans, SolMirRuntimeLoweredValuePlan, value_plan) \
  X(recipe_demands, SolMirRuntimeLoweredRecipeDemand, recipe_demand) \
 X(cleanup_failures, SolMirRuntimeLoweredCleanupFailure, cleanup_failure) \
 X(host_requirements, SolMirRuntimeLoweredHostRequirement, host_requirement) \
 X(host_grants, SolMirRuntimeLoweredHostGrant, host_grant) \
 X(host_incidences, SolMirRuntimeLoweredHostIncidence, host_incidence) \
 X(handler_frames, SolMirRuntimeLoweredHandlerFrame, handler_frame) \
 X(handler_markers, SolMirRuntimeLoweredHandlerMarker, handler_marker) \
 X(handler_exits, SolMirRuntimeLoweredHandlerExit, handler_exit)

typedef struct {
#define SOL_MIR_RUNTIME_LOWERED_LIMIT(member,type,singular) size_t max_##member;
    SOL_MIR_RUNTIME_LOWERED_TABLES(SOL_MIR_RUNTIME_LOWERED_LIMIT)
#undef SOL_MIR_RUNTIME_LOWERED_LIMIT
    size_t max_owned_bytes, max_build_scratch_bytes, max_build_work;
    size_t max_validation_scratch_bytes, max_validation_work;
    /* Rendering is independently bounded: it must never borrow the owner or
     * build limits as an implicit output/sort-allocation budget. */
    size_t max_render_bytes, max_render_scratch_bytes;
} SolMirRuntimeLoweredProgramLimits;
typedef struct {
#define SOL_MIR_RUNTIME_LOWERED_USAGE(member,type,singular) size_t member;
    SOL_MIR_RUNTIME_LOWERED_TABLES(SOL_MIR_RUNTIME_LOWERED_USAGE)
#undef SOL_MIR_RUNTIME_LOWERED_USAGE
    size_t erased_loops, provenance_records, demanded_semantic_plans;
    size_t owned_bytes, build_scratch_bytes, build_work;
    size_t validation_scratch_bytes, validation_work;
    size_t render_bytes, render_scratch_bytes;
} SolMirRuntimeLoweredProgramUsage;
typedef struct {
    const SolMirRuntimeConventions *conventions; const SolMirRuntimeValues *values;
    const SolMirRuntimeCleanup *cleanup; const SolMirRuntimeHostAbi *host_abi;
    const SolMirRuntimeHandlerAbi *handler_abi;
#define SOL_MIR_RUNTIME_LOWERED_MEMBER(member,type,singular) type *member; size_t singular##_count, singular##_capacity;
    SOL_MIR_RUNTIME_LOWERED_TABLES(SOL_MIR_RUNTIME_LOWERED_MEMBER)
#undef SOL_MIR_RUNTIME_LOWERED_MEMBER
    SolMirRuntimeLoweredProgramLimits limits; SolMirRuntimeLoweredProgramUsage usage;
    uint64_t authentication;
} SolMirRuntimeLoweredProgram;
typedef struct { const SolMirRuntimeConventions *conventions; const SolMirRuntimeValues *values; const SolMirRuntimeCleanup *cleanup; const SolMirRuntimeHostAbi *host_abi; const SolMirRuntimeHandlerAbi *handler_abi; const SolMirRuntimeLoweredProgramLimits *limits; } SolMirRuntimeLoweredProgramBuildRequest;
typedef enum { SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED, SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_INVALID_ARGUMENT, SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_INVALID_PREDECESSOR, SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_UNSUPPORTED, SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_RESOURCE_EXHAUSTED, SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_ALLOCATION_FAILED, SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_INTERNAL_FAILED } SolMirRuntimeLoweredProgramBuildOutcome;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
/* The most recent build's deterministic census/write accounting.  These are
 * diagnostics for metering tests, not part of the serialized owner. */
typedef struct {
    size_t census_work;
    size_t draft_work;
    size_t reserved_persistent_work;
    size_t actual_work;
    size_t build_scratch_bytes;
} SolMirRuntimeLoweredProgramTestBuildWork;
#endif
void sol_mir_runtime_lowered_program_init(SolMirRuntimeLoweredProgram *);
void sol_mir_runtime_lowered_program_free(SolMirRuntimeLoweredProgram *);
SolMirRuntimeLoweredProgramLimits sol_mir_runtime_lowered_program_default_limits(void);
SolMirRuntimeLoweredProgramBuildOutcome sol_mir_runtime_lowered_program_build(const SolMirRuntimeLoweredProgramBuildRequest *, SolMirRuntimeLoweredProgram *, SolDiagnostics *);
bool sol_mir_runtime_lowered_program_validate(const SolMirRuntimeLoweredProgram *, SolDiagnostics *);
bool sol_mir_runtime_lowered_program_render(FILE *, const SolMirRuntimeLoweredProgram *);
#ifdef SOL_MIR_PLAN_TEST_HOOKS
SolMirRuntimeLoweredProgramTestBuildWork sol_mir_runtime_lowered_program_test_last_build_work(void);
bool sol_mir_runtime_lowered_program_test_image_instruction(SolMirInstructionKind, SolMirRuntimeLoweredExecution *);
bool sol_mir_runtime_lowered_program_test_image_instruction_descriptor(SolMirInstructionKind, SolMirRuntimeLoweredDemandDescriptor *);
bool sol_mir_runtime_lowered_program_test_image_terminator(SolMirTerminatorKind, SolMirRuntimeLoweredExecution *);
bool sol_mir_runtime_lowered_program_test_image_terminator_descriptor(SolMirTerminatorKind, SolMirRuntimeLoweredDemandDescriptor *);
bool sol_mir_runtime_lowered_program_test_predicate_instruction(SolMirPredicateInstructionKind);
bool sol_mir_runtime_lowered_program_test_predicate_instruction_descriptor(SolMirPredicateInstructionKind, SolMirRuntimeLoweredDemandDescriptor *);
bool sol_mir_runtime_lowered_program_test_predicate_terminator(SolMirPredicateTerminatorKind);
bool sol_mir_runtime_lowered_program_test_predicate_terminator_descriptor(SolMirPredicateTerminatorKind, SolMirRuntimeLoweredDemandDescriptor *);
bool sol_mir_runtime_lowered_program_test_provenance(SolMirOperationProvenanceKind);
bool sol_mir_runtime_lowered_program_test_provenance_descriptor(SolMirOperationProvenanceKind, SolMirRuntimeLoweredDemandDescriptor *);
bool sol_mir_runtime_lowered_program_test_call_facilities(SolMirRuntimeCallTargetKind, SolMirRuntimeSignatureOrigin, bool, uint32_t *);
uint32_t sol_mir_runtime_lowered_program_test_recipe_facilities(uint32_t);
uint32_t sol_mir_runtime_lowered_program_test_value_facilities(SolMirRuntimeAllocationPlanKind, SolMirRuntimeCopyClass, SolMirRuntimeEqualityClass, SolMirRuntimeOwnershipClass, SolMirRuntimeHostResultClass, bool);
bool sol_mir_runtime_lowered_program_test_copy_requires_runtime(SolMirCopyKind);
uint32_t sol_mir_runtime_lowered_program_test_arithmetic_facilities(unsigned);
bool sol_mir_runtime_lowered_program_test_predicate_continuation_ordinal(SolMirPredicateTerminatorKind, bool, size_t *);
bool sol_mir_runtime_lowered_program_test_predicate_legacy_edge_rejected(SolMirPredicateTerminatorKind);
uint64_t sol_mir_runtime_lowered_program_test_seal(const SolMirRuntimeLoweredProgram *);
bool sol_mir_runtime_lowered_program_test_place_key(const SolMirRuntimeLoweredProgram *, size_t, char[65]);
void sol_mir_runtime_lowered_program_test_force_build_scratch_allocation_failure(bool);
void sol_mir_runtime_lowered_program_test_force_persistent_allocation_failure(bool);
void sol_mir_runtime_lowered_program_test_force_validation_scratch_failure(bool);
void sol_mir_runtime_lowered_program_test_force_render_allocation_failure(bool);
void sol_mir_runtime_lowered_program_test_reset_render_write_attempts(void);
size_t sol_mir_runtime_lowered_program_test_render_write_attempts(void);
size_t sol_mir_runtime_lowered_program_test_allocation_attempts(void);
size_t sol_mir_runtime_lowered_program_test_build_scratch_allocation_attempts(void);
size_t sol_mir_runtime_lowered_program_test_persistent_allocation_attempts(void);
void sol_mir_runtime_lowered_program_test_reset_allocation_attempts(void);
void sol_mir_runtime_lowered_program_test_fail_build_scratch_allocation_after(size_t);
void sol_mir_runtime_lowered_program_test_fail_persistent_allocation_after(size_t);
#endif
#endif
