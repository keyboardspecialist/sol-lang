#include "sol/mir_representation.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uintptr_t start;
    uintptr_t end;
} ValidationRange;

static bool validation_error(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) {
        sol_diagnostics_add(diagnostics, "SOL-MIR-REPRESENTATION-001",
            SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    }
    return false;
}

static bool checked_add(size_t *value, size_t amount) {
    if (amount > SIZE_MAX - *value) return false;
    *value += amount;
    return true;
}

static bool checked_mul(size_t left, size_t right, size_t *result) {
    if (left != 0 && right > SIZE_MAX / left) return false;
    *result = left * right;
    return true;
}

static bool complete_limits(SolMirRepresentationLimits limits) {
    return limits.max_recipes != 0 && limits.max_fields != 0
        && limits.max_variants != 0 && limits.max_recipe_ids != 0
        && limits.max_callable_producers != 0
        && limits.max_receiver_roots != 0 && limits.max_owned_bytes != 0
        && limits.max_build_scratch_bytes != 0 && limits.max_build_work != 0
        && limits.max_validation_work != 0
        && limits.max_validation_scratch_bytes != 0;
}

static bool canonical_arena(size_t count, size_t capacity,
    const void *pointer) {
    return count == capacity && ((count == 0) == (pointer == NULL));
}

static bool add_owned_range(ValidationRange ranges[7], size_t *range_count,
    const void *pointer, size_t count, size_t item_size) {
    if (count == 0) return pointer == NULL;
    size_t bytes;
    if (pointer == NULL || !checked_mul(count, item_size, &bytes)) return false;
    uintptr_t start = (uintptr_t)pointer;
    if (bytes > UINTPTR_MAX - start) return false;
    ValidationRange next = {start, start + bytes};
    for (size_t i = 0; i < *range_count; ++i) {
        if (next.start < ranges[i].end && ranges[i].start < next.end)
            return false;
    }
    ranges[(*range_count)++] = next;
    return true;
}

static bool borrowed_overlaps(const ValidationRange ranges[7],
    size_t range_count, const void *pointer, size_t count, size_t item_size) {
    if (count == 0) return pointer != NULL;
    size_t bytes;
    if (pointer == NULL || !checked_mul(count, item_size, &bytes)) return true;
    uintptr_t start = (uintptr_t)pointer;
    if (bytes > UINTPTR_MAX - start) return true;
    uintptr_t end = start + bytes;
    for (size_t i = 0; i < range_count; ++i) {
        if (start < ranges[i].end && ranges[i].start < end) return true;
    }
    return false;
}

static bool persistent_bytes(const SolMirRepresentation *representation,
    size_t *result) {
    size_t total = 0;
    size_t bytes;
#define ADD_BYTES(capacity, field) do { \
    if (!checked_mul((capacity), sizeof(*representation->field), &bytes) \
        || !checked_add(&total, bytes)) return false; \
} while (0)
    ADD_BYTES(representation->recipe_capacity, recipes);
    ADD_BYTES(representation->field_capacity, fields);
    ADD_BYTES(representation->variant_capacity, variants);
    ADD_BYTES(representation->recipe_id_capacity, recipe_ids);
    ADD_BYTES(representation->access_capacity, accesses);
    ADD_BYTES(representation->receiver_root_capacity, receiver_roots);
    ADD_BYTES(representation->callable_producer_capacity, callable_producers);
#undef ADD_BYTES
    *result = total;
    return true;
}

bool sol_mir_representation_validation_requirements(
    const SolMirRepresentation *representation, size_t *work, size_t *scratch) {
    if (representation == NULL || representation->materialization == NULL
        || work == NULL || scratch == NULL) return false;
    const SolMirMaterialization *materialization = representation->materialization;
    if (materialization->plan == NULL || materialization->plan->program == NULL
        || materialization->plan->program->ir == NULL) return false;
    const SolMirPlan *plan = materialization->plan;
    const SolMirProgram *program = plan->program;
    const SolIr *ir = program->ir;

    size_t total = materialization->usage.validation_work;
    size_t records = representation->recipe_count;
    if (!checked_add(&records, representation->field_count)
        || !checked_add(&records, representation->variant_count)
        || !checked_add(&records, representation->recipe_id_count)
        || !checked_add(&records, representation->access_count)
        || !checked_add(&records, representation->callable_producer_count)
        || !checked_add(&records, representation->receiver_root_count)
        || !checked_add(&total, records)) return false;

    size_t fixed_scan = materialization->type_count;
    if (!checked_add(&fixed_scan, materialization->shape_field_count)
        || !checked_add(&fixed_scan, materialization->shape_variant_count))
        return false;
    size_t passes = materialization->type_count;
    if (!checked_add(&passes, 1)) return false;
    size_t fixed_work;
    if (!checked_mul(fixed_scan, passes, &fixed_work)
        || !checked_mul(fixed_work, 3, &fixed_work)
        || !checked_add(&total, fixed_work)) return false;
    size_t wrappers;
    if (!checked_mul(materialization->type_count, passes, &wrappers)
        || !checked_add(&total, wrappers)
        || !checked_add(&total, materialization->semantic_site_count)
        || !checked_add(&total, materialization->receiver_root_count))
        return false;

    size_t borrowed = materialization->usage.concrete_records;
    if (!checked_add(&borrowed, plan->type_component_count)
        || !checked_add(&borrowed, plan->typed_use_count)
        || !checked_add(&borrowed, plan->demand_count)
        || !checked_add(&borrowed, program->reference_count)
        || !checked_add(&borrowed, program->template_count)
        || !checked_add(&borrowed, materialization->image_count)
        || !checked_add(&borrowed, ir->type_count)
        || !checked_add(&borrowed, ir->definition_count)
        || !checked_add(&borrowed, ir->callable_count)
        || !checked_add(&borrowed, ir->expression_count)
        || !checked_add(&borrowed, ir->statement_count)
        || !checked_add(&borrowed, ir->source_length)
        || !checked_mul(borrowed, 8, &borrowed)
        || !checked_add(&total, borrowed)) return false;

    size_t materialization_scratch;
    if (!checked_mul(materialization->usage.validation_work, sizeof(size_t),
            &materialization_scratch)
        || !checked_add(&materialization_scratch,
            materialization->usage.owned_bytes)) return false;
    size_t local_scratch;
    if (!checked_mul(materialization->type_count, 3, &local_scratch))
        return false;
    *work = total;
    *scratch = materialization_scratch > local_scratch
        ? materialization_scratch : local_scratch;
    return true;
}

static bool reject_materialization_overlap(const SolMirRepresentation *r,
    const ValidationRange owned[7], size_t owned_count) {
    const SolMirMaterialization *m = r->materialization;
#define M_RANGE(field, capacity) do { \
    if (borrowed_overlaps(owned, owned_count, m->field, m->capacity, \
            sizeof(*m->field))) return false; \
} while (0)
    M_RANGE(images, image_capacity); M_RANGE(types, type_capacity);
    M_RANGE(shape_fields, shape_field_capacity);
    M_RANGE(shape_variants, shape_variant_capacity);
    M_RANGE(type_ids, type_id_capacity); M_RANGE(accesses, access_capacity);
    M_RANGE(overlays, overlay_capacity); M_RANGE(contexts, context_capacity);
    M_RANGE(locals, local_capacity); M_RANGE(places, place_capacity);
    M_RANGE(projections, projection_capacity); M_RANGE(values, value_capacity);
    M_RANGE(instructions, instruction_capacity);
    M_RANGE(temporaries, temporary_capacity);
    M_RANGE(construct_operands, construct_operand_capacity);
    M_RANGE(call_arguments, call_argument_capacity); M_RANGE(blocks, block_capacity);
    M_RANGE(edges, edge_capacity); M_RANGE(edge_values, edge_value_capacity);
    M_RANGE(parameter_values, parameter_value_capacity); M_RANGE(loops, loop_capacity);
    M_RANGE(bindings, binding_capacity); M_RANGE(semantic_sites, semantic_site_capacity);
    M_RANGE(receiver_roots, receiver_root_capacity); M_RANGE(imports, import_capacity);
    M_RANGE(handlers, handler_capacity); M_RANGE(writebacks, writeback_capacity);
    M_RANGE(effect_rows, effect_row_capacity); M_RANGE(effect_atoms, effect_atom_capacity);
    M_RANGE(effect_row_atoms, effect_row_atom_capacity);
    M_RANGE(effect_names, effect_name_capacity); M_RANGE(literal_bytes, literal_byte_capacity);
#undef M_RANGE
    const SolMirPlan *p = m->plan;
#define P_RANGE(field, capacity) do { \
    if (borrowed_overlaps(owned, owned_count, p->field, p->capacity, \
            sizeof(*p->field))) return false; \
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
#define PROGRAM_RANGE(field, count) do { \
    if (borrowed_overlaps(owned, owned_count, program->field, program->count, \
            sizeof(*program->field))) return false; \
} while (0)
    PROGRAM_RANGE(roots, root_count); PROGRAM_RANGE(approved_imports, approved_import_count);
    PROGRAM_RANGE(templates, template_count); PROGRAM_RANGE(imports, import_count);
    PROGRAM_RANGE(specializations, specialization_count); PROGRAM_RANGE(references, reference_count);
#undef PROGRAM_RANGE
#define MIR_RANGE(mir, field, capacity) do { \
    if (borrowed_overlaps(owned, owned_count, (mir)->field, (mir)->capacity, \
            sizeof(*(mir)->field))) return false; \
} while (0)
    for (size_t i = 0; i < program->template_count; ++i) {
        const SolMir *mir = &program->templates[i].mir;
        MIR_RANGE(mir, blocks, block_capacity); MIR_RANGE(mir, instructions, instruction_capacity);
        MIR_RANGE(mir, values, value_capacity); MIR_RANGE(mir, parameter_values, parameter_value_capacity);
        MIR_RANGE(mir, edge_values, edge_value_capacity); MIR_RANGE(mir, call_arguments, call_argument_capacity);
        MIR_RANGE(mir, loops, loop_capacity); MIR_RANGE(mir, construct_operands, construct_operand_capacity);
        MIR_RANGE(mir, temporaries, temporary_capacity);
    }
    for (size_t i = 0; i < m->image_count; ++i) {
        const SolMir *mir = &m->images[i].topology;
        MIR_RANGE(mir, blocks, block_capacity); MIR_RANGE(mir, instructions, instruction_capacity);
        MIR_RANGE(mir, values, value_capacity); MIR_RANGE(mir, parameter_values, parameter_value_capacity);
        MIR_RANGE(mir, edge_values, edge_value_capacity); MIR_RANGE(mir, call_arguments, call_argument_capacity);
        MIR_RANGE(mir, loops, loop_capacity); MIR_RANGE(mir, construct_operands, construct_operand_capacity);
        MIR_RANGE(mir, temporaries, temporary_capacity);
    }
#undef MIR_RANGE
    const SolIr *ir = program->ir;
#define IR_RANGE(field, count) do { \
    if (borrowed_overlaps(owned, owned_count, ir->field, ir->count, \
            sizeof(*ir->field))) return false; \
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

static bool same_slice(SolMirPlanSlice left, SolMirPlanSlice right) {
    return left.offset == right.offset && left.count == right.count;
}

static bool same_recipe_shape(const SolMirRecipe *actual,
    const SolMirRecipe *expected) {
    return actual->kind == expected->kind
        && actual->concrete_definition == expected->concrete_definition
        && same_slice(actual->fields, expected->fields)
        && same_slice(actual->variants, expected->variants)
        && same_slice(actual->parameters, expected->parameters)
        && same_slice(actual->parameter_accesses,
            expected->parameter_accesses)
        && actual->result == expected->result
        && actual->effects == expected->effects
        && actual->backing == expected->backing
        && actual->capability_source == expected->capability_source;
}

static bool expected_kind(const SolMirMaterializedType *type,
    SolMirRecipeKind *kind) {
    switch (type->kind) {
        case SOL_IR_TYPE_INT64: *kind = SOL_MIR_RECIPE_INT64; return true;
        case SOL_IR_TYPE_BOOL: *kind = SOL_MIR_RECIPE_BOOL; return true;
        case SOL_IR_TYPE_TEXT: *kind = SOL_MIR_RECIPE_TEXT; return true;
        case SOL_IR_TYPE_UNIT: *kind = SOL_MIR_RECIPE_UNIT; return true;
        case SOL_IR_TYPE_NEVER: *kind = SOL_MIR_RECIPE_NEVER; return true;
        case SOL_IR_TYPE_TUPLE: *kind = SOL_MIR_RECIPE_TUPLE; return true;
        case SOL_IR_TYPE_OPTION: *kind = SOL_MIR_RECIPE_OPTION; return true;
        case SOL_IR_TYPE_RESULT: *kind = SOL_MIR_RECIPE_RESULT; return true;
        case SOL_IR_TYPE_FUNCTION: *kind = SOL_MIR_RECIPE_FUNCTION; return true;
        case SOL_IR_TYPE_NOMINAL: break;
        case SOL_IR_TYPE_PARAMETER:
        case SOL_IR_TYPE_SELF:
            return false;
    }
    switch (type->nominal_category) {
        case SOL_MIR_MATERIALIZED_NOMINAL_RECORD:
            *kind = SOL_MIR_RECIPE_RECORD; return true;
        case SOL_MIR_MATERIALIZED_NOMINAL_ENUM:
            *kind = SOL_MIR_RECIPE_ENUM; return !type->nominal_open;
        case SOL_MIR_MATERIALIZED_NOMINAL_DISTINCT:
            *kind = SOL_MIR_RECIPE_DISTINCT; return true;
        case SOL_MIR_MATERIALIZED_NOMINAL_REFINED:
            *kind = SOL_MIR_RECIPE_REFINED; return true;
        case SOL_MIR_MATERIALIZED_NOMINAL_CAPABILITY:
            *kind = SOL_MIR_RECIPE_CAPABILITY; return true;
        case SOL_MIR_MATERIALIZED_NOMINAL_NONE:
            return false;
    }
    return false;
}

static bool same_field(const SolMirRecipeField *actual, SolIrFieldId source,
    size_t ordinal, SolMirRecipeId type) {
    return actual->source_field == source && actual->ordinal == ordinal
        && actual->type == type;
}

static bool same_variant(const SolMirRecipeVariant *actual,
    SolIrVariantId source, size_t ordinal, size_t tag, size_t field_offset,
    size_t field_count) {
    return actual->source_variant == source && actual->ordinal == ordinal
        && actual->semantic_tag == tag
        && actual->fields.offset == field_offset
        && actual->fields.count == field_count;
}

static bool validate_recipes(const SolMirRepresentation *r) {
    const SolMirMaterialization *m = r->materialization;
    size_t field_at = 0, variant_at = 0, id_at = 0, access_at = 0;
    for (size_t i = 0; i < m->type_count; ++i) {
        const SolMirMaterializedType *type = &m->types[i];
        SolMirRecipe expected = {0};
        expected.concrete_definition = SOL_IR_NONE;
        expected.result = SOL_MIR_RECIPE_NONE;
        expected.effects = SOL_MIR_MATERIALIZED_NONE;
        expected.backing = SOL_MIR_RECIPE_NONE;
        expected.capability_source = SOL_MIR_RECIPE_NONE;
        if (!expected_kind(type, &expected.kind)) return false;
        if (type->kind == SOL_IR_TYPE_NOMINAL)
            expected.concrete_definition = type->definition;
        switch (expected.kind) {
            case SOL_MIR_RECIPE_INT64: case SOL_MIR_RECIPE_BOOL:
            case SOL_MIR_RECIPE_TEXT: case SOL_MIR_RECIPE_UNIT:
            case SOL_MIR_RECIPE_NEVER:
                break;
            case SOL_MIR_RECIPE_TUPLE:
                expected.fields = (SolMirPlanSlice){field_at,
                    type->arguments.count};
                for (size_t f = 0; f < type->arguments.count; ++f) {
                    if (field_at >= r->field_count
                        || !same_field(&r->fields[field_at], SOL_IR_NONE, f,
                            m->type_ids[type->arguments.offset + f])) return false;
                    ++field_at;
                }
                break;
            case SOL_MIR_RECIPE_RECORD:
                expected.fields = (SolMirPlanSlice){field_at,
                    type->fields.count};
                for (size_t f = 0; f < type->fields.count; ++f) {
                    const SolMirMaterializedShapeField *source
                        = &m->shape_fields[type->fields.offset + f];
                    if (field_at >= r->field_count
                        || !same_field(&r->fields[field_at],
                            source->source_field, source->ordinal,
                            source->type)) return false;
                    ++field_at;
                }
                break;
            case SOL_MIR_RECIPE_ENUM:
                expected.variants = (SolMirPlanSlice){variant_at,
                    type->variants.count};
                for (size_t v = 0; v < type->variants.count; ++v) {
                    const SolMirMaterializedShapeVariant *source
                        = &m->shape_variants[type->variants.offset + v];
                    size_t start = field_at;
                    for (size_t f = 0; f < source->fields.count; ++f) {
                        const SolMirMaterializedShapeField *item
                            = &m->shape_fields[source->fields.offset + f];
                        if (field_at >= r->field_count
                            || !same_field(&r->fields[field_at],
                                item->source_field, item->ordinal,
                                item->type)) return false;
                        ++field_at;
                    }
                    if (variant_at >= r->variant_count
                        || !same_variant(&r->variants[variant_at],
                            source->source_variant, source->ordinal,
                            source->ordinal, start, source->fields.count))
                        return false;
                    ++variant_at;
                }
                break;
            case SOL_MIR_RECIPE_OPTION: {
                expected.variants = (SolMirPlanSlice){variant_at, 2};
                if (variant_at + 2 > r->variant_count
                    || !same_variant(&r->variants[variant_at], SOL_IR_NONE,
                        0, 0, field_at, 0)) return false;
                ++variant_at;
                size_t start = field_at;
                if (field_at >= r->field_count
                    || !same_field(&r->fields[field_at], SOL_IR_NONE, 0,
                        m->type_ids[type->arguments.offset])) return false;
                ++field_at;
                if (!same_variant(&r->variants[variant_at], SOL_IR_NONE,
                        1, 1, start, 1)) return false;
                ++variant_at;
                break;
            }
            case SOL_MIR_RECIPE_RESULT:
                expected.variants = (SolMirPlanSlice){variant_at, 2};
                for (size_t v = 0; v < 2; ++v) {
                    size_t start = field_at;
                    if (field_at >= r->field_count
                        || !same_field(&r->fields[field_at], SOL_IR_NONE, 0,
                            m->type_ids[type->arguments.offset + v]))
                        return false;
                    ++field_at;
                    if (variant_at >= r->variant_count
                        || !same_variant(&r->variants[variant_at], SOL_IR_NONE,
                            v, v, start, 1)) return false;
                    ++variant_at;
                }
                break;
            case SOL_MIR_RECIPE_DISTINCT: case SOL_MIR_RECIPE_REFINED:
                expected.backing = type->backing;
                break;
            case SOL_MIR_RECIPE_FUNCTION:
                expected.parameters = (SolMirPlanSlice){id_at,
                    type->parameters.count};
                expected.parameter_accesses = (SolMirPlanSlice){access_at,
                    type->parameter_accesses.count};
                if (type->parameters.count != type->parameter_accesses.count)
                    return false;
                for (size_t p = 0; p < type->parameters.count; ++p) {
                    if (id_at >= r->recipe_id_count
                        || access_at >= r->access_count
                        || r->recipe_ids[id_at]
                            != m->type_ids[type->parameters.offset + p]
                        || r->accesses[access_at]
                            != m->accesses[type->parameter_accesses.offset + p])
                        return false;
                    ++id_at; ++access_at;
                }
                expected.result = type->result;
                expected.effects = type->effects;
                break;
            case SOL_MIR_RECIPE_CAPABILITY:
                expected.capability_source = type->capability_source;
                break;
        }
        if (!same_recipe_shape(&r->recipes[i], &expected)) return false;
    }
    return field_at == r->field_count && variant_at == r->variant_count
        && id_at == r->recipe_id_count && access_at == r->access_count;
}

static bool add_static_build_work(const SolMirMaterialization *m,
    size_t *work) {
    *work = 0;
    for (size_t i = 0; i < m->type_count; ++i) {
        const SolMirMaterializedType *type = &m->types[i];
        SolMirRecipeKind kind;
        if (!expected_kind(type, &kind) || !checked_add(work, 2)) return false;
        switch (kind) {
            case SOL_MIR_RECIPE_TUPLE:
                if (!checked_add(work, type->arguments.count)) return false;
                break;
            case SOL_MIR_RECIPE_RECORD:
                if (!checked_add(work, type->fields.count)) return false;
                break;
            case SOL_MIR_RECIPE_ENUM:
                for (size_t v = 0; v < type->variants.count; ++v) {
                    const SolMirMaterializedShapeVariant *variant
                        = &m->shape_variants[type->variants.offset + v];
                    /* Counting and population each visit the variant once. */
                    if (!checked_add(work, 2)
                        || !checked_add(work, variant->fields.count))
                        return false;
                }
                break;
            case SOL_MIR_RECIPE_OPTION:
                if (!checked_add(work, 3)) return false;
                break;
            case SOL_MIR_RECIPE_RESULT:
                if (!checked_add(work, 4)) return false;
                break;
            case SOL_MIR_RECIPE_FUNCTION:
                if (!checked_add(work, type->parameters.count)) return false;
                break;
            case SOL_MIR_RECIPE_INT64: case SOL_MIR_RECIPE_BOOL:
            case SOL_MIR_RECIPE_TEXT: case SOL_MIR_RECIPE_UNIT:
            case SOL_MIR_RECIPE_NEVER: case SOL_MIR_RECIPE_DISTINCT:
            case SOL_MIR_RECIPE_REFINED: case SOL_MIR_RECIPE_CAPABILITY:
                break;
        }
    }
    /* count_graph and populate_producers each scan every semantic site. */
    if (!checked_add(work, m->semantic_site_count)
        || !checked_add(work, m->semantic_site_count)) return false;
    for (size_t i = 0; i < m->semantic_site_count; ++i) {
        const SolMirMaterializedSemanticSite *site = &m->semantic_sites[i];
        switch (site->kind) {
            case SOL_MIR_PLAN_DEMAND_FUNCTION_VALUE:
            case SOL_MIR_PLAN_DEMAND_BOUND_OPERATION:
            case SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE:
                if (!checked_add(work,
                        site->captured_receiver_roots.count)) return false;
                break;
            case SOL_MIR_PLAN_DEMAND_ROOT: case SOL_MIR_PLAN_DEMAND_INVOKE:
            case SOL_MIR_PLAN_DEMAND_CALLBACK: case SOL_MIR_PLAN_DEMAND_PREDICATE:
            case SOL_MIR_PLAN_DEMAND_HANDLER_SOURCE:
            case SOL_MIR_PLAN_DEMAND_HANDLER_PROVIDER:
                break;
            default: return false;
        }
    }
    return true;
}

static bool scan_product_facts(const SolMirMaterialization *m,
    const SolMirMaterializedType *type, const unsigned char *facts,
    size_t *work, bool *value) {
    *value = true;
    if (type->kind == SOL_IR_TYPE_TUPLE) {
        for (size_t i = 0; i < type->arguments.count; ++i) {
            if (!checked_add(work, 1)) return false;
            if (!facts[m->type_ids[type->arguments.offset + i]]) {
                *value = false;
                return true;
            }
        }
    } else {
        for (size_t i = 0; i < type->fields.count; ++i) {
            if (!checked_add(work, 1)) return false;
            if (!facts[m->shape_fields[type->fields.offset + i].type]) {
                *value = false;
                return true;
            }
        }
    }
    return true;
}

static bool scan_inhabited_cases(const SolMirMaterialization *m,
    const SolMirMaterializedType *type, const unsigned char *facts,
    size_t *work, bool *value) {
    if (type->kind == SOL_IR_TYPE_RESULT) {
        for (size_t v = 0; v < 2; ++v) {
            if (!checked_add(work, 2)) return false;
            if (facts[m->type_ids[type->arguments.offset + v]]) {
                *value = true;
                return true;
            }
        }
        *value = false;
        return true;
    }
    for (size_t v = 0; v < type->variants.count; ++v) {
        const SolMirMaterializedShapeVariant *variant
            = &m->shape_variants[type->variants.offset + v];
        if (!checked_add(work, 1)) return false;
        bool inhabited = true;
        for (size_t f = 0; f < variant->fields.count; ++f) {
            if (!checked_add(work, 1)) return false;
            if (!facts[m->shape_fields[variant->fields.offset + f].type]) {
                inhabited = false;
                break;
            }
        }
        if (inhabited) { *value = true; return true; }
    }
    *value = false;
    return true;
}

static bool scan_copy_cases(const SolMirMaterialization *m,
    const SolMirMaterializedType *type, const unsigned char *facts,
    size_t *work, bool *value) {
    size_t variant_count = type->kind == SOL_IR_TYPE_OPTION
            || type->kind == SOL_IR_TYPE_RESULT
        ? 2 : type->variants.count;
    for (size_t v = 0; v < variant_count; ++v) {
        if (!checked_add(work, 1)) return false;
        if (type->kind == SOL_IR_TYPE_OPTION) {
            if (v == 1) {
                if (!checked_add(work, 1)) return false;
                *value = facts[m->type_ids[type->arguments.offset]] != 0;
            }
        } else if (type->kind == SOL_IR_TYPE_RESULT) {
            if (!checked_add(work, 1)) return false;
            *value = facts[m->type_ids[type->arguments.offset + v]] != 0;
        } else {
            const SolMirMaterializedShapeVariant *variant
                = &m->shape_variants[type->variants.offset + v];
            for (size_t f = 0; f < variant->fields.count; ++f) {
                if (!checked_add(work, 1)) return false;
                if (!facts[m->shape_fields[variant->fields.offset + f].type]) {
                    *value = false;
                    break;
                }
            }
        }
        if (!*value) return true;
    }
    return true;
}

static bool derive_fixed_points(const SolMirRepresentation *r,
    unsigned char *inhabited, unsigned char *zero, unsigned char *copy,
    size_t *build_work) {
    const SolMirMaterialization *m = r->materialization;
    if (!add_static_build_work(m, build_work)) return false;
    for (size_t i = 0; i < m->type_count; ++i) {
        SolMirRecipeKind kind;
        if (!expected_kind(&m->types[i], &kind)
            || !checked_add(build_work, 1)) return false;
        copy[i] = kind != SOL_MIR_RECIPE_FUNCTION
            && kind != SOL_MIR_RECIPE_CAPABILITY;
    }
    bool changed = true;
    size_t pass = 0;
    while (changed && pass <= m->type_count) {
        changed = false; ++pass;
        for (size_t i = 0; i < m->type_count; ++i) {
            if (!checked_add(build_work, 1)) return false;
            const SolMirMaterializedType *type = &m->types[i];
            bool value = false;
            switch (type->kind) {
                case SOL_IR_TYPE_INT64: case SOL_IR_TYPE_BOOL:
                case SOL_IR_TYPE_TEXT: case SOL_IR_TYPE_UNIT:
                case SOL_IR_TYPE_FUNCTION: case SOL_IR_TYPE_OPTION:
                    value = true; break;
                case SOL_IR_TYPE_NEVER: value = false; break;
                case SOL_IR_TYPE_TUPLE:
                    if (!scan_product_facts(m, type, inhabited, build_work,
                            &value)) return false;
                    break;
                case SOL_IR_TYPE_RESULT:
                    if (!scan_inhabited_cases(m, type, inhabited, build_work,
                            &value)) return false;
                    break;
                case SOL_IR_TYPE_NOMINAL:
                    switch (type->nominal_category) {
                        case SOL_MIR_MATERIALIZED_NOMINAL_RECORD:
                            if (!scan_product_facts(m, type, inhabited,
                                    build_work, &value)) return false;
                            break;
                        case SOL_MIR_MATERIALIZED_NOMINAL_ENUM:
                            if (!scan_inhabited_cases(m, type, inhabited,
                                    build_work, &value)) return false;
                            break;
                        case SOL_MIR_MATERIALIZED_NOMINAL_DISTINCT:
                        case SOL_MIR_MATERIALIZED_NOMINAL_REFINED:
                            value = inhabited[type->backing] != 0; break;
                        case SOL_MIR_MATERIALIZED_NOMINAL_CAPABILITY:
                            value = true; break;
                        case SOL_MIR_MATERIALIZED_NOMINAL_NONE: return false;
                    }
                    break;
                case SOL_IR_TYPE_PARAMETER: case SOL_IR_TYPE_SELF:
                    return false;
            }
            if (value && !inhabited[i]) { inhabited[i] = 1; changed = true; }
        }
    }
    if (changed || pass > m->type_count + 1) return false;
    changed = true; pass = 0;
    while (changed && pass <= m->type_count) {
        changed = false; ++pass;
        for (size_t i = 0; i < m->type_count; ++i) {
            if (!checked_add(build_work, 1)) return false;
            const SolMirMaterializedType *type = &m->types[i];
            bool value = false;
            if (inhabited[i]) {
                if (type->kind == SOL_IR_TYPE_UNIT) value = true;
                else if (type->kind == SOL_IR_TYPE_TUPLE
                    || (type->kind == SOL_IR_TYPE_NOMINAL
                        && type->nominal_category
                            == SOL_MIR_MATERIALIZED_NOMINAL_RECORD)) {
                    if (!scan_product_facts(m, type, zero, build_work,
                            &value)) return false;
                } else if (type->kind == SOL_IR_TYPE_NOMINAL
                    && (type->nominal_category
                            == SOL_MIR_MATERIALIZED_NOMINAL_DISTINCT
                        || type->nominal_category
                            == SOL_MIR_MATERIALIZED_NOMINAL_REFINED))
                    value = zero[type->backing] != 0;
            }
            if (value && !zero[i]) { zero[i] = 1; changed = true; }
        }
    }
    if (changed || pass > m->type_count + 1) return false;
    changed = true; pass = 0;
    while (changed && pass <= m->type_count) {
        changed = false; ++pass;
        for (size_t i = 0; i < m->type_count; ++i) {
            if (!checked_add(build_work, 1)) return false;
            if (!copy[i]) continue;
            const SolMirMaterializedType *type = &m->types[i];
            bool value = true;
            if (type->kind == SOL_IR_TYPE_FUNCTION
                || (type->kind == SOL_IR_TYPE_NOMINAL
                    && type->nominal_category
                        == SOL_MIR_MATERIALIZED_NOMINAL_CAPABILITY)) value = false;
            else if (type->kind == SOL_IR_TYPE_TUPLE
                || (type->kind == SOL_IR_TYPE_NOMINAL
                    && type->nominal_category
                        == SOL_MIR_MATERIALIZED_NOMINAL_RECORD)) {
                if (!scan_product_facts(m, type, copy, build_work, &value))
                    return false;
            } else if (type->kind == SOL_IR_TYPE_OPTION
                || type->kind == SOL_IR_TYPE_RESULT
                || (type->kind == SOL_IR_TYPE_NOMINAL
                    && type->nominal_category
                        == SOL_MIR_MATERIALIZED_NOMINAL_ENUM)) {
                if (!scan_copy_cases(m, type, copy, build_work, &value))
                    return false;
            } else if (type->kind == SOL_IR_TYPE_NOMINAL
                && (type->nominal_category
                        == SOL_MIR_MATERIALIZED_NOMINAL_DISTINCT
                    || type->nominal_category
                        == SOL_MIR_MATERIALIZED_NOMINAL_REFINED))
                value = copy[type->backing] != 0;
            if (!value) { copy[i] = 0; changed = true; }
        }
    }
    if (changed || pass > m->type_count + 1
        || !checked_add(build_work, m->type_count)) return false;
    for (size_t i = 0; i < m->type_count; ++i) {
        if (!checked_add(build_work, 2)) return false;
        if (!inhabited[i] || zero[i]) continue;
        size_t id = i;
        for (size_t depth = 0; depth <= m->type_count; ++depth) {
            const SolMirMaterializedType *type = &m->types[id];
            if (type->kind != SOL_IR_TYPE_NOMINAL
                || (type->nominal_category
                        != SOL_MIR_MATERIALIZED_NOMINAL_DISTINCT
                    && type->nominal_category
                        != SOL_MIR_MATERIALIZED_NOMINAL_REFINED)) break;
            if (!checked_add(build_work, 1)) return false;
            id = type->backing;
        }
    }
    return true;
}

static SolMirStorageKind derived_storage(const SolMirMaterialization *m,
    size_t id, const unsigned char *inhabited, const unsigned char *zero) {
    if (!inhabited[id] || zero[id]) return SOL_MIR_STORAGE_NONE;
    for (size_t depth = 0; depth <= m->type_count; ++depth) {
        const SolMirMaterializedType *type = &m->types[id];
        if (type->kind == SOL_IR_TYPE_INT64 || type->kind == SOL_IR_TYPE_BOOL)
            return SOL_MIR_STORAGE_SCALAR;
        if (type->kind == SOL_IR_TYPE_TEXT) return SOL_MIR_STORAGE_TEXT_HANDLE;
        if (type->kind == SOL_IR_TYPE_FUNCTION)
            return SOL_MIR_STORAGE_CALLABLE_HANDLE;
        if (type->kind == SOL_IR_TYPE_NOMINAL
            && type->nominal_category
                == SOL_MIR_MATERIALIZED_NOMINAL_CAPABILITY)
            return SOL_MIR_STORAGE_CAPABILITY_HANDLE;
        if (type->kind == SOL_IR_TYPE_NOMINAL
            && (type->nominal_category
                    == SOL_MIR_MATERIALIZED_NOMINAL_DISTINCT
                || type->nominal_category
                    == SOL_MIR_MATERIALIZED_NOMINAL_REFINED)) {
            id = type->backing;
            continue;
        }
        return SOL_MIR_STORAGE_AGGREGATE_VALUE;
    }
    return (SolMirStorageKind)-1;
}

static bool validate_classification(const SolMirRepresentation *r,
    const unsigned char *inhabited, const unsigned char *zero,
    const unsigned char *copy) {
    const SolMirMaterialization *m = r->materialization;
    for (size_t i = 0; i < m->type_count; ++i) {
        const SolMirMaterializedType *type = &m->types[i];
        const SolMirRecipe *recipe = &r->recipes[i];
        SolMirStorageKind storage = derived_storage(m, i, inhabited, zero);
        SolMirCopyKind copy_kind;
        if (!inhabited[i]) copy_kind = SOL_MIR_COPY_UNREACHABLE;
        else if (!copy[i]) copy_kind = SOL_MIR_COPY_FORBIDDEN;
        else if (type->kind == SOL_IR_TYPE_TEXT) copy_kind = SOL_MIR_COPY_TEXT;
        else if (type->kind == SOL_IR_TYPE_NOMINAL
            && (type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_DISTINCT
                || type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_REFINED))
            copy_kind = SOL_MIR_COPY_WRAPPER;
        else if (type->kind == SOL_IR_TYPE_TUPLE
            || type->kind == SOL_IR_TYPE_OPTION || type->kind == SOL_IR_TYPE_RESULT
            || (type->kind == SOL_IR_TYPE_NOMINAL
                && (type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_RECORD
                    || type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_ENUM)))
            copy_kind = SOL_MIR_COPY_AGGREGATE;
        else copy_kind = SOL_MIR_COPY_TRIVIAL;
        SolMirDropKind drop;
        if (!inhabited[i] || type->kind == SOL_IR_TYPE_INT64
            || type->kind == SOL_IR_TYPE_BOOL || type->kind == SOL_IR_TYPE_UNIT
            || type->kind == SOL_IR_TYPE_NEVER) drop = SOL_MIR_DROP_NONE;
        else if (type->kind == SOL_IR_TYPE_TEXT) drop = SOL_MIR_DROP_TEXT;
        else if (type->kind == SOL_IR_TYPE_FUNCTION) drop = SOL_MIR_DROP_CALLABLE;
        else if (type->kind == SOL_IR_TYPE_NOMINAL
            && type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_CAPABILITY)
            drop = SOL_MIR_DROP_CAPABILITY;
        else if (type->kind == SOL_IR_TYPE_NOMINAL
            && (type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_DISTINCT
                || type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_REFINED))
            drop = SOL_MIR_DROP_WRAPPER;
        else drop = SOL_MIR_DROP_AGGREGATE;
        if (recipe->inhabited != (inhabited[i] != 0)
            || recipe->zero_sized != (zero[i] != 0)
            || recipe->is_copy != (copy[i] != 0)
            || type->is_copy != (copy[i] != 0)
            || recipe->storage != storage || recipe->copy_kind != copy_kind
            || recipe->drop_kind != drop) return false;
    }
    return true;
}

static bool same_producer(const SolMirCallableProducer *actual,
    SolMirCallableProducerKind kind, size_t site_id,
    const SolMirMaterializedSemanticSite *site,
    const SolMirMaterializedBinding *binding, SolMirPlanSlice roots,
    SolMirMaterializedEffectRowId effects) {
    if (actual->kind != kind) return false;
    if (actual->function_recipe != site->produced_function_type
        || actual->semantic_site != site_id
        || actual->binding != site->binding) return false;
    if (actual->target_kind != binding->target_kind
        || actual->instance != binding->instance
        || actual->import != binding->import
        || !same_slice(actual->captured_receiver_roots, roots)
        || actual->effects != effects) return false;
    if (kind == SOL_MIR_CALLABLE_PRODUCER_EXACT_FUNCTION) {
        return actual->captured_receiver_type == SOL_MIR_RECIPE_NONE
            && actual->captured_receiver_kind
                == SOL_MIR_MATERIALIZED_RECEIVER_NONE
            && actual->captured_receiver_expression == SOL_IR_NONE
            && actual->captured_receiver_place == SOL_MIR_MATERIALIZED_NONE
            && actual->captured_receiver_temporary == SOL_MIR_MATERIALIZED_NONE
            && actual->captured_receiver_value == SOL_MIR_MATERIALIZED_NONE
            && actual->captured_receiver_instruction
                == SOL_MIR_MATERIALIZED_NONE;
    }
    return actual->captured_receiver_type == site->captured_receiver_type
        && actual->captured_receiver_kind == site->captured_receiver_kind
        && actual->captured_receiver_expression
            == site->captured_receiver_expression
        && actual->captured_receiver_place == site->captured_receiver_place
        && actual->captured_receiver_temporary
            == site->captured_receiver_temporary
        && actual->captured_receiver_value == site->captured_receiver_value
        && actual->captured_receiver_instruction
            == site->captured_receiver_instruction;
}

static bool validate_producers(const SolMirRepresentation *r) {
    const SolMirMaterialization *m = r->materialization;
    size_t producer_at = 0, root_at = 0;
    for (size_t i = 0; i < m->semantic_site_count; ++i) {
        const SolMirMaterializedSemanticSite *site = &m->semantic_sites[i];
        bool expected;
        bool bound = false;
        switch (site->kind) {
            case SOL_MIR_PLAN_DEMAND_FUNCTION_VALUE:
            case SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE:
                expected = true; break;
            case SOL_MIR_PLAN_DEMAND_BOUND_OPERATION:
                expected = true; bound = true; break;
            case SOL_MIR_PLAN_DEMAND_ROOT: case SOL_MIR_PLAN_DEMAND_INVOKE:
            case SOL_MIR_PLAN_DEMAND_CALLBACK: case SOL_MIR_PLAN_DEMAND_PREDICATE:
            case SOL_MIR_PLAN_DEMAND_HANDLER_SOURCE:
            case SOL_MIR_PLAN_DEMAND_HANDLER_PROVIDER:
                expected = false; break;
            default: return false;
        }
        if (!expected) continue;
        if (producer_at >= r->callable_producer_count
            || site->binding >= m->binding_count
            || site->produced_function_type >= m->type_count
            || m->types[site->produced_function_type].kind
                != SOL_IR_TYPE_FUNCTION) return false;
        const SolMirMaterializedBinding *binding = &m->bindings[site->binding];
        SolMirPlanSlice roots = {0};
        SolMirMaterializedEffectRowId effects
            = m->types[site->produced_function_type].effects;
        if (bound) {
            roots = (SolMirPlanSlice){root_at,
                site->captured_receiver_roots.count};
            effects = site->operation.effects;
            for (size_t j = 0; j < roots.count; ++j) {
                if (root_at >= r->receiver_root_count
                    || r->receiver_roots[root_at]
                        != m->receiver_roots[
                            site->captured_receiver_roots.offset + j])
                    return false;
                ++root_at;
            }
        }
        if (!same_producer(&r->callable_producers[producer_at],
                bound ? SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION
                    : SOL_MIR_CALLABLE_PRODUCER_EXACT_FUNCTION,
                i, site, binding, roots, effects)) return false;
        ++producer_at;
    }
    return producer_at == r->callable_producer_count
        && root_at == r->receiver_root_count;
}

bool sol_mir_representation_validate(const SolMirRepresentation *r,
    SolDiagnostics *diagnostics) {
    if (r == NULL || r->materialization == NULL || !complete_limits(r->limits)
        || r->usage.recipes != r->recipe_count
        || r->usage.fields != r->field_count
        || r->usage.variants != r->variant_count
        || r->usage.recipe_ids != r->recipe_id_count
        || r->usage.callable_producers != r->callable_producer_count
        || r->usage.receiver_roots != r->receiver_root_count
        || r->recipe_count > r->limits.max_recipes
        || r->field_count > r->limits.max_fields
        || r->variant_count > r->limits.max_variants
        || r->recipe_id_count > r->limits.max_recipe_ids
        || r->callable_producer_count > r->limits.max_callable_producers
        || r->receiver_root_count > r->limits.max_receiver_roots
        || r->usage.owned_bytes > r->limits.max_owned_bytes
        || r->usage.build_scratch_bytes > r->limits.max_build_scratch_bytes
        || r->usage.build_work > r->limits.max_build_work
        || r->usage.validation_work > r->limits.max_validation_work
        || r->usage.validation_scratch_bytes
            > r->limits.max_validation_scratch_bytes)
        return validation_error(diagnostics,
            "representation header or materialization is invalid");
#define CANON(field, count, capacity) do { \
    if (!canonical_arena(r->count, r->capacity, r->field)) goto malformed; \
} while (0)
    CANON(recipes, recipe_count, recipe_capacity);
    CANON(fields, field_count, field_capacity);
    CANON(variants, variant_count, variant_capacity);
    CANON(recipe_ids, recipe_id_count, recipe_id_capacity);
    CANON(accesses, access_count, access_capacity);
    CANON(receiver_roots, receiver_root_count, receiver_root_capacity);
    CANON(callable_producers, callable_producer_count,
        callable_producer_capacity);
#undef CANON
    size_t bytes;
    if (!persistent_bytes(r, &bytes) || bytes != r->usage.owned_bytes)
        goto malformed;
    size_t expected_build_scratch;
    if (!checked_mul(r->recipe_count, 3, &expected_build_scratch)
        || expected_build_scratch != r->usage.build_scratch_bytes)
        goto malformed;
    ValidationRange owned[7];
    size_t owned_count = 0;
#define OWN(field, capacity) do { \
    if (!add_owned_range(owned, &owned_count, r->field, r->capacity, \
            sizeof(*r->field))) goto malformed; \
} while (0)
    OWN(recipes, recipe_capacity); OWN(fields, field_capacity);
    OWN(variants, variant_capacity); OWN(recipe_ids, recipe_id_capacity);
    OWN(accesses, access_capacity); OWN(receiver_roots, receiver_root_capacity);
    OWN(callable_producers, callable_producer_capacity);
#undef OWN

    if (!sol_mir_materialization_validate(r->materialization, diagnostics))
        return false;
    if (!reject_materialization_overlap(r, owned, owned_count)) goto malformed;
    size_t expected_work, expected_scratch;
    if (!sol_mir_representation_validation_requirements(r, &expected_work,
            &expected_scratch)
        || expected_work != r->usage.validation_work
        || expected_scratch != r->usage.validation_scratch_bytes
        || r->recipe_count != r->materialization->type_count
        || r->access_count != r->recipe_id_count) goto malformed;

    size_t fact_bytes;
    if (!checked_mul(r->recipe_count, 3, &fact_bytes)
        || fact_bytes > r->usage.validation_scratch_bytes) goto malformed;
    unsigned char *facts = fact_bytes == 0 ? NULL : calloc(fact_bytes, 1);
    if (fact_bytes != 0 && facts == NULL)
        return validation_error(diagnostics,
            "representation validation allocation failed");
    unsigned char *inhabited = facts;
    unsigned char *zero = facts == NULL ? NULL : facts + r->recipe_count;
    unsigned char *copy = zero == NULL ? NULL : zero + r->recipe_count;
    size_t expected_build_work = 0;
    bool valid = validate_recipes(r)
        && derive_fixed_points(r, inhabited, zero, copy,
            &expected_build_work)
        && expected_build_work == r->usage.build_work
        && validate_classification(r, inhabited, zero, copy)
        && validate_producers(r);
    free(facts);
    if (valid) return true;
malformed:
    return validation_error(diagnostics,
        "representation graph, classification, or owned arenas are malformed");
}
