#include "sol/mir_layout.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SolMirLayout *out;
    SolDiagnostics *diagnostics;
    SolMirLayoutBuildOutcome outcome;
    unsigned char *object_state;
} LayoutBuilder;

typedef struct { uintptr_t start, end; } Range;
typedef struct { char *data; size_t length, capacity; bool failed; } Buffer;

static bool layout_error(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) sol_diagnostics_add(diagnostics,
        "SOL-MIR-LAYOUT-001", SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    return false;
}

static bool fail(LayoutBuilder *builder, SolMirLayoutBuildOutcome outcome,
    const char *message) {
    builder->outcome = outcome;
    return layout_error(builder->diagnostics, message);
}

static bool checked_add_size(size_t *value, size_t amount) {
    if (amount > SIZE_MAX - *value) return false;
    *value += amount;
    return true;
}

static bool checked_mul_size(size_t left, size_t right, size_t *result) {
    if (left != 0 && right > SIZE_MAX / left) return false;
    *result = left * right;
    return true;
}

static bool checked_add_u64(uint64_t left, uint64_t right, uint64_t *result) {
    if (right > UINT64_MAX - left) return false;
    *result = left + right;
    return true;
}

static bool align_u64(uint64_t value, uint64_t alignment, uint64_t *result) {
    uint64_t mask = alignment - 1;
    if (value > UINT64_MAX - mask) return false;
    *result = (value + mask) & ~mask;
    return true;
}

static bool power_of_two(uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

bool sol_mir_target_descriptor_validate(const SolMirTargetDescriptor *target) {
    if (target == NULL
        || (target->pointer_size != 4 && target->pointer_size != 8)) return false;
    uint64_t addressable_max = target->pointer_size == 4
        ? (uint64_t)UINT32_MAX : UINT64_MAX;
    return power_of_two(target->pointer_alignment)
        && target->pointer_size % target->pointer_alignment == 0
        && power_of_two(target->int64_alignment)
        && 8 % target->int64_alignment == 0
        && (target->endianness == SOL_MIR_ENDIAN_LITTLE
            || target->endianness == SOL_MIR_ENDIAN_BIG)
        && target->max_object_bytes != 0
        && target->max_object_bytes <= addressable_max;
}

SolMirTargetDescriptor sol_mir_target_wasm32(void) {
    return (SolMirTargetDescriptor){4, 4, 8, SOL_MIR_ENDIAN_LITTLE,
        UINT32_MAX};
}

void sol_mir_layout_init(SolMirLayout *layout) {
    if (layout != NULL) memset(layout, 0, sizeof(*layout));
}

void sol_mir_layout_free(SolMirLayout *layout) {
    if (layout == NULL) return;
    free(layout->types); free(layout->fields); free(layout->variants);
    free(layout->projections);
    sol_mir_layout_init(layout);
}

SolMirLayoutLimits sol_mir_layout_default_limits(void) {
    return (SolMirLayoutLimits){
        12000000, 24000000, 12000000, 24000000,
        768u * 1024u * 1024u, 128u * 1024u * 1024u, 100000000,
        896u * 1024u * 1024u, 1000000000,
    };
}

static bool limits_zero(SolMirLayoutLimits limits) {
    return limits.max_type_layouts == 0 && limits.max_field_layouts == 0
        && limits.max_variant_layouts == 0 && limits.max_projection_maps == 0
        && limits.max_owned_bytes == 0 && limits.max_build_scratch_bytes == 0
        && limits.max_build_work == 0
        && limits.max_validation_scratch_bytes == 0
        && limits.max_validation_work == 0;
}

static bool limits_complete(SolMirLayoutLimits limits) {
    return limits.max_type_layouts != 0 && limits.max_field_layouts != 0
        && limits.max_variant_layouts != 0 && limits.max_projection_maps != 0
        && limits.max_owned_bytes != 0 && limits.max_build_scratch_bytes != 0
        && limits.max_build_work != 0
        && limits.max_validation_scratch_bytes != 0
        && limits.max_validation_work != 0;
}

static bool owner_empty(const SolMirLayout *layout) {
    if (layout == NULL) return false;
    const unsigned char *bytes = (const unsigned char *)layout;
    for (size_t i = 0; i < sizeof(*layout); ++i)
        if (bytes[i] != 0) return false;
    return true;
}

static bool charge(LayoutBuilder *builder, size_t amount) {
    return checked_add_size(&builder->out->usage.build_work, amount)
        && builder->out->usage.build_work <= builder->out->limits.max_build_work;
}

static void *allocate(LayoutBuilder *builder, size_t count, size_t item_size) {
    if (count == 0) return NULL;
    size_t bytes;
    if (!checked_mul_size(count, item_size, &bytes)
        || !checked_add_size(&builder->out->usage.owned_bytes, bytes)
        || builder->out->usage.owned_bytes
            > builder->out->limits.max_owned_bytes) {
        builder->outcome = SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED;
        return NULL;
    }
    void *result = calloc(count, item_size);
    if (result == NULL) {
        builder->out->usage.owned_bytes -= bytes;
        builder->outcome = SOL_MIR_LAYOUT_BUILD_ALLOCATION_FAILED;
    }
    return result;
}

static SolMirTypeLayout blank_type(size_t id) {
    SolMirTypeLayout result;
    memset(&result, 0, sizeof(result));
    result.recipe = id;
    result.value_alignment = 1;
    result.object_alignment = 1;
    result.tag_offset = result.payload_offset
        = result.data_handle_offset = result.length_offset
        = result.target_token_offset = result.environment_handle_offset
        = result.root_token_offset = result.private_source_handle_offset
        = SOL_MIR_LAYOUT_OFFSET_NONE;
    return result;
}

static bool value_layout(LayoutBuilder *builder, size_t id) {
    SolMirLayout *out = builder->out;
    const SolMirRepresentation *r = out->representation;
    if (!charge(builder, 1)) return fail(builder,
        SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED, "layout build work limit exceeded");
    const SolMirRecipe *recipe = &r->recipes[id];
    SolMirTypeLayout *layout = &out->types[id];
    *layout = blank_type(id);
    if (!recipe->inhabited || recipe->zero_sized) return true;
    size_t current = id;
    for (size_t depth = 0; depth <= r->recipe_count; ++depth) {
        if (!charge(builder, 1)) return fail(builder,
            SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
            "layout wrapper work limit exceeded");
        const SolMirRecipe *item = &r->recipes[current];
        if (item->kind == SOL_MIR_RECIPE_DISTINCT
            || item->kind == SOL_MIR_RECIPE_REFINED) {
            if (item->backing >= r->recipe_count) return fail(builder,
                SOL_MIR_LAYOUT_BUILD_INVALID_REPRESENTATION,
                "layout wrapper backing is invalid");
            current = item->backing;
            continue;
        }
        if (item->kind == SOL_MIR_RECIPE_INT64) {
            layout->value_size = 8;
            layout->value_alignment = out->target.int64_alignment;
        } else if (item->kind == SOL_MIR_RECIPE_BOOL) {
            layout->value_size = 1; layout->value_alignment = 1;
        } else {
            layout->value_size = out->target.pointer_size;
            layout->value_alignment = out->target.pointer_alignment;
        }
        return true;
    }
    return fail(builder, SOL_MIR_LAYOUT_BUILD_UNSUPPORTED,
        "layout wrapper cycle is not representable");
}

static bool finish_object(LayoutBuilder *builder, SolMirTypeLayout *layout,
    uint64_t end, uint64_t alignment) {
    uint64_t size;
    if (!align_u64(end, alignment, &size)
        || size > builder->out->target.max_object_bytes)
        return fail(builder, SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
            "layout object size overflow or target bound exceeded");
    layout->has_object = true;
    layout->object_size = size;
    layout->object_alignment = alignment;
    layout->tail_padding = size - end;
    return true;
}

static bool header_object(LayoutBuilder *builder, SolMirTypeLayout *layout,
    SolMirLayoutObjectKind kind, size_t fields) {
    const uint64_t size = builder->out->target.pointer_size;
    const uint64_t alignment = builder->out->target.pointer_alignment;
    uint64_t end;
    if (fields > UINT64_MAX / size
        || !checked_add_u64(0, (uint64_t)fields * size, &end))
        return fail(builder, SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
            "layout header arithmetic overflowed");
    layout->object_kind = kind;
    return finish_object(builder, layout, end, alignment);
}

static bool variant_inhabited(const SolMirRepresentation *r,
    const SolMirRecipeVariant *variant) {
    for (size_t i = 0; i < variant->fields.count; ++i)
        if (!r->recipes[r->fields[variant->fields.offset + i].type].inhabited)
            return false;
    return true;
}

static bool layout_product(LayoutBuilder *builder, size_t id) {
    SolMirLayout *out = builder->out;
    const SolMirRecipe *recipe = &out->representation->recipes[id];
    SolMirTypeLayout *layout = &out->types[id];
    bool physical = recipe->inhabited && !recipe->zero_sized;
    uint64_t end = 0, maximum_alignment = 1;
    for (size_t i = 0; i < recipe->fields.count; ++i) {
        if (!charge(builder, 1)) return fail(builder,
            SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
            "layout product work limit exceeded");
        size_t field_id = recipe->fields.offset + i;
        const SolMirRecipeField *field = &out->representation->fields[field_id];
        if (!physical) {
            out->fields[field_id] = (SolMirFieldLayout){
                .field = field_id,
                .owner_recipe = id,
                .variant = SIZE_MAX,
                .alignment = 1,
            };
            continue;
        }
        const SolMirTypeLayout *value = &out->types[field->type];
        uint64_t previous = end, offset;
        if (!align_u64(end, value->value_alignment, &offset)
            || !checked_add_u64(offset, value->value_size, &end))
            return fail(builder, SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
                "layout product arithmetic overflowed");
        out->fields[field_id] = (SolMirFieldLayout){
            .field = field_id,
            .owner_recipe = id,
            .variant = SIZE_MAX,
            .has_storage = true,
            .offset = offset,
            .size = value->value_size,
            .alignment = value->value_alignment,
            .padding_before = offset - previous,
        };
        if (value->value_alignment > maximum_alignment)
            maximum_alignment = value->value_alignment;
    }
    if (!physical) return true;
    layout->object_kind = SOL_MIR_LAYOUT_OBJECT_PRODUCT;
    return finish_object(builder, layout, end, maximum_alignment);
}

static bool layout_sum(LayoutBuilder *builder, size_t id) {
    SolMirLayout *out = builder->out;
    const SolMirRepresentation *r = out->representation;
    const SolMirRecipe *recipe = &r->recipes[id];
    SolMirTypeLayout *layout = &out->types[id];
    uint64_t maximum_size = 0, maximum_alignment = 1;
    for (size_t i = 0; i < recipe->variants.count; ++i) {
        if (!charge(builder, 1)) return fail(builder,
            SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
            "layout sum work limit exceeded");
        size_t variant_id = recipe->variants.offset + i;
        const SolMirRecipeVariant *variant = &r->variants[variant_id];
        if (variant->semantic_tag > UINT32_MAX) return fail(builder,
            SOL_MIR_LAYOUT_BUILD_UNSUPPORTED, "layout sum tag exceeds u32");
        bool inhabited = variant_inhabited(r, variant);
        uint64_t end = 0, alignment = 1;
        for (size_t f = 0; f < variant->fields.count; ++f) {
            if (!charge(builder, 1)) return fail(builder,
                SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
                "layout variant work limit exceeded");
            size_t field_id = variant->fields.offset + f;
            const SolMirRecipeField *field = &r->fields[field_id];
            if (!inhabited) {
                out->fields[field_id] = (SolMirFieldLayout){
                    .field = field_id,
                    .owner_recipe = id,
                    .variant = variant_id,
                    .alignment = 1,
                };
                continue;
            }
            const SolMirTypeLayout *value = &out->types[field->type];
            uint64_t previous = end, offset;
            if (!align_u64(end, value->value_alignment, &offset)
                || !checked_add_u64(offset, value->value_size, &end))
                return fail(builder, SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
                    "layout variant arithmetic overflowed");
            out->fields[field_id] = (SolMirFieldLayout){
                .field = field_id,
                .owner_recipe = id,
                .variant = variant_id,
                .has_storage = true,
                .offset = offset,
                .size = value->value_size,
                .alignment = value->value_alignment,
                .padding_before = offset - previous,
            };
            if (value->value_alignment > alignment)
                alignment = value->value_alignment;
        }
        uint64_t payload_size;
        if (!align_u64(end, alignment, &payload_size)) return fail(builder,
            SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
            "layout variant tail padding overflowed");
        out->variants[variant_id] = (SolMirVariantLayout){
            .variant = variant_id,
            .owner_recipe = id,
            .tag = (uint32_t)variant->semantic_tag,
            .inhabited = inhabited,
            .has_payload_storage = inhabited,
            .payload_size = payload_size,
            .payload_alignment = alignment,
            .tail_padding = payload_size - end,
        };
        if (inhabited) {
            if (payload_size > maximum_size) maximum_size = payload_size;
            if (alignment > maximum_alignment) maximum_alignment = alignment;
        }
    }
    if (!recipe->inhabited) return true;
    uint64_t payload_offset, end;
    if (!align_u64(4, maximum_alignment, &payload_offset)
        || !checked_add_u64(payload_offset, maximum_size, &end))
        return fail(builder, SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
            "layout sum arithmetic overflowed");
    for (size_t i = 0; i < recipe->variants.count; ++i) {
        const SolMirRecipeVariant *variant
            = &r->variants[recipe->variants.offset + i];
        if (!out->variants[recipe->variants.offset + i].has_payload_storage)
            continue;
        for (size_t f = 0; f < variant->fields.count; ++f) {
            SolMirFieldLayout *field = &out->fields[variant->fields.offset + f];
            if (!checked_add_u64(field->offset, payload_offset, &field->offset))
                return fail(builder, SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
                    "layout absolute field offset overflowed");
        }
    }
    layout->object_kind = SOL_MIR_LAYOUT_OBJECT_SUM;
    layout->tag_offset = 0; layout->tag_size = 4;
    layout->payload_offset = payload_offset; layout->payload_size = maximum_size;
    uint64_t object_alignment = maximum_alignment > 4 ? maximum_alignment : 4;
    return finish_object(builder, layout, end, object_alignment);
}

static bool object_layout(LayoutBuilder *builder, size_t id) {
    SolMirLayout *out = builder->out;
    if (builder->object_state[id] == 2) return true;
    if (builder->object_state[id] == 1) return fail(builder,
        SOL_MIR_LAYOUT_BUILD_UNSUPPORTED, "layout wrapper object cycle detected");
    builder->object_state[id] = 1;
    if (!charge(builder, 1)) return fail(builder,
        SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED, "layout object work limit exceeded");
    const SolMirRecipe *recipe = &out->representation->recipes[id];
    SolMirTypeLayout *layout = &out->types[id];
    bool no_object = !recipe->inhabited || recipe->zero_sized;
    bool result = true;
    switch (recipe->kind) {
        case SOL_MIR_RECIPE_TEXT:
            result = header_object(builder, layout, SOL_MIR_LAYOUT_OBJECT_TEXT, 2);
            layout->data_handle_offset = 0;
            layout->length_offset = out->target.pointer_size;
            break;
        case SOL_MIR_RECIPE_FUNCTION: {
            bool environment = false;
            for (size_t i = 0; i < out->representation->callable_producer_count; ++i) {
                if (!charge(builder, 1)) return fail(builder,
                    SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
                    "layout callable producer work limit exceeded");
                const SolMirCallableProducer *producer
                    = &out->representation->callable_producers[i];
                environment |= producer->function_recipe == id
                    && producer->kind == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION;
            }
            result = header_object(builder, layout,
                SOL_MIR_LAYOUT_OBJECT_CALLABLE, environment ? 2 : 1);
            layout->target_token_offset = 0;
            if (environment)
                layout->environment_handle_offset = out->target.pointer_size;
            break;
        }
        case SOL_MIR_RECIPE_CAPABILITY:
            result = header_object(builder, layout,
                SOL_MIR_LAYOUT_OBJECT_CAPABILITY, 2);
            layout->root_token_offset = 0;
            layout->private_source_handle_offset = out->target.pointer_size;
            break;
        case SOL_MIR_RECIPE_TUPLE: case SOL_MIR_RECIPE_RECORD:
            result = layout_product(builder, id); break;
        case SOL_MIR_RECIPE_ENUM: case SOL_MIR_RECIPE_OPTION:
        case SOL_MIR_RECIPE_RESULT:
            result = layout_sum(builder, id); break;
        case SOL_MIR_RECIPE_DISTINCT: case SOL_MIR_RECIPE_REFINED:
            if (recipe->backing >= out->type_count
                || !object_layout(builder, recipe->backing)) return false;
            {
                SolMirTypeLayout backing = out->types[recipe->backing];
                SolMirRecipeId nominal = layout->recipe;
                *layout = backing; layout->recipe = nominal;
            }
            break;
        case SOL_MIR_RECIPE_INT64: case SOL_MIR_RECIPE_BOOL:
        case SOL_MIR_RECIPE_UNIT: case SOL_MIR_RECIPE_NEVER:
            break;
    }
    if (result && no_object) {
        layout->object_kind = SOL_MIR_LAYOUT_OBJECT_NONE;
        layout->has_object = false;
        layout->object_size = 0; layout->object_alignment = 1;
        layout->tail_padding = 0;
        layout->tag_offset = layout->payload_offset
            = layout->data_handle_offset = layout->length_offset
            = layout->target_token_offset = layout->environment_handle_offset
            = layout->root_token_offset = layout->private_source_handle_offset
            = SOL_MIR_LAYOUT_OFFSET_NONE;
        layout->payload_size = 0;
    }
    if (result) builder->object_state[id] = 2;
    return result;
}

static bool projection_maps(LayoutBuilder *builder) {
    SolMirLayout *out = builder->out;
    const SolMirRepresentation *r = out->representation;
    const SolMirMaterialization *m = r->materialization;
    size_t consumed = 0;
    for (size_t p = 0; p < m->place_count; ++p) {
        const SolMirMaterializedPlace *place = &m->places[p];
        SolMirRecipeId base = place->root_type;
        if (place->projections.offset != consumed) return fail(builder,
            SOL_MIR_LAYOUT_BUILD_INVALID_REPRESENTATION,
            "materialized projections are not exactly source ordered");
        for (size_t i = 0; i < place->projections.count; ++i, ++consumed) {
            if (!charge(builder, 1)) return fail(builder,
                SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
                "layout projection work limit exceeded");
            const SolMirMaterializedProjection *projection
                = &m->projections[consumed];
            if (projection->kind != SOL_IR_PROJECTION_FIELD
                && projection->kind != SOL_IR_PROJECTION_TUPLE_FIELD)
                return fail(builder, SOL_MIR_LAYOUT_BUILD_UNSUPPORTED,
                    "index and dereference projections have no initial layout map");
            if (base >= r->recipe_count || projection->type >= r->recipe_count)
                return fail(builder, SOL_MIR_LAYOUT_BUILD_INVALID_REPRESENTATION,
                    "layout projection type is invalid");
            const SolMirRecipe *recipe = &r->recipes[base];
            bool tuple = projection->kind == SOL_IR_PROJECTION_TUPLE_FIELD;
            if ((tuple && recipe->kind != SOL_MIR_RECIPE_TUPLE)
                || (!tuple && recipe->kind != SOL_MIR_RECIPE_RECORD))
                return fail(builder, SOL_MIR_LAYOUT_BUILD_UNSUPPORTED,
                    "layout projection selects a non-product value");
            size_t found = SIZE_MAX;
            for (size_t f = 0; f < recipe->fields.count; ++f) {
                size_t field_id = recipe->fields.offset + f;
                const SolMirRecipeField *field = &r->fields[field_id];
                bool match = tuple
                    ? field->ordinal == projection->tuple_ordinal
                    : projection->tuple_ordinal == SOL_IR_NONE
                        && field->source_field == projection->source_field;
                if (match) { found = field_id; break; }
            }
            if (found == SIZE_MAX || r->fields[found].type != projection->type)
                return fail(builder, SOL_MIR_LAYOUT_BUILD_INVALID_REPRESENTATION,
                    "layout projection does not exactly match its source field");
            out->projections[consumed] = (SolMirProjectionMap){consumed, p,
                base, projection->type, found, out->fields[found].offset};
            base = projection->type;
        }
        if (base != place->final_type) return fail(builder,
            SOL_MIR_LAYOUT_BUILD_INVALID_REPRESENTATION,
            "layout projection final type is invalid");
    }
    return consumed == m->projection_count;
}

static bool derive_usage(LayoutBuilder *builder) {
    SolMirLayout *out = builder->out;
    out->usage.type_layouts = out->type_count;
    out->usage.field_layouts = out->field_count;
    out->usage.variant_layouts = out->variant_count;
    out->usage.projection_maps = out->projection_count;
    if (out->usage.build_scratch_bytes != out->type_count) return fail(builder,
        SOL_MIR_LAYOUT_BUILD_INTERNAL_FAILED,
        "layout build scratch accounting disagreed with preflight");
    size_t records = out->representation->usage.validation_work;
    if (!checked_add_size(&records, 1))
        return fail(builder, SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
            "layout validation work overflowed");
    if (!checked_add_size(&records, out->type_count)
        || !checked_add_size(&records, out->field_count)
        || !checked_add_size(&records, out->variant_count)
        || !checked_add_size(&records, out->projection_count)
        || !checked_add_size(&records, out->usage.build_work))
        return fail(builder, SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
            "layout validation work overflowed");
    out->usage.validation_work = records;
    out->usage.validation_scratch_bytes = out->usage.owned_bytes;
    if (!checked_add_size(&out->usage.validation_scratch_bytes,
            out->usage.build_scratch_bytes))
        return fail(builder, SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
            "layout validation scratch overflowed");
    if (out->representation->usage.validation_scratch_bytes
            > out->usage.validation_scratch_bytes)
        out->usage.validation_scratch_bytes
            = out->representation->usage.validation_scratch_bytes;
    if (out->usage.validation_work > out->limits.max_validation_work
        || out->usage.validation_scratch_bytes
            > out->limits.max_validation_scratch_bytes)
        return fail(builder, SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
            "layout validation resource limit exceeded");
    return true;
}

static SolMirLayoutBuildOutcome build_internal(
    const SolMirLayoutBuildRequest *request, SolMirLayout *output,
    SolDiagnostics *diagnostics, bool validate_source) {
    if (request == NULL || output == NULL || !owner_empty(output)
        || request->representation == NULL || request->target == NULL
        || (request->limits != NULL && !limits_zero(*request->limits)
            && !limits_complete(*request->limits))) {
        layout_error(diagnostics, "invalid layout build request or destination");
        return SOL_MIR_LAYOUT_BUILD_INVALID_ARGUMENT;
    }
    if (!sol_mir_target_descriptor_validate(request->target)) {
        layout_error(diagnostics, "invalid layout target descriptor");
        return SOL_MIR_LAYOUT_BUILD_INVALID_TARGET;
    }
    if (validate_source
        && !sol_mir_representation_validate(request->representation, diagnostics))
        return SOL_MIR_LAYOUT_BUILD_INVALID_REPRESENTATION;
    const SolMirRepresentation *r = request->representation;
    const SolMirMaterialization *m = r->materialization;
    SolMirLayout scratch; sol_mir_layout_init(&scratch);
    scratch.representation = r; scratch.target = *request->target;
    scratch.limits = request->limits == NULL || limits_zero(*request->limits)
        ? sol_mir_layout_default_limits() : *request->limits;
    LayoutBuilder builder = {&scratch, diagnostics,
        SOL_MIR_LAYOUT_BUILD_INTERNAL_FAILED, NULL};
    scratch.usage.build_scratch_bytes = r->recipe_count;
    if (scratch.usage.build_scratch_bytes
            > scratch.limits.max_build_scratch_bytes) {
        fail(&builder, SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
            "layout build scratch limit exceeded");
        return builder.outcome;
    }
    if (r->recipe_count > scratch.limits.max_type_layouts
        || r->field_count > scratch.limits.max_field_layouts
        || r->variant_count > scratch.limits.max_variant_layouts
        || m->projection_count > scratch.limits.max_projection_maps) {
        fail(&builder, SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
            "layout record limit exceeded");
        return builder.outcome;
    }
#define ALLOC(member, count_member, capacity_member, count) do { \
    scratch.member = allocate(&builder, (count), sizeof(*scratch.member)); \
    scratch.count_member = scratch.capacity_member = (count); \
    if ((count) != 0 && scratch.member == NULL) goto failed; \
} while (0)
    ALLOC(types, type_count, type_capacity, r->recipe_count);
    ALLOC(fields, field_count, field_capacity, r->field_count);
    ALLOC(variants, variant_count, variant_capacity, r->variant_count);
    ALLOC(projections, projection_count, projection_capacity,
        m->projection_count);
#undef ALLOC
    builder.object_state = scratch.type_count == 0 ? NULL
        : calloc(scratch.type_count, 1);
    if (scratch.type_count != 0 && builder.object_state == NULL) {
        builder.outcome = SOL_MIR_LAYOUT_BUILD_ALLOCATION_FAILED; goto failed;
    }
    for (size_t i = 0; i < scratch.type_count; ++i)
        if (!value_layout(&builder, i)) goto failed;
    for (size_t i = 0; i < scratch.type_count; ++i)
        if (!object_layout(&builder, i)) goto failed;
    if (!projection_maps(&builder) || !derive_usage(&builder)) goto failed;
    free(builder.object_state);
    *output = scratch;
    return SOL_MIR_LAYOUT_BUILD_SUCCEEDED;
failed:
    free(builder.object_state); sol_mir_layout_free(&scratch);
    return builder.outcome;
}

static bool same_target(SolMirTargetDescriptor a, SolMirTargetDescriptor b) {
    return a.pointer_size == b.pointer_size
        && a.pointer_alignment == b.pointer_alignment
        && a.int64_alignment == b.int64_alignment
        && a.endianness == b.endianness
        && a.max_object_bytes == b.max_object_bytes;
}

static bool same_limits(SolMirLayoutLimits a, SolMirLayoutLimits b) {
    return a.max_type_layouts == b.max_type_layouts
        && a.max_field_layouts == b.max_field_layouts
        && a.max_variant_layouts == b.max_variant_layouts
        && a.max_projection_maps == b.max_projection_maps
        && a.max_owned_bytes == b.max_owned_bytes
        && a.max_build_scratch_bytes == b.max_build_scratch_bytes
        && a.max_build_work == b.max_build_work
        && a.max_validation_scratch_bytes == b.max_validation_scratch_bytes
        && a.max_validation_work == b.max_validation_work;
}

static bool same_usage(SolMirLayoutUsage a, SolMirLayoutUsage b) {
    return a.type_layouts == b.type_layouts
        && a.field_layouts == b.field_layouts
        && a.variant_layouts == b.variant_layouts
        && a.projection_maps == b.projection_maps
        && a.owned_bytes == b.owned_bytes
        && a.build_scratch_bytes == b.build_scratch_bytes
        && a.build_work == b.build_work
        && a.validation_scratch_bytes == b.validation_scratch_bytes
        && a.validation_work == b.validation_work;
}

static bool same_type_layout(const SolMirTypeLayout *a,
    const SolMirTypeLayout *b) {
    return a->recipe == b->recipe
        && a->value_size == b->value_size
        && a->value_alignment == b->value_alignment
        && a->object_kind == b->object_kind
        && a->has_object == b->has_object
        && a->object_size == b->object_size
        && a->object_alignment == b->object_alignment
        && a->tail_padding == b->tail_padding
        && a->tag_offset == b->tag_offset
        && a->tag_size == b->tag_size
        && a->payload_offset == b->payload_offset
        && a->payload_size == b->payload_size
        && a->data_handle_offset == b->data_handle_offset
        && a->length_offset == b->length_offset
        && a->target_token_offset == b->target_token_offset
        && a->environment_handle_offset == b->environment_handle_offset
        && a->root_token_offset == b->root_token_offset
        && a->private_source_handle_offset == b->private_source_handle_offset;
}

static bool same_field_layout(const SolMirFieldLayout *a,
    const SolMirFieldLayout *b) {
    return a->field == b->field && a->owner_recipe == b->owner_recipe
        && a->variant == b->variant && a->has_storage == b->has_storage
        && a->offset == b->offset && a->size == b->size
        && a->alignment == b->alignment
        && a->padding_before == b->padding_before;
}

static bool same_variant_layout(const SolMirVariantLayout *a,
    const SolMirVariantLayout *b) {
    return a->variant == b->variant && a->owner_recipe == b->owner_recipe
        && a->tag == b->tag && a->inhabited == b->inhabited
        && a->has_payload_storage == b->has_payload_storage
        && a->payload_size == b->payload_size
        && a->payload_alignment == b->payload_alignment
        && a->tail_padding == b->tail_padding;
}

static bool same_projection_map(const SolMirProjectionMap *a,
    const SolMirProjectionMap *b) {
    return a->projection == b->projection && a->place == b->place
        && a->base_recipe == b->base_recipe
        && a->result_recipe == b->result_recipe
        && a->field_layout == b->field_layout
        && a->object_offset == b->object_offset;
}

static bool same_layout_records(const SolMirLayout *a,
    const SolMirLayout *b) {
    for (size_t i = 0; i < a->type_count; ++i)
        if (!same_type_layout(&a->types[i], &b->types[i])) return false;
    for (size_t i = 0; i < a->field_count; ++i)
        if (!same_field_layout(&a->fields[i], &b->fields[i])) return false;
    for (size_t i = 0; i < a->variant_count; ++i)
        if (!same_variant_layout(&a->variants[i], &b->variants[i])) return false;
    for (size_t i = 0; i < a->projection_count; ++i)
        if (!same_projection_map(&a->projections[i], &b->projections[i]))
            return false;
    return true;
}

static bool canonical(size_t count, size_t capacity, const void *pointer) {
    return count == capacity && ((count == 0) == (pointer == NULL));
}

static bool add_range(Range ranges[4], size_t *count, const void *pointer,
    size_t items, size_t item_size) {
    if (items == 0) return pointer == NULL;
    size_t bytes;
    if (pointer == NULL || !checked_mul_size(items, item_size, &bytes)) return false;
    uintptr_t start = (uintptr_t)pointer;
    if (bytes > UINTPTR_MAX - start) return false;
    Range next = {start, start + bytes};
    for (size_t i = 0; i < *count; ++i)
        if (next.start < ranges[i].end && ranges[i].start < next.end) return false;
    ranges[(*count)++] = next; return true;
}

static bool overlaps(const Range ranges[4], size_t count, const void *pointer,
    size_t items, size_t item_size) {
    if (items == 0) return pointer != NULL;
    size_t bytes;
    if (pointer == NULL || !checked_mul_size(items, item_size, &bytes)) return true;
    uintptr_t start = (uintptr_t)pointer;
    if (bytes > UINTPTR_MAX - start) return true;
    uintptr_t end = start + bytes;
    for (size_t i = 0; i < count; ++i)
        if (start < ranges[i].end && ranges[i].start < end) return true;
    return false;
}

static bool local_layout_checks(const SolMirLayout *layout) {
    for (size_t i = 0; i < layout->type_count; ++i) {
        const SolMirTypeLayout *type = &layout->types[i];
        if (type->recipe != i || !power_of_two(type->value_alignment)
            || !power_of_two(type->object_alignment)
            || (type->has_object
                && (type->object_size > layout->target.max_object_bytes
                    || type->object_size % type->object_alignment != 0))) return false;
    }
    for (size_t i = 0; i < layout->field_count; ++i) {
        const SolMirFieldLayout *field = &layout->fields[i];
        if (field->field != i || field->owner_recipe >= layout->type_count
            || !power_of_two(field->alignment)) return false;
        if (!field->has_storage) {
            if (field->offset != 0 || field->size != 0
                || field->alignment != 1 || field->padding_before != 0)
                return false;
            continue;
        }
        const SolMirTypeLayout *owner = &layout->types[field->owner_recipe];
        if (!owner->has_object || field->offset > owner->object_size
            || field->size > owner->object_size - field->offset) return false;
    }
    for (size_t i = 0; i < layout->variant_count; ++i) {
        const SolMirVariantLayout *variant = &layout->variants[i];
        if (variant->variant != i
            || variant->owner_recipe >= layout->type_count
            || !power_of_two(variant->payload_alignment)
            || variant->has_payload_storage != variant->inhabited) return false;
        if (!variant->has_payload_storage
            && (variant->payload_size != 0
                || variant->payload_alignment != 1
                || variant->tail_padding != 0)) return false;
    }
    for (size_t i = 0; i < layout->projection_count; ++i) {
        const SolMirProjectionMap *map = &layout->projections[i];
        if (map->projection != i || map->base_recipe >= layout->type_count
            || map->result_recipe >= layout->type_count
            || map->field_layout >= layout->field_count
            || !layout->fields[map->field_layout].has_storage
            || map->object_offset != layout->fields[map->field_layout].offset)
            return false;
    }
    return true;
}

bool sol_mir_layout_validate(const SolMirLayout *layout,
    SolDiagnostics *diagnostics) {
    if (layout == NULL || layout->representation == NULL
        || !sol_mir_target_descriptor_validate(&layout->target)
        || !limits_complete(layout->limits)
        || layout->usage.type_layouts != layout->type_count
        || layout->usage.field_layouts != layout->field_count
        || layout->usage.variant_layouts != layout->variant_count
        || layout->usage.projection_maps != layout->projection_count
        || layout->type_count > layout->limits.max_type_layouts
        || layout->field_count > layout->limits.max_field_layouts
        || layout->variant_count > layout->limits.max_variant_layouts
        || layout->projection_count > layout->limits.max_projection_maps
        || layout->usage.owned_bytes > layout->limits.max_owned_bytes
        || layout->usage.build_scratch_bytes
            > layout->limits.max_build_scratch_bytes
        || layout->usage.build_work > layout->limits.max_build_work
        || layout->usage.validation_scratch_bytes
            > layout->limits.max_validation_scratch_bytes
        || layout->usage.validation_work > layout->limits.max_validation_work)
        return layout_error(diagnostics, "layout header is malformed");
    if (!canonical(layout->type_count, layout->type_capacity, layout->types)
        || !canonical(layout->field_count, layout->field_capacity, layout->fields)
        || !canonical(layout->variant_count, layout->variant_capacity,
            layout->variants)
        || !canonical(layout->projection_count, layout->projection_capacity,
            layout->projections))
        return layout_error(diagnostics, "layout arenas are noncanonical");
    Range ranges[4]; size_t range_count = 0;
#define RANGE(member, capacity) do { \
    if (!add_range(ranges, &range_count, layout->member, \
            layout->capacity, sizeof(*layout->member))) goto malformed; \
} while (0)
    RANGE(types, type_capacity); RANGE(fields, field_capacity);
    RANGE(variants, variant_capacity); RANGE(projections, projection_capacity);
#undef RANGE
    const SolMirRepresentation *r = layout->representation;
#define BORROW(member, capacity) do { \
    if (overlaps(ranges, range_count, r->member, r->capacity, \
            sizeof(*r->member))) goto malformed; \
} while (0)
    BORROW(recipes, recipe_capacity); BORROW(fields, field_capacity);
    BORROW(variants, variant_capacity); BORROW(recipe_ids, recipe_id_capacity);
    BORROW(accesses, access_capacity); BORROW(receiver_roots, receiver_root_capacity);
    BORROW(callable_producers, callable_producer_capacity);
#undef BORROW
    const SolMirMaterialization *m = r->materialization;
#define M_BORROW(member, capacity) do { \
    if (m == NULL || overlaps(ranges, range_count, m->member, m->capacity, \
            sizeof(*m->member))) goto malformed; \
} while (0)
    M_BORROW(images, image_capacity); M_BORROW(types, type_capacity);
    M_BORROW(shape_fields, shape_field_capacity);
    M_BORROW(shape_variants, shape_variant_capacity);
    M_BORROW(type_ids, type_id_capacity); M_BORROW(accesses, access_capacity);
    M_BORROW(overlays, overlay_capacity); M_BORROW(contexts, context_capacity);
    M_BORROW(locals, local_capacity); M_BORROW(places, place_capacity);
    M_BORROW(projections, projection_capacity); M_BORROW(values, value_capacity);
    M_BORROW(instructions, instruction_capacity);
    M_BORROW(temporaries, temporary_capacity);
    M_BORROW(construct_operands, construct_operand_capacity);
    M_BORROW(call_arguments, call_argument_capacity); M_BORROW(blocks, block_capacity);
    M_BORROW(edges, edge_capacity); M_BORROW(edge_values, edge_value_capacity);
    M_BORROW(parameter_values, parameter_value_capacity); M_BORROW(loops, loop_capacity);
    M_BORROW(bindings, binding_capacity); M_BORROW(semantic_sites, semantic_site_capacity);
    M_BORROW(receiver_roots, receiver_root_capacity); M_BORROW(imports, import_capacity);
    M_BORROW(handlers, handler_capacity); M_BORROW(writebacks, writeback_capacity);
    M_BORROW(effect_rows, effect_row_capacity); M_BORROW(effect_atoms, effect_atom_capacity);
    M_BORROW(effect_row_atoms, effect_row_atom_capacity);
    M_BORROW(effect_names, effect_name_capacity);
    M_BORROW(literal_bytes, literal_byte_capacity);
#undef M_BORROW
    const SolMirPlan *plan = m->plan;
#define P_BORROW(member, capacity) do { \
    if (plan == NULL || overlaps(ranges, range_count, plan->member, \
            plan->capacity, sizeof(*plan->member))) goto malformed; \
} while (0)
    P_BORROW(types, type_capacity);
    P_BORROW(type_components, type_component_capacity);
    P_BORROW(type_parameter_accesses, type_parameter_access_capacity);
    P_BORROW(effect_atoms, effect_atom_capacity);
    P_BORROW(effect_rows, effect_row_capacity);
    P_BORROW(effect_row_atoms, effect_row_atom_capacity);
    P_BORROW(instances, instance_capacity);
    P_BORROW(instance_type_ids, instance_type_id_capacity);
    P_BORROW(instance_accesses, instance_access_capacity);
    P_BORROW(dictionary_entries, dictionary_entry_capacity);
    P_BORROW(imports, import_capacity); P_BORROW(typed_uses, typed_use_capacity);
    P_BORROW(contexts, context_capacity); P_BORROW(demands, demand_capacity);
#undef P_BORROW
    const SolMirProgram *program = plan->program;
#define PROGRAM_BORROW(member, count) do { \
    if (program == NULL || overlaps(ranges, range_count, program->member, \
            program->count, sizeof(*program->member))) goto malformed; \
} while (0)
    PROGRAM_BORROW(roots, root_count);
    PROGRAM_BORROW(approved_imports, approved_import_count);
    PROGRAM_BORROW(templates, template_count); PROGRAM_BORROW(imports, import_count);
    PROGRAM_BORROW(specializations, specialization_count);
    PROGRAM_BORROW(references, reference_count);
#undef PROGRAM_BORROW
    const SolIr *ir = program->ir;
#define IR_BORROW(member, count) do { \
    if (ir == NULL || overlaps(ranges, range_count, ir->member, ir->count, \
            sizeof(*ir->member))) goto malformed; \
} while (0)
    IR_BORROW(types, type_count); IR_BORROW(type_ids, type_id_count);
    IR_BORROW(accesses, access_count); IR_BORROW(definitions, definition_count);
    IR_BORROW(callables, callable_count); IR_BORROW(members, member_count);
    IR_BORROW(evidence, evidence_count); IR_BORROW(locals, local_count);
    IR_BORROW(fields, field_count); IR_BORROW(variants, variant_count);
    IR_BORROW(expressions, expression_count); IR_BORROW(places, place_count);
    IR_BORROW(projections, projection_count); IR_BORROW(statements, statement_count);
    IR_BORROW(statement_ids, statement_id_count); IR_BORROW(arms, arm_count);
    IR_BORROW(arm_ids, arm_id_count); IR_BORROW(patterns, pattern_count);
    IR_BORROW(pattern_children, pattern_child_count); IR_BORROW(operands, operand_count);
    IR_BORROW(roots, root_count); IR_BORROW(cleanup_locals, cleanup_local_count);
    IR_BORROW(effects, effect_count);
    IR_BORROW(generic_parameters, generic_parameter_count);
    IR_BORROW(effect_parameters, effect_parameter_count);
    IR_BORROW(obligations, obligation_count); IR_BORROW(snapshots, snapshot_count);
    IR_BORROW(loop_obligations, loop_obligation_count);
    IR_BORROW(unreachable_obligations, unreachable_obligation_count);
    IR_BORROW(files, file_count);
#undef IR_BORROW
    if (!sol_mir_representation_validate(r, diagnostics)) return false;
    SolMirLayout expected; sol_mir_layout_init(&expected);
    SolMirLayoutBuildRequest request = {r, &layout->target, &layout->limits};
    if (build_internal(&request, &expected, NULL, false)
            != SOL_MIR_LAYOUT_BUILD_SUCCEEDED) goto malformed;
    bool equal = same_target(layout->target, expected.target)
        && same_limits(layout->limits, expected.limits)
        && same_usage(layout->usage, expected.usage)
        && layout->type_count == expected.type_count
        && layout->field_count == expected.field_count
        && layout->variant_count == expected.variant_count
        && layout->projection_count == expected.projection_count
        && same_layout_records(layout, &expected)
        && local_layout_checks(layout);
    sol_mir_layout_free(&expected);
    if (equal) return true;
malformed:
    return layout_error(diagnostics, "layout graph or owned arenas are malformed");
}

SolMirLayoutBuildOutcome sol_mir_layout_build(
    const SolMirLayoutBuildRequest *request, SolMirLayout *layout,
    SolDiagnostics *diagnostics) {
    SolMirLayoutBuildOutcome outcome
        = build_internal(request, layout, diagnostics, true);
    if (outcome == SOL_MIR_LAYOUT_BUILD_SUCCEEDED
        && !sol_mir_layout_validate(layout, diagnostics)) {
        sol_mir_layout_free(layout);
        return diagnostics != NULL && diagnostics->allocation_failed
            ? SOL_MIR_LAYOUT_BUILD_ALLOCATION_FAILED
            : SOL_MIR_LAYOUT_BUILD_INTERNAL_FAILED;
    }
    return outcome;
}

static void format(Buffer *buffer, const char *pattern, ...) {
    if (buffer->failed) return;
    va_list args; va_start(args, pattern); va_list copy; va_copy(copy, args);
    int count = vsnprintf(NULL, 0, pattern, copy); va_end(copy);
    if (count < 0 || (size_t)count > SIZE_MAX - buffer->length - 1) {
        buffer->failed = true; va_end(args); return;
    }
    size_t needed = buffer->length + (size_t)count + 1;
    if (needed > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
            capacity *= 2;
        }
        char *grown = realloc(buffer->data, capacity);
        if (grown == NULL) { buffer->failed = true; va_end(args); return; }
        buffer->data = grown; buffer->capacity = capacity;
    }
    (void)vsnprintf(buffer->data + buffer->length,
        buffer->capacity - buffer->length, pattern, args);
    va_end(args); buffer->length += (size_t)count;
}

bool sol_mir_layout_render(FILE *stream, const SolMirLayout *layout) {
    if (stream == NULL || !sol_mir_layout_validate(layout, NULL)) return false;
    Buffer buffer = {0};
    format(&buffer, "mir_layout ptr=%" PRIu64 "/%" PRIu64
        " i64_align=%" PRIu64 " endian=%d max=%" PRIu64
        " types=%zu fields=%zu variants=%zu projections=%zu\n",
        layout->target.pointer_size, layout->target.pointer_alignment,
        layout->target.int64_alignment, (int)layout->target.endianness,
        layout->target.max_object_bytes, layout->type_count,
        layout->field_count, layout->variant_count, layout->projection_count);
    for (size_t i = 0; i < layout->type_count; ++i) {
        const SolMirTypeLayout *type = &layout->types[i];
        format(&buffer, "type r%zu value=%" PRIu64 "/%" PRIu64
            " object=%d:%d:%" PRIu64 "/%" PRIu64
            " tail=%" PRIu64 " tag=%" PRIu64 "/%" PRIu64
            " payload=%" PRIu64 "/%" PRIu64 " headers=%" PRIu64
            ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%" PRIu64 "\n", i, type->value_size, type->value_alignment,
            (int)type->object_kind, type->has_object, type->object_size,
            type->object_alignment, type->tail_padding, type->tag_offset,
            type->tag_size, type->payload_offset, type->payload_size,
            type->data_handle_offset, type->length_offset,
            type->target_token_offset, type->environment_handle_offset,
            type->root_token_offset, type->private_source_handle_offset);
    }
    for (size_t i = 0; i < layout->field_count; ++i) {
        const SolMirFieldLayout *field = &layout->fields[i];
        format(&buffer, "field f%zu owner=r%zu variant=%zu storage=%d offset=%" PRIu64
            " size=%" PRIu64 " align=%" PRIu64 " padding=%" PRIu64 "\n",
            i, field->owner_recipe, field->variant, field->has_storage,
            field->offset,
            field->size, field->alignment, field->padding_before);
    }
    for (size_t i = 0; i < layout->variant_count; ++i) {
        const SolMirVariantLayout *variant = &layout->variants[i];
        format(&buffer, "variant v%zu owner=r%zu tag=%" PRIu32
            " inhabited=%d payload_storage=%d payload=%" PRIu64 "/%" PRIu64
            " tail=%" PRIu64 "\n", i, variant->owner_recipe, variant->tag,
            variant->inhabited, variant->has_payload_storage,
            variant->payload_size,
            variant->payload_alignment, variant->tail_padding);
    }
    for (size_t i = 0; i < layout->projection_count; ++i) {
        const SolMirProjectionMap *map = &layout->projections[i];
        format(&buffer, "projection p%zu place=%zu base=r%zu result=r%zu"
            " field=f%zu offset=%" PRIu64 "\n", i, map->place,
            map->base_recipe, map->result_recipe, map->field_layout,
            map->object_offset);
    }
    bool ok = !buffer.failed
        && (buffer.length == 0 || fwrite(buffer.data, buffer.length, 1, stream) == 1);
    free(buffer.data); return ok;
}
