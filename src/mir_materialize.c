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
    free(owner->type_ids);
    free(owner->accesses);
    free(owner->overlays);
    free(owner->invoke_bindings);
    sol_mir_materialization_init(owner);
}

SolMirMaterializeLimits sol_mir_materialize_default_limits(void) {
    return (SolMirMaterializeLimits){4096, 4000000, 1000000,
        512u * 1024u * 1024u, 8000000};
}

static bool limits_zero(SolMirMaterializeLimits limits) {
    return limits.max_instances == 0 && limits.max_cfg_items == 0
        && limits.max_bindings == 0 && limits.max_owned_bytes == 0
        && limits.max_materialization_work == 0;
}

static bool limits_complete(SolMirMaterializeLimits limits) {
    return limits.max_instances != 0 && limits.max_cfg_items != 0
        && limits.max_bindings != 0 && limits.max_owned_bytes != 0
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
    size_t source, size_t ordinal) {
    const SolMirPlanTypedUse *found = NULL;
    for (size_t index = 0; index < instance->typed_uses.count; ++index) {
        const SolMirPlanTypedUse *use
            = &plan->typed_uses[instance->typed_uses.offset + index];
        if (use->context == 0 && use->kind == kind && use->source == source
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
    if (argument->access == SOL_ACCESS_OWNED) {
        use = find_use(plan, parent, SOL_MIR_PLAN_USE_MIR_TEMPORARY,
            argument->temporary, 0);
    } else if (argument->place != SOL_IR_NONE) {
        use = find_use(plan, parent, SOL_MIR_PLAN_USE_PLACE_FINAL,
            argument->place, 0);
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
        SOL_MIR_PLAN_USE_MIR_VALUE, term->as.invoke.result, 0);
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
        if (demand->parent == parent_id && demand->context == 0
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

static bool build_scratch(Materializer *builder) {
    SolMirMaterialization *output = builder->output;
    const SolMirPlan *plan = output->plan;
    for (size_t index = 0; index < plan->demand_count; ++index) {
        if (plan->demands[index].kind == SOL_MIR_PLAN_DEMAND_HANDLER_SOURCE
            || plan->demands[index].kind
                == SOL_MIR_PLAN_DEMAND_HANDLER_PROVIDER) {
            return report(builder,
                SOL_MIR_MATERIALIZE_BUILD_UNSUPPORTED_OR_UNRESOLVED,
                "handler source/provider binding is deferred to P2.3b");
        }
    }
    if (plan->instance_count > output->limits.max_instances) {
        return report(builder, SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
            "materialized instance limit exceeded");
    }
    size_t context_zero = 0;
    size_t invokes = 0;
    size_t signature_types = 0;
    size_t signature_accesses = 0;
    for (size_t id = 0; id < plan->instance_count; ++id) {
        const SolMirPlanInstance *instance = &plan->instances[id];
        const SolMirProgramTemplate *template = find_template(plan,
            instance->callable);
        if (template == NULL) return report(builder,
            SOL_MIR_MATERIALIZE_BUILD_UNSUPPORTED_OR_UNRESOLVED,
            "materialized instance has no MIR template");
        for (size_t use = 0; use < instance->typed_uses.count; ++use) {
            context_zero += plan->typed_uses[
                instance->typed_uses.offset + use].context == 0;
        }
        for (size_t block = 0; block < template->mir.block_count; ++block) {
            invokes += template->mir.blocks[block].terminator.kind
                == SOL_MIR_TERM_INVOKE;
        }
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
    if (context_zero > output->limits.max_bindings
        || invokes > output->limits.max_bindings - context_zero) {
        return report(builder, SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED,
            "materialized overlay/binding limit exceeded");
    }
    output->images = allocate_owned(builder, plan->instance_count,
        sizeof(*output->images));
    output->type_ids = allocate_owned(builder, signature_types,
        sizeof(*output->type_ids));
    output->accesses = allocate_owned(builder, signature_accesses,
        sizeof(*output->accesses));
    output->overlays = allocate_owned(builder, context_zero,
        sizeof(*output->overlays));
    output->invoke_bindings = allocate_owned(builder, invokes,
        sizeof(*output->invoke_bindings));
    if ((plan->instance_count != 0 && output->images == NULL)
        || (signature_types != 0 && output->type_ids == NULL)
        || (signature_accesses != 0 && output->accesses == NULL)
        || (context_zero != 0 && output->overlays == NULL)
        || (invokes != 0 && output->invoke_bindings == NULL)) return false;
    output->image_count = output->image_capacity = plan->instance_count;
    output->type_id_count = output->type_id_capacity = signature_types;
    output->access_count = output->access_capacity = signature_accesses;
    output->overlay_capacity = context_zero;
    output->invoke_binding_capacity = invokes;
    size_t type_at = 0;
    size_t access_at = 0;
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
            if (source->context != 0) continue;
            output->overlays[output->overlay_count++]
                = (SolMirMaterializedTypeOverlay){source->kind, source->source,
                    source->ordinal, source->type, source->access};
        }
        image->overlays.count = output->overlay_count - image->overlays.offset;
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
        || owner->usage.owned_bytes > owner->limits.max_owned_bytes
        || owner->usage.materialization_work
            > owner->limits.max_materialization_work
        || !canonical(owner->image_count, owner->image_capacity, owner->images)
        || !canonical(owner->type_id_count, owner->type_id_capacity, owner->type_ids)
        || !canonical(owner->access_count, owner->access_capacity, owner->accesses)
        || !canonical(owner->overlay_count, owner->overlay_capacity, owner->overlays)
        || !canonical(owner->invoke_binding_count,
            owner->invoke_binding_capacity, owner->invoke_bindings)
        || !sol_mir_plan_validate(owner->plan, diagnostics)) {
        return validation_error(diagnostics,
            "materialized MIR owner header or borrowed plan is invalid");
    }
    size_t max_ranges = 5;
    size_t templates = owner->plan->program->template_count;
    if (owner->image_count > (SIZE_MAX - max_ranges) / 9) {
        return validation_error(diagnostics, "materialized MIR range count overflowed");
    }
    max_ranges += owner->image_count * 9;
    if (templates > (SIZE_MAX - max_ranges) / 9) {
        return validation_error(diagnostics, "materialized MIR range count overflowed");
    }
    max_ranges += templates * 9;
    if (max_ranges > SIZE_MAX - 19
        || owner->plan->effect_atom_count > SIZE_MAX - max_ranges - 19) {
        return validation_error(diagnostics, "materialized MIR range count overflowed");
    }
    max_ranges += 19 + owner->plan->effect_atom_count;
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
    RANGE(owner->type_ids, owner->type_id_capacity, *owner->type_ids);
    RANGE(owner->accesses, owner->access_capacity, *owner->accesses);
    RANGE(owner->overlays, owner->overlay_capacity, *owner->overlays);
    RANGE(owner->invoke_bindings, owner->invoke_binding_capacity,
        *owner->invoke_bindings);
    size_t expected_types = 0, expected_accesses = 0, expected_overlays = 0;
    size_t expected_invokes = 0, expected_cfg = 0, expected_bindings = 0;
    size_t expected_bytes = 0;
    if (!add_product(&expected_bytes, owner->image_capacity,
            sizeof(*owner->images))
        || !add_product(&expected_bytes, owner->type_id_capacity,
            sizeof(*owner->type_ids))
        || !add_product(&expected_bytes, owner->access_capacity,
            sizeof(*owner->accesses))
        || !add_product(&expected_bytes, owner->overlay_capacity,
            sizeof(*owner->overlays))
        || !add_product(&expected_bytes, owner->invoke_binding_capacity,
            sizeof(*owner->invoke_bindings))) goto malformed;
    for (size_t id = 0; id < owner->image_count; ++id) {
        const SolMirMaterializedImage *image = &owner->images[id];
        const SolMirPlanInstance *instance = &owner->plan->instances[id];
        const SolMirProgramTemplate *template = find_template(owner->plan,
            instance->callable);
        if (image->instance != id || image->source_callable != instance->callable
            || image->receiver != instance->receiver || image->result != instance->result
            || image->effects != instance->effects || template == NULL
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
            if (source->context != 0) continue;
            if (use_at >= image->overlays.count) goto malformed;
            const SolMirMaterializedTypeOverlay *overlay
                = &owner->overlays[image->overlays.offset + use_at++];
            if (overlay->kind != source->kind || overlay->source != source->source
                || overlay->ordinal != source->ordinal || overlay->type != source->type
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
                if (item->parent == id && item->context == 0
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
    free(ranges);
    if (expected_types != owner->type_id_count
        || expected_accesses != owner->access_count
        || expected_overlays != owner->overlay_count
        || expected_invokes != owner->invoke_binding_count
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
    format(&buffer, "materialized_mir images=%zu overlays=%zu invokes=%zu\n",
        owner->image_count, owner->overlay_count, owner->invoke_binding_count);
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
            format(&buffer, "  overlay kind=%d source=%zu ordinal=%zu type=t%zu access=%d\n",
                (int)overlay->kind, overlay->source, overlay->ordinal,
                overlay->type, (int)overlay->access);
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
