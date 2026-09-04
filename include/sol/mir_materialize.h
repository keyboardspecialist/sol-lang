#ifndef SOL_MIR_MATERIALIZE_H
#define SOL_MIR_MATERIALIZE_H

#include "sol/mir_plan.h"

/* Executable references index arenas owned by SolMirMaterialization. */
typedef size_t SolMirMaterializedTypeId;
typedef size_t SolMirMaterializedEffectRowId;
typedef size_t SolMirMaterializedLocalId;
typedef size_t SolMirMaterializedPlaceId;
typedef size_t SolMirMaterializedValueId;
typedef size_t SolMirMaterializedInstructionId;
typedef size_t SolMirMaterializedTemporaryId;
typedef size_t SolMirMaterializedBlockId;
typedef size_t SolMirMaterializedEdgeId;
typedef size_t SolMirMaterializedLoopId;
typedef size_t SolMirMaterializedBindingId;
typedef size_t SolMirMaterializedHandlerId;
typedef size_t SolMirMaterializedImportId;
typedef size_t SolMirMaterializedSemanticSiteId;

#define SOL_MIR_MATERIALIZED_NONE SIZE_MAX

typedef struct {
    SolMirPlanTypedUseKind kind;
    size_t source; /* Explicit source-IR or template-MIR provenance. */
    size_t ordinal;
    SolMirPlanContextId context;
    SolMirMaterializedTypeId type;
    SolAccessMode access;
} SolMirMaterializedTypeOverlay;

typedef enum {
    SOL_MIR_MATERIALIZED_NOMINAL_NONE,
    SOL_MIR_MATERIALIZED_NOMINAL_RECORD,
    SOL_MIR_MATERIALIZED_NOMINAL_ENUM,
    SOL_MIR_MATERIALIZED_NOMINAL_DISTINCT,
    SOL_MIR_MATERIALIZED_NOMINAL_REFINED,
    SOL_MIR_MATERIALIZED_NOMINAL_CAPABILITY,
} SolMirMaterializedNominalCategory;

typedef struct {
    SolIrFieldId source_field;
    size_t ordinal;
    SolMirMaterializedTypeId type;
} SolMirMaterializedShapeField;

typedef struct {
    SolIrVariantId source_variant;
    size_t ordinal;
    SolMirPlanSlice fields;
} SolMirMaterializedShapeVariant;

typedef struct {
    SolIrTypeKind kind;
    SolIrDefinitionId definition; /* Source provenance, not a layout choice. */
    SolMirMaterializedNominalCategory nominal_category;
    SolMirPlanSlice arguments;
    SolMirPlanSlice parameters;
    SolMirPlanSlice parameter_accesses;
    SolMirMaterializedTypeId result;
    SolMirMaterializedEffectRowId effects;
    /* Concrete source-order nominal shape, separate from ownership semantics. */
    SolMirPlanSlice fields;
    SolMirPlanSlice variants;
    SolMirMaterializedTypeId backing;
    SolMirMaterializedTypeId capability_source;
    SolMirPlanSlice ownership_components;
    bool nominal_open;
    bool is_copy;
} SolMirMaterializedType;

typedef enum {
    SOL_MIR_MATERIALIZED_LOCAL_RECEIVER,
    SOL_MIR_MATERIALIZED_LOCAL_PARAMETER,
    SOL_MIR_MATERIALIZED_LOCAL_BODY,
} SolMirMaterializedLocalKind;

typedef struct {
    SolMirPlanInstanceId instance;
    SolIrLocalId source_local;
    SolMirMaterializedTypeId type;
    SolAccessMode access;
    SolMirMaterializedLocalKind kind;
    size_t ordinal;
} SolMirMaterializedLocal;

typedef struct {
    SolIrProjectionKind kind;
    SolMirMaterializedTypeId type;
    SolIrFieldId source_field;
    size_t tuple_ordinal;
    size_t source_projection;
} SolMirMaterializedProjection;

typedef struct {
    SolMirPlanInstanceId instance;
    SolIrPlaceId source_place;
    SolMirMaterializedLocalId local;
    SolMirMaterializedTypeId root_type;
    SolMirPlanSlice projections;
    SolMirMaterializedTypeId final_type;
} SolMirMaterializedPlace;

typedef struct {
    SolMirValueKind kind;
    SolMirMaterializedTypeId type;
    SolMirMaterializedBlockId block;
    size_t source_definition;
    SolMirMaterializedInstructionId instruction;
    SolIrExpressionId source_expression;
    SolSpan span;
} SolMirMaterializedValue;

/* Fields not selected by kind are canonical NONE/zero. */
typedef struct {
    SolMirInstructionKind kind;
    SolMirMaterializedBlockId block;
    SolMirMaterializedValueId result;
    SolMirMaterializedTypeId type;
    SolIrExpressionId source_expression;
    SolSpan span;
    int64_t integer;
    bool boolean;
    SolMirPlanSlice text;
    SolMirMaterializedLocalId local;
    SolMirMaterializedPlaceId place;
    SolMirMaterializedValueId left;
    SolMirMaterializedValueId right;
    SolMirMaterializedTemporaryId temporary;
    SolMirMaterializedTemporaryId previous;
    size_t preserve_depth;
    SolTokenKind operator_kind;
    SolIrStatementId source_statement;
    SolIrExpressionId match_expression;
    SolIrArmId source_arm;
    size_t arm_ordinal;
    SolIrPatternId source_pattern;
    SolMirMaterializedTemporaryId pattern_scrutinee;
    SolIrSnapshotId source_snapshot;
    SolMirMaterializedHandlerId handler;
    SolMirConstructKind construct_kind;
    SolIrDefinitionId construct_definition;
    SolIrVariantId construct_variant;
    SolMirPlanSlice construct_operands;
    SolIrSlice source_capability_roots;
    SolIrSlice source_operation_roots;
} SolMirMaterializedInstruction;

typedef struct {
    SolMirMaterializedTypeId type;
    SolIrExpressionId source_expression;
    SolSpan span;
} SolMirMaterializedTemporary;

typedef struct {
    size_t formal;
    SolIrExpressionId source_expression;
    SolMirMaterializedTypeId type;
    SolMirMaterializedTemporaryId temporary;
} SolMirMaterializedConstructOperand;

typedef struct {
    size_t formal;
    SolAccessMode access;
    SolIrExpressionId source_expression;
    SolMirMaterializedTypeId type;
    SolMirMaterializedTemporaryId temporary;
    SolMirMaterializedPlaceId place;
} SolMirMaterializedCallArgument;

typedef struct {
    SolMirMaterializedBlockId block;
    SolMirPlanSlice arguments;
} SolMirMaterializedEdge;

typedef struct {
    SolMirTerminatorKind kind;
    SolSpan span;
    SolIrExpressionId source_expression;
    SolIrCallKind call_kind;
    SolMirMaterializedBindingId binding;
    SolMirMaterializedEffectRowId effects;
    SolMirMaterializedTemporaryId callee;
    SolMirMaterializedCallArgument receiver;
    SolMirPlanSlice arguments;
    SolMirMaterializedValueId result;
    SolMirMaterializedValueId value;
    SolMirMaterializedValueId condition;
    SolMirMaterializedValueId value_result;
    SolMirMaterializedValueId residual_result;
    SolMirMaterializedTemporaryId representation;
    SolMirMaterializedTemporaryId operand;
    SolMirMaterializedEdgeId edge;
    SolMirMaterializedEdgeId true_edge;
    SolMirMaterializedEdgeId false_edge;
    SolMirMaterializedEdgeId normal_edge;
    SolMirMaterializedEdgeId failure_edge;
    SolMirMaterializedEdgeId value_edge;
    SolMirMaterializedEdgeId residual_edge;
    SolMirMaterializedEdgeId satisfied_edge;
    SolMirMaterializedEdgeId violation_edge;
    SolMirMaterializedLoopId loop;
    SolMirMaterializedSemanticSiteId callable_site;
    bool predicate_inline;
    SolMirPlanSlice writebacks;
    SolIrStatementId source_statement;
    SolIrDefinitionId source_definition;
    SolObligationId source_obligation;
    size_t obligation_ordinal;
    SolIrPropagationKind propagation_kind;
    SolContractClauseKind contract_phase;
    SolContractOutcomeKind contract_outcome;
} SolMirMaterializedTerminator;

typedef struct {
    SolMirMaterializedBlockId id;
    size_t order;
    SolMirPlanSlice parameters;
    SolMirPlanSlice instructions;
    SolMirMaterializedTerminator terminator;
    SolSpan span;
    bool started;
    SolMirBlockId source_block;
} SolMirMaterializedBlock;

typedef struct {
    SolIrStatementId source_statement;
    SolMirMaterializedLoopId parent;
    SolMirMaterializedBlockId preheader;
    SolMirMaterializedBlockId header;
    SolMirMaterializedBlockId condition;
    SolMirMaterializedBlockId body;
    SolMirMaterializedBlockId backedge;
    SolMirMaterializedBlockId exit;
    SolIrSlice source_obligations;
    SolSpan span;
    SolMirLoopId source_loop;
} SolMirMaterializedLoop;

typedef struct {
    bool receiver;
    size_t formal;
    SolMirMaterializedPlaceId place;
    SolMirMaterializedTypeId type;
} SolMirMaterializedWriteback;

typedef enum {
    SOL_MIR_MATERIALIZED_TARGET_INSTANCE,
    SOL_MIR_MATERIALIZED_TARGET_IMPORT,
} SolMirMaterializedTargetKind;

typedef struct {
    SolMirMaterializedTargetKind target_kind;
    SolMirPlanInstanceId instance;
    SolMirMaterializedImportId import;
    SolMirMaterializedTypeId receiver;
    SolMirMaterializedPlaceId root;
    SolMirMaterializedEffectRowId effects;
} SolMirMaterializedOperationKey;

typedef struct {
    size_t source_demand;
    SolMirPlanDemandKind kind;
    SolMirPlanInstanceId parent;
    SolMirPlanContextId context;
    SolMirProgramSource source;
    SolIrCallableId symbolic_callable; /* Provenance only; never executable. */
    SolIrDefinitionId dispatch_trait;  /* Provenance only. */
    SolIrCallableId dispatch_requirement; /* Provenance only. */
    SolMirMaterializedTargetKind target_kind;
    SolMirPlanInstanceId instance;
    SolMirMaterializedImportId import;
    SolMirMaterializedSemanticSiteId site;
} SolMirMaterializedBinding;

typedef enum {
    SOL_MIR_MATERIALIZED_PRODUCER_ROOT,
    SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION,
    SOL_MIR_MATERIALIZED_PRODUCER_TERMINATOR,
    SOL_MIR_MATERIALIZED_PRODUCER_PREDICATE,
    SOL_MIR_MATERIALIZED_PRODUCER_HANDLER,
} SolMirMaterializedProducerKind;

typedef enum {
    SOL_MIR_MATERIALIZED_RECEIVER_NONE,
    SOL_MIR_MATERIALIZED_RECEIVER_SOURCE_EXPRESSION,
    SOL_MIR_MATERIALIZED_RECEIVER_PLACE,
    SOL_MIR_MATERIALIZED_RECEIVER_TEMPORARY,
    SOL_MIR_MATERIALIZED_RECEIVER_VALUE,
} SolMirMaterializedReceiverKind;

typedef struct {
    SolMirPlanDemandKind kind;
    SolMirMaterializedBindingId binding;
    SolMirPlanInstanceId parent;
    SolMirPlanContextId context;
    SolMirProgramSource source;
    SolIrDefinitionId source_definition;
    SolObligationId source_obligation;
    SolMirMaterializedProducerKind producer_kind;
    SolMirMaterializedBlockId block;
    SolMirMaterializedInstructionId instruction;
    SolMirMaterializedHandlerId handler;
    SolMirMaterializedTypeId produced_function_type;
    SolMirMaterializedTypeId captured_receiver_type;
    SolMirMaterializedReceiverKind captured_receiver_kind;
    SolIrExpressionId captured_receiver_expression;
    SolMirMaterializedPlaceId captured_receiver_place;
    SolMirMaterializedTemporaryId captured_receiver_temporary;
    SolMirMaterializedValueId captured_receiver_value;
    SolMirMaterializedInstructionId captured_receiver_instruction;
    SolMirPlanSlice captured_receiver_roots;
    SolMirMaterializedOperationKey operation;
} SolMirMaterializedSemanticSite;

typedef struct {
    SolIrCallableId source_callable;
    SolMirMaterializedTypeId receiver;
    SolAccessMode receiver_access;
    SolMirPlanSlice parameter_types;
    SolMirPlanSlice parameter_accesses;
    SolMirMaterializedTypeId result;
    SolMirMaterializedEffectRowId effects;
    SolMirPlanImportId source_import;
} SolMirMaterializedImport;

typedef struct { SolMirPlanSlice atoms; } SolMirMaterializedEffectRow;

typedef struct {
    SolMirPlanSlice name;
    SolMirPlanEffectAuthority authority;
    size_t ordinal;
} SolMirMaterializedEffectAtom;

typedef struct {
    SolMirPlanInstanceId parent;
    SolMirPlanContextId context;
    SolIrExpressionId source_expression;
    SolMirMaterializedBindingId source_binding;
    SolMirMaterializedBindingId provider_binding;
    SolMirMaterializedPlaceId authority;
    SolMirMaterializedPlaceId provider;
    SolIrCallableId handled_operation;
    size_t source_effect;
    SolIrLocalId source_root;
    SolMirMaterializedOperationKey operation;
    SolSpan span;
} SolMirMaterializedHandler;

typedef struct {
    SolMirPlanInstanceId instance;
    SolIrCallableId source_callable;
    SolMirMaterializedTypeId receiver;
    SolAccessMode receiver_access;
    SolMirPlanSlice type_arguments;
    SolMirPlanSlice parameter_types;
    SolMirPlanSlice parameter_accesses;
    SolMirMaterializedTypeId result;
    SolMirMaterializedEffectRowId effects;
    SolMirPlanSlice overlays;
    SolMirPlanSlice contexts;
    SolMirPlanSlice locals;
    SolMirPlanSlice places;
    SolMirPlanSlice values;
    SolMirPlanSlice instructions;
    SolMirPlanSlice temporaries;
    SolMirPlanSlice construct_operands;
    SolMirPlanSlice call_arguments;
    SolMirPlanSlice blocks;
    SolMirPlanSlice loops;
    SolMirPlanSlice handlers;
    SolMirPlanSlice bindings;
    SolMirMaterializedBlockId entry;
    SolMirMaterializedBlockId contract_body;
    SolMirMaterializedBlockId contract_epilogue;
    /* Exact deep-owned symbolic CFG used only to authenticate provenance. */
    SolMir topology;
} SolMirMaterializedImage;

typedef struct {
    size_t max_instances;
    size_t max_cfg_items;
    size_t max_bindings;
    size_t max_concrete_records;
    size_t max_owned_bytes;
    size_t max_materialization_work;
    size_t max_shape_resolution_work;
    size_t max_validation_work;
} SolMirMaterializeLimits;

typedef struct {
    size_t instances;
    size_t cfg_items;
    size_t bindings;
    size_t concrete_records;
    size_t owned_bytes;
    size_t materialization_work;
    size_t shape_resolution_work;
    size_t validation_work;
} SolMirMaterializeUsage;

/* Unstable trusted mutable compiler-internal owner. The validated plan and its
   program/IR must outlive this owner for provenance validation only. Concrete
   arenas are the complete executable CFG. Restore pointer mutations before free. */
typedef struct {
    const SolMirPlan *plan;
    SolMirMaterializedImage *images;
    size_t image_count;
    size_t image_capacity;
    SolMirMaterializedType *types;
    size_t type_count;
    size_t type_capacity;
    SolMirMaterializedShapeField *shape_fields;
    size_t shape_field_count;
    size_t shape_field_capacity;
    SolMirMaterializedShapeVariant *shape_variants;
    size_t shape_variant_count;
    size_t shape_variant_capacity;
    SolMirMaterializedTypeId *type_ids;
    size_t type_id_count;
    size_t type_id_capacity;
    SolAccessMode *accesses;
    size_t access_count;
    size_t access_capacity;
    SolMirMaterializedTypeOverlay *overlays;
    size_t overlay_count;
    size_t overlay_capacity;
    SolMirPlanContext *contexts;
    size_t context_count;
    size_t context_capacity;
    SolMirMaterializedLocal *locals;
    size_t local_count;
    size_t local_capacity;
    SolMirMaterializedPlace *places;
    size_t place_count;
    size_t place_capacity;
    SolMirMaterializedProjection *projections;
    size_t projection_count;
    size_t projection_capacity;
    SolMirMaterializedValue *values;
    size_t value_count;
    size_t value_capacity;
    SolMirMaterializedInstruction *instructions;
    size_t instruction_count;
    size_t instruction_capacity;
    SolMirMaterializedTemporary *temporaries;
    size_t temporary_count;
    size_t temporary_capacity;
    SolMirMaterializedConstructOperand *construct_operands;
    size_t construct_operand_count;
    size_t construct_operand_capacity;
    SolMirMaterializedCallArgument *call_arguments;
    size_t call_argument_count;
    size_t call_argument_capacity;
    SolMirMaterializedBlock *blocks;
    size_t block_count;
    size_t block_capacity;
    SolMirMaterializedEdge *edges;
    size_t edge_count;
    size_t edge_capacity;
    SolMirMaterializedValueId *edge_values;
    size_t edge_value_count;
    size_t edge_value_capacity;
    SolMirMaterializedValueId *parameter_values;
    size_t parameter_value_count;
    size_t parameter_value_capacity;
    SolMirMaterializedLoop *loops;
    size_t loop_count;
    size_t loop_capacity;
    SolMirMaterializedBinding *bindings;
    size_t binding_count;
    size_t binding_capacity;
    SolMirMaterializedSemanticSite *semantic_sites;
    size_t semantic_site_count;
    size_t semantic_site_capacity;
    SolMirMaterializedLocalId *receiver_roots;
    size_t receiver_root_count;
    size_t receiver_root_capacity;
    SolMirMaterializedImport *imports;
    size_t import_count;
    size_t import_capacity;
    SolMirMaterializedHandler *handlers;
    size_t handler_count;
    size_t handler_capacity;
    SolMirMaterializedWriteback *writebacks;
    size_t writeback_count;
    size_t writeback_capacity;
    SolMirMaterializedEffectRow *effect_rows;
    size_t effect_row_count;
    size_t effect_row_capacity;
    SolMirMaterializedEffectAtom *effect_atoms;
    size_t effect_atom_count;
    size_t effect_atom_capacity;
    size_t *effect_row_atoms;
    size_t effect_row_atom_count;
    size_t effect_row_atom_capacity;
    char *effect_names;
    size_t effect_name_count;
    size_t effect_name_capacity;
    char *literal_bytes;
    size_t literal_byte_count;
    size_t literal_byte_capacity;
    SolMirMaterializeLimits limits;
    SolMirMaterializeUsage usage;
} SolMirMaterialization;

typedef struct {
    const SolMirPlan *plan;
    /* NULL or wholly zero selects defaults. Partial zero is invalid. */
    const SolMirMaterializeLimits *limits;
} SolMirMaterializeBuildRequest;

typedef enum {
    SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED,
    SOL_MIR_MATERIALIZE_BUILD_INVALID_ARGUMENT,
    SOL_MIR_MATERIALIZE_BUILD_INVALID_PLAN,
    SOL_MIR_MATERIALIZE_BUILD_UNSUPPORTED_OR_UNRESOLVED,
    SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
    SOL_MIR_MATERIALIZE_BUILD_ALLOCATION_FAILED,
    SOL_MIR_MATERIALIZE_BUILD_INTERNAL_FAILED,
} SolMirMaterializeBuildOutcome;

void sol_mir_materialization_init(SolMirMaterialization *materialization);
void sol_mir_materialization_free(SolMirMaterialization *materialization);
SolMirMaterializeLimits sol_mir_materialize_default_limits(void);
SolMirMaterializeBuildOutcome sol_mir_materialize_build(
    const SolMirMaterializeBuildRequest *request,
    SolMirMaterialization *materialization,
    SolDiagnostics *diagnostics
);
bool sol_mir_materialization_validate(
    const SolMirMaterialization *materialization,
    SolDiagnostics *diagnostics
);
/* Validation and rendering complete before the single output write. */
bool sol_mir_materialization_render(
    FILE *stream,
    const SolMirMaterialization *materialization
);

#endif
