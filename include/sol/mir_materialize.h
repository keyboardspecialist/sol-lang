#ifndef SOL_MIR_MATERIALIZE_H
#define SOL_MIR_MATERIALIZE_H

#include "sol/mir_plan.h"

/* Specialization-record references index arenas owned by SolMirMaterialization.
   These records are checked overlays over topology, not a complete CFG. */
typedef size_t SolMirMaterializedTypeId;
typedef size_t SolMirMaterializedLocalId;
typedef size_t SolMirMaterializedPlaceId;
typedef size_t SolMirMaterializedValueId;
typedef size_t SolMirMaterializedInstructionId;
typedef size_t SolMirMaterializedTemporaryId;

#define SOL_MIR_MATERIALIZED_NONE SIZE_MAX

typedef struct {
    SolMirPlanTypedUseKind kind;
    size_t source; /* Explicit source-IR or template-MIR provenance. */
    size_t ordinal;
    SolMirPlanContextId context;
    SolMirMaterializedTypeId type;
    SolAccessMode access;
} SolMirMaterializedTypeOverlay;

typedef struct {
    SolIrTypeKind kind;
    SolIrDefinitionId definition; /* Source provenance, not a layout choice. */
    SolMirPlanSlice arguments;
    SolMirPlanSlice parameters;
    SolMirPlanSlice parameter_accesses;
    SolMirMaterializedTypeId result;
    SolMirPlanEffectRowId effects;
    SolMirPlanSlice ownership_components;
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
    SolMirBlockId block;
    size_t source_definition;
    SolMirMaterializedInstructionId instruction;
    SolIrExpressionId source_expression;
    SolSpan span;
} SolMirMaterializedValue;

/* Checked specialization metadata for one symbolic instruction. Blocks,
   terminators, edges, and deferred payload execution remain in topology. Fields
   not selected by kind are canonical NONE/zero. */
typedef struct {
    SolMirInstructionKind kind;
    SolMirBlockId block;
    SolMirMaterializedValueId result;
    SolMirMaterializedTypeId type;
    SolIrExpressionId source_expression;
    SolSpan span;
    int64_t integer;
    bool boolean;
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

typedef enum {
    SOL_MIR_MATERIALIZED_TARGET_INSTANCE,
    SOL_MIR_MATERIALIZED_TARGET_IMPORT,
} SolMirMaterializedTargetKind;

typedef struct {
    SolMirBlockId block;
    SolMirProgramSource source;
    SolIrCallableId symbolic_callable; /* Provenance only; never executable. */
    SolIrDefinitionId dispatch_trait;  /* Provenance only. */
    SolIrCallableId dispatch_requirement; /* Provenance only. */
    SolMirMaterializedTargetKind target_kind;
    SolMirPlanInstanceId instance;
    SolMirPlanImportId import;
} SolMirMaterializedInvokeBinding;

typedef struct {
    SolMirPlanInstanceId instance;
    SolIrCallableId source_callable;
    SolMirMaterializedTypeId receiver;
    SolMirPlanSlice type_arguments;
    SolMirPlanSlice parameter_types;
    SolMirPlanSlice parameter_accesses;
    SolMirMaterializedTypeId result;
    SolMirPlanEffectRowId effects;
    SolMirPlanSlice overlays;
    SolMirPlanSlice contexts;
    SolMirPlanSlice locals;
    SolMirPlanSlice places;
    SolMirPlanSlice values;
    SolMirPlanSlice instructions;
    SolMirPlanSlice temporaries;
    SolMirPlanSlice construct_operands;
    SolMirPlanSlice call_arguments;
    SolMirPlanSlice invokes;
    /* Exact deep-owned, structurally executable symbolic CFG and authenticated
       source provenance. P2.3b1 metadata specializes its types/locals/places and
       selected payload references; P2.3b2 will replace blocks, terminators,
       edges, deferred payloads, handler/dispatch, and writeback operations. */
    SolMir topology;
} SolMirMaterializedImage;

typedef struct {
    size_t max_instances;
    size_t max_cfg_items;
    size_t max_bindings;
    size_t max_concrete_records;
    size_t max_owned_bytes;
    size_t max_materialization_work;
} SolMirMaterializeLimits;

typedef struct {
    size_t instances;
    size_t cfg_items;
    size_t bindings;
    size_t concrete_records;
    size_t owned_bytes;
    size_t materialization_work;
} SolMirMaterializeUsage;

/* Unstable trusted mutable compiler-internal owner. The validated plan and its
   program/IR must outlive this owner. P2.3b1 concrete records specialize type,
   local, place, value, temporary, and selected instruction/operand metadata.
   SolMirMaterializedImage.topology remains the structurally executable symbolic
   source for blocks, terminators, edges, loops, and deferred payloads until
   P2.3b2. Restore test pointer mutations before free. */
typedef struct {
    const SolMirPlan *plan;
    SolMirMaterializedImage *images;
    size_t image_count;
    size_t image_capacity;
    SolMirMaterializedType *types;
    size_t type_count;
    size_t type_capacity;
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
    SolMirMaterializedInvokeBinding *invoke_bindings;
    size_t invoke_binding_count;
    size_t invoke_binding_capacity;
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
