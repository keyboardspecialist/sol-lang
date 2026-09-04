#ifndef SOL_MIR_MATERIALIZE_H
#define SOL_MIR_MATERIALIZE_H

#include "sol/mir_plan.h"

/* Concrete type references in this owner index the borrowed plan's concrete
   type table. They are deliberately not SolIrTypeId values. */
typedef size_t SolMirMaterializedTypeId;

#define SOL_MIR_MATERIALIZED_NONE SIZE_MAX

typedef struct {
    SolMirPlanTypedUseKind kind;
    size_t source; /* Explicit source-IR or template-MIR provenance. */
    size_t ordinal;
    SolMirMaterializedTypeId type;
    SolAccessMode access;
} SolMirMaterializedTypeOverlay;

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
    SolMirPlanSlice invokes;
    /* Exact deep-owned template topology and source provenance. Its SolIrTypeId
       and symbolic invoke fields are not executable metadata; concrete types
       and targets are exclusively in overlays and invoke bindings above. */
    SolMir topology;
} SolMirMaterializedImage;

typedef struct {
    size_t max_instances;
    size_t max_cfg_items;
    size_t max_bindings;
    size_t max_owned_bytes;
    size_t max_materialization_work;
} SolMirMaterializeLimits;

typedef struct {
    size_t instances;
    size_t cfg_items;
    size_t bindings;
    size_t owned_bytes;
    size_t materialization_work;
} SolMirMaterializeUsage;

/* Unstable trusted mutable compiler-internal owner. The validated plan and its
   program/IR must outlive this owner. Restore test pointer mutations before free. */
typedef struct {
    const SolMirPlan *plan;
    SolMirMaterializedImage *images;
    size_t image_count;
    size_t image_capacity;
    SolMirMaterializedTypeId *type_ids;
    size_t type_id_count;
    size_t type_id_capacity;
    SolAccessMode *accesses;
    size_t access_count;
    size_t access_capacity;
    SolMirMaterializedTypeOverlay *overlays;
    size_t overlay_count;
    size_t overlay_capacity;
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
