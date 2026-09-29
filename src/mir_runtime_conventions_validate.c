#include "sol/mir_runtime_conventions.h"
#include "mir_concrete_internal.h"
#include "mir_linkage_internal.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local size_t metered_work;
static _Thread_local size_t metered_limit;
static _Thread_local bool metering_validation;
static _Thread_local bool reconstruction_overflow;
static _Thread_local size_t preflight_steps;

#ifdef SOL_MIR_PLAN_TEST_HOOKS
extern _Thread_local bool sol_mir_runtime_force_host_collision;
extern _Thread_local bool sol_mir_runtime_force_recipe_collision;
extern _Thread_local bool sol_mir_runtime_force_host_symbol_collision;
extern _Thread_local bool sol_mir_runtime_force_recipe_symbol_collision;
static _Thread_local size_t observed_validation_work;

size_t sol_mir_runtime_conventions_test_observed_validation_work(void) {
    return observed_validation_work;
}
#endif

static bool invalid(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) sol_diagnostics_add(diagnostics,
        "SOL-MIR-RUNTIME-CONVENTIONS-002", SOL_SEVERITY_ERROR, (SolSpan){0},
        message);
    return false;
}

static bool add_size(size_t *value, size_t amount) {
    if (amount > SIZE_MAX - *value) return false;
    *value += amount;
    return true;
}

static bool tick(size_t amount) {
    if (!metering_validation) return true;
    return add_size(&metered_work, amount) && metered_work <= metered_limit;
}

static bool preflight_step(void) {
    return tick(1) && add_size(&preflight_steps, 1);
}

static bool preflight_charge(size_t amount) {
    return tick(amount) && add_size(&preflight_steps, amount);
}

static bool preflight_slice(SolMirPlanSlice value, size_t count) {
    return preflight_step() && value.offset <= count
        && value.count <= count - value.offset;
}

static bool mul_size(size_t left, size_t right, size_t *result) {
    if (left != 0 && right > SIZE_MAX / left) return false;
    *result = left * right;
    return true;
}

static bool limits_complete(SolMirRuntimeConventionsLimits v) {
#define REQUIRED(member) v.member != 0
    return REQUIRED(max_signatures) && REQUIRED(max_signature_slots)
        && REQUIRED(max_calls) && REQUIRED(max_operands)
        && REQUIRED(max_writebacks) && REQUIRED(max_entries)
        && REQUIRED(max_imports) && REQUIRED(max_failure_sites)
        && REQUIRED(max_owned_bytes)
        && REQUIRED(max_build_scratch_bytes) && REQUIRED(max_build_work)
        && REQUIRED(max_validation_scratch_bytes)
        && REQUIRED(max_validation_work);
#undef REQUIRED
}

static bool range_valid(const void *pointer, size_t count, size_t size) {
    if (count == 0) return pointer == NULL;
    size_t bytes;
    return pointer != NULL && mul_size(count, size, &bytes)
        && (uintptr_t)pointer <= UINTPTR_MAX - bytes;
}

static bool overlaps(const void *a, size_t ac, size_t as,
    const void *b, size_t bc, size_t bs) {
    if (ac == 0 || bc == 0) return false;
    size_t ab, bb;
    if (!mul_size(ac, as, &ab) || !mul_size(bc, bs, &bb)) return true;
    uintptr_t ap = (uintptr_t)a, bp = (uintptr_t)b;
    if (ap > UINTPTR_MAX - ab || bp > UINTPTR_MAX - bb) return true;
    return ap < bp + bb && bp < ap + ab;
}

typedef struct { const void *pointer; size_t count, size; } Range;

static bool validate_aliases(const SolMirRuntimeConventions *owner) {
    Range owned[] = {
#define OWNER_RANGE(member, type, singular) \
        {owner->member, owner->singular##_capacity, sizeof(type)},
        SOL_MIR_RUNTIME_CONVENTIONS_ARENAS(OWNER_RANGE)
#undef OWNER_RANGE
    };
    for (size_t i = 0; i < sizeof(owned) / sizeof(owned[0]); ++i) {
        if (!tick(1)) return false;
        if (overlaps(owned[i].pointer, owned[i].count, owned[i].size,
                owner, 1, sizeof(*owner))) return false;
        for (size_t q = i + 1; q < sizeof(owned) / sizeof(owned[0]); ++q) {
            if (!tick(1)) return false;
            if (overlaps(owned[i].pointer, owned[i].count, owned[i].size,
                    owned[q].pointer, owned[q].count, owned[q].size)) return false;
        }
    }
    const SolMirConcreteProgram *c = owner->concrete;
#define AGAINST(ptr_value, count_value, type) do { \
    if (!tick(1)) return false; \
    for (size_t z = 0; z < sizeof(owned) / sizeof(owned[0]); ++z) { \
        if (!tick(1)) return false; \
        if (overlaps(owned[z].pointer, owned[z].count, owned[z].size, \
                (ptr_value), (count_value), sizeof(type))) return false; \
    } \
} while (0)
#define FIELD(object, member, type, singular) \
    AGAINST((object).member, (object).singular##_capacity, type);
    AGAINST(c, 1, SolMirConcreteProgram);
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
        if (!tick(1)) return false;
        const SolMir *mir = &c->program.templates[i].mir;
        AGAINST(mir, 1, SolMir);
        AGAINST(mir->blocks, mir->block_capacity, SolMirBlock);
        AGAINST(mir->instructions, mir->instruction_capacity, SolMirInstruction);
        AGAINST(mir->values, mir->value_capacity, SolMirValue);
        AGAINST(mir->parameter_values, mir->parameter_value_capacity, SolMirValueId);
        AGAINST(mir->edge_values, mir->edge_value_capacity, SolMirValueId);
        AGAINST(mir->call_arguments, mir->call_argument_capacity, SolMirCallArgument);
        AGAINST(mir->loops, mir->loop_capacity, SolMirLoop);
        AGAINST(mir->construct_operands, mir->construct_operand_capacity,
            SolMirConstructOperand);
        AGAINST(mir->temporaries, mir->temporary_capacity, SolMirTemporary);
    }
    const SolMirPlan *p = &c->plan;
    AGAINST(p->types, p->type_capacity, SolMirPlanType);
    AGAINST(p->type_components, p->type_component_capacity, SolMirPlanTypeId);
    AGAINST(p->type_parameter_accesses, p->type_parameter_access_capacity,
        SolAccessMode);
    AGAINST(p->effect_atoms, p->effect_atom_capacity, SolMirPlanEffectAtom);
    AGAINST(p->effect_rows, p->effect_row_capacity, SolMirPlanEffectRow);
    AGAINST(p->effect_row_atoms, p->effect_row_atom_capacity, size_t);
    AGAINST(p->instances, p->instance_capacity, SolMirPlanInstance);
    AGAINST(p->instance_type_ids, p->instance_type_id_capacity, SolMirPlanTypeId);
    AGAINST(p->instance_accesses, p->instance_access_capacity, SolAccessMode);
    AGAINST(p->dictionary_entries, p->dictionary_entry_capacity,
        SolMirPlanDictionaryEntry);
    AGAINST(p->imports, p->import_capacity, SolMirPlanImport);
    AGAINST(p->typed_uses, p->typed_use_capacity, SolMirPlanTypedUse);
    AGAINST(p->contexts, p->context_capacity, SolMirPlanContext);
    AGAINST(p->demands, p->demand_capacity, SolMirPlanDemand);
    const SolMirMaterialization *m = &c->materialization;
    FIELD((*m), images, SolMirMaterializedImage, image)
    FIELD((*m), types, SolMirMaterializedType, type)
    FIELD((*m), shape_fields, SolMirMaterializedShapeField, shape_field)
    FIELD((*m), shape_variants, SolMirMaterializedShapeVariant, shape_variant)
    FIELD((*m), type_ids, SolMirMaterializedTypeId, type_id)
    FIELD((*m), accesses, SolAccessMode, access)
    FIELD((*m), overlays, SolMirMaterializedTypeOverlay, overlay)
    FIELD((*m), contexts, SolMirPlanContext, context)
    FIELD((*m), locals, SolMirMaterializedLocal, local)
    FIELD((*m), places, SolMirMaterializedPlace, place)
    FIELD((*m), projections, SolMirMaterializedProjection, projection)
    FIELD((*m), values, SolMirMaterializedValue, value)
    FIELD((*m), instructions, SolMirMaterializedInstruction, instruction)
    FIELD((*m), temporaries, SolMirMaterializedTemporary, temporary)
    FIELD((*m), construct_operands, SolMirMaterializedConstructOperand,
        construct_operand)
    FIELD((*m), call_arguments, SolMirMaterializedCallArgument, call_argument)
    FIELD((*m), blocks, SolMirMaterializedBlock, block)
    FIELD((*m), edges, SolMirMaterializedEdge, edge)
    FIELD((*m), edge_values, SolMirMaterializedValueId, edge_value)
    FIELD((*m), parameter_values, SolMirMaterializedValueId, parameter_value)
    FIELD((*m), loops, SolMirMaterializedLoop, loop)
    FIELD((*m), bindings, SolMirMaterializedBinding, binding)
    FIELD((*m), semantic_sites, SolMirMaterializedSemanticSite, semantic_site)
    FIELD((*m), receiver_roots, SolMirMaterializedLocalId, receiver_root)
    FIELD((*m), imports, SolMirMaterializedImport, import)
    FIELD((*m), handlers, SolMirMaterializedHandler, handler)
    FIELD((*m), writebacks, SolMirMaterializedWriteback, writeback)
    FIELD((*m), effect_rows, SolMirMaterializedEffectRow, effect_row)
    FIELD((*m), effect_atoms, SolMirMaterializedEffectAtom, effect_atom)
    FIELD((*m), effect_row_atoms, size_t, effect_row_atom)
    FIELD((*m), effect_names, char, effect_name)
    FIELD((*m), literal_bytes, char, literal_byte)
    for (size_t i = 0; i < m->image_count; ++i) {
        if (!tick(1)) return false;
        const SolMir *mir = &m->images[i].topology;
        AGAINST(mir, 1, SolMir);
        AGAINST(mir->blocks, mir->block_capacity, SolMirBlock);
        AGAINST(mir->instructions, mir->instruction_capacity, SolMirInstruction);
        AGAINST(mir->values, mir->value_capacity, SolMirValue);
        AGAINST(mir->parameter_values, mir->parameter_value_capacity, SolMirValueId);
        AGAINST(mir->edge_values, mir->edge_value_capacity, SolMirValueId);
        AGAINST(mir->call_arguments, mir->call_argument_capacity, SolMirCallArgument);
        AGAINST(mir->loops, mir->loop_capacity, SolMirLoop);
        AGAINST(mir->construct_operands, mir->construct_operand_capacity,
            SolMirConstructOperand);
        AGAINST(mir->temporaries, mir->temporary_capacity, SolMirTemporary);
    }
    const SolMirRepresentation *r = &c->representation;
    FIELD((*r), recipes, SolMirRecipe, recipe)
    FIELD((*r), fields, SolMirRecipeField, field)
    FIELD((*r), variants, SolMirRecipeVariant, variant)
    FIELD((*r), recipe_ids, SolMirRecipeId, recipe_id)
    FIELD((*r), accesses, SolAccessMode, access)
    FIELD((*r), receiver_roots, SolMirMaterializedLocalId, receiver_root)
    FIELD((*r), callable_producers, SolMirCallableProducer, callable_producer)
    const SolMirLayout *layout = &c->layout;
    FIELD((*layout), types, SolMirTypeLayout, type)
    FIELD((*layout), fields, SolMirFieldLayout, field)
    FIELD((*layout), variants, SolMirVariantLayout, variant)
    FIELD((*layout), projections, SolMirProjectionMap, projection)
#define OP_RANGE(member, type, singular) FIELD(c->operations, member, type, singular)
    SOL_MIR_OPERATIONS_ARENAS(OP_RANGE)
#undef OP_RANGE
#define LINK_RANGE(member, type, singular) FIELD(c->linkage, member, type, singular)
    SOL_MIR_LINKAGE_ARENAS(LINK_RANGE)
#undef LINK_RANGE
    const SolIr *ir = c->program.ir;
    if (ir == NULL) return false;
    AGAINST(ir, 1, SolIr);
#define IR_RANGE(member, count, type) AGAINST(ir->member, ir->count, type)
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
    if (ir->source_length == SIZE_MAX) return false;
    AGAINST(ir->source_path, 1, char);
    AGAINST(ir->source_bytes, ir->source_length + 1, char);
    for (size_t i = 0; i < p->effect_atom_count; ++i) {
        if (!tick(1)) return false;
        AGAINST(p->effect_atoms[i].name, 1, char);
    }
#define OPTIONAL_TEXT(array, count, member) do { \
    for (size_t text_i = 0; text_i < (count); ++text_i) { \
        if (!tick(1)) return false; \
        if ((array)[text_i].member != NULL) \
            AGAINST((array)[text_i].member, 1, char); \
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
        if (!tick(1)) return false;
        AGAINST(ir->effects[i].name, 1, char);
    }
    for (size_t i = 0; i < ir->expression_count; ++i) {
        if (!tick(1)) return false;
        if (ir->expressions[i].kind == SOL_IR_EXPR_STRING)
            AGAINST(ir->expressions[i].as.string, 1, char);
        else if (ir->expressions[i].kind == SOL_IR_EXPR_HANDLE)
            AGAINST(ir->expressions[i].as.handler.effect_name, 1, char);
    }
    for (size_t i = 0; i < ir->generic_parameter_count; ++i) {
        if (!tick(1)) return false;
        AGAINST(ir->generic_parameters[i].name, 1, char);
    }
    for (size_t i = 0; i < ir->effect_parameter_count; ++i) {
        if (!tick(1)) return false;
        AGAINST(ir->effect_parameters[i].name, 1, char);
    }
    for (size_t i = 0; i < ir->file_count; ++i) {
        if (!tick(1)) return false;
        AGAINST(ir->files[i].path, 1, char);
    }
#undef FIELD
#undef AGAINST
    return true;
}

static bool measured_text_size(const char *text, size_t *size) {
    size_t length = 0;
    do {
        if (!tick(1) || length == SIZE_MAX) return false;
    } while (text[length++] != '\0');
    *size = length;
    return true;
}

static bool validate_text_aliases(const SolMirRuntimeConventions *owner) {
    Range owned[] = {
#define OWNER_RANGE(member, type, singular) \
        {owner->member, owner->singular##_capacity, sizeof(type)},
        SOL_MIR_RUNTIME_CONVENTIONS_ARENAS(OWNER_RANGE)
#undef OWNER_RANGE
    };
    const SolMirConcreteProgram *c = owner->concrete;
    const SolIr *ir = c->program.ir;
    size_t bytes;
#define TEXT_RANGE(text_pointer, text_count) do { \
    if (!tick(1)) return false; \
    for (size_t range_i = 0; range_i < sizeof(owned) / sizeof(owned[0]); \
            ++range_i) { \
        if (!tick(1)) return false; \
        if (overlaps(owned[range_i].pointer, owned[range_i].count, \
                owned[range_i].size, (text_pointer), (text_count), \
                sizeof(char))) \
            return false; \
    } \
} while (0)
#define MEASURED_TEXT(text_pointer) do { \
    if (!measured_text_size((text_pointer), &bytes)) return false; \
    TEXT_RANGE((text_pointer), bytes); \
} while (0)
    MEASURED_TEXT(ir->source_path);
    TEXT_RANGE(ir->source_bytes, ir->source_length + 1);
    for (size_t i = 0; i < c->plan.effect_atom_count; ++i) {
        if (!tick(1)) return false;
        TEXT_RANGE(c->plan.effect_atoms[i].name,
            c->plan.effect_atoms[i].length + 1);
    }
#define OPTIONAL_TEXT(array, count, member) do { \
    for (size_t text_i = 0; text_i < (count); ++text_i) { \
        if (!tick(1)) return false; \
        if ((array)[text_i].member != NULL) \
            MEASURED_TEXT((array)[text_i].member); \
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
        if (!tick(1)) return false;
        MEASURED_TEXT(ir->effects[i].name);
    }
    for (size_t i = 0; i < ir->expression_count; ++i) {
        if (!tick(1)) return false;
        if (ir->expressions[i].kind == SOL_IR_EXPR_STRING)
            MEASURED_TEXT(ir->expressions[i].as.string);
        else if (ir->expressions[i].kind == SOL_IR_EXPR_HANDLE)
            MEASURED_TEXT(ir->expressions[i].as.handler.effect_name);
    }
    for (size_t i = 0; i < ir->generic_parameter_count; ++i) {
        if (!tick(1)) return false;
        MEASURED_TEXT(ir->generic_parameters[i].name);
    }
    for (size_t i = 0; i < ir->effect_parameter_count; ++i) {
        if (!tick(1)) return false;
        MEASURED_TEXT(ir->effect_parameters[i].name);
    }
    for (size_t i = 0; i < ir->file_count; ++i) {
        if (!tick(1)) return false;
        MEASURED_TEXT(ir->files[i].path);
    }
#undef MEASURED_TEXT
#undef TEXT_RANGE
    return true;
}

static SolMirRuntimeResultClass result_class(const SolMirRepresentation *r,
    SolMirRecipeId recipe) {
    if (recipe >= r->recipe_count) return (SolMirRuntimeResultClass)99;
    if (r->recipes[recipe].kind == SOL_MIR_RECIPE_UNIT)
        return SOL_MIR_RUNTIME_RESULT_UNIT;
    if (r->recipes[recipe].kind == SOL_MIR_RECIPE_NEVER)
        return SOL_MIR_RUNTIME_RESULT_NEVER;
    return SOL_MIR_RUNTIME_RESULT_VALUE;
}

static bool program_source_equal(SolMirProgramSource a, SolMirProgramSource b) {
    return a.callable == b.callable && a.expression == b.expression
        && a.file == b.file && a.start == b.start && a.end == b.end;
}

static bool slice(SolMirRuntimeSlice value, size_t count) {
    return value.offset <= count && value.count <= count - value.offset;
}

static bool validate_headers(const SolMirRuntimeConventions *owner) {
    if (owner == NULL || owner->concrete == NULL || !limits_complete(owner->limits))
        return false;
    size_t owned_bytes = 0;
#define HEADER(member, type, singular) \
    size_t member##_bytes; \
    if (!tick(1) || owner->singular##_count != owner->singular##_capacity \
        || !range_valid(owner->member, owner->singular##_capacity, sizeof(type)) \
        || !mul_size(owner->singular##_capacity, sizeof(type), \
            &member##_bytes) \
        || !add_size(&owned_bytes, member##_bytes)) \
        return false;
    SOL_MIR_RUNTIME_CONVENTIONS_ARENAS(HEADER)
#undef HEADER
    if (owner->signature_count > owner->limits.max_signatures
        || owner->signature_slot_count > owner->limits.max_signature_slots
        || owner->call_count > owner->limits.max_calls
        || owner->operand_count > owner->limits.max_operands
        || owner->writeback_count > owner->limits.max_writebacks
        || owner->entry_count > owner->limits.max_entries
        || owner->import_count > owner->limits.max_imports
        || owner->failure_site_count > owner->limits.max_failure_sites
        || owned_bytes != owner->usage.owned_bytes
        || owned_bytes > owner->limits.max_owned_bytes) return false;
    return true;
}

static bool validate_slices(const SolMirRuntimeConventions *owner) {
    for (size_t i = 0; i < owner->signature_count; ++i) {
        if (!tick(1)
            || !slice(owner->signatures[i].slots,
                owner->signature_slot_count)) return false;
    }
    for (size_t i = 0; i < owner->call_count; ++i) {
        if (!tick(1)
            || !slice(owner->calls[i].operands, owner->operand_count)
            || !slice(owner->calls[i].writebacks,
                owner->writeback_count)) return false;
    }
    return true;
}

static bool validate_slot(const SolMirRuntimeConventions *owner, size_t id,
    SolMirRuntimeSlotRole role, size_t formal, SolMirRecipeId recipe,
    SolAccessMode access) {
    if (id >= owner->signature_slot_count) return false;
    const SolMirRuntimeSignatureSlot *slot = &owner->signature_slots[id];
    return slot->role == role && slot->formal == formal
        && slot->recipe == recipe && slot->access == access;
}

static bool validate_signature_record(const SolMirRuntimeConventions *owner,
    size_t id, SolMirRuntimeSignatureOrigin origin, size_t internal, size_t host,
    size_t function, SolMirRecipeId receiver, SolAccessMode receiver_access,
    SolMirPlanSlice parameters, SolMirPlanSlice accesses,
    const SolMirRecipeId *recipes, const SolAccessMode *access_values,
    SolMirRecipeId result, SolMirMaterializedEffectRowId effects,
    size_t *slot_at) {
    if (id >= owner->signature_count) return false;
    const SolMirRuntimeSignature *s = &owner->signatures[id];
    size_t count = parameters.count + (receiver != SOL_MIR_RECIPE_NONE);
    if (s->origin != origin || s->internal != internal || s->host != host
        || s->function_recipe != function || s->slots.offset != *slot_at
        || s->slots.count != count || s->result != result
        || s->result_class != result_class(&owner->concrete->representation, result)
        || s->effects != effects) return false;
    if (receiver != SOL_MIR_RECIPE_NONE) {
        if (!tick(1)) return false;
        if (!validate_slot(owner, (*slot_at)++, SOL_MIR_RUNTIME_SLOT_RECEIVER,
                SOL_MIR_RUNTIME_NONE, receiver, receiver_access)) return false;
    }
    for (size_t i = 0; i < parameters.count; ++i) {
        if (!tick(1)) return false;
        if (!validate_slot(owner, (*slot_at)++, SOL_MIR_RUNTIME_SLOT_PARAMETER,
                i, recipes[parameters.offset + i],
                access_values[accesses.offset + i])) return false;
    }
    return true;
}

static bool predicate_indirect(const SolMirOperations *operations,
    const SolMirPredicateTerminator *term) {
    if (term->call_kind == SOL_IR_CALL_CALLBACK) return true;
    if (term->callee >= operations->predicate_value_count) return false;
    const SolMirPredicateValue *value = &operations->predicate_values[term->callee];
    return value->kind == SOL_MIR_PREDICATE_VALUE_INSTRUCTION
        && value->definition < operations->predicate_instruction_count
        && operations->predicate_instructions[value->definition].kind
            == SOL_MIR_PREDICATE_INST_BOUND_OPERATION;
}

static bool validate_signatures(const SolMirRuntimeConventions *owner) {
    const SolMirConcreteProgram *c = owner->concrete;
    const SolMirMaterialization *m = &c->materialization;
    const SolMirRepresentation *r = &c->representation;
    const SolMirLinkage *l = &c->linkage;
    size_t signature = 0, slot = 0;
    for (size_t i = 0; i < l->callable_count; ++i) {
        if (!tick(1)) return false;
        const SolMirMaterializedImage *image = NULL;
        for (size_t q = 0; q < m->image_count; ++q) {
            if (!tick(1)) return false;
            if (m->images[q].instance == l->callables[i].instance) image = &m->images[q];
        }
        if (image == NULL || !validate_signature_record(owner, signature++,
                SOL_MIR_RUNTIME_SIGNATURE_INTERNAL, i, SOL_MIR_RUNTIME_NONE,
                SOL_MIR_RECIPE_NONE, image->receiver, image->receiver_access,
                image->parameter_types, image->parameter_accesses, m->type_ids,
                m->accesses, image->result, image->effects, &slot)) return false;
    }
    for (size_t i = 0; i < l->host_requirement_count; ++i) {
        if (!tick(1)) return false;
        const SolMirLinkageHostRequirement *h = &l->host_requirements[i];
        if (!validate_signature_record(owner, signature++,
                SOL_MIR_RUNTIME_SIGNATURE_HOST, SOL_MIR_RUNTIME_NONE, i,
                SOL_MIR_RECIPE_NONE, h->receiver, h->receiver_access,
                h->parameters, h->parameter_accesses, m->type_ids, m->accesses,
                h->result, h->effects, &slot)) return false;
    }
    SolMirRecipeId previous = SOL_MIR_RECIPE_NONE;
    while (signature < owner->signature_count) {
        if (!tick(1)) return false;
        const SolMirRuntimeSignature *actual = &owner->signatures[signature];
        size_t recipe = actual->function_recipe;
        if (actual->origin != SOL_MIR_RUNTIME_SIGNATURE_FUNCTION_RECIPE
            || recipe >= r->recipe_count
            || r->recipes[recipe].kind != SOL_MIR_RECIPE_FUNCTION
            || (previous != SOL_MIR_RECIPE_NONE && recipe <= previous)) return false;
        bool used = false;
        for (size_t i = 0; i < m->block_count; ++i) {
            if (!tick(1)) return false;
            const SolMirMaterializedTerminator *term = &m->blocks[i].terminator;
            used |= term->kind == SOL_MIR_TERM_INVOKE
                && term->call_kind == SOL_IR_CALL_CALLBACK
                && m->temporaries[term->callee].type == recipe;
        }
        for (size_t i = 0; i < c->operations.predicate_block_count; ++i) {
            if (!tick(1)) return false;
            const SolMirPredicateTerminator *term
                = &c->operations.predicate_blocks[i].terminator;
            used |= term->kind == SOL_MIR_PREDICATE_TERM_INVOKE
                && predicate_indirect(&c->operations, term)
                && c->operations.predicate_values[term->callee].recipe == recipe;
        }
        const SolMirRecipe *f = &r->recipes[recipe];
        if (!used || !validate_signature_record(owner, signature++,
                SOL_MIR_RUNTIME_SIGNATURE_FUNCTION_RECIPE,
                SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE, recipe,
                SOL_MIR_RECIPE_NONE, SOL_ACCESS_OWNED, f->parameters,
                f->parameter_accesses, r->recipe_ids, r->accesses, f->result,
                f->effects, &slot)) return false;
        previous = recipe;
    }
    return signature == owner->signature_count
        && slot == owner->signature_slot_count;
}

static size_t direct_signature(const SolMirRuntimeConventions *owner,
    size_t binding) {
    const SolMirLinkage *l = &owner->concrete->linkage;
    if (binding >= l->binding_count) return SOL_MIR_RUNTIME_NONE;
    const SolMirLinkageBinding *b = &l->bindings[binding];
    return b->target_kind == SOL_MIR_LINKAGE_TARGET_INTERNAL
        ? b->internal : l->callable_count + b->host;
}

static size_t function_signature(const SolMirRuntimeConventions *owner,
    SolMirRecipeId recipe) {
    for (size_t i = 0; i < owner->signature_count; ++i) {
        if (!tick(1)) return SOL_MIR_RUNTIME_NONE;
        if (owner->signatures[i].origin
                == SOL_MIR_RUNTIME_SIGNATURE_FUNCTION_RECIPE
            && owner->signatures[i].function_recipe == recipe) return i;
    }
    return SOL_MIR_RUNTIME_NONE;
}

static size_t table_for_site(const SolMirConcreteProgram *c, size_t site) {
    for (size_t i = 0; i < c->operations.callable_count; ++i) {
        if (!tick(1)) return SOL_MIR_LINKAGE_NONE;
        if (c->operations.callables[i].semantic_site == site)
            return c->linkage.callable_values[i].table;
    }
    return SOL_MIR_LINKAGE_NONE;
}

static size_t table_for_predicate(const SolMirConcreteProgram *c, size_t value) {
    const SolMirOperations *o = &c->operations;
    if (value >= o->predicate_value_count) return SOL_MIR_LINKAGE_NONE;
    const SolMirPredicateValue *v = &o->predicate_values[value];
    if (v->kind != SOL_MIR_PREDICATE_VALUE_INSTRUCTION
        || v->definition >= o->predicate_instruction_count) return SOL_MIR_LINKAGE_NONE;
    const SolMirPredicateInstruction *instruction
        = &o->predicate_instructions[v->definition];
    for (size_t i = 0; i < o->callable_count; ++i) {
        if (!tick(1)) return SOL_MIR_LINKAGE_NONE;
        const SolMirOperationCallablePlan *p = &o->callables[i];
        if (p->function_recipe == v->recipe
            && c->materialization.semantic_sites[p->semantic_site].binding
                == instruction->binding) return c->linkage.callable_values[i].table;
    }
    return SOL_MIR_LINKAGE_NONE;
}

static bool indirect_target_valid(const SolMirRuntimeCall *call, size_t table,
    size_t table_count) {
    return call->target_kind == SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE
        && call->internal == SOL_MIR_LINKAGE_NONE
        && call->host == SOL_MIR_LINKAGE_NONE && call->table == table
        && table < table_count;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
bool sol_mir_runtime_conventions_test_indirect_target_valid(
    const SolMirRuntimeCall *call, size_t table, size_t table_count) {
    return call != NULL && indirect_target_valid(call, table, table_count);
}
#endif

static bool target_valid(const SolMirRuntimeConventions *owner,
    const SolMirRuntimeCall *call, size_t binding, bool indirect, size_t table) {
    if (indirect) return indirect_target_valid(call, table,
        owner->concrete->linkage.table_entry_count);
    const SolMirLinkageBinding *b = &owner->concrete->linkage.bindings[binding];
    if (b->target_kind == SOL_MIR_LINKAGE_TARGET_INTERNAL)
        return call->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL
            && call->internal == b->internal && call->host == SOL_MIR_LINKAGE_NONE
            && call->table == SOL_MIR_LINKAGE_NONE;
    return call->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_HOST
        && call->internal == SOL_MIR_LINKAGE_NONE && call->host == b->host
        && call->table == SOL_MIR_LINKAGE_NONE;
}

static bool value_equal(SolMirRuntimeValueRef value,
    SolMirRuntimeValueKind kind, size_t id) {
    return value.kind == kind && value.id == id;
}

static bool supported_call_kind(SolIrCallKind kind) {
    switch (kind) {
        case SOL_IR_CALL_FUNCTION:
        case SOL_IR_CALL_CALLBACK:
        case SOL_IR_CALL_CAPABILITY:
        case SOL_IR_CALL_METHOD:
            return true;
        case SOL_IR_CALL_BUILTIN_OK:
        case SOL_IR_CALL_BUILTIN_ERR:
        case SOL_IR_CALL_BUILTIN_SOME:
        case SOL_IR_CALL_BUILTIN_NONE:
        case SOL_IR_CALL_ENUM_CONSTRUCTOR:
        case SOL_IR_CALL_DISTINCT_CONSTRUCTOR:
            return false;
    }
    return false;
}

static bool operand_valid(const SolMirRuntimeConventions *owner, size_t id,
    size_t slot, SolMirRuntimeValueKind kind, size_t value) {
    return id < owner->operand_count
        && owner->operands[id].signature_slot == slot
        && value_equal(owner->operands[id].value, kind, value);
}

static bool validate_image_call(const SolMirRuntimeConventions *owner,
    size_t call_id, size_t image_id, size_t block_id, size_t *operand_at,
    size_t *writeback_at) {
    const SolMirConcreteProgram *c = owner->concrete;
    const SolMirMaterialization *m = &c->materialization;
    const SolMirMaterializedTerminator *term = &m->blocks[block_id].terminator;
    if (call_id >= owner->call_count || term->kind != SOL_MIR_TERM_INVOKE)
        return false;
    const SolMirRuntimeCall *call = &owner->calls[call_id];
    bool indirect = term->call_kind == SOL_IR_CALL_CALLBACK;
    size_t signature = indirect
        ? function_signature(owner, m->temporaries[term->callee].type)
        : direct_signature(owner, term->binding);
    if (term->binding >= c->materialization.binding_count
        || !supported_call_kind(term->call_kind)
        || signature >= owner->signature_count
        || call_id >= owner->failure_site_count) return false;
    size_t table = indirect ? table_for_site(c, term->callable_site)
        : SOL_MIR_LINKAGE_NONE;
    if (call->owner_kind != SOL_MIR_RUNTIME_CALL_OWNER_IMAGE
        || call->image != image_id || call->predicate != SOL_MIR_RUNTIME_NONE
        || call->block != block_id || call->call_kind != term->call_kind
        || call->signature != signature || call->normal_edge != term->normal_edge
        || call->failure_edge != term->failure_edge
        || call->failure_site != call_id
        || !target_valid(owner, call, term->binding, indirect, table)
        || call->operands.offset != *operand_at
        || call->writebacks.offset != *writeback_at) return false;
    if (indirect) {
        if (!value_equal(call->callee,
                SOL_MIR_RUNTIME_VALUE_MATERIALIZED_TEMPORARY, term->callee))
            return false;
    } else if (!value_equal(call->callee, SOL_MIR_RUNTIME_VALUE_NONE,
            SOL_MIR_RUNTIME_NONE)) return false;
    const SolMirRuntimeSignature *sig = &owner->signatures[signature];
    if (term->failure_edge >= m->edge_count
        || m->edges[term->failure_edge].arguments.count != 0) return false;
    switch (sig->result_class) {
        case SOL_MIR_RUNTIME_RESULT_VALUE:
        case SOL_MIR_RUNTIME_RESULT_UNIT:
            if (term->normal_edge >= m->edge_count
                || term->result >= m->value_count
                || m->edges[term->normal_edge].arguments.count != 1)
                return false;
            break;
        case SOL_MIR_RUNTIME_RESULT_NEVER:
            if (term->normal_edge != SOL_MIR_MATERIALIZED_NONE
                || term->result != SOL_MIR_MATERIALIZED_NONE
                || term->writebacks.count != 0) return false;
            break;
        default:
            return false;
    }
    size_t ordinal = 0;
    size_t exclusive = 0;
    if (term->receiver.source_expression != SOL_IR_NONE) {
        SolMirRuntimeValueKind kind = term->receiver.access == SOL_ACCESS_OWNED
            ? SOL_MIR_RUNTIME_VALUE_MATERIALIZED_TEMPORARY
            : SOL_MIR_RUNTIME_VALUE_MATERIALIZED_PLACE;
        size_t value = term->receiver.access == SOL_ACCESS_OWNED
            ? term->receiver.temporary : term->receiver.place;
        if (!operand_valid(owner, (*operand_at)++, sig->slots.offset, kind, value))
            return false;
        exclusive += term->receiver.access == SOL_ACCESS_EXCLUSIVE;
        ++ordinal;
    }
    for (size_t i = 0; i < term->arguments.count; ++i) {
        if (!tick(1)) return false;
        const SolMirMaterializedCallArgument *a
            = &m->call_arguments[term->arguments.offset + i];
        SolMirRuntimeValueKind kind = a->access == SOL_ACCESS_OWNED
            ? SOL_MIR_RUNTIME_VALUE_MATERIALIZED_TEMPORARY
            : SOL_MIR_RUNTIME_VALUE_MATERIALIZED_PLACE;
        size_t value = a->access == SOL_ACCESS_OWNED ? a->temporary : a->place;
        if (!operand_valid(owner, (*operand_at)++, sig->slots.offset + ordinal++,
                kind, value)) return false;
        exclusive += a->access == SOL_ACCESS_EXCLUSIVE;
    }
    if (call->operands.count != ordinal) return false;
    if (sig->result_class == SOL_MIR_RUNTIME_RESULT_VALUE) {
        if (!value_equal(call->result, SOL_MIR_RUNTIME_VALUE_MATERIALIZED_VALUE,
                term->result)) return false;
    } else if (!value_equal(call->result, SOL_MIR_RUNTIME_VALUE_NONE,
            SOL_MIR_RUNTIME_NONE)) return false;
    if (term->writebacks.count != (sig->result_class
            == SOL_MIR_RUNTIME_RESULT_NEVER ? 0 : exclusive)) return false;
    for (size_t i = 0; i < term->writebacks.count; ++i) {
        if (!tick(1)) return false;
        if (*writeback_at >= owner->writeback_count) return false;
        const SolMirMaterializedWriteback *w
            = &m->writebacks[term->writebacks.offset + i];
        const SolMirRuntimeWriteback *actual = &owner->writebacks[*writeback_at];
        size_t relative = w->receiver ? 0
            : (sig->slots.count != term->arguments.count) + w->formal;
        if (actual->operand != call->operands.offset + relative
            || actual->receiver != w->receiver || actual->formal != w->formal
            || actual->place != w->place || actual->recipe != w->type)
            return false;
        if (actual->operand >= owner->operand_count
            || owner->operands[actual->operand].signature_slot
                >= owner->signature_slot_count
            || owner->signature_slots[owner->operands[actual->operand]
                .signature_slot].access != SOL_ACCESS_EXCLUSIVE) return false;
        for (size_t q = call->writebacks.offset; q < *writeback_at; ++q) {
            if (!tick(1)) return false;
            if (owner->writebacks[q].operand == actual->operand) return false;
        }
        ++*writeback_at;
    }
    return call->writebacks.count == term->writebacks.count;
}

static bool validate_predicate_call(const SolMirRuntimeConventions *owner,
    size_t call_id, size_t body_id, size_t block_id, size_t *operand_at,
    size_t writeback_at) {
    const SolMirConcreteProgram *c = owner->concrete;
    const SolMirOperations *o = &c->operations;
    const SolMirPredicateTerminator *term
        = &o->predicate_blocks[block_id].terminator;
    if (call_id >= owner->call_count
        || term->kind != SOL_MIR_PREDICATE_TERM_INVOKE) return false;
    const SolMirRuntimeCall *call = &owner->calls[call_id];
    bool indirect = predicate_indirect(o, term);
    size_t signature = indirect
        ? function_signature(owner, o->predicate_values[term->callee].recipe)
        : direct_signature(owner, term->binding);
    size_t table = indirect ? table_for_predicate(c, term->callee)
        : SOL_MIR_LINKAGE_NONE;
    if (!supported_call_kind(term->call_kind)
        || signature >= owner->signature_count
        || call->owner_kind != SOL_MIR_RUNTIME_CALL_OWNER_PREDICATE
        || call->image != SOL_MIR_RUNTIME_NONE || call->predicate != body_id
        || call->block != block_id || call->call_kind != term->call_kind
        || call->signature != signature || call->normal_edge != term->normal_edge
        || call->failure_edge != term->failure_edge
        || call->failure_site != call_id || call_id >= owner->failure_site_count
        || !target_valid(owner, call, term->binding, indirect, table)
        || call->operands.offset != *operand_at
        || call->writebacks.offset != writeback_at || call->writebacks.count != 0)
        return false;
    if (indirect) {
        if (!value_equal(call->callee, SOL_MIR_RUNTIME_VALUE_PREDICATE_VALUE,
                term->callee)) return false;
    } else if (!value_equal(call->callee, SOL_MIR_RUNTIME_VALUE_NONE,
            SOL_MIR_RUNTIME_NONE)) return false;
    const SolMirRuntimeSignature *sig = &owner->signatures[signature];
    if (term->failure_edge >= o->predicate_edge_count
        || o->predicate_edges[term->failure_edge].arguments.count != 0)
        return false;
    switch (sig->result_class) {
        case SOL_MIR_RUNTIME_RESULT_VALUE:
        case SOL_MIR_RUNTIME_RESULT_UNIT:
            if (term->normal_edge >= o->predicate_edge_count
                || term->result >= o->predicate_value_count) return false;
            break;
        case SOL_MIR_RUNTIME_RESULT_NEVER:
            if (term->normal_edge != SOL_MIR_OPERATION_NONE
                || term->result != SOL_MIR_OPERATION_NONE) return false;
            break;
        default:
            return false;
    }
    size_t ordinal = 0;
    if (!indirect && (term->receiver != SOL_MIR_OPERATION_NONE
        || term->call_kind == SOL_IR_CALL_CAPABILITY)) {
        size_t receiver = term->call_kind == SOL_IR_CALL_CAPABILITY
            ? term->callee : term->receiver;
        SolMirRuntimeValueKind kind = term->call_kind == SOL_IR_CALL_CAPABILITY
            ? SOL_MIR_RUNTIME_VALUE_BOUND_RECEIVER
            : SOL_MIR_RUNTIME_VALUE_PREDICATE_VALUE;
        if (!operand_valid(owner, (*operand_at)++, sig->slots.offset, kind,
                receiver)) return false;
        ++ordinal;
    }
    for (size_t i = 0; i < term->arguments.count; ++i) {
        if (!tick(1)) return false;
        const SolMirPredicateOperand *a
            = &o->predicate_operands[term->arguments.offset + i];
        if (!operand_valid(owner, (*operand_at)++, sig->slots.offset + ordinal++,
                SOL_MIR_RUNTIME_VALUE_PREDICATE_VALUE, a->value)) return false;
    }
    if (call->operands.count != ordinal) return false;
    return sig->result_class == SOL_MIR_RUNTIME_RESULT_VALUE
        ? value_equal(call->result, SOL_MIR_RUNTIME_VALUE_PREDICATE_VALUE,
            term->result)
        : value_equal(call->result, SOL_MIR_RUNTIME_VALUE_NONE,
            SOL_MIR_RUNTIME_NONE);
}

static bool validate_calls(const SolMirRuntimeConventions *owner) {
    const SolMirConcreteProgram *c = owner->concrete;
    size_t call = 0, operand = 0, writeback = 0;
    for (size_t image = 0; image < c->materialization.image_count; ++image) {
        if (!tick(1)) return false;
        SolMirPlanSlice blocks = c->materialization.images[image].blocks;
        for (size_t q = 0; q < blocks.count; ++q) {
            if (!tick(1)) return false;
            size_t block = blocks.offset + q;
            if (c->materialization.blocks[block].terminator.kind
                    != SOL_MIR_TERM_INVOKE) continue;
            if (!validate_image_call(owner, call++, image, block, &operand,
                    &writeback)) return false;
        }
    }
    for (size_t body = 0; body < c->operations.predicate_body_count; ++body) {
        if (!tick(1)) return false;
        SolMirPlanSlice blocks = c->operations.predicate_bodies[body].blocks;
        for (size_t q = 0; q < blocks.count; ++q) {
            if (!tick(1)) return false;
            size_t block = blocks.offset + q;
            if (c->operations.predicate_blocks[block].terminator.kind
                    != SOL_MIR_PREDICATE_TERM_INVOKE) continue;
            if (!validate_predicate_call(owner, call++, body, block, &operand,
                    writeback)) return false;
        }
    }
    return call == owner->call_count && operand == owner->operand_count
        && writeback == owner->writeback_count;
}

static uint32_t failure_code_bit(SolMirRuntimeFailureCode code) {
    return UINT32_C(1) << ((unsigned)code - 1);
}

static bool validate_call_failure_mask(const SolMirRuntimeCall *call,
    const SolMirLinkage *linkage, uint32_t *mask) {
    bool targets_host;
    if (call->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL) {
        targets_host = false;
    } else if (call->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_HOST) {
        targets_host = true;
    } else if (call->target_kind == SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE) {
        if (call->table >= linkage->table_entry_count) return false;
        SolMirLinkageTargetKind target
            = linkage->table_entries[call->table].target_kind;
        if (target != SOL_MIR_LINKAGE_TARGET_INTERNAL
            && target != SOL_MIR_LINKAGE_TARGET_HOST) return false;
        targets_host = target == SOL_MIR_LINKAGE_TARGET_HOST;
    } else return false;
    *mask = failure_code_bit(SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT);
    if (targets_host) {
        *mask |= failure_code_bit(SOL_MIR_RUNTIME_FAILURE_HOST_CALL_LIMIT);
        *mask |= failure_code_bit(SOL_MIR_RUNTIME_FAILURE_HOST_ERROR);
    }
    return true;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
bool sol_mir_runtime_conventions_test_validate_call_failure_mask(
    const SolMirRuntimeCall *call, const SolMirLinkage *linkage,
    uint32_t *mask) {
    return call != NULL && linkage != NULL && mask != NULL
        && validate_call_failure_mask(call, linkage, mask);
}
#endif

static uint32_t arithmetic_failure_mask(unsigned failures) {
    uint32_t mask = 0;
    if ((failures & SOL_MIR_OPERATION_FAILURE_OVERFLOW) != 0)
        mask |= failure_code_bit(SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW);
    if ((failures & SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO) != 0)
        mask |= failure_code_bit(SOL_MIR_RUNTIME_FAILURE_DIVISION_BY_ZERO);
    return mask;
}

static bool runtime_source_from_span(const SolIr *ir, SolSpan span,
    SolMirRuntimeSource *source) {
    if (span.start > span.end) return false;
    bool found = false;
    for (size_t i = 0; i < ir->file_count; ++i) {
        if (!tick(1)) return false;
        const SolIrSourceFile *file = &ir->files[i];
        if (span.start >= file->aggregate_start
            && span.end <= file->aggregate_end) {
            if (found) return false;
            *source = (SolMirRuntimeSource){i,
                span.start - file->aggregate_start,
                span.end - file->aggregate_start};
            found = true;
        }
    }
    return found;
}

static bool validate_expected_site(const SolMirRuntimeConventions *owner,
    size_t *site_at, SolMirRuntimeFailureOriginKind origin, size_t owner_id,
    size_t block, size_t instruction, SolSpan span, uint32_t allowed_codes) {
    if (!tick(1) || *site_at >= owner->failure_site_count
        || allowed_codes == 0) return false;
    SolMirRuntimeSource source;
    if (!runtime_source_from_span(owner->concrete->program.ir, span, &source))
        return false;
    const SolMirRuntimeFailureSite *site = &owner->failure_sites[*site_at];
    if (site->origin_kind != origin || site->owner != owner_id
        || site->block != block || site->instruction != instruction
        || site->source.file != source.file || site->source.start != source.start
        || site->source.end != source.end
        || site->allowed_codes != allowed_codes) return false;
    ++*site_at;
    return true;
}

static SolMirOperationOpcode site_predicate_opcode(SolTokenKind token,
    bool unary, unsigned *failures) {
    *failures = SOL_MIR_OPERATION_FAILURE_NONE;
    switch (token) {
        case SOL_TOKEN_BANG: return SOL_MIR_OPERATION_BOOL_NOT;
        case SOL_TOKEN_MINUS:
            *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW;
            return unary ? SOL_MIR_OPERATION_I64_NEG : SOL_MIR_OPERATION_I64_SUB;
        case SOL_TOKEN_PLUS:
            *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW;
            return SOL_MIR_OPERATION_I64_ADD;
        case SOL_TOKEN_STAR:
            *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW;
            return SOL_MIR_OPERATION_I64_MUL;
        case SOL_TOKEN_SLASH:
            *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW
                | SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO;
            return SOL_MIR_OPERATION_I64_DIV;
        case SOL_TOKEN_PERCENT:
            *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW
                | SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO;
            return SOL_MIR_OPERATION_I64_REM;
        case SOL_TOKEN_LESS: return SOL_MIR_OPERATION_I64_LT;
        case SOL_TOKEN_LESS_EQUAL: return SOL_MIR_OPERATION_I64_LE;
        case SOL_TOKEN_GREATER: return SOL_MIR_OPERATION_I64_GT;
        case SOL_TOKEN_GREATER_EQUAL: return SOL_MIR_OPERATION_I64_GE;
        case SOL_TOKEN_EQUAL_EQUAL: return SOL_MIR_OPERATION_VALUE_EQ;
        case SOL_TOKEN_BANG_EQUAL: return SOL_MIR_OPERATION_VALUE_NE;
        default: return (SolMirOperationOpcode)-1;
    }
}

typedef struct {
    const SolMirRuntimeConventions *owner;
    const SolMirOperations *operations;
    const SolIr *ir;
    const SolMirPredicateBody *body;
    size_t body_id;
    size_t allocated_blocks;
    size_t block;
    size_t instruction_at;
    size_t *site_at;
} PredicateSiteValidator;

static size_t site_new_block(PredicateSiteValidator *replay) {
    if (replay->allocated_blocks >= replay->body->blocks.count)
        return SOL_MIR_OPERATION_NONE;
    size_t block = replay->body->blocks.offset + replay->allocated_blocks++;
    return replay->operations->predicate_blocks[block].body == replay->body_id
        ? block : SOL_MIR_OPERATION_NONE;
}

static bool site_start_block(PredicateSiteValidator *replay, size_t block) {
    if (block < replay->body->blocks.offset
        || block >= replay->body->blocks.offset + replay->body->blocks.count)
        return false;
    replay->block = block;
    replay->instruction_at = 0;
    return true;
}

static const SolMirPredicateInstruction *site_instruction(
    PredicateSiteValidator *replay, SolMirPredicateInstructionKind kind) {
    const SolMirPredicateBlock *block
        = &replay->operations->predicate_blocks[replay->block];
    if (replay->instruction_at >= block->instructions.count) return NULL;
    size_t id = block->instructions.offset + replay->instruction_at++;
    const SolMirPredicateInstruction *instruction
        = &replay->operations->predicate_instructions[id];
    return instruction->block == replay->block && instruction->kind == kind
        ? instruction : NULL;
}

static bool site_edge_target(const PredicateSiteValidator *replay, size_t edge,
    size_t target) {
    return edge < replay->operations->predicate_edge_count
        && replay->operations->predicate_edges[edge].source == replay->block
        && replay->operations->predicate_edges[edge].target == target;
}

static bool site_end_block(PredicateSiteValidator *replay,
    SolMirPredicateTerminatorKind kind) {
    const SolMirPredicateBlock *block
        = &replay->operations->predicate_blocks[replay->block];
    return replay->instruction_at == block->instructions.count
        && block->terminator.kind == kind;
}

static bool site_failure_block(PredicateSiteValidator *replay, size_t block,
    SolMirPredicateFailureKind kind) {
    const SolMirPredicateBlock *failure
        = &replay->operations->predicate_blocks[block];
    return failure->body == replay->body_id && failure->instructions.count == 0
        && failure->terminator.kind == SOL_MIR_PREDICATE_TERM_FAILURE
        && failure->terminator.failure_kind == kind;
}

static bool validate_predicate_site_expression(PredicateSiteValidator *replay,
    SolIrExpressionId expression, size_t depth);

static bool validate_predicate_site_match(PredicateSiteValidator *replay,
    SolIrExpressionId expression, const SolIrExpression *source, size_t depth) {
    if (!validate_predicate_site_expression(replay,
            source->as.match_expr.scrutinee, depth + 1)) return false;
    size_t join = site_new_block(replay);
    if (join == SOL_MIR_OPERATION_NONE) return false;
    size_t next = replay->block;
    for (size_t i = 0; i < source->as.match_expr.arms.count; ++i) {
        if (i != 0 && !site_start_block(replay, next)) return false;
        size_t arm_id = replay->ir->arm_ids[source->as.match_expr.arms.offset + i];
        const SolIrArm *arm = &replay->ir->arms[arm_id];
        if (site_instruction(replay, SOL_MIR_PREDICATE_INST_PATTERN_TEST)
                == NULL) return false;
        size_t arm_block = site_new_block(replay);
        size_t miss_block = site_new_block(replay);
        if (arm_block == SOL_MIR_OPERATION_NONE
            || miss_block == SOL_MIR_OPERATION_NONE
            || !site_end_block(replay, SOL_MIR_PREDICATE_TERM_BRANCH))
            return false;
        const SolMirPredicateTerminator *branch
            = &replay->operations->predicate_blocks[replay->block].terminator;
        if (!site_edge_target(replay, branch->true_edge, arm_block)
            || !site_edge_target(replay, branch->false_edge, miss_block)
            || !site_start_block(replay, arm_block)) return false;
        for (size_t q = 0; q < arm->bindings.count; ++q)
            if (site_instruction(replay,
                    SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT) == NULL) return false;
        if (arm->guard != SOL_IR_NONE) {
            if (!validate_predicate_site_expression(replay, arm->guard,
                    depth + 1)) return false;
            size_t body_block = site_new_block(replay);
            if (body_block == SOL_MIR_OPERATION_NONE
                || !site_end_block(replay, SOL_MIR_PREDICATE_TERM_BRANCH))
                return false;
            branch = &replay->operations->predicate_blocks[
                replay->block].terminator;
            if (!site_edge_target(replay, branch->true_edge, body_block)
                || !site_edge_target(replay, branch->false_edge, miss_block)
                || !site_start_block(replay, body_block)) return false;
        }
        if (!validate_predicate_site_expression(replay, arm->body, depth + 1)
            || !site_end_block(replay, SOL_MIR_PREDICATE_TERM_JUMP))
            return false;
        const SolMirPredicateTerminator *jump
            = &replay->operations->predicate_blocks[replay->block].terminator;
        if (!site_edge_target(replay, jump->edge, join)) return false;
        next = miss_block;
    }
    if (!site_start_block(replay, next)
        || !site_end_block(replay, SOL_MIR_PREDICATE_TERM_FAILURE)) return false;
    const SolMirPredicateTerminator *failure
        = &replay->operations->predicate_blocks[next].terminator;
    if (failure->failure_kind != SOL_MIR_PREDICATE_FAILURE_NO_MATCH
        || !validate_expected_site(replay->owner, replay->site_at,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_NO_MATCH,
            replay->body_id, next, SOL_MIR_RUNTIME_NONE,
            replay->ir->expressions[expression].span,
            failure_code_bit(SOL_MIR_RUNTIME_FAILURE_NO_MATCH))) return false;
    return site_start_block(replay, join);
}

static bool validate_predicate_site_expression(PredicateSiteValidator *replay,
    SolIrExpressionId expression, size_t depth) {
    if (expression >= replay->ir->expression_count
        || depth > replay->ir->expression_count) return false;
    const SolIrExpression *source = &replay->ir->expressions[expression];
    if (source->kind == SOL_IR_EXPR_PLACE) {
        const SolIrPlace *place = &replay->ir->places[source->as.place];
        if (place->root_kind == SOL_IR_PLACE_ROOT_TEMPORARY
            && !validate_predicate_site_expression(replay, place->temporary,
                depth + 1)) return false;
        return place->projections.count == 0
            || site_instruction(replay, SOL_MIR_PREDICATE_INST_PROJECT) != NULL;
    }
    if (source->kind == SOL_IR_EXPR_RESULT
        || source->kind == SOL_IR_EXPR_SNAPSHOT_READ
        || source->kind == SOL_IR_EXPR_REFINEMENT_SELF) return true;
    if (source->kind == SOL_IR_EXPR_BINARY
        && (source->as.binary.operator_kind == SOL_TOKEN_AMP_AMP
            || source->as.binary.operator_kind == SOL_TOKEN_PIPE_PIPE)) {
        if (!validate_predicate_site_expression(replay, source->as.binary.left,
                depth + 1)) return false;
        size_t branch_block = replay->block;
        size_t rhs = site_new_block(replay), shortcut = site_new_block(replay);
        size_t join = site_new_block(replay);
        if (rhs == SOL_MIR_OPERATION_NONE || shortcut == SOL_MIR_OPERATION_NONE
            || join == SOL_MIR_OPERATION_NONE
            || !site_end_block(replay, SOL_MIR_PREDICATE_TERM_BRANCH))
            return false;
        const SolMirPredicateTerminator *branch
            = &replay->operations->predicate_blocks[branch_block].terminator;
        bool is_and = source->as.binary.operator_kind == SOL_TOKEN_AMP_AMP;
        if (!site_edge_target(replay, branch->true_edge,
                is_and ? rhs : shortcut)
            || !site_edge_target(replay, branch->false_edge,
                is_and ? shortcut : rhs)
            || !site_start_block(replay, rhs)
            || !validate_predicate_site_expression(replay,
                source->as.binary.right, depth + 1)
            || !site_end_block(replay, SOL_MIR_PREDICATE_TERM_JUMP))
            return false;
        const SolMirPredicateTerminator *jump
            = &replay->operations->predicate_blocks[replay->block].terminator;
        if (!site_edge_target(replay, jump->edge, join)
            || !site_start_block(replay, shortcut)
            || site_instruction(replay, SOL_MIR_PREDICATE_INST_BOOL) == NULL
            || !site_end_block(replay, SOL_MIR_PREDICATE_TERM_JUMP)) return false;
        jump = &replay->operations->predicate_blocks[replay->block].terminator;
        return site_edge_target(replay, jump->edge, join)
            && site_start_block(replay, join);
    }
    if (source->kind == SOL_IR_EXPR_IF) {
        if (!validate_predicate_site_expression(replay,
                source->as.if_expr.condition, depth + 1)) return false;
        size_t branch_block = replay->block;
        size_t then_block = site_new_block(replay);
        size_t else_block = site_new_block(replay);
        size_t join = site_new_block(replay);
        if (then_block == SOL_MIR_OPERATION_NONE
            || else_block == SOL_MIR_OPERATION_NONE
            || join == SOL_MIR_OPERATION_NONE
            || !site_end_block(replay, SOL_MIR_PREDICATE_TERM_BRANCH))
            return false;
        const SolMirPredicateTerminator *branch
            = &replay->operations->predicate_blocks[branch_block].terminator;
        if (!site_edge_target(replay, branch->true_edge, then_block)
            || !site_edge_target(replay, branch->false_edge, else_block)
            || !site_start_block(replay, then_block)
            || !validate_predicate_site_expression(replay,
                source->as.if_expr.then_branch, depth + 1)
            || !site_end_block(replay, SOL_MIR_PREDICATE_TERM_JUMP))
            return false;
        const SolMirPredicateTerminator *jump
            = &replay->operations->predicate_blocks[replay->block].terminator;
        if (!site_edge_target(replay, jump->edge, join)
            || !site_start_block(replay, else_block)
            || !validate_predicate_site_expression(replay,
                source->as.if_expr.else_branch, depth + 1)
            || !site_end_block(replay, SOL_MIR_PREDICATE_TERM_JUMP))
            return false;
        jump = &replay->operations->predicate_blocks[replay->block].terminator;
        return site_edge_target(replay, jump->edge, join)
            && site_start_block(replay, join);
    }
    if (source->kind == SOL_IR_EXPR_MATCH)
        return validate_predicate_site_match(replay, expression, source, depth);
    if (source->kind == SOL_IR_EXPR_BLOCK) {
        for (size_t i = 0; i < source->as.block.statements.count; ++i) {
            const SolIrStatement *statement = &replay->ir->statements[
                replay->ir->statement_ids[source->as.block.statements.offset + i]];
            if (!validate_predicate_site_expression(replay,
                    statement->expression, depth + 1)) return false;
            if (statement->kind == SOL_IR_STATEMENT_RETURN) break;
        }
        if (source->as.block.statements.count == 0)
            return site_instruction(replay,
                SOL_MIR_PREDICATE_INST_UNIT) != NULL;
        return true;
    }
    if (source->kind == SOL_IR_EXPR_DEFINITION)
        return site_instruction(replay,
            SOL_MIR_PREDICATE_INST_FUNCTION) != NULL;
    if (source->kind == SOL_IR_EXPR_BOUND_OPERATION)
        return validate_predicate_site_expression(replay,
                source->as.operation.receiver, depth + 1)
            && site_instruction(replay,
                SOL_MIR_PREDICATE_INST_BOUND_OPERATION) != NULL;
    if (source->kind == SOL_IR_EXPR_CALL
        && source->as.call.kind <= SOL_IR_CALL_METHOD) {
        if ((source->as.call.kind == SOL_IR_CALL_CALLBACK
                || source->as.call.kind == SOL_IR_CALL_CAPABILITY)
            && !validate_predicate_site_expression(replay,
                source->as.call.callee, depth + 1)) return false;
        if (source->as.call.kind == SOL_IR_CALL_METHOD
            && !validate_predicate_site_expression(replay,
                source->as.call.receiver, depth + 1)) return false;
        for (size_t i = 0; i < source->as.call.operands.count; ++i)
            if (!validate_predicate_site_expression(replay,
                    replay->ir->operands[source->as.call.operands.offset + i].value,
                    depth + 1)) return false;
        size_t source_block = replay->block;
        size_t normal = site_new_block(replay), failure = site_new_block(replay);
        if (normal == SOL_MIR_OPERATION_NONE || failure == SOL_MIR_OPERATION_NONE
            || !site_failure_block(replay, failure,
                SOL_MIR_PREDICATE_FAILURE_CALL)
            || !site_end_block(replay, SOL_MIR_PREDICATE_TERM_INVOKE))
            return false;
        const SolMirPredicateTerminator *term
            = &replay->operations->predicate_blocks[source_block].terminator;
        if (term->binding >= replay->owner->concrete->materialization.binding_count
            || replay->owner->concrete->materialization.bindings[
                term->binding].source.expression != expression
            || !site_edge_target(replay, term->normal_edge, normal)
            || !site_edge_target(replay, term->failure_edge, failure))
            return false;
        return site_start_block(replay, normal);
    }
    if (source->kind == SOL_IR_EXPR_PROPAGATE) {
        if (!validate_predicate_site_expression(replay,
                source->as.propagate.operand, depth + 1)) return false;
        size_t normal = site_new_block(replay), failure = site_new_block(replay);
        if (normal == SOL_MIR_OPERATION_NONE || failure == SOL_MIR_OPERATION_NONE
            || !site_failure_block(replay, failure,
                SOL_MIR_PREDICATE_FAILURE_PROPAGATION)
            || !site_end_block(replay, SOL_MIR_PREDICATE_TERM_PROPAGATE))
            return false;
        const SolMirPredicateTerminator *term
            = &replay->operations->predicate_blocks[replay->block].terminator;
        return site_edge_target(replay, term->normal_edge, normal)
            && site_edge_target(replay, term->failure_edge, failure)
            && site_start_block(replay, normal);
    }
    SolMirPredicateInstructionKind kind;
    if (source->kind == SOL_IR_EXPR_INTEGER) kind = SOL_MIR_PREDICATE_INST_I64;
    else if (source->kind == SOL_IR_EXPR_BOOL) kind = SOL_MIR_PREDICATE_INST_BOOL;
    else if (source->kind == SOL_IR_EXPR_UNIT) kind = SOL_MIR_PREDICATE_INST_UNIT;
    else if (source->kind == SOL_IR_EXPR_STRING) kind = SOL_MIR_PREDICATE_INST_TEXT;
    else if (source->kind == SOL_IR_EXPR_UNARY) {
        if (!validate_predicate_site_expression(replay,
                source->as.unary.operand, depth + 1)) return false;
        kind = SOL_MIR_PREDICATE_INST_UNARY;
    } else if (source->kind == SOL_IR_EXPR_BINARY) {
        if (!validate_predicate_site_expression(replay, source->as.binary.left,
                depth + 1)
            || !validate_predicate_site_expression(replay,
                source->as.binary.right, depth + 1)) return false;
        kind = SOL_MIR_PREDICATE_INST_BINARY;
    } else if (source->kind == SOL_IR_EXPR_RECORD
        || source->kind == SOL_IR_EXPR_TUPLE
        || source->kind == SOL_IR_EXPR_VARIANT
        || (source->kind == SOL_IR_EXPR_CALL
            && source->as.call.kind >= SOL_IR_CALL_BUILTIN_OK)) {
        SolIrSlice operands = source->kind == SOL_IR_EXPR_RECORD
            ? source->as.record.fields : source->kind == SOL_IR_EXPR_TUPLE
            ? source->as.tuple.operands : source->kind == SOL_IR_EXPR_VARIANT
            ? (SolIrSlice){0, 0} : source->as.call.operands;
        if (source->kind == SOL_IR_EXPR_CALL
            && source->as.call.kind == SOL_IR_CALL_DISTINCT_CONSTRUCTOR
            && source->as.call.definition < replay->ir->definition_count
            && replay->ir->definitions[source->as.call.definition].kind
                == SOL_IR_DEFINITION_REFINED) {
            if (operands.count != 1
                || !validate_predicate_site_expression(replay,
                    replay->ir->operands[operands.offset].value, depth + 1))
                return false;
            size_t normal = site_new_block(replay);
            size_t failure = site_new_block(replay);
            if (normal == SOL_MIR_OPERATION_NONE
                || failure == SOL_MIR_OPERATION_NONE
                || !site_failure_block(replay, failure,
                    SOL_MIR_PREDICATE_FAILURE_REFINEMENT)
                || !site_end_block(replay,
                    SOL_MIR_PREDICATE_TERM_CHECK_REFINED)) return false;
            const SolMirPredicateTerminator *term
                = &replay->operations->predicate_blocks[replay->block].terminator;
            return site_edge_target(replay, term->normal_edge, normal)
                && site_edge_target(replay, term->failure_edge, failure)
                && site_start_block(replay, normal);
        }
        for (size_t i = 0; i < operands.count; ++i)
            if (!validate_predicate_site_expression(replay,
                    replay->ir->operands[operands.offset + i].value, depth + 1))
                return false;
        kind = SOL_MIR_PREDICATE_INST_CONSTRUCT;
    } else return false;
    const SolMirPredicateInstruction *instruction
        = site_instruction(replay, kind);
    if (instruction == NULL) return false;
    if (kind == SOL_MIR_PREDICATE_INST_UNARY
        || kind == SOL_MIR_PREDICATE_INST_BINARY) {
        unsigned failures;
        SolTokenKind token = kind == SOL_MIR_PREDICATE_INST_UNARY
            ? source->as.unary.operator_kind : source->as.binary.operator_kind;
        SolMirOperationOpcode opcode = site_predicate_opcode(token,
            kind == SOL_MIR_PREDICATE_INST_UNARY, &failures);
        size_t instruction_id = (size_t)(instruction
            - replay->operations->predicate_instructions);
        uint32_t mask = arithmetic_failure_mask(failures);
        if (instruction->opcode != opcode || instruction->failures != failures)
            return false;
        if (mask != 0 && !validate_expected_site(replay->owner,
                replay->site_at,
                SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC,
                replay->body_id, replay->block, instruction_id,
                source->span, mask)) return false;
    }
    return true;
}

static bool validate_failure_sites(const SolMirRuntimeConventions *owner) {
    const SolMirConcreteProgram *c = owner->concrete;
    const SolMirMaterialization *m = &c->materialization;
    const SolMirOperations *o = &c->operations;
    const SolIr *ir = c->program.ir;
    size_t site_at = 0;
    for (size_t i = 0; i < owner->call_count; ++i) {
        if (!tick(1)) return false;
        const SolMirRuntimeCall *call = &owner->calls[i];
        SolSpan span;
        SolMirRuntimeFailureOriginKind origin;
        if (call->owner_kind == SOL_MIR_RUNTIME_CALL_OWNER_IMAGE) {
            span = m->blocks[call->block].terminator.span;
            origin = SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_CALL;
        } else {
            const SolMirPredicateTerminator *term
                = &o->predicate_blocks[call->block].terminator;
            if (term->binding >= m->binding_count
                || m->bindings[term->binding].source.expression
                    >= ir->expression_count) return false;
            span = ir->expressions[
                m->bindings[term->binding].source.expression].span;
            origin = SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_CALL;
        }
        uint32_t mask;
        if (!validate_call_failure_mask(call, &c->linkage, &mask)) return false;
        if (call->failure_site != i
            || !validate_expected_site(owner, &site_at, origin,
                call->owner_kind == SOL_MIR_RUNTIME_CALL_OWNER_IMAGE
                    ? call->image : call->predicate,
                call->block, SOL_MIR_RUNTIME_NONE, span, mask)) return false;
    }
    for (size_t i = 0; i < o->arithmetic_count; ++i) {
        if (!tick(1)) return false;
        const SolMirOperationArithmeticPlan *plan = &o->arithmetic[i];
        uint32_t mask = arithmetic_failure_mask(plan->failures);
        if (mask == 0) continue;
        const SolMirMaterializedInstruction *instruction
            = &m->instructions[plan->instruction];
        if (!validate_expected_site(owner, &site_at,
                SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_ARITHMETIC,
                plan->image, instruction->block, plan->instruction,
                instruction->span, mask)) return false;
    }
    for (size_t image = 0; image < m->image_count; ++image) {
        if (!tick(1)) return false;
        SolMirPlanSlice blocks = m->images[image].blocks;
        for (size_t q = 0; q < blocks.count; ++q) {
            if (!tick(1)) return false;
            size_t block = blocks.offset + q;
            const SolMirMaterializedTerminator *term = &m->blocks[block].terminator;
            SolMirRuntimeFailureOriginKind origin;
            SolMirRuntimeFailureCode code;
            if (term->kind == SOL_MIR_TERM_PANIC) {
                origin = SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_PANIC;
                code = SOL_MIR_RUNTIME_FAILURE_PANIC;
            } else if (term->kind == SOL_MIR_TERM_MATCH_FAILURE) {
                origin = SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_NO_MATCH;
                code = SOL_MIR_RUNTIME_FAILURE_NO_MATCH;
            } else if (term->kind == SOL_MIR_TERM_UNREACHABLE) {
                origin = SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_UNREACHABLE;
                code = SOL_MIR_RUNTIME_FAILURE_REACHED_UNREACHABLE;
            } else continue;
            if (!validate_expected_site(owner, &site_at, origin, image, block,
                    SOL_MIR_RUNTIME_NONE, term->span,
                    failure_code_bit(code))) return false;
        }
    }
    for (size_t body_id = 0; body_id < o->predicate_body_count; ++body_id) {
        if (!tick(1) || !tick(ir->expression_count)) return false;
        const SolMirPredicateBody *body = &o->predicate_bodies[body_id];
        PredicateSiteValidator replay = {owner, o, ir, body, body_id, 0,
            SOL_MIR_OPERATION_NONE, 0, &site_at};
        size_t entry = site_new_block(&replay);
        const SolMirPlanContext *context = &m->contexts[body->context];
        if (entry != body->entry || context->obligation >= ir->obligation_count
            || !site_start_block(&replay, entry)) return false;
        SolIrExpressionId predicate
            = ir->obligations[context->obligation].predicate;
        if (!validate_predicate_site_expression(&replay, predicate, 0)
            || !site_end_block(&replay, SOL_MIR_PREDICATE_TERM_RETURN)
            || replay.allocated_blocks != body->blocks.count) return false;
        SolMirRuntimeFailureCode code = context->kind
                == SOL_MIR_PLAN_CONTEXT_REFINEMENT
            ? SOL_MIR_RUNTIME_FAILURE_REFINEMENT_VIOLATION
            : body->phase == SOL_CONTRACT_REQUIRES
                ? SOL_MIR_RUNTIME_FAILURE_REQUIRE_VIOLATION
                : SOL_MIR_RUNTIME_FAILURE_ENSURE_VIOLATION;
        if (!validate_expected_site(owner, &site_at,
                SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT,
                body_id, replay.block, SOL_MIR_RUNTIME_NONE,
                ir->expressions[predicate].span, failure_code_bit(code)))
            return false;
    }
    return site_at == owner->failure_site_count;
}

static bool validate_entries(const SolMirRuntimeConventions *owner) {
    const SolMirConcreteProgram *c = owner->concrete;
    if (owner->entry_count != c->linkage.entry_export_count
        || owner->entry_count > 1) return false;
    for (size_t i = 0; i < owner->entry_count; ++i) {
        if (!tick(1)) return false;
        const SolMirRuntimeEntry *e = &owner->entries[i];
        const SolMirLinkageEntryExport *x = &c->linkage.entry_exports[i];
        if (e->export_id != i || e->binding != x->root_binding
            || e->callable != x->callable || e->signature != x->callable
            || memcmp(&e->symbol, &x->symbol, sizeof(e->symbol)) != 0
            || e->binding >= c->materialization.binding_count
            || !program_source_equal(e->source,
                c->materialization.bindings[e->binding].source)
            || e->signature >= owner->signature_count
            || e->result_class != owner->signatures[e->signature].result_class)
            return false;
        const SolMirRuntimeSignature *s = &owner->signatures[e->signature];
        if (s->result_class == SOL_MIR_RUNTIME_RESULT_NEVER
            || (s->result_class == SOL_MIR_RUNTIME_RESULT_VALUE
                && c->representation.recipes[s->result].kind
                    != SOL_MIR_RECIPE_INT64)) return false;
        for (size_t q = 0; q < s->slots.count; ++q) {
            if (!tick(1)) return false;
            const SolMirRuntimeSignatureSlot *slot
                = &owner->signature_slots[s->slots.offset + q];
            if (slot->role != SOL_MIR_RUNTIME_SLOT_PARAMETER
                || slot->access != SOL_ACCESS_OWNED
                || c->representation.recipes[slot->recipe].kind
                    != SOL_MIR_RECIPE_CAPABILITY) return false;
        }
    }
    return true;
}

static void hash_host(const SolMirLinkageHostRequirement *host,
    SolMirLinkageDigest *digest) {
    static const char domain[] = "sol.mir.runtime-host-import/1";
    SolMirLinkageSha256 sha;
    sol_mir_linkage_internal_sha256_init(&sha);
    sol_mir_linkage_internal_sha256_write(&sha, domain, sizeof(domain));
    uint8_t semantic[16];
    for (size_t i = 0; i < 8; ++i) {
        semantic[i] = (uint8_t)(host->semantic_id.high >> (56 - i * 8));
        semantic[8 + i] = (uint8_t)(host->semantic_id.low >> (56 - i * 8));
    }
    sol_mir_linkage_internal_sha256_write(&sha, semantic, sizeof(semantic));
    sol_mir_linkage_internal_sha256_write(&sha, host->requirement_key.bytes, 32);
    (void)sol_mir_linkage_internal_sha256_finish(&sha, digest);
}

static void hash_recipe(uint8_t tag, const SolMirLinkageDigest *key,
    SolMirLinkageDigest *digest) {
    static const char domain[] = "sol.mir.runtime-recipe-import/1";
    SolMirLinkageSha256 sha;
    sol_mir_linkage_internal_sha256_init(&sha);
    sol_mir_linkage_internal_sha256_write(&sha, domain, sizeof(domain));
    sol_mir_linkage_internal_sha256_write(&sha, &tag, 1);
    sol_mir_linkage_internal_sha256_write(&sha, key->bytes, 32);
    (void)sol_mir_linkage_internal_sha256_finish(&sha, digest);
}

static void digest_hex(const SolMirLinkageDigest *digest, char *at) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        at[i * 2] = hex[digest->bytes[i] >> 4];
        at[i * 2 + 1] = hex[digest->bytes[i] & 15];
    }
}

static const char *operation_name(SolMirRuntimeImportKind kind) {
    static const char *const names[] = {"host", "create", "copy", "drop",
        "equal", "boundenv"};
    return (size_t)kind < sizeof(names) / sizeof(names[0])
        ? names[kind] : "invalid";
}

static bool validate_imports(const SolMirRuntimeConventions *owner) {
    const SolMirLinkage *l = &owner->concrete->linkage;
    size_t expected = l->host_requirement_count;
    for (size_t i = 0; i < l->runtime_requirement_count; ++i) {
        if (!tick(1)) return false;
        for (uint32_t bit = 1; bit <= SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT;
            bit <<= 1) {
            if (!tick(1)) return false;
            size_t present = (l->runtime_requirements[i].operations & bit) != 0;
            if (!add_size(&expected, present)) return false;
        }
    }
    if (owner->import_count != expected) return false;
    for (size_t i = 0; i < owner->import_count; ++i) {
        if (!tick(1)) return false;
        const SolMirRuntimeImport *x = &owner->imports[i];
        if (memchr(x->symbol.bytes, '\0', sizeof(x->symbol.bytes)) == NULL
            || (i != 0 && memcmp(&owner->imports[i - 1].symbol, &x->symbol,
                sizeof(x->symbol)) >= 0)) return false;
        SolMirLinkageDigest digest;
        char symbol[SOL_MIR_RUNTIME_IMPORT_SYMBOL_CAPACITY] = {0};
        if (x->kind == SOL_MIR_RUNTIME_IMPORT_HOST) {
            if (x->host >= l->host_requirement_count
                 || x->recipe != SOL_MIR_RECIPE_NONE || x->recipe_operation != 0)
                return false;
            const SolMirLinkageHostRequirement *h = &l->host_requirements[x->host];
            if (!tick(sizeof("sol.mir.runtime-host-import/1")
                    + sizeof(h->semantic_id) + sizeof(h->requirement_key)))
                return false;
            hash_host(h, &digest);
            (void)snprintf(symbol, sizeof(symbol),
                "sol.h1.%016" PRIx64 "%016" PRIx64 ".", h->semantic_id.high,
                h->semantic_id.low);
            if (!tick(SOL_MIR_LINKAGE_DIGEST_BYTES)) return false;
            digest_hex(&digest, symbol + 40); symbol[104] = '\0';
        } else {
            static const uint32_t flags[] = {0, SOL_MIR_LINKAGE_RUNTIME_CREATE,
                SOL_MIR_LINKAGE_RUNTIME_COPY, SOL_MIR_LINKAGE_RUNTIME_DROP,
                SOL_MIR_LINKAGE_RUNTIME_EQUAL,
                SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT};
            if ((size_t)x->kind >= sizeof(flags) / sizeof(flags[0])
                || x->host != SOL_MIR_RUNTIME_NONE
                || x->recipe_operation != flags[x->kind]) return false;
            const SolMirLinkageRuntimeRequirement *requirement = NULL;
            for (size_t q = 0; q < l->runtime_requirement_count; ++q) {
                if (!tick(1)) return false;
                if (l->runtime_requirements[q].recipe == x->recipe)
                    requirement = &l->runtime_requirements[q];
            }
            if (requirement == NULL
                || (requirement->operations & x->recipe_operation) == 0)
                return false;
            if (!tick(sizeof("sol.mir.runtime-recipe-import/1") + 1
                    + sizeof(requirement->recipe_key))) return false;
            hash_recipe((uint8_t)x->kind, &requirement->recipe_key, &digest);
            int prefix = snprintf(symbol, sizeof(symbol), "sol.r1.%s.",
                operation_name(x->kind));
            if (prefix < 0 || (size_t)prefix + 64 >= sizeof(symbol)) return false;
            if (!tick(SOL_MIR_LINKAGE_DIGEST_BYTES)) return false;
            digest_hex(&digest, symbol + (size_t)prefix);
            symbol[(size_t)prefix + 64] = '\0';
        }
        if (memcmp(&digest, &x->identity, sizeof(digest)) != 0
            || memcmp(symbol, x->symbol.bytes, sizeof(symbol)) != 0) return false;
    }
    return true;
}

typedef struct {
    size_t signatures, slots, calls, operands, writebacks, entries, imports;
    size_t failure_sites;
} ExpectedCounts;

static bool build_work_add(size_t *work, size_t amount) {
    if (preflight_step() && add_size(work, amount))
        return true;
    reconstruction_overflow = true;
    return false;
}

static bool count_add(size_t *count, size_t amount) {
    if (preflight_step() && add_size(count, amount))
        return true;
    reconstruction_overflow = true;
    return false;
}

static const SolMirMaterializedImage *image_for_instance(
    const SolMirMaterialization *m, SolMirPlanInstanceId instance,
    size_t *work) {
    const SolMirMaterializedImage *result = NULL;
    for (size_t i = 0; i < m->image_count; ++i) {
        if (!build_work_add(work, 1)) return NULL;
        if (m->images[i].instance == instance) result = &m->images[i];
    }
    return result;
}

static bool signature_population_work(size_t parameters, bool receiver,
    size_t *work) {
    if (receiver && !build_work_add(work, 1)) return false;
    for (size_t i = 0; i < parameters; ++i)
        if (!build_work_add(work, 2)) return false;
    return build_work_add(work, 1);
}

static bool recipe_is_indirect(const SolMirConcreteProgram *c,
    SolMirRecipeId recipe, bool *result) {
    *result = false;
    const SolMirMaterialization *m = &c->materialization;
    for (size_t i = 0; i < m->block_count; ++i) {
        if (!preflight_step()) return false;
        const SolMirMaterializedTerminator *term = &m->blocks[i].terminator;
        if (term->kind == SOL_MIR_TERM_INVOKE
            && term->call_kind == SOL_IR_CALL_CALLBACK
            && m->temporaries[term->callee].type == recipe) {
            *result = true;
            return true;
        }
    }
    const SolMirOperations *o = &c->operations;
    for (size_t i = 0; i < o->predicate_block_count; ++i) {
        if (!preflight_step()) return false;
        const SolMirPredicateTerminator *term
            = &o->predicate_blocks[i].terminator;
        if (term->kind == SOL_MIR_PREDICATE_TERM_INVOKE
            && predicate_indirect(o, term)
            && o->predicate_values[term->callee].recipe == recipe) {
            *result = true;
            return true;
        }
    }
    return true;
}

static bool indirect_signature_work(const SolMirConcreteProgram *c,
    size_t direct_count, SolMirRecipeId recipe,
    size_t *work) {
    size_t position = direct_count;
    for (size_t i = 0; i < c->representation.recipe_count; ++i) {
        bool indirect;
        if (!recipe_is_indirect(c, i, &indirect)) return false;
        if (!indirect) continue;
        if (i == recipe) {
            size_t comparisons = position;
            return count_add(&comparisons, 1)
                && build_work_add(work, comparisons);
        }
        if (!count_add(&position, 1)) return false;
    }
    return false;
}

static bool table_site_work(const SolMirConcreteProgram *c, size_t site,
    size_t *work) {
    for (size_t i = 0; i < c->operations.callable_count; ++i) {
        if (!build_work_add(work, 1)) return false;
        if (c->operations.callables[i].semantic_site == site) return true;
    }
    return false;
}

static bool table_predicate_work(const SolMirConcreteProgram *c, size_t value,
    size_t *work) {
    const SolMirOperations *o = &c->operations;
    if (value >= o->predicate_value_count) return false;
    const SolMirPredicateValue *v = &o->predicate_values[value];
    if (v->kind != SOL_MIR_PREDICATE_VALUE_INSTRUCTION
        || v->definition >= o->predicate_instruction_count) return false;
    const SolMirPredicateInstruction *instruction
        = &o->predicate_instructions[v->definition];
    if (instruction->kind != SOL_MIR_PREDICATE_INST_FUNCTION
        && instruction->kind != SOL_MIR_PREDICATE_INST_BOUND_OPERATION)
        return false;
    for (size_t i = 0; i < o->callable_count; ++i) {
        if (!build_work_add(work, 1)) return false;
        const SolMirOperationCallablePlan *plan = &o->callables[i];
        if (plan->function_recipe == v->recipe
            && c->materialization.semantic_sites[plan->semantic_site].binding
                == instruction->binding) return true;
    }
    return false;
}

typedef struct {
    uint32_t flag;
    SolMirRuntimeImportKind kind;
    uint8_t tag;
} ExpectedImportOperation;

static const ExpectedImportOperation expected_import_operations[] = {
    {SOL_MIR_LINKAGE_RUNTIME_CREATE, SOL_MIR_RUNTIME_IMPORT_RECIPE_CREATE, 1},
    {SOL_MIR_LINKAGE_RUNTIME_COPY, SOL_MIR_RUNTIME_IMPORT_RECIPE_COPY, 2},
    {SOL_MIR_LINKAGE_RUNTIME_DROP, SOL_MIR_RUNTIME_IMPORT_RECIPE_DROP, 3},
    {SOL_MIR_LINKAGE_RUNTIME_EQUAL, SOL_MIR_RUNTIME_IMPORT_RECIPE_EQUAL, 4},
    {SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT,
        SOL_MIR_RUNTIME_IMPORT_RECIPE_BOUND_ENVIRONMENT, 5},
};

typedef struct {
    bool host;
    size_t host_id;
    size_t requirement;
    size_t operation;
} ExpectedImportDescriptor;

static bool expected_import_candidate(const SolMirLinkage *l, size_t index,
    ExpectedImportDescriptor *descriptor) {
    if (index < l->host_requirement_count) {
        if (!preflight_step()) return false;
        *descriptor = (ExpectedImportDescriptor){.host = true,
            .host_id = index};
        return true;
    }
    index -= l->host_requirement_count;
    size_t operation_count = sizeof(expected_import_operations)
        / sizeof(expected_import_operations[0]);
    for (size_t requirement = 0; requirement < l->runtime_requirement_count;
            ++requirement) {
        for (size_t operation = 0; operation < operation_count; ++operation) {
            if (!preflight_step()) return false;
            if ((l->runtime_requirements[requirement].operations
                    & expected_import_operations[operation].flag) == 0)
                continue;
            if (index-- != 0) continue;
            *descriptor = (ExpectedImportDescriptor){
                .requirement = requirement, .operation = operation};
            return true;
        }
    }
    return false;
}

static bool materialize_expected_import(const SolMirLinkage *l,
    ExpectedImportDescriptor descriptor, SolMirRuntimeImport *item) {
    memset(item, 0, sizeof(*item));
    if (descriptor.host) {
        if (descriptor.host_id >= l->host_requirement_count
            || !tick(sizeof("sol.mir.runtime-host-import/1") + 16
                + SOL_MIR_LINKAGE_DIGEST_BYTES)
            || !tick(SOL_MIR_LINKAGE_DIGEST_BYTES)) return false;
        const SolMirLinkageHostRequirement *host
            = &l->host_requirements[descriptor.host_id];
        item->kind = SOL_MIR_RUNTIME_IMPORT_HOST;
        item->host = descriptor.host_id;
        item->recipe = SOL_MIR_RECIPE_NONE;
        hash_host(host, &item->identity);
        SolMirLinkageDigest symbol_identity = item->identity;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        if (sol_mir_runtime_force_host_collision)
            memset(&item->identity, 0, sizeof(item->identity));
#endif
        (void)snprintf(item->symbol.bytes, sizeof(item->symbol.bytes),
            "sol.h1.%016" PRIx64 "%016" PRIx64 ".",
            host->semantic_id.high, host->semantic_id.low);
        digest_hex(&symbol_identity, item->symbol.bytes + 40);
        item->symbol.bytes[104] = '\0';
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        if (sol_mir_runtime_force_host_symbol_collision) {
            memset(&item->symbol, 0, sizeof(item->symbol));
            memcpy(item->symbol.bytes, "sol.test.host-collision", 23);
        }
#endif
        return true;
    }
    if (descriptor.requirement >= l->runtime_requirement_count
        || descriptor.operation >= sizeof(expected_import_operations)
            / sizeof(expected_import_operations[0])) return false;
    const SolMirLinkageRuntimeRequirement *requirement
        = &l->runtime_requirements[descriptor.requirement];
    const ExpectedImportOperation *operation
        = &expected_import_operations[descriptor.operation];
    if (!tick(sizeof("sol.mir.runtime-recipe-import/1") + 1
            + SOL_MIR_LINKAGE_DIGEST_BYTES)
        || !tick(SOL_MIR_LINKAGE_DIGEST_BYTES)) return false;
    item->kind = operation->kind;
    item->host = SOL_MIR_RUNTIME_NONE;
    item->recipe = requirement->recipe;
    item->recipe_operation = operation->flag;
    hash_recipe(operation->tag, &requirement->recipe_key, &item->identity);
    SolMirLinkageDigest symbol_identity = item->identity;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (sol_mir_runtime_force_recipe_collision)
        memset(&item->identity, 0, sizeof(item->identity));
#endif
    int prefix = snprintf(item->symbol.bytes, sizeof(item->symbol.bytes),
        "sol.r1.%s.", operation_name(item->kind));
    if (prefix < 0 || (size_t)prefix + 64 >= sizeof(item->symbol.bytes))
        return false;
    digest_hex(&symbol_identity, item->symbol.bytes + (size_t)prefix);
    item->symbol.bytes[(size_t)prefix + 64] = '\0';
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (sol_mir_runtime_force_recipe_symbol_collision) {
        memset(&item->symbol, 0, sizeof(item->symbol));
        memcpy(item->symbol.bytes, "sol.test.recipe-collision", 25);
    }
#endif
    return true;
}

static bool same_import_descriptor(const SolMirRuntimeImport *a,
    const SolMirRuntimeImport *b) {
    return a->kind == b->kind && a->host == b->host
        && a->recipe == b->recipe
        && a->recipe_operation == b->recipe_operation;
}

static bool add_product(size_t *value, size_t left, size_t right);

typedef struct {
    size_t imports;
    size_t import_build_work;
    size_t validation_scratch_bytes;
    size_t validation_work;
    size_t concrete_validation_work;
    ExpectedCounts counts;
    size_t build_work;
} ShallowPreflight;

static bool reconstruct_build(const SolMirConcreteProgram *concrete,
    const ShallowPreflight *shallow,
    ExpectedCounts *expected_counts, size_t *expected_work,
    SolDiagnostics *diagnostics,
    SolMirRuntimeConventionsBuildOutcome *outcome);

static bool linkage_limits_complete(SolMirLinkageLimits v) {
#define REQUIRED(member) v.member != 0
    return REQUIRED(max_callables) && REQUIRED(max_bindings)
        && REQUIRED(max_entry_exports) && REQUIRED(max_table_entries)
        && REQUIRED(max_callable_values) && REQUIRED(max_host_requirements)
        && REQUIRED(max_runtime_requirements) && REQUIRED(max_owned_bytes)
        && REQUIRED(max_build_scratch_bytes) && REQUIRED(max_build_work)
        && REQUIRED(max_validation_scratch_bytes)
        && REQUIRED(max_validation_work);
#undef REQUIRED
}

static bool shallow_predecessor_headers(const SolMirConcreteProgram *c) {
    if (c == NULL || !preflight_charge(8) || c->program.ir == NULL
        || c->plan.program != &c->program
        || c->materialization.plan != &c->plan
        || c->representation.materialization != &c->materialization
        || c->layout.representation != &c->representation
        || c->operations.layout != &c->layout
        || c->linkage.operations != &c->operations
        || !linkage_limits_complete(c->linkage.limits)) return false;
#define SHALLOW_RANGE(pointer, count, capacity, type) \
    if (!preflight_step() || (count) != (capacity) \
        || !range_valid((pointer), (capacity), sizeof(type))) return false;
    if (!preflight_charge(6)
        || !range_valid(c->program.roots, c->program.root_count,
            sizeof(SolMirProgramRoot))
        || !range_valid(c->program.approved_imports,
            c->program.approved_import_count, sizeof(SolIrCallableId))
        || !range_valid(c->program.templates, c->program.template_count,
            sizeof(SolMirProgramTemplate))
        || !range_valid(c->program.imports, c->program.import_count,
            sizeof(SolMirProgramImport))
        || !range_valid(c->program.specializations,
            c->program.specialization_count, sizeof(SolMirProgramSpecialization))
        || !range_valid(c->program.references, c->program.reference_count,
            sizeof(SolMirProgramReference))) return false;
    const SolMirPlan *p = &c->plan;
    SHALLOW_RANGE(p->types, p->type_count, p->type_capacity, SolMirPlanType)
    SHALLOW_RANGE(p->type_components, p->type_component_count,
        p->type_component_capacity, SolMirPlanTypeId)
    SHALLOW_RANGE(p->type_parameter_accesses, p->type_parameter_access_count,
        p->type_parameter_access_capacity, SolAccessMode)
    SHALLOW_RANGE(p->effect_atoms, p->effect_atom_count,
        p->effect_atom_capacity, SolMirPlanEffectAtom)
    SHALLOW_RANGE(p->effect_rows, p->effect_row_count,
        p->effect_row_capacity, SolMirPlanEffectRow)
    SHALLOW_RANGE(p->effect_row_atoms, p->effect_row_atom_count,
        p->effect_row_atom_capacity, size_t)
    SHALLOW_RANGE(p->instances, p->instance_count,
        p->instance_capacity, SolMirPlanInstance)
    SHALLOW_RANGE(p->instance_type_ids, p->instance_type_id_count,
        p->instance_type_id_capacity, SolMirPlanTypeId)
    SHALLOW_RANGE(p->instance_accesses, p->instance_access_count,
        p->instance_access_capacity, SolAccessMode)
    SHALLOW_RANGE(p->dictionary_entries, p->dictionary_entry_count,
        p->dictionary_entry_capacity, SolMirPlanDictionaryEntry)
    SHALLOW_RANGE(p->imports, p->import_count, p->import_capacity,
        SolMirPlanImport)
    SHALLOW_RANGE(p->typed_uses, p->typed_use_count, p->typed_use_capacity,
        SolMirPlanTypedUse)
    SHALLOW_RANGE(p->contexts, p->context_count, p->context_capacity,
        SolMirPlanContext)
    SHALLOW_RANGE(p->demands, p->demand_count, p->demand_capacity,
        SolMirPlanDemand)
    const SolMirMaterialization *m = &c->materialization;
#define MATERIAL_RANGE(member, type, singular) \
    SHALLOW_RANGE(m->member, m->singular##_count, m->singular##_capacity, type)
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
    const SolMirRepresentation *r = &c->representation;
#define REPRESENTATION_RANGE(member, type, singular) \
    SHALLOW_RANGE(r->member, r->singular##_count, r->singular##_capacity, type)
    REPRESENTATION_RANGE(recipes, SolMirRecipe, recipe)
    REPRESENTATION_RANGE(fields, SolMirRecipeField, field)
    REPRESENTATION_RANGE(variants, SolMirRecipeVariant, variant)
    REPRESENTATION_RANGE(recipe_ids, SolMirRecipeId, recipe_id)
    REPRESENTATION_RANGE(accesses, SolAccessMode, access)
    REPRESENTATION_RANGE(receiver_roots, SolMirMaterializedLocalId, receiver_root)
    REPRESENTATION_RANGE(callable_producers, SolMirCallableProducer,
        callable_producer)
#undef REPRESENTATION_RANGE
    const SolMirLayout *layout = &c->layout;
    SHALLOW_RANGE(layout->types, layout->type_count, layout->type_capacity,
        SolMirTypeLayout)
    SHALLOW_RANGE(layout->fields, layout->field_count, layout->field_capacity,
        SolMirFieldLayout)
    SHALLOW_RANGE(layout->variants, layout->variant_count,
        layout->variant_capacity, SolMirVariantLayout)
    SHALLOW_RANGE(layout->projections, layout->projection_count,
        layout->projection_capacity, SolMirProjectionMap)
#define OPERATION_RANGE(member, type, singular) \
    SHALLOW_RANGE(c->operations.member, c->operations.singular##_count, \
        c->operations.singular##_capacity, type)
    SOL_MIR_OPERATIONS_ARENAS(OPERATION_RANGE)
#undef OPERATION_RANGE
    const SolMirLinkage *l = &c->linkage;
#define SHALLOW_HEADER(member, type, singular) \
    if (!preflight_step() \
        || l->singular##_count != l->singular##_capacity \
        || !range_valid(l->member, l->singular##_capacity, sizeof(type))) \
        return false;
    SOL_MIR_LINKAGE_ARENAS(SHALLOW_HEADER)
#undef SHALLOW_HEADER
    const SolIr *ir = c->program.ir;
#define IR_RANGE(member, count, type) \
    if (!preflight_step() \
        || !range_valid(ir->member, ir->count, sizeof(type))) return false;
    IR_RANGE(definitions, definition_count, SolIrDefinition)
    IR_RANGE(callables, callable_count, SolIrCallable)
    IR_RANGE(types, type_count, SolIrType)
    IR_RANGE(type_ids, type_id_count, SolIrTypeId)
    IR_RANGE(accesses, access_count, SolAccessMode)
    IR_RANGE(members, member_count, SolIrMember)
    IR_RANGE(evidence, evidence_count, SolIrDispatchEvidence)
    IR_RANGE(locals, local_count, SolIrLocal)
    IR_RANGE(fields, field_count, SolIrField)
    IR_RANGE(variants, variant_count, SolIrVariant)
    IR_RANGE(expressions, expression_count, SolIrExpression)
    IR_RANGE(places, place_count, SolIrPlace)
    IR_RANGE(projections, projection_count, SolIrProjection)
    IR_RANGE(statements, statement_count, SolIrStatement)
    IR_RANGE(statement_ids, statement_id_count, SolIrStatementId)
    IR_RANGE(arms, arm_count, SolIrArm)
    IR_RANGE(arm_ids, arm_id_count, SolIrArmId)
    IR_RANGE(patterns, pattern_count, SolIrPattern)
    IR_RANGE(pattern_children, pattern_child_count, SolIrPatternChild)
    IR_RANGE(operands, operand_count, SolIrOperand)
    IR_RANGE(roots, root_count, SolIrLocalId)
    IR_RANGE(obligations, obligation_count, SolIrObligation)
    IR_RANGE(snapshots, snapshot_count, SolIrSnapshot)
    IR_RANGE(cleanup_locals, cleanup_local_count, SolIrLocalId)
    IR_RANGE(effects, effect_count, SolIrEffect)
    IR_RANGE(generic_parameters, generic_parameter_count, SolIrGenericParameter)
    IR_RANGE(effect_parameters, effect_parameter_count, SolIrEffectParameter)
    IR_RANGE(loop_obligations, loop_obligation_count, SolObligationId)
    IR_RANGE(unreachable_obligations, unreachable_obligation_count,
        SolObligationId)
    IR_RANGE(files, file_count, SolIrSourceFile)
#undef IR_RANGE
    if (!preflight_step() || ir->source_path == NULL || ir->source_bytes == NULL
        || ir->source_length == SIZE_MAX
        || !range_valid(ir->source_bytes, ir->source_length + 1, sizeof(char)))
        return false;
    for (size_t i = 0; i < p->effect_atom_count; ++i)
        if (!preflight_step() || p->effect_atoms[i].name == NULL) return false;
    for (size_t i = 0; i < ir->effect_count; ++i)
        if (!preflight_step() || ir->effects[i].name == NULL) return false;
    for (size_t i = 0; i < ir->expression_count; ++i) {
        if (!preflight_step()) return false;
        if ((ir->expressions[i].kind == SOL_IR_EXPR_STRING
                && ir->expressions[i].as.string == NULL)
            || (ir->expressions[i].kind == SOL_IR_EXPR_HANDLE
                && ir->expressions[i].as.handler.effect_name == NULL))
            return false;
    }
    for (size_t i = 0; i < ir->generic_parameter_count; ++i)
        if (!preflight_step() || ir->generic_parameters[i].name == NULL)
            return false;
    for (size_t i = 0; i < ir->effect_parameter_count; ++i)
        if (!preflight_step() || ir->effect_parameters[i].name == NULL)
            return false;
    for (size_t i = 0; i < ir->file_count; ++i)
        if (!preflight_step() || ir->files[i].path == NULL) return false;
    for (size_t i = 0; i < m->image_count; ++i) {
        if (!preflight_slice(m->images[i].parameter_types, m->type_id_count)
            || !preflight_slice(m->images[i].parameter_accesses,
                m->access_count)
            || !preflight_slice(m->images[i].blocks, m->block_count))
            return false;
    }
    for (size_t i = 0; i < l->host_requirement_count; ++i)
        if (!preflight_slice(l->host_requirements[i].parameters,
                m->type_id_count)
            || !preflight_slice(l->host_requirements[i].parameter_accesses,
                m->access_count)) return false;
    for (size_t i = 0; i < r->recipe_count; ++i)
        if (!preflight_slice(r->recipes[i].parameters, r->recipe_id_count)
            || !preflight_slice(r->recipes[i].parameter_accesses,
                r->access_count)) return false;
    for (size_t i = 0; i < l->entry_export_count; ++i)
        if (!preflight_step()
            || l->entry_exports[i].callable >= l->callable_count) return false;
    for (size_t i = 0; i < m->block_count; ++i) {
        if (!preflight_step()) return false;
        const SolMirMaterializedTerminator *term = &m->blocks[i].terminator;
        if (term->kind != SOL_MIR_TERM_INVOKE) continue;
        if (!preflight_slice(term->arguments, m->call_argument_count)
            || !preflight_slice(term->writebacks, m->writeback_count)
            || (term->call_kind == SOL_IR_CALL_CALLBACK
                && term->callee >= m->temporary_count)) return false;
    }
    for (size_t i = 0; i < c->operations.predicate_body_count; ++i)
        if (!preflight_slice(c->operations.predicate_bodies[i].blocks,
                c->operations.predicate_block_count)) return false;
    for (size_t i = 0; i < c->operations.predicate_block_count; ++i) {
        if (!preflight_step()) return false;
        const SolMirPredicateBlock *block = &c->operations.predicate_blocks[i];
        if (!preflight_slice(block->instructions,
                c->operations.predicate_instruction_count)) return false;
        const SolMirPredicateTerminator *term = &block->terminator;
        if (term->kind != SOL_MIR_PREDICATE_TERM_INVOKE) continue;
        if (!preflight_slice(term->arguments,
                c->operations.predicate_operand_count)
            || ((term->call_kind == SOL_IR_CALL_CALLBACK
                    || term->call_kind == SOL_IR_CALL_CAPABILITY)
                && term->callee >= c->operations.predicate_value_count))
            return false;
        if (predicate_indirect(&c->operations, term)) {
            const SolMirPredicateValue *value
                = &c->operations.predicate_values[term->callee];
            if (!preflight_step()
                || value->kind != SOL_MIR_PREDICATE_VALUE_INSTRUCTION
                || value->definition
                    >= c->operations.predicate_instruction_count)
                return false;
        }
    }
    for (size_t i = 0; i < c->operations.callable_count; ++i)
        if (!preflight_step()
            || c->operations.callables[i].semantic_site
                >= m->semantic_site_count) return false;
#undef SHALLOW_RANGE
    bool operations_match = true;
#define SHALLOW_OPERATION(member, type, singular) \
    operations_match = operations_match && preflight_step() \
        && c->operations.usage.member <= c->operations.limits.max_##member;
    SOL_MIR_OPERATIONS_ARENAS(SHALLOW_OPERATION)
#undef SHALLOW_OPERATION
    return preflight_step() && operations_match
        && c->program.usage.callable_classifications
            <= c->program.limits.max_callable_classifications
        && c->program.usage.references <= c->program.limits.max_references
        && c->plan.usage.instances <= c->plan.limits.max_instances
        && c->plan.usage.concrete_types <= c->plan.limits.max_concrete_types
        && c->plan.usage.demands <= c->plan.limits.max_demands
        && c->plan.usage.typed_uses <= c->plan.limits.max_typed_uses
        && c->plan.usage.contexts <= c->plan.limits.max_contexts
        && c->materialization.usage.instances
            <= c->materialization.limits.max_instances
        && c->materialization.usage.cfg_items
            <= c->materialization.limits.max_cfg_items
        && c->materialization.usage.bindings
            <= c->materialization.limits.max_bindings
        && c->materialization.usage.concrete_records
            <= c->materialization.limits.max_concrete_records
        && c->representation.usage.recipes
            <= c->representation.limits.max_recipes
        && c->representation.usage.fields
            <= c->representation.limits.max_fields
        && c->representation.usage.variants
            <= c->representation.limits.max_variants
        && c->representation.usage.recipe_ids
            <= c->representation.limits.max_recipe_ids
        && c->representation.usage.callable_producers
            <= c->representation.limits.max_callable_producers
        && c->representation.usage.receiver_roots
            <= c->representation.limits.max_receiver_roots
        && c->layout.usage.type_layouts <= c->layout.limits.max_type_layouts
        && c->layout.usage.field_layouts <= c->layout.limits.max_field_layouts
        && c->layout.usage.variant_layouts
            <= c->layout.limits.max_variant_layouts
        && c->layout.usage.projection_maps
            <= c->layout.limits.max_projection_maps
        && l->usage.callables == l->callable_count
        && l->usage.bindings == l->binding_count
        && l->usage.entry_exports == l->entry_export_count
        && l->usage.table_entries == l->table_entry_count
        && l->usage.callable_values == l->callable_value_count
        && l->usage.host_requirements == l->host_requirement_count
        && l->usage.runtime_requirements == l->runtime_requirement_count
        && l->callable_count <= l->limits.max_callables
        && l->binding_count <= l->limits.max_bindings
        && l->entry_export_count <= l->limits.max_entry_exports
        && l->table_entry_count <= l->limits.max_table_entries
        && l->callable_value_count <= l->limits.max_callable_values
        && l->host_requirement_count <= l->limits.max_host_requirements
        && l->runtime_requirement_count <= l->limits.max_runtime_requirements
        && l->usage.owned_bytes <= l->limits.max_owned_bytes
        && l->usage.build_scratch_bytes <= l->limits.max_build_scratch_bytes
        && l->usage.build_work <= l->limits.max_build_work
        && l->usage.validation_scratch_bytes
            <= l->limits.max_validation_scratch_bytes
        && l->usage.validation_work <= l->limits.max_validation_work;
}

/* Requirement reconstruction is itself validation work. Each checked addition
   below charges the reconstruction step and records the operation it predicts. */
static bool require_work(size_t *work, size_t predicted) {
    return tick(1) && add_size(work, 1) && add_size(work, predicted);
}

static bool require_text(const char *text, size_t *work) {
    size_t at = 0;
    do {
        if (!require_work(work, 1) || at == SIZE_MAX) return false;
    } while (text[at++] != '\0');
    return require_work(work, 9);
}

static bool alias_validation_requirement(const SolMirConcreteProgram *c,
    size_t *work) {
    /* Eight owned arenas against the owner and each other. */
    if (!require_work(work, 36)) return false;
#define REQUIRE_AGAINST() \
    do { if (!require_work(work, 9)) return false; } while (0)
    REQUIRE_AGAINST();
    REQUIRE_AGAINST(); REQUIRE_AGAINST(); REQUIRE_AGAINST();
    REQUIRE_AGAINST(); REQUIRE_AGAINST(); REQUIRE_AGAINST();
    for (size_t i = 0; i < c->program.template_count; ++i) {
        if (!require_work(work, 1)) return false;
        for (size_t q = 0; q < 10; ++q) REQUIRE_AGAINST();
    }
    for (size_t i = 0; i < 14; ++i) REQUIRE_AGAINST();
#define REQUIRE_FIELD(member, type, singular) REQUIRE_AGAINST();
    SOL_MIR_OPERATIONS_ARENAS(REQUIRE_FIELD)
    SOL_MIR_LINKAGE_ARENAS(REQUIRE_FIELD)
#undef REQUIRE_FIELD
    for (size_t i = 0; i < 32 + 7 + 4; ++i) REQUIRE_AGAINST();
    for (size_t i = 0; i < c->materialization.image_count; ++i) {
        if (!require_work(work, 1)) return false;
        for (size_t q = 0; q < 10; ++q) REQUIRE_AGAINST();
    }
    /* The IR object, thirty arenas, source path, and source bytes. */
    for (size_t i = 0; i < 33; ++i) REQUIRE_AGAINST();
    for (size_t i = 0; i < c->plan.effect_atom_count; ++i) {
        if (!require_work(work, 1)) return false;
        REQUIRE_AGAINST();
    }
    const SolIr *ir = c->program.ir;
#define REQUIRE_OPTIONAL(array, count, member) do { \
    for (size_t i = 0; i < (count); ++i) { \
        if (!require_work(work, 1)) return false; \
        if ((array)[i].member != NULL) REQUIRE_AGAINST(); \
    } \
} while (0)
    REQUIRE_OPTIONAL(ir->definitions, ir->definition_count, name);
    REQUIRE_OPTIONAL(ir->callables, ir->callable_count, name);
    REQUIRE_OPTIONAL(ir->locals, ir->local_count, name);
    REQUIRE_OPTIONAL(ir->fields, ir->field_count, name);
    REQUIRE_OPTIONAL(ir->variants, ir->variant_count, name);
    REQUIRE_OPTIONAL(ir->statements, ir->statement_count, region_label);
#undef REQUIRE_OPTIONAL
    for (size_t i = 0; i < ir->effect_count; ++i) {
        if (!require_work(work, 1)) return false;
        REQUIRE_AGAINST();
    }
    for (size_t i = 0; i < ir->expression_count; ++i) {
        if (!require_work(work, 1)) return false;
        if (ir->expressions[i].kind == SOL_IR_EXPR_STRING
            || ir->expressions[i].kind == SOL_IR_EXPR_HANDLE)
            REQUIRE_AGAINST();
    }
    for (size_t i = 0; i < ir->generic_parameter_count; ++i) {
        if (!require_work(work, 1)) return false;
        REQUIRE_AGAINST();
    }
    for (size_t i = 0; i < ir->effect_parameter_count; ++i) {
        if (!require_work(work, 1)) return false;
        REQUIRE_AGAINST();
    }
    for (size_t i = 0; i < ir->file_count; ++i) {
        if (!require_work(work, 1)) return false;
        REQUIRE_AGAINST();
    }
#undef REQUIRE_AGAINST
    return true;
}

static bool text_validation_requirement(const SolMirConcreteProgram *c,
    size_t *work) {
    const SolIr *ir = c->program.ir;
    if (!require_text(ir->source_path, work) || !require_work(work, 9))
        return false;
    for (size_t i = 0; i < c->plan.effect_atom_count; ++i)
        if (!require_work(work, 1) || !require_work(work, 9)) return false;
#define REQUIRE_OPTIONAL_TEXT(array, count, member) do { \
    for (size_t i = 0; i < (count); ++i) { \
        if (!require_work(work, 1)) return false; \
        if ((array)[i].member != NULL \
            && !require_text((array)[i].member, work)) return false; \
    } \
} while (0)
    REQUIRE_OPTIONAL_TEXT(ir->definitions, ir->definition_count, name);
    REQUIRE_OPTIONAL_TEXT(ir->callables, ir->callable_count, name);
    REQUIRE_OPTIONAL_TEXT(ir->locals, ir->local_count, name);
    REQUIRE_OPTIONAL_TEXT(ir->fields, ir->field_count, name);
    REQUIRE_OPTIONAL_TEXT(ir->variants, ir->variant_count, name);
    REQUIRE_OPTIONAL_TEXT(ir->statements, ir->statement_count, region_label);
#undef REQUIRE_OPTIONAL_TEXT
    for (size_t i = 0; i < ir->effect_count; ++i)
        if (!require_work(work, 1) || !require_text(ir->effects[i].name, work))
            return false;
    for (size_t i = 0; i < ir->expression_count; ++i) {
        if (!require_work(work, 1)) return false;
        const char *text = ir->expressions[i].kind == SOL_IR_EXPR_STRING
            ? ir->expressions[i].as.string
            : ir->expressions[i].kind == SOL_IR_EXPR_HANDLE
                ? ir->expressions[i].as.handler.effect_name : NULL;
        if (text != NULL && !require_text(text, work)) return false;
    }
    for (size_t i = 0; i < ir->generic_parameter_count; ++i)
        if (!require_work(work, 1)
            || !require_text(ir->generic_parameters[i].name, work)) return false;
    for (size_t i = 0; i < ir->effect_parameter_count; ++i)
        if (!require_work(work, 1)
            || !require_text(ir->effect_parameters[i].name, work)) return false;
    for (size_t i = 0; i < ir->file_count; ++i)
        if (!require_work(work, 1) || !require_text(ir->files[i].path, work))
            return false;
    return true;
}

static bool indirect_recipe_requirement(const SolMirConcreteProgram *c,
    SolMirRecipeId recipe, size_t *work, bool *used) {
    *used = false;
    for (size_t i = 0; i < c->materialization.block_count; ++i) {
        if (!require_work(work, 0)) return false;
        const SolMirMaterializedTerminator *term
            = &c->materialization.blocks[i].terminator;
        if (term->kind == SOL_MIR_TERM_INVOKE
            && term->call_kind == SOL_IR_CALL_CALLBACK
            && c->materialization.temporaries[term->callee].type == recipe) {
            *used = true;
            return true;
        }
    }
    for (size_t i = 0; i < c->operations.predicate_block_count; ++i) {
        if (!require_work(work, 0)) return false;
        const SolMirPredicateTerminator *term
            = &c->operations.predicate_blocks[i].terminator;
        if (term->kind == SOL_MIR_PREDICATE_TERM_INVOKE
            && predicate_indirect(&c->operations, term)
            && c->operations.predicate_values[term->callee].recipe == recipe) {
            *used = true;
            return true;
        }
    }
    return true;
}

static bool local_validation_requirement(const SolMirConcreteProgram *c,
    const ExpectedCounts *counts, size_t *result) {
    const SolMirMaterialization *m = &c->materialization;
    const SolMirRepresentation *r = &c->representation;
    const SolMirOperations *o = &c->operations;
    const SolMirLinkage *l = &c->linkage;
    size_t work = 0;
    if (!require_work(&work, 8)
        || !alias_validation_requirement(c, &work)
        || !text_validation_requirement(c, &work)
        || !require_work(&work, counts->signatures)
        || !require_work(&work, counts->calls)) return false;
    for (size_t i = 0; i < l->callable_count; ++i) {
        if (!require_work(&work, 1 + m->image_count)) return false;
        const SolMirMaterializedImage *image = NULL;
        for (size_t q = 0; q < m->image_count; ++q) {
            if (!require_work(&work, 0)) return false;
            if (m->images[q].instance == l->callables[i].instance)
                image = &m->images[q];
        }
        if (image == NULL || !require_work(&work,
                image->parameter_types.count
                    + (image->receiver != SOL_MIR_RECIPE_NONE))) return false;
    }
    for (size_t i = 0; i < l->host_requirement_count; ++i)
        if (!require_work(&work, 1 + l->host_requirements[i].parameters.count
                + (l->host_requirements[i].receiver != SOL_MIR_RECIPE_NONE)))
            return false;
    for (size_t i = 0; i < r->recipe_count; ++i) {
        bool used;
        if (!indirect_recipe_requirement(c, i, &work, &used)) return false;
        if (used && !require_work(&work, 1 + m->block_count
                + o->predicate_block_count + r->recipes[i].parameters.count))
            return false;
    }
    size_t direct = l->callable_count;
    if (!add_size(&direct, l->host_requirement_count)) return false;
    for (size_t image = 0; image < m->image_count; ++image) {
        if (!require_work(&work, 1)) return false;
        SolMirPlanSlice blocks = m->images[image].blocks;
        for (size_t q = 0; q < blocks.count; ++q) {
            if (!require_work(&work, 1)) return false;
            const SolMirMaterializedTerminator *term
                = &m->blocks[blocks.offset + q].terminator;
            if (term->kind != SOL_MIR_TERM_INVOKE) continue;
            if (term->call_kind == SOL_IR_CALL_CALLBACK) {
                size_t position = direct;
                for (size_t recipe = 0; recipe < r->recipe_count; ++recipe) {
                    bool used;
                    if (!indirect_recipe_requirement(c, recipe, &work, &used))
                        return false;
                    if (!used) continue;
                    if (recipe == m->temporaries[term->callee].type) break;
                    if (!add_size(&position, 1)) return false;
                }
                if (!require_work(&work, position + 1)) return false;
            }
            if (term->call_kind == SOL_IR_CALL_CALLBACK) {
                size_t visited = 0;
                for (; visited < o->callable_count; ++visited) {
                    if (!require_work(&work, 0)) return false;
                    if (o->callables[visited].semantic_site
                            == term->callable_site) break;
                }
                if (!require_work(&work, visited + 1)) return false;
            }
            if (!require_work(&work, term->arguments.count
                    + term->writebacks.count)) return false;
            size_t pairs = 0;
            if (term->writebacks.count > 1
                && (!mul_size(term->writebacks.count,
                        term->writebacks.count - 1, &pairs))) return false;
            if (!require_work(&work, pairs / 2)) return false;
        }
    }
    for (size_t body = 0; body < o->predicate_body_count; ++body) {
        if (!require_work(&work, 1)) return false;
        SolMirPlanSlice blocks = o->predicate_bodies[body].blocks;
        for (size_t q = 0; q < blocks.count; ++q) {
            if (!require_work(&work, 1)) return false;
            const SolMirPredicateTerminator *term
                = &o->predicate_blocks[blocks.offset + q].terminator;
            if (term->kind != SOL_MIR_PREDICATE_TERM_INVOKE) continue;
            if (predicate_indirect(o, term)) {
                size_t position = direct;
                SolMirRecipeId target = o->predicate_values[term->callee].recipe;
                for (size_t recipe = 0; recipe < r->recipe_count; ++recipe) {
                    bool used;
                    if (!indirect_recipe_requirement(c, recipe, &work, &used))
                        return false;
                    if (!used) continue;
                    if (recipe == target) break;
                    if (!add_size(&position, 1)) return false;
                }
                if (!require_work(&work, position + 1)) return false;
                size_t visited = 0;
                for (; visited < o->callable_count; ++visited) {
                    if (!require_work(&work, 0)) return false;
                    const SolMirOperationCallablePlan *plan
                        = &o->callables[visited];
                    if (plan->function_recipe == target
                        && m->semantic_sites[plan->semantic_site].binding
                            == o->predicate_instructions[
                                o->predicate_values[term->callee].definition]
                                .binding) break;
                }
                if (!require_work(&work, visited + 1)) return false;
            }
            if (!require_work(&work, term->arguments.count)) return false;
        }
    }
    size_t failure_site_work = counts->calls + o->arithmetic_count
        + m->image_count;
    if (!add_product(&failure_site_work, o->predicate_body_count,
            1 + c->program.ir->expression_count)) return false;
    for (size_t image = 0; image < m->image_count; ++image)
        if (!add_size(&failure_site_work, m->images[image].blocks.count))
            return false;
    if (!add_product(&failure_site_work, counts->failure_sites,
            1 + c->program.ir->file_count)
        || !require_work(&work, failure_site_work)) return false;
    for (size_t i = 0; i < l->entry_export_count; ++i) {
        const SolMirMaterializedImage *image = NULL;
        for (size_t q = 0; q < m->image_count; ++q) {
            if (!require_work(&work, 0)) return false;
            if (m->images[q].instance
                    == l->callables[l->entry_exports[i].callable].instance)
                image = &m->images[q];
        }
        if (image == NULL || !require_work(&work, 1
                + image->parameter_types.count
                + (image->receiver != SOL_MIR_RECIPE_NONE))) return false;
    }
    size_t runtime_import_scan;
    if (!mul_size(l->runtime_requirement_count, 6, &runtime_import_scan)
        || !require_work(&work, runtime_import_scan)) return false;
    for (size_t i = 0; i < l->host_requirement_count; ++i)
        if (!require_work(&work, 1 + sizeof("sol.mir.runtime-host-import/1")
                + sizeof(l->host_requirements[i].semantic_id)
                + sizeof(l->host_requirements[i].requirement_key)
                + SOL_MIR_LINKAGE_DIGEST_BYTES)) return false;
    for (size_t i = 0; i < l->runtime_requirement_count; ++i)
        for (size_t q = 0; q < sizeof(expected_import_operations)
                / sizeof(expected_import_operations[0]); ++q)
            if ((l->runtime_requirements[i].operations
                    & expected_import_operations[q].flag) != 0
                && !require_work(&work, 1 + l->runtime_requirement_count
                    + sizeof("sol.mir.runtime-recipe-import/1") + 1
                    + sizeof(l->runtime_requirements[i].recipe_key)
                    + SOL_MIR_LINKAGE_DIGEST_BYTES)) return false;
    if (!require_work(&work, 8)) return false;
    *result = work;
    return true;
}

/* Reconstruct the exact contract from shallow-authenticated predecessor data.
   The runtime meter independently executes the predicted semantic charges. */
static bool exact_validation_requirement(const SolMirConcreteProgram *c,
    const ExpectedCounts *counts, size_t concrete_validation_work,
    size_t *result) {
    size_t work = concrete_validation_work;
    if (!add_size(&work, preflight_steps)) return false;
    size_t runtime_candidates = counts->imports
        - c->linkage.host_requirement_count;
    size_t candidates = counts->imports;
    size_t descriptor_work = 0;
    if (!add_product(&descriptor_work, c->linkage.host_requirement_count,
            sizeof("sol.mir.runtime-host-import/1") + 16
                + 2 * SOL_MIR_LINKAGE_DIGEST_BYTES)
        || !add_product(&descriptor_work, runtime_candidates,
            sizeof("sol.mir.runtime-recipe-import/1") + 1
                + 2 * SOL_MIR_LINKAGE_DIGEST_BYTES)) return false;
    size_t descriptor_passes = candidates;
    if (!add_size(&descriptor_passes, 2)
        || !add_product(&work, descriptor_work, descriptor_passes)
        || !add_product(&work, candidates, candidates)
        || !add_size(&work, candidates)) return false;
    size_t pairs = 0;
    if (candidates > 1) {
        if (!mul_size(candidates, candidates - 1, &pairs)) return false;
        pairs /= 2;
    }
    size_t local_work;
    if (!add_size(&work, pairs)
        || !local_validation_requirement(c, counts, &local_work)
        || !add_size(&work, local_work)) return false;
    *result = work;
    return true;
}

static bool shallow_preflight(const SolMirConcreteProgram *c,
    const SolMirRuntimeConventionsLimits *limits, ShallowPreflight *preflight,
    SolMirRuntimeConventionsBuildOutcome *outcome) {
    reconstruction_overflow = false;
    preflight_steps = 0;
    *outcome = SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INVALID_CONCRETE_PROGRAM;
    if (!shallow_predecessor_headers(c)) return false;
    const SolMirLinkage *l = &c->linkage;
    size_t validation_scratch = c->linkage.usage.validation_scratch_bytes;
    size_t imports = l->host_requirement_count, import_build_work = 0;
    for (size_t i = 0; i < l->host_requirement_count; ++i) {
        if (!build_work_add(&import_build_work, 1)
            || !build_work_add(&import_build_work,
                sizeof("sol.mir.runtime-host-import/1") + 16
                    + SOL_MIR_LINKAGE_DIGEST_BYTES)
            || !build_work_add(&import_build_work,
                SOL_MIR_LINKAGE_DIGEST_BYTES)) goto exhausted;
    }
    for (size_t i = 0; i < l->runtime_requirement_count; ++i) {
        if (!build_work_add(&import_build_work, 1)) goto exhausted;
        for (size_t q = 0; q < sizeof(expected_import_operations)
                / sizeof(expected_import_operations[0]); ++q) {
            if (!build_work_add(&import_build_work, 1)) goto exhausted;
            if ((l->runtime_requirements[i].operations
                    & expected_import_operations[q].flag) == 0) continue;
            if (!count_add(&imports, 1)
                || !build_work_add(&import_build_work,
                    sizeof("sol.mir.runtime-recipe-import/1") + 1
                        + SOL_MIR_LINKAGE_DIGEST_BYTES)
                || !build_work_add(&import_build_work,
                    SOL_MIR_LINKAGE_DIGEST_BYTES)) goto exhausted;
        }
    }
    if (imports > limits->max_imports) goto exhausted;
    size_t candidates = imports;
    size_t actual_index = 0;
    for (size_t i = 0; i < candidates; ++i) {
        ExpectedImportDescriptor a_descriptor;
        SolMirRuntimeImport a;
        if (!expected_import_candidate(l, i, &a_descriptor)
            || !materialize_expected_import(l, a_descriptor, &a))
            goto exhausted;
        size_t greater = 0;
        bool has_not_greater = false;
        for (size_t q = 0; q < candidates; ++q) {
            ExpectedImportDescriptor b_descriptor;
            SolMirRuntimeImport b;
            if (!expected_import_candidate(l, q, &b_descriptor)
                || !materialize_expected_import(l, b_descriptor, &b))
                goto exhausted;
            if (!tick(1)) goto exhausted;
            int comparison = memcmp(b.symbol.bytes, a.symbol.bytes,
                sizeof(a.symbol.bytes));
            if (q < i) {
                if (comparison > 0) {
                    if (!count_add(&greater, 1)) goto exhausted;
                } else has_not_greater = true;
            }
            bool same_descriptor = same_import_descriptor(&a, &b);
            bool same_identity = false;
            if (q > i) {
                if (!tick(1)) goto exhausted;
                same_identity
                    = memcmp(&a.identity, &b.identity, sizeof(a.identity)) == 0;
            }
            if (q > i && !same_descriptor
                && (same_identity || comparison == 0)) {
                *outcome = SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SYMBOL_COLLISION;
                return false;
            }
        }
        if (!tick(1)) goto exhausted;
        if (actual_index != 0
            && (!build_work_add(&import_build_work, 1)
                || !build_work_add(&import_build_work, greater)
                || (has_not_greater
                    && !build_work_add(&import_build_work, 1)))) goto exhausted;
        ++actual_index;
    }
    if (actual_index != imports) goto exhausted;
    for (size_t i = 0; i < candidates; ++i) {
        ExpectedImportDescriptor descriptor;
        SolMirRuntimeImport item;
        if (!expected_import_candidate(l, i, &descriptor)
            || !materialize_expected_import(l, descriptor, &item))
            goto exhausted;
    }
    size_t pairs = 0;
    if (imports > 1) {
        if (!mul_size(imports, imports - 1, &pairs)) goto exhausted;
        pairs /= 2;
    }
    if (!build_work_add(&import_build_work, imports)
        || !build_work_add(&import_build_work, pairs)) goto exhausted;
    size_t concrete_validation_work, concrete_validation_scratch;
    if (!sol_mir_concrete_internal_validation_requirements(c,
            &concrete_validation_work, &concrete_validation_scratch))
        goto exhausted;
    if (concrete_validation_scratch > validation_scratch)
        validation_scratch = concrete_validation_scratch;
    ShallowPreflight result = {
        .imports = imports,
        .import_build_work = import_build_work,
        .validation_scratch_bytes = validation_scratch,
        .concrete_validation_work = concrete_validation_work,
    };
    if (!reconstruct_build(c, &result, &result.counts, &result.build_work,
            NULL, outcome)) goto exhausted;
    if (!exact_validation_requirement(c, &result.counts,
            result.concrete_validation_work,
            &result.validation_work)) goto exhausted;
    if (validation_scratch > limits->max_validation_scratch_bytes
        || result.validation_work > limits->max_validation_work)
        goto exhausted;
    *preflight = result;
    *outcome = SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED;
    return true;
exhausted:
    *outcome = SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED;
    return false;
}

static bool reconstruct_build(const SolMirConcreteProgram *concrete,
    const ShallowPreflight *shallow,
    ExpectedCounts *expected_counts, size_t *expected_work,
    SolDiagnostics *diagnostics,
    SolMirRuntimeConventionsBuildOutcome *outcome) {
    (void)diagnostics;
    *outcome = SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INTERNAL_FAILED;
    const SolMirConcreteProgram *c = concrete;
    const SolMirMaterialization *m = &c->materialization;
    const SolMirRepresentation *r = &c->representation;
    const SolMirOperations *o = &c->operations;
    const SolMirLinkage *l = &c->linkage;
    const SolIr *ir = c->program.ir;
    size_t work = 0;
    ExpectedCounts counts = {0};
    bool ok = true;
    if (r->recipe_count != 0) ok = build_work_add(&work, 1);
    ok = ok && count_add(&counts.signatures, l->callable_count)
        && count_add(&counts.signatures, l->host_requirement_count);
    counts.entries = l->entry_export_count;
    for (size_t i = 0; ok && i < l->entry_export_count; ++i) {
        ok = build_work_add(&work, 1);
        if (!ok) break;
        const SolMirMaterializedImage *image = image_for_instance(m,
            l->callables[l->entry_exports[i].callable].instance, &work);
        if (image == NULL) { ok = false; break; }
        for (size_t q = 0; ok && q < image->parameter_types.count; ++q)
            ok = build_work_add(&work, 1);
    }
    for (size_t i = 0; ok && i < l->runtime_requirement_count; ++i) {
        ok = build_work_add(&work, 1);
        for (uint32_t bit = 1; ok
                && bit <= SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT; bit <<= 1) {
            ok = build_work_add(&work, 1);
            if ((l->runtime_requirements[i].operations & bit) != 0)
                ok = count_add(&counts.imports, 1);
        }
    }
    ok = ok && count_add(&counts.imports, l->host_requirement_count);
    for (size_t i = 0; ok && i < m->block_count; ++i) {
        ok = build_work_add(&work, 1);
        const SolMirMaterializedTerminator *term = &m->blocks[i].terminator;
        if (!ok || term->kind != SOL_MIR_TERM_INVOKE) {
            if (ok && (term->kind == SOL_MIR_TERM_PANIC
                    || term->kind == SOL_MIR_TERM_MATCH_FAILURE
                    || term->kind == SOL_MIR_TERM_UNREACHABLE))
                ok = count_add(&counts.failure_sites, 1);
            continue;
        }
        ok = count_add(&counts.calls, 1)
            && count_add(&counts.failure_sites, 1)
            && count_add(&counts.operands, term->arguments.count)
            && count_add(&counts.writebacks, term->writebacks.count);
        if (ok && term->receiver.source_expression != SOL_IR_NONE)
            ok = count_add(&counts.operands, 1);
        if (ok && term->call_kind == SOL_IR_CALL_CALLBACK) {
            SolMirRecipeId recipe = m->temporaries[term->callee].type;
            if (recipe >= r->recipe_count) ok = false;
            else (void)recipe;
        }
    }
    for (size_t i = 0; ok && i < o->predicate_block_count; ++i) {
        ok = build_work_add(&work, 1);
        const SolMirPredicateTerminator *term
            = &o->predicate_blocks[i].terminator;
        if (!ok || term->kind != SOL_MIR_PREDICATE_TERM_INVOKE) {
            if (ok && term->kind == SOL_MIR_PREDICATE_TERM_FAILURE
                && term->failure_kind == SOL_MIR_PREDICATE_FAILURE_NO_MATCH)
                ok = count_add(&counts.failure_sites, 1);
            continue;
        }
        ok = count_add(&counts.calls, 1)
            && count_add(&counts.failure_sites, 1)
            && count_add(&counts.operands, term->arguments.count);
        if (ok && !predicate_indirect(o, term) && (term->receiver != SOL_MIR_OPERATION_NONE
                || term->call_kind == SOL_IR_CALL_CAPABILITY))
            ok = count_add(&counts.operands, 1);
        if (ok && predicate_indirect(o, term)) {
            SolMirRecipeId recipe = o->predicate_values[term->callee].recipe;
            if (recipe >= r->recipe_count) ok = false;
            else (void)recipe;
        }
    }
    for (size_t i = 0; ok && i < o->arithmetic_count; ++i) {
        ok = build_work_add(&work, 1);
        if (ok && (o->arithmetic[i].failures
                & (SOL_MIR_OPERATION_FAILURE_OVERFLOW
                    | SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO)) != 0)
            ok = count_add(&counts.failure_sites, 1);
    }
    for (size_t i = 0; ok && i < o->predicate_instruction_count; ++i) {
        ok = build_work_add(&work, 1);
        if (ok && (o->predicate_instructions[i].failures
                & (SOL_MIR_OPERATION_FAILURE_OVERFLOW
                    | SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO)) != 0)
            ok = count_add(&counts.failure_sites, 1);
    }
    for (size_t i = 0; ok && i < o->predicate_body_count; ++i)
        ok = build_work_add(&work, 1)
            && count_add(&counts.failure_sites, 1);
    for (size_t i = 0; ok && i < r->recipe_count; ++i) {
        ok = build_work_add(&work, 1);
        bool indirect = false;
        if (ok) ok = recipe_is_indirect(c, i, &indirect);
        if (ok && indirect)
            ok = count_add(&counts.signatures, 1)
                && count_add(&counts.slots, r->recipes[i].parameters.count);
    }
    for (size_t i = 0; ok && i < l->callable_count; ++i) {
        ok = build_work_add(&work, 1);
        const SolMirMaterializedImage *image = ok ? image_for_instance(m,
            l->callables[i].instance, &work) : NULL;
        if (image == NULL) ok = false;
        else ok = count_add(&counts.slots, image->parameter_types.count)
            && (image->receiver == SOL_MIR_RECIPE_NONE
                || count_add(&counts.slots, 1));
    }
    for (size_t i = 0; ok && i < l->host_requirement_count; ++i) {
        ok = build_work_add(&work, 1)
            && count_add(&counts.slots,
                l->host_requirements[i].parameters.count)
            && (l->host_requirements[i].receiver == SOL_MIR_RECIPE_NONE
                || count_add(&counts.slots, 1));
    }
    size_t arenas[] = {counts.signatures, counts.slots, counts.calls,
        counts.operands, counts.writebacks, counts.entries, counts.imports,
        counts.failure_sites};
    for (size_t i = 0; ok && i < sizeof(arenas) / sizeof(arenas[0]); ++i) {
        ok = preflight_step();
        if (ok && arenas[i] != 0) ok = build_work_add(&work, 1);
    }
    for (size_t i = 0; ok && i < l->callable_count; ++i) {
        ok = build_work_add(&work, 1);
        const SolMirMaterializedImage *image = ok ? image_for_instance(m,
            l->callables[i].instance, &work) : NULL;
        ok = image != NULL && signature_population_work(
            image->parameter_types.count,
            image->receiver != SOL_MIR_RECIPE_NONE, &work);
    }
    for (size_t i = 0; ok && i < l->host_requirement_count; ++i)
        ok = build_work_add(&work, 1)
            && signature_population_work(l->host_requirements[i].parameters.count,
                l->host_requirements[i].receiver != SOL_MIR_RECIPE_NONE, &work);
    for (size_t i = 0; ok && i < r->recipe_count; ++i) {
        ok = build_work_add(&work, 1);
        bool indirect = false;
        if (ok) ok = recipe_is_indirect(c, i, &indirect);
        if (ok && indirect)
            ok = signature_population_work(r->recipes[i].parameters.count,
                false, &work);
    }
    size_t direct_count = 0;
    ok = ok && count_add(&direct_count, l->callable_count)
        && count_add(&direct_count, l->host_requirement_count);
    for (size_t image = 0; ok && image < m->image_count; ++image) {
        ok = build_work_add(&work, 1);
        SolMirPlanSlice blocks = m->images[image].blocks;
        for (size_t q = 0; ok && q < blocks.count; ++q) {
            ok = build_work_add(&work, 1);
            const SolMirMaterializedTerminator *term
                = &m->blocks[blocks.offset + q].terminator;
            if (!ok || term->kind != SOL_MIR_TERM_INVOKE) continue;
            if (term->call_kind == SOL_IR_CALL_CALLBACK)
                ok = indirect_signature_work(c, direct_count,
                        m->temporaries[term->callee].type, &work)
                    && table_site_work(c, term->callable_site, &work);
            else if (ok) ok = build_work_add(&work, 2);
            if (ok && term->receiver.source_expression != SOL_IR_NONE)
                ok = build_work_add(&work, 1);
            for (size_t a = 0; ok && a < term->arguments.count; ++a)
                ok = build_work_add(&work, 2);
            for (size_t w = 0; ok && w < term->writebacks.count; ++w)
                ok = build_work_add(&work, 1);
            if (ok) ok = build_work_add(&work, 1);
        }
    }
    for (size_t body = 0; ok && body < o->predicate_body_count; ++body) {
        ok = build_work_add(&work, 1);
        SolMirPlanSlice blocks = o->predicate_bodies[body].blocks;
        for (size_t q = 0; ok && q < blocks.count; ++q) {
            ok = build_work_add(&work, 1);
            const SolMirPredicateTerminator *term
                = &o->predicate_blocks[blocks.offset + q].terminator;
            if (!ok || term->kind != SOL_MIR_PREDICATE_TERM_INVOKE) continue;
            if (predicate_indirect(o, term))
                ok = indirect_signature_work(c, direct_count,
                        o->predicate_values[term->callee].recipe,
                        &work)
                    && table_predicate_work(c, term->callee, &work);
            else ok = build_work_add(&work, 2);
            if (ok && !predicate_indirect(o, term) && (term->receiver != SOL_MIR_OPERATION_NONE
                    || term->call_kind == SOL_IR_CALL_CAPABILITY))
                ok = build_work_add(&work, 1);
            for (size_t a = 0; ok && a < term->arguments.count; ++a)
                ok = build_work_add(&work, 2);
            if (ok) ok = build_work_add(&work, 1);
        }
    }
    size_t site_record_work = 1 + ir->file_count;
    for (size_t i = 0; ok && i < counts.calls; ++i)
        ok = build_work_add(&work, 1)
            && build_work_add(&work, site_record_work);
    for (size_t i = 0; ok && i < o->arithmetic_count; ++i) {
        ok = build_work_add(&work, 1);
        if (ok && (o->arithmetic[i].failures
                & (SOL_MIR_OPERATION_FAILURE_OVERFLOW
                    | SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO)) != 0)
            ok = build_work_add(&work, site_record_work);
    }
    for (size_t image = 0; ok && image < m->image_count; ++image) {
        ok = build_work_add(&work, 1);
        SolMirPlanSlice blocks = m->images[image].blocks;
        for (size_t q = 0; ok && q < blocks.count; ++q) {
            ok = build_work_add(&work, 1);
            SolMirTerminatorKind kind
                = m->blocks[blocks.offset + q].terminator.kind;
            if (ok && (kind == SOL_MIR_TERM_PANIC
                    || kind == SOL_MIR_TERM_MATCH_FAILURE
                    || kind == SOL_MIR_TERM_UNREACHABLE))
                ok = build_work_add(&work, site_record_work);
        }
    }
    for (size_t body = 0; ok && body < o->predicate_body_count; ++body) {
        ok = build_work_add(&work, 1)
            && build_work_add(&work, ir->expression_count);
        SolMirPlanSlice blocks = o->predicate_bodies[body].blocks;
        for (size_t q = 0; ok && q < blocks.count; ++q) {
            const SolMirPredicateBlock *block
                = &o->predicate_blocks[blocks.offset + q];
            for (size_t x = 0; ok && x < block->instructions.count; ++x) {
                const SolMirPredicateInstruction *instruction
                    = &o->predicate_instructions[block->instructions.offset + x];
                if ((instruction->failures
                        & (SOL_MIR_OPERATION_FAILURE_OVERFLOW
                            | SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO)) != 0)
                    ok = build_work_add(&work, site_record_work);
            }
            if (ok && block->terminator.kind == SOL_MIR_PREDICATE_TERM_FAILURE
                && block->terminator.failure_kind
                    == SOL_MIR_PREDICATE_FAILURE_NO_MATCH)
                ok = build_work_add(&work, site_record_work);
        }
        if (ok) ok = build_work_add(&work, site_record_work);
    }
    for (size_t i = 0; ok && i < l->entry_export_count; ++i) {
        ok = build_work_add(&work, 1);
        const SolMirMaterializedImage *image = NULL;
        SolMirPlanInstanceId instance
            = l->callables[l->entry_exports[i].callable].instance;
        for (size_t q = 0; q < m->image_count; ++q) {
            if (!preflight_step()) { ok = false; break; }
            if (m->images[q].instance == instance) image = &m->images[q];
        }
        if (image == NULL) { ok = false; break; }
        size_t slots = image->parameter_types.count;
        if (image->receiver != SOL_MIR_RECIPE_NONE
            && !count_add(&slots, 1)) { ok = false; break; }
        for (size_t q = 0; ok && q < slots; ++q)
            ok = build_work_add(&work, 1);
        if (ok) ok = build_work_add(&work, 1);
    }
    if (counts.imports != shallow->imports) ok = false;
    if (ok) ok = build_work_add(&work, shallow->import_build_work);
    if (reconstruction_overflow) ok = false;
    if (ok) {
        if (expected_counts != NULL) *expected_counts = counts;
        *expected_work = work;
        *outcome = SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED;
    }
    if (!ok && reconstruction_overflow)
        *outcome = SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED;
    return ok;
}

static bool add_product(size_t *value, size_t left, size_t right) {
    size_t product;
    return mul_size(left, right, &product) && add_size(value, product);
}

static bool preflight_usage(const SolMirConcreteProgram *c,
    const ShallowPreflight *shallow,
    const SolMirRuntimeConventionsLimits *limits,
    SolMirRuntimeConventionsUsage *usage,
    SolMirRuntimeConventionsBuildOutcome *outcome) {
    *outcome = SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INTERNAL_FAILED;
    const ExpectedCounts counts = shallow->counts;
    SolMirRuntimeConventionsUsage result = {
        .signatures = counts.signatures, .signature_slots = counts.slots,
        .calls = counts.calls, .operands = counts.operands,
        .writebacks = counts.writebacks, .entries = counts.entries,
        .imports = counts.imports, .failure_sites = counts.failure_sites,
        .build_scratch_bytes = c->representation.recipe_count,
        .build_work = shallow->build_work,
    };
#define PREFLIGHT_BYTES(count, type) do { \
    size_t bytes; \
    if (!mul_size((count), sizeof(type), &bytes) \
        || !add_size(&result.owned_bytes, bytes)) { \
        *outcome = SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED; \
        return false; \
    } \
} while (0)
    PREFLIGHT_BYTES(counts.signatures, SolMirRuntimeSignature);
    PREFLIGHT_BYTES(counts.slots, SolMirRuntimeSignatureSlot);
    PREFLIGHT_BYTES(counts.calls, SolMirRuntimeCall);
    PREFLIGHT_BYTES(counts.operands, SolMirRuntimeOperand);
    PREFLIGHT_BYTES(counts.writebacks, SolMirRuntimeWriteback);
    PREFLIGHT_BYTES(counts.entries, SolMirRuntimeEntry);
    PREFLIGHT_BYTES(counts.imports, SolMirRuntimeImport);
    PREFLIGHT_BYTES(counts.failure_sites, SolMirRuntimeFailureSite);
#undef PREFLIGHT_BYTES
    result.validation_scratch_bytes = shallow->validation_scratch_bytes;
    result.validation_work = shallow->validation_work;
    if (result.signatures > limits->max_signatures
        || result.signature_slots > limits->max_signature_slots
        || result.calls > limits->max_calls
        || result.operands > limits->max_operands
        || result.writebacks > limits->max_writebacks
        || result.entries > limits->max_entries
        || result.imports > limits->max_imports
        || result.failure_sites > limits->max_failure_sites
        || result.owned_bytes > limits->max_owned_bytes
        || result.build_scratch_bytes > limits->max_build_scratch_bytes
        || result.build_work > limits->max_build_work
        || result.validation_scratch_bytes
            > limits->max_validation_scratch_bytes
        || result.validation_work > limits->max_validation_work) {
        *outcome = SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED;
        return false;
    }
    *usage = result;
    *outcome = SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED;
    return true;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
bool sol_mir_runtime_conventions_test_reconstruct_usage(
    const SolMirConcreteProgram *concrete,
    const SolMirRuntimeConventionsLimits *limits,
    SolMirRuntimeConventionsUsage *usage) {
    if (concrete == NULL || limits == NULL || usage == NULL) return false;
    metering_validation = false;
    metered_work = 0;
    metered_limit = SIZE_MAX;
    SolMirRuntimeConventionsBuildOutcome outcome;
    ShallowPreflight shallow;
    return shallow_preflight(concrete, limits, &shallow, &outcome)
        && preflight_usage(concrete, &shallow, limits,
            usage, &outcome);
}
#endif

SolMirRuntimeConventionsBuildOutcome sol_mir_runtime_conventions_internal_preflight(
    const SolMirConcreteProgram *concrete,
    const SolMirRuntimeConventionsLimits *limits,
    SolMirRuntimeConventionsUsage *usage,
    SolDiagnostics *diagnostics, bool meter_validation) {
    metering_validation = meter_validation;
    if (concrete == NULL || limits == NULL || usage == NULL)
        return SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INVALID_ARGUMENT;
    SolMirRuntimeConventionsBuildOutcome outcome;
    ShallowPreflight shallow;
    if (!shallow_preflight(concrete, limits, &shallow, &outcome)) return outcome;
    size_t concrete_work, concrete_scratch;
    if (!preflight_usage(concrete, &shallow, limits, usage, &outcome))
        return outcome;
    if (!tick(shallow.concrete_validation_work))
        return SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED;
    if (!sol_mir_concrete_internal_validate_measured(concrete, diagnostics,
            shallow.concrete_validation_work,
            shallow.validation_scratch_bytes, &concrete_work,
            &concrete_scratch))
        return diagnostics != NULL && diagnostics->allocation_failed
            ? SOL_MIR_RUNTIME_CONVENTIONS_BUILD_ALLOCATION_FAILED
            : SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INVALID_CONCRETE_PROGRAM;
    if (concrete_work != shallow.concrete_validation_work
        || concrete_scratch != shallow.validation_scratch_bytes)
        return SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED;
    return SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED;
}

static bool validate_usage(const SolMirRuntimeConventions *owner,
    const SolMirRuntimeConventionsUsage *expected,
    bool authenticate_resources) {
    const SolMirConcreteProgram *c = owner->concrete;
    SolMirRuntimeConventionsUsage u = {
        .signatures = owner->signature_count,
        .signature_slots = owner->signature_slot_count,
        .calls = owner->call_count,
        .operands = owner->operand_count,
        .writebacks = owner->writeback_count,
        .entries = owner->entry_count,
        .imports = owner->import_count,
        .failure_sites = owner->failure_site_count,
        .build_scratch_bytes = c->representation.recipe_count,
        .validation_scratch_bytes = expected->validation_scratch_bytes,
        .validation_work = expected->validation_work,
    };
#define BYTES(member, type, singular) do { \
    size_t bytes; \
    if (!tick(1) \
        || !mul_size(owner->singular##_count, sizeof(type), &bytes) \
        || !add_size(&u.owned_bytes, bytes)) return false; \
} while (0);
    SOL_MIR_RUNTIME_CONVENTIONS_ARENAS(BYTES)
#undef BYTES
    u.build_work = expected->build_work;
    return u.signatures == owner->usage.signatures
        && u.signature_slots == owner->usage.signature_slots
        && u.calls == owner->usage.calls
        && u.operands == owner->usage.operands
        && u.writebacks == owner->usage.writebacks
        && u.entries == owner->usage.entries
        && u.imports == owner->usage.imports
        && u.failure_sites == owner->usage.failure_sites
        && u.owned_bytes == owner->usage.owned_bytes
        && u.build_scratch_bytes == owner->usage.build_scratch_bytes
        && u.build_work == owner->usage.build_work
        && (!authenticate_resources
            || (owner->usage.validation_scratch_bytes
                    == expected->validation_scratch_bytes
                && owner->usage.validation_work == expected->validation_work))
        && u.owned_bytes <= owner->limits.max_owned_bytes
        && u.build_scratch_bytes <= owner->limits.max_build_scratch_bytes
        && u.build_work <= owner->limits.max_build_work
        && expected->validation_scratch_bytes
            <= owner->limits.max_validation_scratch_bytes;
}

static bool validate(const SolMirRuntimeConventions *owner,
    SolDiagnostics *diagnostics, bool authenticate_resources,
    size_t *measured_work, size_t *measured_scratch) {
    metered_work = 0;
    metered_limit = owner == NULL ? 0 : owner->limits.max_validation_work;
    metering_validation = true;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    observed_validation_work = 0;
#endif
    if (owner == NULL || owner->concrete == NULL)
        return invalid(diagnostics, "runtime conventions owner is empty");
    if (!validate_headers(owner))
        return invalid(diagnostics, "runtime conventions arena header is invalid");
    SolMirRuntimeConventionsUsage expected;
    SolMirRuntimeConventionsBuildOutcome preflight_outcome
        = sol_mir_runtime_conventions_internal_preflight(owner->concrete,
            &owner->limits, &expected, diagnostics, true);
    if (preflight_outcome != SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        || expected.validation_scratch_bytes
            > owner->limits.max_validation_scratch_bytes)
        return invalid(diagnostics,
            "runtime conventions prerequisite resource limit exceeded");
    if (!validate_aliases(owner) || !validate_text_aliases(owner))
        return invalid(diagnostics, "runtime conventions owner aliases borrowed data");
    if (!validate_slices(owner))
        return invalid(diagnostics, "runtime conventions slices are invalid");
    if (!validate_signatures(owner))
        return invalid(diagnostics, "runtime signatures are not reconstructive");
    if (!validate_calls(owner))
        return invalid(diagnostics, "runtime calls are not reconstructive");
    if (!validate_failure_sites(owner))
        return invalid(diagnostics, "runtime failure sites are not reconstructive");
    if (!validate_entries(owner))
        return invalid(diagnostics, "runtime entries are not reconstructive");
    if (!validate_imports(owner))
        return invalid(diagnostics, "runtime imports are not reconstructive");
    if (!validate_usage(owner, &expected, authenticate_resources))
        return invalid(diagnostics, "runtime resource usage is not exact");
    if (owner->usage.validation_work != expected.validation_work
        || metered_work != expected.validation_work)
        return invalid(diagnostics, "runtime validation work is not exact");
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    observed_validation_work = metered_work;
#endif
    if (measured_work != NULL) *measured_work = metered_work;
    if (measured_scratch != NULL)
        *measured_scratch = expected.validation_scratch_bytes;
    return true;
}

bool sol_mir_runtime_conventions_internal_validation_requirements(
    const SolMirRuntimeConventions *owner, size_t *work, size_t *scratch,
    SolDiagnostics *diagnostics) {
    return work != NULL && scratch != NULL
        && validate(owner, diagnostics, false, work, scratch);
}

bool sol_mir_runtime_conventions_validate(
    const SolMirRuntimeConventions *owner, SolDiagnostics *diagnostics) {
    return validate(owner, diagnostics, true, NULL, NULL);
}
