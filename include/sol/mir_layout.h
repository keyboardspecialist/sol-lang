#ifndef SOL_MIR_LAYOUT_H
#define SOL_MIR_LAYOUT_H

#include "sol/mir_representation.h"

#include <stdint.h>

typedef enum {
    SOL_MIR_ENDIAN_LITTLE,
    SOL_MIR_ENDIAN_BIG,
} SolMirEndianness;

typedef struct {
    uint64_t pointer_size;
    uint64_t pointer_alignment;
    uint64_t int64_alignment;
    SolMirEndianness endianness;
    uint64_t max_object_bytes;
} SolMirTargetDescriptor;

bool sol_mir_target_descriptor_validate(const SolMirTargetDescriptor *target);
SolMirTargetDescriptor sol_mir_target_wasm32(void);

typedef enum {
    SOL_MIR_LAYOUT_OBJECT_NONE,
    SOL_MIR_LAYOUT_OBJECT_PRODUCT,
    SOL_MIR_LAYOUT_OBJECT_SUM,
    SOL_MIR_LAYOUT_OBJECT_TEXT,
    SOL_MIR_LAYOUT_OBJECT_CALLABLE,
    SOL_MIR_LAYOUT_OBJECT_CAPABILITY,
} SolMirLayoutObjectKind;

#define SOL_MIR_LAYOUT_OFFSET_NONE UINT64_MAX

typedef struct {
    SolMirRecipeId recipe;
    uint64_t value_size;
    uint64_t value_alignment;
    SolMirLayoutObjectKind object_kind;
    bool has_object;
    uint64_t object_size;
    uint64_t object_alignment;
    uint64_t tail_padding;
    uint64_t tag_offset;
    uint64_t tag_size;
    uint64_t payload_offset;
    uint64_t payload_size;
    uint64_t data_handle_offset;
    uint64_t length_offset;
    uint64_t target_token_offset;
    uint64_t environment_handle_offset;
    uint64_t root_token_offset;
    uint64_t private_source_handle_offset;
} SolMirTypeLayout;

typedef struct {
    size_t field;
    SolMirRecipeId owner_recipe;
    size_t variant;
    /* False shape records use canonical zero/zero/align-one layout. */
    bool has_storage;
    uint64_t offset;
    uint64_t size;
    uint64_t alignment;
    uint64_t padding_before;
} SolMirFieldLayout;

typedef struct {
    size_t variant;
    SolMirRecipeId owner_recipe;
    uint32_t tag;
    bool inhabited;
    /* False for an unreachable variant; its fields have no storage. */
    bool has_payload_storage;
    uint64_t payload_size;
    uint64_t payload_alignment;
    uint64_t tail_padding;
} SolMirVariantLayout;

typedef struct {
    size_t projection;
    SolMirMaterializedPlaceId place;
    SolMirRecipeId base_recipe;
    SolMirRecipeId result_recipe;
    size_t field_layout;
    uint64_t object_offset;
} SolMirProjectionMap;

typedef struct {
    size_t max_type_layouts;
    size_t max_field_layouts;
    size_t max_variant_layouts;
    size_t max_projection_maps;
    size_t max_owned_bytes;
    size_t max_build_scratch_bytes;
    size_t max_build_work;
    size_t max_validation_scratch_bytes;
    size_t max_validation_work;
} SolMirLayoutLimits;

typedef struct {
    size_t type_layouts;
    size_t field_layouts;
    size_t variant_layouts;
    size_t projection_maps;
    size_t owned_bytes;
    size_t build_scratch_bytes;
    size_t build_work;
    size_t validation_scratch_bytes;
    size_t validation_work;
} SolMirLayoutUsage;

/* Unstable target-specific owner. representation is borrowed and must outlive
   this owner. For a malformed trusted owner, restore allocation bases and
   capacities before free; free is not an arbitrary-pointer recovery API. */
typedef struct {
    const SolMirRepresentation *representation;
    SolMirTargetDescriptor target;
    SolMirTypeLayout *types;
    size_t type_count;
    size_t type_capacity;
    SolMirFieldLayout *fields;
    size_t field_count;
    size_t field_capacity;
    SolMirVariantLayout *variants;
    size_t variant_count;
    size_t variant_capacity;
    SolMirProjectionMap *projections;
    size_t projection_count;
    size_t projection_capacity;
    SolMirLayoutLimits limits;
    SolMirLayoutUsage usage;
} SolMirLayout;

typedef struct {
    const SolMirRepresentation *representation;
    const SolMirTargetDescriptor *target;
    /* NULL or wholly zero selects defaults. Partial zero is invalid. */
    const SolMirLayoutLimits *limits;
} SolMirLayoutBuildRequest;

typedef enum {
    SOL_MIR_LAYOUT_BUILD_SUCCEEDED,
    SOL_MIR_LAYOUT_BUILD_INVALID_ARGUMENT,
    SOL_MIR_LAYOUT_BUILD_INVALID_REPRESENTATION,
    SOL_MIR_LAYOUT_BUILD_INVALID_TARGET,
    SOL_MIR_LAYOUT_BUILD_UNSUPPORTED,
    SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
    SOL_MIR_LAYOUT_BUILD_ALLOCATION_FAILED,
    SOL_MIR_LAYOUT_BUILD_INTERNAL_FAILED,
} SolMirLayoutBuildOutcome;

void sol_mir_layout_init(SolMirLayout *layout);
void sol_mir_layout_free(SolMirLayout *layout);
SolMirLayoutLimits sol_mir_layout_default_limits(void);
SolMirLayoutBuildOutcome sol_mir_layout_build(
    const SolMirLayoutBuildRequest *request,
    SolMirLayout *layout,
    SolDiagnostics *diagnostics
);
bool sol_mir_layout_validate(const SolMirLayout *layout,
    SolDiagnostics *diagnostics);
/* Validation and buffering complete before the single output write. */
bool sol_mir_layout_render(FILE *stream, const SolMirLayout *layout);

#endif
