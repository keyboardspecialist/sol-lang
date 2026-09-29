#ifndef SOL_MIR_RUNTIME_ARENA_INTERNAL_H
#define SOL_MIR_RUNTIME_ARENA_INTERNAL_H

/* Private, raw arena census shared by the P3 runtime-owner validators.  The
 * visitor must authenticate each descriptor before using it. */
#include "sol/mir_runtime_conventions.h"

typedef bool (*SolMirRuntimeArenaVisitor)(const void *, size_t, size_t, void *);

typedef enum {
    SOL_MIR_RUNTIME_ARENA_VISIT_OK,
    SOL_MIR_RUNTIME_ARENA_VISIT_STOPPED,
    SOL_MIR_RUNTIME_ARENA_VISIT_MALFORMED,
    SOL_MIR_RUNTIME_ARENA_VISIT_EXHAUSTED,
} SolMirRuntimeArenaVisit;
typedef enum { SOL_MIR_RUNTIME_TEXT_SAFE, SOL_MIR_RUNTIME_TEXT_OVERLAP,
    SOL_MIR_RUNTIME_TEXT_MALFORMED, SOL_MIR_RUNTIME_TEXT_EXHAUSTED } SolMirRuntimeTextGuardResult;
typedef SolMirRuntimeTextGuardResult (*SolMirRuntimeTextGuard)(uintptr_t,
    uintptr_t *, void *);
static _Thread_local SolMirRuntimeTextGuard sol_mir_runtime_text_guard;
static _Thread_local void *sol_mir_runtime_text_guard_context;

static inline bool sol_mir_runtime_arena_range(const void *pointer, size_t count,
    size_t size) {
    size_t bytes;
    return count == 0 ? pointer == NULL
        : pointer != NULL && (size == 0 || count <= SIZE_MAX / size)
            && (bytes = count * size, (uintptr_t)pointer <= UINTPTR_MAX - bytes);
}

static inline SolMirRuntimeArenaVisit sol_mir_runtime_visit_checked_range(
    const void *pointer, size_t count, size_t size, SolMirRuntimeArenaVisitor visit,
    void *context) {
    if (!sol_mir_runtime_arena_range(pointer, count, size))
        return SOL_MIR_RUNTIME_ARENA_VISIT_MALFORMED;
    return visit(pointer, count, size, context)
        ? SOL_MIR_RUNTIME_ARENA_VISIT_OK : SOL_MIR_RUNTIME_ARENA_VISIT_STOPPED;
}

static inline SolMirRuntimeArenaVisit sol_mir_runtime_visit_text(
    const char *text, SolMirRuntimeArenaVisitor visit, void *context) {
    if (text == NULL) return sol_mir_runtime_visit_checked_range(NULL, 0,
        sizeof(char), visit, context);
    uintptr_t address=(uintptr_t)text;
    for (;;) {
        uintptr_t boundary=0;
        if (sol_mir_runtime_text_guard == NULL) {
            SolMirRuntimeArenaVisit one=sol_mir_runtime_visit_checked_range(
                (const void *)(uintptr_t)address,1,sizeof(char),visit,context);
            if (one!=SOL_MIR_RUNTIME_ARENA_VISIT_OK) return one;
            if (*(const char *)(uintptr_t)address=='\0') return SOL_MIR_RUNTIME_ARENA_VISIT_OK;
            if (address==UINTPTR_MAX) return SOL_MIR_RUNTIME_ARENA_VISIT_MALFORMED;
            ++address; continue;
        }
        SolMirRuntimeTextGuardResult guarded=sol_mir_runtime_text_guard(address,&boundary,
            sol_mir_runtime_text_guard_context);
        if (guarded==SOL_MIR_RUNTIME_TEXT_EXHAUSTED) return SOL_MIR_RUNTIME_ARENA_VISIT_EXHAUSTED;
        if (guarded==SOL_MIR_RUNTIME_TEXT_MALFORMED) return SOL_MIR_RUNTIME_ARENA_VISIT_MALFORMED;
        if (guarded!=SOL_MIR_RUNTIME_TEXT_SAFE) return SOL_MIR_RUNTIME_ARENA_VISIT_STOPPED;
        if (boundary<=address) return SOL_MIR_RUNTIME_ARENA_VISIT_MALFORMED;
        while (address<boundary) {
            /* NULL/zero is the byte-tick callback; it has no descriptor range. */
            if (!visit(NULL,0,0,context)) return SOL_MIR_RUNTIME_ARENA_VISIT_STOPPED;
            char value=*(const char *)(uintptr_t)address;
            if (value=='\0') return SOL_MIR_RUNTIME_ARENA_VISIT_OK;
            if (address==UINTPTR_MAX) return SOL_MIR_RUNTIME_ARENA_VISIT_MALFORMED;
            ++address;
        }
    }
}

static inline SolMirRuntimeArenaVisit sol_mir_runtime_visit_mir_arenas(const SolMir *mir,
    SolMirRuntimeArenaVisitor visit, void *context) {
#define ARENA(member, singular, type) \
    if (mir->singular##_count > mir->singular##_capacity) return SOL_MIR_RUNTIME_ARENA_VISIT_MALFORMED; \
    { SolMirRuntimeArenaVisit result=sol_mir_runtime_visit_checked_range(mir->member,mir->singular##_capacity,sizeof(type),visit,context); if(result!=SOL_MIR_RUNTIME_ARENA_VISIT_OK)return result; }
    { SolMirRuntimeArenaVisit result=sol_mir_runtime_visit_checked_range(mir,1,sizeof(*mir),visit,context); if(result!=SOL_MIR_RUNTIME_ARENA_VISIT_OK)return result; }
    ARENA(blocks, block, SolMirBlock); ARENA(instructions, instruction, SolMirInstruction);
    ARENA(values, value, SolMirValue); ARENA(parameter_values, parameter_value, SolMirValueId);
    ARENA(edge_values, edge_value, SolMirValueId); ARENA(call_arguments, call_argument, SolMirCallArgument);
    ARENA(loops, loop, SolMirLoop); ARENA(construct_operands, construct_operand, SolMirConstructOperand);
    ARENA(temporaries, temporary, SolMirTemporary);
#undef ARENA
    return SOL_MIR_RUNTIME_ARENA_VISIT_OK;
}

static inline SolMirRuntimeArenaVisit sol_mir_runtime_visit_concrete_arenas(
    const SolMirConcreteProgram *concrete, SolMirRuntimeArenaVisitor visit,
    void *context) {
    if (concrete==NULL) return SOL_MIR_RUNTIME_ARENA_VISIT_MALFORMED;
    const SolMirProgram *program = &concrete->program;
    const SolMirPlan *plan = &concrete->plan;
    const SolMirMaterialization *materialization = &concrete->materialization;
    const SolMirRepresentation *representation = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    const SolMirOperations *operations = &concrete->operations;
    const SolMirLinkage *linkage = &concrete->linkage;
#define COUNT_ARENA(owner, member, count, type) do { SolMirRuntimeArenaVisit result=sol_mir_runtime_visit_checked_range((owner)->member,(owner)->count,sizeof(type),visit,context);if(result!=SOL_MIR_RUNTIME_ARENA_VISIT_OK)return result; } while(0)
#define CAPACITY_ARENA(owner, member, singular, type) do { if((owner)->singular##_count>(owner)->singular##_capacity)return SOL_MIR_RUNTIME_ARENA_VISIT_MALFORMED; SolMirRuntimeArenaVisit result=sol_mir_runtime_visit_checked_range((owner)->member,(owner)->singular##_capacity,sizeof(type),visit,context);if(result!=SOL_MIR_RUNTIME_ARENA_VISIT_OK)return result; } while(0)
    if (sol_mir_runtime_visit_checked_range(concrete,1,sizeof(*concrete),visit,context)!=SOL_MIR_RUNTIME_ARENA_VISIT_OK
        || sol_mir_runtime_visit_checked_range(program,1,sizeof(*program),visit,context)!=SOL_MIR_RUNTIME_ARENA_VISIT_OK
        || sol_mir_runtime_visit_checked_range(plan,1,sizeof(*plan),visit,context)!=SOL_MIR_RUNTIME_ARENA_VISIT_OK
        || sol_mir_runtime_visit_checked_range(materialization,1,sizeof(*materialization),visit,context)!=SOL_MIR_RUNTIME_ARENA_VISIT_OK
        || sol_mir_runtime_visit_checked_range(representation,1,sizeof(*representation),visit,context)!=SOL_MIR_RUNTIME_ARENA_VISIT_OK
        || sol_mir_runtime_visit_checked_range(layout,1,sizeof(*layout),visit,context)!=SOL_MIR_RUNTIME_ARENA_VISIT_OK
        || sol_mir_runtime_visit_checked_range(operations,1,sizeof(*operations),visit,context)!=SOL_MIR_RUNTIME_ARENA_VISIT_OK
        || sol_mir_runtime_visit_checked_range(linkage,1,sizeof(*linkage),visit,context)!=SOL_MIR_RUNTIME_ARENA_VISIT_OK) return SOL_MIR_RUNTIME_ARENA_VISIT_STOPPED;
    COUNT_ARENA(program, roots, root_count, SolMirProgramRoot);
    COUNT_ARENA(program, approved_imports, approved_import_count, SolIrCallableId);
    COUNT_ARENA(program, templates, template_count, SolMirProgramTemplate);
    COUNT_ARENA(program, imports, import_count, SolMirProgramImport);
    COUNT_ARENA(program, specializations, specialization_count, SolMirProgramSpecialization);
    COUNT_ARENA(program, references, reference_count, SolMirProgramReference);
    for (size_t i = 0; i < program->template_count; ++i) { SolMirRuntimeArenaVisit result=sol_mir_runtime_visit_mir_arenas(&program->templates[i].mir,visit,context);if(result!=SOL_MIR_RUNTIME_ARENA_VISIT_OK)return result; }
    CAPACITY_ARENA(plan, types, type, SolMirPlanType); CAPACITY_ARENA(plan, type_components, type_component, SolMirPlanTypeId);
    CAPACITY_ARENA(plan, type_parameter_accesses, type_parameter_access, SolAccessMode); CAPACITY_ARENA(plan, effect_atoms, effect_atom, SolMirPlanEffectAtom);
    CAPACITY_ARENA(plan, effect_rows, effect_row, SolMirPlanEffectRow); CAPACITY_ARENA(plan, effect_row_atoms, effect_row_atom, size_t);
    CAPACITY_ARENA(plan, instances, instance, SolMirPlanInstance); CAPACITY_ARENA(plan, instance_type_ids, instance_type_id, SolMirPlanTypeId);
    CAPACITY_ARENA(plan, instance_accesses, instance_access, SolAccessMode); CAPACITY_ARENA(plan, dictionary_entries, dictionary_entry, SolMirPlanDictionaryEntry);
    CAPACITY_ARENA(plan, imports, import, SolMirPlanImport); CAPACITY_ARENA(plan, typed_uses, typed_use, SolMirPlanTypedUse);
    CAPACITY_ARENA(plan, contexts, context, SolMirPlanContext); CAPACITY_ARENA(plan, demands, demand, SolMirPlanDemand);
    CAPACITY_ARENA(materialization, images, image, SolMirMaterializedImage); CAPACITY_ARENA(materialization, types, type, SolMirMaterializedType);
    CAPACITY_ARENA(materialization, shape_fields, shape_field, SolMirMaterializedShapeField); CAPACITY_ARENA(materialization, shape_variants, shape_variant, SolMirMaterializedShapeVariant);
    CAPACITY_ARENA(materialization, type_ids, type_id, SolMirMaterializedTypeId); CAPACITY_ARENA(materialization, accesses, access, SolAccessMode);
    CAPACITY_ARENA(materialization, overlays, overlay, SolMirMaterializedTypeOverlay); CAPACITY_ARENA(materialization, contexts, context, SolMirPlanContext);
    CAPACITY_ARENA(materialization, locals, local, SolMirMaterializedLocal); CAPACITY_ARENA(materialization, places, place, SolMirMaterializedPlace);
    CAPACITY_ARENA(materialization, projections, projection, SolMirMaterializedProjection); CAPACITY_ARENA(materialization, values, value, SolMirMaterializedValue);
    CAPACITY_ARENA(materialization, instructions, instruction, SolMirMaterializedInstruction); CAPACITY_ARENA(materialization, temporaries, temporary, SolMirMaterializedTemporary);
    CAPACITY_ARENA(materialization, construct_operands, construct_operand, SolMirMaterializedConstructOperand); CAPACITY_ARENA(materialization, call_arguments, call_argument, SolMirMaterializedCallArgument);
    CAPACITY_ARENA(materialization, blocks, block, SolMirMaterializedBlock); CAPACITY_ARENA(materialization, edges, edge, SolMirMaterializedEdge);
    CAPACITY_ARENA(materialization, edge_values, edge_value, SolMirMaterializedValueId); CAPACITY_ARENA(materialization, parameter_values, parameter_value, SolMirMaterializedValueId);
    CAPACITY_ARENA(materialization, loops, loop, SolMirMaterializedLoop); CAPACITY_ARENA(materialization, bindings, binding, SolMirMaterializedBinding);
    CAPACITY_ARENA(materialization, semantic_sites, semantic_site, SolMirMaterializedSemanticSite); CAPACITY_ARENA(materialization, receiver_roots, receiver_root, SolMirMaterializedLocalId);
    CAPACITY_ARENA(materialization, imports, import, SolMirMaterializedImport); CAPACITY_ARENA(materialization, handlers, handler, SolMirMaterializedHandler);
    CAPACITY_ARENA(materialization, writebacks, writeback, SolMirMaterializedWriteback); CAPACITY_ARENA(materialization, effect_rows, effect_row, SolMirMaterializedEffectRow);
    CAPACITY_ARENA(materialization, effect_atoms, effect_atom, SolMirMaterializedEffectAtom); CAPACITY_ARENA(materialization, effect_row_atoms, effect_row_atom, size_t);
    CAPACITY_ARENA(materialization, effect_names, effect_name, char); CAPACITY_ARENA(materialization, literal_bytes, literal_byte, char);
    for (size_t i = 0; i < materialization->image_count; ++i) { SolMirRuntimeArenaVisit result=sol_mir_runtime_visit_mir_arenas(&materialization->images[i].topology,visit,context);if(result!=SOL_MIR_RUNTIME_ARENA_VISIT_OK)return result; }
    CAPACITY_ARENA(representation, recipes, recipe, SolMirRecipe); CAPACITY_ARENA(representation, fields, field, SolMirRecipeField);
    CAPACITY_ARENA(representation, variants, variant, SolMirRecipeVariant); CAPACITY_ARENA(representation, recipe_ids, recipe_id, SolMirRecipeId);
    CAPACITY_ARENA(representation, accesses, access, SolAccessMode); CAPACITY_ARENA(representation, receiver_roots, receiver_root, SolMirMaterializedLocalId);
    CAPACITY_ARENA(representation, callable_producers, callable_producer, SolMirCallableProducer);
    CAPACITY_ARENA(layout, types, type, SolMirTypeLayout); CAPACITY_ARENA(layout, fields, field, SolMirFieldLayout);
    CAPACITY_ARENA(layout, variants, variant, SolMirVariantLayout); CAPACITY_ARENA(layout, projections, projection, SolMirProjectionMap);
#define OPERATION(member, type, singular) CAPACITY_ARENA(operations, member, singular, type);
    SOL_MIR_OPERATIONS_ARENAS(OPERATION)
#undef OPERATION
#define LINKAGE(member, type, singular) CAPACITY_ARENA(linkage, member, singular, type);
    SOL_MIR_LINKAGE_ARENAS(LINKAGE)
#undef LINKAGE
    const SolIr *ir = program->ir;
    if (ir == NULL) return SOL_MIR_RUNTIME_ARENA_VISIT_MALFORMED;
    { SolMirRuntimeArenaVisit result=sol_mir_runtime_visit_checked_range(ir,1,sizeof(*ir),visit,context);if(result!=SOL_MIR_RUNTIME_ARENA_VISIT_OK)return result; }
    COUNT_ARENA(ir, definitions, definition_count, SolIrDefinition); COUNT_ARENA(ir, callables, callable_count, SolIrCallable);
    COUNT_ARENA(ir, types, type_count, SolIrType); COUNT_ARENA(ir, type_ids, type_id_count, SolIrTypeId); COUNT_ARENA(ir, accesses, access_count, SolAccessMode);
    COUNT_ARENA(ir, members, member_count, SolIrMember); COUNT_ARENA(ir, evidence, evidence_count, SolIrDispatchEvidence); COUNT_ARENA(ir, locals, local_count, SolIrLocal);
    COUNT_ARENA(ir, fields, field_count, SolIrField); COUNT_ARENA(ir, variants, variant_count, SolIrVariant); COUNT_ARENA(ir, expressions, expression_count, SolIrExpression);
    COUNT_ARENA(ir, places, place_count, SolIrPlace); COUNT_ARENA(ir, projections, projection_count, SolIrProjection); COUNT_ARENA(ir, statements, statement_count, SolIrStatement);
    COUNT_ARENA(ir, statement_ids, statement_id_count, SolIrStatementId); COUNT_ARENA(ir, arms, arm_count, SolIrArm); COUNT_ARENA(ir, arm_ids, arm_id_count, SolIrArmId);
    COUNT_ARENA(ir, patterns, pattern_count, SolIrPattern); COUNT_ARENA(ir, pattern_children, pattern_child_count, SolIrPatternChild); COUNT_ARENA(ir, operands, operand_count, SolIrOperand);
    COUNT_ARENA(ir, roots, root_count, SolIrLocalId); COUNT_ARENA(ir, obligations, obligation_count, SolIrObligation); COUNT_ARENA(ir, snapshots, snapshot_count, SolIrSnapshot);
    COUNT_ARENA(ir, cleanup_locals, cleanup_local_count, SolIrLocalId); COUNT_ARENA(ir, effects, effect_count, SolIrEffect);
    COUNT_ARENA(ir, generic_parameters, generic_parameter_count, SolIrGenericParameter); COUNT_ARENA(ir, effect_parameters, effect_parameter_count, SolIrEffectParameter);
    COUNT_ARENA(ir, loop_obligations, loop_obligation_count, SolObligationId); COUNT_ARENA(ir, unreachable_obligations, unreachable_obligation_count, SolObligationId);
    COUNT_ARENA(ir, files, file_count, SolIrSourceFile);
    if (ir->source_length == SIZE_MAX) return SOL_MIR_RUNTIME_ARENA_VISIT_MALFORMED;
    { SolMirRuntimeArenaVisit result=sol_mir_runtime_visit_text(ir->source_path,visit,context);if(result!=SOL_MIR_RUNTIME_ARENA_VISIT_OK)return result; result=sol_mir_runtime_visit_checked_range(ir->source_bytes,ir->source_length+1,sizeof(*ir->source_bytes),visit,context);if(result!=SOL_MIR_RUNTIME_ARENA_VISIT_OK)return result; }
    for (size_t i = 0; i < plan->effect_atom_count; ++i)
        { SolMirRuntimeArenaVisit result=sol_mir_runtime_visit_text(plan->effect_atoms[i].name,visit,context);if(result!=SOL_MIR_RUNTIME_ARENA_VISIT_OK)return result; }
#define OPTIONAL_TEXT(array, count, member) for (size_t i = 0; i < (count); ++i) { SolMirRuntimeArenaVisit result=sol_mir_runtime_visit_text((array)[i].member,visit,context);if(result!=SOL_MIR_RUNTIME_ARENA_VISIT_OK)return result; }
    OPTIONAL_TEXT(ir->definitions, ir->definition_count, name); OPTIONAL_TEXT(ir->callables, ir->callable_count, name);
    OPTIONAL_TEXT(ir->locals, ir->local_count, name); OPTIONAL_TEXT(ir->fields, ir->field_count, name);
    OPTIONAL_TEXT(ir->variants, ir->variant_count, name); OPTIONAL_TEXT(ir->statements, ir->statement_count, region_label);
    OPTIONAL_TEXT(ir->effects, ir->effect_count, name); OPTIONAL_TEXT(ir->generic_parameters, ir->generic_parameter_count, name);
    OPTIONAL_TEXT(ir->effect_parameters, ir->effect_parameter_count, name); OPTIONAL_TEXT(ir->files, ir->file_count, path);
#undef OPTIONAL_TEXT
    for (size_t i = 0; i < ir->expression_count; ++i) {
        const char *text = ir->expressions[i].kind == SOL_IR_EXPR_STRING ? ir->expressions[i].as.string
            : ir->expressions[i].kind == SOL_IR_EXPR_HANDLE ? ir->expressions[i].as.handler.effect_name : NULL;
        { SolMirRuntimeArenaVisit result=sol_mir_runtime_visit_text(text,visit,context);if(result!=SOL_MIR_RUNTIME_ARENA_VISIT_OK)return result; }
    }
#undef CAPACITY_ARENA
#undef COUNT_ARENA
    return SOL_MIR_RUNTIME_ARENA_VISIT_OK;
}

#endif
