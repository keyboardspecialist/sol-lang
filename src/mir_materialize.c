#include "sol/mir_materialize.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SolMirMaterialization *output;
    SolDiagnostics *diagnostics;
    SolMirMaterializeBuildOutcome outcome;
} Materializer;

static bool owner_empty(const SolMirMaterialization *owner) {
    if (owner == NULL) return false;
    const unsigned char *bytes = (const unsigned char *)owner;
    for (size_t index = 0; index < sizeof(*owner); ++index) {
        if (bytes[index] != 0) return false;
    }
    return true;
}

void sol_mir_materialization_init(SolMirMaterialization *owner) {
    if (owner != NULL) memset(owner, 0, sizeof(*owner));
}

void sol_mir_materialization_free(SolMirMaterialization *owner) {
    if (owner == NULL) return;
    for (size_t index = 0; owner->images != NULL
        && index < owner->image_capacity; ++index) {
        sol_mir_free(&owner->images[index].topology);
    }
    free(owner->images);
    free(owner->types);
    free(owner->type_ids);
    free(owner->accesses);
    free(owner->overlays);
    free(owner->contexts);
    free(owner->locals);
    free(owner->places);
    free(owner->projections);
    free(owner->values);
    free(owner->instructions);
    free(owner->temporaries);
    free(owner->construct_operands);
    free(owner->call_arguments);
    free(owner->invoke_bindings);
    sol_mir_materialization_init(owner);
}

SolMirMaterializeLimits sol_mir_materialize_default_limits(void) {
    return (SolMirMaterializeLimits){4096, 4000000, 1000000, 8000000,
        512u * 1024u * 1024u, 16000000};
}

static bool limits_zero(SolMirMaterializeLimits limits) {
    return limits.max_instances == 0 && limits.max_cfg_items == 0
        && limits.max_bindings == 0 && limits.max_owned_bytes == 0
        && limits.max_concrete_records == 0
        && limits.max_materialization_work == 0;
}

static bool limits_complete(SolMirMaterializeLimits limits) {
    return limits.max_instances != 0 && limits.max_cfg_items != 0
        && limits.max_bindings != 0 && limits.max_owned_bytes != 0
        && limits.max_concrete_records != 0
        && limits.max_materialization_work != 0;
}

static bool report(Materializer *builder, SolMirMaterializeBuildOutcome outcome,
    const char *message) {
    builder->outcome = outcome;
    if (builder->diagnostics != NULL) sol_diagnostics_add(builder->diagnostics,
        "SOL-MIR-MATERIALIZE-001", SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    return false;
}

static bool add_limited(size_t *value, size_t amount, size_t limit) {
    if (amount > limit - *value) return false;
    *value += amount;
    return true;
}

static bool charge(Materializer *builder, size_t work) {
    if (!add_limited(&builder->output->usage.materialization_work, work,
            builder->output->limits.max_materialization_work)) {
        return report(builder, SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
            "MIR materialization work limit exceeded");
    }
    return true;
}

static void *allocate_owned(Materializer *builder, size_t count, size_t size) {
    if (count == 0) return NULL;
    if (size == 0 || count > SIZE_MAX / size) {
        report(builder, SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
            "MIR materialization owned-byte calculation overflowed");
        return NULL;
    }
    size_t bytes = count * size;
    if (!add_limited(&builder->output->usage.owned_bytes, bytes,
            builder->output->limits.max_owned_bytes)) {
        report(builder, SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
            "MIR materialization owned-byte limit exceeded");
        return NULL;
    }
    void *memory = calloc(count, size);
    if (memory == NULL) {
        builder->output->usage.owned_bytes -= bytes;
        report(builder, SOL_MIR_MATERIALIZE_BUILD_ALLOCATION_FAILED,
            "MIR materialization allocation failed");
    }
    return memory;
}

static bool clone_array(Materializer *builder, void **target, const void *source,
    size_t count, size_t size) {
    if (!charge(builder, count)) return false;
    *target = allocate_owned(builder, count, size);
    if (count != 0 && *target == NULL) return false;
    if (count != 0) memcpy(*target, source, count * size);
    return true;
}

static size_t mir_item_count(const SolMir *mir) {
    size_t counts[] = {mir->block_count, mir->instruction_count, mir->value_count,
        mir->parameter_value_count, mir->edge_value_count,
        mir->call_argument_count, mir->loop_count,
        mir->construct_operand_count, mir->temporary_count};
    size_t total = 0;
    for (size_t index = 0; index < sizeof(counts) / sizeof(counts[0]); ++index) {
        if (counts[index] > SIZE_MAX - total) return SIZE_MAX;
        total += counts[index];
    }
    return total;
}

static bool clone_mir(Materializer *builder, const SolMir *source,
    SolMir *target) {
    size_t items = mir_item_count(source);
    if (items == SIZE_MAX || !add_limited(&builder->output->usage.cfg_items,
            items, builder->output->limits.max_cfg_items)) {
        return report(builder, SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
            "cloned MIR CFG item limit exceeded");
    }
    *target = *source;
    target->blocks = NULL; target->instructions = NULL; target->values = NULL;
    target->parameter_values = NULL; target->edge_values = NULL;
    target->call_arguments = NULL; target->loops = NULL;
    target->construct_operands = NULL; target->temporaries = NULL;
#define CLONE(field, count, capacity) do { \
    target->capacity = source->count; \
    if (!clone_array(builder, (void **)&target->field, source->field, \
            source->count, sizeof(*source->field))) return false; \
} while (0)
    CLONE(blocks, block_count, block_capacity);
    CLONE(instructions, instruction_count, instruction_capacity);
    CLONE(values, value_count, value_capacity);
    CLONE(parameter_values, parameter_value_count, parameter_value_capacity);
    CLONE(edge_values, edge_value_count, edge_value_capacity);
    CLONE(call_arguments, call_argument_count, call_argument_capacity);
    CLONE(loops, loop_count, loop_capacity);
    CLONE(construct_operands, construct_operand_count,
        construct_operand_capacity);
    CLONE(temporaries, temporary_count, temporary_capacity);
#undef CLONE
    return true;
}

static const SolMirProgramTemplate *find_template(const SolMirPlan *plan,
    SolIrCallableId callable) {
    for (size_t index = 0; index < plan->program->template_count; ++index) {
        if (plan->program->templates[index].callable == callable) {
            return &plan->program->templates[index];
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
    source->callable = callable;
    source->expression = term->as.invoke.source_expression;
    source->file = 0;
    source->start = term->span.start;
    source->end = term->span.end;
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
    const SolMirPlanTypedUse *found = NULL;
    for (size_t index = 0; index < instance->typed_uses.count; ++index) {
        const SolMirPlanTypedUse *use
            = &plan->typed_uses[instance->typed_uses.offset + index];
        if (use->context == context && use->kind == kind && use->source == source
            && use->ordinal == ordinal) {
            if (found != NULL) return NULL;
            found = use;
        }
    }
    return found;
}

static SolMirMaterializedTypeId argument_type(const SolMirPlan *plan,
    const SolMirPlanInstance *parent, const SolMirCallArgument *argument) {
    const SolMirPlanTypedUse *use;
    SolMirPlanContextId body = parent->contexts.offset;
    if (argument->access == SOL_ACCESS_OWNED) {
        use = find_use(plan, parent, SOL_MIR_PLAN_USE_MIR_TEMPORARY,
            argument->temporary, 0, body);
    } else if (argument->place != SOL_IR_NONE) {
        use = find_use(plan, parent, SOL_MIR_PLAN_USE_PLACE_FINAL,
            argument->place, 0, body);
    } else {
        return SOL_MIR_MATERIALIZED_NONE;
    }
    return use == NULL ? SOL_MIR_MATERIALIZED_NONE : use->type;
}

static bool verify_signature(Materializer *builder,
    const SolMirPlanInstance *parent, const SolMir *mir,
    const SolMirTerminator *term, const SolMirPlanDemand *demand) {
    const SolMirPlan *plan = builder->output->plan;
    SolMirPlanSlice parameters;
    SolMirPlanSlice accesses;
    SolMirMaterializedTypeId receiver;
    SolMirMaterializedTypeId result;
    if (demand->instance != SOL_MIR_PLAN_NONE) {
        const SolMirPlanInstance *target = &plan->instances[demand->instance];
        parameters = target->parameter_types;
        accesses = target->parameter_accesses;
        receiver = target->receiver;
        result = target->result;
    } else {
        const SolMirPlanImport *target = &plan->imports[demand->import];
        parameters = target->parameter_types;
        accesses = target->parameter_accesses;
        receiver = target->receiver;
        result = target->result;
    }
    if (parameters.count != term->as.invoke.arguments.count
        || accesses.count != parameters.count) return false;
    for (size_t index = 0; index < parameters.count; ++index) {
        const SolMirCallArgument *argument = &mir->call_arguments[
            term->as.invoke.arguments.offset + index];
        if (argument->formal != index
            || argument->access != plan->instance_accesses[accesses.offset + index]
            || argument_type(plan, parent, argument)
                != plan->instance_type_ids[parameters.offset + index]) return false;
    }
    if (receiver == SOL_MIR_PLAN_NONE) {
        if (term->as.invoke.receiver.source_expression != SOL_IR_NONE) return false;
    } else if (argument_type(plan, parent, &term->as.invoke.receiver) != receiver) {
        return false;
    }
    if (term->as.invoke.result == SOL_MIR_NONE) {
        return result < plan->type_count
            && plan->types[result].kind == SOL_IR_TYPE_NEVER
            && term->as.invoke.normal_edge.block == SOL_MIR_NONE;
    }
    const SolMirPlanTypedUse *result_use = find_use(plan, parent,
        SOL_MIR_PLAN_USE_MIR_VALUE, term->as.invoke.result, 0,
        parent->contexts.offset);
    return result_use != NULL && result_use->type == result;
}

static bool append_binding(Materializer *builder, SolMirPlanInstanceId parent_id,
    const SolMirPlanInstance *parent, const SolMir *mir, size_t block) {
    const SolMirTerminator *term = &mir->blocks[block].terminator;
    SolMirProgramSource source;
    if (!source_for_term(builder->output->plan, parent->callable, term, &source)) {
        return report(builder, SOL_MIR_MATERIALIZE_BUILD_INVALID_PLAN,
            "materialized invoke has no exact source provenance");
    }
    const SolMirPlanDemand *found = NULL;
    for (size_t index = 0; index < builder->output->plan->demand_count; ++index) {
        const SolMirPlanDemand *demand = &builder->output->plan->demands[index];
        if (demand->parent == parent_id
            && demand->context == parent->contexts.offset
            && (demand->kind == SOL_MIR_PLAN_DEMAND_INVOKE
                || demand->kind == SOL_MIR_PLAN_DEMAND_CALLBACK)
            && source_equal(demand->source, source)) {
            if (found != NULL) return report(builder,
                SOL_MIR_MATERIALIZE_BUILD_UNSUPPORTED_OR_UNRESOLVED,
                "materialized invoke has ambiguous canonical plan demands");
            found = demand;
        }
    }
    if (found == NULL || !verify_signature(builder, parent, mir, term, found)) {
        return report(builder, SOL_MIR_MATERIALIZE_BUILD_UNSUPPORTED_OR_UNRESOLVED,
            "materialized invoke demand or concrete signature does not match");
    }
    SolMirMaterializedInvokeBinding *binding
        = &builder->output->invoke_bindings[builder->output->invoke_binding_count++];
    *binding = (SolMirMaterializedInvokeBinding){block, source,
        found->symbolic_target, found->dispatch_trait,
        found->dispatch_requirement,
        found->instance != SOL_MIR_PLAN_NONE
            ? SOL_MIR_MATERIALIZED_TARGET_INSTANCE
            : SOL_MIR_MATERIALIZED_TARGET_IMPORT,
        found->instance, found->import};
    ++builder->output->usage.bindings;
    return charge(builder, 1);
}

static bool instruction_place(const SolMirInstruction *instruction,
    SolMirPlace *place) {
    switch (instruction->kind) {
        case SOL_MIR_INST_LOAD_COPY:
        case SOL_MIR_INST_LOAD_MOVE:
        case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
            *place = instruction->as.place; return true;
        case SOL_MIR_INST_LOAD_UPDATE:
            *place = instruction->as.update_load.place; return true;
        case SOL_MIR_INST_STORE:
            *place = instruction->as.store.place; return true;
        case SOL_MIR_INST_COMPOUND_UPDATE:
            *place = instruction->as.compound_update.place; return true;
        default: return false;
    }
}

static bool copy_candidate(const SolMirPlan *plan, size_t id) {
    const SolMirPlanType *type = &plan->types[id];
    if (type->kind == SOL_IR_TYPE_INT64 || type->kind == SOL_IR_TYPE_BOOL
        || type->kind == SOL_IR_TYPE_TEXT || type->kind == SOL_IR_TYPE_UNIT
        || type->kind == SOL_IR_TYPE_NEVER || type->kind == SOL_IR_TYPE_OPTION
        || type->kind == SOL_IR_TYPE_RESULT || type->kind == SOL_IR_TYPE_TUPLE) {
        return true;
    }
    if (type->kind != SOL_IR_TYPE_NOMINAL
        || type->definition >= plan->program->ir->definition_count) return false;
    SolIrDefinitionKind kind
        = plan->program->ir->definitions[type->definition].kind;
    return kind == SOL_IR_DEFINITION_RECORD || kind == SOL_IR_DEFINITION_ENUM
        || kind == SOL_IR_DEFINITION_DISTINCT || kind == SOL_IR_DEFINITION_REFINED;
}

static bool classify_copy(Materializer *builder) {
    SolMirMaterialization *output = builder->output;
    for (size_t id = 0; id < output->type_count; ++id) {
        if (!charge(builder, 1)) return false;
        output->types[id].is_copy = copy_candidate(output->plan, id);
    }
    bool changed;
    do {
        changed = false;
        for (size_t id = 0; id < output->type_count; ++id) {
            if (!charge(builder, 1)) return false;
            SolMirMaterializedType *type = &output->types[id];
            if (!type->is_copy) continue;
            for (size_t item = 0; item < type->ownership_components.count; ++item) {
                if (!charge(builder, 1)) return false;
                size_t child = output->type_ids[
                    type->ownership_components.offset + item];
                if (!output->types[child].is_copy) {
                    type->is_copy = false;
                    changed = true;
                    break;
                }
            }
        }
    } while (changed);
    return true;
}

static SolMirMaterializedLocalId local_for(const SolMirMaterialization *output,
    const SolMirMaterializedImage *image, SolIrLocalId source) {
    for (size_t item = 0; item < image->locals.count; ++item) {
        size_t id = image->locals.offset + item;
        if (output->locals[id].source_local == source) return id;
    }
    return SOL_MIR_MATERIALIZED_NONE;
}

static SolMirMaterializedPlaceId place_for(const SolMirMaterialization *output,
    const SolMirMaterializedImage *image, SolMirPlace source) {
    SolMirMaterializedLocalId local = local_for(output, image, source.local);
    for (size_t item = 0; item < image->places.count; ++item) {
        size_t id = image->places.offset + item;
        if (output->places[id].source_place == source.source_place
            && output->places[id].local == local) return id;
    }
    return SOL_MIR_MATERIALIZED_NONE;
}

static bool translate_instruction(const SolMirMaterialization *output,
    const SolMirMaterializedImage *image, const SolMirPlanInstance *instance,
    const SolMirInstruction *source, size_t source_id,
    SolMirMaterializedInstruction *target) {
    const SolMirPlan *plan = output->plan;
    SolMirPlanContextId body = instance->contexts.offset;
    memset(target, 0, sizeof(*target));
    target->kind = source->kind;
    target->block = source->block;
    target->result = source->result == SOL_MIR_NONE
        ? SOL_MIR_MATERIALIZED_NONE : image->values.offset + source->result;
    target->type = SOL_MIR_MATERIALIZED_NONE;
    if (source->type != SOL_IR_NONE) {
        const SolMirPlanTypedUse *type = find_use(plan, instance,
            SOL_MIR_PLAN_USE_MIR_INSTRUCTION, source_id, 0, body);
        if (type == NULL) return false;
        target->type = type->type;
    }
    target->source_expression = source->source_expression;
    target->span = source->span;
    target->local = target->place = target->left = target->right
        = target->temporary = target->previous = target->pattern_scrutinee
        = SOL_MIR_MATERIALIZED_NONE;
    target->source_statement = target->match_expression = target->source_arm
        = target->source_pattern = target->source_snapshot = SOL_IR_NONE;
    target->construct_definition = target->construct_variant = SOL_IR_NONE;
    SolMirPlace mir_place;
    if (instruction_place(source, &mir_place)) {
        target->place = place_for(output, image, mir_place);
        if (target->place == SOL_MIR_MATERIALIZED_NONE) return false;
    }
    switch (source->kind) {
        case SOL_MIR_INST_CONST_INT64:
            target->integer = source->as.integer;
            break;
        case SOL_MIR_INST_CONST_BOOL:
            target->boolean = source->as.boolean;
            break;
        case SOL_MIR_INST_PARAMETER_LIVE:
        case SOL_MIR_INST_STORAGE_LIVE:
        case SOL_MIR_INST_DROP_IF_INITIALIZED:
        case SOL_MIR_INST_STORAGE_DEAD:
            target->local = local_for(output, image, source->as.local);
            if (target->local == SOL_MIR_MATERIALIZED_NONE) return false;
            break;
        case SOL_MIR_INST_LOAD_UPDATE:
            target->source_statement = source->as.update_load.statement;
            break;
        case SOL_MIR_INST_STORE:
            target->left = image->values.offset + source->as.store.value;
            break;
        case SOL_MIR_INST_UNARY:
            target->operator_kind = source->as.unary.operator_kind;
            target->left = image->values.offset + source->as.unary.operand;
            break;
        case SOL_MIR_INST_BINARY:
            target->operator_kind = source->as.binary.operator_kind;
            target->left = image->values.offset + source->as.binary.left;
            target->right = image->values.offset + source->as.binary.right;
            break;
        case SOL_MIR_INST_COMPOUND_UPDATE:
            target->operator_kind = source->as.compound_update.operator_kind;
            target->previous = image->temporaries.offset
                + source->as.compound_update.previous;
            target->right = image->values.offset + source->as.compound_update.right;
            target->source_statement = source->as.compound_update.statement;
            break;
        case SOL_MIR_INST_REGION_ENTER:
        case SOL_MIR_INST_REGION_EXIT:
            target->source_statement = source->as.region;
            break;
        case SOL_MIR_INST_TEMPORARY_INIT:
            target->temporary = image->temporaries.offset
                + source->as.temporary_init.temporary;
            target->left = image->values.offset + source->as.temporary_init.value;
            break;
        case SOL_MIR_INST_TEMPORARY_DROP:
            target->temporary = image->temporaries.offset
                + source->as.temporary_drop.temporary;
            target->preserve_depth = source->as.temporary_drop.preserve_depth;
            break;
        case SOL_MIR_INST_EXPRESSION_RESULT:
            target->left = image->values.offset + source->as.operand;
            break;
        case SOL_MIR_INST_PATTERN_TEST:
        case SOL_MIR_INST_PATTERN_VALUE:
        case SOL_MIR_INST_MATCH_ARM:
            target->match_expression = source->as.pattern.match_expression;
            target->source_arm = source->as.pattern.arm;
            target->arm_ordinal = source->as.pattern.arm_ordinal;
            target->source_pattern = source->as.pattern.pattern;
            target->pattern_scrutinee = image->temporaries.offset
                + source->as.pattern.scrutinee;
            break;
        case SOL_MIR_INST_CONSTRUCT:
            target->construct_kind = source->as.construct.kind;
            target->construct_definition = source->as.construct.definition;
            target->construct_variant = source->as.construct.variant;
            target->construct_operands = (SolMirPlanSlice){
                image->construct_operands.offset
                    + source->as.construct.operands.offset,
                source->as.construct.operands.count};
            target->source_capability_roots = source->as.construct.capability_roots;
            target->source_operation_roots = source->as.construct.operation_roots;
            break;
        case SOL_MIR_INST_CAPTURE_SNAPSHOT:
            target->source_snapshot = source->as.snapshot;
            break;
        default:
            break;
    }
    return true;
}

static bool instruction_equal(const SolMirMaterializedInstruction *a,
    const SolMirMaterializedInstruction *b) {
#define SAME(field) (a->field == b->field)
    return SAME(kind) && SAME(block) && SAME(result) && SAME(type)
        && SAME(source_expression) && SAME(span.start) && SAME(span.end)
        && SAME(integer) && SAME(boolean) && SAME(local) && SAME(place)
        && SAME(left) && SAME(right) && SAME(temporary) && SAME(previous)
        && SAME(preserve_depth) && SAME(operator_kind) && SAME(source_statement)
        && SAME(match_expression) && SAME(source_arm) && SAME(arm_ordinal)
        && SAME(source_pattern) && SAME(pattern_scrutinee)
        && SAME(source_snapshot) && SAME(construct_kind)
        && SAME(construct_definition) && SAME(construct_variant)
        && SAME(construct_operands.offset) && SAME(construct_operands.count)
        && SAME(source_capability_roots.offset)
        && SAME(source_capability_roots.count)
        && SAME(source_operation_roots.offset)
        && SAME(source_operation_roots.count);
#undef SAME
}

static bool build_scratch(Materializer *builder) {
    SolMirMaterialization *output = builder->output;
    const SolMirPlan *plan = output->plan;
    for (size_t index = 0; index < plan->demand_count; ++index) {
        if (plan->demands[index].kind == SOL_MIR_PLAN_DEMAND_HANDLER_SOURCE
            || plan->demands[index].kind
                == SOL_MIR_PLAN_DEMAND_HANDLER_PROVIDER) {
            return report(builder,
                SOL_MIR_MATERIALIZE_BUILD_UNSUPPORTED_OR_UNRESOLVED,
                "handler source/provider binding is deferred to P2.3b2");
        }
    }
    if (plan->instance_count > output->limits.max_instances) {
        return report(builder, SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
            "materialized instance limit exceeded");
    }
    size_t overlay_total = plan->typed_use_count;
    size_t invokes = 0;
    size_t signature_types = plan->type_component_count;
    size_t signature_accesses = plan->type_parameter_access_count;
    size_t local_total = 0, place_total = 0, projection_total = 0;
    size_t value_total = 0, instruction_total = 0, temporary_total = 0;
    size_t operand_total = 0, call_argument_total = 0;
    for (size_t id = 0; id < plan->instance_count; ++id) {
        const SolMirPlanInstance *instance = &plan->instances[id];
        const SolMirProgramTemplate *template = find_template(plan,
            instance->callable);
        if (template == NULL) return report(builder,
            SOL_MIR_MATERIALIZE_BUILD_UNSUPPORTED_OR_UNRESOLVED,
            "materialized instance has no MIR template");
        SolMirPlanContextId body = instance->contexts.offset;
        for (size_t use = 0; use < instance->typed_uses.count; ++use) {
            const SolMirPlanTypedUse *typed = &plan->typed_uses[
                instance->typed_uses.offset + use];
            if (typed->context != body) continue;
            if (typed->kind == SOL_MIR_PLAN_USE_LOCAL) {
                if (local_total == SIZE_MAX) return report(builder,
                    SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
                    "materialized local count overflowed");
                ++local_total;
            }
            if (typed->kind == SOL_MIR_PLAN_USE_PLACE_ROOT) {
                if (place_total == SIZE_MAX
                    || plan->program->ir->places[typed->source].projections.count
                        > SIZE_MAX - projection_total) return report(builder,
                    SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
                    "materialized place/projection count overflowed");
                ++place_total;
                projection_total += plan->program->ir->places[typed->source]
                    .projections.count;
            }
        }
        for (size_t block = 0; block < template->mir.block_count; ++block) {
            if (template->mir.blocks[block].terminator.kind
                    == SOL_MIR_TERM_INVOKE) {
                if (invokes == SIZE_MAX) return report(builder,
                    SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
                    "materialized invoke count overflowed");
                ++invokes;
            }
        }
        for (size_t instruction = 0; instruction < template->mir.instruction_count;
            ++instruction) {
            SolMirPlace place;
            if (!instruction_place(&template->mir.instructions[instruction], &place)
                || place.source_place != SOL_IR_NONE) continue;
            bool earlier = false;
            for (size_t previous = 0; previous < instruction; ++previous) {
                SolMirPlace candidate;
                earlier = earlier || (instruction_place(
                    &template->mir.instructions[previous], &candidate)
                    && candidate.source_place == SOL_IR_NONE
                    && candidate.local == place.local);
            }
            if (!earlier) {
                if (place_total == SIZE_MAX) return report(builder,
                    SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
                    "materialized place count overflowed");
                ++place_total;
            }
        }
#define ADD_TOTAL(total, count) do { if ((count) > SIZE_MAX - (total)) \
            return report(builder, SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED, \
                "materialized concrete array count overflowed"); \
        (total) += (count); } while (0)
        ADD_TOTAL(value_total, template->mir.value_count);
        ADD_TOTAL(instruction_total, template->mir.instruction_count);
        ADD_TOTAL(temporary_total, template->mir.temporary_count);
        ADD_TOTAL(operand_total, template->mir.construct_operand_count);
        ADD_TOTAL(call_argument_total, template->mir.call_argument_count);
#undef ADD_TOTAL
        if (instance->type_arguments.count > SIZE_MAX - signature_types
            || instance->parameter_types.count > SIZE_MAX - signature_types
                - instance->type_arguments.count
            || instance->parameter_accesses.count
                > SIZE_MAX - signature_accesses) {
            return report(builder, SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
                "materialized signature count overflowed");
        }
        signature_types += instance->type_arguments.count
            + instance->parameter_types.count;
        signature_accesses += instance->parameter_accesses.count;
    }
    if (overlay_total > output->limits.max_bindings
        || invokes > output->limits.max_bindings - overlay_total) {
        return report(builder, SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
            "materialized overlay/binding limit exceeded");
    }
    if (plan->context_count > SIZE_MAX - plan->type_count) return report(builder,
        SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
        "materialized concrete record count overflowed");
    size_t concrete_records = plan->type_count + plan->context_count;
#define ADD_RECORDS(count) do { if ((count) > SIZE_MAX - concrete_records) \
        return report(builder, SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED, \
            "materialized concrete record count overflowed"); \
        concrete_records += (count); } while (0)
    ADD_RECORDS(overlay_total); ADD_RECORDS(local_total); ADD_RECORDS(place_total);
    ADD_RECORDS(projection_total); ADD_RECORDS(value_total);
    ADD_RECORDS(instruction_total); ADD_RECORDS(temporary_total);
    ADD_RECORDS(operand_total);
    ADD_RECORDS(call_argument_total);
#undef ADD_RECORDS
    if (concrete_records > output->limits.max_concrete_records) return report(builder,
        SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
        "materialized concrete record limit exceeded");
    output->images = allocate_owned(builder, plan->instance_count,
        sizeof(*output->images));
    output->types = allocate_owned(builder, plan->type_count, sizeof(*output->types));
    output->type_ids = allocate_owned(builder, signature_types,
        sizeof(*output->type_ids));
    output->accesses = allocate_owned(builder, signature_accesses,
        sizeof(*output->accesses));
    output->overlays = allocate_owned(builder, overlay_total,
        sizeof(*output->overlays));
    output->contexts = allocate_owned(builder, plan->context_count,
        sizeof(*output->contexts));
    output->locals = allocate_owned(builder, local_total, sizeof(*output->locals));
    output->places = allocate_owned(builder, place_total, sizeof(*output->places));
    output->projections = allocate_owned(builder, projection_total,
        sizeof(*output->projections));
    output->values = allocate_owned(builder, value_total, sizeof(*output->values));
    output->instructions = allocate_owned(builder, instruction_total,
        sizeof(*output->instructions));
    output->temporaries = allocate_owned(builder, temporary_total,
        sizeof(*output->temporaries));
    output->construct_operands = allocate_owned(builder, operand_total,
        sizeof(*output->construct_operands));
    output->call_arguments = allocate_owned(builder, call_argument_total,
        sizeof(*output->call_arguments));
    output->invoke_bindings = allocate_owned(builder, invokes,
        sizeof(*output->invoke_bindings));
    if ((plan->instance_count != 0 && output->images == NULL)
        || (signature_types != 0 && output->type_ids == NULL)
        || (signature_accesses != 0 && output->accesses == NULL)
        || (plan->type_count != 0 && output->types == NULL)
        || (overlay_total != 0 && output->overlays == NULL)
        || (plan->context_count != 0 && output->contexts == NULL)
        || (local_total != 0 && output->locals == NULL)
        || (place_total != 0 && output->places == NULL)
        || (projection_total != 0 && output->projections == NULL)
        || (value_total != 0 && output->values == NULL)
        || (instruction_total != 0 && output->instructions == NULL)
        || (temporary_total != 0 && output->temporaries == NULL)
        || (operand_total != 0 && output->construct_operands == NULL)
        || (call_argument_total != 0 && output->call_arguments == NULL)
        || (invokes != 0 && output->invoke_bindings == NULL)) return false;
    output->image_count = output->image_capacity = plan->instance_count;
    output->type_count = output->type_capacity = plan->type_count;
    output->type_id_count = output->type_id_capacity = signature_types;
    output->access_count = output->access_capacity = signature_accesses;
    output->overlay_capacity = overlay_total;
    output->context_count = output->context_capacity = plan->context_count;
    output->local_capacity = local_total;
    output->place_capacity = place_total;
    output->projection_capacity = projection_total;
    output->value_capacity = value_total;
    output->instruction_capacity = instruction_total;
    output->temporary_capacity = temporary_total;
    output->construct_operand_capacity = operand_total;
    output->call_argument_capacity = call_argument_total;
    output->invoke_binding_capacity = invokes;
    size_t type_at = 0;
    size_t access_at = 0;
    for (size_t id = 0; id < plan->type_count; ++id) {
        const SolMirPlanType *source = &plan->types[id];
        SolMirMaterializedType *target = &output->types[id];
        *target = (SolMirMaterializedType){source->kind, source->definition,
            {type_at, source->argument_count},
            {type_at + source->argument_count, source->parameter_count},
            {access_at, source->parameter_count}, source->result, source->effects,
            {type_at + source->argument_count + source->parameter_count,
                source->ownership_component_count}, false};
        size_t count = source->argument_count + source->parameter_count
            + source->ownership_component_count;
        if (count != 0) memcpy(output->type_ids + type_at,
            plan->type_components + source->argument_offset,
            count * sizeof(*output->type_ids));
        if (source->parameter_count != 0) memcpy(output->accesses + access_at,
            plan->type_parameter_accesses + source->parameter_access_offset,
            source->parameter_count * sizeof(*output->accesses));
        type_at += count;
        access_at += source->parameter_count;
    }
    if (plan->context_count != 0) memcpy(output->contexts, plan->contexts,
        plan->context_count * sizeof(*output->contexts));
    if (!classify_copy(builder)) return false;
    for (size_t id = 0; id < plan->instance_count; ++id) {
        const SolMirPlanInstance *instance = &plan->instances[id];
        const SolMirProgramTemplate *template = find_template(plan,
            instance->callable);
        SolMirMaterializedImage *image = &output->images[id];
        image->instance = id;
        image->source_callable = instance->callable;
        image->receiver = instance->receiver;
        image->result = instance->result;
        image->effects = instance->effects;
        image->contexts = instance->contexts;
        image->type_arguments = (SolMirPlanSlice){type_at,
            instance->type_arguments.count};
        if (instance->type_arguments.count != 0) memcpy(output->type_ids + type_at,
            plan->instance_type_ids + instance->type_arguments.offset,
            instance->type_arguments.count * sizeof(*output->type_ids));
        type_at += instance->type_arguments.count;
        image->parameter_types = (SolMirPlanSlice){type_at,
            instance->parameter_types.count};
        if (instance->parameter_types.count != 0) memcpy(output->type_ids + type_at,
            plan->instance_type_ids + instance->parameter_types.offset,
            instance->parameter_types.count * sizeof(*output->type_ids));
        type_at += instance->parameter_types.count;
        image->parameter_accesses = (SolMirPlanSlice){access_at,
            instance->parameter_accesses.count};
        if (instance->parameter_accesses.count != 0) memcpy(output->accesses + access_at,
            plan->instance_accesses + instance->parameter_accesses.offset,
            instance->parameter_accesses.count * sizeof(*output->accesses));
        access_at += instance->parameter_accesses.count;
        image->overlays.offset = output->overlay_count;
        for (size_t use = 0; use < instance->typed_uses.count; ++use) {
            const SolMirPlanTypedUse *source
                = &plan->typed_uses[instance->typed_uses.offset + use];
            output->overlays[output->overlay_count++]
                = (SolMirMaterializedTypeOverlay){source->kind, source->source,
                    source->ordinal, source->context, source->type, source->access};
        }
        image->overlays.count = output->overlay_count - image->overlays.offset;
        SolMirPlanContextId body = instance->contexts.offset;
        image->locals.offset = output->local_count;
        const SolIrCallable *callable = &plan->program->ir->callables[
            instance->callable];
        for (size_t use = 0; use < instance->typed_uses.count; ++use) {
            const SolMirPlanTypedUse *typed = &plan->typed_uses[
                instance->typed_uses.offset + use];
            if (typed->context != body || typed->kind != SOL_MIR_PLAN_USE_LOCAL) {
                continue;
            }
            SolMirMaterializedLocalKind kind = SOL_MIR_MATERIALIZED_LOCAL_BODY;
            size_t ordinal = SOL_MIR_MATERIALIZED_NONE;
            if (typed->source == callable->receiver) {
                kind = SOL_MIR_MATERIALIZED_LOCAL_RECEIVER;
                ordinal = 0;
            }
            for (size_t parameter = 0; parameter < callable->parameters.count;
                ++parameter) {
                if (plan->program->ir->roots[callable->parameters.offset + parameter]
                        == typed->source) {
                    kind = SOL_MIR_MATERIALIZED_LOCAL_PARAMETER;
                    ordinal = parameter;
                }
            }
            output->locals[output->local_count++] = (SolMirMaterializedLocal){id,
                typed->source, typed->type, typed->access, kind, ordinal};
        }
        image->locals.count = output->local_count - image->locals.offset;
        image->places.offset = output->place_count;
        for (size_t use = 0; use < instance->typed_uses.count; ++use) {
            const SolMirPlanTypedUse *typed = &plan->typed_uses[
                instance->typed_uses.offset + use];
            if (typed->context != body
                || typed->kind != SOL_MIR_PLAN_USE_PLACE_ROOT) continue;
            const SolIrPlace *source_place = &plan->program->ir->places[typed->source];
            SolMirMaterializedLocalId local = local_for(output, image,
                source_place->local);
            const SolMirPlanTypedUse *final = find_use(plan, instance,
                SOL_MIR_PLAN_USE_PLACE_FINAL, typed->source, 0, body);
            if (local == SOL_MIR_MATERIALIZED_NONE || final == NULL) return report(builder,
                SOL_MIR_MATERIALIZE_BUILD_INVALID_PLAN,
                "source place has no concrete local or final type");
            SolMirMaterializedPlace *place = &output->places[output->place_count++];
            *place = (SolMirMaterializedPlace){id, typed->source, local, typed->type,
                {output->projection_count, source_place->projections.count},
                final->type};
            for (size_t projection = 0; projection < source_place->projections.count;
                ++projection) {
                size_t source_id = source_place->projections.offset + projection;
                const SolIrProjection *source
                    = &plan->program->ir->projections[source_id];
                const SolMirPlanTypedUse *projection_type = find_use(plan, instance,
                    SOL_MIR_PLAN_USE_PLACE_PROJECTION, typed->source, projection,
                    body);
                if (projection_type == NULL) return report(builder,
                    SOL_MIR_MATERIALIZE_BUILD_INVALID_PLAN,
                    "source projection has no concrete type");
                output->projections[output->projection_count++]
                    = (SolMirMaterializedProjection){source->kind,
                        projection_type->type, source->field, source->ordinal,
                        source_id};
            }
        }
        for (size_t instruction = 0; instruction < template->mir.instruction_count;
            ++instruction) {
            SolMirPlace source;
            if (!instruction_place(&template->mir.instructions[instruction], &source)
                || source.source_place != SOL_IR_NONE
                || place_for(output, image, source) != SOL_MIR_MATERIALIZED_NONE) {
                continue;
            }
            SolMirMaterializedLocalId local = local_for(output, image, source.local);
            const SolMirPlanTypedUse *local_type = find_use(plan, instance,
                SOL_MIR_PLAN_USE_LOCAL, source.local, 0, body);
            if (local == SOL_MIR_MATERIALIZED_NONE || local_type == NULL) return report(
                builder, SOL_MIR_MATERIALIZE_BUILD_INVALID_PLAN,
                "synthetic place has no concrete local");
            output->places[output->place_count++] = (SolMirMaterializedPlace){id,
                SOL_IR_NONE, local, local_type->type,
                {output->projection_count, 0}, local_type->type};
        }
        image->places.count = output->place_count - image->places.offset;
        image->values = (SolMirPlanSlice){output->value_count,
            template->mir.value_count};
        image->instructions = (SolMirPlanSlice){output->instruction_count,
            template->mir.instruction_count};
        image->temporaries = (SolMirPlanSlice){output->temporary_count,
            template->mir.temporary_count};
        image->construct_operands = (SolMirPlanSlice){output->construct_operand_count,
            template->mir.construct_operand_count};
        image->call_arguments = (SolMirPlanSlice){output->call_argument_count,
            template->mir.call_argument_count};
        for (size_t value = 0; value < template->mir.value_count; ++value) {
            const SolMirValue *source = &template->mir.values[value];
            const SolMirPlanTypedUse *type = find_use(plan, instance,
                SOL_MIR_PLAN_USE_MIR_VALUE, value, 0, body);
            if (type == NULL) return report(builder,
                SOL_MIR_MATERIALIZE_BUILD_INVALID_PLAN,
                "MIR value has no concrete type");
            output->values[output->value_count++] = (SolMirMaterializedValue){
                source->kind, type->type, source->block, source->definition,
                source->kind == SOL_MIR_VALUE_INSTRUCTION
                    ? image->instructions.offset + source->definition
                    : SOL_MIR_MATERIALIZED_NONE,
                source->source_expression, source->span};
        }
        for (size_t temporary = 0; temporary < template->mir.temporary_count;
            ++temporary) {
            const SolMirTemporary *source = &template->mir.temporaries[temporary];
            const SolMirPlanTypedUse *type = find_use(plan, instance,
                SOL_MIR_PLAN_USE_MIR_TEMPORARY, temporary, 0, body);
            if (type == NULL) return report(builder,
                SOL_MIR_MATERIALIZE_BUILD_INVALID_PLAN,
                "MIR temporary has no concrete type");
            output->temporaries[output->temporary_count++]
                = (SolMirMaterializedTemporary){type->type,
                    source->source_expression, source->span};
        }
        for (size_t operand = 0; operand < template->mir.construct_operand_count;
            ++operand) {
            const SolMirConstructOperand *source
                = &template->mir.construct_operands[operand];
            const SolMirPlanTypedUse *type = find_use(plan, instance,
                SOL_MIR_PLAN_USE_CONSTRUCT_OPERAND, operand, source->formal, body);
            if (type == NULL || source->temporary >= template->mir.temporary_count) {
                return report(
                builder, SOL_MIR_MATERIALIZE_BUILD_INVALID_PLAN,
                "construct operand has no concrete type or temporary");
            }
            SolMirMaterializedTemporaryId temporary
                = image->temporaries.offset + source->temporary;
            output->construct_operands[output->construct_operand_count++]
                = (SolMirMaterializedConstructOperand){source->formal,
                    source->source_expression, type->type, temporary};
        }
        for (size_t instruction = 0; instruction < template->mir.instruction_count;
            ++instruction) {
            const SolMirInstruction *source = &template->mir.instructions[instruction];
            SolMirMaterializedInstruction target;
            if (!translate_instruction(output, image, instance, source,
                    instruction, &target)) return report(builder,
                SOL_MIR_MATERIALIZE_BUILD_INVALID_PLAN,
                "MIR instruction specialization failed");
            output->instructions[output->instruction_count++] = target;
        }
        for (size_t argument = 0; argument < template->mir.call_argument_count;
            ++argument) {
            const SolMirCallArgument *source
                = &template->mir.call_arguments[argument];
            SolMirMaterializedCallArgument target = {source->formal,
                source->access, source->source_expression,
                SOL_MIR_MATERIALIZED_NONE, SOL_MIR_MATERIALIZED_NONE,
                SOL_MIR_MATERIALIZED_NONE};
            if (source->access == SOL_ACCESS_OWNED) {
                const SolMirPlanTypedUse *type = find_use(plan, instance,
                    SOL_MIR_PLAN_USE_MIR_TEMPORARY, source->temporary, 0, body);
                if (type == NULL) return report(builder,
                    SOL_MIR_MATERIALIZE_BUILD_INVALID_PLAN,
                    "owned call argument has no concrete temporary type");
                target.type = type->type;
                target.temporary = image->temporaries.offset + source->temporary;
            } else {
                const SolMirPlanTypedUse *type = find_use(plan, instance,
                    SOL_MIR_PLAN_USE_PLACE_FINAL, source->place, 0, body);
                if (type == NULL || source->place >= plan->program->ir->place_count) {
                    return report(builder, SOL_MIR_MATERIALIZE_BUILD_INVALID_PLAN,
                        "borrowed call argument has no concrete place type");
                }
                const SolIrPlace *place = &plan->program->ir->places[source->place];
                target.type = type->type;
                target.place = place_for(output, image,
                    (SolMirPlace){place->local, source->place});
                if (target.place == SOL_MIR_MATERIALIZED_NONE) return report(builder,
                    SOL_MIR_MATERIALIZE_BUILD_INVALID_PLAN,
                    "borrowed call argument place was not materialized");
            }
            output->call_arguments[output->call_argument_count++] = target;
        }
        image->invokes.offset = output->invoke_binding_count;
        if (!clone_mir(builder, &template->mir, &image->topology)) return false;
        for (size_t block = 0; block < image->topology.block_count; ++block) {
            if (image->topology.blocks[block].terminator.kind == SOL_MIR_TERM_INVOKE
                && !append_binding(builder, id, instance,
                    &image->topology, block)) return false;
        }
        image->invokes.count = output->invoke_binding_count
            - image->invokes.offset;
        if (!charge(builder, 1 + image->overlays.count)) return false;
    }
    output->usage.instances = output->image_count;
    output->usage.bindings += output->overlay_count;
    output->usage.concrete_records = concrete_records;
    if (!charge(builder, concrete_records)) return false;
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
        return diagnostics->allocation_failed
            ? SOL_MIR_MATERIALIZE_BUILD_ALLOCATION_FAILED
            : SOL_MIR_MATERIALIZE_BUILD_INVALID_PLAN;
    }
    SolMirMaterialization scratch;
    sol_mir_materialization_init(&scratch);
    scratch.plan = request->plan;
    scratch.limits = request->limits == NULL || limits_zero(*request->limits)
        ? sol_mir_materialize_default_limits() : *request->limits;
    Materializer builder = {&scratch, diagnostics,
        SOL_MIR_MATERIALIZE_BUILD_INTERNAL_FAILED};
    if (build_scratch(&builder)) {
        builder.outcome = SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED;
        if (sol_mir_materialization_validate(&scratch, diagnostics)) {
            *output = scratch;
            return builder.outcome;
        }
        builder.outcome = diagnostics->allocation_failed
            ? SOL_MIR_MATERIALIZE_BUILD_ALLOCATION_FAILED
            : SOL_MIR_MATERIALIZE_BUILD_INTERNAL_FAILED;
    }
    sol_mir_materialization_free(&scratch);
    return builder.outcome;
}

typedef struct { uintptr_t start; uintptr_t end; } OwnedRange;

static bool validation_error(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) sol_diagnostics_add(diagnostics,
        "SOL-MIR-MATERIALIZE-002", SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    return false;
}

static bool canonical(size_t count, size_t capacity, const void *pointer) {
    return count == capacity && ((count == 0) == (pointer == NULL));
}

static bool add_range(OwnedRange *ranges, size_t *range_count,
    const void *pointer, size_t count, size_t size) {
    if (count == 0) return true;
    if (pointer == NULL || size == 0 || count > SIZE_MAX / size) return false;
    uintptr_t start = (uintptr_t)pointer;
    size_t bytes = count * size;
    if (bytes > UINTPTR_MAX - start) return false;
    uintptr_t end = start + bytes;
    for (size_t index = 0; index < *range_count; ++index) {
        if (start < ranges[index].end && ranges[index].start < end) return false;
    }
    ranges[(*range_count)++] = (OwnedRange){start, end};
    return true;
}

static bool bytes_equal(const void *a, const void *b, size_t count, size_t size) {
    return count == 0 || memcmp(a, b, count * size) == 0;
}

static bool topology_authentic(const SolMir *mir, const SolMir *source) {
    return mir->callable == source->callable
        && mir->generic_parameters.offset == source->generic_parameters.offset
        && mir->generic_parameters.count == source->generic_parameters.count
        && mir->effect_parameters.offset == source->effect_parameters.offset
        && mir->effect_parameters.count == source->effect_parameters.count
        && mir->entry == source->entry && mir->contract_body == source->contract_body
        && mir->contract_epilogue == source->contract_epilogue
#define SAME(field, count, capacity) && mir->count == source->count \
        && mir->capacity == mir->count \
        && (mir->count == 0 || mir->field != source->field) \
        && bytes_equal(mir->field, source->field, mir->count, sizeof(*mir->field))
        SAME(blocks, block_count, block_capacity)
        SAME(instructions, instruction_count, instruction_capacity)
        SAME(values, value_count, value_capacity)
        SAME(parameter_values, parameter_value_count, parameter_value_capacity)
        SAME(edge_values, edge_value_count, edge_value_capacity)
        SAME(call_arguments, call_argument_count, call_argument_capacity)
        SAME(loops, loop_count, loop_capacity)
        SAME(construct_operands, construct_operand_count,
            construct_operand_capacity)
        SAME(temporaries, temporary_count, temporary_capacity)
#undef SAME
        ;
}

static bool slice_ok(SolMirPlanSlice slice, size_t count) {
    return slice.offset <= count && slice.count <= count - slice.offset;
}

static bool add_product(size_t *total, size_t count, size_t size) {
    if (size == 0 || count > SIZE_MAX / size) return false;
    size_t bytes = count * size;
    if (bytes > SIZE_MAX - *total) return false;
    *total += bytes;
    return true;
}

bool sol_mir_materialization_validate(const SolMirMaterialization *owner,
    SolDiagnostics *diagnostics) {
    if (owner == NULL || owner->plan == NULL || !limits_complete(owner->limits)
        || owner->image_count != owner->plan->instance_count
        || owner->usage.instances != owner->image_count
        || owner->usage.cfg_items > owner->limits.max_cfg_items
        || owner->usage.bindings > owner->limits.max_bindings
        || owner->usage.concrete_records > owner->limits.max_concrete_records
        || owner->usage.owned_bytes > owner->limits.max_owned_bytes
        || owner->usage.materialization_work
            > owner->limits.max_materialization_work
        || !canonical(owner->image_count, owner->image_capacity, owner->images)
        || !canonical(owner->type_count, owner->type_capacity, owner->types)
        || !canonical(owner->type_id_count, owner->type_id_capacity, owner->type_ids)
        || !canonical(owner->access_count, owner->access_capacity, owner->accesses)
        || !canonical(owner->overlay_count, owner->overlay_capacity, owner->overlays)
        || !canonical(owner->context_count, owner->context_capacity, owner->contexts)
        || !canonical(owner->local_count, owner->local_capacity, owner->locals)
        || !canonical(owner->place_count, owner->place_capacity, owner->places)
        || !canonical(owner->projection_count, owner->projection_capacity,
            owner->projections)
        || !canonical(owner->value_count, owner->value_capacity, owner->values)
        || !canonical(owner->instruction_count, owner->instruction_capacity,
            owner->instructions)
        || !canonical(owner->temporary_count, owner->temporary_capacity,
            owner->temporaries)
        || !canonical(owner->construct_operand_count,
            owner->construct_operand_capacity, owner->construct_operands)
        || !canonical(owner->call_argument_count, owner->call_argument_capacity,
            owner->call_arguments)
        || !canonical(owner->invoke_binding_count,
            owner->invoke_binding_capacity, owner->invoke_bindings)
        || !sol_mir_plan_validate(owner->plan, diagnostics)) {
        return validation_error(diagnostics,
            "materialized MIR owner header or borrowed plan is invalid");
    }
    size_t max_ranges = 15;
    size_t templates = owner->plan->program->template_count;
    if (owner->image_count > (SIZE_MAX - max_ranges) / 9) {
        return validation_error(diagnostics, "materialized MIR range count overflowed");
    }
    max_ranges += owner->image_count * 9;
    if (templates > (SIZE_MAX - max_ranges) / 9) {
        return validation_error(diagnostics, "materialized MIR range count overflowed");
    }
    max_ranges += templates * 9;
    if (max_ranges > SIZE_MAX - 20
        || owner->plan->effect_atom_count > SIZE_MAX - max_ranges - 20) {
        return validation_error(diagnostics, "materialized MIR range count overflowed");
    }
    max_ranges += 20 + owner->plan->effect_atom_count;
    const SolIr *ir = owner->plan->program->ir;
#define ADD_RANGE_COUNT(count) do { \
    if ((count) > SIZE_MAX - max_ranges) return validation_error(diagnostics, \
        "materialized MIR range count overflowed"); \
    max_ranges += (count); \
} while (0)
    ADD_RANGE_COUNT(32);
    ADD_RANGE_COUNT(ir->definition_count);
    ADD_RANGE_COUNT(ir->callable_count);
    ADD_RANGE_COUNT(ir->local_count);
    ADD_RANGE_COUNT(ir->field_count);
    ADD_RANGE_COUNT(ir->variant_count);
    ADD_RANGE_COUNT(ir->expression_count);
    ADD_RANGE_COUNT(ir->statement_count);
    ADD_RANGE_COUNT(ir->effect_count);
    ADD_RANGE_COUNT(ir->generic_parameter_count);
    ADD_RANGE_COUNT(ir->effect_parameter_count);
    ADD_RANGE_COUNT(ir->file_count);
#undef ADD_RANGE_COUNT
    OwnedRange *ranges = calloc(max_ranges, sizeof(*ranges));
    if (ranges == NULL) return validation_error(diagnostics,
        "allocation failed while validating materialized MIR ranges");
    size_t range_count = 0;
#define RANGE(pointer, count, type) if (!add_range(ranges, &range_count, \
        (pointer), (count), sizeof(type))) goto malformed
    const SolMirPlan *plan = owner->plan;
    RANGE(plan->types, plan->type_capacity, *plan->types);
    RANGE(plan->type_components, plan->type_component_capacity,
        *plan->type_components);
    RANGE(plan->type_parameter_accesses, plan->type_parameter_access_capacity,
        *plan->type_parameter_accesses);
    RANGE(plan->effect_atoms, plan->effect_atom_capacity, *plan->effect_atoms);
    RANGE(plan->effect_rows, plan->effect_row_capacity, *plan->effect_rows);
    RANGE(plan->effect_row_atoms, plan->effect_row_atom_capacity,
        *plan->effect_row_atoms);
    RANGE(plan->instances, plan->instance_capacity, *plan->instances);
    RANGE(plan->instance_type_ids, plan->instance_type_id_capacity,
        *plan->instance_type_ids);
    RANGE(plan->instance_accesses, plan->instance_access_capacity,
        *plan->instance_accesses);
    RANGE(plan->dictionary_entries, plan->dictionary_entry_capacity,
        *plan->dictionary_entries);
    RANGE(plan->imports, plan->import_capacity, *plan->imports);
    RANGE(plan->typed_uses, plan->typed_use_capacity, *plan->typed_uses);
    RANGE(plan->contexts, plan->context_capacity, *plan->contexts);
    RANGE(plan->demands, plan->demand_capacity, *plan->demands);
    for (size_t id = 0; id < plan->effect_atom_count; ++id) {
        if (plan->effect_atoms[id].length == SIZE_MAX) goto malformed;
        RANGE(plan->effect_atoms[id].name, plan->effect_atoms[id].length + 1,
            char);
    }
    const SolMirProgram *program = plan->program;
    RANGE(program->roots, program->root_count, *program->roots);
    RANGE(program->approved_imports, program->approved_import_count,
        *program->approved_imports);
    RANGE(program->templates, program->template_count, *program->templates);
    RANGE(program->imports, program->import_count, *program->imports);
    RANGE(program->specializations, program->specialization_count,
        *program->specializations);
    RANGE(program->references, program->reference_count, *program->references);
#define IR_RANGE(field, count) RANGE(ir->field, ir->count, *ir->field)
    RANGE(ir->source_path, strlen(ir->source_path) + 1, char);
    RANGE(ir->source_bytes, ir->source_length + 1, char);
    IR_RANGE(types, type_count);
    IR_RANGE(type_ids, type_id_count);
    IR_RANGE(accesses, access_count);
    IR_RANGE(definitions, definition_count);
    IR_RANGE(callables, callable_count);
    IR_RANGE(members, member_count);
    IR_RANGE(evidence, evidence_count);
    IR_RANGE(locals, local_count);
    IR_RANGE(fields, field_count);
    IR_RANGE(variants, variant_count);
    IR_RANGE(expressions, expression_count);
    IR_RANGE(places, place_count);
    IR_RANGE(projections, projection_count);
    IR_RANGE(statements, statement_count);
    IR_RANGE(statement_ids, statement_id_count);
    IR_RANGE(arms, arm_count);
    IR_RANGE(arm_ids, arm_id_count);
    IR_RANGE(patterns, pattern_count);
    IR_RANGE(pattern_children, pattern_child_count);
    IR_RANGE(operands, operand_count);
    IR_RANGE(roots, root_count);
    IR_RANGE(cleanup_locals, cleanup_local_count);
    IR_RANGE(effects, effect_count);
    IR_RANGE(generic_parameters, generic_parameter_count);
    IR_RANGE(effect_parameters, effect_parameter_count);
    IR_RANGE(obligations, obligation_count);
    IR_RANGE(snapshots, snapshot_count);
    IR_RANGE(loop_obligations, loop_obligation_count);
    IR_RANGE(unreachable_obligations, unreachable_obligation_count);
    IR_RANGE(files, file_count);
#undef IR_RANGE
#define STRING_RANGE(pointer) do { \
    const char *string_range_value = (pointer); \
    if (string_range_value != NULL) \
        RANGE(string_range_value, strlen(string_range_value) + 1, char); \
} while (0)
    for (size_t id = 0; id < ir->definition_count; ++id) {
        STRING_RANGE(ir->definitions[id].name);
    }
    for (size_t id = 0; id < ir->callable_count; ++id) {
        STRING_RANGE(ir->callables[id].name);
    }
    for (size_t id = 0; id < ir->local_count; ++id) {
        STRING_RANGE(ir->locals[id].name);
    }
    for (size_t id = 0; id < ir->field_count; ++id) {
        STRING_RANGE(ir->fields[id].name);
    }
    for (size_t id = 0; id < ir->variant_count; ++id) {
        STRING_RANGE(ir->variants[id].name);
    }
    for (size_t id = 0; id < ir->expression_count; ++id) {
        const SolIrExpression *expression = &ir->expressions[id];
        if (expression->kind == SOL_IR_EXPR_STRING) {
            STRING_RANGE(expression->as.string);
        } else if (expression->kind == SOL_IR_EXPR_HANDLE) {
            STRING_RANGE(expression->as.handler.effect_name);
        }
    }
    for (size_t id = 0; id < ir->statement_count; ++id) {
        STRING_RANGE(ir->statements[id].region_label);
    }
    for (size_t id = 0; id < ir->effect_count; ++id) {
        STRING_RANGE(ir->effects[id].name);
    }
    for (size_t id = 0; id < ir->generic_parameter_count; ++id) {
        STRING_RANGE(ir->generic_parameters[id].name);
    }
    for (size_t id = 0; id < ir->effect_parameter_count; ++id) {
        STRING_RANGE(ir->effect_parameters[id].name);
    }
    for (size_t id = 0; id < ir->file_count; ++id) {
        STRING_RANGE(ir->files[id].path);
    }
#undef STRING_RANGE
    for (size_t id = 0; id < templates; ++id) {
        const SolMir *borrowed = &owner->plan->program->templates[id].mir;
#define BORROWED_RANGE(field, capacity) RANGE(borrowed->field, \
            borrowed->capacity, *borrowed->field)
        BORROWED_RANGE(blocks, block_capacity);
        BORROWED_RANGE(instructions, instruction_capacity);
        BORROWED_RANGE(values, value_capacity);
        BORROWED_RANGE(parameter_values, parameter_value_capacity);
        BORROWED_RANGE(edge_values, edge_value_capacity);
        BORROWED_RANGE(call_arguments, call_argument_capacity);
        BORROWED_RANGE(loops, loop_capacity);
        BORROWED_RANGE(construct_operands, construct_operand_capacity);
        BORROWED_RANGE(temporaries, temporary_capacity);
#undef BORROWED_RANGE
    }
    RANGE(owner->images, owner->image_capacity, *owner->images);
    RANGE(owner->types, owner->type_capacity, *owner->types);
    RANGE(owner->type_ids, owner->type_id_capacity, *owner->type_ids);
    RANGE(owner->accesses, owner->access_capacity, *owner->accesses);
    RANGE(owner->overlays, owner->overlay_capacity, *owner->overlays);
    RANGE(owner->contexts, owner->context_capacity, *owner->contexts);
    RANGE(owner->locals, owner->local_capacity, *owner->locals);
    RANGE(owner->places, owner->place_capacity, *owner->places);
    RANGE(owner->projections, owner->projection_capacity, *owner->projections);
    RANGE(owner->values, owner->value_capacity, *owner->values);
    RANGE(owner->instructions, owner->instruction_capacity, *owner->instructions);
    RANGE(owner->temporaries, owner->temporary_capacity, *owner->temporaries);
    RANGE(owner->construct_operands, owner->construct_operand_capacity,
        *owner->construct_operands);
    RANGE(owner->call_arguments, owner->call_argument_capacity,
        *owner->call_arguments);
    RANGE(owner->invoke_bindings, owner->invoke_binding_capacity,
        *owner->invoke_bindings);
    size_t expected_types = 0, expected_accesses = 0, expected_overlays = 0;
    size_t expected_invokes = 0, expected_cfg = 0, expected_bindings = 0;
    size_t expected_locals = 0, expected_places = 0, expected_projections = 0;
    size_t expected_values = 0, expected_instructions = 0;
    size_t expected_temporaries = 0, expected_operands = 0;
    size_t expected_call_arguments = 0;
    size_t expected_bytes = 0;
    if (!add_product(&expected_bytes, owner->image_capacity,
            sizeof(*owner->images))
        || !add_product(&expected_bytes, owner->type_capacity,
            sizeof(*owner->types))
        || !add_product(&expected_bytes, owner->type_id_capacity,
            sizeof(*owner->type_ids))
        || !add_product(&expected_bytes, owner->access_capacity,
            sizeof(*owner->accesses))
        || !add_product(&expected_bytes, owner->overlay_capacity,
            sizeof(*owner->overlays))
        || !add_product(&expected_bytes, owner->context_capacity,
            sizeof(*owner->contexts))
        || !add_product(&expected_bytes, owner->local_capacity,
            sizeof(*owner->locals))
        || !add_product(&expected_bytes, owner->place_capacity,
            sizeof(*owner->places))
        || !add_product(&expected_bytes, owner->projection_capacity,
            sizeof(*owner->projections))
        || !add_product(&expected_bytes, owner->value_capacity,
            sizeof(*owner->values))
        || !add_product(&expected_bytes, owner->instruction_capacity,
            sizeof(*owner->instructions))
        || !add_product(&expected_bytes, owner->temporary_capacity,
            sizeof(*owner->temporaries))
        || !add_product(&expected_bytes, owner->construct_operand_capacity,
            sizeof(*owner->construct_operands))
        || !add_product(&expected_bytes, owner->call_argument_capacity,
            sizeof(*owner->call_arguments))
        || !add_product(&expected_bytes, owner->invoke_binding_capacity,
            sizeof(*owner->invoke_bindings))) goto malformed;
    if (owner->type_count != plan->type_count
        || owner->context_count != plan->context_count) goto malformed;
    for (size_t id = 0; id < owner->type_count; ++id) {
        const SolMirPlanType *source = &plan->types[id];
        const SolMirMaterializedType *type = &owner->types[id];
        if (type->kind != source->kind || type->definition != source->definition
            || type->arguments.offset != expected_types
            || type->arguments.count != source->argument_count
            || type->parameters.offset != expected_types + source->argument_count
            || type->parameters.count != source->parameter_count
            || type->parameter_accesses.offset != expected_accesses
            || type->parameter_accesses.count != source->parameter_count
            || type->result != source->result || type->effects != source->effects
            || type->ownership_components.offset != expected_types
                + source->argument_count + source->parameter_count
            || type->ownership_components.count
                != source->ownership_component_count) goto malformed;
        size_t count = source->argument_count + source->parameter_count
            + source->ownership_component_count;
        if (!bytes_equal(owner->type_ids + expected_types,
                plan->type_components + source->argument_offset, count,
                sizeof(*owner->type_ids))
            || !bytes_equal(owner->accesses + expected_accesses,
                plan->type_parameter_accesses + source->parameter_access_offset,
                source->parameter_count, sizeof(*owner->accesses))) goto malformed;
        expected_types += count;
        expected_accesses += source->parameter_count;
    }
    if (!bytes_equal(owner->contexts, plan->contexts, plan->context_count,
            sizeof(*owner->contexts))) goto malformed;
    bool *copies = calloc(owner->type_count, sizeof(*copies));
    if (owner->type_count != 0 && copies == NULL) goto malformed;
    size_t copy_work = owner->type_count;
    for (size_t id = 0; id < owner->type_count; ++id) {
        copies[id] = copy_candidate(plan, id);
    }
    bool copy_changed;
    do {
        copy_changed = false;
        for (size_t id = 0; id < owner->type_count; ++id) {
            if (copy_work == SIZE_MAX) { free(copies); goto malformed; }
            ++copy_work;
            if (!copies[id]) continue;
            const SolMirMaterializedType *type = &owner->types[id];
            for (size_t item = 0; item < type->ownership_components.count; ++item) {
                if (copy_work == SIZE_MAX) { free(copies); goto malformed; }
                ++copy_work;
                size_t child = owner->type_ids[type->ownership_components.offset + item];
                if (child >= owner->type_count) { free(copies); goto malformed; }
                if (!copies[child]) {
                    copies[id] = false;
                    copy_changed = true;
                    break;
                }
            }
        }
    } while (copy_changed);
    for (size_t id = 0; id < owner->type_count; ++id) {
        if (owner->types[id].is_copy != copies[id]) { free(copies); goto malformed; }
    }
    free(copies);
    for (size_t id = 0; id < owner->image_count; ++id) {
        const SolMirMaterializedImage *image = &owner->images[id];
        const SolMirPlanInstance *instance = &owner->plan->instances[id];
        const SolMirProgramTemplate *template = find_template(owner->plan,
            instance->callable);
        if (image->instance != id || image->source_callable != instance->callable
            || image->receiver != instance->receiver || image->result != instance->result
            || image->effects != instance->effects || template == NULL
            || image->contexts.offset != instance->contexts.offset
            || image->contexts.count != instance->contexts.count
            || image->type_arguments.offset != expected_types
            || image->type_arguments.count != instance->type_arguments.count
            || !slice_ok(image->type_arguments, owner->type_id_count)) goto malformed;
        for (size_t item = 0; item < image->type_arguments.count; ++item) {
            if (owner->type_ids[image->type_arguments.offset + item]
                != owner->plan->instance_type_ids[
                    instance->type_arguments.offset + item]) goto malformed;
        }
        expected_types += image->type_arguments.count;
        if (image->parameter_types.offset != expected_types
            || image->parameter_types.count != instance->parameter_types.count
            || !slice_ok(image->parameter_types, owner->type_id_count)) goto malformed;
        for (size_t item = 0; item < image->parameter_types.count; ++item) {
            if (owner->type_ids[image->parameter_types.offset + item]
                != owner->plan->instance_type_ids[
                    instance->parameter_types.offset + item]) goto malformed;
        }
        expected_types += image->parameter_types.count;
        if (image->parameter_accesses.offset != expected_accesses
            || image->parameter_accesses.count
                != instance->parameter_accesses.count
            || !slice_ok(image->parameter_accesses, owner->access_count)) goto malformed;
        for (size_t item = 0; item < image->parameter_accesses.count; ++item) {
            if (owner->accesses[image->parameter_accesses.offset + item]
                != owner->plan->instance_accesses[
                    instance->parameter_accesses.offset + item]) goto malformed;
        }
        expected_accesses += image->parameter_accesses.count;
        if (image->overlays.offset != expected_overlays
            || !slice_ok(image->overlays, owner->overlay_count)) goto malformed;
        size_t use_at = 0;
        for (size_t use = 0; use < instance->typed_uses.count; ++use) {
            const SolMirPlanTypedUse *source = &owner->plan->typed_uses[
                instance->typed_uses.offset + use];
            if (use_at >= image->overlays.count) goto malformed;
            const SolMirMaterializedTypeOverlay *overlay
                = &owner->overlays[image->overlays.offset + use_at++];
            if (overlay->kind != source->kind || overlay->source != source->source
                || overlay->ordinal != source->ordinal || overlay->type != source->type
                || overlay->context != source->context
                || overlay->access != source->access
                || overlay->type >= owner->plan->type_count) goto malformed;
        }
        if (use_at != image->overlays.count) goto malformed;
        expected_overlays += image->overlays.count;
        if (!topology_authentic(&image->topology, &template->mir)
            || !sol_mir_validate(owner->plan->program->ir,
                &image->topology, diagnostics)) goto malformed;
#define MIR_RANGE(field, capacity) RANGE(image->topology.field, \
            image->topology.capacity, *image->topology.field)
        MIR_RANGE(blocks, block_capacity);
        MIR_RANGE(instructions, instruction_capacity);
        MIR_RANGE(values, value_capacity);
        MIR_RANGE(parameter_values, parameter_value_capacity);
        MIR_RANGE(edge_values, edge_value_capacity);
        MIR_RANGE(call_arguments, call_argument_capacity);
        MIR_RANGE(loops, loop_capacity);
        MIR_RANGE(construct_operands, construct_operand_capacity);
        MIR_RANGE(temporaries, temporary_capacity);
#undef MIR_RANGE
        if (!add_product(&expected_bytes, image->topology.block_capacity,
                sizeof(*image->topology.blocks))
            || !add_product(&expected_bytes,
                image->topology.instruction_capacity,
                sizeof(*image->topology.instructions))
            || !add_product(&expected_bytes, image->topology.value_capacity,
                sizeof(*image->topology.values))
            || !add_product(&expected_bytes,
                image->topology.parameter_value_capacity,
                sizeof(*image->topology.parameter_values))
            || !add_product(&expected_bytes,
                image->topology.edge_value_capacity,
                sizeof(*image->topology.edge_values))
            || !add_product(&expected_bytes,
                image->topology.call_argument_capacity,
                sizeof(*image->topology.call_arguments))
            || !add_product(&expected_bytes, image->topology.loop_capacity,
                sizeof(*image->topology.loops))
            || !add_product(&expected_bytes,
                image->topology.construct_operand_capacity,
                sizeof(*image->topology.construct_operands))
            || !add_product(&expected_bytes,
                image->topology.temporary_capacity,
                sizeof(*image->topology.temporaries))) goto malformed;
        size_t items = mir_item_count(&image->topology);
        if (items == SIZE_MAX || items > SIZE_MAX - expected_cfg) goto malformed;
        expected_cfg += items;
        if (image->locals.offset != expected_locals
            || image->places.offset != expected_places
            || image->values.offset != expected_values
            || image->values.count != template->mir.value_count
            || image->instructions.offset != expected_instructions
            || image->instructions.count != template->mir.instruction_count
            || image->temporaries.offset != expected_temporaries
            || image->temporaries.count != template->mir.temporary_count
            || image->construct_operands.offset != expected_operands
            || image->construct_operands.count
                != template->mir.construct_operand_count
            || image->call_arguments.offset != expected_call_arguments
            || image->call_arguments.count != template->mir.call_argument_count
            || !slice_ok(image->locals, owner->local_count)
            || !slice_ok(image->places, owner->place_count)
            || !slice_ok(image->values, owner->value_count)
            || !slice_ok(image->instructions, owner->instruction_count)
            || !slice_ok(image->temporaries, owner->temporary_count)
            || !slice_ok(image->construct_operands,
                owner->construct_operand_count)
            || !slice_ok(image->call_arguments,
                owner->call_argument_count)) goto malformed;
        SolMirPlanContextId body = instance->contexts.offset;
        size_t local_at = 0;
        for (size_t use = 0; use < instance->typed_uses.count; ++use) {
            const SolMirPlanTypedUse *typed = &plan->typed_uses[
                instance->typed_uses.offset + use];
            if (typed->context != body || typed->kind != SOL_MIR_PLAN_USE_LOCAL) {
                continue;
            }
            if (local_at >= image->locals.count) goto malformed;
            const SolMirMaterializedLocal *local
                = &owner->locals[image->locals.offset + local_at++];
            SolMirMaterializedLocalKind expected_kind
                = SOL_MIR_MATERIALIZED_LOCAL_BODY;
            size_t expected_ordinal = SOL_MIR_MATERIALIZED_NONE;
            const SolIrCallable *callable = &ir->callables[instance->callable];
            if (typed->source == callable->receiver) {
                expected_kind = SOL_MIR_MATERIALIZED_LOCAL_RECEIVER;
                expected_ordinal = 0;
            }
            for (size_t parameter = 0; parameter < callable->parameters.count;
                ++parameter) {
                if (ir->roots[callable->parameters.offset + parameter]
                        == typed->source) {
                    expected_kind = SOL_MIR_MATERIALIZED_LOCAL_PARAMETER;
                    expected_ordinal = parameter;
                }
            }
            if (local->instance != id || local->source_local != typed->source
                || local->type != typed->type || local->access != typed->access
                || local->kind != expected_kind
                || local->ordinal != expected_ordinal
                || local->type >= owner->type_count) goto malformed;
        }
        if (local_at != image->locals.count) goto malformed;
        expected_locals += image->locals.count;
        size_t place_at = 0;
        for (size_t use = 0; use < instance->typed_uses.count; ++use) {
            const SolMirPlanTypedUse *typed = &plan->typed_uses[
                instance->typed_uses.offset + use];
            if (typed->context != body
                || typed->kind != SOL_MIR_PLAN_USE_PLACE_ROOT) continue;
            if (place_at >= image->places.count
                || owner->places[image->places.offset + place_at].source_place
                    != typed->source) goto malformed;
            ++place_at;
        }
        for (size_t instruction = 0; instruction < template->mir.instruction_count;
            ++instruction) {
            SolMirPlace source;
            if (!instruction_place(&template->mir.instructions[instruction], &source)
                || source.source_place != SOL_IR_NONE) continue;
            bool earlier = false;
            for (size_t previous = 0; previous < instruction; ++previous) {
                SolMirPlace candidate;
                earlier = earlier || (instruction_place(
                    &template->mir.instructions[previous], &candidate)
                    && candidate.source_place == SOL_IR_NONE
                    && candidate.local == source.local);
            }
            if (earlier) continue;
            if (place_at >= image->places.count) goto malformed;
            const SolMirMaterializedPlace *place
                = &owner->places[image->places.offset + place_at++];
            if (place->source_place != SOL_IR_NONE
                || place->local < image->locals.offset
                || place->local >= image->locals.offset + image->locals.count
                || owner->locals[place->local].source_local != source.local) {
                goto malformed;
            }
        }
        if (place_at != image->places.count) goto malformed;
        for (size_t place_item = 0; place_item < image->places.count; ++place_item) {
            const SolMirMaterializedPlace *place
                = &owner->places[image->places.offset + place_item];
            if (place->instance != id || place->local < image->locals.offset
                || place->local >= image->locals.offset + image->locals.count
                || place->root_type >= owner->type_count
                || place->final_type >= owner->type_count
                || place->projections.offset != expected_projections
                || !slice_ok(place->projections, owner->projection_count)) goto malformed;
            if (place->source_place == SOL_IR_NONE) {
                if (place->projections.count != 0
                    || place->root_type != owner->locals[place->local].type
                    || place->final_type != place->root_type) goto malformed;
            } else {
                if (place->source_place >= ir->place_count) goto malformed;
                const SolIrPlace *source = &ir->places[place->source_place];
                if (source->local != owner->locals[place->local].source_local
                    || source->projections.count != place->projections.count) {
                    goto malformed;
                }
                const SolMirPlanTypedUse *root = find_use(plan, instance,
                    SOL_MIR_PLAN_USE_PLACE_ROOT, place->source_place, 0, body);
                const SolMirPlanTypedUse *final = find_use(plan, instance,
                    SOL_MIR_PLAN_USE_PLACE_FINAL, place->source_place, 0, body);
                if (root == NULL || final == NULL || root->type != place->root_type
                    || final->type != place->final_type) goto malformed;
                for (size_t projection = 0; projection < place->projections.count;
                    ++projection) {
                    const SolMirMaterializedProjection *concrete
                        = &owner->projections[place->projections.offset + projection];
                    size_t source_id = source->projections.offset + projection;
                    const SolIrProjection *symbolic = &ir->projections[source_id];
                    const SolMirPlanTypedUse *type = find_use(plan, instance,
                        SOL_MIR_PLAN_USE_PLACE_PROJECTION, place->source_place,
                        projection, body);
                    if (type == NULL || concrete->kind != symbolic->kind
                        || concrete->type != type->type
                        || concrete->source_field != symbolic->field
                        || concrete->tuple_ordinal != symbolic->ordinal
                        || concrete->source_projection != source_id) goto malformed;
                }
            }
            expected_projections += place->projections.count;
        }
        expected_places += image->places.count;
        for (size_t value = 0; value < image->values.count; ++value) {
            const SolMirValue *source = &template->mir.values[value];
            const SolMirMaterializedValue *concrete
                = &owner->values[image->values.offset + value];
            const SolMirPlanTypedUse *type = find_use(plan, instance,
                SOL_MIR_PLAN_USE_MIR_VALUE, value, 0, body);
            if (type == NULL || concrete->kind != source->kind
                || concrete->type != type->type || concrete->type >= owner->type_count
                || concrete->block != source->block
                || concrete->source_expression != source->source_expression
                || concrete->span.start != source->span.start
                || concrete->span.end != source->span.end
                || concrete->source_definition != source->definition
                || concrete->instruction != (source->kind
                        == SOL_MIR_VALUE_INSTRUCTION
                    ? image->instructions.offset + source->definition
                    : SOL_MIR_MATERIALIZED_NONE)) goto malformed;
        }
        expected_values += image->values.count;
        for (size_t temporary = 0; temporary < image->temporaries.count; ++temporary) {
            const SolMirTemporary *source = &template->mir.temporaries[temporary];
            const SolMirMaterializedTemporary *concrete
                = &owner->temporaries[image->temporaries.offset + temporary];
            const SolMirPlanTypedUse *type = find_use(plan, instance,
                SOL_MIR_PLAN_USE_MIR_TEMPORARY, temporary, 0, body);
            if (type == NULL || concrete->type != type->type
                || concrete->type >= owner->type_count
                || concrete->source_expression != source->source_expression
                || concrete->span.start != source->span.start
                || concrete->span.end != source->span.end) goto malformed;
        }
        expected_temporaries += image->temporaries.count;
        for (size_t operand = 0; operand < image->construct_operands.count; ++operand) {
            const SolMirConstructOperand *source
                = &template->mir.construct_operands[operand];
            const SolMirMaterializedConstructOperand *concrete
                = &owner->construct_operands[image->construct_operands.offset + operand];
            const SolMirPlanTypedUse *type = find_use(plan, instance,
                SOL_MIR_PLAN_USE_CONSTRUCT_OPERAND, operand, source->formal, body);
            SolMirMaterializedTemporaryId expected_temporary
                = source->temporary < template->mir.temporary_count
                ? image->temporaries.offset + source->temporary
                : SOL_MIR_MATERIALIZED_NONE;
            if (type == NULL || concrete->formal != source->formal
                || concrete->source_expression != source->source_expression
                || concrete->type != type->type || concrete->type >= owner->type_count
                || concrete->temporary != expected_temporary) goto malformed;
        }
        expected_operands += image->construct_operands.count;
        for (size_t argument = 0; argument < image->call_arguments.count;
            ++argument) {
            const SolMirCallArgument *source = &template->mir.call_arguments[argument];
            const SolMirMaterializedCallArgument *concrete
                = &owner->call_arguments[image->call_arguments.offset + argument];
            if (concrete->formal != source->formal
                || concrete->access != source->access
                || concrete->source_expression != source->source_expression
                || concrete->type >= owner->type_count) goto malformed;
            if (source->access == SOL_ACCESS_OWNED) {
                const SolMirPlanTypedUse *type = find_use(plan, instance,
                    SOL_MIR_PLAN_USE_MIR_TEMPORARY, source->temporary, 0, body);
                if (concrete->temporary
                        != image->temporaries.offset + source->temporary
                    || concrete->place != SOL_MIR_MATERIALIZED_NONE
                    || type == NULL || concrete->type != type->type) goto malformed;
            } else {
                const SolMirPlanTypedUse *type = find_use(plan, instance,
                    SOL_MIR_PLAN_USE_PLACE_FINAL, source->place, 0, body);
                if (source->place >= ir->place_count) goto malformed;
                const SolIrPlace *source_place = &ir->places[source->place];
                SolMirMaterializedPlaceId expected_place = place_for(owner, image,
                    (SolMirPlace){source_place->local, source->place});
                if (concrete->place != expected_place
                    || concrete->temporary != SOL_MIR_MATERIALIZED_NONE
                    || type == NULL || concrete->type != type->type) goto malformed;
            }
        }
        expected_call_arguments += image->call_arguments.count;
        for (size_t instruction = 0; instruction < image->instructions.count;
            ++instruction) {
            const SolMirInstruction *source = &template->mir.instructions[instruction];
            const SolMirMaterializedInstruction *concrete
                = &owner->instructions[image->instructions.offset + instruction];
            SolMirMaterializedInstruction reconstructed;
            if (!translate_instruction(owner, image, instance, source,
                    instruction, &reconstructed)
                || !instruction_equal(concrete, &reconstructed)) goto malformed;
            if (concrete->kind != source->kind || concrete->block != source->block
                || concrete->source_expression != source->source_expression
                || concrete->span.start != source->span.start
                || concrete->span.end != source->span.end
                || (concrete->type != SOL_MIR_MATERIALIZED_NONE
                    && concrete->type >= owner->type_count)
                || (concrete->local != SOL_MIR_MATERIALIZED_NONE
                    && (concrete->local < image->locals.offset
                        || concrete->local >= image->locals.offset
                            + image->locals.count))
                || (concrete->place != SOL_MIR_MATERIALIZED_NONE
                    && (concrete->place < image->places.offset
                        || concrete->place >= image->places.offset
                            + image->places.count))
                || (concrete->result != SOL_MIR_MATERIALIZED_NONE
                    && concrete->result >= owner->value_count)
                || (concrete->left != SOL_MIR_MATERIALIZED_NONE
                    && concrete->left >= owner->value_count)
                || (concrete->right != SOL_MIR_MATERIALIZED_NONE
                    && concrete->right >= owner->value_count)
                || (concrete->temporary != SOL_MIR_MATERIALIZED_NONE
                    && concrete->temporary >= owner->temporary_count)
                || (concrete->previous != SOL_MIR_MATERIALIZED_NONE
                    && concrete->previous >= owner->temporary_count)
                || !slice_ok(concrete->construct_operands,
                    owner->construct_operand_count)) goto malformed;
            SolMirPlace source_place;
            if (instruction_place(source, &source_place)) {
                if (concrete->place != place_for(owner, image, source_place)) {
                    goto malformed;
                }
            } else if (concrete->place != SOL_MIR_MATERIALIZED_NONE) goto malformed;
            if (concrete->result != (source->result == SOL_MIR_NONE
                    ? SOL_MIR_MATERIALIZED_NONE
                    : image->values.offset + source->result)) goto malformed;
            if (source->kind == SOL_MIR_INST_PARAMETER_LIVE
                || source->kind == SOL_MIR_INST_STORAGE_LIVE
                || source->kind == SOL_MIR_INST_DROP_IF_INITIALIZED
                || source->kind == SOL_MIR_INST_STORAGE_DEAD) {
                if (concrete->local != local_for(owner, image, source->as.local)) {
                    goto malformed;
                }
            } else if (concrete->local != SOL_MIR_MATERIALIZED_NONE) goto malformed;
            if (source->type == SOL_IR_NONE) {
                if (concrete->type != SOL_MIR_MATERIALIZED_NONE) goto malformed;
            } else {
                const SolMirPlanTypedUse *type = find_use(plan, instance,
                    SOL_MIR_PLAN_USE_MIR_INSTRUCTION, instruction, 0, body);
                if (type == NULL || concrete->type != type->type) goto malformed;
            }
        }
        expected_instructions += image->instructions.count;
        if (image->invokes.offset != expected_invokes
            || !slice_ok(image->invokes, owner->invoke_binding_count)) goto malformed;
        size_t binding_at = 0;
        for (size_t block = 0; block < image->topology.block_count; ++block) {
            const SolMirTerminator *term = &image->topology.blocks[block].terminator;
            if (term->kind != SOL_MIR_TERM_INVOKE) continue;
            if (binding_at >= image->invokes.count) goto malformed;
            const SolMirMaterializedInvokeBinding *binding
                = &owner->invoke_bindings[image->invokes.offset + binding_at++];
            SolMirProgramSource source;
            if (binding->block != block
                || !source_for_term(owner->plan, instance->callable, term, &source)
                || !source_equal(binding->source, source)) goto malformed;
            const SolMirPlanDemand *found = NULL;
            for (size_t demand = 0; demand < owner->plan->demand_count; ++demand) {
                const SolMirPlanDemand *item = &owner->plan->demands[demand];
                if (item->parent == id
                    && item->context == instance->contexts.offset
                    && (item->kind == SOL_MIR_PLAN_DEMAND_INVOKE
                        || item->kind == SOL_MIR_PLAN_DEMAND_CALLBACK)
                    && source_equal(item->source, source)) {
                    if (found != NULL) goto malformed;
                    found = item;
                }
            }
            if (found == NULL || binding->symbolic_callable != found->symbolic_target
                || binding->dispatch_trait != found->dispatch_trait
                || binding->dispatch_requirement != found->dispatch_requirement
                || binding->instance != found->instance || binding->import != found->import
                || binding->target_kind != (found->instance != SOL_MIR_PLAN_NONE
                    ? SOL_MIR_MATERIALIZED_TARGET_INSTANCE
                    : SOL_MIR_MATERIALIZED_TARGET_IMPORT)) goto malformed;
            Materializer verifier = {(SolMirMaterialization *)(uintptr_t)owner,
                diagnostics, SOL_MIR_MATERIALIZE_BUILD_INTERNAL_FAILED};
            if (!verify_signature(&verifier, instance, &image->topology,
                    term, found)) goto malformed;
        }
        if (binding_at != image->invokes.count) goto malformed;
        expected_invokes += image->invokes.count;
    }
#undef RANGE
    expected_bindings = expected_overlays + expected_invokes;
    size_t expected_work = expected_cfg;
    if (expected_invokes > SIZE_MAX - expected_work
        || owner->image_count > SIZE_MAX - expected_work - expected_invokes
        || expected_overlays > SIZE_MAX - expected_work - expected_invokes
            - owner->image_count) goto malformed;
    expected_work += expected_invokes + owner->image_count + expected_overlays;
    if (owner->context_count > SIZE_MAX - owner->type_count) goto malformed;
    size_t expected_records = owner->type_count + owner->context_count;
    if (owner->overlay_count > SIZE_MAX - expected_records
        || owner->local_count > SIZE_MAX - expected_records - owner->overlay_count
        || owner->place_count > SIZE_MAX - expected_records - owner->overlay_count
            - owner->local_count) goto malformed;
    expected_records += owner->overlay_count + owner->local_count + owner->place_count;
    size_t remaining[] = {owner->projection_count, owner->value_count,
        owner->instruction_count, owner->temporary_count,
        owner->construct_operand_count, owner->call_argument_count};
    for (size_t index = 0; index < sizeof(remaining) / sizeof(remaining[0]); ++index) {
        if (remaining[index] > SIZE_MAX - expected_records) goto malformed;
        expected_records += remaining[index];
    }
    if (expected_records > SIZE_MAX - expected_work
        || copy_work > SIZE_MAX - expected_work - expected_records) goto malformed;
    expected_work += expected_records + copy_work;
    free(ranges);
    if (expected_types != owner->type_id_count
        || expected_accesses != owner->access_count
        || expected_overlays != owner->overlay_count
        || expected_invokes != owner->invoke_binding_count
        || expected_locals != owner->local_count
        || expected_places != owner->place_count
        || expected_projections != owner->projection_count
        || expected_values != owner->value_count
        || expected_instructions != owner->instruction_count
        || expected_temporaries != owner->temporary_count
        || expected_operands != owner->construct_operand_count
        || expected_call_arguments != owner->call_argument_count
        || expected_records != owner->usage.concrete_records
        || expected_cfg != owner->usage.cfg_items
        || expected_bindings != owner->usage.bindings
        || expected_bytes != owner->usage.owned_bytes
        || expected_work != owner->usage.materialization_work) {
        return validation_error(diagnostics,
            "materialized MIR counts are not canonical");
    }
    return true;
malformed:
    free(ranges);
    return validation_error(diagnostics,
        "materialized MIR provenance, overlay, binding, slice, or allocation is malformed");
}

typedef struct { char *data; size_t length; size_t capacity; bool failed; } Buffer;

static void format(Buffer *buffer, const char *pattern, ...) {
    if (buffer->failed) return;
    va_list arguments;
    va_start(arguments, pattern);
    va_list copy;
    va_copy(copy, arguments);
    int count = vsnprintf(NULL, 0, pattern, copy);
    va_end(copy);
    if (count < 0 || (size_t)count > SIZE_MAX - buffer->length - 1) {
        buffer->failed = true; va_end(arguments); return;
    }
    size_t needed = buffer->length + (size_t)count + 1;
    if (needed > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
            capacity *= 2;
        }
        char *grown = realloc(buffer->data, capacity);
        if (grown == NULL) { buffer->failed = true; va_end(arguments); return; }
        buffer->data = grown; buffer->capacity = capacity;
    }
    (void)vsnprintf(buffer->data + buffer->length,
        buffer->capacity - buffer->length, pattern, arguments);
    va_end(arguments);
    buffer->length += (size_t)count;
}

bool sol_mir_materialization_render(FILE *stream,
    const SolMirMaterialization *owner) {
    if (stream == NULL || !sol_mir_materialization_validate(owner, NULL)) return false;
    Buffer buffer = {0};
    format(&buffer, "materialized_mir images=%zu types=%zu contexts=%zu overlays=%zu invokes=%zu\n",
        owner->image_count, owner->type_count, owner->context_count,
        owner->overlay_count, owner->invoke_binding_count);
    for (size_t id = 0; id < owner->type_count; ++id) {
        const SolMirMaterializedType *type = &owner->types[id];
        format(&buffer, "type t%zu kind=%d definition=%zu copy=%d args=[", id,
            (int)type->kind, type->definition, type->is_copy ? 1 : 0);
        for (size_t item = 0; item < type->arguments.count; ++item) {
            format(&buffer, "%st%zu", item == 0 ? "" : ",",
                owner->type_ids[type->arguments.offset + item]);
        }
        format(&buffer, "] params=[");
        for (size_t item = 0; item < type->parameters.count; ++item) {
            format(&buffer, "%st%zu/%d", item == 0 ? "" : ",",
                owner->type_ids[type->parameters.offset + item],
                (int)owner->accesses[type->parameter_accesses.offset + item]);
        }
        format(&buffer, "] result=");
        if (type->result == SOL_MIR_MATERIALIZED_NONE) format(&buffer, "none");
        else format(&buffer, "t%zu", type->result);
        format(&buffer, " effects=%zu ownership=[", type->effects);
        for (size_t item = 0; item < type->ownership_components.count; ++item) {
            format(&buffer, "%st%zu", item == 0 ? "" : ",",
                owner->type_ids[type->ownership_components.offset + item]);
        }
        format(&buffer, "]\n");
    }
    for (size_t id = 0; id < owner->context_count; ++id) {
        const SolMirPlanContext *context = &owner->contexts[id];
        format(&buffer, "context k%zu kind=%d instance=i%zu block=%zu definition=%zu obligation=%zu source=c%zu:x%zu\n",
            id, (int)context->kind, context->instance, context->source_block,
            context->definition, context->obligation, context->source.callable,
            context->source.expression);
    }
    for (size_t id = 0; id < owner->image_count; ++id) {
        const SolMirMaterializedImage *image = &owner->images[id];
        format(&buffer, "image i%zu callable=c%zu receiver=", image->instance,
            image->source_callable);
        if (image->receiver == SOL_MIR_PLAN_NONE) format(&buffer, "none");
        else format(&buffer, "t%zu", image->receiver);
        format(&buffer, " type_args=[");
        for (size_t item = 0; item < image->type_arguments.count; ++item) {
            format(&buffer, "%st%zu", item == 0 ? "" : ",",
                owner->type_ids[image->type_arguments.offset + item]);
        }
        format(&buffer, "] params=[");
        for (size_t item = 0; item < image->parameter_types.count; ++item) {
            format(&buffer, "%st%zu/%d", item == 0 ? "" : ",",
                owner->type_ids[image->parameter_types.offset + item],
                (int)owner->accesses[image->parameter_accesses.offset + item]);
        }
        format(&buffer, "] result=t%zu effects=e%zu template=c%zu\n",
            image->result, image->effects, image->topology.callable);
        for (size_t item = 0; item < image->overlays.count; ++item) {
            const SolMirMaterializedTypeOverlay *overlay
                = &owner->overlays[image->overlays.offset + item];
            format(&buffer, "  overlay context=k%zu kind=%d source=%zu ordinal=%zu type=t%zu access=%d\n",
                overlay->context,
                (int)overlay->kind, overlay->source, overlay->ordinal,
                overlay->type, (int)overlay->access);
        }
        for (size_t item = 0; item < image->locals.count; ++item) {
            const SolMirMaterializedLocal *local
                = &owner->locals[image->locals.offset + item];
            format(&buffer, "  local l%zu source=l%zu kind=%d ordinal=%zu type=t%zu access=%d\n",
                image->locals.offset + item, local->source_local, (int)local->kind,
                local->ordinal, local->type, (int)local->access);
        }
        for (size_t item = 0; item < image->places.count; ++item) {
            const SolMirMaterializedPlace *place
                = &owner->places[image->places.offset + item];
            format(&buffer, "  place p%zu source=%zu local=l%zu root=t%zu final=t%zu projections=%zu\n",
                image->places.offset + item, place->source_place, place->local,
                place->root_type, place->final_type, place->projections.count);
            for (size_t projection = 0; projection < place->projections.count;
                ++projection) {
                const SolMirMaterializedProjection *projection_record
                    = &owner->projections[place->projections.offset + projection];
                format(&buffer, "    projection r%zu source=%zu kind=%d field=%zu tuple=%zu type=t%zu\n",
                    place->projections.offset + projection,
                    projection_record->source_projection,
                    (int)projection_record->kind,
                    projection_record->source_field,
                    projection_record->tuple_ordinal, projection_record->type);
            }
        }
        for (size_t item = 0; item < image->values.count; ++item) {
            const SolMirMaterializedValue *value
                = &owner->values[image->values.offset + item];
            format(&buffer, "  value v%zu kind=%d block=b%zu type=t%zu source_definition=%zu instruction=%zu source=x%zu\n",
                image->values.offset + item, (int)value->kind, value->block,
                value->type, value->source_definition, value->instruction,
                value->source_expression);
        }
        for (size_t item = 0; item < image->instructions.count; ++item) {
            const SolMirMaterializedInstruction *instruction
                = &owner->instructions[image->instructions.offset + item];
            format(&buffer, "  instruction n%zu kind=%d block=b%zu type=",
                image->instructions.offset + item, (int)instruction->kind,
                instruction->block);
            if (instruction->type == SOL_MIR_MATERIALIZED_NONE) {
                format(&buffer, "none");
            } else format(&buffer, "t%zu", instruction->type);
            format(&buffer, " result=%zu local=%zu place=%zu left=%zu right=%zu temporary=%zu previous=%zu preserve=%zu operator=%d statement=%zu match=x%zu arm=%zu arm_ordinal=%zu pattern=%zu scrutinee=%zu snapshot=%zu construct=%d definition=%zu variant=%zu operands=[%zu,%zu] roots=[%zu,%zu] operations=[%zu,%zu] source=x%zu integer=%lld boolean=%d\n",
                instruction->result, instruction->local, instruction->place,
                instruction->left, instruction->right, instruction->temporary,
                instruction->previous, instruction->preserve_depth,
                (int)instruction->operator_kind, instruction->source_statement,
                instruction->match_expression, instruction->source_arm,
                instruction->arm_ordinal, instruction->source_pattern,
                instruction->pattern_scrutinee, instruction->source_snapshot,
                (int)instruction->construct_kind,
                instruction->construct_definition,
                instruction->construct_variant,
                instruction->construct_operands.offset,
                instruction->construct_operands.count,
                instruction->source_capability_roots.offset,
                instruction->source_capability_roots.count,
                instruction->source_operation_roots.offset,
                instruction->source_operation_roots.count,
                instruction->source_expression,
                (long long)instruction->integer,
                instruction->boolean ? 1 : 0);
        }
        for (size_t item = 0; item < image->temporaries.count; ++item) {
            const SolMirMaterializedTemporary *temporary
                = &owner->temporaries[image->temporaries.offset + item];
            format(&buffer, "  temporary q%zu type=t%zu source=x%zu\n",
                image->temporaries.offset + item, temporary->type,
                temporary->source_expression);
        }
        for (size_t item = 0; item < image->construct_operands.count; ++item) {
            const SolMirMaterializedConstructOperand *operand
                = &owner->construct_operands[image->construct_operands.offset + item];
            format(&buffer, "  construct_operand o%zu formal=%zu type=t%zu temporary=q%zu source=x%zu\n",
                image->construct_operands.offset + item, operand->formal,
                operand->type, operand->temporary, operand->source_expression);
        }
        for (size_t item = 0; item < image->call_arguments.count; ++item) {
            const SolMirMaterializedCallArgument *argument
                = &owner->call_arguments[image->call_arguments.offset + item];
            format(&buffer, "  call_argument a%zu formal=%zu access=%d type=t%zu temporary=%zu place=%zu source=x%zu\n",
                image->call_arguments.offset + item, argument->formal,
                (int)argument->access, argument->type, argument->temporary,
                argument->place, argument->source_expression);
        }
        for (size_t item = 0; item < image->invokes.count; ++item) {
            const SolMirMaterializedInvokeBinding *binding
                = &owner->invoke_bindings[image->invokes.offset + item];
            format(&buffer, "  invoke block=b%zu source=c%zu:x%zu symbolic=c%zu target=",
                binding->block, binding->source.callable,
                binding->source.expression, binding->symbolic_callable);
            if (binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE) {
                format(&buffer, "i%zu", binding->instance);
            } else format(&buffer, "import%zu", binding->import);
            format(&buffer, " trait=%zu requirement=%zu\n",
                binding->dispatch_trait, binding->dispatch_requirement);
        }
    }
    bool ok = !buffer.failed
        && (buffer.length == 0
            || fwrite(buffer.data, 1, buffer.length, stream) == buffer.length);
    free(buffer.data);
    return ok;
}
