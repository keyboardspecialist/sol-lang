#include "sol/mir_runtime_values.h"
#include "mir_runtime_conventions_internal.h"
#include "mir_runtime_values_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local size_t metered_validation_work;
static _Thread_local size_t metered_validation_limit;
static _Thread_local bool validation_work_exhausted;

enum { VALIDATION_HEADER_WORK = 8 };

#ifdef SOL_MIR_PLAN_TEST_HOOKS
static _Thread_local bool force_validation_allocation_failure;
static _Thread_local size_t validation_allocation_attempts;

void sol_mir_runtime_values_test_force_validation_allocation_failure(bool force) {
    force_validation_allocation_failure = force;
}

size_t sol_mir_runtime_values_test_validation_allocation_attempts(void) {
    return validation_allocation_attempts;
}
#endif

static bool invalid(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) sol_diagnostics_add(diagnostics,
        "SOL-MIR-RUNTIME-VALUES-002", SOL_SEVERITY_ERROR, (SolSpan){0},
        message);
    return false;
}

static bool add_size(size_t *value, size_t amount) {
    if (amount > SIZE_MAX - *value) return false;
    *value += amount;
    return true;
}

static bool validation_event(size_t amount) {
    if (!add_size(&metered_validation_work, amount)
        || metered_validation_work > metered_validation_limit) {
        validation_work_exhausted = true;
        return false;
    }
    return true;
}

static bool mul_size(size_t left, size_t right, size_t *result) {
    if (left != 0 && right > SIZE_MAX / left) return false;
    *result = left * right;
    return true;
}

static bool limits_complete(SolMirRuntimeValuesLimits value) {
    return value.max_records != 0 && value.max_owned_bytes != 0
        && value.max_build_scratch_bytes != 0 && value.max_build_work != 0
        && value.max_validation_scratch_bytes != 0
        && value.max_validation_work != 0;
}

static bool range_valid(const void *pointer, size_t count, size_t size) {
    size_t bytes;
    return count == 0 ? pointer == NULL
        : pointer != NULL && mul_size(count, size, &bytes)
            && (uintptr_t)pointer <= UINTPTR_MAX - bytes;
}

static bool overlaps(const void *left, size_t left_count, size_t left_size,
    const void *right, size_t right_count, size_t right_size) {
    if (left_count == 0 || right_count == 0) return false;
    size_t left_bytes, right_bytes;
    if (!mul_size(left_count, left_size, &left_bytes)
        || !mul_size(right_count, right_size, &right_bytes)) return true;
    uintptr_t a = (uintptr_t)left, b = (uintptr_t)right;
    if (a > UINTPTR_MAX - left_bytes || b > UINTPTR_MAX - right_bytes)
        return true;
    return a < b + right_bytes && b < a + left_bytes;
}

static bool reconstruct_usage(const SolMirRuntimeConventions *conventions,
    size_t predecessor_work, size_t predecessor_scratch, size_t alias_work,
    SolMirRuntimeValuesUsage *usage) {
    const SolMirConcreteProgram *concrete = conventions->concrete;
    size_t records = concrete->representation.recipe_count;
    size_t owned_bytes, build_work = 0, validation_work = 0;
    if (!mul_size(records, sizeof(SolMirRuntimeRecipeOperations), &owned_bytes)
        || !mul_size(records, 2, &build_work)
        || !add_size(&build_work, concrete->linkage.runtime_requirement_count)
        || !add_size(&build_work, conventions->import_count)
        || !add_size(&build_work, conventions->import_count)
        || (conventions->import_count != 0 && !add_size(&build_work, 1))
        || (records != 0 && !add_size(&build_work, 1))) return false;
    size_t local_validation_scratch = records;
    if (!add_size(&local_validation_scratch, conventions->import_count))
        return false;
    size_t validation_scratch = predecessor_scratch;
    if (validation_scratch < local_validation_scratch)
        validation_scratch = local_validation_scratch;
    validation_work = predecessor_work;
    if (!add_size(&validation_work, VALIDATION_HEADER_WORK)
        || !add_size(&validation_work, alias_work)
        || !add_size(&validation_work, alias_work)
        || (local_validation_scratch != 0
            && !add_size(&validation_work, 1))
        || records > (SIZE_MAX - validation_work) / 6
        || !add_size(&validation_work, records * 6)
        || !add_size(&validation_work,
            concrete->linkage.runtime_requirement_count)
        || !add_size(&validation_work, conventions->import_count)) return false;
    *usage = (SolMirRuntimeValuesUsage){records, owned_bytes,
        conventions->import_count, build_work, validation_scratch,
        validation_work};
    return true;
}

static bool usage_fits(const SolMirRuntimeValuesUsage *usage,
    const SolMirRuntimeValuesLimits *limits) {
    return usage->records <= limits->max_records
        && usage->owned_bytes <= limits->max_owned_bytes
        && usage->build_scratch_bytes <= limits->max_build_scratch_bytes
        && usage->build_work <= limits->max_build_work
        && usage->validation_scratch_bytes
            <= limits->max_validation_scratch_bytes
        && usage->validation_work <= limits->max_validation_work;
}

static bool measured_text_size(const char *text, size_t *size) {
    size_t length = 0;
    do {
        if (!validation_event(1) || length == SIZE_MAX) return false;
    } while (text[length++] != '\0');
    *size = length;
    return true;
}

static bool walk_aliases(const SolMirRuntimeValues *values,
    const SolMirRuntimeConventions *runtime, bool check) {
    const void *owned = check ? values->recipe_operations : NULL;
    size_t count = check ? values->recipe_operation_capacity : 0;
    const SolMirConcreteProgram *c = runtime->concrete;
#define AGAINST(pointer, item_count, type) \
    if (!validation_event(1) || (check && overlaps(owned, count, \
            sizeof(*values->recipe_operations), (pointer), (item_count), \
            sizeof(type)))) return false
    AGAINST(values, 1, SolMirRuntimeValues);
    AGAINST(runtime, 1, SolMirRuntimeConventions);
#define RUNTIME_RANGE(member, type, singular) \
    AGAINST(runtime->member, runtime->singular##_capacity, type);
    SOL_MIR_RUNTIME_CONVENTIONS_ARENAS(RUNTIME_RANGE)
#undef RUNTIME_RANGE
    AGAINST(c, 1, SolMirConcreteProgram);
    AGAINST(&c->program, 1, SolMirProgram);
    AGAINST(c->program.roots, c->program.root_count, SolMirProgramRoot);
    AGAINST(c->program.approved_imports, c->program.approved_import_count,
        SolIrCallableId);
    AGAINST(c->program.templates, c->program.template_count,
        SolMirProgramTemplate);
    AGAINST(c->program.imports, c->program.import_count, SolMirProgramImport);
    AGAINST(c->program.specializations, c->program.specialization_count,
        SolMirProgramSpecialization);
    AGAINST(c->program.references, c->program.reference_count,
        SolMirProgramReference);
    for (size_t i = 0; i < c->program.template_count; ++i) {
        if (!validation_event(1)) return false;
        const SolMir *mir = &c->program.templates[i].mir;
        AGAINST(mir, 1, SolMir);
        AGAINST(mir->blocks, mir->block_capacity, SolMirBlock);
        AGAINST(mir->instructions, mir->instruction_capacity, SolMirInstruction);
        AGAINST(mir->values, mir->value_capacity, SolMirValue);
        AGAINST(mir->parameter_values, mir->parameter_value_capacity,
            SolMirValueId);
        AGAINST(mir->edge_values, mir->edge_value_capacity, SolMirValueId);
        AGAINST(mir->call_arguments, mir->call_argument_capacity,
            SolMirCallArgument);
        AGAINST(mir->loops, mir->loop_capacity, SolMirLoop);
        AGAINST(mir->construct_operands, mir->construct_operand_capacity,
            SolMirConstructOperand);
        AGAINST(mir->temporaries, mir->temporary_capacity, SolMirTemporary);
    }
    const SolMirPlan *p = &c->plan;
    AGAINST(p, 1, SolMirPlan);
#define PLAN_RANGE(member, type, singular) \
    AGAINST(p->member, p->singular##_capacity, type)
    PLAN_RANGE(types, SolMirPlanType, type);
    PLAN_RANGE(type_components, SolMirPlanTypeId, type_component);
    PLAN_RANGE(type_parameter_accesses, SolAccessMode, type_parameter_access);
    PLAN_RANGE(effect_atoms, SolMirPlanEffectAtom, effect_atom);
    PLAN_RANGE(effect_rows, SolMirPlanEffectRow, effect_row);
    PLAN_RANGE(effect_row_atoms, size_t, effect_row_atom);
    PLAN_RANGE(instances, SolMirPlanInstance, instance);
    PLAN_RANGE(instance_type_ids, SolMirPlanTypeId, instance_type_id);
    PLAN_RANGE(instance_accesses, SolAccessMode, instance_access);
    PLAN_RANGE(dictionary_entries, SolMirPlanDictionaryEntry, dictionary_entry);
    PLAN_RANGE(imports, SolMirPlanImport, import);
    PLAN_RANGE(typed_uses, SolMirPlanTypedUse, typed_use);
    PLAN_RANGE(contexts, SolMirPlanContext, context);
    PLAN_RANGE(demands, SolMirPlanDemand, demand);
#undef PLAN_RANGE
    const SolMirMaterialization *m = &c->materialization;
    AGAINST(m, 1, SolMirMaterialization);
#define MATERIAL_RANGE(member, type, singular) \
    AGAINST(m->member, m->singular##_capacity, type);
    MATERIAL_RANGE(images, SolMirMaterializedImage, image)
    MATERIAL_RANGE(types, SolMirMaterializedType, type)
    MATERIAL_RANGE(shape_fields, SolMirMaterializedShapeField, shape_field)
    MATERIAL_RANGE(shape_variants, SolMirMaterializedShapeVariant, shape_variant)
    MATERIAL_RANGE(type_ids, SolMirMaterializedTypeId, type_id)
    MATERIAL_RANGE(accesses, SolAccessMode, access)
    MATERIAL_RANGE(overlays, SolMirMaterializedTypeOverlay, overlay)
    MATERIAL_RANGE(contexts, SolMirPlanContext, context)
    MATERIAL_RANGE(locals, SolMirMaterializedLocal, local)
    MATERIAL_RANGE(places, SolMirMaterializedPlace, place)
    MATERIAL_RANGE(projections, SolMirMaterializedProjection, projection)
    MATERIAL_RANGE(values, SolMirMaterializedValue, value)
    MATERIAL_RANGE(instructions, SolMirMaterializedInstruction, instruction)
    MATERIAL_RANGE(temporaries, SolMirMaterializedTemporary, temporary)
    MATERIAL_RANGE(construct_operands, SolMirMaterializedConstructOperand,
        construct_operand)
    MATERIAL_RANGE(call_arguments, SolMirMaterializedCallArgument, call_argument)
    MATERIAL_RANGE(blocks, SolMirMaterializedBlock, block)
    MATERIAL_RANGE(edges, SolMirMaterializedEdge, edge)
    MATERIAL_RANGE(edge_values, SolMirMaterializedValueId, edge_value)
    MATERIAL_RANGE(parameter_values, SolMirMaterializedValueId, parameter_value)
    MATERIAL_RANGE(loops, SolMirMaterializedLoop, loop)
    MATERIAL_RANGE(bindings, SolMirMaterializedBinding, binding)
    MATERIAL_RANGE(semantic_sites, SolMirMaterializedSemanticSite, semantic_site)
    MATERIAL_RANGE(receiver_roots, SolMirMaterializedLocalId, receiver_root)
    MATERIAL_RANGE(imports, SolMirMaterializedImport, import)
    MATERIAL_RANGE(handlers, SolMirMaterializedHandler, handler)
    MATERIAL_RANGE(writebacks, SolMirMaterializedWriteback, writeback)
    MATERIAL_RANGE(effect_rows, SolMirMaterializedEffectRow, effect_row)
    MATERIAL_RANGE(effect_atoms, SolMirMaterializedEffectAtom, effect_atom)
    MATERIAL_RANGE(effect_row_atoms, size_t, effect_row_atom)
    MATERIAL_RANGE(effect_names, char, effect_name)
    MATERIAL_RANGE(literal_bytes, char, literal_byte)
#undef MATERIAL_RANGE
    for (size_t i = 0; i < m->image_count; ++i) {
        if (!validation_event(1)) return false;
        const SolMir *mir = &m->images[i].topology;
        AGAINST(mir, 1, SolMir);
        AGAINST(mir->blocks, mir->block_capacity, SolMirBlock);
        AGAINST(mir->instructions, mir->instruction_capacity, SolMirInstruction);
        AGAINST(mir->values, mir->value_capacity, SolMirValue);
        AGAINST(mir->parameter_values, mir->parameter_value_capacity,
            SolMirValueId);
        AGAINST(mir->edge_values, mir->edge_value_capacity, SolMirValueId);
        AGAINST(mir->call_arguments, mir->call_argument_capacity,
            SolMirCallArgument);
        AGAINST(mir->loops, mir->loop_capacity, SolMirLoop);
        AGAINST(mir->construct_operands, mir->construct_operand_capacity,
            SolMirConstructOperand);
        AGAINST(mir->temporaries, mir->temporary_capacity, SolMirTemporary);
    }
    const SolMirRepresentation *r = &c->representation;
    AGAINST(r, 1, SolMirRepresentation);
    AGAINST(r->recipes, r->recipe_capacity, SolMirRecipe);
    AGAINST(r->fields, r->field_capacity, SolMirRecipeField);
    AGAINST(r->variants, r->variant_capacity, SolMirRecipeVariant);
    AGAINST(r->recipe_ids, r->recipe_id_capacity, SolMirRecipeId);
    AGAINST(r->accesses, r->access_capacity, SolAccessMode);
    AGAINST(r->receiver_roots, r->receiver_root_capacity,
        SolMirMaterializedLocalId);
    AGAINST(r->callable_producers, r->callable_producer_capacity,
        SolMirCallableProducer);
    const SolMirLayout *layout = &c->layout;
    AGAINST(layout, 1, SolMirLayout);
    AGAINST(layout->types, layout->type_capacity, SolMirTypeLayout);
    AGAINST(layout->fields, layout->field_capacity, SolMirFieldLayout);
    AGAINST(layout->variants, layout->variant_capacity, SolMirVariantLayout);
    AGAINST(layout->projections, layout->projection_capacity,
        SolMirProjectionMap);
    AGAINST(&c->operations, 1, SolMirOperations);
#define OP_RANGE(member, type, singular) \
    AGAINST(c->operations.member, c->operations.singular##_capacity, type);
    SOL_MIR_OPERATIONS_ARENAS(OP_RANGE)
#undef OP_RANGE
    AGAINST(&c->linkage, 1, SolMirLinkage);
#define LINK_RANGE(member, type, singular) \
    AGAINST(c->linkage.member, c->linkage.singular##_capacity, type);
    SOL_MIR_LINKAGE_ARENAS(LINK_RANGE)
#undef LINK_RANGE
    const SolIr *ir = c->program.ir;
    AGAINST(ir, 1, SolIr);
#define IR_RANGE(member, item_count, type) \
    AGAINST(ir->member, ir->item_count, type)
    IR_RANGE(definitions, definition_count, SolIrDefinition);
    IR_RANGE(callables, callable_count, SolIrCallable);
    IR_RANGE(types, type_count, SolIrType);
    IR_RANGE(type_ids, type_id_count, SolIrTypeId);
    IR_RANGE(accesses, access_count, SolAccessMode);
    IR_RANGE(members, member_count, SolIrMember);
    IR_RANGE(evidence, evidence_count, SolIrDispatchEvidence);
    IR_RANGE(locals, local_count, SolIrLocal);
    IR_RANGE(fields, field_count, SolIrField);
    IR_RANGE(variants, variant_count, SolIrVariant);
    IR_RANGE(expressions, expression_count, SolIrExpression);
    IR_RANGE(places, place_count, SolIrPlace);
    IR_RANGE(projections, projection_count, SolIrProjection);
    IR_RANGE(statements, statement_count, SolIrStatement);
    IR_RANGE(statement_ids, statement_id_count, SolIrStatementId);
    IR_RANGE(arms, arm_count, SolIrArm);
    IR_RANGE(arm_ids, arm_id_count, SolIrArmId);
    IR_RANGE(patterns, pattern_count, SolIrPattern);
    IR_RANGE(pattern_children, pattern_child_count, SolIrPatternChild);
    IR_RANGE(operands, operand_count, SolIrOperand);
    IR_RANGE(roots, root_count, SolIrLocalId);
    IR_RANGE(obligations, obligation_count, SolIrObligation);
    IR_RANGE(snapshots, snapshot_count, SolIrSnapshot);
    IR_RANGE(cleanup_locals, cleanup_local_count, SolIrLocalId);
    IR_RANGE(effects, effect_count, SolIrEffect);
    IR_RANGE(generic_parameters, generic_parameter_count, SolIrGenericParameter);
    IR_RANGE(effect_parameters, effect_parameter_count, SolIrEffectParameter);
    IR_RANGE(loop_obligations, loop_obligation_count, SolObligationId);
    IR_RANGE(unreachable_obligations, unreachable_obligation_count,
        SolObligationId);
    IR_RANGE(files, file_count, SolIrSourceFile);
#undef IR_RANGE
    size_t text_size;
    if (!measured_text_size(ir->source_path, &text_size)) return false;
    AGAINST(ir->source_path, text_size, char);
    AGAINST(ir->source_bytes, ir->source_length + 1, char);
    for (size_t i = 0; i < p->effect_atom_count; ++i) {
        if (!validation_event(1)) return false;
        AGAINST(p->effect_atoms[i].name, p->effect_atoms[i].length + 1, char);
    }
#define OPTIONAL_TEXT(array, item_count, member) do { \
    for (size_t text_i = 0; text_i < (item_count); ++text_i) { \
        if (!validation_event(1)) return false; \
        if ((array)[text_i].member != NULL) { \
            if (!measured_text_size((array)[text_i].member, &text_size)) \
                return false; \
            AGAINST((array)[text_i].member, text_size, char); \
        } \
    } \
} while (0)
    OPTIONAL_TEXT(ir->definitions, ir->definition_count, name);
    OPTIONAL_TEXT(ir->callables, ir->callable_count, name);
    OPTIONAL_TEXT(ir->locals, ir->local_count, name);
    OPTIONAL_TEXT(ir->fields, ir->field_count, name);
    OPTIONAL_TEXT(ir->variants, ir->variant_count, name);
    OPTIONAL_TEXT(ir->statements, ir->statement_count, region_label);
#undef OPTIONAL_TEXT
    for (size_t i = 0; i < ir->effect_count; ++i) {
        if (!validation_event(1)
            || !measured_text_size(ir->effects[i].name, &text_size)) return false;
        AGAINST(ir->effects[i].name, text_size, char);
    }
    for (size_t i = 0; i < ir->expression_count; ++i) {
        if (!validation_event(1)) return false;
        if (ir->expressions[i].kind == SOL_IR_EXPR_STRING) {
            if (!measured_text_size(ir->expressions[i].as.string, &text_size))
                return false;
            AGAINST(ir->expressions[i].as.string, text_size, char);
        } else if (ir->expressions[i].kind == SOL_IR_EXPR_HANDLE) {
            if (!measured_text_size(ir->expressions[i].as.handler.effect_name,
                    &text_size)) return false;
            AGAINST(ir->expressions[i].as.handler.effect_name, text_size, char);
        }
    }
    for (size_t i = 0; i < ir->generic_parameter_count; ++i) {
        if (!validation_event(1)
            || !measured_text_size(ir->generic_parameters[i].name, &text_size))
            return false;
        AGAINST(ir->generic_parameters[i].name, text_size, char);
    }
    for (size_t i = 0; i < ir->effect_parameter_count; ++i) {
        if (!validation_event(1)
            || !measured_text_size(ir->effect_parameters[i].name, &text_size))
            return false;
        AGAINST(ir->effect_parameters[i].name, text_size, char);
    }
    for (size_t i = 0; i < ir->file_count; ++i) {
        if (!validation_event(1)
            || !measured_text_size(ir->files[i].path, &text_size)) return false;
        AGAINST(ir->files[i].path, text_size, char);
    }
#undef AGAINST
    return true;
}

static uint32_t value_operation_mask(void) {
    return SOL_MIR_LINKAGE_RUNTIME_CREATE | SOL_MIR_LINKAGE_RUNTIME_COPY
        | SOL_MIR_LINKAGE_RUNTIME_DROP | SOL_MIR_LINKAGE_RUNTIME_EQUAL;
}

static SolMirRuntimeImportId record_import(
    const SolMirRuntimeRecipeOperations *record, uint32_t operation) {
    if (operation == SOL_MIR_LINKAGE_RUNTIME_CREATE) return record->create_import;
    if (operation == SOL_MIR_LINKAGE_RUNTIME_COPY) return record->copy_import;
    if (operation == SOL_MIR_LINKAGE_RUNTIME_DROP) return record->drop_import;
    if (operation == SOL_MIR_LINKAGE_RUNTIME_EQUAL) return record->equal_import;
    return SOL_MIR_RUNTIME_NONE;
}

static SolMirRuntimeImportKind import_kind(uint32_t operation) {
    if (operation == SOL_MIR_LINKAGE_RUNTIME_CREATE)
        return SOL_MIR_RUNTIME_IMPORT_RECIPE_CREATE;
    if (operation == SOL_MIR_LINKAGE_RUNTIME_COPY)
        return SOL_MIR_RUNTIME_IMPORT_RECIPE_COPY;
    if (operation == SOL_MIR_LINKAGE_RUNTIME_DROP)
        return SOL_MIR_RUNTIME_IMPORT_RECIPE_DROP;
    return SOL_MIR_RUNTIME_IMPORT_RECIPE_EQUAL;
}

static bool validate_records(const SolMirRuntimeValues *values,
    unsigned char *requirement_seen, unsigned char *import_consumed) {
    const SolMirRuntimeConventions *runtime = values->conventions;
    const SolMirLinkage *linkage = &runtime->concrete->linkage;
    for (size_t recipe = 0; recipe < values->recipe_operation_count; ++recipe) {
        if (!validation_event(1)) return false;
        const SolMirRuntimeRecipeOperations *record
            = &values->recipe_operations[recipe];
        if (record->recipe != recipe
            || (record->demanded_operations & ~value_operation_mask()) != 0)
            return false;
    }
    for (size_t i = 0; i < linkage->runtime_requirement_count; ++i) {
        if (!validation_event(1)) return false;
        const SolMirLinkageRuntimeRequirement *requirement
            = &linkage->runtime_requirements[i];
        if (requirement->recipe >= values->recipe_operation_count
            || requirement_seen[requirement->recipe]) return false;
        requirement_seen[requirement->recipe] = 1;
        if (values->recipe_operations[requirement->recipe].demanded_operations
                != (requirement->operations & value_operation_mask())) return false;
    }
    for (size_t recipe = 0; recipe < values->recipe_operation_count; ++recipe) {
        if (!validation_event(1)) return false;
        const SolMirRuntimeRecipeOperations *record
            = &values->recipe_operations[recipe];
        uint32_t expected = record->demanded_operations;
        if (!requirement_seen[recipe] && expected != 0) return false;
        const uint32_t operations[] = {SOL_MIR_LINKAGE_RUNTIME_CREATE,
            SOL_MIR_LINKAGE_RUNTIME_COPY, SOL_MIR_LINKAGE_RUNTIME_DROP,
            SOL_MIR_LINKAGE_RUNTIME_EQUAL};
        for (size_t q = 0; q < sizeof(operations) / sizeof(operations[0]); ++q) {
            if (!validation_event(1)) return false;
            size_t id = record_import(record, operations[q]);
            bool demanded = (expected & operations[q]) != 0;
            if (!demanded) {
                if (id != SOL_MIR_RUNTIME_NONE) return false;
                continue;
            }
            if (id >= runtime->import_count || import_consumed[id]) return false;
            const SolMirRuntimeImport *import = &runtime->imports[id];
            if (import->recipe != recipe
                || import->recipe_operation != operations[q]
                || import->kind != import_kind(operations[q])) return false;
            import_consumed[id] = 1;
        }
    }
    for (size_t i = 0; i < runtime->import_count; ++i) {
        if (!validation_event(1)) return false;
        bool value_import = runtime->imports[i].kind
                >= SOL_MIR_RUNTIME_IMPORT_RECIPE_CREATE
            && runtime->imports[i].kind <= SOL_MIR_RUNTIME_IMPORT_RECIPE_EQUAL;
        if (value_import != (import_consumed[i] != 0)) return false;
    }
    return true;
}

static SolMirRuntimeValuesBuildOutcome validate_predecessor(
    const SolMirRuntimeConventions *conventions, size_t *work, size_t *scratch,
    SolDiagnostics *diagnostics) {
    SolDiagnostics local;
    sol_diagnostics_init(&local);
    bool valid = conventions != NULL
        && sol_mir_runtime_conventions_internal_validation_requirements(
            conventions, work, scratch, &local)
        && *work == conventions->usage.validation_work
        && *scratch == conventions->usage.validation_scratch_bytes;
    bool allocation_failed = local.allocation_failed;
    sol_diagnostics_free(&local);
    if (valid) return SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED;
    if (allocation_failed) {
        if (diagnostics != NULL) diagnostics->allocation_failed = true;
        invalid(diagnostics, "runtime values predecessor validation allocation failed");
        return SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED;
    }
    invalid(diagnostics, "runtime values predecessor is invalid");
    return SOL_MIR_RUNTIME_VALUES_BUILD_INVALID_CONVENTIONS;
}

static bool measure_alias_work(const SolMirRuntimeConventions *conventions,
    size_t limit, size_t *work) {
    metered_validation_work = 0;
    metered_validation_limit = limit;
    validation_work_exhausted = false;
    if (!walk_aliases(NULL, conventions, false)) return false;
    *work = metered_validation_work;
    return true;
}

SolMirRuntimeValuesBuildOutcome sol_mir_runtime_values_internal_preflight(
    const SolMirRuntimeConventions *conventions,
    const SolMirRuntimeValuesLimits *limits, SolMirRuntimeValuesUsage *usage,
    SolDiagnostics *diagnostics) {
    if (conventions == NULL || limits == NULL || usage == NULL
        || !limits_complete(*limits))
        return SOL_MIR_RUNTIME_VALUES_BUILD_INVALID_ARGUMENT;
    size_t predecessor_work = 0, predecessor_scratch = 0;
    SolMirRuntimeValuesBuildOutcome predecessor = validate_predecessor(
        conventions, &predecessor_work, &predecessor_scratch, diagnostics);
    if (predecessor != SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED)
        return predecessor;
    size_t alias_work;
    if (!measure_alias_work(conventions, SIZE_MAX, &alias_work)
        || !reconstruct_usage(conventions, predecessor_work,
            predecessor_scratch, alias_work, usage)) {
        invalid(diagnostics, "runtime values resource reconstruction overflowed");
        return SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED;
    }
    if (!usage_fits(usage, limits)) {
        invalid(diagnostics, "runtime values preflight resource limit exceeded");
        return SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED;
    }
    return SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED;
}

SolMirRuntimeValuesBuildOutcome sol_mir_runtime_values_internal_validate(
    const SolMirRuntimeValues *values, SolDiagnostics *diagnostics) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    validation_allocation_attempts = 0;
#endif
    if (values == NULL || values->conventions == NULL)
        {
            invalid(diagnostics, "runtime values owner is empty");
            return SOL_MIR_RUNTIME_VALUES_BUILD_INTERNAL_FAILED;
        }
    if (!limits_complete(values->limits)) {
        invalid(diagnostics, "runtime values limits are incomplete");
        return SOL_MIR_RUNTIME_VALUES_BUILD_INTERNAL_FAILED;
    }
    size_t predecessor_work = 0, predecessor_scratch = 0;
    SolMirRuntimeValuesBuildOutcome predecessor = validate_predecessor(
        values->conventions, &predecessor_work, &predecessor_scratch,
        diagnostics);
    if (predecessor != SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED)
        return predecessor;
    if (predecessor_work > values->limits.max_validation_work) {
        invalid(diagnostics, "runtime values predecessor work limit exceeded");
        return SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED;
    }
    metered_validation_work = 0;
    metered_validation_limit
        = values->limits.max_validation_work - predecessor_work;
    validation_work_exhausted = false;
    if (!validation_event(VALIDATION_HEADER_WORK)) {
        invalid(diagnostics, "runtime values validation work limit exceeded");
        return SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED;
    }
    size_t alias_start = metered_validation_work;
    if (!walk_aliases(NULL, values->conventions, false)) {
        invalid(diagnostics, "runtime values validation preflight work exceeded");
        return SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED;
    }
    size_t alias_work = metered_validation_work - alias_start;
    SolMirRuntimeValuesUsage expected;
    if (!reconstruct_usage(values->conventions, predecessor_work,
            predecessor_scratch, alias_work, &expected)
        || values->recipe_operation_count != expected.records
        || values->recipe_operation_capacity != expected.records
        || !range_valid(values->recipe_operations,
            values->recipe_operation_capacity,
            sizeof(*values->recipe_operations))
        || !usage_fits(&expected, &values->limits)
        || memcmp(&expected, &values->usage, sizeof(expected)) != 0) {
        invalid(diagnostics, "runtime values preflight or usage is invalid");
        return SOL_MIR_RUNTIME_VALUES_BUILD_INTERNAL_FAILED;
    }
    if (!walk_aliases(values, values->conventions, true)) {
        invalid(diagnostics, "runtime values owner aliases predecessor data");
        return validation_work_exhausted
            ? SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED
            : SOL_MIR_RUNTIME_VALUES_BUILD_INTERNAL_FAILED;
    }
    size_t recipes = values->recipe_operation_count;
    size_t imports = values->conventions->import_count;
    size_t scratch_bytes = recipes;
    if (!add_size(&scratch_bytes, imports)) {
        invalid(diagnostics, "runtime values validation scratch overflow");
        return SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED;
    }
    unsigned char *scratch = NULL;
    if (scratch_bytes != 0) {
        if (!validation_event(1)) {
            invalid(diagnostics, "runtime values validation work limit exceeded");
            return SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED;
        }
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        ++validation_allocation_attempts;
        if (!force_validation_allocation_failure)
#endif
            scratch = calloc(scratch_bytes, 1);
        if (scratch == NULL) {
            if (diagnostics != NULL) diagnostics->allocation_failed = true;
            invalid(diagnostics,
                "runtime values validation scratch allocation failed");
            return SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED;
        }
    }
    unsigned char *import_consumed = scratch == NULL ? NULL : scratch + recipes;
    bool ok = validate_records(values, scratch, import_consumed);
    free(scratch);
    if (!ok) {
        invalid(diagnostics, "runtime values records are not reconstructive");
        return validation_work_exhausted
            ? SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED
            : SOL_MIR_RUNTIME_VALUES_BUILD_INTERNAL_FAILED;
    }
    size_t total_work = predecessor_work;
    if (!add_size(&total_work, metered_validation_work)
        || total_work != expected.validation_work) {
        invalid(diagnostics, "runtime values validation work is not exact");
        return SOL_MIR_RUNTIME_VALUES_BUILD_INTERNAL_FAILED;
    }
    return SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED;
}

bool sol_mir_runtime_values_validate(const SolMirRuntimeValues *values,
    SolDiagnostics *diagnostics) {
    return sol_mir_runtime_values_internal_validate(values, diagnostics)
        == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
bool sol_mir_runtime_values_test_reconstruct_usage(
    const SolMirRuntimeConventions *conventions,
    const SolMirRuntimeValuesLimits *limits, SolMirRuntimeValuesUsage *usage) {
    return sol_mir_runtime_values_internal_preflight(conventions, limits, usage,
        NULL) == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED;
}
#endif
