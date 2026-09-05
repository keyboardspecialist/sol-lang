#ifndef SOL_MIR_OPERATIONS_H
#define SOL_MIR_OPERATIONS_H

#include "sol/mir_layout.h"

typedef size_t SolMirOperationAccessId;
typedef size_t SolMirPredicateBodyId;
typedef size_t SolMirPredicateBlockId;
typedef size_t SolMirPredicateInputId;
typedef size_t SolMirPredicateValueId;
typedef size_t SolMirPredicateInstructionId;
#define SOL_MIR_OPERATION_NONE SIZE_MAX

typedef struct {
    size_t projection;
    SolMirRecipeId base_recipe;
    SolMirRecipeId result_recipe;
    size_t field_layout;
    uint64_t object_offset;
} SolMirOperationAccessStep;

typedef struct {
    SolMirMaterializedPlaceId place;
    SolMirPlanInstanceId image;
    SolMirMaterializedLocalId local;
    SolMirRecipeId root_recipe;
    SolMirRecipeId final_recipe;
    SolMirPlanSlice steps;
} SolMirOperationAccessPlan;

typedef enum {
    SOL_MIR_OPERATION_CONSTRUCT_RECORD,
    SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY,
    SOL_MIR_OPERATION_CONSTRUCT_TUPLE,
    SOL_MIR_OPERATION_CONSTRUCT_SUM,
    SOL_MIR_OPERATION_CONSTRUCT_WRAPPER,
} SolMirOperationConstructKind;

typedef enum {
    SOL_MIR_OPERATION_CAPABILITY_NONE,
    SOL_MIR_OPERATION_CAPABILITY_ROOT_SOURCE,
    SOL_MIR_OPERATION_CAPABILITY_BASE_SOURCE,
    SOL_MIR_OPERATION_CAPABILITY_PRIVATE_SOURCE,
} SolMirOperationCapabilityRule;

typedef struct {
    size_t formal_ordinal;
    size_t source_operand_ordinal;
    SolMirMaterializedTemporaryId temporary;
    SolMirRecipeId recipe;
    size_t recipe_field;
    size_t layout_field;
    uint64_t absolute_offset;
} SolMirOperationConstructOperand;

typedef struct {
    SolMirPlanInstanceId image;
    SolMirMaterializedInstructionId instruction;
    SolMirMaterializedValueId result;
    SolMirOperationConstructKind kind;
    SolMirRecipeId result_recipe;
    SolMirLayoutObjectKind object_kind;
    size_t variant_layout;
    uint32_t semantic_tag;
    SolMirPlanSlice operands;
    SolMirRecipeId wrapper_backing;
    SolMirOperationCapabilityRule capability_rule;
    size_t capability_source_operand;
    SolMirMaterializedLocalId inherited_root;
} SolMirOperationConstructPlan;

typedef enum {
    SOL_MIR_OPERATION_PATTERN_WILDCARD,
    SOL_MIR_OPERATION_PATTERN_BINDING,
    SOL_MIR_OPERATION_PATTERN_BOOL,
    SOL_MIR_OPERATION_PATTERN_SUM_TAG,
    SOL_MIR_OPERATION_PATTERN_PRODUCT,
} SolMirOperationPatternKind;

typedef struct {
    SolMirRecipeId base_recipe;
    SolMirRecipeId result_recipe;
    size_t field_layout;
    uint64_t object_offset;
} SolMirOperationPathStep;

typedef struct {
    SolMirOperationPatternKind kind;
    SolMirRecipeId recipe;
    SolMirPlanSlice path;
    uint32_t semantic_tag;
    bool boolean;
} SolMirOperationPatternNode;

typedef struct {
    SolMirPlanInstanceId image;
    SolMirMaterializedInstructionId instruction;
    SolMirMaterializedTemporaryId scrutinee;
    SolMirRecipeId scrutinee_recipe;
    SolMirMaterializedValueId result;
    SolMirPlanSlice nodes;
} SolMirOperationPatternTest;

typedef struct {
    SolMirPlanInstanceId image;
    SolMirMaterializedInstructionId instruction;
    SolMirMaterializedTemporaryId scrutinee;
    SolMirRecipeId scrutinee_recipe;
    SolMirMaterializedValueId result;
    SolMirRecipeId result_recipe;
    SolMirPlanSlice path;
    SolMirCopyKind copy_kind;
} SolMirOperationPatternExtraction;

typedef struct {
    SolMirPlanInstanceId image;
    SolMirMaterializedBlockId block;
    SolMirMaterializedTemporaryId source;
    SolMirRecipeId source_recipe;
    SolMirMaterializedValueId success_result;
    SolMirRecipeId success_recipe;
    SolMirMaterializedValueId residual_result;
    SolMirRecipeId residual_recipe;
    size_t success_variant_layout;
    uint32_t success_tag;
    size_t source_residual_variant_layout;
    uint32_t source_residual_tag;
    size_t destination_residual_variant_layout;
    uint32_t destination_residual_tag;
    size_t success_field_layout;
    uint64_t success_field_offset;
    size_t source_residual_field_layout;
    SolMirRecipeId source_residual_field_recipe;
    uint64_t source_residual_field_offset;
    size_t destination_residual_field_layout;
    SolMirRecipeId destination_residual_field_recipe;
    uint64_t destination_residual_field_offset;
    SolMirMaterializedEdgeId success_edge;
    SolMirMaterializedEdgeId residual_edge;
} SolMirOperationPropagationPlan;

typedef enum {
    SOL_MIR_OPERATION_BOOL_NOT,
    SOL_MIR_OPERATION_I64_NEG,
    SOL_MIR_OPERATION_I64_ADD,
    SOL_MIR_OPERATION_I64_SUB,
    SOL_MIR_OPERATION_I64_MUL,
    SOL_MIR_OPERATION_I64_DIV,
    SOL_MIR_OPERATION_I64_REM,
    SOL_MIR_OPERATION_I64_LT,
    SOL_MIR_OPERATION_I64_LE,
    SOL_MIR_OPERATION_I64_GT,
    SOL_MIR_OPERATION_I64_GE,
    SOL_MIR_OPERATION_BOOL_AND,
    SOL_MIR_OPERATION_BOOL_OR,
    SOL_MIR_OPERATION_VALUE_EQ,
    SOL_MIR_OPERATION_VALUE_NE,
} SolMirOperationOpcode;

enum {
    SOL_MIR_OPERATION_FAILURE_NONE = 0,
    SOL_MIR_OPERATION_FAILURE_OVERFLOW = 1,
    SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO = 2,
};

typedef enum {
    SOL_MIR_OPERATION_EQUAL_SCALAR,
    SOL_MIR_OPERATION_EQUAL_TEXT,
    SOL_MIR_OPERATION_EQUAL_PRODUCT,
    SOL_MIR_OPERATION_EQUAL_SUM,
    SOL_MIR_OPERATION_EQUAL_WRAPPER,
} SolMirOperationEqualityKind;

typedef struct {
    SolMirRecipeId recipe;
    SolMirOperationEqualityKind kind;
    SolMirPlanSlice children;
} SolMirOperationEqualityNode;

typedef struct {
    SolMirRecipeId recipe;
    size_t field_layout;
    size_t variant_layout;
    uint32_t semantic_tag;
} SolMirOperationEqualityChild;

typedef struct {
    SolMirPlanInstanceId image;
    SolMirMaterializedInstructionId instruction;
    SolMirOperationOpcode opcode;
    SolMirMaterializedValueId left;
    SolMirMaterializedValueId right;
    SolMirMaterializedTemporaryId previous;
    SolMirRecipeId operand_recipe;
    SolMirMaterializedValueId result;
    SolMirRecipeId result_recipe;
    unsigned failures;
    bool compound;
    SolMirPlanSlice equality;
} SolMirOperationArithmeticPlan;

typedef struct {
    SolMirPlanInstanceId image;
    SolMirMaterializedInstructionId instruction;
    size_t slot;
    SolMirPlanContextId context;
    SolMirOperationAccessId access;
    SolMirMaterializedLocalId local;
    SolMirRecipeId root_recipe;
    SolMirPlanSlice path;
    SolMirRecipeId recipe;
    SolMirCopyKind copy_kind;
    size_t provenance;
} SolMirOperationSnapshotPlan;

typedef enum {
    SOL_MIR_OPERATION_CAPTURE_NONE,
    SOL_MIR_OPERATION_CAPTURE_PLACE,
    SOL_MIR_OPERATION_CAPTURE_TEMPORARY,
    SOL_MIR_OPERATION_CAPTURE_VALUE,
} SolMirOperationCaptureKind;

typedef struct {
    size_t semantic_site;
    SolMirCallableProducerKind kind;
    SolMirRecipeId function_recipe;
    SolMirMaterializedTargetKind target_kind;
    SolMirPlanInstanceId target_instance;
    SolMirMaterializedImportId target_import;
    SolMirOperationCaptureKind capture_kind;
    SolMirOperationAccessId capture_access;
    SolMirMaterializedTemporaryId capture_temporary;
    SolMirMaterializedValueId capture_value;
    SolMirMaterializedInstructionId capture_instruction;
    SolMirRecipeId capture_recipe;
    SolMirMaterializedEffectRowId effects;
    SolMirPlanSlice roots;
} SolMirOperationCallablePlan;

typedef enum { SOL_MIR_OPERATION_ROOT_TOKEN_EQUAL } SolMirOperationRootMatchRule;

typedef struct {
    SolMirMaterializedHandlerId handler;
    SolMirPlanInstanceId image;
    size_t frame_parent;
    SolMirMaterializedBindingId source_binding;
    SolMirMaterializedBindingId provider_binding;
    SolMirOperationAccessId authority_access;
    SolMirOperationAccessId provider_access;
    SolMirMaterializedOperationKey operation;
    SolMirRecipeId receiver_recipe;
    SolMirPlanSlice parameter_recipes;
    SolMirRecipeId result_recipe;
    SolMirOperationRootMatchRule root_match;
    SolMirMaterializedEffectRowId effects;
} SolMirOperationHandlerPlan;

typedef enum {
    SOL_MIR_OPERATION_PREDICATE_CONTRACT,
    SOL_MIR_OPERATION_PREDICATE_REFINEMENT,
} SolMirOperationPredicateKind;

typedef struct {
    SolMirOperationPredicateKind kind;
    SolMirPredicateBodyId body;
    SolMirPlanInstanceId image;
    SolMirMaterializedBlockId block;
    SolMirPlanContextId context;
    SolMirMaterializedTemporaryId representation;
    SolMirRecipeId input_recipe;
    SolMirMaterializedValueId result;
    SolMirRecipeId result_recipe;
    SolMirRecipeId output_recipe;
    SolContractClauseKind contract_phase;
    SolContractOutcomeKind contract_outcome;
    size_t provenance;
} SolMirOperationPredicatePlan;

typedef enum {
    SOL_MIR_PREDICATE_INPUT_RECEIVER,
    SOL_MIR_PREDICATE_INPUT_PRIVATE_SOURCE,
    SOL_MIR_PREDICATE_INPUT_PARAMETER,
    SOL_MIR_PREDICATE_INPUT_COMPLETE_RESULT,
    SOL_MIR_PREDICATE_INPUT_SUCCESS_RESULT,
    SOL_MIR_PREDICATE_INPUT_SNAPSHOT,
    SOL_MIR_PREDICATE_INPUT_REFINEMENT_SELF,
} SolMirPredicateInputKind;

typedef struct {
    SolMirPredicateInputKind kind;
    size_t ordinal;
    SolMirRecipeId recipe;
    SolAccessMode access;
} SolMirPredicateInput;

typedef enum {
    SOL_MIR_PREDICATE_VALUE_INPUT,
    SOL_MIR_PREDICATE_VALUE_INSTRUCTION,
} SolMirPredicateValueKind;

typedef struct {
    SolMirPredicateValueKind kind;
    SolMirRecipeId recipe;
    SolMirPredicateBlockId block;
    size_t definition;
} SolMirPredicateValue;

typedef enum {
    SOL_MIR_PREDICATE_INST_I64,
    SOL_MIR_PREDICATE_INST_BOOL,
    SOL_MIR_PREDICATE_INST_TEXT,
    SOL_MIR_PREDICATE_INST_UNIT,
    SOL_MIR_PREDICATE_INST_UNARY,
    SOL_MIR_PREDICATE_INST_BINARY,
} SolMirPredicateInstructionKind;

typedef struct {
    SolMirPredicateInstructionKind kind;
    SolMirPredicateBlockId block;
    SolMirPredicateValueId result;
    SolMirRecipeId recipe;
    SolMirOperationOpcode opcode;
    SolMirPredicateValueId left;
    SolMirPredicateValueId right;
    int64_t integer;
    bool boolean;
    SolMirPlanSlice bytes;
    unsigned failures;
} SolMirPredicateInstruction;

typedef enum { SOL_MIR_PREDICATE_TERM_RETURN } SolMirPredicateTerminatorKind;
typedef struct {
    SolMirPredicateTerminatorKind kind;
    SolMirPredicateValueId value;
} SolMirPredicateTerminator;

typedef struct {
    SolMirPredicateBodyId body;
    SolMirPlanSlice instructions;
    SolMirPredicateTerminator terminator;
} SolMirPredicateBlock;

typedef enum {
    SOL_MIR_PREDICATE_OWNER_INSTANCE,
    SOL_MIR_PREDICATE_OWNER_IMPORT,
} SolMirPredicateOwnerKind;

typedef struct {
    SolMirPredicateOwnerKind owner_kind;
    SolMirPlanInstanceId instance;
    SolMirMaterializedImportId import;
    SolMirPlanContextId context;
    SolContractClauseKind phase;
    SolContractOutcomeKind outcome;
    SolMirPlanSlice inputs;
    SolMirPlanSlice blocks;
    SolMirPlanSlice values;
    SolMirPredicateBlockId entry;
    SolMirRecipeId output_recipe;
} SolMirPredicateBody;

typedef struct {
    SolMirMaterializedImportId import;
    SolMirRecipeId receiver;
    SolAccessMode receiver_access;
    SolMirPlanSlice parameters;
    SolMirPlanSlice parameter_accesses;
    SolMirRecipeId result;
    SolMirMaterializedEffectRowId effects;
    SolMirPlanSlice requires;
    SolMirPlanSlice snapshots;
    SolMirPlanSlice ensures;
    bool host_invoke;
} SolMirImportContractEnvelope;

typedef struct {
    SolMirMaterializedImportId import;
    SolMirPlanContextId context;
    size_t slot;
    SolMirPredicateInputKind input_kind;
    size_t ordinal;
    SolMirRecipeId recipe;
    SolAccessMode access;
    size_t provenance;
} SolMirImportSnapshotCapture;

typedef enum {
    SOL_MIR_OPERATION_PROVENANCE_CONSTRUCT,
    SOL_MIR_OPERATION_PROVENANCE_PATTERN_TEST,
    SOL_MIR_OPERATION_PROVENANCE_PATTERN_EXTRACTION,
    SOL_MIR_OPERATION_PROVENANCE_ARITHMETIC,
    SOL_MIR_OPERATION_PROVENANCE_PROPAGATION,
    SOL_MIR_OPERATION_PROVENANCE_SNAPSHOT,
    SOL_MIR_OPERATION_PROVENANCE_PREDICATE,
    SOL_MIR_OPERATION_PROVENANCE_HANDLER,
    SOL_MIR_OPERATION_PROVENANCE_CALLABLE,
    SOL_MIR_OPERATION_PROVENANCE_IMPORT_SNAPSHOT,
} SolMirOperationProvenanceKind;

typedef struct {
    SolMirOperationProvenanceKind kind;
    size_t executable;
    size_t source_expression;
    size_t source_pattern;
    size_t source_field;
    size_t source_variant;
    size_t source_obligation;
    size_t source_snapshot;
} SolMirOperationProvenance;

typedef struct {
    size_t max_access_plans, max_access_steps;
    size_t max_constructors, max_construct_operands;
    size_t max_pattern_tests, max_pattern_extractions;
    size_t max_pattern_nodes, max_path_steps;
    size_t max_propagations, max_arithmetic, max_equality_nodes;
    size_t max_equality_children;
    size_t max_snapshots, max_callables, max_handlers;
    size_t max_predicates, max_recipe_ids, max_roots, max_provenance;
    size_t max_predicate_bodies, max_predicate_blocks, max_predicate_inputs;
    size_t max_predicate_values, max_predicate_instructions;
    size_t max_import_envelopes, max_import_contract_references;
    size_t max_import_snapshots;
    size_t max_literal_bytes;
    size_t max_owned_bytes, max_build_scratch_bytes, max_build_work;
    size_t max_validation_scratch_bytes, max_validation_work;
} SolMirOperationsLimits;

typedef struct {
    size_t access_plans, access_steps, constructors, construct_operands;
    size_t pattern_tests, pattern_extractions, pattern_nodes, path_steps;
    size_t propagations, arithmetic, equality_nodes, equality_children;
    size_t snapshots, callables;
    size_t handlers, predicates, recipe_ids, roots, provenance;
    size_t predicate_bodies, predicate_blocks, predicate_inputs;
    size_t predicate_values, predicate_instructions;
    size_t import_envelopes, import_contract_references, literal_bytes;
    size_t import_snapshots;
    size_t owned_bytes, build_scratch_bytes, build_work;
    size_t validation_scratch_bytes, validation_work;
} SolMirOperationsUsage;

#define SOL_MIR_OPERATIONS_ARENAS(X) \
    X(access_plans, SolMirOperationAccessPlan, access_plan) \
    X(access_steps, SolMirOperationAccessStep, access_step) \
    X(constructors, SolMirOperationConstructPlan, constructor) \
    X(construct_operands, SolMirOperationConstructOperand, construct_operand) \
    X(pattern_tests, SolMirOperationPatternTest, pattern_test) \
    X(pattern_extractions, SolMirOperationPatternExtraction, pattern_extraction) \
    X(pattern_nodes, SolMirOperationPatternNode, pattern_node) \
    X(path_steps, SolMirOperationPathStep, path_step) \
    X(propagations, SolMirOperationPropagationPlan, propagation) \
    X(arithmetic, SolMirOperationArithmeticPlan, arithmetic) \
    X(equality_nodes, SolMirOperationEqualityNode, equality_node) \
    X(equality_children, SolMirOperationEqualityChild, equality_child) \
    X(snapshots, SolMirOperationSnapshotPlan, snapshot) \
    X(callables, SolMirOperationCallablePlan, callable) \
    X(handlers, SolMirOperationHandlerPlan, handler) \
    X(predicates, SolMirOperationPredicatePlan, predicate) \
    X(predicate_bodies, SolMirPredicateBody, predicate_body) \
    X(predicate_blocks, SolMirPredicateBlock, predicate_block) \
    X(predicate_inputs, SolMirPredicateInput, predicate_input) \
    X(predicate_values, SolMirPredicateValue, predicate_value) \
    X(predicate_instructions, SolMirPredicateInstruction, predicate_instruction) \
    X(import_envelopes, SolMirImportContractEnvelope, import_envelope) \
    X(import_contract_references, SolMirPredicateBodyId, import_contract_reference) \
    X(import_snapshots, SolMirImportSnapshotCapture, import_snapshot) \
    X(literal_bytes, char, literal_byte) \
    X(recipe_ids, SolMirRecipeId, recipe_id) \
    X(roots, SolMirMaterializedLocalId, root) \
    X(provenance, SolMirOperationProvenance, provenance)

typedef struct {
    const SolMirLayout *layout;
#define SOL_MIR_OP_MEMBER(member, type, singular) \
    type *member; size_t singular##_count, singular##_capacity;
    SOL_MIR_OPERATIONS_ARENAS(SOL_MIR_OP_MEMBER)
#undef SOL_MIR_OP_MEMBER
    SolMirOperationsLimits limits;
    SolMirOperationsUsage usage;
} SolMirOperations;

typedef struct {
    const SolMirLayout *layout;
    /* NULL or wholly zero selects defaults. Partial zero is invalid. */
    const SolMirOperationsLimits *limits;
} SolMirOperationsBuildRequest;

typedef enum {
    SOL_MIR_OPERATIONS_BUILD_SUCCEEDED,
    SOL_MIR_OPERATIONS_BUILD_INVALID_ARGUMENT,
    SOL_MIR_OPERATIONS_BUILD_INVALID_LAYOUT,
    SOL_MIR_OPERATIONS_BUILD_UNSUPPORTED,
    SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED,
    SOL_MIR_OPERATIONS_BUILD_ALLOCATION_FAILED,
    SOL_MIR_OPERATIONS_BUILD_INTERNAL_FAILED,
} SolMirOperationsBuildOutcome;

void sol_mir_operations_init(SolMirOperations *operations);
void sol_mir_operations_free(SolMirOperations *operations);
SolMirOperationsLimits sol_mir_operations_default_limits(void);
SolMirOperationsBuildOutcome sol_mir_operations_build(
    const SolMirOperationsBuildRequest *request,
    SolMirOperations *operations,
    SolDiagnostics *diagnostics
);
bool sol_mir_operations_validate(const SolMirOperations *operations,
    SolDiagnostics *diagnostics);
bool sol_mir_operations_render(FILE *stream, const SolMirOperations *operations);

#endif
