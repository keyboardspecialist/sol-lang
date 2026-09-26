#include "sol/mir_concrete.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *data; size_t length, capacity; bool failed; } Buffer;

static void report(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) sol_diagnostics_add(diagnostics,
        "SOL-MIR-CONCRETE-001", SOL_SEVERITY_ERROR, (SolSpan){0}, message);
}

void sol_mir_concrete_program_init(SolMirConcreteProgram *program) {
    if (program != NULL) memset(program, 0, sizeof(*program));
}

void sol_mir_concrete_program_free(SolMirConcreteProgram *program) {
    if (program == NULL) return;
    sol_mir_linkage_free(&program->linkage);
    sol_mir_operations_free(&program->operations);
    sol_mir_layout_free(&program->layout);
    sol_mir_representation_free(&program->representation);
    sol_mir_materialization_free(&program->materialization);
    sol_mir_plan_free(&program->plan);
    sol_mir_program_free(&program->program);
    sol_mir_concrete_program_init(program);
}

SolMirConcreteLimits sol_mir_concrete_default_limits(void) {
    return (SolMirConcreteLimits){
        .program = sol_mir_program_default_limits(),
        .plan = sol_mir_plan_default_limits(),
        .materialization = sol_mir_materialize_default_limits(),
        .representation = sol_mir_representation_default_limits(),
        .layout = sol_mir_layout_default_limits(),
        .operations = sol_mir_operations_default_limits(),
        .linkage = sol_mir_linkage_default_limits(),
    };
}

static bool limits_zero(const SolMirConcreteLimits *limits) {
#define P(name) limits->program.name == 0
#define N(name) limits->plan.name == 0
#define M(name) limits->materialization.name == 0
#define R(name) limits->representation.name == 0
#define L(name) limits->layout.name == 0
#define O(name) limits->operations.name == 0
#define K(name) limits->linkage.name == 0
    bool zero = P(max_callable_classifications) && P(max_references)
        && P(max_discovery_work) && N(max_instances) && N(max_concrete_types)
        && N(max_demands) && N(max_typed_uses) && N(max_contexts)
        && N(max_planning_work) && N(max_substitution_depth)
        && M(max_instances) && M(max_cfg_items) && M(max_bindings)
        && M(max_concrete_records) && M(max_owned_bytes)
        && M(max_materialization_work) && M(max_shape_resolution_work)
        && M(max_validation_work) && R(max_recipes) && R(max_fields)
        && R(max_variants) && R(max_recipe_ids) && R(max_callable_producers)
        && R(max_receiver_roots) && R(max_owned_bytes)
        && R(max_build_scratch_bytes) && R(max_build_work)
        && R(max_validation_work) && R(max_validation_scratch_bytes)
        && L(max_type_layouts) && L(max_field_layouts)
        && L(max_variant_layouts) && L(max_projection_maps)
        && L(max_owned_bytes) && L(max_build_scratch_bytes)
        && L(max_build_work) && L(max_validation_scratch_bytes)
        && L(max_validation_work) && O(max_access_plans)
        && O(max_access_steps) && O(max_constructors)
        && O(max_construct_operands) && O(max_pattern_tests)
        && O(max_pattern_extractions) && O(max_pattern_nodes)
        && O(max_path_steps) && O(max_propagations) && O(max_arithmetic)
        && O(max_equality_nodes) && O(max_equality_children)
        && O(max_snapshots) && O(max_callables) && O(max_handlers)
        && O(max_predicates) && O(max_recipe_ids) && O(max_roots)
        && O(max_provenance) && O(max_predicate_bodies)
        && O(max_predicate_blocks) && O(max_predicate_inputs)
        && O(max_predicate_values) && O(max_predicate_instructions)
        && O(max_predicate_edges) && O(max_predicate_edge_values)
        && O(max_predicate_operands) && O(max_predicate_path_steps)
        && O(max_predicate_pattern_nodes) && O(max_import_envelopes)
        && O(max_import_contract_references) && O(max_import_snapshots)
        && O(max_literal_bytes) && O(max_owned_bytes)
        && O(max_build_scratch_bytes) && O(max_build_work)
        && O(max_validation_scratch_bytes) && O(max_validation_work)
        && K(max_callables) && K(max_bindings) && K(max_entry_exports)
        && K(max_table_entries) && K(max_callable_values)
        && K(max_host_requirements) && K(max_runtime_requirements)
        && K(max_owned_bytes) && K(max_build_scratch_bytes)
        && K(max_build_work) && K(max_validation_scratch_bytes)
        && K(max_validation_work);
#undef P
#undef N
#undef M
#undef R
#undef L
#undef O
#undef K
    return zero;
}

static bool limits_complete(const SolMirConcreteLimits *v) {
#define P(name) v->program.name != 0
#define N(name) v->plan.name != 0
#define M(name) v->materialization.name != 0
#define R(name) v->representation.name != 0
#define L(name) v->layout.name != 0
#define O(name) v->operations.name != 0
#define K(name) v->linkage.name != 0
    bool complete = P(max_callable_classifications) && P(max_references)
        && P(max_discovery_work) && N(max_instances) && N(max_concrete_types)
        && N(max_demands) && N(max_typed_uses) && N(max_contexts)
        && N(max_planning_work) && N(max_substitution_depth)
        && M(max_instances) && M(max_cfg_items) && M(max_bindings)
        && M(max_concrete_records) && M(max_owned_bytes)
        && M(max_materialization_work) && M(max_shape_resolution_work)
        && M(max_validation_work) && R(max_recipes) && R(max_fields)
        && R(max_variants) && R(max_recipe_ids) && R(max_callable_producers)
        && R(max_receiver_roots) && R(max_owned_bytes)
        && R(max_build_scratch_bytes) && R(max_build_work)
        && R(max_validation_work) && R(max_validation_scratch_bytes)
        && L(max_type_layouts) && L(max_field_layouts)
        && L(max_variant_layouts) && L(max_projection_maps)
        && L(max_owned_bytes) && L(max_build_scratch_bytes)
        && L(max_build_work) && L(max_validation_scratch_bytes)
        && L(max_validation_work) && O(max_access_plans)
        && O(max_access_steps) && O(max_constructors)
        && O(max_construct_operands) && O(max_pattern_tests)
        && O(max_pattern_extractions) && O(max_pattern_nodes)
        && O(max_path_steps) && O(max_propagations) && O(max_arithmetic)
        && O(max_equality_nodes) && O(max_equality_children)
        && O(max_snapshots) && O(max_callables) && O(max_handlers)
        && O(max_predicates) && O(max_recipe_ids) && O(max_roots)
        && O(max_provenance) && O(max_predicate_bodies)
        && O(max_predicate_blocks) && O(max_predicate_inputs)
        && O(max_predicate_values) && O(max_predicate_instructions)
        && O(max_predicate_edges) && O(max_predicate_edge_values)
        && O(max_predicate_operands) && O(max_predicate_path_steps)
        && O(max_predicate_pattern_nodes) && O(max_import_envelopes)
        && O(max_import_contract_references) && O(max_import_snapshots)
        && O(max_literal_bytes) && O(max_owned_bytes)
        && O(max_build_scratch_bytes) && O(max_build_work)
        && O(max_validation_scratch_bytes) && O(max_validation_work)
        && K(max_callables) && K(max_bindings) && K(max_entry_exports)
        && K(max_table_entries) && K(max_callable_values)
        && K(max_host_requirements) && K(max_runtime_requirements)
        && K(max_owned_bytes) && K(max_build_scratch_bytes)
        && K(max_build_work) && K(max_validation_scratch_bytes)
        && K(max_validation_work);
#undef P
#undef N
#undef M
#undef R
#undef L
#undef O
#undef K
    return complete;
}

static bool program_empty(const SolMirConcreteProgram *program) {
    if (program == NULL) return false;
    SolMirConcreteProgram empty;
    sol_mir_concrete_program_init(&empty);
    return memcmp(program, &empty, sizeof(empty)) == 0;
}

static SolMirConcreteBuildOutcome allocation_or_internal(
    const SolDiagnostics *diagnostics) {
    return diagnostics != NULL && diagnostics->allocation_failed
        ? SOL_MIR_CONCRETE_BUILD_ALLOCATION_FAILED
        : SOL_MIR_CONCRETE_BUILD_INTERNAL_FAILED;
}

static void rebind(SolMirConcreteProgram *program) {
    program->plan.program = &program->program;
    program->materialization.plan = &program->plan;
    program->representation.materialization = &program->materialization;
    program->layout.representation = &program->representation;
    program->operations.layout = &program->layout;
    program->linkage.operations = &program->operations;
}

SolMirConcreteBuildOutcome sol_mir_concrete_program_build(
    const SolMirConcreteBuildRequest *request, SolMirConcreteProgram *program,
    SolDiagnostics *diagnostics) {
    if (request == NULL || diagnostics == NULL || request->ir == NULL
        || request->target == NULL
        || !program_empty(program)
        || (request->root_count != 0 && request->roots == NULL)
        || (request->approved_import_count != 0
            && request->approved_imports == NULL)
        || (request->limits != NULL && !limits_zero(request->limits)
            && !limits_complete(request->limits))) {
        report(diagnostics, "invalid concrete program build request or destination");
        return SOL_MIR_CONCRETE_BUILD_INVALID_ARGUMENT;
    }
    if (!sol_mir_target_descriptor_validate(request->target)) {
        report(diagnostics, "invalid concrete program target descriptor");
        return SOL_MIR_CONCRETE_BUILD_INVALID_TARGET;
    }
    SolMirConcreteLimits limits = request->limits == NULL
            || limits_zero(request->limits)
        ? sol_mir_concrete_default_limits() : *request->limits;
    SolMirConcreteProgram scratch;
    sol_mir_concrete_program_init(&scratch);
    SolMirProgramBuildRequest a = {request->ir, request->roots,
        request->root_count, request->approved_imports,
        request->approved_import_count, &limits.program};
    SolMirProgramBuildOutcome ao = sol_mir_program_build(&a, &scratch.program,
        diagnostics);
    if (ao != SOL_MIR_PROGRAM_BUILD_SUCCEEDED) {
        SolMirConcreteBuildOutcome out = ao == SOL_MIR_PROGRAM_BUILD_INVALID_IR
            ? SOL_MIR_CONCRETE_BUILD_INVALID_IR
            : ao == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE
                ? SOL_MIR_CONCRETE_BUILD_UNSUPPORTED_CLOSURE
            : ao == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED
                ? SOL_MIR_CONCRETE_BUILD_RESOURCE_EXHAUSTED
            : ao == SOL_MIR_PROGRAM_BUILD_ALLOCATION_FAILED
                ? SOL_MIR_CONCRETE_BUILD_ALLOCATION_FAILED
            : ao == SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT
                ? SOL_MIR_CONCRETE_BUILD_INVALID_ARGUMENT
                : SOL_MIR_CONCRETE_BUILD_INTERNAL_FAILED;
        report(diagnostics, "concrete program symbolic program build failed");
        sol_mir_concrete_program_free(&scratch); return out;
    }
    SolMirPlanBuildRequest b = {&scratch.program, &limits.plan};
    SolMirPlanBuildOutcome bo = sol_mir_plan_build(&b, &scratch.plan, diagnostics);
    if (bo != SOL_MIR_PLAN_BUILD_SUCCEEDED) {
        SolMirConcreteBuildOutcome out
            = bo == SOL_MIR_PLAN_BUILD_EXPANDING_RECURSION
                ? SOL_MIR_CONCRETE_BUILD_EXPANDING_RECURSION
            : bo == SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED
                ? SOL_MIR_CONCRETE_BUILD_UNSUPPORTED_CLOSURE
            : bo == SOL_MIR_PLAN_BUILD_RESOURCE_EXHAUSTED
                ? SOL_MIR_CONCRETE_BUILD_RESOURCE_EXHAUSTED
            : bo == SOL_MIR_PLAN_BUILD_ALLOCATION_FAILED
                ? SOL_MIR_CONCRETE_BUILD_ALLOCATION_FAILED
            : bo == SOL_MIR_PLAN_BUILD_INVALID_ARGUMENT
                ? SOL_MIR_CONCRETE_BUILD_INVALID_ARGUMENT
                : SOL_MIR_CONCRETE_BUILD_INTERNAL_FAILED;
        report(diagnostics, "concrete program planning failed");
        sol_mir_concrete_program_free(&scratch); return out;
    }
    SolMirMaterializeBuildRequest c = {&scratch.plan, &limits.materialization};
    SolMirMaterializeBuildOutcome co = sol_mir_materialize_build(&c,
        &scratch.materialization, diagnostics);
    if (co != SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED) {
        SolMirConcreteBuildOutcome out
            = co == SOL_MIR_MATERIALIZE_BUILD_UNSUPPORTED_OR_UNRESOLVED
                ? SOL_MIR_CONCRETE_BUILD_UNSUPPORTED_CLOSURE
            : co == SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED
                ? SOL_MIR_CONCRETE_BUILD_RESOURCE_EXHAUSTED
            : co == SOL_MIR_MATERIALIZE_BUILD_ALLOCATION_FAILED
                ? SOL_MIR_CONCRETE_BUILD_ALLOCATION_FAILED
            : co == SOL_MIR_MATERIALIZE_BUILD_INVALID_ARGUMENT
                ? SOL_MIR_CONCRETE_BUILD_INVALID_ARGUMENT
                : SOL_MIR_CONCRETE_BUILD_INTERNAL_FAILED;
        report(diagnostics, "concrete program materialization failed");
        sol_mir_concrete_program_free(&scratch); return out;
    }
    SolMirRepresentationBuildRequest d = {&scratch.materialization,
        &limits.representation};
    SolMirRepresentationBuildOutcome do_ = sol_mir_representation_build(&d,
        &scratch.representation, diagnostics);
    if (do_ != SOL_MIR_REPRESENTATION_BUILD_SUCCEEDED) {
        SolMirConcreteBuildOutcome out
            = do_ == SOL_MIR_REPRESENTATION_BUILD_UNSUPPORTED
                ? SOL_MIR_CONCRETE_BUILD_UNSUPPORTED_CLOSURE
            : do_ == SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED
                ? SOL_MIR_CONCRETE_BUILD_RESOURCE_EXHAUSTED
            : do_ == SOL_MIR_REPRESENTATION_BUILD_ALLOCATION_FAILED
                ? SOL_MIR_CONCRETE_BUILD_ALLOCATION_FAILED
            : do_ == SOL_MIR_REPRESENTATION_BUILD_INVALID_ARGUMENT
                ? SOL_MIR_CONCRETE_BUILD_INVALID_ARGUMENT
                : SOL_MIR_CONCRETE_BUILD_INTERNAL_FAILED;
        report(diagnostics, "concrete program representation build failed");
        sol_mir_concrete_program_free(&scratch); return out;
    }
    SolMirLayoutBuildRequest e = {&scratch.representation, request->target,
        &limits.layout};
    SolMirLayoutBuildOutcome eo = sol_mir_layout_build(&e, &scratch.layout,
        diagnostics);
    if (eo != SOL_MIR_LAYOUT_BUILD_SUCCEEDED) {
        SolMirConcreteBuildOutcome out = eo == SOL_MIR_LAYOUT_BUILD_INVALID_TARGET
            ? SOL_MIR_CONCRETE_BUILD_INVALID_TARGET
            : eo == SOL_MIR_LAYOUT_BUILD_UNSUPPORTED
                ? SOL_MIR_CONCRETE_BUILD_UNSUPPORTED_CLOSURE
            : eo == SOL_MIR_LAYOUT_BUILD_RESOURCE_EXHAUSTED
                ? SOL_MIR_CONCRETE_BUILD_RESOURCE_EXHAUSTED
            : eo == SOL_MIR_LAYOUT_BUILD_ALLOCATION_FAILED
                ? SOL_MIR_CONCRETE_BUILD_ALLOCATION_FAILED
            : eo == SOL_MIR_LAYOUT_BUILD_INVALID_ARGUMENT
                ? SOL_MIR_CONCRETE_BUILD_INVALID_ARGUMENT
                : SOL_MIR_CONCRETE_BUILD_INTERNAL_FAILED;
        report(diagnostics, "concrete program layout build failed");
        sol_mir_concrete_program_free(&scratch); return out;
    }
    SolMirOperationsBuildRequest f = {&scratch.layout, &limits.operations};
    SolMirOperationsBuildOutcome fo = sol_mir_operations_build(&f,
        &scratch.operations, diagnostics);
    if (fo != SOL_MIR_OPERATIONS_BUILD_SUCCEEDED) {
        SolMirConcreteBuildOutcome out = fo == SOL_MIR_OPERATIONS_BUILD_UNSUPPORTED
            ? SOL_MIR_CONCRETE_BUILD_UNSUPPORTED_CLOSURE
            : fo == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED
                ? SOL_MIR_CONCRETE_BUILD_RESOURCE_EXHAUSTED
            : fo == SOL_MIR_OPERATIONS_BUILD_ALLOCATION_FAILED
                ? SOL_MIR_CONCRETE_BUILD_ALLOCATION_FAILED
            : fo == SOL_MIR_OPERATIONS_BUILD_INVALID_ARGUMENT
                ? SOL_MIR_CONCRETE_BUILD_INVALID_ARGUMENT
                : SOL_MIR_CONCRETE_BUILD_INTERNAL_FAILED;
        report(diagnostics, "concrete program operations build failed");
        sol_mir_concrete_program_free(&scratch); return out;
    }
    SolMirLinkageBuildRequest g = {&scratch.operations, &limits.linkage};
    SolMirLinkageBuildOutcome go = sol_mir_linkage_build(&g, &scratch.linkage,
        diagnostics);
    if (go != SOL_MIR_LINKAGE_BUILD_SUCCEEDED) {
        SolMirConcreteBuildOutcome out
            = go == SOL_MIR_LINKAGE_BUILD_UNRESOLVED_EXTERNAL
                ? SOL_MIR_CONCRETE_BUILD_UNSUPPORTED_CLOSURE
            : go == SOL_MIR_LINKAGE_BUILD_SYMBOL_COLLISION
                ? SOL_MIR_CONCRETE_BUILD_SYMBOL_COLLISION
            : go == SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED
                ? SOL_MIR_CONCRETE_BUILD_RESOURCE_EXHAUSTED
            : go == SOL_MIR_LINKAGE_BUILD_ALLOCATION_FAILED
                ? SOL_MIR_CONCRETE_BUILD_ALLOCATION_FAILED
            : go == SOL_MIR_LINKAGE_BUILD_INVALID_ARGUMENT
                ? SOL_MIR_CONCRETE_BUILD_INVALID_ARGUMENT
                : SOL_MIR_CONCRETE_BUILD_INTERNAL_FAILED;
        report(diagnostics, "concrete program linkage build failed");
        sol_mir_concrete_program_free(&scratch); return out;
    }
    if (!sol_mir_concrete_program_validate(&scratch, diagnostics)) {
        SolMirConcreteBuildOutcome out = allocation_or_internal(diagnostics);
        report(diagnostics, "concrete program private validation failed");
        sol_mir_concrete_program_free(&scratch); return out;
    }
    *program = scratch;
    rebind(program);
    return SOL_MIR_CONCRETE_BUILD_SUCCEEDED;
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

static void append_file(Buffer *buffer, FILE *file) {
    if (buffer->failed || fflush(file) != 0 || fseek(file, 0, SEEK_END) != 0) {
        buffer->failed = true; return;
    }
    long end = ftell(file);
    if (end < 0 || fseek(file, 0, SEEK_SET) != 0
        || (size_t)end > SIZE_MAX - buffer->length - 1) {
        buffer->failed = true; return;
    }
    size_t needed = buffer->length + (size_t)end + 1;
    if (needed > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
            capacity *= 2;
        }
        char *grown = realloc(buffer->data, capacity);
        if (grown == NULL) { buffer->failed = true; return; }
        buffer->data = grown; buffer->capacity = capacity;
    }
    if ((size_t)end != 0
        && fread(buffer->data + buffer->length, 1, (size_t)end, file)
            != (size_t)end) {
        buffer->failed = true; return;
    }
    buffer->length += (size_t)end;
}

static void render_program(Buffer *out, const SolMirProgram *p) {
    format(out, "program roots=%zu approvals=%zu templates=%zu imports=%zu specializations=%zu references=%zu usage=%zu/%zu/%zu\n",
        p->root_count, p->approved_import_count, p->template_count,
        p->import_count, p->specialization_count, p->reference_count,
        p->usage.callable_classifications, p->usage.references,
        p->usage.discovery_work);
    for (size_t i = 0; i < p->root_count; ++i)
        format(out, "program.root %zu callable=%zu kind=%d\n", i,
            p->roots[i].callable, (int)p->roots[i].kind);
    for (size_t i = 0; i < p->approved_import_count; ++i)
        format(out, "program.approval %zu callable=%zu\n", i,
            p->approved_imports[i]);
    for (size_t i = 0; i < p->template_count; ++i) {
        format(out, "program.template %zu callable=%zu\n", i,
            p->templates[i].callable);
        FILE *file = tmpfile();
        if (file == NULL || !sol_mir_render(file, p->ir, &p->templates[i].mir)) {
            if (file != NULL) fclose(file); out->failed = true; return;
        }
        append_file(out, file); fclose(file);
    }
    for (size_t i = 0; i < p->import_count; ++i)
        format(out, "program.import %zu callable=%zu source=%zu:%zu:%zu:%zu:%zu count=%zu\n",
            i, p->imports[i].callable, p->imports[i].first_source.callable,
            p->imports[i].first_source.expression, p->imports[i].first_source.file,
            p->imports[i].first_source.start, p->imports[i].first_source.end,
            p->imports[i].source_count);
    for (size_t i = 0; i < p->specialization_count; ++i) {
        const SolMirProgramSpecialization *s = &p->specializations[i];
        format(out, "program.specialization %zu trait=%zu requirement=%zu type=%zu implementation=%zu method=%zu source=%zu:%zu:%zu:%zu:%zu count=%zu\n",
            i, s->trait, s->requirement, s->type, s->implementation, s->method,
            s->first_source.callable, s->first_source.expression,
            s->first_source.file, s->first_source.start, s->first_source.end,
            s->source_count);
    }
    for (size_t i = 0; i < p->reference_count; ++i) {
        const SolMirProgramReference *r = &p->references[i];
        format(out, "program.reference %zu kind=%d source=%zu:%zu:%zu:%zu:%zu target=%zu\n",
            i, (int)r->kind, r->source.callable, r->source.expression,
            r->source.file, r->source.start, r->source.end, r->target);
    }
}

static void render_plan(Buffer *out, const SolMirPlan *p) {
    format(out, "plan types=%zu instances=%zu imports=%zu demands=%zu uses=%zu contexts=%zu dictionaries=%zu usage=%zu/%zu/%zu/%zu/%zu/%zu/%zu\n",
        p->type_count, p->instance_count, p->import_count, p->demand_count,
        p->typed_use_count, p->context_count, p->dictionary_entry_count,
        p->usage.instances, p->usage.concrete_types, p->usage.demands,
        p->usage.typed_uses, p->usage.contexts, p->usage.planning_work,
        p->usage.substitution_depth);
    for (size_t i = 0; i < p->type_count; ++i) {
        const SolMirPlanType *t = &p->types[i];
        format(out, "plan.type %zu kind=%d definition=%zu arguments=%zu:%zu parameters=%zu:%zu accesses=%zu result=%zu effects=%zu capability=%zu ownership=%zu:%zu\n",
            i, (int)t->kind, t->definition, t->argument_offset,
            t->argument_count, t->parameter_offset, t->parameter_count,
            t->parameter_access_offset, t->result, t->effects,
            t->capability_source, t->ownership_component_offset,
            t->ownership_component_count);
    }
    for (size_t i = 0; i < p->type_component_count; ++i)
        format(out, "plan.type_component %zu type=%zu\n", i,
            p->type_components[i]);
    for (size_t i = 0; i < p->type_parameter_access_count; ++i)
        format(out, "plan.type_parameter_access %zu access=%d\n", i,
            (int)p->type_parameter_accesses[i]);
    for (size_t i = 0; i < p->effect_atom_count; ++i) {
        const SolMirPlanEffectAtom *a = &p->effect_atoms[i];
        format(out, "plan.effect_atom %zu name=", i);
        for (size_t q = 0; q < a->length; ++q)
            format(out, "%02x", (unsigned char)a->name[q]);
        format(out, " authority=%d ordinal=%zu\n", (int)a->authority,
            a->ordinal);
    }
    for (size_t i = 0; i < p->effect_row_count; ++i)
        format(out, "plan.effect_row %zu atoms=%zu:%zu\n", i,
            p->effect_rows[i].atom_offset, p->effect_rows[i].atom_count);
    for (size_t i = 0; i < p->effect_row_atom_count; ++i)
        format(out, "plan.effect_row_atom %zu atom=%zu\n", i,
            p->effect_row_atoms[i]);
    for (size_t i = 0; i < p->instance_count; ++i) {
        const SolMirPlanInstance *x = &p->instances[i];
        format(out, "plan.instance %zu callable=%zu receiver=%zu arguments=%zu:%zu dictionary=%zu:%zu parameters=%zu:%zu accesses=%zu:%zu result=%zu tail=%zu effects=%zu uses=%zu:%zu contexts=%zu:%zu\n",
            i, x->callable, x->receiver, x->type_arguments.offset,
            x->type_arguments.count, x->dictionary.offset, x->dictionary.count,
            x->parameter_types.offset, x->parameter_types.count,
            x->parameter_accesses.offset, x->parameter_accesses.count,
            x->result, x->effect_tail, x->effects, x->typed_uses.offset,
            x->typed_uses.count, x->contexts.offset, x->contexts.count);
    }
    for (size_t i = 0; i < p->instance_type_id_count; ++i)
        format(out, "plan.instance_type %zu type=%zu\n", i,
            p->instance_type_ids[i]);
    for (size_t i = 0; i < p->instance_access_count; ++i)
        format(out, "plan.instance_access %zu access=%d\n", i,
            (int)p->instance_accesses[i]);
    for (size_t i = 0; i < p->dictionary_entry_count; ++i) {
        const SolMirPlanDictionaryEntry *d = &p->dictionary_entries[i];
        format(out, "plan.dictionary %zu generic=%zu trait=%zu requirement=%zu type=%zu implementation=%zu method=%zu\n",
            i, d->generic_ordinal, d->trait, d->requirement, d->type,
            d->implementation, d->method);
    }
    for (size_t i = 0; i < p->import_count; ++i) {
        const SolMirPlanImport *x = &p->imports[i];
        format(out, "plan.import %zu callable=%zu receiver=%zu parameters=%zu:%zu accesses=%zu:%zu result=%zu effects=%zu uses=%zu:%zu contexts=%zu:%zu\n",
            i, x->callable, x->receiver, x->parameter_types.offset,
            x->parameter_types.count, x->parameter_accesses.offset,
            x->parameter_accesses.count, x->result, x->effects,
            x->typed_uses.offset, x->typed_uses.count, x->contexts.offset,
            x->contexts.count);
    }
    for (size_t i = 0; i < p->typed_use_count; ++i) {
        const SolMirPlanTypedUse *u = &p->typed_uses[i];
        format(out, "plan.use %zu kind=%d source=%zu ordinal=%zu context=%zu type=%zu access=%d\n",
            i, (int)u->kind, u->source, u->ordinal, u->context, u->type,
            (int)u->access);
    }
    for (size_t i = 0; i < p->context_count; ++i) {
        const SolMirPlanContext *c = &p->contexts[i];
        format(out, "plan.context %zu kind=%d instance=%zu block=%zu definition=%zu obligation=%zu source=%zu:%zu:%zu:%zu:%zu target=%d import=%zu refinement=%zu\n",
            i, (int)c->kind, c->instance, c->source_block, c->definition,
            c->obligation, c->source.callable, c->source.expression,
            c->source.file, c->source.start, c->source.end,
            (int)c->target_kind, c->import, c->refinement_type);
    }
    for (size_t i = 0; i < p->demand_count; ++i) {
        const SolMirPlanDemand *d = &p->demands[i];
        format(out, "plan.demand %zu kind=%d owner=%d parent=%zu import_parent=%zu source=%zu:%zu:%zu:%zu:%zu symbolic=%zu instance=%zu import=%zu trait=%zu requirement=%zu context=%zu\n",
            i, (int)d->kind, (int)d->owner_kind, d->parent,
            d->parent_import, d->source.callable, d->source.expression,
            d->source.file, d->source.start, d->source.end,
            d->symbolic_target, d->instance, d->import, d->dispatch_trait,
            d->dispatch_requirement, d->context);
    }
}

static void render_materialization_supplement(Buffer *out,
    const SolMirMaterialization *m) {
    format(out, "materialization.complete counts=%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu usage=%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu\n",
        m->image_count, m->type_count, m->shape_field_count,
        m->shape_variant_count, m->type_id_count, m->access_count,
        m->overlay_count, m->context_count, m->local_count, m->place_count,
        m->projection_count, m->value_count, m->instruction_count,
        m->temporary_count, m->construct_operand_count, m->call_argument_count,
        m->block_count, m->edge_count, m->edge_value_count,
        m->parameter_value_count, m->loop_count, m->binding_count,
        m->semantic_site_count, m->receiver_root_count, m->import_count,
        m->handler_count, m->writeback_count, m->effect_row_count,
        m->effect_atom_count, m->effect_row_atom_count, m->effect_name_count,
        m->literal_byte_count, m->usage.instances, m->usage.cfg_items,
        m->usage.bindings, m->usage.concrete_records, m->usage.owned_bytes,
        m->usage.materialization_work, m->usage.shape_resolution_work,
        m->usage.validation_work);
    for (size_t i = 0; i < m->type_count; ++i) {
        const SolMirMaterializedType *t = &m->types[i];
        format(out, "materialization.type_complete %zu arguments=%zu:%zu parameters=%zu:%zu accesses=%zu:%zu result=%zu\n",
            i, t->arguments.offset, t->arguments.count, t->parameters.offset,
            t->parameters.count, t->parameter_accesses.offset,
            t->parameter_accesses.count, t->result);
    }
    for (size_t i = 0; i < m->type_id_count; ++i)
        format(out, "materialization.type_id %zu=%zu\n", i, m->type_ids[i]);
    for (size_t i = 0; i < m->access_count; ++i)
        format(out, "materialization.access %zu=%d\n", i, (int)m->accesses[i]);
    for (size_t i = 0; i < m->overlay_count; ++i) {
        const SolMirMaterializedTypeOverlay *x = &m->overlays[i];
        format(out, "materialization.overlay %zu kind=%d source=%zu ordinal=%zu context=%zu type=%zu access=%d\n",
            i, (int)x->kind, x->source, x->ordinal, x->context, x->type,
            (int)x->access);
    }
    for (size_t i = 0; i < m->context_count; ++i) {
        const SolMirPlanContext *x = &m->contexts[i];
        format(out, "materialization.context_complete %zu source=%zu:%zu:%zu:%zu:%zu\n",
            i, x->source.callable, x->source.expression, x->source.file,
            x->source.start, x->source.end);
    }
    for (size_t i = 0; i < m->image_count; ++i) {
        const SolMirMaterializedImage *x = &m->images[i];
        format(out, "materialization.image_complete %zu instance=%zu type_arguments=%zu:%zu parameters=%zu:%zu accesses=%zu:%zu overlays=%zu:%zu contexts=%zu:%zu locals=%zu:%zu places=%zu:%zu values=%zu:%zu instructions=%zu:%zu temporaries=%zu:%zu construct_operands=%zu:%zu call_arguments=%zu:%zu blocks=%zu:%zu loops=%zu:%zu handlers=%zu:%zu bindings=%zu:%zu contract=%zu/%zu\n",
            i, x->instance, x->type_arguments.offset, x->type_arguments.count,
            x->parameter_types.offset, x->parameter_types.count,
            x->parameter_accesses.offset, x->parameter_accesses.count,
            x->overlays.offset, x->overlays.count, x->contexts.offset,
            x->contexts.count, x->locals.offset, x->locals.count,
            x->places.offset, x->places.count, x->values.offset,
            x->values.count, x->instructions.offset, x->instructions.count,
            x->temporaries.offset, x->temporaries.count,
            x->construct_operands.offset, x->construct_operands.count,
            x->call_arguments.offset, x->call_arguments.count,
            x->blocks.offset, x->blocks.count, x->loops.offset, x->loops.count,
            x->handlers.offset, x->handlers.count, x->bindings.offset,
            x->bindings.count, x->contract_body, x->contract_epilogue);
    }
    for (size_t i = 0; i < m->local_count; ++i)
        format(out, "materialization.local_owner %zu instance=%zu\n", i,
            m->locals[i].instance);
    for (size_t i = 0; i < m->place_count; ++i)
        format(out, "materialization.place_owner %zu instance=%zu\n", i,
            m->places[i].instance);
    for (size_t i = 0; i < m->binding_count; ++i) {
        const SolMirMaterializedBinding *x = &m->bindings[i];
        format(out, "materialization.binding_complete %zu source=%zu:%zu:%zu:%zu:%zu site=%zu instance=%zu import=%zu\n",
            i, x->source.callable, x->source.expression, x->source.file,
            x->source.start, x->source.end, x->site, x->instance, x->import);
    }
    for (size_t i = 0; i < m->semantic_site_count; ++i) {
        const SolMirMaterializedSemanticSite *x = &m->semantic_sites[i];
        format(out, "materialization.site_complete %zu producer=%d source=%zu:%zu:%zu:%zu:%zu operation=%d:%zu:%zu:%zu:%zu:%zu\n",
            i, (int)x->producer_kind, x->source.callable,
            x->source.expression, x->source.file, x->source.start,
            x->source.end, (int)x->operation.target_kind,
            x->operation.instance, x->operation.import, x->operation.receiver,
            x->operation.root, x->operation.effects);
    }
    for (size_t i = 0; i < m->receiver_root_count; ++i)
        format(out, "materialization.receiver_root %zu=%zu\n", i,
            m->receiver_roots[i]);
    for (size_t i = 0; i < m->handler_count; ++i) {
        const SolMirMaterializedHandler *x = &m->handlers[i];
        format(out, "materialization.handler_complete %zu parent=%zu context=%zu expression=%zu operation_binding=%zu operation=%d:%zu:%zu:%zu:%zu:%zu span=%zu:%zu\n",
            i, x->parent, x->context, x->source_expression,
            x->source_binding, (int)x->operation.target_kind,
            x->operation.instance, x->operation.import, x->operation.receiver,
            x->operation.root, x->operation.effects, x->span.start,
            x->span.end);
    }
}

static void render_representation_supplement(Buffer *out,
    const SolMirRepresentation *r) {
    format(out, "representation.complete counts=%zu/%zu/%zu/%zu/%zu/%zu/%zu usage=%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu\n",
        r->recipe_count, r->field_count, r->variant_count, r->recipe_id_count,
        r->access_count, r->receiver_root_count, r->callable_producer_count,
        r->usage.recipes, r->usage.fields, r->usage.variants,
        r->usage.recipe_ids, r->usage.callable_producers,
        r->usage.receiver_roots, r->usage.owned_bytes,
        r->usage.build_scratch_bytes, r->usage.build_work,
        r->usage.validation_work, r->usage.validation_scratch_bytes);
    for (size_t i = 0; i < r->recipe_count; ++i)
        format(out, "representation.recipe_complete %zu parameter_accesses=%zu:%zu\n",
            i, r->recipes[i].parameter_accesses.offset,
            r->recipes[i].parameter_accesses.count);
    for (size_t i = 0; i < r->recipe_id_count; ++i)
        format(out, "representation.recipe_id %zu=%zu\n", i,
            r->recipe_ids[i]);
    for (size_t i = 0; i < r->access_count; ++i)
        format(out, "representation.access %zu=%d\n", i,
            (int)r->accesses[i]);
    for (size_t i = 0; i < r->receiver_root_count; ++i)
        format(out, "representation.receiver_root %zu=%zu\n", i,
            r->receiver_roots[i]);
}

static void render_layout_supplement(Buffer *out, const SolMirLayout *layout) {
    format(out, "layout.complete usage=%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu\n",
        layout->usage.type_layouts, layout->usage.field_layouts,
        layout->usage.variant_layouts, layout->usage.projection_maps,
        layout->usage.owned_bytes, layout->usage.build_scratch_bytes,
        layout->usage.build_work, layout->usage.validation_scratch_bytes,
        layout->usage.validation_work);
}

static void render_operations_supplement(Buffer *out,
    const SolMirOperations *operations) {
#define USAGE(member, type, singular) \
    format(out, "operations.usage.%s=%zu\n", #member, operations->usage.member);
    SOL_MIR_OPERATIONS_ARENAS(USAGE)
#undef USAGE
    format(out, "operations.resources owned=%zu build_scratch=%zu build_work=%zu validation_scratch=%zu validation_work=%zu\n",
        operations->usage.owned_bytes, operations->usage.build_scratch_bytes,
        operations->usage.build_work,
        operations->usage.validation_scratch_bytes,
        operations->usage.validation_work);
}

static void render_linkage_supplement(Buffer *out,
    const SolMirLinkage *linkage) {
#define USAGE(member, type, singular) \
    format(out, "linkage.usage.%s=%zu\n", #member, linkage->usage.member);
    SOL_MIR_LINKAGE_ARENAS(USAGE)
#undef USAGE
    format(out, "linkage.resources owned=%zu build_scratch=%zu build_work=%zu validation_scratch=%zu validation_work=%zu\n",
        linkage->usage.owned_bytes, linkage->usage.build_scratch_bytes,
        linkage->usage.build_work, linkage->usage.validation_scratch_bytes,
        linkage->usage.validation_work);
    format(out, "linkage.authentication callable=instance-key binding=semantic-target entry=export-symbol host=requirement-key table=identity callable-value=table-identity runtime=recipe-key\n");
}

typedef bool (*StageRenderer)(FILE *, const void *);
static bool render_materialization(FILE *f, const void *p) {
    return sol_mir_materialization_render(f, p);
}
static bool render_representation(FILE *f, const void *p) {
    return sol_mir_representation_render(f, p);
}
static bool render_layout(FILE *f, const void *p) {
    return sol_mir_layout_render(f, p);
}
static bool render_operations(FILE *f, const void *p) {
    return sol_mir_operations_render(f, p);
}
static bool render_linkage(FILE *f, const void *p) {
    return sol_mir_linkage_render(f, p);
}

static void capture(Buffer *out, StageRenderer renderer, const void *stage,
    bool filter_limits) {
    FILE *file = tmpfile();
    if (file == NULL || !renderer(file, stage)) {
        if (file != NULL) fclose(file); out->failed = true; return;
    }
    if (!filter_limits) {
        append_file(out, file); fclose(file); return;
    }
    if (fflush(file) != 0 || fseek(file, 0, SEEK_SET) != 0) {
        out->failed = true; fclose(file); return;
    }
    char line[8192];
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strncmp(line, "limits ", 7) == 0
            || strncmp(line, "predicate_limits ", 17) == 0) continue;
        format(out, "%s", line);
    }
    if (ferror(file)) out->failed = true;
    fclose(file);
}

bool sol_mir_concrete_program_render(FILE *stream,
    const SolMirConcreteProgram *program) {
    if (stream == NULL || !sol_mir_concrete_program_validate(program, NULL))
        return false;
    Buffer out = {0};
    format(&out, "mir_concrete_program\nsection=program\n");
    render_program(&out, &program->program);
    format(&out, "section=plan\n"); render_plan(&out, &program->plan);
    format(&out, "section=materialization\n");
    capture(&out, render_materialization, &program->materialization, false);
    render_materialization_supplement(&out, &program->materialization);
    format(&out, "section=representation\n");
    capture(&out, render_representation, &program->representation, false);
    render_representation_supplement(&out, &program->representation);
    format(&out, "section=layout\n");
    capture(&out, render_layout, &program->layout, false);
    render_layout_supplement(&out, &program->layout);
    format(&out, "section=operations\n");
    capture(&out, render_operations, &program->operations, true);
    render_operations_supplement(&out, &program->operations);
    format(&out, "section=linkage\n");
    capture(&out, render_linkage, &program->linkage, false);
    render_linkage_supplement(&out, &program->linkage);
    bool ok = !out.failed && (out.length == 0
        || fwrite(out.data, out.length, 1, stream) == 1);
    free(out.data); return ok;
}
