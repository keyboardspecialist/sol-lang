#ifndef SOL_MIR_REPRESENTATION_H
#define SOL_MIR_REPRESENTATION_H

#include "sol/mir_materialize.h"

typedef size_t SolMirRecipeId;

#define SOL_MIR_RECIPE_NONE SIZE_MAX

typedef enum {
    SOL_MIR_RECIPE_INT64,
    SOL_MIR_RECIPE_BOOL,
    SOL_MIR_RECIPE_TEXT,
    SOL_MIR_RECIPE_UNIT,
    SOL_MIR_RECIPE_NEVER,
    SOL_MIR_RECIPE_TUPLE,
    SOL_MIR_RECIPE_RECORD,
    SOL_MIR_RECIPE_ENUM,
    SOL_MIR_RECIPE_OPTION,
    SOL_MIR_RECIPE_RESULT,
    SOL_MIR_RECIPE_DISTINCT,
    SOL_MIR_RECIPE_REFINED,
    SOL_MIR_RECIPE_FUNCTION,
    SOL_MIR_RECIPE_CAPABILITY,
} SolMirRecipeKind;

typedef enum {
    SOL_MIR_STORAGE_NONE,
    SOL_MIR_STORAGE_SCALAR,
    SOL_MIR_STORAGE_TEXT_HANDLE,
    /* Abstract represented aggregate value; P2.5 chooses inline vs indirect. */
    SOL_MIR_STORAGE_AGGREGATE_VALUE,
    SOL_MIR_STORAGE_CALLABLE_HANDLE,
    SOL_MIR_STORAGE_CAPABILITY_HANDLE,
} SolMirStorageKind;

typedef enum {
    SOL_MIR_COPY_TRIVIAL,
    SOL_MIR_COPY_TEXT,
    SOL_MIR_COPY_AGGREGATE,
    SOL_MIR_COPY_WRAPPER,
    SOL_MIR_COPY_FORBIDDEN,
    SOL_MIR_COPY_UNREACHABLE,
} SolMirCopyKind;

typedef enum {
    SOL_MIR_DROP_NONE,
    SOL_MIR_DROP_TEXT,
    SOL_MIR_DROP_AGGREGATE,
    SOL_MIR_DROP_CALLABLE,
    SOL_MIR_DROP_CAPABILITY,
    SOL_MIR_DROP_WRAPPER,
} SolMirDropKind;

typedef struct {
    SolIrFieldId source_field;
    size_t ordinal;
    SolMirRecipeId type;
} SolMirRecipeField;

typedef struct {
    SolIrVariantId source_variant;
    size_t ordinal;
    size_t semantic_tag;
    SolMirPlanSlice fields;
} SolMirRecipeVariant;

typedef struct {
    SolMirRecipeKind kind;
    SolIrDefinitionId concrete_definition;
    SolMirPlanSlice fields;
    SolMirPlanSlice variants;
    SolMirPlanSlice parameters;
    SolMirPlanSlice parameter_accesses;
    SolMirRecipeId result;
    SolMirMaterializedEffectRowId effects;
    SolMirRecipeId backing;
    SolMirRecipeId capability_source;
    SolMirStorageKind storage;
    SolMirCopyKind copy_kind;
    SolMirDropKind drop_kind;
    bool inhabited;
    bool zero_sized;
    bool is_copy;
} SolMirRecipe;

typedef enum {
    SOL_MIR_CALLABLE_PRODUCER_EXACT_FUNCTION,
    SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION,
} SolMirCallableProducerKind;

typedef struct {
    SolMirCallableProducerKind kind;
    SolMirRecipeId function_recipe;
    SolMirMaterializedSemanticSiteId semantic_site;
    SolMirMaterializedBindingId binding;
    SolMirMaterializedTargetKind target_kind;
    SolMirPlanInstanceId instance;
    SolMirMaterializedImportId import;
    SolMirRecipeId captured_receiver_type;
    SolMirMaterializedReceiverKind captured_receiver_kind;
    SolIrExpressionId captured_receiver_expression;
    SolMirMaterializedPlaceId captured_receiver_place;
    SolMirMaterializedTemporaryId captured_receiver_temporary;
    SolMirMaterializedValueId captured_receiver_value;
    SolMirMaterializedInstructionId captured_receiver_instruction;
    SolMirPlanSlice captured_receiver_roots;
    SolMirMaterializedEffectRowId effects;
} SolMirCallableProducer;

typedef struct {
    size_t max_recipes;
    size_t max_fields;
    size_t max_variants;
    size_t max_recipe_ids;
    size_t max_callable_producers;
    size_t max_receiver_roots;
    size_t max_owned_bytes;
    size_t max_build_scratch_bytes;
    size_t max_build_work;
    size_t max_validation_work;
    size_t max_validation_scratch_bytes;
} SolMirRepresentationLimits;

typedef struct {
    size_t recipes;
    size_t fields;
    size_t variants;
    size_t recipe_ids;
    size_t callable_producers;
    size_t receiver_roots;
    size_t owned_bytes;
    size_t build_scratch_bytes;
    size_t build_work;
    size_t validation_work;
    size_t validation_scratch_bytes;
} SolMirRepresentationUsage;

/* Unstable target-neutral owner. The validated materialization is borrowed and
   must outlive this graph. owned_bytes counts persistent output allocations;
   scratch limits separately bound peak build/validation temporary storage.
   Recipes deliberately contain no target layout or ABI. Restore any pointer or
   capacity mutation, especially a rejected alias, before calling free. */
typedef struct {
    const SolMirMaterialization *materialization;
    SolMirRecipe *recipes;
    size_t recipe_count;
    size_t recipe_capacity;
    SolMirRecipeField *fields;
    size_t field_count;
    size_t field_capacity;
    SolMirRecipeVariant *variants;
    size_t variant_count;
    size_t variant_capacity;
    SolMirRecipeId *recipe_ids;
    size_t recipe_id_count;
    size_t recipe_id_capacity;
    SolAccessMode *accesses;
    size_t access_count;
    size_t access_capacity;
    SolMirMaterializedLocalId *receiver_roots;
    size_t receiver_root_count;
    size_t receiver_root_capacity;
    SolMirCallableProducer *callable_producers;
    size_t callable_producer_count;
    size_t callable_producer_capacity;
    SolMirRepresentationLimits limits;
    SolMirRepresentationUsage usage;
} SolMirRepresentation;

typedef struct {
    const SolMirMaterialization *materialization;
    /* NULL or wholly zero selects defaults. Partial zero is invalid. */
    const SolMirRepresentationLimits *limits;
} SolMirRepresentationBuildRequest;

typedef enum {
    SOL_MIR_REPRESENTATION_BUILD_SUCCEEDED,
    SOL_MIR_REPRESENTATION_BUILD_INVALID_ARGUMENT,
    SOL_MIR_REPRESENTATION_BUILD_INVALID_MATERIALIZATION,
    SOL_MIR_REPRESENTATION_BUILD_UNSUPPORTED,
    SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED,
    SOL_MIR_REPRESENTATION_BUILD_ALLOCATION_FAILED,
    SOL_MIR_REPRESENTATION_BUILD_INTERNAL_FAILED,
} SolMirRepresentationBuildOutcome;

void sol_mir_representation_init(SolMirRepresentation *representation);
void sol_mir_representation_free(SolMirRepresentation *representation);
SolMirRepresentationLimits sol_mir_representation_default_limits(void);
SolMirRepresentationBuildOutcome sol_mir_representation_build(
    const SolMirRepresentationBuildRequest *request,
    SolMirRepresentation *representation,
    SolDiagnostics *diagnostics
);
bool sol_mir_representation_validate(
    const SolMirRepresentation *representation,
    SolDiagnostics *diagnostics
);
/* Validation and buffering complete before the single output write. */
bool sol_mir_representation_render(
    FILE *stream,
    const SolMirRepresentation *representation
);

#endif
