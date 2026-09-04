#include "sol/mir_layout.h"

#include <stdint.h>
#include <string.h>

typedef struct {
    uintptr_t start;
    uintptr_t end;
} LayoutRange;

static bool invalid(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) {
        sol_diagnostics_add(diagnostics, "SOL-MIR-LAYOUT-001",
            SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    }
    return false;
}

static bool add_size(size_t *value, size_t amount) {
    if (amount > SIZE_MAX - *value) return false;
    *value += amount;
    return true;
}

static bool multiply_size(size_t left, size_t right, size_t *result) {
    if (left != 0 && right > SIZE_MAX / left) return false;
    *result = left * right;
    return true;
}

static bool add_u64(uint64_t left, uint64_t right, uint64_t *result) {
    if (right > UINT64_MAX - left) return false;
    *result = left + right;
    return true;
}

static bool power_of_two(uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static bool align_u64(uint64_t value, uint64_t alignment, uint64_t *result) {
    if (!power_of_two(alignment)) return false;
    uint64_t mask = alignment - 1;
    if (value > UINT64_MAX - mask) return false;
    *result = (value + mask) & ~mask;
    return true;
}

static bool valid_target(const SolMirTargetDescriptor *target) {
    if (target == NULL || (target->pointer_size != 4
            && target->pointer_size != 8)) return false;
    uint64_t addressable = target->pointer_size == 4
        ? (uint64_t)UINT32_MAX : UINT64_MAX;
    return power_of_two(target->pointer_alignment)
        && target->pointer_size % target->pointer_alignment == 0
        && power_of_two(target->int64_alignment)
        && 8 % target->int64_alignment == 0
        && (target->endianness == SOL_MIR_ENDIAN_LITTLE
            || target->endianness == SOL_MIR_ENDIAN_BIG)
        && target->max_object_bytes != 0
        && target->max_object_bytes <= addressable;
}

static bool complete_limits(SolMirLayoutLimits limits) {
    return limits.max_type_layouts != 0 && limits.max_field_layouts != 0
        && limits.max_variant_layouts != 0
        && limits.max_projection_maps != 0 && limits.max_owned_bytes != 0
        && limits.max_build_scratch_bytes != 0 && limits.max_build_work != 0
        && limits.max_validation_scratch_bytes != 0
        && limits.max_validation_work != 0;
}

static bool canonical(size_t count, size_t capacity, const void *pointer) {
    return count == capacity && ((count == 0) == (pointer == NULL));
}

static bool add_owned(LayoutRange ranges[4], size_t *range_count,
    const void *pointer, size_t count, size_t item_size) {
    if (count == 0) return pointer == NULL;
    size_t bytes;
    if (pointer == NULL || !multiply_size(count, item_size, &bytes)) return false;
    uintptr_t start = (uintptr_t)pointer;
    if (bytes > UINTPTR_MAX - start) return false;
    LayoutRange next = {start, start + bytes};
    for (size_t i = 0; i < *range_count; ++i) {
        if (next.start < ranges[i].end && ranges[i].start < next.end)
            return false;
    }
    ranges[(*range_count)++] = next;
    return true;
}

static bool borrowed_overlaps(const LayoutRange ranges[4], size_t range_count,
    const void *pointer, size_t count, size_t item_size) {
    if (count == 0) return pointer != NULL;
    size_t bytes;
    if (pointer == NULL || !multiply_size(count, item_size, &bytes)) return true;
    uintptr_t start = (uintptr_t)pointer;
    if (bytes > UINTPTR_MAX - start) return true;
    uintptr_t end = start + bytes;
    for (size_t i = 0; i < range_count; ++i) {
        if (start < ranges[i].end && ranges[i].start < end) return true;
    }
    return false;
}

static bool persistent_bytes(const SolMirLayout *layout, size_t *result) {
    size_t total = 0, bytes;
#define ADD_BYTES(capacity, member) do { \
    if (!multiply_size((capacity), sizeof(*layout->member), &bytes) \
        || !add_size(&total, bytes)) return false; \
} while (0)
    ADD_BYTES(layout->type_capacity, types);
    ADD_BYTES(layout->field_capacity, fields);
    ADD_BYTES(layout->variant_capacity, variants);
    ADD_BYTES(layout->projection_capacity, projections);
#undef ADD_BYTES
    *result = total;
    return true;
}

static bool derive_build_work(const SolMirLayout *layout, size_t *result) {
    const SolMirRepresentation *r = layout->representation;
    size_t work = 0;
    for (size_t i = 0; i < r->recipe_count; ++i) {
        const SolMirRecipe *recipe = &r->recipes[i];
        if (!add_size(&work, 2)) return false;
        if (recipe->inhabited && !recipe->zero_sized) {
            size_t current = i;
            for (size_t depth = 0; depth <= r->recipe_count; ++depth) {
                if (!add_size(&work, 1) || current >= r->recipe_count)
                    return false;
                SolMirRecipeKind kind = r->recipes[current].kind;
                if (kind != SOL_MIR_RECIPE_DISTINCT
                    && kind != SOL_MIR_RECIPE_REFINED) break;
                current = r->recipes[current].backing;
                if (depth == r->recipe_count) return false;
            }
        }
        switch (recipe->kind) {
            case SOL_MIR_RECIPE_TUPLE: case SOL_MIR_RECIPE_RECORD:
                if (!add_size(&work, recipe->fields.count)) return false;
                break;
            case SOL_MIR_RECIPE_ENUM: case SOL_MIR_RECIPE_OPTION:
            case SOL_MIR_RECIPE_RESULT:
                if (!add_size(&work, recipe->variants.count)) return false;
                for (size_t v = 0; v < recipe->variants.count; ++v) {
                    size_t variant = recipe->variants.offset + v;
                    if (variant >= r->variant_count
                        || !add_size(&work, r->variants[variant].fields.count))
                        return false;
                }
                break;
            case SOL_MIR_RECIPE_FUNCTION:
                if (!add_size(&work, r->callable_producer_count)) return false;
                break;
            case SOL_MIR_RECIPE_INT64: case SOL_MIR_RECIPE_BOOL:
            case SOL_MIR_RECIPE_TEXT: case SOL_MIR_RECIPE_UNIT:
            case SOL_MIR_RECIPE_NEVER: case SOL_MIR_RECIPE_DISTINCT:
            case SOL_MIR_RECIPE_REFINED: case SOL_MIR_RECIPE_CAPABILITY:
                break;
            default: return false;
        }
    }
    if (!add_size(&work, layout->projection_count)) return false;
    *result = work;
    return true;
}

bool sol_mir_layout_internal_requirements(const SolMirLayout *layout,
    size_t *work, size_t *scratch) {
    if (layout == NULL || layout->representation == NULL || work == NULL
        || scratch == NULL) return false;
    size_t build_work;
    if (!derive_build_work(layout, &build_work)) return false;
    size_t total = layout->representation->usage.validation_work;
    if (!add_size(&total, 1) || !add_size(&total, layout->type_count)
        || !add_size(&total, layout->field_count)
        || !add_size(&total, layout->variant_count)
        || !add_size(&total, layout->projection_count)
        || !add_size(&total, build_work)) return false;
    *work = total;
    *scratch = layout->representation->usage.validation_scratch_bytes;
    return true;
}

static bool preflight_owner_chain(const SolMirLayout *layout,
    const LayoutRange owned[4], size_t owned_count) {
    const SolMirRepresentation *r = layout->representation;
    if (borrowed_overlaps(owned, owned_count, r, 1, sizeof(*r))) return false;
    const SolMirMaterialization *m = r->materialization;
    if (m == NULL
        || borrowed_overlaps(owned, owned_count, m, 1, sizeof(*m))) return false;
    const SolMirPlan *p = m->plan;
    if (p == NULL
        || borrowed_overlaps(owned, owned_count, p, 1, sizeof(*p))) return false;
    const SolMirProgram *program = p->program;
    if (program == NULL
        || borrowed_overlaps(owned, owned_count, program, 1,
            sizeof(*program))) return false;
    const SolIr *ir = program->ir;
    return ir != NULL
        && !borrowed_overlaps(owned, owned_count, ir, 1, sizeof(*ir));
}

static bool reject_borrowed_overlap(const SolMirLayout *layout,
    const LayoutRange owned[4], size_t owned_count) {
    const SolMirRepresentation *r = layout->representation;
#define R_RANGE(member, capacity) do { \
    if (borrowed_overlaps(owned, owned_count, r->member, r->capacity, \
            sizeof(*r->member))) return false; \
} while (0)
    R_RANGE(recipes, recipe_capacity); R_RANGE(fields, field_capacity);
    R_RANGE(variants, variant_capacity); R_RANGE(recipe_ids, recipe_id_capacity);
    R_RANGE(accesses, access_capacity); R_RANGE(receiver_roots, receiver_root_capacity);
    R_RANGE(callable_producers, callable_producer_capacity);
#undef R_RANGE
    const SolMirMaterialization *m = r->materialization;
#define M_RANGE(member, capacity) do { \
    if (borrowed_overlaps(owned, owned_count, m->member, m->capacity, \
            sizeof(*m->member))) return false; \
} while (0)
    M_RANGE(images, image_capacity); M_RANGE(types, type_capacity);
    M_RANGE(shape_fields, shape_field_capacity); M_RANGE(shape_variants, shape_variant_capacity);
    M_RANGE(type_ids, type_id_capacity); M_RANGE(accesses, access_capacity);
    M_RANGE(overlays, overlay_capacity); M_RANGE(contexts, context_capacity);
    M_RANGE(locals, local_capacity); M_RANGE(places, place_capacity);
    M_RANGE(projections, projection_capacity); M_RANGE(values, value_capacity);
    M_RANGE(instructions, instruction_capacity); M_RANGE(temporaries, temporary_capacity);
    M_RANGE(construct_operands, construct_operand_capacity);
    M_RANGE(call_arguments, call_argument_capacity); M_RANGE(blocks, block_capacity);
    M_RANGE(edges, edge_capacity); M_RANGE(edge_values, edge_value_capacity);
    M_RANGE(parameter_values, parameter_value_capacity); M_RANGE(loops, loop_capacity);
    M_RANGE(bindings, binding_capacity); M_RANGE(semantic_sites, semantic_site_capacity);
    M_RANGE(receiver_roots, receiver_root_capacity); M_RANGE(imports, import_capacity);
    M_RANGE(handlers, handler_capacity); M_RANGE(writebacks, writeback_capacity);
    M_RANGE(effect_rows, effect_row_capacity); M_RANGE(effect_atoms, effect_atom_capacity);
    M_RANGE(effect_row_atoms, effect_row_atom_capacity); M_RANGE(effect_names, effect_name_capacity);
    M_RANGE(literal_bytes, literal_byte_capacity);
#undef M_RANGE
    const SolMirPlan *p = m->plan;
#define P_RANGE(member, capacity) do { \
    if (borrowed_overlaps(owned, owned_count, p->member, p->capacity, \
            sizeof(*p->member))) return false; \
} while (0)
    P_RANGE(types, type_capacity); P_RANGE(type_components, type_component_capacity);
    P_RANGE(type_parameter_accesses, type_parameter_access_capacity);
    P_RANGE(effect_atoms, effect_atom_capacity); P_RANGE(effect_rows, effect_row_capacity);
    P_RANGE(effect_row_atoms, effect_row_atom_capacity); P_RANGE(instances, instance_capacity);
    P_RANGE(instance_type_ids, instance_type_id_capacity);
    P_RANGE(instance_accesses, instance_access_capacity);
    P_RANGE(dictionary_entries, dictionary_entry_capacity); P_RANGE(imports, import_capacity);
    P_RANGE(typed_uses, typed_use_capacity); P_RANGE(contexts, context_capacity);
    P_RANGE(demands, demand_capacity);
#undef P_RANGE
    const SolMirProgram *program = p->program;
#define PROGRAM_RANGE(member, count) do { \
    if (borrowed_overlaps(owned, owned_count, program->member, program->count, \
            sizeof(*program->member))) return false; \
} while (0)
    PROGRAM_RANGE(roots, root_count); PROGRAM_RANGE(approved_imports, approved_import_count);
    PROGRAM_RANGE(templates, template_count); PROGRAM_RANGE(imports, import_count);
    PROGRAM_RANGE(specializations, specialization_count); PROGRAM_RANGE(references, reference_count);
#undef PROGRAM_RANGE
#define MIR_RANGE(mir, member, capacity) do { \
    if (borrowed_overlaps(owned, owned_count, (mir)->member, (mir)->capacity, \
            sizeof(*(mir)->member))) return false; \
} while (0)
    for (size_t i = 0; i < program->template_count; ++i) {
        const SolMir *mir = &program->templates[i].mir;
        if (borrowed_overlaps(owned, owned_count, mir, 1, sizeof(*mir)))
            return false;
        MIR_RANGE(mir, blocks, block_capacity); MIR_RANGE(mir, instructions, instruction_capacity);
        MIR_RANGE(mir, values, value_capacity); MIR_RANGE(mir, parameter_values, parameter_value_capacity);
        MIR_RANGE(mir, edge_values, edge_value_capacity); MIR_RANGE(mir, call_arguments, call_argument_capacity);
        MIR_RANGE(mir, loops, loop_capacity); MIR_RANGE(mir, construct_operands, construct_operand_capacity);
        MIR_RANGE(mir, temporaries, temporary_capacity);
    }
    for (size_t i = 0; i < m->image_count; ++i) {
        const SolMir *mir = &m->images[i].topology;
        if (borrowed_overlaps(owned, owned_count, mir, 1, sizeof(*mir)))
            return false;
        MIR_RANGE(mir, blocks, block_capacity); MIR_RANGE(mir, instructions, instruction_capacity);
        MIR_RANGE(mir, values, value_capacity); MIR_RANGE(mir, parameter_values, parameter_value_capacity);
        MIR_RANGE(mir, edge_values, edge_value_capacity); MIR_RANGE(mir, call_arguments, call_argument_capacity);
        MIR_RANGE(mir, loops, loop_capacity); MIR_RANGE(mir, construct_operands, construct_operand_capacity);
        MIR_RANGE(mir, temporaries, temporary_capacity);
    }
#undef MIR_RANGE
    const SolIr *ir = program->ir;
#define IR_RANGE(member, count) do { \
    if (borrowed_overlaps(owned, owned_count, ir->member, ir->count, \
            sizeof(*ir->member))) return false; \
} while (0)
    IR_RANGE(types, type_count); IR_RANGE(type_ids, type_id_count); IR_RANGE(accesses, access_count);
    IR_RANGE(definitions, definition_count); IR_RANGE(callables, callable_count);
    IR_RANGE(members, member_count); IR_RANGE(evidence, evidence_count); IR_RANGE(locals, local_count);
    IR_RANGE(fields, field_count); IR_RANGE(variants, variant_count); IR_RANGE(expressions, expression_count);
    IR_RANGE(places, place_count); IR_RANGE(projections, projection_count); IR_RANGE(statements, statement_count);
    IR_RANGE(statement_ids, statement_id_count); IR_RANGE(arms, arm_count); IR_RANGE(arm_ids, arm_id_count);
    IR_RANGE(patterns, pattern_count); IR_RANGE(pattern_children, pattern_child_count);
    IR_RANGE(operands, operand_count); IR_RANGE(roots, root_count); IR_RANGE(cleanup_locals, cleanup_local_count);
    IR_RANGE(effects, effect_count); IR_RANGE(generic_parameters, generic_parameter_count);
    IR_RANGE(effect_parameters, effect_parameter_count); IR_RANGE(obligations, obligation_count);
    IR_RANGE(snapshots, snapshot_count); IR_RANGE(loop_obligations, loop_obligation_count);
    IR_RANGE(unreachable_obligations, unreachable_obligation_count); IR_RANGE(files, file_count);
#undef IR_RANGE
#define TEXT_RANGE(pointer, count) do { \
    if (borrowed_overlaps(owned, owned_count, (pointer), (count), sizeof(char))) \
        return false; \
} while (0)
    TEXT_RANGE(ir->source_path, strlen(ir->source_path) + 1);
    TEXT_RANGE(ir->source_bytes, ir->source_length + 1);
    for (size_t i = 0; i < p->effect_atom_count; ++i)
        TEXT_RANGE(p->effect_atoms[i].name, p->effect_atoms[i].length + 1);
    for (size_t i = 0; i < ir->definition_count; ++i)
        if (ir->definitions[i].name != NULL)
            TEXT_RANGE(ir->definitions[i].name, strlen(ir->definitions[i].name) + 1);
    for (size_t i = 0; i < ir->callable_count; ++i)
        if (ir->callables[i].name != NULL)
            TEXT_RANGE(ir->callables[i].name, strlen(ir->callables[i].name) + 1);
    for (size_t i = 0; i < ir->local_count; ++i)
        if (ir->locals[i].name != NULL)
            TEXT_RANGE(ir->locals[i].name, strlen(ir->locals[i].name) + 1);
    for (size_t i = 0; i < ir->field_count; ++i)
        if (ir->fields[i].name != NULL)
            TEXT_RANGE(ir->fields[i].name, strlen(ir->fields[i].name) + 1);
    for (size_t i = 0; i < ir->variant_count; ++i)
        if (ir->variants[i].name != NULL)
            TEXT_RANGE(ir->variants[i].name, strlen(ir->variants[i].name) + 1);
    for (size_t i = 0; i < ir->expression_count; ++i) {
        if (ir->expressions[i].kind == SOL_IR_EXPR_STRING)
            TEXT_RANGE(ir->expressions[i].as.string,
                strlen(ir->expressions[i].as.string) + 1);
        else if (ir->expressions[i].kind == SOL_IR_EXPR_HANDLE)
            TEXT_RANGE(ir->expressions[i].as.handler.effect_name,
                strlen(ir->expressions[i].as.handler.effect_name) + 1);
    }
    for (size_t i = 0; i < ir->statement_count; ++i)
        if (ir->statements[i].region_label != NULL)
            TEXT_RANGE(ir->statements[i].region_label,
                strlen(ir->statements[i].region_label) + 1);
    for (size_t i = 0; i < ir->effect_count; ++i)
        TEXT_RANGE(ir->effects[i].name, strlen(ir->effects[i].name) + 1);
    for (size_t i = 0; i < ir->generic_parameter_count; ++i)
        TEXT_RANGE(ir->generic_parameters[i].name,
            strlen(ir->generic_parameters[i].name) + 1);
    for (size_t i = 0; i < ir->effect_parameter_count; ++i)
        TEXT_RANGE(ir->effect_parameters[i].name,
            strlen(ir->effect_parameters[i].name) + 1);
    for (size_t i = 0; i < ir->file_count; ++i)
        TEXT_RANGE(ir->files[i].path, strlen(ir->files[i].path) + 1);
#undef TEXT_RANGE
    return true;
}

static SolMirTypeLayout blank_type(size_t recipe) {
    SolMirTypeLayout value;
    memset(&value, 0, sizeof(value));
    value.recipe = recipe;
    value.value_alignment = 1;
    value.object_alignment = 1;
    value.tag_offset = value.payload_offset = value.data_handle_offset
        = value.length_offset = value.target_token_offset
        = value.environment_handle_offset = value.root_token_offset
        = value.private_source_handle_offset = SOL_MIR_LAYOUT_OFFSET_NONE;
    return value;
}

static bool same_type(const SolMirTypeLayout *a, const SolMirTypeLayout *b) {
    return a->recipe == b->recipe && a->value_size == b->value_size
        && a->value_alignment == b->value_alignment
        && a->object_kind == b->object_kind && a->has_object == b->has_object
        && a->object_size == b->object_size
        && a->object_alignment == b->object_alignment
        && a->tail_padding == b->tail_padding && a->tag_offset == b->tag_offset
        && a->tag_size == b->tag_size && a->payload_offset == b->payload_offset
        && a->payload_size == b->payload_size
        && a->data_handle_offset == b->data_handle_offset
        && a->length_offset == b->length_offset
        && a->target_token_offset == b->target_token_offset
        && a->environment_handle_offset == b->environment_handle_offset
        && a->root_token_offset == b->root_token_offset
        && a->private_source_handle_offset == b->private_source_handle_offset;
}

static bool same_field(const SolMirFieldLayout *a,
    const SolMirFieldLayout *b) {
    return a->field == b->field && a->owner_recipe == b->owner_recipe
        && a->variant == b->variant && a->has_storage == b->has_storage
        && a->offset == b->offset && a->size == b->size
        && a->alignment == b->alignment
        && a->padding_before == b->padding_before;
}

static bool same_variant(const SolMirVariantLayout *a,
    const SolMirVariantLayout *b) {
    return a->variant == b->variant && a->owner_recipe == b->owner_recipe
        && a->tag == b->tag && a->inhabited == b->inhabited
        && a->has_payload_storage == b->has_payload_storage
        && a->payload_size == b->payload_size
        && a->payload_alignment == b->payload_alignment
        && a->tail_padding == b->tail_padding;
}

static bool derive_value(const SolMirLayout *layout, size_t id,
    SolMirTypeLayout *expected) {
    const SolMirRepresentation *r = layout->representation;
    *expected = blank_type(id);
    if (!r->recipes[id].inhabited || r->recipes[id].zero_sized) return true;
    size_t current = id;
    for (size_t depth = 0; depth <= r->recipe_count; ++depth) {
        if (current >= r->recipe_count) return false;
        const SolMirRecipe *recipe = &r->recipes[current];
        switch (recipe->kind) {
            case SOL_MIR_RECIPE_DISTINCT: case SOL_MIR_RECIPE_REFINED:
                current = recipe->backing;
                break;
            case SOL_MIR_RECIPE_INT64:
                expected->value_size = 8;
                expected->value_alignment = layout->target.int64_alignment;
                return true;
            case SOL_MIR_RECIPE_BOOL:
                expected->value_size = 1; expected->value_alignment = 1;
                return true;
            case SOL_MIR_RECIPE_TEXT: case SOL_MIR_RECIPE_TUPLE:
            case SOL_MIR_RECIPE_RECORD: case SOL_MIR_RECIPE_ENUM:
            case SOL_MIR_RECIPE_OPTION: case SOL_MIR_RECIPE_RESULT:
            case SOL_MIR_RECIPE_FUNCTION: case SOL_MIR_RECIPE_CAPABILITY:
                expected->value_size = layout->target.pointer_size;
                expected->value_alignment = layout->target.pointer_alignment;
                return true;
            case SOL_MIR_RECIPE_UNIT: case SOL_MIR_RECIPE_NEVER:
                return false;
            default: return false;
        }
    }
    return false;
}

static bool finish_object(const SolMirLayout *layout, SolMirTypeLayout *type,
    uint64_t end, uint64_t alignment) {
    uint64_t size;
    if (!align_u64(end, alignment, &size)
        || size > layout->target.max_object_bytes) return false;
    type->has_object = true; type->object_size = size;
    type->object_alignment = alignment; type->tail_padding = size - end;
    return true;
}

static bool derive_type(const SolMirLayout *, size_t, SolMirTypeLayout *, bool,
    size_t);

static bool derive_product(const SolMirLayout *layout, size_t id,
    SolMirTypeLayout *type, bool check_records) {
    const SolMirRepresentation *r = layout->representation;
    const SolMirRecipe *recipe = &r->recipes[id];
    bool physical = recipe->inhabited && !recipe->zero_sized;
    uint64_t end = 0, maximum_alignment = 1;
    for (size_t i = 0; i < recipe->fields.count; ++i) {
        size_t field_id = recipe->fields.offset + i;
        if (field_id >= r->field_count) return false;
        SolMirFieldLayout expected = {.field = field_id,
            .owner_recipe = id, .variant = SIZE_MAX, .alignment = 1};
        if (physical) {
            const SolMirRecipeField *field = &r->fields[field_id];
            if (field->type >= r->recipe_count) return false;
            SolMirTypeLayout value;
            if (!derive_value(layout, field->type, &value)) return false;
            uint64_t previous = end, offset;
            if (!align_u64(end, value.value_alignment, &offset)
                || !add_u64(offset, value.value_size, &end)) return false;
            expected.has_storage = true; expected.offset = offset;
            expected.size = value.value_size; expected.alignment = value.value_alignment;
            expected.padding_before = offset - previous;
            if (value.value_alignment > maximum_alignment)
                maximum_alignment = value.value_alignment;
        }
        if (check_records && !same_field(&layout->fields[field_id], &expected))
            return false;
    }
    if (!physical) return true;
    type->object_kind = SOL_MIR_LAYOUT_OBJECT_PRODUCT;
    return finish_object(layout, type, end, maximum_alignment);
}

static bool variant_inhabited(const SolMirRepresentation *r,
    const SolMirRecipeVariant *variant) {
    for (size_t i = 0; i < variant->fields.count; ++i) {
        size_t field = variant->fields.offset + i;
        if (field >= r->field_count
            || !r->recipes[r->fields[field].type].inhabited) return false;
    }
    return true;
}

static bool pack_variant(const SolMirLayout *layout, size_t owner,
    size_t variant_id, uint64_t payload_offset, bool absolute,
    bool check_records, uint64_t *payload_size, uint64_t *payload_alignment) {
    const SolMirRepresentation *r = layout->representation;
    const SolMirRecipeVariant *variant = &r->variants[variant_id];
    bool inhabited = variant_inhabited(r, variant);
    uint64_t end = 0, alignment = 1;
    for (size_t f = 0; f < variant->fields.count; ++f) {
        size_t field_id = variant->fields.offset + f;
        if (field_id >= r->field_count) return false;
        SolMirFieldLayout expected = {.field = field_id,
            .owner_recipe = owner, .variant = variant_id, .alignment = 1};
        if (inhabited) {
            const SolMirRecipeField *field = &r->fields[field_id];
            if (field->type >= r->recipe_count) return false;
            SolMirTypeLayout value;
            if (!derive_value(layout, field->type, &value)) return false;
            uint64_t previous = end, offset;
            if (!align_u64(end, value.value_alignment, &offset)
                || !add_u64(offset, value.value_size, &end)) return false;
            expected.has_storage = true;
            expected.offset = offset; expected.size = value.value_size;
            expected.alignment = value.value_alignment;
            expected.padding_before = offset - previous;
            if (absolute && !add_u64(expected.offset, payload_offset,
                    &expected.offset)) return false;
            if (value.value_alignment > alignment) alignment = value.value_alignment;
        }
        if (check_records && !same_field(&layout->fields[field_id], &expected))
            return false;
    }
    if (!align_u64(end, alignment, payload_size)) return false;
    *payload_alignment = alignment;
    return true;
}

static bool derive_sum(const SolMirLayout *layout, size_t id,
    SolMirTypeLayout *type, bool check_records) {
    const SolMirRepresentation *r = layout->representation;
    const SolMirRecipe *recipe = &r->recipes[id];
    uint64_t maximum_size = 0, maximum_alignment = 1;
    for (size_t i = 0; i < recipe->variants.count; ++i) {
        size_t variant_id = recipe->variants.offset + i;
        if (variant_id >= r->variant_count) return false;
        const SolMirRecipeVariant *variant = &r->variants[variant_id];
        if (variant->semantic_tag > UINT32_MAX) return false;
        uint64_t size, alignment;
        if (!pack_variant(layout, id, variant_id, 0, false,
                check_records && !recipe->inhabited,
                &size, &alignment)) return false;
        bool inhabited = variant_inhabited(r, variant);
        SolMirVariantLayout expected = {.variant = variant_id,
            .owner_recipe = id, .tag = (uint32_t)variant->semantic_tag,
            .inhabited = inhabited, .has_payload_storage = inhabited,
            .payload_size = size, .payload_alignment = alignment};
        if (inhabited) {
            uint64_t end = 0;
            for (size_t f = 0; f < variant->fields.count; ++f) {
                SolMirTypeLayout value;
                size_t field = variant->fields.offset + f;
                if (!derive_value(layout, r->fields[field].type, &value)
                    || !align_u64(end, value.value_alignment, &end)
                    || !add_u64(end, value.value_size, &end)) return false;
            }
            expected.tail_padding = size - end;
            if (size > maximum_size) maximum_size = size;
            if (alignment > maximum_alignment) maximum_alignment = alignment;
        }
        if (check_records
            && !same_variant(&layout->variants[variant_id], &expected)) return false;
    }
    if (!recipe->inhabited) return true;
    uint64_t payload_offset, end;
    if (!align_u64(4, maximum_alignment, &payload_offset)
        || !add_u64(payload_offset, maximum_size, &end)) return false;
    if (check_records) {
        for (size_t i = 0; i < recipe->variants.count; ++i) {
            size_t variant_id = recipe->variants.offset + i;
            uint64_t size, alignment;
            if (!pack_variant(layout, id, variant_id, payload_offset, true,
                    true, &size, &alignment)) return false;
        }
    }
    type->object_kind = SOL_MIR_LAYOUT_OBJECT_SUM;
    type->tag_offset = 0; type->tag_size = 4;
    type->payload_offset = payload_offset; type->payload_size = maximum_size;
    uint64_t alignment = maximum_alignment > 4 ? maximum_alignment : 4;
    return finish_object(layout, type, end, alignment);
}

static bool header_object(const SolMirLayout *layout, SolMirTypeLayout *type,
    SolMirLayoutObjectKind kind, uint64_t fields) {
    uint64_t end;
    if (fields > UINT64_MAX / layout->target.pointer_size) return false;
    end = fields * layout->target.pointer_size;
    type->object_kind = kind;
    return finish_object(layout, type, end, layout->target.pointer_alignment);
}

static bool derive_type(const SolMirLayout *layout, size_t id,
    SolMirTypeLayout *expected, bool check_records, size_t depth) {
    const SolMirRepresentation *r = layout->representation;
    if (id >= r->recipe_count || depth > r->recipe_count
        || !derive_value(layout, id, expected)) return false;
    const SolMirRecipe *recipe = &r->recipes[id];
    if (!recipe->inhabited || recipe->zero_sized) {
        if (recipe->kind == SOL_MIR_RECIPE_TUPLE
            || recipe->kind == SOL_MIR_RECIPE_RECORD)
            return derive_product(layout, id, expected, check_records);
        if (recipe->kind == SOL_MIR_RECIPE_ENUM
            || recipe->kind == SOL_MIR_RECIPE_OPTION
            || recipe->kind == SOL_MIR_RECIPE_RESULT)
            return derive_sum(layout, id, expected, check_records);
        return true;
    }
    switch (recipe->kind) {
        case SOL_MIR_RECIPE_TEXT:
            if (!header_object(layout, expected, SOL_MIR_LAYOUT_OBJECT_TEXT, 2))
                return false;
            expected->data_handle_offset = 0;
            expected->length_offset = layout->target.pointer_size;
            return true;
        case SOL_MIR_RECIPE_FUNCTION: {
            bool environment = false;
            for (size_t i = 0; i < r->callable_producer_count; ++i) {
                const SolMirCallableProducer *producer = &r->callable_producers[i];
                if (producer->function_recipe == id
                    && producer->kind == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION)
                    environment = true;
            }
            if (!header_object(layout, expected, SOL_MIR_LAYOUT_OBJECT_CALLABLE,
                    environment ? 2 : 1)) return false;
            expected->target_token_offset = 0;
            if (environment)
                expected->environment_handle_offset = layout->target.pointer_size;
            return true;
        }
        case SOL_MIR_RECIPE_CAPABILITY:
            if (!header_object(layout, expected,
                    SOL_MIR_LAYOUT_OBJECT_CAPABILITY, 2)) return false;
            expected->root_token_offset = 0;
            expected->private_source_handle_offset = layout->target.pointer_size;
            return true;
        case SOL_MIR_RECIPE_TUPLE: case SOL_MIR_RECIPE_RECORD:
            return derive_product(layout, id, expected, check_records);
        case SOL_MIR_RECIPE_ENUM: case SOL_MIR_RECIPE_OPTION:
        case SOL_MIR_RECIPE_RESULT:
            return derive_sum(layout, id, expected, check_records);
        case SOL_MIR_RECIPE_DISTINCT: case SOL_MIR_RECIPE_REFINED: {
            SolMirTypeLayout backing;
            if (!derive_type(layout, recipe->backing, &backing, false, depth + 1))
                return false;
            SolMirRecipeId nominal = expected->recipe;
            *expected = backing; expected->recipe = nominal;
            return true;
        }
        case SOL_MIR_RECIPE_INT64: case SOL_MIR_RECIPE_BOOL:
        case SOL_MIR_RECIPE_UNIT: case SOL_MIR_RECIPE_NEVER:
            return true;
        default: return false;
    }
}

static bool validate_records(const SolMirLayout *layout) {
    const SolMirRepresentation *r = layout->representation;
    size_t field_at = 0, variant_at = 0;
    for (size_t i = 0; i < r->recipe_count; ++i) {
        const SolMirRecipe *recipe = &r->recipes[i];
        switch (recipe->kind) {
            case SOL_MIR_RECIPE_TUPLE: case SOL_MIR_RECIPE_RECORD:
                if (recipe->fields.offset != field_at
                    || !add_size(&field_at, recipe->fields.count)
                    || field_at > r->field_count) return false;
                break;
            case SOL_MIR_RECIPE_ENUM: case SOL_MIR_RECIPE_OPTION:
            case SOL_MIR_RECIPE_RESULT:
                if (recipe->variants.offset != variant_at) return false;
                for (size_t v = 0; v < recipe->variants.count; ++v) {
                    if (variant_at >= r->variant_count
                        || r->variants[variant_at].fields.offset != field_at
                        || !add_size(&field_at,
                            r->variants[variant_at].fields.count)
                        || field_at > r->field_count) return false;
                    ++variant_at;
                }
                break;
            case SOL_MIR_RECIPE_INT64: case SOL_MIR_RECIPE_BOOL:
            case SOL_MIR_RECIPE_TEXT: case SOL_MIR_RECIPE_UNIT:
            case SOL_MIR_RECIPE_NEVER: case SOL_MIR_RECIPE_DISTINCT:
            case SOL_MIR_RECIPE_REFINED: case SOL_MIR_RECIPE_FUNCTION:
            case SOL_MIR_RECIPE_CAPABILITY:
                break;
            default: return false;
        }
        SolMirTypeLayout expected;
        if (!derive_type(layout, i, &expected, true, 0)
            || !same_type(&layout->types[i], &expected)) return false;
    }
    return field_at == layout->field_count
        && variant_at == layout->variant_count;
}

static bool validate_projections(const SolMirLayout *layout) {
    const SolMirRepresentation *r = layout->representation;
    const SolMirMaterialization *m = r->materialization;
    size_t consumed = 0;
    for (size_t p = 0; p < m->place_count; ++p) {
        const SolMirMaterializedPlace *place = &m->places[p];
        SolMirRecipeId base = place->root_type;
        if (place->projections.offset != consumed) return false;
        for (size_t i = 0; i < place->projections.count; ++i, ++consumed) {
            if (consumed >= m->projection_count || consumed >= layout->projection_count
                || base >= r->recipe_count) return false;
            const SolMirMaterializedProjection *projection = &m->projections[consumed];
            bool tuple;
            switch (projection->kind) {
                case SOL_IR_PROJECTION_FIELD: tuple = false; break;
                case SOL_IR_PROJECTION_TUPLE_FIELD: tuple = true; break;
                case SOL_IR_PROJECTION_INDEX: case SOL_IR_PROJECTION_DEREFERENCE:
                default: return false;
            }
            const SolMirRecipe *recipe = &r->recipes[base];
            if ((tuple && recipe->kind != SOL_MIR_RECIPE_TUPLE)
                || (!tuple && recipe->kind != SOL_MIR_RECIPE_RECORD)) return false;
            size_t found = SIZE_MAX, matches = 0;
            for (size_t f = 0; f < recipe->fields.count; ++f) {
                size_t field_id = recipe->fields.offset + f;
                const SolMirRecipeField *field = &r->fields[field_id];
                bool match = tuple ? field->ordinal == projection->tuple_ordinal
                    : projection->tuple_ordinal == SOL_IR_NONE
                        && field->source_field == projection->source_field;
                if (match) { found = field_id; ++matches; }
            }
            if (matches != 1 || projection->type >= r->recipe_count
                || r->fields[found].type != projection->type
                || !layout->fields[found].has_storage) return false;
            const SolMirProjectionMap *actual = &layout->projections[consumed];
            if (actual->projection != consumed || actual->place != p
                || actual->base_recipe != base
                || actual->result_recipe != projection->type
                || actual->field_layout != found
                || actual->object_offset != layout->fields[found].offset)
                return false;
            base = projection->type;
        }
        if (base != place->final_type) return false;
    }
    return consumed == m->projection_count
        && consumed == layout->projection_count;
}

bool sol_mir_layout_validate(const SolMirLayout *layout,
    SolDiagnostics *diagnostics) {
    if (layout == NULL || layout->representation == NULL
        || !valid_target(&layout->target) || !complete_limits(layout->limits)
        || layout->type_count > layout->limits.max_type_layouts
        || layout->field_count > layout->limits.max_field_layouts
        || layout->variant_count > layout->limits.max_variant_layouts
        || layout->projection_count > layout->limits.max_projection_maps
        || layout->usage.owned_bytes > layout->limits.max_owned_bytes
        || layout->usage.build_scratch_bytes > layout->limits.max_build_scratch_bytes
        || layout->usage.build_work > layout->limits.max_build_work
        || layout->usage.validation_scratch_bytes
            > layout->limits.max_validation_scratch_bytes
        || layout->usage.validation_work > layout->limits.max_validation_work)
        return invalid(diagnostics, "layout header or descriptor is malformed");
    if (!canonical(layout->type_count, layout->type_capacity, layout->types)
        || !canonical(layout->field_count, layout->field_capacity, layout->fields)
        || !canonical(layout->variant_count, layout->variant_capacity, layout->variants)
        || !canonical(layout->projection_count, layout->projection_capacity,
            layout->projections))
        return invalid(diagnostics, "layout arenas are noncanonical");
    size_t persistent;
    if (!persistent_bytes(layout, &persistent)
        || layout->usage.type_layouts != layout->type_count
        || layout->usage.field_layouts != layout->field_count
        || layout->usage.variant_layouts != layout->variant_count
        || layout->usage.projection_maps != layout->projection_count
        || layout->usage.owned_bytes != persistent
        || layout->usage.build_scratch_bytes != layout->type_count)
        return invalid(diagnostics, "layout resource accounting is malformed");
    LayoutRange owned[4]; size_t owned_count = 0;
#define OWN(member, capacity) do { \
    if (!add_owned(owned, &owned_count, layout->member, layout->capacity, \
            sizeof(*layout->member))) goto malformed; \
} while (0)
    OWN(types, type_capacity); OWN(fields, field_capacity);
    OWN(variants, variant_capacity); OWN(projections, projection_capacity);
#undef OWN
    if (!preflight_owner_chain(layout, owned, owned_count)) goto malformed;
    if (!sol_mir_representation_validate(layout->representation, diagnostics))
        return false;
    if (!reject_borrowed_overlap(layout, owned, owned_count)) goto malformed;
    size_t build_work, validation_work, validation_scratch;
    if (!derive_build_work(layout, &build_work)
        || !sol_mir_layout_internal_requirements(layout, &validation_work,
            &validation_scratch)
        || layout->usage.build_work != build_work
        || layout->usage.validation_work != validation_work
        || layout->usage.validation_scratch_bytes != validation_scratch)
        goto malformed;
    const SolMirRepresentation *r = layout->representation;
    const SolMirMaterialization *m = r->materialization;
    if (layout->type_count != r->recipe_count
        || layout->field_count != r->field_count
        || layout->variant_count != r->variant_count
        || layout->projection_count != m->projection_count
        || !validate_records(layout) || !validate_projections(layout))
        goto malformed;
    return true;
malformed:
    return invalid(diagnostics,
        "layout graph, access maps, or owned arenas are malformed");
}
