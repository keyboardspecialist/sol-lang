#include "sol/mir_materialize.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SolMirMaterialization *out;
    SolDiagnostics *diagnostics;
    SolMirMaterializeBuildOutcome outcome;
} Builder;

typedef struct { uintptr_t start, end; } Range;

bool sol_mir_materialization_validate_concrete(
    const SolMirMaterialization *owner, SolDiagnostics *diagnostics);
bool sol_mir_materialization_validation_work(
    const SolMirMaterialization *owner, size_t *work);

static bool fail(Builder *b, SolMirMaterializeBuildOutcome outcome,
    const char *message) {
    b->outcome = outcome;
    if (b->diagnostics != NULL) sol_diagnostics_add(b->diagnostics,
        "SOL-MIR-MATERIALIZE-001", SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    return false;
}

static bool validation_error(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) sol_diagnostics_add(diagnostics,
        "SOL-MIR-MATERIALIZE-002", SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    return false;
}

static bool owner_empty(const SolMirMaterialization *owner) {
    if (owner == NULL) return false;
    const unsigned char *bytes = (const unsigned char *)owner;
    for (size_t i = 0; i < sizeof(*owner); ++i) if (bytes[i] != 0) return false;
    return true;
}

void sol_mir_materialization_init(SolMirMaterialization *owner) {
    if (owner != NULL) memset(owner, 0, sizeof(*owner));
}

void sol_mir_materialization_free(SolMirMaterialization *owner) {
    if (owner == NULL) return;
    for (size_t i = 0; owner->images != NULL && i < owner->image_capacity; ++i) {
        sol_mir_free(&owner->images[i].topology);
    }
#define FREE(field) free(owner->field)
    FREE(images); FREE(types); FREE(shape_fields); FREE(shape_variants);
    FREE(type_ids); FREE(accesses); FREE(overlays);
    FREE(contexts); FREE(locals); FREE(places); FREE(projections); FREE(values);
    FREE(instructions); FREE(temporaries); FREE(construct_operands);
    FREE(call_arguments); FREE(blocks); FREE(edges); FREE(edge_values);
    FREE(parameter_values); FREE(loops); FREE(bindings); FREE(imports);
    FREE(handlers); FREE(writebacks); FREE(semantic_sites); FREE(receiver_roots);
    FREE(effect_rows); FREE(effect_atoms);
    FREE(effect_row_atoms); FREE(effect_names); FREE(literal_bytes);
#undef FREE
    sol_mir_materialization_init(owner);
}

SolMirMaterializeLimits sol_mir_materialize_default_limits(void) {
    return (SolMirMaterializeLimits){
        .max_instances = 4096,
        .max_cfg_items = 8000000,
        .max_bindings = 2000000,
        .max_concrete_records = 12000000,
        .max_owned_bytes = 768u * 1024u * 1024u,
        .max_materialization_work = 32000000,
        .max_shape_resolution_work = 16000000,
        .max_validation_work = 1000000000,
    };
}

static bool limits_zero(SolMirMaterializeLimits x) {
    return x.max_instances == 0 && x.max_cfg_items == 0 && x.max_bindings == 0
        && x.max_concrete_records == 0 && x.max_owned_bytes == 0
        && x.max_materialization_work == 0
        && x.max_shape_resolution_work == 0 && x.max_validation_work == 0;
}

static bool limits_complete(SolMirMaterializeLimits x) {
    return x.max_instances != 0 && x.max_cfg_items != 0 && x.max_bindings != 0
        && x.max_concrete_records != 0 && x.max_owned_bytes != 0
        && x.max_materialization_work != 0
        && x.max_shape_resolution_work != 0 && x.max_validation_work != 0;
}

static bool add_limited(size_t *value, size_t amount, size_t limit) {
    if (*value > limit || amount > limit - *value) return false;
    *value += amount;
    return true;
}

static bool charge(Builder *b, size_t amount) {
    return add_limited(&b->out->usage.materialization_work, amount,
        b->out->limits.max_materialization_work) || fail(b,
        SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
        "MIR materialization work limit exceeded");
}

static bool shape_charge(Builder *b, size_t amount) {
    if (!charge(b, amount)) return false;
    return add_limited(&b->out->usage.shape_resolution_work, amount,
        b->out->limits.max_shape_resolution_work) || fail(b,
        SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
        "MIR concrete shape-resolution work limit exceeded");
}

static void *allocate(Builder *b, size_t count, size_t size) {
    if (count == 0) return NULL;
    if (size == 0 || count > SIZE_MAX / size
        || !add_limited(&b->out->usage.owned_bytes, count * size,
            b->out->limits.max_owned_bytes)) {
        fail(b, SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
            "MIR materialization owned-byte limit exceeded");
        return NULL;
    }
    void *result = calloc(count, size);
    if (result == NULL) {
        b->out->usage.owned_bytes -= count * size;
        fail(b, SOL_MIR_MATERIALIZE_BUILD_ALLOCATION_FAILED,
            "MIR materialization allocation failed");
    }
    return result;
}

static bool clone_array(Builder *b, void **target, const void *source,
    size_t count, size_t size) {
    if (!charge(b, count)) return false;
    *target = allocate(b, count, size);
    if (count != 0 && *target == NULL) return false;
    if (count != 0) memcpy(*target, source, count * size);
    return true;
}

static size_t mir_items(const SolMir *mir) {
    size_t counts[] = {mir->block_count, mir->instruction_count, mir->value_count,
        mir->parameter_value_count, mir->edge_value_count, mir->call_argument_count,
        mir->loop_count, mir->construct_operand_count, mir->temporary_count};
    size_t result = 0;
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        if (counts[i] > SIZE_MAX - result) return SIZE_MAX;
        result += counts[i];
    }
    return result;
}

static bool clone_mir(Builder *b, const SolMir *source, SolMir *target) {
    *target = *source;
    target->blocks = NULL; target->instructions = NULL; target->values = NULL;
    target->parameter_values = NULL; target->edge_values = NULL;
    target->call_arguments = NULL; target->loops = NULL;
    target->construct_operands = NULL; target->temporaries = NULL;
#define CLONE(field, count, capacity) do { target->capacity = source->count; \
    if (!clone_array(b, (void **)&target->field, source->field, source->count, \
        sizeof(*source->field))) return false; } while (0)
    CLONE(blocks, block_count, block_capacity);
    CLONE(instructions, instruction_count, instruction_capacity);
    CLONE(values, value_count, value_capacity);
    CLONE(parameter_values, parameter_value_count, parameter_value_capacity);
    CLONE(edge_values, edge_value_count, edge_value_capacity);
    CLONE(call_arguments, call_argument_count, call_argument_capacity);
    CLONE(loops, loop_count, loop_capacity);
    CLONE(construct_operands, construct_operand_count, construct_operand_capacity);
    CLONE(temporaries, temporary_count, temporary_capacity);
#undef CLONE
    return true;
}

static const SolMirProgramTemplate *find_template(const SolMirPlan *plan,
    SolIrCallableId callable) {
    for (size_t i = 0; i < plan->program->template_count; ++i) {
        if (plan->program->templates[i].callable == callable) {
            return &plan->program->templates[i];
        }
    }
    return NULL;
}

static bool source_equal(SolMirProgramSource a, SolMirProgramSource b) {
    return a.callable == b.callable && a.expression == b.expression
        && a.file == b.file && a.start == b.start && a.end == b.end;
}

static bool source_for_term(const SolMirPlan *plan, SolIrCallableId callable,
    const SolMirTerminator *term, SolMirProgramSource *source) {
    *source = (SolMirProgramSource){callable, term->as.invoke.source_expression,
        0, term->span.start, term->span.end};
    for (size_t file = 0; file < plan->program->ir->file_count; ++file) {
        const SolIrSourceFile *item = &plan->program->ir->files[file];
        if (term->span.start >= item->aggregate_start
            && term->span.end <= item->aggregate_end) {
            source->file = file;
            source->start -= item->aggregate_start;
            source->end -= item->aggregate_start;
            return true;
        }
    }
    return false;
}

static const SolMirPlanTypedUse *find_use(const SolMirPlan *plan,
    const SolMirPlanInstance *instance, SolMirPlanTypedUseKind kind,
    size_t source, size_t ordinal, SolMirPlanContextId context) {
    const SolMirPlanTypedUse *result = NULL;
    for (size_t i = 0; i < instance->typed_uses.count; ++i) {
        const SolMirPlanTypedUse *use = &plan->typed_uses[
            instance->typed_uses.offset + i];
        if (use->kind == kind && use->source == source && use->ordinal == ordinal
            && use->context == context) {
            if (result != NULL) return NULL;
            result = use;
        }
    }
    return result;
}

static SolMirMaterializedLocalId local_for(const SolMirMaterialization *out,
    const SolMirMaterializedImage *image, SolIrLocalId source) {
    for (size_t i = 0; i < image->locals.count; ++i) {
        size_t id = image->locals.offset + i;
        if (out->locals[id].source_local == source) return id;
    }
    return SOL_MIR_MATERIALIZED_NONE;
}

static SolMirMaterializedPlaceId place_for(const SolMirMaterialization *out,
    const SolMirMaterializedImage *image, SolMirPlace source) {
    SolMirMaterializedLocalId local = local_for(out, image, source.local);
    for (size_t i = 0; i < image->places.count; ++i) {
        size_t id = image->places.offset + i;
        if (out->places[id].local == local
            && out->places[id].source_place == source.source_place) return id;
    }
    return SOL_MIR_MATERIALIZED_NONE;
}

static bool instruction_place(const SolMirInstruction *instruction,
    SolMirPlace *place) {
    switch (instruction->kind) {
        case SOL_MIR_INST_LOAD_COPY: case SOL_MIR_INST_LOAD_MOVE:
        case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
            *place = instruction->as.place; return true;
        case SOL_MIR_INST_LOAD_UPDATE:
            *place = instruction->as.update_load.place; return true;
        case SOL_MIR_INST_STORE: *place = instruction->as.store.place; return true;
        case SOL_MIR_INST_COMPOUND_UPDATE:
            *place = instruction->as.compound_update.place; return true;
        default: return false;
    }
}

static bool copy_candidate(const SolMirMaterializedType *type) {
    if (type->kind == SOL_IR_TYPE_INT64 || type->kind == SOL_IR_TYPE_BOOL
        || type->kind == SOL_IR_TYPE_TEXT || type->kind == SOL_IR_TYPE_UNIT
        || type->kind == SOL_IR_TYPE_NEVER || type->kind == SOL_IR_TYPE_OPTION
        || type->kind == SOL_IR_TYPE_RESULT || type->kind == SOL_IR_TYPE_TUPLE) {
        return true;
    }
    return type->kind == SOL_IR_TYPE_NOMINAL
        && (type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_RECORD
            || type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_ENUM
            || type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_DISTINCT
            || type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_REFINED);
}

static bool classify_copy(Builder *b) {
    for (size_t i = 0; i < b->out->type_count; ++i) {
        if (!charge(b, 1)) return false;
        b->out->types[i].is_copy = copy_candidate(&b->out->types[i]);
    }
    bool changed;
    do {
        changed = false;
        for (size_t i = 0; i < b->out->type_count; ++i) {
            if (!charge(b, 1)) return false;
            SolMirMaterializedType *type = &b->out->types[i];
            if (!type->is_copy) continue;
            for (size_t j = 0; j < type->ownership_components.count; ++j) {
                if (!charge(b, 1)) return false;
                if (!b->out->types[b->out->type_ids[
                        type->ownership_components.offset + j]].is_copy) {
                    type->is_copy = false; changed = true; break;
                }
            }
        }
    } while (changed);
    return true;
}

static bool source_effects_match(Builder *b,
    const SolIrType *source, SolMirMaterializedEffectRowId row) {
    const SolMirMaterialization *out = b->out;
    const SolIr *ir = out->plan->program->ir;
    if (row >= out->effect_row_count || source->effect_parameter != SOL_IR_NONE
        || source->effects.count != out->effect_rows[row].atoms.count) return false;
    for (size_t i = 0; i < source->effects.count; ++i) {
        if (!shape_charge(b, 1)) return false;
        const SolIrEffect *left = &ir->effects[source->effects.offset + i];
        const SolMirMaterializedEffectAtom *right = &out->effect_atoms[
            out->effect_row_atoms[out->effect_rows[row].atoms.offset + i]];
        if (left->authority_kind != SOL_IR_AUTHORITY_NONE
            || right->authority != SOL_MIR_PLAN_EFFECT_AUTHORITY_NONE
            || strlen(left->name) != right->name.count
            || memcmp(left->name, out->effect_names + right->name.offset,
                right->name.count) != 0) return false;
    }
    return true;
}

static SolMirMaterializedTypeId resolve_source_type(Builder *b,
    SolIrTypeId source_id, SolMirMaterializedTypeId environment, size_t depth);

static bool source_type_matches(Builder *b, const SolIrType *source,
    const SolMirMaterializedType *candidate, SolMirMaterializedTypeId environment,
    size_t depth) {
    const SolIr *ir = b->out->plan->program->ir;
    if (source->kind != candidate->kind || source->definition != candidate->definition
        || source->argument_count != candidate->arguments.count
        || source->parameter_count != candidate->parameters.count) return false;
    for (size_t i = 0; i < source->argument_count; ++i) {
        SolMirMaterializedTypeId child = resolve_source_type(b,
            ir->type_ids[source->argument_offset + i], environment, depth + 1);
        if (child == SOL_MIR_MATERIALIZED_NONE
            || child != b->out->type_ids[candidate->arguments.offset + i]) return false;
    }
    for (size_t i = 0; i < source->parameter_count; ++i) {
        SolMirMaterializedTypeId child = resolve_source_type(b,
            ir->type_ids[source->parameter_offset + i], environment, depth + 1);
        if (child == SOL_MIR_MATERIALIZED_NONE
            || child != b->out->type_ids[candidate->parameters.offset + i]
            || ir->accesses[source->parameter_access_offset + i]
                != b->out->accesses[candidate->parameter_accesses.offset + i]) return false;
    }
    if (source->kind == SOL_IR_TYPE_FUNCTION) {
        SolMirMaterializedTypeId result = resolve_source_type(b, source->result,
            environment, depth + 1);
        if (result != candidate->result
            || !source_effects_match(b, source, candidate->effects)) return false;
    }
    return true;
}

static SolMirMaterializedTypeId resolve_source_type(Builder *b,
    SolIrTypeId source_id, SolMirMaterializedTypeId environment, size_t depth) {
    const SolIr *ir = b->out->plan->program->ir;
    if (source_id >= ir->type_count || depth > b->out->type_count + 1
        || !shape_charge(b, 1)) return SOL_MIR_MATERIALIZED_NONE;
    const SolIrType *source = &ir->types[source_id];
    if (source->kind == SOL_IR_TYPE_SELF) return environment;
    if (source->kind == SOL_IR_TYPE_PARAMETER) {
        if (environment >= b->out->type_count
            || source->definition >= ir->generic_parameter_count) {
            return SOL_MIR_MATERIALIZED_NONE;
        }
        const SolMirMaterializedType *nominal = &b->out->types[environment];
        const SolIrGenericParameter *parameter
            = &ir->generic_parameters[source->definition];
        if (nominal->kind != SOL_IR_TYPE_NOMINAL
            || parameter->owner != nominal->definition
            || parameter->ordinal >= nominal->arguments.count) {
            return SOL_MIR_MATERIALIZED_NONE;
        }
        return b->out->type_ids[nominal->arguments.offset + parameter->ordinal];
    }
    SolMirMaterializedTypeId found = SOL_MIR_MATERIALIZED_NONE;
    for (size_t i = 0; i < b->out->type_count; ++i) {
        if (!shape_charge(b, 1)) return SOL_MIR_MATERIALIZED_NONE;
        if (!source_type_matches(b, source, &b->out->types[i], environment,
                depth)) continue;
        if (found != SOL_MIR_MATERIALIZED_NONE) return SOL_MIR_MATERIALIZED_NONE;
        found = i;
    }
    return found;
}

static bool complete_type_shapes(Builder *b) {
    SolMirMaterialization *out = b->out;
    const SolIr *ir = out->plan->program->ir;
    size_t field_at = 0, variant_at = 0;
    for (size_t id = 0; id < out->type_count; ++id) {
        if (!shape_charge(b, 1)) return false;
        SolMirMaterializedType *type = &out->types[id];
        if (type->kind != SOL_IR_TYPE_NOMINAL
            || type->definition >= ir->definition_count) continue;
        const SolIrDefinition *definition = &ir->definitions[type->definition];
        type->nominal_open = definition->open;
        if (type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_RECORD) {
            type->fields = (SolMirPlanSlice){field_at, definition->fields.count};
            for (size_t i = 0; i < definition->fields.count; ++i) {
                if (!shape_charge(b, 1)) return false;
                SolIrFieldId source_id = definition->fields.offset + i;
                SolMirMaterializedTypeId concrete = resolve_source_type(b,
                    ir->fields[source_id].type, id, 0);
                if (concrete == SOL_MIR_MATERIALIZED_NONE) return false;
                out->shape_fields[field_at++] = (SolMirMaterializedShapeField){
                    source_id, i, concrete};
            }
        } else if (type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_ENUM) {
            type->variants = (SolMirPlanSlice){variant_at,
                definition->variants.count};
            for (size_t i = 0; i < definition->variants.count; ++i) {
                if (!shape_charge(b, 1)) return false;
                SolIrVariantId source_id = definition->variants.offset + i;
                const SolIrVariant *source = &ir->variants[source_id];
                SolMirMaterializedShapeVariant *variant
                    = &out->shape_variants[variant_at++];
                *variant = (SolMirMaterializedShapeVariant){source_id, i,
                    {field_at, source->fields.count}};
                for (size_t j = 0; j < source->fields.count; ++j) {
                    if (!shape_charge(b, 1)) return false;
                    SolIrFieldId field_id = source->fields.offset + j;
                    SolMirMaterializedTypeId concrete = resolve_source_type(b,
                        ir->fields[field_id].type, id, 0);
                    if (concrete == SOL_MIR_MATERIALIZED_NONE) return false;
                    out->shape_fields[field_at++] = (SolMirMaterializedShapeField){
                        field_id, j, concrete};
                }
            }
        } else if (type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_DISTINCT
            || type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_REFINED) {
            type->backing = resolve_source_type(b, definition->representation, id, 0);
            if (type->backing == SOL_MIR_MATERIALIZED_NONE) return false;
        } else if (type->nominal_category
                == SOL_MIR_MATERIALIZED_NOMINAL_CAPABILITY
            && definition->capability_source != SOL_IR_NONE) {
            SolMirMaterializedTypeId reconstructed = resolve_source_type(b,
                ir->locals[definition->capability_source].type, id, 0);
            if (reconstructed == SOL_MIR_MATERIALIZED_NONE
                || reconstructed != type->capability_source) return false;
        }
    }
    return field_at == out->shape_field_capacity
        && variant_at == out->shape_variant_capacity;
}

static size_t term_edge_count(const SolMirTerminator *term) {
    switch (term->kind) {
        case SOL_MIR_TERM_GOTO: case SOL_MIR_TERM_BREAK:
        case SOL_MIR_TERM_CONTINUE: return 1;
        case SOL_MIR_TERM_BRANCH: case SOL_MIR_TERM_CHECK_REFINED:
        case SOL_MIR_TERM_PROPAGATE: return 2;
        case SOL_MIR_TERM_INVOKE:
            return (term->as.invoke.normal_edge.block != SOL_MIR_NONE)
                + (term->as.invoke.failure_edge.block != SOL_MIR_NONE);
        case SOL_MIR_TERM_CHECK_CONTRACT: return 3;
        default: return 0;
    }
}

static size_t term_edge_values(const SolMirTerminator *term) {
    size_t result = 0;
#define ADD(edge) do { if ((edge).block != SOL_MIR_NONE) result += (edge).arguments.count; } while (0)
    switch (term->kind) {
        case SOL_MIR_TERM_GOTO: ADD(term->as.go_to); break;
        case SOL_MIR_TERM_BRANCH:
            ADD(term->as.branch.true_edge); ADD(term->as.branch.false_edge); break;
        case SOL_MIR_TERM_INVOKE:
            ADD(term->as.invoke.normal_edge); ADD(term->as.invoke.failure_edge); break;
        case SOL_MIR_TERM_BREAK: case SOL_MIR_TERM_CONTINUE:
            ADD(term->as.transfer.edge); break;
        case SOL_MIR_TERM_CHECK_REFINED:
            ADD(term->as.check_refined.normal_edge);
            ADD(term->as.check_refined.failure_edge); break;
        case SOL_MIR_TERM_PROPAGATE:
            ADD(term->as.propagate.value_edge);
            ADD(term->as.propagate.residual_edge); break;
        case SOL_MIR_TERM_CHECK_CONTRACT:
            ADD(term->as.check_contract.satisfied_edge);
            ADD(term->as.check_contract.violation_edge);
            ADD(term->as.check_contract.failure_edge); break;
        default: break;
    }
#undef ADD
    return result;
}

static const SolMirPlanDemand *find_demand(const SolMirPlan *plan,
    SolMirPlanInstanceId parent, SolMirPlanContextId context,
    SolMirProgramSource source, SolMirPlanDemandKind first,
    SolMirPlanDemandKind second, size_t *id) {
    const SolMirPlanDemand *result = NULL;
    for (size_t i = 0; i < plan->demand_count; ++i) {
        const SolMirPlanDemand *item = &plan->demands[i];
        if (item->parent == parent && item->context == context
            && (item->kind == first || item->kind == second)
            && source_equal(item->source, source)) {
            if (result != NULL) return NULL;
            result = item; *id = i;
        }
    }
    return result;
}

static SolMirMaterializedBindingId binding_for_demand(
    const SolMirMaterialization *out, size_t demand) {
    for (size_t id = 0; id < out->binding_count; ++id) {
        if (out->bindings[id].source_demand == demand) return id;
    }
    return SOL_MIR_MATERIALIZED_NONE;
}

static SolMirMaterializedTypeId argument_type(const SolMirPlan *plan,
    const SolMirPlanInstance *parent, const SolMirCallArgument *argument) {
    const SolMirPlanTypedUse *use = NULL;
    if (argument->access == SOL_ACCESS_OWNED) use = find_use(plan, parent,
        SOL_MIR_PLAN_USE_MIR_TEMPORARY, argument->temporary, 0,
        parent->contexts.offset);
    else if (argument->place != SOL_IR_NONE) use = find_use(plan, parent,
        SOL_MIR_PLAN_USE_PLACE_FINAL, argument->place, 0,
        parent->contexts.offset);
    return use == NULL ? SOL_MIR_MATERIALIZED_NONE : use->type;
}

static bool verify_signature(const SolMirMaterialization *out,
    const SolMirPlanInstance *parent, const SolMir *mir,
    const SolMirTerminator *term, const SolMirPlanDemand *demand) {
    SolMirPlanSlice parameters, accesses;
    SolMirMaterializedTypeId receiver, result;
    if (demand->instance != SOL_MIR_PLAN_NONE) {
        const SolMirPlanInstance *target = &out->plan->instances[demand->instance];
        parameters = target->parameter_types; accesses = target->parameter_accesses;
        receiver = target->receiver; result = target->result;
    } else {
        const SolMirPlanImport *target = &out->plan->imports[demand->import];
        parameters = target->parameter_types; accesses = target->parameter_accesses;
        receiver = target->receiver; result = target->result;
    }
    if (parameters.count != term->as.invoke.arguments.count
        || accesses.count != parameters.count) return false;
    for (size_t i = 0; i < parameters.count; ++i) {
        const SolMirCallArgument *argument = &mir->call_arguments[
            term->as.invoke.arguments.offset + i];
        if (argument->formal != i
            || argument->access != out->plan->instance_accesses[accesses.offset + i]
            || argument_type(out->plan, parent, argument)
                != out->plan->instance_type_ids[parameters.offset + i]) return false;
    }
    if ((receiver == SOL_MIR_MATERIALIZED_NONE)
            != (term->as.invoke.receiver.source_expression == SOL_IR_NONE)
        || (receiver != SOL_MIR_MATERIALIZED_NONE
            && argument_type(out->plan, parent, &term->as.invoke.receiver)
                != receiver)) return false;
    if (term->as.invoke.result == SOL_MIR_NONE) {
        return out->types[result].kind == SOL_IR_TYPE_NEVER
            && term->as.invoke.normal_edge.block == SOL_MIR_NONE;
    }
    const SolMirPlanTypedUse *use = find_use(out->plan, parent,
        SOL_MIR_PLAN_USE_MIR_VALUE, term->as.invoke.result, 0,
        parent->contexts.offset);
    return use != NULL && use->type == result;
}

static void binding_signature(const SolMirMaterialization *out,
    const SolMirMaterializedBinding *binding, SolMirMaterializedTypeId *receiver,
    SolMirPlanSlice *parameters, SolMirPlanSlice *accesses,
    SolMirMaterializedTypeId *result) {
    if (binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE) {
        const SolMirPlanInstance *target = &out->plan->instances[binding->instance];
        *receiver = target->receiver; *parameters = target->parameter_types;
        *accesses = target->parameter_accesses; *result = target->result;
    } else {
        const SolMirPlanImport *target = &out->plan->imports[binding->import];
        *receiver = target->receiver; *parameters = target->parameter_types;
        *accesses = target->parameter_accesses; *result = target->result;
    }
}

static bool handler_signatures_match(const SolMirMaterialization *out,
    const SolMirMaterializedHandler *handler) {
    const SolMirMaterializedBinding *source = &out->bindings[
        handler->source_binding];
    const SolMirMaterializedBinding *provider = &out->bindings[
        handler->provider_binding];
    SolMirMaterializedTypeId source_receiver, provider_receiver;
    SolMirMaterializedTypeId source_result, provider_result;
    SolMirPlanSlice source_parameters, provider_parameters;
    SolMirPlanSlice source_accesses, provider_accesses;
    binding_signature(out, source, &source_receiver, &source_parameters,
        &source_accesses, &source_result);
    binding_signature(out, provider, &provider_receiver, &provider_parameters,
        &provider_accesses, &provider_result);
    if (source->kind != SOL_MIR_PLAN_DEMAND_HANDLER_SOURCE
        || provider->kind != SOL_MIR_PLAN_DEMAND_HANDLER_PROVIDER
        || source->parent != handler->parent || provider->parent != handler->parent
        || source->context != handler->context || provider->context != handler->context
        || source->symbolic_callable != handler->handled_operation
        || source_receiver != out->places[handler->authority].final_type
        || provider_receiver != out->places[handler->provider].final_type
        || source_result != provider_result
        || source_parameters.count != provider_parameters.count
        || source_accesses.count != provider_accesses.count) return false;
    for (size_t i = 0; i < source_parameters.count; ++i) {
        if (out->plan->instance_type_ids[source_parameters.offset + i]
                != out->plan->instance_type_ids[provider_parameters.offset + i]
            || out->plan->instance_accesses[source_accesses.offset + i]
                != out->plan->instance_accesses[provider_accesses.offset + i]) {
            return false;
        }
    }
    return true;
}

static bool translate_argument(const SolMirMaterialization *out,
    const SolMirMaterializedImage *image, const SolMirPlanInstance *instance,
    const SolMirCallArgument *source, SolMirMaterializedCallArgument *target) {
    *target = (SolMirMaterializedCallArgument){source->formal, source->access,
        source->source_expression, SOL_MIR_MATERIALIZED_NONE,
        SOL_MIR_MATERIALIZED_NONE, SOL_MIR_MATERIALIZED_NONE};
    if (source->source_expression == SOL_IR_NONE) return true;
    if (source->access == SOL_ACCESS_OWNED) {
        const SolMirPlanTypedUse *type = find_use(out->plan, instance,
            SOL_MIR_PLAN_USE_MIR_TEMPORARY, source->temporary, 0,
            instance->contexts.offset);
        if (type == NULL || source->temporary >= image->temporaries.count) return false;
        target->type = type->type;
        target->temporary = image->temporaries.offset + source->temporary;
    } else {
        if (source->place >= out->plan->program->ir->place_count) return false;
        const SolMirPlanTypedUse *type = find_use(out->plan, instance,
            SOL_MIR_PLAN_USE_PLACE_FINAL, source->place, 0,
            instance->contexts.offset);
        const SolIrPlace *place = &out->plan->program->ir->places[source->place];
        if (type == NULL) return false;
        target->type = type->type;
        target->place = place_for(out, image,
            (SolMirPlace){place->local, source->place});
        if (target->place == SOL_MIR_MATERIALIZED_NONE) return false;
    }
    return true;
}

static SolMirMaterializedHandlerId handler_for(const SolMirMaterialization *out,
    const SolMirMaterializedImage *image, SolIrExpressionId expression) {
    for (size_t i = 0; i < image->handlers.count; ++i) {
        size_t id = image->handlers.offset + i;
        if (out->handlers[id].source_expression == expression) return id;
    }
    return SOL_MIR_MATERIALIZED_NONE;
}

static SolMirMaterializedTypeId expression_type_for_site(
    const SolMirMaterialization *out, const SolMirMaterializedImage *image,
    SolMirPlanContextId context, SolIrExpressionId expression) {
    SolMirMaterializedTypeId found = SOL_MIR_MATERIALIZED_NONE;
    for (size_t i = 0; i < image->overlays.count; ++i) {
        const SolMirMaterializedTypeOverlay *overlay
            = &out->overlays[image->overlays.offset + i];
        if (overlay->kind != SOL_MIR_PLAN_USE_EXPRESSION
            || overlay->context != context || overlay->source != expression) continue;
        if (found != SOL_MIR_MATERIALIZED_NONE) return SOL_MIR_MATERIALIZED_NONE;
        found = overlay->type;
    }
    return found;
}

static bool translate_instruction(SolMirMaterialization *out,
    const SolMirMaterializedImage *image, const SolMirPlanInstance *instance,
    const SolMirInstruction *source, size_t source_id,
    SolMirMaterializedInstruction *target) {
    memset(target, 0, sizeof(*target));
    target->kind = source->kind;
    target->block = image->blocks.offset + source->block;
    target->result = source->result == SOL_MIR_NONE ? SOL_MIR_MATERIALIZED_NONE
        : image->values.offset + source->result;
    target->type = target->local = target->place = target->left = target->right
        = target->temporary = target->previous = target->pattern_scrutinee
        = target->handler = SOL_MIR_MATERIALIZED_NONE;
    target->source_statement = target->match_expression = target->source_arm
        = target->source_pattern = target->source_snapshot = SOL_IR_NONE;
    target->construct_definition = target->construct_variant = SOL_IR_NONE;
    target->source_expression = source->source_expression;
    target->span = source->span;
    if (source->type != SOL_IR_NONE) {
        const SolMirPlanTypedUse *type = find_use(out->plan, instance,
            SOL_MIR_PLAN_USE_MIR_INSTRUCTION, source_id, 0,
            instance->contexts.offset);
        if (type == NULL) return false;
        target->type = type->type;
    }
    SolMirPlace mir_place;
    if (instruction_place(source, &mir_place)) {
        target->place = place_for(out, image, mir_place);
        if (target->place == SOL_MIR_MATERIALIZED_NONE) return false;
    }
    switch (source->kind) {
        case SOL_MIR_INST_CONST_INT64: target->integer = source->as.integer; break;
        case SOL_MIR_INST_CONST_BOOL: target->boolean = source->as.boolean; break;
        case SOL_MIR_INST_CONST_TEXT: {
            const char *text = out->plan->program->ir->expressions[
                source->source_expression].as.string;
            size_t length = strlen(text);
            target->text = (SolMirPlanSlice){out->literal_byte_count, length};
            memcpy(out->literal_bytes + out->literal_byte_count, text, length + 1);
            out->literal_byte_count += length + 1;
            break;
        }
        case SOL_MIR_INST_PARAMETER_LIVE: case SOL_MIR_INST_STORAGE_LIVE:
        case SOL_MIR_INST_DROP_IF_INITIALIZED: case SOL_MIR_INST_STORAGE_DEAD:
            target->local = local_for(out, image, source->as.local); break;
        case SOL_MIR_INST_LOAD_UPDATE:
            target->source_statement = source->as.update_load.statement; break;
        case SOL_MIR_INST_STORE:
            target->left = image->values.offset + source->as.store.value; break;
        case SOL_MIR_INST_UNARY:
            target->operator_kind = source->as.unary.operator_kind;
            target->left = image->values.offset + source->as.unary.operand; break;
        case SOL_MIR_INST_BINARY:
            target->operator_kind = source->as.binary.operator_kind;
            target->left = image->values.offset + source->as.binary.left;
            target->right = image->values.offset + source->as.binary.right; break;
        case SOL_MIR_INST_COMPOUND_UPDATE:
            target->operator_kind = source->as.compound_update.operator_kind;
            target->previous = image->temporaries.offset
                + source->as.compound_update.previous;
            target->right = image->values.offset + source->as.compound_update.right;
            target->source_statement = source->as.compound_update.statement; break;
        case SOL_MIR_INST_REGION_ENTER: case SOL_MIR_INST_REGION_EXIT:
            target->source_statement = source->as.region; break;
        case SOL_MIR_INST_TEMPORARY_INIT:
            target->temporary = image->temporaries.offset
                + source->as.temporary_init.temporary;
            target->left = image->values.offset + source->as.temporary_init.value; break;
        case SOL_MIR_INST_TEMPORARY_DROP:
            target->temporary = image->temporaries.offset
                + source->as.temporary_drop.temporary;
            target->preserve_depth = source->as.temporary_drop.preserve_depth; break;
        case SOL_MIR_INST_EXPRESSION_RESULT:
            target->left = image->values.offset + source->as.operand; break;
        case SOL_MIR_INST_PATTERN_TEST: case SOL_MIR_INST_PATTERN_VALUE:
        case SOL_MIR_INST_MATCH_ARM:
            target->match_expression = source->as.pattern.match_expression;
            target->source_arm = source->as.pattern.arm;
            target->arm_ordinal = source->as.pattern.arm_ordinal;
            target->source_pattern = source->as.pattern.pattern;
            target->pattern_scrutinee = image->temporaries.offset
                + source->as.pattern.scrutinee; break;
        case SOL_MIR_INST_HANDLER_ENTER: case SOL_MIR_INST_HANDLER_EXIT:
            target->handler = handler_for(out, image, source->source_expression);
            if (target->handler == SOL_MIR_MATERIALIZED_NONE) return false;
            break;
        case SOL_MIR_INST_CONSTRUCT:
            target->construct_kind = source->as.construct.kind;
            target->construct_definition = source->as.construct.definition;
            target->construct_variant = source->as.construct.variant;
            target->construct_operands = (SolMirPlanSlice){
                image->construct_operands.offset + source->as.construct.operands.offset,
                source->as.construct.operands.count};
            target->source_capability_roots = source->as.construct.capability_roots;
            target->source_operation_roots = source->as.construct.operation_roots;
            break;
        case SOL_MIR_INST_CAPTURE_SNAPSHOT:
            target->source_snapshot = source->as.snapshot;
            {
                const SolMirPlanTypedUse *snapshot = NULL;
                for (size_t u = 0; u < instance->typed_uses.count; ++u) {
                    const SolMirPlanTypedUse *candidate = &out->plan->typed_uses[
                        instance->typed_uses.offset + u];
                    if (candidate->kind == SOL_MIR_PLAN_USE_SNAPSHOT
                        && candidate->source == source->as.snapshot) {
                        if (snapshot != NULL) return false;
                        snapshot = candidate;
                    }
                }
                if (snapshot == NULL) return false;
                target->type = snapshot->type;
            }
            break;
        default: break;
    }
    if (source->kind == SOL_MIR_INST_LOAD_MOVE
        && target->place != SOL_MIR_MATERIALIZED_NONE
        && out->types[out->places[target->place].final_type].is_copy) {
        target->kind = SOL_MIR_INST_LOAD_COPY;
    }
    return true;
}

static SolMirMaterializedEdgeId append_edge(SolMirMaterialization *out,
    const SolMirMaterializedImage *image, const SolMir *mir, SolMirEdge source) {
    if (source.block == SOL_MIR_NONE) return SOL_MIR_MATERIALIZED_NONE;
    size_t id = out->edge_count++;
    SolMirMaterializedEdge *edge = &out->edges[id];
    edge->block = image->blocks.offset + source.block;
    edge->arguments = (SolMirPlanSlice){out->edge_value_count, source.arguments.count};
    for (size_t i = 0; i < source.arguments.count; ++i) {
        out->edge_values[out->edge_value_count++] = image->values.offset
            + mir->edge_values[source.arguments.offset + i];
    }
    return id;
}

static void init_term(SolMirMaterializedTerminator *term) {
    memset(term, 0, sizeof(*term));
    term->source_expression = term->source_statement = term->source_definition
        = term->source_obligation = SOL_IR_NONE;
    term->binding = term->effects = term->callee = term->result = term->value
        = term->condition = term->value_result = term->residual_result
        = term->representation = term->operand = term->edge = term->true_edge
        = term->false_edge = term->normal_edge = term->failure_edge
        = term->value_edge = term->residual_edge = term->satisfied_edge
        = term->violation_edge = term->loop = term->callable_site
        = SOL_MIR_MATERIALIZED_NONE;
    term->receiver.type = term->receiver.temporary = term->receiver.place
        = SOL_MIR_MATERIALIZED_NONE;
    term->receiver.source_expression = SOL_IR_NONE;
}

static bool translate_terminator(Builder *b, SolMirPlanInstanceId parent_id,
    const SolMirPlanInstance *instance, const SolMirMaterializedImage *image,
    const SolMir *mir, const SolMirTerminator *source,
    SolMirMaterializedTerminator *target) {
    init_term(target); target->kind = source->kind; target->span = source->span;
#define VALUE(v) ((v) == SOL_MIR_NONE ? SOL_MIR_MATERIALIZED_NONE : image->values.offset + (v))
#define TEMP(v) ((v) == SOL_MIR_NONE ? SOL_MIR_MATERIALIZED_NONE : image->temporaries.offset + (v))
    switch (source->kind) {
        case SOL_MIR_TERM_GOTO:
            target->edge = append_edge(b->out, image, mir, source->as.go_to); break;
        case SOL_MIR_TERM_BRANCH:
            target->condition = VALUE(source->as.branch.condition);
            target->true_edge = append_edge(b->out, image, mir,
                source->as.branch.true_edge);
            target->false_edge = append_edge(b->out, image, mir,
                source->as.branch.false_edge); break;
        case SOL_MIR_TERM_RETURN: case SOL_MIR_TERM_PANIC:
        case SOL_MIR_TERM_RESUME_FAILURE:
            target->value = VALUE(source->as.value); break;
        case SOL_MIR_TERM_INVOKE: {
            SolMirProgramSource provenance;
            size_t binding_id = 0;
            if (!source_for_term(b->out->plan, instance->callable, source,
                    &provenance)) return false;
            const SolMirPlanDemand *demand = find_demand(b->out->plan, parent_id,
                instance->contexts.offset, provenance, SOL_MIR_PLAN_DEMAND_INVOKE,
                SOL_MIR_PLAN_DEMAND_CALLBACK, &binding_id);
            if (demand == NULL || !verify_signature(b->out, instance, mir, source,
                    demand)) return false;
            binding_id = binding_for_demand(b->out, binding_id);
            if (binding_id == SOL_MIR_MATERIALIZED_NONE) return false;
            target->source_expression = source->as.invoke.source_expression;
            target->call_kind = source->as.invoke.kind;
            target->binding = binding_id;
            target->effects = demand->instance != SOL_MIR_PLAN_NONE
                ? b->out->plan->instances[demand->instance].effects
                : b->out->plan->imports[demand->import].effects;
            target->callee = TEMP(source->as.invoke.callee);
            if (!translate_argument(b->out, image, instance,
                    &source->as.invoke.receiver, &target->receiver)) return false;
            target->arguments = (SolMirPlanSlice){image->call_arguments.offset
                + source->as.invoke.arguments.offset,
                source->as.invoke.arguments.count};
            target->result = VALUE(source->as.invoke.result);
            target->normal_edge = append_edge(b->out, image, mir,
                source->as.invoke.normal_edge);
            target->failure_edge = append_edge(b->out, image, mir,
                source->as.invoke.failure_edge);
            target->writebacks.offset = b->out->writeback_count;
            if (target->normal_edge != SOL_MIR_MATERIALIZED_NONE
                && target->receiver.access == SOL_ACCESS_EXCLUSIVE) {
                b->out->writebacks[b->out->writeback_count++]
                    = (SolMirMaterializedWriteback){true, 0,
                        target->receiver.place, target->receiver.type};
            }
            if (target->normal_edge != SOL_MIR_MATERIALIZED_NONE) {
                for (size_t i = 0; i < target->arguments.count; ++i) {
                    const SolMirMaterializedCallArgument *argument
                        = &b->out->call_arguments[target->arguments.offset + i];
                    if (argument->access == SOL_ACCESS_EXCLUSIVE) {
                        b->out->writebacks[b->out->writeback_count++]
                            = (SolMirMaterializedWriteback){false,
                                argument->formal, argument->place, argument->type};
                    }
                }
            }
            target->writebacks.count = b->out->writeback_count
                - target->writebacks.offset;
            break;
        }
        case SOL_MIR_TERM_UNREACHABLE:
            target->source_statement = source->as.unreachable.statement;
            target->obligation_ordinal = source->as.unreachable.obligation; break;
        case SOL_MIR_TERM_BREAK: case SOL_MIR_TERM_CONTINUE:
            target->source_statement = source->as.transfer.statement;
            target->loop = image->loops.offset + source->as.transfer.loop;
            target->edge = append_edge(b->out, image, mir,
                source->as.transfer.edge); break;
        case SOL_MIR_TERM_CHECK_REFINED:
            target->source_expression = source->as.check_refined.source_expression;
            target->source_definition = source->as.check_refined.definition;
            target->source_obligation = source->as.check_refined.obligation;
            target->representation = TEMP(source->as.check_refined.representation);
            target->result = VALUE(source->as.check_refined.result);
            target->normal_edge = append_edge(b->out, image, mir,
                source->as.check_refined.normal_edge);
            target->failure_edge = append_edge(b->out, image, mir,
                source->as.check_refined.failure_edge); break;
        case SOL_MIR_TERM_MATCH_FAILURE:
            target->source_expression = source->as.match_failure; break;
        case SOL_MIR_TERM_PROPAGATE:
            target->source_expression = source->as.propagate.source_expression;
            target->propagation_kind = source->as.propagate.kind;
            target->operand = TEMP(source->as.propagate.operand);
            target->value_result = VALUE(source->as.propagate.value_result);
            target->residual_result = VALUE(source->as.propagate.residual_result);
            target->value_edge = append_edge(b->out, image, mir,
                source->as.propagate.value_edge);
            target->residual_edge = append_edge(b->out, image, mir,
                source->as.propagate.residual_edge); break;
        case SOL_MIR_TERM_CHECK_CONTRACT:
            target->source_obligation = source->as.check_contract.obligation;
            target->contract_phase = source->as.check_contract.phase;
            target->contract_outcome = source->as.check_contract.outcome;
            target->result = VALUE(source->as.check_contract.result);
            target->satisfied_edge = append_edge(b->out, image, mir,
                source->as.check_contract.satisfied_edge);
            target->violation_edge = append_edge(b->out, image, mir,
                source->as.check_contract.violation_edge);
            target->failure_edge = append_edge(b->out, image, mir,
                source->as.check_contract.failure_edge); break;
        case SOL_MIR_TERM_CONTRACT_VIOLATION:
            target->source_obligation = source->as.contract_violation; break;
        case SOL_MIR_TERM_INVALID: return false;
    }
#undef VALUE
#undef TEMP
    return true;
}

static bool add_count(size_t *total, size_t amount) {
    if (amount > SIZE_MAX - *total) return false;
    *total += amount; return true;
}

static bool build_scratch(Builder *b) {
    SolMirMaterialization *out = b->out;
    const SolMirPlan *plan = out->plan;
    if (plan->instance_count > out->limits.max_instances) return fail(b,
        SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
        "materialized instance limit exceeded");
    size_t types = plan->type_component_count, accesses = plan->type_parameter_access_count;
    size_t locals = 0, places = 0, projections = 0, values = 0, instructions = 0;
    size_t temporaries = 0, operands = 0, arguments = 0, blocks = 0, loops = 0;
    size_t edges = 0, edge_values = 0, parameters = 0, handlers = 0, writebacks = 0;
    size_t symbolic_cfg = 0, effect_names = 0, literal_bytes = 0;
    size_t shape_fields = 0, shape_variants = 0;
    size_t receiver_roots = 0;
    for (size_t i = 0; i < plan->effect_atom_count; ++i) {
        if (!add_count(&effect_names, plan->effect_atoms[i].length + 1)) return false;
    }
    for (size_t id = 0; id < plan->instance_count; ++id) {
        const SolMirPlanInstance *instance = &plan->instances[id];
        const SolMirProgramTemplate *tpl = find_template(plan, instance->callable);
        if (tpl == NULL) return fail(b, SOL_MIR_MATERIALIZE_BUILD_UNSUPPORTED_OR_UNRESOLVED,
            "materialized instance has no MIR template");
        size_t items = mir_items(&tpl->mir);
        if (items == SIZE_MAX || !add_count(&symbolic_cfg, items)) return false;
        if (!add_count(&types, instance->type_arguments.count)
            || !add_count(&types, instance->parameter_types.count)
            || !add_count(&accesses, instance->parameter_accesses.count)
            || !add_count(&values, tpl->mir.value_count)
            || !add_count(&instructions, tpl->mir.instruction_count)
            || !add_count(&temporaries, tpl->mir.temporary_count)
            || !add_count(&operands, tpl->mir.construct_operand_count)
            || !add_count(&arguments, tpl->mir.call_argument_count)
            || !add_count(&blocks, tpl->mir.block_count)
            || !add_count(&loops, tpl->mir.loop_count)
            || !add_count(&parameters, tpl->mir.parameter_value_count)) return false;
        for (size_t u = 0; u < instance->typed_uses.count; ++u) {
            const SolMirPlanTypedUse *use = &plan->typed_uses[instance->typed_uses.offset + u];
            if (use->context != instance->contexts.offset) continue;
            if (use->kind == SOL_MIR_PLAN_USE_LOCAL) ++locals;
            if (use->kind == SOL_MIR_PLAN_USE_PLACE_ROOT) {
                ++places;
                if (!add_count(&projections,
                        plan->program->ir->places[use->source].projections.count)) return false;
            }
        }
        for (size_t n = 0; n < tpl->mir.instruction_count; ++n) {
            SolMirPlace place;
            const SolMirInstruction *instruction = &tpl->mir.instructions[n];
            if (instruction->kind == SOL_MIR_INST_CONST_TEXT) {
                const char *text = plan->program->ir->expressions[
                    instruction->source_expression].as.string;
                if (!add_count(&literal_bytes, strlen(text) + 1)) return false;
            }
            if (instruction->kind == SOL_MIR_INST_HANDLER_ENTER) ++handlers;
            if (!instruction_place(instruction, &place) || place.source_place != SOL_IR_NONE) continue;
            bool seen = false;
            for (size_t p = 0; p < n; ++p) {
                SolMirPlace previous;
                if (instruction_place(&tpl->mir.instructions[p], &previous)
                    && previous.source_place == SOL_IR_NONE
                    && previous.local == place.local) seen = true;
            }
            if (!seen) ++places;
        }
        for (size_t k = 0; k < tpl->mir.block_count; ++k) {
            const SolMirTerminator *term = &tpl->mir.blocks[k].terminator;
            if (!add_count(&edges, term_edge_count(term))
                || !add_count(&edge_values, term_edge_values(term))) return false;
            if (term->kind == SOL_MIR_TERM_INVOKE
                && term->as.invoke.normal_edge.block != SOL_MIR_NONE) {
                writebacks += term->as.invoke.receiver.access == SOL_ACCESS_EXCLUSIVE;
                for (size_t a = 0; a < term->as.invoke.arguments.count; ++a) {
                    writebacks += tpl->mir.call_arguments[
                        term->as.invoke.arguments.offset + a].access
                        == SOL_ACCESS_EXCLUSIVE;
                }
            }
        }
    }
    for (size_t i = 0; i < plan->import_count; ++i) {
        if (!add_count(&types, plan->imports[i].parameter_types.count)
            || !add_count(&accesses, plan->imports[i].parameter_accesses.count)) return false;
    }
    for (size_t i = 0; i < plan->demand_count; ++i) {
        const SolMirPlanDemand *demand = &plan->demands[i];
        if (demand->kind != SOL_MIR_PLAN_DEMAND_BOUND_OPERATION) continue;
        if (demand->source.expression >= plan->program->ir->expression_count)
            return false;
        const SolIrExpression *operation = &plan->program->ir->expressions[
            demand->source.expression];
        if (operation->kind != SOL_IR_EXPR_BOUND_OPERATION
            || operation->as.operation.receiver
                >= plan->program->ir->expression_count) return false;
        const SolIrExpression *receiver = &plan->program->ir->expressions[
            operation->as.operation.receiver];
        if (!add_count(&receiver_roots, receiver->capability_roots.count)) return false;
    }
    for (size_t i = 0; i < plan->type_count; ++i) {
        const SolMirPlanType *type = &plan->types[i];
        if (type->kind != SOL_IR_TYPE_NOMINAL
            || type->definition >= plan->program->ir->definition_count) continue;
        const SolIrDefinition *definition
            = &plan->program->ir->definitions[type->definition];
        if (definition->kind == SOL_IR_DEFINITION_RECORD) {
            if (!add_count(&shape_fields, definition->fields.count)) return false;
        } else if (definition->kind == SOL_IR_DEFINITION_ENUM) {
            if (!add_count(&shape_variants, definition->variants.count)) return false;
            for (size_t v = 0; v < definition->variants.count; ++v) {
                const SolIrVariant *variant = &plan->program->ir->variants[
                    definition->variants.offset + v];
                if (!add_count(&shape_fields, variant->fields.count)) return false;
            }
        }
    }
    size_t concrete = 0, cfg = 0, bindings = 0;
    size_t records[] = {plan->type_count, shape_fields, shape_variants,
        plan->context_count, receiver_roots,
        plan->typed_use_count, locals, places, projections, values, instructions,
        temporaries, operands, arguments, blocks, edges, edge_values, parameters,
        loops, plan->demand_count, plan->demand_count, plan->import_count, handlers, writebacks,
        plan->effect_row_count, plan->effect_atom_count,
        plan->effect_row_atom_count};
    for (size_t i = 0; i < sizeof(records) / sizeof(records[0]); ++i) {
        if (!add_count(&concrete, records[i])) return fail(b,
            SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
            "materialized concrete record count overflowed");
    }
    size_t cfg_counts[] = {symbolic_cfg, blocks, edges, edge_values, parameters, loops};
    for (size_t i = 0; i < sizeof(cfg_counts) / sizeof(cfg_counts[0]); ++i) {
        if (!add_count(&cfg, cfg_counts[i])) return fail(b,
            SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
            "materialized CFG item count overflowed");
    }
    if (!add_count(&bindings, plan->typed_use_count)
        || !add_count(&bindings, plan->demand_count)
        || !add_count(&bindings, handlers)) return fail(b,
            SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
            "materialized binding count overflowed");
    if (cfg > out->limits.max_cfg_items || bindings > out->limits.max_bindings
        || concrete > out->limits.max_concrete_records) return fail(b,
        SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
        "materialized concrete record limit exceeded");
#define ALLOC(field, count, capacity, type) do { out->field = allocate(b, (count), sizeof(type)); \
    out->capacity = (count); if ((count) != 0 && out->field == NULL) return false; } while (0)
    ALLOC(images, plan->instance_count, image_capacity, *out->images);
    ALLOC(types, plan->type_count, type_capacity, *out->types);
    ALLOC(shape_fields, shape_fields, shape_field_capacity, *out->shape_fields);
    ALLOC(shape_variants, shape_variants, shape_variant_capacity,
        *out->shape_variants);
    ALLOC(type_ids, types, type_id_capacity, *out->type_ids);
    ALLOC(accesses, accesses, access_capacity, *out->accesses);
    ALLOC(overlays, plan->typed_use_count, overlay_capacity, *out->overlays);
    ALLOC(contexts, plan->context_count, context_capacity, *out->contexts);
    ALLOC(locals, locals, local_capacity, *out->locals);
    ALLOC(places, places, place_capacity, *out->places);
    ALLOC(projections, projections, projection_capacity, *out->projections);
    ALLOC(values, values, value_capacity, *out->values);
    ALLOC(instructions, instructions, instruction_capacity, *out->instructions);
    ALLOC(temporaries, temporaries, temporary_capacity, *out->temporaries);
    ALLOC(construct_operands, operands, construct_operand_capacity, *out->construct_operands);
    ALLOC(call_arguments, arguments, call_argument_capacity, *out->call_arguments);
    ALLOC(blocks, blocks, block_capacity, *out->blocks);
    ALLOC(edges, edges, edge_capacity, *out->edges);
    ALLOC(edge_values, edge_values, edge_value_capacity, *out->edge_values);
    ALLOC(parameter_values, parameters, parameter_value_capacity, *out->parameter_values);
    ALLOC(loops, loops, loop_capacity, *out->loops);
    ALLOC(bindings, plan->demand_count, binding_capacity, *out->bindings);
    ALLOC(semantic_sites, plan->demand_count, semantic_site_capacity, *out->semantic_sites);
    ALLOC(receiver_roots, receiver_roots, receiver_root_capacity,
        *out->receiver_roots);
    ALLOC(imports, plan->import_count, import_capacity, *out->imports);
    ALLOC(handlers, handlers, handler_capacity, *out->handlers);
    ALLOC(writebacks, writebacks, writeback_capacity, *out->writebacks);
    ALLOC(effect_rows, plan->effect_row_count, effect_row_capacity, *out->effect_rows);
    ALLOC(effect_atoms, plan->effect_atom_count, effect_atom_capacity, *out->effect_atoms);
    ALLOC(effect_row_atoms, plan->effect_row_atom_count, effect_row_atom_capacity, *out->effect_row_atoms);
    ALLOC(effect_names, effect_names, effect_name_capacity, *out->effect_names);
    ALLOC(literal_bytes, literal_bytes, literal_byte_capacity, *out->literal_bytes);
#undef ALLOC
    out->image_count = out->image_capacity; out->type_count = out->type_capacity;
    out->context_count = out->context_capacity; out->effect_row_count = out->effect_row_capacity;
    out->effect_atom_count = out->effect_atom_capacity;
    out->effect_row_atom_count = out->effect_row_atom_capacity;
    out->effect_name_count = out->effect_name_capacity;
    size_t name_at = 0;
    for (size_t i = 0; i < plan->effect_atom_count; ++i) {
        const SolMirPlanEffectAtom *source = &plan->effect_atoms[i];
        out->effect_atoms[i] = (SolMirMaterializedEffectAtom){
            {name_at, source->length}, source->authority, source->ordinal};
        memcpy(out->effect_names + name_at, source->name, source->length + 1);
        name_at += source->length + 1;
    }
    for (size_t i = 0; i < plan->effect_row_count; ++i) {
        out->effect_rows[i].atoms = (SolMirPlanSlice){plan->effect_rows[i].atom_offset,
            plan->effect_rows[i].atom_count};
    }
    if (plan->effect_row_atom_count != 0) memcpy(out->effect_row_atoms,
        plan->effect_row_atoms, plan->effect_row_atom_count * sizeof(*out->effect_row_atoms));
    size_t type_at = 0, access_at = 0;
    for (size_t i = 0; i < plan->type_count; ++i) {
        const SolMirPlanType *source = &plan->types[i];
        SolMirMaterializedNominalCategory category
            = SOL_MIR_MATERIALIZED_NOMINAL_NONE;
        if (source->kind == SOL_IR_TYPE_NOMINAL
            && source->definition < plan->program->ir->definition_count) {
            switch (plan->program->ir->definitions[source->definition].kind) {
                case SOL_IR_DEFINITION_RECORD:
                    category = SOL_MIR_MATERIALIZED_NOMINAL_RECORD; break;
                case SOL_IR_DEFINITION_ENUM:
                    category = SOL_MIR_MATERIALIZED_NOMINAL_ENUM; break;
                case SOL_IR_DEFINITION_DISTINCT:
                    category = SOL_MIR_MATERIALIZED_NOMINAL_DISTINCT; break;
                case SOL_IR_DEFINITION_REFINED:
                    category = SOL_MIR_MATERIALIZED_NOMINAL_REFINED; break;
                case SOL_IR_DEFINITION_CAPABILITY:
                    category = SOL_MIR_MATERIALIZED_NOMINAL_CAPABILITY; break;
                default: break;
            }
        }
        out->types[i] = (SolMirMaterializedType){
            .kind = source->kind,
            .definition = source->definition,
            .nominal_category = category,
            .arguments = {type_at, source->argument_count},
            .parameters = {type_at + source->argument_count,
                source->parameter_count},
            .parameter_accesses = {access_at, source->parameter_count},
            .result = source->result,
            .effects = source->effects,
            .backing = SOL_MIR_MATERIALIZED_NONE,
            .capability_source = source->capability_source,
            .ownership_components = {
                type_at + source->argument_count + source->parameter_count,
                source->ownership_component_count},
        };
        size_t count = source->argument_count + source->parameter_count
            + source->ownership_component_count;
        if (count != 0) memcpy(out->type_ids + type_at,
            plan->type_components + source->argument_offset,
            count * sizeof(*out->type_ids));
        if (source->parameter_count != 0) memcpy(out->accesses + access_at,
            plan->type_parameter_accesses + source->parameter_access_offset,
            source->parameter_count * sizeof(*out->accesses));
        type_at += count; access_at += source->parameter_count;
    }
    if (!complete_type_shapes(b)) {
        if (b->outcome == SOL_MIR_MATERIALIZE_BUILD_INTERNAL_FAILED) return fail(b,
            SOL_MIR_MATERIALIZE_BUILD_UNSUPPORTED_OR_UNRESOLVED,
            "concrete nominal shape could not be substituted");
        return false;
    }
    out->shape_field_count = out->shape_field_capacity;
    out->shape_variant_count = out->shape_variant_capacity;
    if (plan->context_count != 0) memcpy(out->contexts, plan->contexts,
        plan->context_count * sizeof(*out->contexts));
    if (!classify_copy(b)) return false;
    for (size_t i = 0; i < plan->import_count; ++i) {
        const SolMirPlanImport *source = &plan->imports[i];
        SolMirMaterializedImport *target = &out->imports[i];
        *target = (SolMirMaterializedImport){source->callable, source->receiver,
            source->receiver == SOL_MIR_PLAN_NONE ? SOL_ACCESS_OWNED : SOL_ACCESS_SHARED,
            {type_at, source->parameter_types.count},
            {access_at, source->parameter_accesses.count}, source->result,
            source->effects, i, {0, 0}, source->contexts};
        if (source->parameter_types.count != 0) memcpy(out->type_ids + type_at,
            plan->instance_type_ids + source->parameter_types.offset,
            source->parameter_types.count * sizeof(*out->type_ids));
        if (source->parameter_accesses.count != 0) memcpy(out->accesses + access_at,
            plan->instance_accesses + source->parameter_accesses.offset,
            source->parameter_accesses.count * sizeof(*out->accesses));
        type_at += source->parameter_types.count;
        access_at += source->parameter_accesses.count;
    }
    out->import_count = out->import_capacity;
    size_t binding_at = 0;
    for (size_t parent = 0; parent < plan->instance_count; ++parent) {
        out->images[parent].bindings = (SolMirPlanSlice){binding_at, 0};
        for (size_t demand = 0; demand < plan->demand_count; ++demand) {
            const SolMirPlanDemand *source = &plan->demands[demand];
            if (source->owner_kind != SOL_MIR_PLAN_DEMAND_OWNER_INSTANCE
                || source->parent != parent) continue;
            out->bindings[binding_at] = (SolMirMaterializedBinding){
                .source_demand = demand, .kind = source->kind,
                .owner_kind = source->owner_kind, .parent = source->parent,
                .parent_import = source->parent_import,
                .context = source->context, .source = source->source,
                .symbolic_callable = source->symbolic_target,
                .dispatch_trait = source->dispatch_trait,
                .dispatch_requirement = source->dispatch_requirement,
                .target_kind = source->instance != SOL_MIR_PLAN_NONE
                    ? SOL_MIR_MATERIALIZED_TARGET_INSTANCE
                    : SOL_MIR_MATERIALIZED_TARGET_IMPORT,
                .instance = source->instance, .import = source->import,
                .site = binding_at};
            ++binding_at;
            ++out->images[parent].bindings.count;
        }
    }
    for (size_t demand = 0; demand < plan->demand_count; ++demand) {
        const SolMirPlanDemand *source = &plan->demands[demand];
        if (source->owner_kind == SOL_MIR_PLAN_DEMAND_OWNER_INSTANCE) continue;
        out->bindings[binding_at] = (SolMirMaterializedBinding){
            .source_demand = demand, .kind = source->kind,
            .owner_kind = source->owner_kind, .parent = source->parent,
            .parent_import = source->parent_import,
            .context = source->context, .source = source->source,
            .symbolic_callable = source->symbolic_target,
            .dispatch_trait = source->dispatch_trait,
            .dispatch_requirement = source->dispatch_requirement,
            .target_kind = source->instance != SOL_MIR_PLAN_NONE
                ? SOL_MIR_MATERIALIZED_TARGET_INSTANCE
                : SOL_MIR_MATERIALIZED_TARGET_IMPORT,
            .instance = source->instance, .import = source->import,
            .site = binding_at};
        ++binding_at;
    }
    if (binding_at != plan->demand_count) return false;
    out->binding_count = out->binding_capacity;
    for (size_t id = 0; id < plan->instance_count; ++id) {
        const SolMirPlanInstance *instance = &plan->instances[id];
        const SolMirProgramTemplate *tpl = find_template(plan, instance->callable);
        const SolMir *mir = &tpl->mir;
        SolMirMaterializedImage *image = &out->images[id];
        image->instance = id; image->source_callable = instance->callable;
        image->receiver = instance->receiver; image->result = instance->result;
        image->receiver_access
            = plan->program->ir->callables[instance->callable].receiver_access;
        image->effects = instance->effects; image->contexts = instance->contexts;
        image->type_arguments = (SolMirPlanSlice){type_at, instance->type_arguments.count};
        if (instance->type_arguments.count != 0) memcpy(out->type_ids + type_at,
            plan->instance_type_ids + instance->type_arguments.offset,
            instance->type_arguments.count * sizeof(*out->type_ids));
        type_at += instance->type_arguments.count;
        image->parameter_types = (SolMirPlanSlice){type_at, instance->parameter_types.count};
        if (instance->parameter_types.count != 0) memcpy(out->type_ids + type_at,
            plan->instance_type_ids + instance->parameter_types.offset,
            instance->parameter_types.count * sizeof(*out->type_ids));
        type_at += instance->parameter_types.count;
        image->parameter_accesses = (SolMirPlanSlice){access_at,
            instance->parameter_accesses.count};
        if (instance->parameter_accesses.count != 0) memcpy(out->accesses + access_at,
            plan->instance_accesses + instance->parameter_accesses.offset,
            instance->parameter_accesses.count * sizeof(*out->accesses));
        access_at += instance->parameter_accesses.count;
        image->overlays.offset = out->overlay_count;
        for (size_t u = 0; u < instance->typed_uses.count; ++u) {
            const SolMirPlanTypedUse *source = &plan->typed_uses[instance->typed_uses.offset + u];
            out->overlays[out->overlay_count++] = (SolMirMaterializedTypeOverlay){
                source->kind, source->source, source->ordinal, source->context,
                source->type, source->access};
        }
        image->overlays.count = out->overlay_count - image->overlays.offset;
        image->locals.offset = out->local_count;
        const SolIrCallable *callable = &plan->program->ir->callables[instance->callable];
        for (size_t u = 0; u < instance->typed_uses.count; ++u) {
            const SolMirPlanTypedUse *use = &plan->typed_uses[instance->typed_uses.offset + u];
            if (use->context != instance->contexts.offset
                || use->kind != SOL_MIR_PLAN_USE_LOCAL) continue;
            SolMirMaterializedLocalKind kind = SOL_MIR_MATERIALIZED_LOCAL_BODY;
            size_t ordinal = SOL_MIR_MATERIALIZED_NONE;
            if (use->source == callable->receiver) {
                kind = SOL_MIR_MATERIALIZED_LOCAL_RECEIVER; ordinal = 0;
            }
            for (size_t p = 0; p < callable->parameters.count; ++p) {
                if (plan->program->ir->roots[callable->parameters.offset + p]
                    == use->source) { kind = SOL_MIR_MATERIALIZED_LOCAL_PARAMETER; ordinal = p; }
            }
            out->locals[out->local_count++] = (SolMirMaterializedLocal){id,
                use->source, use->type, use->access, kind, ordinal};
        }
        image->locals.count = out->local_count - image->locals.offset;
        image->places.offset = out->place_count;
        for (size_t u = 0; u < instance->typed_uses.count; ++u) {
            const SolMirPlanTypedUse *use = &plan->typed_uses[instance->typed_uses.offset + u];
            if (use->context != instance->contexts.offset
                || use->kind != SOL_MIR_PLAN_USE_PLACE_ROOT) continue;
            const SolIrPlace *source = &plan->program->ir->places[use->source];
            const SolMirPlanTypedUse *final = find_use(plan, instance,
                SOL_MIR_PLAN_USE_PLACE_FINAL, use->source, 0, instance->contexts.offset);
            SolMirMaterializedPlace *place = &out->places[out->place_count++];
            if (final == NULL) return false;
            *place = (SolMirMaterializedPlace){id, use->source,
                local_for(out, image, source->local), use->type,
                {out->projection_count, source->projections.count}, final->type};
            for (size_t p = 0; p < source->projections.count; ++p) {
                size_t source_id = source->projections.offset + p;
                const SolIrProjection *projection = &plan->program->ir->projections[source_id];
                const SolMirPlanTypedUse *type = find_use(plan, instance,
                    SOL_MIR_PLAN_USE_PLACE_PROJECTION, use->source, p,
                    instance->contexts.offset);
                if (type == NULL) return false;
                out->projections[out->projection_count++] = (SolMirMaterializedProjection){
                    projection->kind, type->type, projection->field,
                    projection->ordinal, source_id};
            }
        }
        for (size_t n = 0; n < mir->instruction_count; ++n) {
            SolMirPlace source;
            if (!instruction_place(&mir->instructions[n], &source)
                || source.source_place != SOL_IR_NONE
                || place_for(out, image, source) != SOL_MIR_MATERIALIZED_NONE) continue;
            const SolMirPlanTypedUse *type = find_use(plan, instance,
                SOL_MIR_PLAN_USE_LOCAL, source.local, 0, instance->contexts.offset);
            if (type == NULL) return false;
            SolMirMaterializedLocalId local = local_for(out, image, source.local);
            out->places[out->place_count++] = (SolMirMaterializedPlace){id,
                SOL_IR_NONE, local, type->type, {out->projection_count, 0}, type->type};
        }
        image->places.count = out->place_count - image->places.offset;
        image->blocks = (SolMirPlanSlice){out->block_count, mir->block_count};
        image->loops = (SolMirPlanSlice){out->loop_count, mir->loop_count};
        image->entry = image->blocks.offset + mir->entry;
        image->contract_body = mir->contract_body == SOL_MIR_NONE
            ? SOL_MIR_MATERIALIZED_NONE : image->blocks.offset + mir->contract_body;
        image->contract_epilogue = mir->contract_epilogue == SOL_MIR_NONE
            ? SOL_MIR_MATERIALIZED_NONE : image->blocks.offset + mir->contract_epilogue;
        image->values = (SolMirPlanSlice){out->value_count, mir->value_count};
        image->instructions = (SolMirPlanSlice){out->instruction_count, mir->instruction_count};
        image->temporaries = (SolMirPlanSlice){out->temporary_count, mir->temporary_count};
        image->construct_operands = (SolMirPlanSlice){out->construct_operand_count,
            mir->construct_operand_count};
        image->call_arguments = (SolMirPlanSlice){out->call_argument_count,
            mir->call_argument_count};
        for (size_t v = 0; v < mir->value_count; ++v) {
            const SolMirValue *source = &mir->values[v];
            const SolMirPlanTypedUse *type = find_use(plan, instance,
                SOL_MIR_PLAN_USE_MIR_VALUE, v, 0, instance->contexts.offset);
            if (type == NULL) return false;
            out->values[out->value_count++] = (SolMirMaterializedValue){source->kind,
                type->type, image->blocks.offset + source->block, source->definition,
                source->kind == SOL_MIR_VALUE_INSTRUCTION
                    ? image->instructions.offset + source->definition
                    : SOL_MIR_MATERIALIZED_NONE,
                source->source_expression, source->span};
        }
        for (size_t t = 0; t < mir->temporary_count; ++t) {
            const SolMirPlanTypedUse *type = find_use(plan, instance,
                SOL_MIR_PLAN_USE_MIR_TEMPORARY, t, 0, instance->contexts.offset);
            if (type == NULL) return false;
            out->temporaries[out->temporary_count++] = (SolMirMaterializedTemporary){
                type->type, mir->temporaries[t].source_expression, mir->temporaries[t].span};
        }
        for (size_t o = 0; o < mir->construct_operand_count; ++o) {
            const SolMirConstructOperand *source = &mir->construct_operands[o];
            const SolMirPlanTypedUse *type = find_use(plan, instance,
                SOL_MIR_PLAN_USE_CONSTRUCT_OPERAND, o, source->formal,
                instance->contexts.offset);
            if (type == NULL) return false;
            out->construct_operands[out->construct_operand_count++]
                = (SolMirMaterializedConstructOperand){source->formal,
                    source->source_expression, type->type,
                    image->temporaries.offset + source->temporary};
        }
        for (size_t a = 0; a < mir->call_argument_count; ++a) {
            if (!translate_argument(out, image, instance, &mir->call_arguments[a],
                    &out->call_arguments[out->call_argument_count++])) return false;
        }
        image->handlers.offset = out->handler_count;
        for (size_t n = 0; n < mir->instruction_count; ++n) {
            const SolMirInstruction *instruction = &mir->instructions[n];
            if (instruction->kind != SOL_MIR_INST_HANDLER_ENTER) continue;
            SolIrExpressionId expression_id = instruction->source_expression;
            const SolIrExpression *expression = &plan->program->ir->expressions[expression_id];
            SolMirProgramSource provenance = {instance->callable, expression_id, 0,
                instruction->span.start, instruction->span.end};
            bool found_file = false;
            for (size_t f = 0; f < plan->program->ir->file_count; ++f) {
                const SolIrSourceFile *file = &plan->program->ir->files[f];
                if (instruction->span.start >= file->aggregate_start
                    && instruction->span.end <= file->aggregate_end) {
                    provenance.file = f; provenance.start -= file->aggregate_start;
                    provenance.end -= file->aggregate_start; found_file = true; break;
                }
            }
            size_t source_demand = 0, provider_demand = 0;
            if (!found_file || find_demand(plan, id, instance->contexts.offset,
                    provenance, SOL_MIR_PLAN_DEMAND_HANDLER_SOURCE,
                    SOL_MIR_PLAN_DEMAND_HANDLER_SOURCE, &source_demand) == NULL
                || find_demand(plan, id, instance->contexts.offset, provenance,
                    SOL_MIR_PLAN_DEMAND_HANDLER_PROVIDER,
                    SOL_MIR_PLAN_DEMAND_HANDLER_PROVIDER, &provider_demand) == NULL) return false;
            size_t source_binding = binding_for_demand(out, source_demand);
            size_t provider_binding = binding_for_demand(out, provider_demand);
            if (source_binding == SOL_MIR_MATERIALIZED_NONE
                || provider_binding == SOL_MIR_MATERIALIZED_NONE) return false;
            const SolIrExpression *authority = &plan->program->ir->expressions[
                expression->as.handler.authority];
            const SolIrExpression *provider = &plan->program->ir->expressions[
                expression->as.handler.provider];
            size_t effect = SOL_IR_NONE;
            for (size_t e = 0; e < plan->program->ir->effect_count; ++e) {
                if (strcmp(plan->program->ir->effects[e].name,
                        expression->as.handler.effect_name) == 0) { effect = e; break; }
            }
            SolMirMaterializedHandler *handler = &out->handlers[out->handler_count++];
            *handler = (SolMirMaterializedHandler){id,
                instance->contexts.offset, expression_id, source_binding,
                provider_binding, place_for(out, image, (SolMirPlace){
                    plan->program->ir->places[authority->as.place].local,
                    authority->as.place}), place_for(out, image, (SolMirPlace){
                    plan->program->ir->places[provider->as.place].local,
                    provider->as.place}), expression->as.handler.source, effect,
                expression->as.handler.root, {0}, instruction->span};
            SolMirMaterializedTypeId operation_receiver, operation_result;
            SolMirPlanSlice operation_parameters, operation_accesses;
            binding_signature(out, &out->bindings[source_binding],
                &operation_receiver, &operation_parameters, &operation_accesses,
                &operation_result);
            const SolMirMaterializedBinding *operation_binding
                = &out->bindings[source_binding];
            handler->operation = (SolMirMaterializedOperationKey){
                operation_binding->target_kind, operation_binding->instance,
                operation_binding->import, operation_receiver, handler->authority,
                operation_binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE
                    ? out->plan->instances[operation_binding->instance].effects
                    : out->imports[operation_binding->import].effects};
            (void)operation_parameters; (void)operation_accesses;
            (void)operation_result;
            if (out->bindings[provider_binding].symbolic_callable
                    != expression->as.handler.provider_callable
                || !handler_signatures_match(out, handler)) return false;
        }
        image->handlers.count = out->handler_count - image->handlers.offset;
        for (size_t n = 0; n < mir->instruction_count; ++n) {
            if (!translate_instruction(out, image, instance, &mir->instructions[n], n,
                    &out->instructions[out->instruction_count++])) return false;
        }
        for (size_t l = 0; l < mir->loop_count; ++l) {
            const SolMirLoop *source = &mir->loops[l];
#define BLOCK(v) (image->blocks.offset + (v))
            out->loops[out->loop_count++] = (SolMirMaterializedLoop){
                source->statement, source->parent == SOL_MIR_NONE
                    ? SOL_MIR_MATERIALIZED_NONE : image->loops.offset + source->parent,
                BLOCK(source->preheader), BLOCK(source->header), BLOCK(source->condition),
                BLOCK(source->body), BLOCK(source->backedge), BLOCK(source->exit),
                source->obligations, source->span, l};
#undef BLOCK
        }
        for (size_t p = 0; p < mir->parameter_value_count; ++p) {
            out->parameter_values[out->parameter_value_count++] = image->values.offset
                + mir->parameter_values[p];
        }
        for (size_t k = 0; k < mir->block_count; ++k) {
            const SolMirBlock *source = &mir->blocks[k];
            SolMirMaterializedBlock *block = &out->blocks[out->block_count++];
            block->id = image->blocks.offset + source->id; block->order = source->order;
            block->parameters = (SolMirPlanSlice){out->parameter_value_count
                - mir->parameter_value_count + source->parameters.offset,
                source->parameters.count};
            block->instructions = (SolMirPlanSlice){image->instructions.offset
                + source->instructions.offset, source->instructions.count};
            block->span = source->span; block->started = source->started;
            block->source_block = k;
            if (!translate_terminator(b, id, instance, image, mir,
                    &source->terminator, &block->terminator)) return fail(b,
                SOL_MIR_MATERIALIZE_BUILD_UNSUPPORTED_OR_UNRESOLVED,
                "concrete terminator dispatch or signature is unresolved");
        }
        if (!clone_mir(b, mir, &image->topology)) return false;
    }
    for (size_t i = 0; i < plan->import_count; ++i) {
        const SolMirPlanImport *source = &plan->imports[i];
        SolMirMaterializedImport *target = &out->imports[i];
        target->overlays.offset = out->overlay_count;
        for (size_t u = 0; u < source->typed_uses.count; ++u) {
            const SolMirPlanTypedUse *use
                = &plan->typed_uses[source->typed_uses.offset + u];
            out->overlays[out->overlay_count++] = (SolMirMaterializedTypeOverlay){
                use->kind, use->source, use->ordinal, use->context,
                use->type, use->access};
        }
        target->overlays.count = out->overlay_count - target->overlays.offset;
    }
    size_t receiver_root_at = 0;
    for (size_t id = 0; id < out->binding_count; ++id) {
        const SolMirMaterializedBinding *binding = &out->bindings[id];
        SolMirMaterializedSemanticSite *site = &out->semantic_sites[id];
        *site = (SolMirMaterializedSemanticSite){
            .kind = binding->kind,
            .binding = id,
            .owner_kind = binding->owner_kind,
            .parent = binding->parent,
            .parent_import = binding->parent_import,
            .context = binding->context,
            .source = binding->source,
            .source_definition = SOL_IR_NONE,
            .source_obligation = SOL_IR_NONE,
            .producer_kind = SOL_MIR_MATERIALIZED_PRODUCER_ROOT,
            .block = SOL_MIR_MATERIALIZED_NONE,
            .instruction = SOL_MIR_MATERIALIZED_NONE,
            .handler = SOL_MIR_MATERIALIZED_NONE,
            .produced_function_type = SOL_MIR_MATERIALIZED_NONE,
            .captured_receiver_type = SOL_MIR_MATERIALIZED_NONE,
            .captured_receiver_kind = SOL_MIR_MATERIALIZED_RECEIVER_NONE,
            .captured_receiver_expression = SOL_IR_NONE,
            .captured_receiver_place = SOL_MIR_MATERIALIZED_NONE,
            .captured_receiver_temporary = SOL_MIR_MATERIALIZED_NONE,
            .captured_receiver_value = SOL_MIR_MATERIALIZED_NONE,
            .captured_receiver_instruction = SOL_MIR_MATERIALIZED_NONE,
        };
        if (binding->kind == SOL_MIR_PLAN_DEMAND_ROOT) {
            if (binding->target_kind != SOL_MIR_MATERIALIZED_TARGET_INSTANCE
                || binding->instance >= out->image_count) return false;
            site->block = out->images[binding->instance].entry;
            continue;
        }
        if (binding->owner_kind == SOL_MIR_PLAN_DEMAND_OWNER_IMPORT) {
            if (binding->parent != SOL_MIR_PLAN_NONE
                || binding->parent_import >= out->import_count
                || binding->context >= out->context_count
                || out->contexts[binding->context].target_kind
                    != SOL_MIR_PLAN_TARGET_IMPORT
                || out->contexts[binding->context].import
                    != binding->parent_import) return false;
            site->source_definition = out->contexts[binding->context].definition;
            site->source_obligation = out->contexts[binding->context].obligation;
            site->producer_kind = SOL_MIR_MATERIALIZED_PRODUCER_PREDICATE;
            continue;
        }
        if (binding->parent >= out->image_count) return false;
        const SolMirMaterializedImage *image = &out->images[binding->parent];
        if (binding->kind == SOL_MIR_PLAN_DEMAND_FUNCTION_VALUE
            || binding->kind == SOL_MIR_PLAN_DEMAND_BOUND_OPERATION
            || binding->kind == SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE) {
            site->produced_function_type = expression_type_for_site(out, image,
                binding->context, binding->source.expression);
            if (site->produced_function_type >= out->type_count
                || out->types[site->produced_function_type].kind
                    != SOL_IR_TYPE_FUNCTION) return false;
            if (binding->kind == SOL_MIR_PLAN_DEMAND_BOUND_OPERATION) {
                const SolIr *ir = plan->program->ir;
                if (binding->source.expression >= ir->expression_count) return false;
                const SolIrExpression *operation
                    = &ir->expressions[binding->source.expression];
                if (operation->kind != SOL_IR_EXPR_BOUND_OPERATION
                    || operation->as.operation.receiver >= ir->expression_count)
                    return false;
                const SolIrExpression *receiver
                    = &ir->expressions[operation->as.operation.receiver];
                site->captured_receiver_type = expression_type_for_site(out, image,
                    binding->context, operation->as.operation.receiver);
                site->captured_receiver_expression
                    = operation->as.operation.receiver;
                site->captured_receiver_roots = (SolMirPlanSlice){
                    receiver_root_at, receiver->capability_roots.count};
                for (size_t r = 0; r < receiver->capability_roots.count; ++r) {
                    SolIrLocalId source_root = ir->roots[
                        receiver->capability_roots.offset + r];
                    SolMirMaterializedLocalId concrete_root
                        = local_for(out, image, source_root);
                    if (concrete_root == SOL_MIR_MATERIALIZED_NONE) return false;
                    out->receiver_roots[receiver_root_at++] = concrete_root;
                }
                SolMirMaterializedPlaceId root = SOL_MIR_MATERIALIZED_NONE;
                if (receiver->kind == SOL_IR_EXPR_PLACE
                    && receiver->as.place < ir->place_count) {
                    const SolIrPlace *place = &ir->places[receiver->as.place];
                    if (place->root_kind == SOL_IR_PLACE_ROOT_LOCAL)
                        root = place_for(out, image,
                            (SolMirPlace){place->local, receiver->as.place});
                    if (root == SOL_MIR_MATERIALIZED_NONE
                        && place->root_kind == SOL_IR_PLACE_ROOT_LOCAL) {
                        SolMirMaterializedLocalId local
                            = local_for(out, image, place->local);
                        for (size_t p = 0; p < image->places.count; ++p) {
                            size_t candidate = image->places.offset + p;
                            if (out->places[candidate].local == local
                                && out->places[candidate].projections.count == 0) {
                                root = candidate; break;
                            }
                        }
                    }
                }
                if (root != SOL_MIR_MATERIALIZED_NONE) {
                    site->captured_receiver_kind
                        = SOL_MIR_MATERIALIZED_RECEIVER_PLACE;
                    site->captured_receiver_place = root;
                } else {
                    for (size_t t = 0; t < image->temporaries.count; ++t) {
                        size_t temporary = image->temporaries.offset + t;
                        if (out->temporaries[temporary].source_expression
                                == operation->as.operation.receiver) {
                            site->captured_receiver_kind
                                = SOL_MIR_MATERIALIZED_RECEIVER_TEMPORARY;
                            site->captured_receiver_temporary = temporary;
                            break;
                        }
                    }
                    for (size_t v = 0; site->captured_receiver_kind
                            == SOL_MIR_MATERIALIZED_RECEIVER_NONE
                        && v < image->values.count; ++v) {
                        size_t value = image->values.offset + v;
                        if (out->values[value].source_expression
                                == operation->as.operation.receiver) {
                            site->captured_receiver_kind
                                = SOL_MIR_MATERIALIZED_RECEIVER_VALUE;
                            site->captured_receiver_value = value;
                            site->captured_receiver_instruction
                                = out->values[value].instruction;
                        }
                    }
                    if (site->captured_receiver_kind
                        == SOL_MIR_MATERIALIZED_RECEIVER_NONE) {
                        site->captured_receiver_kind
                            = SOL_MIR_MATERIALIZED_RECEIVER_SOURCE_EXPRESSION;
                    }
                }
                if (site->captured_receiver_type >= out->type_count
                    || receiver->capability_roots.count == 0) return false;
                site->operation = (SolMirMaterializedOperationKey){
                    binding->target_kind, binding->instance, binding->import,
                    site->captured_receiver_type, root,
                    out->types[site->produced_function_type].effects};
            }
        }
        if (binding->context < out->context_count
            && (out->contexts[binding->context].kind
                    == SOL_MIR_PLAN_CONTEXT_REFINEMENT
                || out->contexts[binding->context].kind
                    == SOL_MIR_PLAN_CONTEXT_CONTRACT)) {
            site->source_definition = out->contexts[binding->context].definition;
            site->source_obligation = out->contexts[binding->context].obligation;
        }
        if (binding->kind == SOL_MIR_PLAN_DEMAND_INVOKE
            || binding->kind == SOL_MIR_PLAN_DEMAND_CALLBACK) {
            for (size_t block = 0; block < image->blocks.count; ++block) {
                size_t block_id = image->blocks.offset + block;
                if (out->blocks[block_id].terminator.kind == SOL_MIR_TERM_INVOKE
                    && out->blocks[block_id].terminator.binding == id) {
                    if (site->block != SOL_MIR_MATERIALIZED_NONE) return false;
                    site->block = block_id;
                    site->producer_kind = SOL_MIR_MATERIALIZED_PRODUCER_TERMINATOR;
                }
            }
            if (site->block == SOL_MIR_MATERIALIZED_NONE
                && (binding->kind != SOL_MIR_PLAN_DEMAND_CALLBACK
                    || site->source_obligation == SOL_IR_NONE)) return false;
            if (site->block == SOL_MIR_MATERIALIZED_NONE) {
                for (size_t block = 0; block < image->blocks.count; ++block) {
                    size_t block_id = image->blocks.offset + block;
                    const SolMirMaterializedTerminator *term
                        = &out->blocks[block_id].terminator;
                    bool matches = term->kind == SOL_MIR_TERM_CHECK_CONTRACT
                        ? term->source_obligation == site->source_obligation
                        : term->kind == SOL_MIR_TERM_CHECK_REFINED
                            && binding->context < out->context_count
                            && out->contexts[binding->context].kind
                                == SOL_MIR_PLAN_CONTEXT_REFINEMENT
                            && term->source_expression
                                == out->contexts[binding->context].source.expression;
                    if (matches) {
                        if (site->block != SOL_MIR_MATERIALIZED_NONE) return false;
                        site->block = block_id;
                        site->producer_kind
                            = SOL_MIR_MATERIALIZED_PRODUCER_PREDICATE;
                    }
                }
                if (site->block == SOL_MIR_MATERIALIZED_NONE) return false;
            }
        } else if (binding->kind == SOL_MIR_PLAN_DEMAND_HANDLER_SOURCE
            || binding->kind == SOL_MIR_PLAN_DEMAND_HANDLER_PROVIDER) {
            for (size_t h = 0; h < image->handlers.count; ++h) {
                size_t handler_id = image->handlers.offset + h;
                const SolMirMaterializedHandler *handler = &out->handlers[handler_id];
                if ((binding->kind == SOL_MIR_PLAN_DEMAND_HANDLER_SOURCE
                        && handler->source_binding == id)
                    || (binding->kind == SOL_MIR_PLAN_DEMAND_HANDLER_PROVIDER
                        && handler->provider_binding == id)) {
                    if (site->handler != SOL_MIR_MATERIALIZED_NONE) return false;
                    site->handler = handler_id;
                    site->producer_kind = SOL_MIR_MATERIALIZED_PRODUCER_HANDLER;
                }
            }
            if (site->handler == SOL_MIR_MATERIALIZED_NONE) return false;
        } else {
            if (site->source_obligation != SOL_IR_NONE) {
                for (size_t block = 0; block < image->blocks.count; ++block) {
                    size_t block_id = image->blocks.offset + block;
                    const SolMirMaterializedTerminator *term
                        = &out->blocks[block_id].terminator;
                    bool matches = term->kind == SOL_MIR_TERM_CHECK_CONTRACT
                        ? term->source_obligation == site->source_obligation
                        : term->kind == SOL_MIR_TERM_CHECK_REFINED
                            && binding->context < out->context_count
                            && out->contexts[binding->context].kind
                                == SOL_MIR_PLAN_CONTEXT_REFINEMENT
                            && term->source_expression
                                == out->contexts[binding->context].source.expression;
                    if (matches) {
                        if (site->block != SOL_MIR_MATERIALIZED_NONE) return false;
                        site->block = block_id;
                        site->producer_kind
                            = SOL_MIR_MATERIALIZED_PRODUCER_PREDICATE;
                    }
                }
            }
            for (size_t n = 0; site->source_obligation == SOL_IR_NONE
                && binding->kind != SOL_MIR_PLAN_DEMAND_FUNCTION_VALUE
                && binding->kind != SOL_MIR_PLAN_DEMAND_BOUND_OPERATION
                && n < image->instructions.count; ++n) {
                size_t instruction_id = image->instructions.offset + n;
                const SolMirMaterializedInstruction *instruction
                    = &out->instructions[instruction_id];
                if (instruction->source_expression == binding->source.expression) {
                    if (site->block == SOL_MIR_MATERIALIZED_NONE) {
                        site->instruction = instruction_id;
                        site->block = instruction->block;
                        site->producer_kind
                            = SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION;
                    }
                }
            }
            if (site->block == SOL_MIR_MATERIALIZED_NONE) {
                for (size_t block = 0; block < image->blocks.count; ++block) {
                    size_t block_id = image->blocks.offset + block;
                    const SolMirMaterializedTerminator *term
                        = &out->blocks[block_id].terminator;
                    bool matches = binding->kind == SOL_MIR_PLAN_DEMAND_FUNCTION_VALUE
                        && term->kind == SOL_MIR_TERM_INVOKE
                        && term->call_kind == SOL_IR_CALL_CALLBACK
                        && term->callee < out->temporary_count
                        && out->temporaries[term->callee].source_expression
                            == binding->source.expression;
                    if (!matches && binding->kind
                            == SOL_MIR_PLAN_DEMAND_BOUND_OPERATION
                        && term->kind == SOL_MIR_TERM_INVOKE
                        && term->call_kind == SOL_IR_CALL_CAPABILITY
                        && term->source_expression
                            < plan->program->ir->expression_count) {
                        const SolIrExpression *call = &plan->program->ir->expressions[
                            term->source_expression];
                        matches = call->kind == SOL_IR_EXPR_CALL
                            && call->as.call.callee == binding->source.expression;
                    }
                    if (matches) {
                        if (site->block != SOL_MIR_MATERIALIZED_NONE) return false;
                        site->block = block_id;
                        site->producer_kind
                            = SOL_MIR_MATERIALIZED_PRODUCER_TERMINATOR;
                    }
                }
            }
            if (site->block == SOL_MIR_MATERIALIZED_NONE) return false;
        }
    }
    for (size_t block = 0; block < out->block_count; ++block) {
        SolMirMaterializedTerminator *term = &out->blocks[block].terminator;
        if (term->kind != SOL_MIR_TERM_CHECK_REFINED
            && term->kind != SOL_MIR_TERM_CHECK_CONTRACT) continue;
        size_t image_id = 0;
        while (image_id < out->image_count
            && !(block >= out->images[image_id].blocks.offset
                && block - out->images[image_id].blocks.offset
                    < out->images[image_id].blocks.count)) ++image_id;
        if (image_id == out->image_count) return false;
        size_t found = 0;
        for (size_t site_id = 0; site_id < out->binding_count; ++site_id) {
            const SolMirMaterializedSemanticSite *site = &out->semantic_sites[site_id];
            if (site->parent != image_id
                || (site->kind != SOL_MIR_PLAN_DEMAND_PREDICATE
                    && site->kind != SOL_MIR_PLAN_DEMAND_CALLBACK)
                || (term->kind == SOL_MIR_TERM_CHECK_REFINED
                    ? site->context >= out->context_count
                        || out->contexts[site->context].kind
                            != SOL_MIR_PLAN_CONTEXT_REFINEMENT
                        || out->contexts[site->context].source.expression
                            != term->source_expression
                    : site->source_obligation != term->source_obligation)) continue;
            ++found;
        }
        term->predicate_inline = found == 0;
    }
    for (size_t site_id = 0; site_id < out->binding_count; ++site_id) {
        SolMirMaterializedSemanticSite *site = &out->semantic_sites[site_id];
        if (site->producer_kind != SOL_MIR_MATERIALIZED_PRODUCER_TERMINATOR
            || (site->kind != SOL_MIR_PLAN_DEMAND_FUNCTION_VALUE
                && site->kind != SOL_MIR_PLAN_DEMAND_BOUND_OPERATION)) continue;
        SolMirMaterializedTerminator *term = &out->blocks[site->block].terminator;
        if (term->callable_site != SOL_MIR_MATERIALIZED_NONE) return false;
        term->callable_site = site_id;
        if (site->kind == SOL_MIR_PLAN_DEMAND_BOUND_OPERATION) {
            const SolMirMaterializedBinding *binding = &out->bindings[site->binding];
            site->operation = (SolMirMaterializedOperationKey){binding->target_kind,
                binding->instance, binding->import, term->receiver.type,
                term->receiver.place, term->effects};
            site->captured_receiver_type = term->receiver.type;
            if (term->receiver.place != SOL_MIR_MATERIALIZED_NONE) {
                site->captured_receiver_kind = SOL_MIR_MATERIALIZED_RECEIVER_PLACE;
                site->captured_receiver_place = term->receiver.place;
                site->captured_receiver_temporary = SOL_MIR_MATERIALIZED_NONE;
            } else if (term->receiver.temporary != SOL_MIR_MATERIALIZED_NONE) {
                site->captured_receiver_kind
                    = SOL_MIR_MATERIALIZED_RECEIVER_TEMPORARY;
                site->captured_receiver_temporary = term->receiver.temporary;
                site->captured_receiver_place = SOL_MIR_MATERIALIZED_NONE;
            }
        }
    }
    out->type_id_count = out->type_id_capacity; out->access_count = out->access_capacity;
    out->overlay_count = out->overlay_capacity; out->local_count = out->local_capacity;
    out->place_count = out->place_capacity; out->projection_count = out->projection_capacity;
    out->value_count = out->value_capacity; out->instruction_count = out->instruction_capacity;
    out->temporary_count = out->temporary_capacity;
    out->construct_operand_count = out->construct_operand_capacity;
    out->call_argument_count = out->call_argument_capacity;
    out->block_count = out->block_capacity; out->edge_count = out->edge_capacity;
    out->edge_value_count = out->edge_value_capacity;
    out->parameter_value_count = out->parameter_value_capacity;
    out->loop_count = out->loop_capacity; out->handler_count = out->handler_capacity;
    out->semantic_site_count = out->semantic_site_capacity;
    if (receiver_root_at != out->receiver_root_capacity) return false;
    out->receiver_root_count = out->receiver_root_capacity;
    out->writeback_count = out->writeback_capacity;
    out->literal_byte_count = out->literal_byte_capacity;
    out->usage.instances = out->image_count; out->usage.cfg_items = cfg;
    out->usage.bindings = bindings; out->usage.concrete_records = concrete;
    if (!charge(b, concrete)) return false;
    if (!sol_mir_materialization_validation_work(out,
            &out->usage.validation_work)
        || out->usage.validation_work > out->limits.max_validation_work) {
        return fail(b, SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
            "materialized MIR validation work limit exceeded");
    }
    return true;
}

SolMirMaterializeBuildOutcome sol_mir_materialize_build(
    const SolMirMaterializeBuildRequest *request, SolMirMaterialization *output,
    SolDiagnostics *diagnostics) {
    if (request == NULL || output == NULL || diagnostics == NULL
        || !owner_empty(output) || request->plan == NULL
        || (request->limits != NULL && !limits_zero(*request->limits)
            && !limits_complete(*request->limits))) {
        if (diagnostics != NULL) sol_diagnostics_add(diagnostics,
            "SOL-MIR-MATERIALIZE-001", SOL_SEVERITY_ERROR, (SolSpan){0},
            "invalid MIR materialization request or destination");
        return SOL_MIR_MATERIALIZE_BUILD_INVALID_ARGUMENT;
    }
    if (!sol_mir_plan_validate(request->plan, diagnostics)) {
        return diagnostics->allocation_failed ? SOL_MIR_MATERIALIZE_BUILD_ALLOCATION_FAILED
            : SOL_MIR_MATERIALIZE_BUILD_INVALID_PLAN;
    }
    SolMirMaterialization scratch;
    sol_mir_materialization_init(&scratch);
    scratch.plan = request->plan;
    scratch.limits = request->limits == NULL || limits_zero(*request->limits)
        ? sol_mir_materialize_default_limits() : *request->limits;
    Builder builder = {&scratch, diagnostics, SOL_MIR_MATERIALIZE_BUILD_INTERNAL_FAILED};
    if (build_scratch(&builder)) {
        builder.outcome = SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED;
        if (sol_mir_materialization_validate(&scratch, diagnostics)) {
            *output = scratch; return builder.outcome;
        }
        builder.outcome = diagnostics->allocation_failed
            ? SOL_MIR_MATERIALIZE_BUILD_ALLOCATION_FAILED
            : SOL_MIR_MATERIALIZE_BUILD_INTERNAL_FAILED;
    }
    sol_mir_materialization_free(&scratch);
    return builder.outcome;
}

static bool canonical(size_t count, size_t capacity, const void *pointer) {
    return count == capacity && ((count == 0) == (pointer == NULL));
}

static bool add_range(Range *ranges, size_t *count, const void *pointer,
    size_t items, size_t size) {
    if (items == 0) return pointer == NULL;
    if (pointer == NULL || size == 0 || items > SIZE_MAX / size) return false;
    uintptr_t start = (uintptr_t)pointer;
    size_t bytes = items * size;
    if (bytes > UINTPTR_MAX - start) return false;
    Range next = {start, start + bytes};
    for (size_t i = 0; i < *count; ++i) {
        if (next.start < ranges[i].end && ranges[i].start < next.end) return false;
    }
    ranges[(*count)++] = next; return true;
}

static bool topology_equal(const SolMir *a, const SolMir *b) {
    if (a->callable != b->callable || a->generic_parameters.offset != b->generic_parameters.offset
        || a->generic_parameters.count != b->generic_parameters.count
        || a->effect_parameters.offset != b->effect_parameters.offset
        || a->effect_parameters.count != b->effect_parameters.count
        || a->entry != b->entry || a->contract_body != b->contract_body
        || a->contract_epilogue != b->contract_epilogue) return false;
#define SAME(field, count, capacity) if (a->count != b->count || a->capacity != a->count \
    || (a->count != 0 && memcmp(a->field, b->field, a->count * sizeof(*a->field)) != 0)) return false
    SAME(blocks, block_count, block_capacity); SAME(instructions, instruction_count, instruction_capacity);
    SAME(values, value_count, value_capacity); SAME(parameter_values, parameter_value_count, parameter_value_capacity);
    SAME(edge_values, edge_value_count, edge_value_capacity); SAME(call_arguments, call_argument_count, call_argument_capacity);
    SAME(loops, loop_count, loop_capacity); SAME(construct_operands, construct_operand_count, construct_operand_capacity);
    SAME(temporaries, temporary_count, temporary_capacity);
#undef SAME
    return true;
}

static bool arrays_equal(const SolMirMaterialization *a,
    const SolMirMaterialization *b) {
#define SAME(field, count) if (a->count != b->count || (a->count != 0 \
    && memcmp(a->field, b->field, a->count * sizeof(*a->field)) != 0)) return false
    SAME(types, type_count); SAME(shape_fields, shape_field_count);
    SAME(shape_variants, shape_variant_count);
    SAME(type_ids, type_id_count); SAME(accesses, access_count);
    SAME(overlays, overlay_count); SAME(contexts, context_count); SAME(locals, local_count);
    SAME(places, place_count); SAME(projections, projection_count); SAME(values, value_count);
    SAME(instructions, instruction_count); SAME(temporaries, temporary_count);
    SAME(construct_operands, construct_operand_count); SAME(call_arguments, call_argument_count);
    SAME(blocks, block_count); SAME(edges, edge_count); SAME(edge_values, edge_value_count);
    SAME(parameter_values, parameter_value_count); SAME(loops, loop_count);
    SAME(bindings, binding_count); SAME(semantic_sites, semantic_site_count);
    SAME(receiver_roots, receiver_root_count);
    SAME(imports, import_count); SAME(handlers, handler_count);
    SAME(writebacks, writeback_count); SAME(effect_rows, effect_row_count);
    SAME(effect_atoms, effect_atom_count); SAME(effect_row_atoms, effect_row_atom_count);
    SAME(effect_names, effect_name_count); SAME(literal_bytes, literal_byte_count);
#undef SAME
    if (a->image_count != b->image_count) return false;
    for (size_t i = 0; i < a->image_count; ++i) {
        SolMir left = a->images[i].topology, right = b->images[i].topology;
        SolMirMaterializedImage ai = a->images[i], bi = b->images[i];
        memset(&ai.topology, 0, sizeof(ai.topology));
        memset(&bi.topology, 0, sizeof(bi.topology));
        if (memcmp(&ai, &bi, sizeof(ai)) != 0 || !topology_equal(&left, &right)) return false;
    }
    return memcmp(&a->usage, &b->usage, sizeof(a->usage)) == 0;
}

bool sol_mir_materialization_validate(const SolMirMaterialization *owner,
    SolDiagnostics *diagnostics) {
    if (owner == NULL || owner->plan == NULL || !limits_complete(owner->limits)
        || owner->usage.instances != owner->image_count
        || owner->usage.instances > owner->limits.max_instances
        || owner->usage.cfg_items > owner->limits.max_cfg_items
        || owner->usage.bindings > owner->limits.max_bindings
        || owner->usage.concrete_records > owner->limits.max_concrete_records
        || owner->usage.owned_bytes > owner->limits.max_owned_bytes
        || owner->usage.materialization_work > owner->limits.max_materialization_work
        || owner->usage.shape_resolution_work
            > owner->limits.max_shape_resolution_work
        || owner->usage.validation_work > owner->limits.max_validation_work
        || !sol_mir_plan_validate(owner->plan, diagnostics)) return validation_error(
            diagnostics, "materialized MIR header or borrowed provenance plan is invalid");
#define CANON(field, count, capacity) if (!canonical(owner->count, owner->capacity, owner->field)) goto malformed
    CANON(images, image_count, image_capacity); CANON(types, type_count, type_capacity);
    CANON(shape_fields, shape_field_count, shape_field_capacity);
    CANON(shape_variants, shape_variant_count, shape_variant_capacity);
    CANON(type_ids, type_id_count, type_id_capacity); CANON(accesses, access_count, access_capacity);
    CANON(overlays, overlay_count, overlay_capacity); CANON(contexts, context_count, context_capacity);
    CANON(locals, local_count, local_capacity); CANON(places, place_count, place_capacity);
    CANON(projections, projection_count, projection_capacity); CANON(values, value_count, value_capacity);
    CANON(instructions, instruction_count, instruction_capacity); CANON(temporaries, temporary_count, temporary_capacity);
    CANON(construct_operands, construct_operand_count, construct_operand_capacity);
    CANON(call_arguments, call_argument_count, call_argument_capacity); CANON(blocks, block_count, block_capacity);
    CANON(edges, edge_count, edge_capacity); CANON(edge_values, edge_value_count, edge_value_capacity);
    CANON(parameter_values, parameter_value_count, parameter_value_capacity); CANON(loops, loop_count, loop_capacity);
    CANON(bindings, binding_count, binding_capacity); CANON(imports, import_count, import_capacity);
    CANON(semantic_sites, semantic_site_count, semantic_site_capacity);
    CANON(receiver_roots, receiver_root_count, receiver_root_capacity);
    CANON(handlers, handler_count, handler_capacity); CANON(writebacks, writeback_count, writeback_capacity);
    CANON(effect_rows, effect_row_count, effect_row_capacity); CANON(effect_atoms, effect_atom_count, effect_atom_capacity);
    CANON(effect_row_atoms, effect_row_atom_count, effect_row_atom_capacity);
    CANON(effect_names, effect_name_count, effect_name_capacity);
    CANON(literal_bytes, literal_byte_count, literal_byte_capacity);
#undef CANON
    if (owner->image_count > (SIZE_MAX - 100) / 9) goto malformed;
    size_t max_ranges = 100 + owner->image_count * 9;
    if (owner->plan->program->template_count > (SIZE_MAX - max_ranges) / 9) {
        goto malformed;
    }
    max_ranges += owner->plan->program->template_count * 9;
    if (owner->plan->effect_atom_count > SIZE_MAX - max_ranges) goto malformed;
    max_ranges += owner->plan->effect_atom_count;
    size_t string_ranges[] = {2, owner->plan->program->ir->definition_count,
        owner->plan->program->ir->callable_count,
        owner->plan->program->ir->local_count,
        owner->plan->program->ir->field_count,
        owner->plan->program->ir->variant_count,
        owner->plan->program->ir->expression_count,
        owner->plan->program->ir->statement_count,
        owner->plan->program->ir->effect_count,
        owner->plan->program->ir->generic_parameter_count,
        owner->plan->program->ir->effect_parameter_count,
        owner->plan->program->ir->file_count};
    for (size_t i = 0; i < sizeof(string_ranges) / sizeof(string_ranges[0]); ++i) {
        if (string_ranges[i] > SIZE_MAX - max_ranges) goto malformed;
        max_ranges += string_ranges[i];
    }
    Range *ranges = calloc(max_ranges, sizeof(*ranges));
    if (ranges == NULL) return validation_error(diagnostics,
        "allocation failed while validating materialized MIR ranges");
    size_t range_count = 0;
    const SolMirPlan *plan = owner->plan;
    const SolMirProgram *program = plan->program;
    const SolIr *ir = program->ir;
#define BORROW(pointer, count, type) if (!add_range(ranges, &range_count, pointer, count, sizeof(type))) goto range_malformed
    BORROW(plan->types, plan->type_capacity, *plan->types);
    BORROW(plan->type_components, plan->type_component_capacity, *plan->type_components);
    BORROW(plan->type_parameter_accesses, plan->type_parameter_access_capacity, *plan->type_parameter_accesses);
    BORROW(plan->effect_atoms, plan->effect_atom_capacity, *plan->effect_atoms);
    BORROW(plan->effect_rows, plan->effect_row_capacity, *plan->effect_rows);
    BORROW(plan->effect_row_atoms, plan->effect_row_atom_capacity, *plan->effect_row_atoms);
    BORROW(plan->instances, plan->instance_capacity, *plan->instances);
    BORROW(plan->instance_type_ids, plan->instance_type_id_capacity, *plan->instance_type_ids);
    BORROW(plan->instance_accesses, plan->instance_access_capacity, *plan->instance_accesses);
    BORROW(plan->dictionary_entries, plan->dictionary_entry_capacity, *plan->dictionary_entries);
    BORROW(plan->imports, plan->import_capacity, *plan->imports);
    BORROW(plan->typed_uses, plan->typed_use_capacity, *plan->typed_uses);
    BORROW(plan->contexts, plan->context_capacity, *plan->contexts);
    BORROW(plan->demands, plan->demand_capacity, *plan->demands);
    for (size_t i = 0; i < plan->effect_atom_count; ++i) {
        BORROW(plan->effect_atoms[i].name, plan->effect_atoms[i].length + 1, char);
    }
    BORROW(program->roots, program->root_count, *program->roots);
    BORROW(program->approved_imports, program->approved_import_count, *program->approved_imports);
    BORROW(program->templates, program->template_count, *program->templates);
    BORROW(program->imports, program->import_count, *program->imports);
    BORROW(program->specializations, program->specialization_count, *program->specializations);
    BORROW(program->references, program->reference_count, *program->references);
    BORROW(ir->types, ir->type_count, *ir->types);
    BORROW(ir->type_ids, ir->type_id_count, *ir->type_ids);
    BORROW(ir->accesses, ir->access_count, *ir->accesses);
    BORROW(ir->definitions, ir->definition_count, *ir->definitions);
    BORROW(ir->callables, ir->callable_count, *ir->callables);
    BORROW(ir->members, ir->member_count, *ir->members);
    BORROW(ir->evidence, ir->evidence_count, *ir->evidence);
    BORROW(ir->locals, ir->local_count, *ir->locals);
    BORROW(ir->fields, ir->field_count, *ir->fields);
    BORROW(ir->variants, ir->variant_count, *ir->variants);
    BORROW(ir->expressions, ir->expression_count, *ir->expressions);
    BORROW(ir->places, ir->place_count, *ir->places);
    BORROW(ir->projections, ir->projection_count, *ir->projections);
    BORROW(ir->statements, ir->statement_count, *ir->statements);
    BORROW(ir->statement_ids, ir->statement_id_count, *ir->statement_ids);
    BORROW(ir->arms, ir->arm_count, *ir->arms);
    BORROW(ir->arm_ids, ir->arm_id_count, *ir->arm_ids);
    BORROW(ir->patterns, ir->pattern_count, *ir->patterns);
    BORROW(ir->pattern_children, ir->pattern_child_count, *ir->pattern_children);
    BORROW(ir->operands, ir->operand_count, *ir->operands);
    BORROW(ir->roots, ir->root_count, *ir->roots);
    BORROW(ir->cleanup_locals, ir->cleanup_local_count, *ir->cleanup_locals);
    BORROW(ir->effects, ir->effect_count, *ir->effects);
    BORROW(ir->generic_parameters, ir->generic_parameter_count, *ir->generic_parameters);
    BORROW(ir->effect_parameters, ir->effect_parameter_count, *ir->effect_parameters);
    BORROW(ir->obligations, ir->obligation_count, *ir->obligations);
    BORROW(ir->snapshots, ir->snapshot_count, *ir->snapshots);
    BORROW(ir->loop_obligations, ir->loop_obligation_count, *ir->loop_obligations);
    BORROW(ir->unreachable_obligations, ir->unreachable_obligation_count, *ir->unreachable_obligations);
    BORROW(ir->files, ir->file_count, *ir->files);
    BORROW(ir->source_path, strlen(ir->source_path) + 1, char);
    BORROW(ir->source_bytes, ir->source_length + 1, char);
#define STRING(pointer) do { const char *text = (pointer); if (text != NULL) \
    BORROW(text, strlen(text) + 1, char); } while (0)
    for (size_t i = 0; i < ir->definition_count; ++i) STRING(ir->definitions[i].name);
    for (size_t i = 0; i < ir->callable_count; ++i) STRING(ir->callables[i].name);
    for (size_t i = 0; i < ir->local_count; ++i) STRING(ir->locals[i].name);
    for (size_t i = 0; i < ir->field_count; ++i) STRING(ir->fields[i].name);
    for (size_t i = 0; i < ir->variant_count; ++i) STRING(ir->variants[i].name);
    for (size_t i = 0; i < ir->expression_count; ++i) {
        if (ir->expressions[i].kind == SOL_IR_EXPR_STRING) {
            STRING(ir->expressions[i].as.string);
        } else if (ir->expressions[i].kind == SOL_IR_EXPR_HANDLE) {
            STRING(ir->expressions[i].as.handler.effect_name);
        }
    }
    for (size_t i = 0; i < ir->statement_count; ++i) STRING(ir->statements[i].region_label);
    for (size_t i = 0; i < ir->effect_count; ++i) STRING(ir->effects[i].name);
    for (size_t i = 0; i < ir->generic_parameter_count; ++i) STRING(ir->generic_parameters[i].name);
    for (size_t i = 0; i < ir->effect_parameter_count; ++i) STRING(ir->effect_parameters[i].name);
    for (size_t i = 0; i < ir->file_count; ++i) STRING(ir->files[i].path);
#undef STRING
    for (size_t i = 0; i < program->template_count; ++i) {
        const SolMir *mir = &program->templates[i].mir;
#define SOURCE_RANGE(field, capacity) BORROW(mir->field, mir->capacity, *mir->field)
        SOURCE_RANGE(blocks, block_capacity); SOURCE_RANGE(instructions, instruction_capacity);
        SOURCE_RANGE(values, value_capacity); SOURCE_RANGE(parameter_values, parameter_value_capacity);
        SOURCE_RANGE(edge_values, edge_value_capacity); SOURCE_RANGE(call_arguments, call_argument_capacity);
        SOURCE_RANGE(loops, loop_capacity); SOURCE_RANGE(construct_operands, construct_operand_capacity);
        SOURCE_RANGE(temporaries, temporary_capacity);
#undef SOURCE_RANGE
    }
#undef BORROW
#define RANGE(field, count) if (!add_range(ranges, &range_count, owner->field, owner->count, sizeof(*owner->field))) goto range_malformed
    RANGE(images, image_capacity); RANGE(types, type_capacity);
    RANGE(shape_fields, shape_field_capacity);
    RANGE(shape_variants, shape_variant_capacity);
    RANGE(type_ids, type_id_capacity);
    RANGE(accesses, access_capacity); RANGE(overlays, overlay_capacity); RANGE(contexts, context_capacity);
    RANGE(locals, local_capacity); RANGE(places, place_capacity); RANGE(projections, projection_capacity);
    RANGE(values, value_capacity); RANGE(instructions, instruction_capacity); RANGE(temporaries, temporary_capacity);
    RANGE(construct_operands, construct_operand_capacity); RANGE(call_arguments, call_argument_capacity);
    RANGE(blocks, block_capacity); RANGE(edges, edge_capacity); RANGE(edge_values, edge_value_capacity);
    RANGE(parameter_values, parameter_value_capacity); RANGE(loops, loop_capacity); RANGE(bindings, binding_capacity);
    RANGE(semantic_sites, semantic_site_capacity);
    RANGE(receiver_roots, receiver_root_capacity);
    RANGE(imports, import_capacity); RANGE(handlers, handler_capacity); RANGE(writebacks, writeback_capacity);
    RANGE(effect_rows, effect_row_capacity); RANGE(effect_atoms, effect_atom_capacity);
    RANGE(effect_row_atoms, effect_row_atom_capacity); RANGE(effect_names, effect_name_capacity);
    RANGE(literal_bytes, literal_byte_capacity);
    for (size_t i = 0; i < owner->image_count; ++i) {
        const SolMir *mir = &owner->images[i].topology;
#define MIR_RANGE(field, capacity) if (!add_range(ranges, &range_count, mir->field, mir->capacity, sizeof(*mir->field))) goto range_malformed
        MIR_RANGE(blocks, block_capacity); MIR_RANGE(instructions, instruction_capacity);
        MIR_RANGE(values, value_capacity); MIR_RANGE(parameter_values, parameter_value_capacity);
        MIR_RANGE(edge_values, edge_value_capacity); MIR_RANGE(call_arguments, call_argument_capacity);
        MIR_RANGE(loops, loop_capacity); MIR_RANGE(construct_operands, construct_operand_capacity);
        MIR_RANGE(temporaries, temporary_capacity);
#undef MIR_RANGE
    }
#undef RANGE
    free(ranges);
    SolMirMaterialization expected;
    sol_mir_materialization_init(&expected);
    expected.plan = owner->plan; expected.limits = owner->limits;
    Builder builder = {&expected, NULL, SOL_MIR_MATERIALIZE_BUILD_INTERNAL_FAILED};
    if (!build_scratch(&builder)) { sol_mir_materialization_free(&expected); goto malformed; }
    bool equal = arrays_equal(owner, &expected);
    sol_mir_materialization_free(&expected);
    if (!equal) goto malformed;
    if (!sol_mir_materialization_validate_concrete(owner, diagnostics)) {
        return false;
    }
    return true;
range_malformed:
    free(ranges);
malformed:
    return validation_error(diagnostics,
        "materialized MIR concrete CFG, provenance, slice, or allocation is malformed");
}

typedef struct { char *data; size_t length, capacity; bool failed; } Buffer;

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

bool sol_mir_materialization_render(FILE *stream,
    const SolMirMaterialization *owner) {
    if (stream == NULL || !sol_mir_materialization_validate(owner, NULL)) return false;
    Buffer out = {0};
    format(&out, "materialized_mir images=%zu types=%zu imports=%zu bindings=%zu sites=%zu handlers=%zu blocks=%zu edges=%zu writebacks=%zu shape_work=%zu validation_work=%zu\n",
        owner->image_count, owner->type_count, owner->import_count,
        owner->binding_count, owner->semantic_site_count, owner->handler_count,
        owner->block_count, owner->edge_count, owner->writeback_count,
        owner->usage.shape_resolution_work,
        owner->usage.validation_work);
    for (size_t i = 0; i < owner->type_count; ++i) {
        const SolMirMaterializedType *type = &owner->types[i];
        format(&out, "type t%zu kind=%d definition=%zu nominal=%d open=%d copy=%d effects=e%zu fields=[%zu,%zu] variants=[%zu,%zu] backing=%zu capability_source=%zu ownership=[%zu,%zu]\n",
            i, (int)type->kind, type->definition, (int)type->nominal_category,
            type->nominal_open, type->is_copy, type->effects,
            type->fields.offset, type->fields.count, type->variants.offset,
            type->variants.count, type->backing, type->capability_source,
            type->ownership_components.offset, type->ownership_components.count);
    }
    for (size_t i = 0; i < owner->shape_field_count; ++i) {
        const SolMirMaterializedShapeField *field = &owner->shape_fields[i];
        format(&out, "shape_field f%zu source=%zu ordinal=%zu type=t%zu\n",
            i, field->source_field, field->ordinal, field->type);
    }
    for (size_t i = 0; i < owner->shape_variant_count; ++i) {
        const SolMirMaterializedShapeVariant *variant = &owner->shape_variants[i];
        format(&out, "shape_variant v%zu source=%zu ordinal=%zu fields=[%zu,%zu]\n",
            i, variant->source_variant, variant->ordinal,
            variant->fields.offset, variant->fields.count);
    }
    for (size_t i = 0; i < owner->effect_atom_count; ++i) {
        const SolMirMaterializedEffectAtom *atom = &owner->effect_atoms[i];
        format(&out, "effect_atom e%zu name=", i);
        for (size_t j = 0; j < atom->name.count; ++j) {
            format(&out, "%02x", (unsigned char)owner->effect_names[
                atom->name.offset + j]);
        }
        format(&out, " authority=%d ordinal=%zu\n", (int)atom->authority,
            atom->ordinal);
    }
    for (size_t i = 0; i < owner->effect_row_count; ++i) {
        const SolMirMaterializedEffectRow *row = &owner->effect_rows[i];
        format(&out, "effect_row e%zu atoms=[", i);
        for (size_t j = 0; j < row->atoms.count; ++j) {
            format(&out, "%s%zu", j == 0 ? "" : ",",
                owner->effect_row_atoms[row->atoms.offset + j]);
        }
        format(&out, "]\n");
    }
    format(&out, "literal_bytes=");
    for (size_t i = 0; i < owner->literal_byte_count; ++i) {
        format(&out, "%02x", (unsigned char)owner->literal_bytes[i]);
    }
    format(&out, "\n");
    for (size_t i = 0; i < owner->context_count; ++i) {
        const SolMirPlanContext *context = &owner->contexts[i];
        format(&out, "context k%zu kind=%d owner=%d:%zu:%zu block=%zu definition=%zu obligation=%zu source=c%zu:x%zu\n",
            i, (int)context->kind, (int)context->target_kind,
            context->instance, context->import, context->source_block,
            context->definition, context->obligation, context->source.callable,
            context->source.expression);
    }
    for (size_t i = 0; i < owner->import_count; ++i) {
        const SolMirMaterializedImport *item = &owner->imports[i];
        format(&out, "import m%zu callable=c%zu receiver=%zu receiver_access=%d result=t%zu effects=e%zu params=[",
            i, item->source_callable, item->receiver, (int)item->receiver_access,
            item->result, item->effects);
        for (size_t j = 0; j < item->parameter_types.count; ++j) {
            format(&out, "%st%zu/%d", j == 0 ? "" : ",",
                owner->type_ids[item->parameter_types.offset + j],
                (int)owner->accesses[item->parameter_accesses.offset + j]);
        }
        format(&out, "] source_import=%zu overlays=%zu:%zu contexts=%zu:%zu\n",
            item->source_import, item->overlays.offset, item->overlays.count,
            item->contexts.offset, item->contexts.count);
    }
    for (size_t i = 0; i < owner->binding_count; ++i) {
        const SolMirMaterializedBinding *item = &owner->bindings[i];
        format(&out, "binding d%zu plan=d%zu kind=%d owner=%d:%zu:%zu context=%zu source=c%zu:x%zu target=%c%zu symbolic=c%zu trait=%zu requirement=%zu\n",
            i, item->source_demand, (int)item->kind, (int)item->owner_kind,
            item->parent, item->parent_import, item->context, item->source.callable,
            item->source.expression,
            item->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE ? 'i' : 'm',
            item->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE
                ? item->instance : item->import,
            item->symbolic_callable, item->dispatch_trait,
            item->dispatch_requirement);
    }
    for (size_t i = 0; i < owner->semantic_site_count; ++i) {
        const SolMirMaterializedSemanticSite *item = &owner->semantic_sites[i];
        format(&out, "semantic_site s%zu kind=%d binding=d%zu owner=%d:%zu:%zu context=%zu definition=%zu obligation=%zu block=%zu instruction=%zu handler=%zu function_type=%zu receiver_type=%zu receiver_kind=%d receiver_expression=%zu receiver_place=%zu receiver_temporary=%zu receiver_value=%zu receiver_instruction=%zu receiver_roots=[%zu,%zu]",
            i, (int)item->kind, item->binding, (int)item->owner_kind,
            item->parent, item->parent_import, item->context,
            item->source_definition, item->source_obligation, item->block,
            item->instruction, item->handler, item->produced_function_type,
            item->captured_receiver_type, (int)item->captured_receiver_kind,
            item->captured_receiver_expression, item->captured_receiver_place,
            item->captured_receiver_temporary, item->captured_receiver_value,
            item->captured_receiver_instruction,
            item->captured_receiver_roots.offset,
            item->captured_receiver_roots.count);
        if (item->kind == SOL_MIR_PLAN_DEMAND_BOUND_OPERATION) {
            format(&out, " operation={target=%d/%zu/%zu receiver=%zu root=%zu effects=%zu}",
                (int)item->operation.target_kind, item->operation.instance,
                item->operation.import, item->operation.receiver,
                item->operation.root, item->operation.effects);
        }
        format(&out, " source=c%zu:x%zu\n", item->source.callable,
            item->source.expression);
    }
    for (size_t i = 0; i < owner->image_count; ++i) {
        const SolMirMaterializedImage *image = &owner->images[i];
        format(&out, "image i%zu callable=c%zu receiver=%zu receiver_access=%d result=t%zu effects=e%zu entry=b%zu blocks=%zu handlers=%zu provenance=authentication-only\n",
            i, image->source_callable, image->receiver, (int)image->receiver_access, image->result,
            image->effects, image->entry, image->blocks.count,
            image->handlers.count);
        for (size_t j = 0; j < image->locals.count; ++j) {
            const SolMirMaterializedLocal *x = &owner->locals[image->locals.offset + j];
            format(&out, "  local l%zu source=l%zu type=t%zu access=%d kind=%d ordinal=%zu\n",
                image->locals.offset + j, x->source_local, x->type,
                (int)x->access, (int)x->kind, x->ordinal);
        }
        for (size_t j = 0; j < image->places.count; ++j) {
            const SolMirMaterializedPlace *x = &owner->places[image->places.offset + j];
            format(&out, "  place p%zu source=%zu local=l%zu root=t%zu final=t%zu projections=[%zu,%zu]\n",
                image->places.offset + j, x->source_place, x->local,
                x->root_type, x->final_type, x->projections.offset,
                x->projections.count);
            for (size_t p = 0; p < x->projections.count; ++p) {
                const SolMirMaterializedProjection *projection
                    = &owner->projections[x->projections.offset + p];
                format(&out, "    projection r%zu kind=%d type=t%zu field=%zu tuple=%zu source=%zu\n",
                    x->projections.offset + p, (int)projection->kind,
                    projection->type, projection->source_field,
                    projection->tuple_ordinal, projection->source_projection);
            }
        }
        for (size_t j = 0; j < image->values.count; ++j) {
            const SolMirMaterializedValue *x = &owner->values[image->values.offset + j];
            format(&out, "  value v%zu kind=%d block=b%zu type=t%zu definition=%zu instruction=%zu source=x%zu span=%zu..%zu\n",
                image->values.offset + j, (int)x->kind, x->block, x->type,
                x->source_definition, x->instruction, x->source_expression,
                x->span.start, x->span.end);
        }
        for (size_t j = 0; j < image->instructions.count; ++j) {
            const SolMirMaterializedInstruction *x = &owner->instructions[image->instructions.offset + j];
            format(&out, "  instruction n%zu kind=%d block=b%zu result=%zu type=%zu integer=%lld boolean=%d text=[%zu,%zu] local=%zu place=%zu left=%zu right=%zu temporary=%zu previous=%zu preserve=%zu operator=%d statement=%zu match=%zu arm=%zu arm_ordinal=%zu pattern=%zu scrutinee=%zu snapshot=%zu handler=%zu construct=%d definition=%zu variant=%zu operands=[%zu,%zu] roots=[%zu,%zu] operations=[%zu,%zu] source=x%zu span=%zu..%zu\n",
                image->instructions.offset + j, (int)x->kind, x->block,
                x->result, x->type, (long long)x->integer, x->boolean,
                x->text.offset, x->text.count, x->local, x->place, x->left,
                x->right, x->temporary, x->previous, x->preserve_depth,
                (int)x->operator_kind, x->source_statement, x->match_expression,
                x->source_arm, x->arm_ordinal, x->source_pattern,
                x->pattern_scrutinee, x->source_snapshot, x->handler,
                (int)x->construct_kind, x->construct_definition,
                x->construct_variant, x->construct_operands.offset,
                x->construct_operands.count, x->source_capability_roots.offset,
                x->source_capability_roots.count, x->source_operation_roots.offset,
                x->source_operation_roots.count, x->source_expression,
                x->span.start, x->span.end);
        }
        for (size_t j = 0; j < image->temporaries.count; ++j) {
            const SolMirMaterializedTemporary *x
                = &owner->temporaries[image->temporaries.offset + j];
            format(&out, "  temporary q%zu type=t%zu source=x%zu span=%zu..%zu\n",
                image->temporaries.offset + j, x->type, x->source_expression,
                x->span.start, x->span.end);
        }
        for (size_t j = 0; j < image->construct_operands.count; ++j) {
            const SolMirMaterializedConstructOperand *x
                = &owner->construct_operands[image->construct_operands.offset + j];
            format(&out, "  construct_operand o%zu formal=%zu source=x%zu type=t%zu temporary=q%zu\n",
                image->construct_operands.offset + j, x->formal,
                x->source_expression, x->type, x->temporary);
        }
        for (size_t j = 0; j < image->call_arguments.count; ++j) {
            const SolMirMaterializedCallArgument *x
                = &owner->call_arguments[image->call_arguments.offset + j];
            format(&out, "  call_argument a%zu formal=%zu access=%d source=x%zu type=t%zu temporary=%zu place=%zu\n",
                image->call_arguments.offset + j, x->formal, (int)x->access,
                x->source_expression, x->type, x->temporary, x->place);
        }
        for (size_t j = 0; j < image->handlers.count; ++j) {
            const SolMirMaterializedHandler *x = &owner->handlers[image->handlers.offset + j];
            format(&out, "  handler h%zu source=d%zu provider=d%zu authority=p%zu provider_place=p%zu operation=c%zu effect=%zu root=l%zu\n",
                image->handlers.offset + j, x->source_binding, x->provider_binding,
                x->authority, x->provider, x->handled_operation, x->source_effect,
                x->source_root);
        }
        for (size_t j = 0; j < image->blocks.count; ++j) {
            const SolMirMaterializedBlock *block = &owner->blocks[image->blocks.offset + j];
            const SolMirMaterializedTerminator *term = &block->terminator;
            format(&out, "  block b%zu source=b%zu order=%zu params=[",
                block->id, block->source_block, block->order);
            for (size_t p = 0; p < block->parameters.count; ++p) {
                format(&out, "%sv%zu", p == 0 ? "" : ",",
                    owner->parameter_values[block->parameters.offset + p]);
            }
            format(&out, "] instructions=[%zu,%zu] started=%d span=%zu..%zu\n",
                block->instructions.offset, block->instructions.count,
                block->started, block->span.start, block->span.end);
            format(&out, "    terminator kind=%d binding=%zu effects=%zu call_kind=%d callee=%zu receiver={formal=%zu access=%d source=%zu type=%zu temporary=%zu place=%zu} arguments=[%zu,%zu] result=%zu value=%zu condition=%zu value_result=%zu residual_result=%zu representation=%zu operand=%zu edge=%zu true=%zu false=%zu normal=%zu failure=%zu value_edge=%zu residual_edge=%zu satisfied=%zu violation=%zu loop=%zu predicate_inline=%d writebacks=[%zu,%zu] statement=%zu definition=%zu obligation=%zu ordinal=%zu propagation=%d phase=%d outcome=%d source=x%zu span=%zu..%zu\n",
                (int)term->kind, term->binding, term->effects,
                (int)term->call_kind, term->callee, term->receiver.formal,
                (int)term->receiver.access, term->receiver.source_expression,
                term->receiver.type, term->receiver.temporary,
                term->receiver.place, term->arguments.offset,
                term->arguments.count, term->result, term->value,
                term->condition, term->value_result, term->residual_result,
                term->representation, term->operand, term->edge,
                term->true_edge, term->false_edge, term->normal_edge,
                term->failure_edge, term->value_edge, term->residual_edge,
                term->satisfied_edge, term->violation_edge, term->loop,
                term->predicate_inline,
                term->writebacks.offset, term->writebacks.count,
                term->source_statement, term->source_definition,
                term->source_obligation, term->obligation_ordinal,
                (int)term->propagation_kind, (int)term->contract_phase,
                (int)term->contract_outcome, term->source_expression,
                term->span.start, term->span.end);
        }
        for (size_t j = 0; j < image->loops.count; ++j) {
            const SolMirMaterializedLoop *x = &owner->loops[image->loops.offset + j];
            format(&out, "  loop z%zu source=z%zu statement=%zu parent=%zu preheader=b%zu header=b%zu condition=b%zu body=b%zu backedge=b%zu exit=b%zu obligations=[%zu,%zu] span=%zu..%zu\n",
                image->loops.offset + j, x->source_loop, x->source_statement,
                x->parent, x->preheader, x->header, x->condition, x->body,
                x->backedge, x->exit, x->source_obligations.offset,
                x->source_obligations.count, x->span.start, x->span.end);
        }
    }
    for (size_t i = 0; i < owner->edge_count; ++i) {
        format(&out, "edge g%zu block=b%zu arguments=[", i,
            owner->edges[i].block);
        for (size_t j = 0; j < owner->edges[i].arguments.count; ++j) {
            format(&out, "%sv%zu", j == 0 ? "" : ",",
                owner->edge_values[owner->edges[i].arguments.offset + j]);
        }
        format(&out, "]\n");
    }
    for (size_t i = 0; i < owner->writeback_count; ++i) {
        const SolMirMaterializedWriteback *x = &owner->writebacks[i];
        format(&out, "writeback w%zu receiver=%d formal=%zu place=p%zu type=t%zu\n",
            i, x->receiver, x->formal, x->place, x->type);
    }
    bool ok = !out.failed && (out.length == 0
        || fwrite(out.data, 1, out.length, stream) == out.length);
    free(out.data); return ok;
}
