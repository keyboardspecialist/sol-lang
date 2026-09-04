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

typedef struct { char *data; size_t length, capacity; bool failed; } Buffer;

/* Validation plumbing implemented by the independent validator. The builder
   calls it only for its freshly constructed, validated-source graph. */
bool sol_mir_layout_internal_requirements(const SolMirLayout *layout,
    size_t *work, size_t *scratch);

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
    if (!sol_mir_layout_internal_requirements(out,
            &out->usage.validation_work,
            &out->usage.validation_scratch_bytes))
        return fail(builder, SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED,
            "layout validation requirements overflowed");
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
