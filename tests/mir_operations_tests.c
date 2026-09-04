#include "sol/mir_operations.h"

#include "sol/effects.h"
#include "sol/lexer.h"
#include "sol/ownership.h"
#include "sol/package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: check failed: %s\n", \
    __FILE__, __LINE__, #x); ++failures; } } while (0)

bool sol_mir_operations_internal_expected_build_work(const SolMirOperations *,
    size_t *);

typedef struct {
    SolSource source;
    SolTokens tokens;
    SolSyntaxTree syntax;
    SolDiagnostics diagnostics;
    SolHirModule hir;
    SolTypeTable types;
    SolEffectTable effects;
    SolContractTable contracts;
    SolIr ir;
} Compilation;

typedef struct {
    SolMirProgram program;
    SolMirPlan plan;
    SolMirMaterialization materialization;
    SolMirRepresentation representation;
    SolMirLayout layout;
    SolMirOperations operations;
    SolDiagnostics diagnostics;
} Pipeline;

static SolIrCallableId callable(const SolIr *ir, const char *name,
    SolIrCallableKind kind) {
    for (size_t i = 0; i < ir->callable_count; ++i)
        if (ir->callables[i].kind == kind && strcmp(ir->callables[i].name, name) == 0)
            return i;
    return SOL_IR_NONE;
}

static void pipeline_init(Pipeline *p) {
    memset(p, 0, sizeof(*p));
    sol_mir_program_init(&p->program); sol_mir_plan_init(&p->plan);
    sol_mir_materialization_init(&p->materialization);
    sol_mir_representation_init(&p->representation);
    sol_mir_layout_init(&p->layout); sol_mir_operations_init(&p->operations);
    sol_diagnostics_init(&p->diagnostics);
}

static void pipeline_free(Pipeline *p) {
    sol_mir_operations_free(&p->operations); sol_mir_layout_free(&p->layout);
    sol_mir_representation_free(&p->representation);
    sol_mir_materialization_free(&p->materialization);
    sol_mir_plan_free(&p->plan); sol_mir_program_free(&p->program);
    sol_diagnostics_free(&p->diagnostics);
}

static bool build_pipeline(const SolIr *ir, const SolMirProgramRoot *roots,
    size_t root_count, const SolIrCallableId *imports, size_t import_count,
    Pipeline *p, const SolMirOperationsLimits *limits) {
    SolMirProgramBuildRequest a = {ir, roots, root_count, imports, import_count, NULL};
    SolMirPlanBuildRequest b = {&p->program, NULL};
    SolMirMaterializeBuildRequest c = {&p->plan, NULL};
    SolMirRepresentationBuildRequest d = {&p->materialization, NULL};
    SolMirTargetDescriptor wasm = sol_mir_target_wasm32();
    SolMirLayoutBuildRequest e = {&p->representation, &wasm, NULL};
    SolMirOperationsBuildRequest f = {&p->layout, limits};
    bool layout = sol_mir_program_build(&a, &p->program, &p->diagnostics)
            == SOL_MIR_PROGRAM_BUILD_SUCCEEDED
        && sol_mir_plan_build(&b, &p->plan, &p->diagnostics)
            == SOL_MIR_PLAN_BUILD_SUCCEEDED
        && sol_mir_materialize_build(&c, &p->materialization, &p->diagnostics)
            == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED
        && sol_mir_representation_build(&d, &p->representation, &p->diagnostics)
            == SOL_MIR_REPRESENTATION_BUILD_SUCCEEDED
        && sol_mir_layout_build(&e, &p->layout, &p->diagnostics)
            == SOL_MIR_LAYOUT_BUILD_SUCCEEDED;
    return layout && sol_mir_operations_build(&f, &p->operations, &p->diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_SUCCEEDED;
}

static bool compile_text(Compilation *c, const char *text) {
    memset(c, 0, sizeof(*c)); sol_tokens_init(&c->tokens);
    sol_diagnostics_init(&c->diagnostics); sol_syntax_tree_init(&c->syntax);
    sol_hir_module_init(&c->hir); sol_type_table_init(&c->types);
    sol_effect_table_init(&c->effects); sol_contract_table_init(&c->contracts);
    sol_ir_init(&c->ir);
    return sol_source_from_text(&c->source, "operations.sol", text)
        && sol_lex(&c->source, &c->tokens, &c->diagnostics)
        && sol_parse(&c->source, &c->tokens, &c->syntax, &c->diagnostics)
        && sol_hir_lower(&c->source, &c->syntax, &c->hir, &c->diagnostics)
        && sol_type_check(&c->source, &c->syntax, &c->hir, &c->types, &c->diagnostics)
        && sol_effect_check(&c->source, &c->syntax, &c->hir, &c->types,
            &c->effects, &c->diagnostics)
        && sol_contract_lower(&c->source, &c->syntax, &c->hir, &c->types,
            &c->effects, &c->contracts, &c->diagnostics)
        && sol_ir_lower(&c->source, &c->syntax, &c->hir, &c->types,
            &c->effects, &c->contracts, &c->ir, &c->diagnostics);
}

static void free_text(Compilation *c) {
    sol_ir_free(&c->ir); sol_contract_table_free(&c->contracts);
    sol_effect_table_free(&c->effects); sol_type_table_free(&c->types);
    sol_hir_module_free(&c->hir); sol_syntax_tree_free(&c->syntax);
    sol_tokens_free(&c->tokens); sol_source_free(&c->source);
    sol_diagnostics_free(&c->diagnostics);
}

static bool compile_e6(Compilation *c, SolPackage *package) {
    memset(c, 0, sizeof(*c)); sol_package_init(package);
    sol_diagnostics_init(&c->diagnostics); sol_hir_module_init(&c->hir);
    sol_type_table_init(&c->types); sol_effect_table_init(&c->effects);
    sol_contract_table_init(&c->contracts); sol_ir_init(&c->ir);
    char message[256];
    if (!sol_package_load_directory(package,
            SOL_TEST_SOURCE_DIR "/tests/conformance/e6", &c->diagnostics,
            message, sizeof(message))) return false;
    SolHirFileScope *scopes = package->file_count == 0 ? NULL
        : malloc(package->file_count * sizeof(*scopes));
    if (package->file_count != 0 && scopes == NULL) return false;
    for (size_t i = 0; i < package->file_count; ++i)
        scopes[i] = (SolHirFileScope){package->files[i].module_name,
            package->files[i].import_start, package->files[i].import_count,
            package->files[i].item_start, package->files[i].item_count};
    bool ok = sol_hir_lower_scoped(&package->source, &package->syntax, scopes,
            package->file_count, &c->hir, &c->diagnostics)
        && sol_type_check(&package->source, &package->syntax, &c->hir,
            &c->types, &c->diagnostics)
        && sol_effect_check(&package->source, &package->syntax, &c->hir,
            &c->types, &c->effects, &c->diagnostics)
        && sol_contract_lower(&package->source, &package->syntax, &c->hir,
            &c->types, &c->effects, &c->contracts, &c->diagnostics)
        && sol_ir_lower_scoped(&package->source, &package->syntax, &c->hir,
            &c->types, &c->effects, &c->contracts, package->files,
            package->file_count, &c->ir, &c->diagnostics);
    free(scopes); return ok;
}

static void free_e6(Compilation *c, SolPackage *package) {
    sol_ir_free(&c->ir); sol_contract_table_free(&c->contracts);
    sol_effect_table_free(&c->effects); sol_type_table_free(&c->types);
    sol_hir_module_free(&c->hir); sol_diagnostics_free(&c->diagnostics);
    sol_package_free(package);
}

static char *render(const SolMirOperations *o, size_t *length) {
    FILE *f = tmpfile();
    if (f == NULL || !sol_mir_operations_render(f, o) || fflush(f) != 0
        || fseek(f, 0, SEEK_END) != 0) { if (f != NULL) fclose(f); return NULL; }
    long end = ftell(f);
    if (end < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *text = malloc((size_t)end + 1);
    if (text == NULL || fread(text, 1, (size_t)end, f) != (size_t)end) {
        free(text); fclose(f); return NULL;
    }
    fclose(f); text[end] = '\0'; *length = (size_t)end; return text;
}

static SolMirOperationsLimits exact_limits(const SolMirOperations *o) {
#define NONZERO(value) ((value) == 0 ? 1 : (value))
    return (SolMirOperationsLimits){
        .max_access_plans = NONZERO(o->usage.access_plans),
        .max_access_steps = NONZERO(o->usage.access_steps),
        .max_constructors = NONZERO(o->usage.constructors),
        .max_construct_operands = NONZERO(o->usage.construct_operands),
        .max_pattern_tests = NONZERO(o->usage.pattern_tests),
        .max_pattern_extractions = NONZERO(o->usage.pattern_extractions),
        .max_pattern_nodes = NONZERO(o->usage.pattern_nodes),
        .max_path_steps = NONZERO(o->usage.path_steps),
        .max_propagations = NONZERO(o->usage.propagations),
        .max_arithmetic = NONZERO(o->usage.arithmetic),
        .max_equality_nodes = NONZERO(o->usage.equality_nodes),
        .max_equality_children = NONZERO(o->usage.equality_children),
        .max_snapshots = NONZERO(o->usage.snapshots),
        .max_callables = NONZERO(o->usage.callables),
        .max_handlers = NONZERO(o->usage.handlers),
        .max_predicates = NONZERO(o->usage.predicates),
        .max_recipe_ids = NONZERO(o->usage.recipe_ids),
        .max_roots = NONZERO(o->usage.roots),
        .max_provenance = NONZERO(o->usage.provenance),
        .max_owned_bytes = NONZERO(o->usage.owned_bytes),
        .max_build_scratch_bytes = NONZERO(o->usage.build_scratch_bytes),
        .max_build_work = NONZERO(o->usage.build_work),
        .max_validation_scratch_bytes
            = NONZERO(o->usage.validation_scratch_bytes),
        .max_validation_work = NONZERO(o->usage.validation_work)};
#undef NONZERO
}

static void test_e6_operations(void) {
    Compilation c; SolPackage package;
    bool compiled = compile_e6(&c, &package); CHECK(compiled);
    if (!compiled) { free_e6(&c, &package); return; }
    const char *names[] = {"write", "get", "count", "read"};
    SolIrCallableId imports[4];
    for (size_t i = 0; i < 4; ++i)
        imports[i] = callable(&c.ir, names[i], SOL_IR_CALLABLE_CAPABILITY);
    SolMirProgramRoot roots[5] = {{callable(&c.ir, "launch",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_ENTRY}};
    size_t root_count = 1;
    for (size_t i = 0; i < c.ir.callable_count; ++i)
        if (c.ir.callables[i].kind == SOL_IR_CALLABLE_TEST)
            roots[root_count++] = (SolMirProgramRoot){i, SOL_MIR_PROGRAM_ROOT_TEST};
    Pipeline p; pipeline_init(&p);
    bool built = root_count == 5 && build_pipeline(&c.ir, roots, root_count,
        imports, 4, &p, NULL);
    if (!built) sol_diagnostics_render_human(stderr, &package.source, &p.diagnostics);
    CHECK(built);
    if (!built) { pipeline_free(&p); free_e6(&c, &package); return; }
    SolMirOperations *o = &p.operations;
    CHECK(sol_mir_operations_validate(o, NULL));
    CHECK(o->access_plan_count == p.materialization.place_count
        && o->access_step_count == 5);
    CHECK(o->constructor_count == 28 && o->arithmetic_count == 17
        && o->pattern_test_count == 3 && o->pattern_extraction_count == 3
        && o->propagation_count == 2 && o->snapshot_count == 1
        && o->predicate_count == 4 && o->handler_count == 0
        && o->callable_count == 0);
    size_t checked = 0, comparison = 0, equality = 0, compound = 0;
    for (size_t i = 0; i < o->arithmetic_count; ++i) {
        checked += o->arithmetic[i].failures != SOL_MIR_OPERATION_FAILURE_NONE;
        comparison += o->arithmetic[i].opcode >= SOL_MIR_OPERATION_I64_LT
            && o->arithmetic[i].opcode <= SOL_MIR_OPERATION_I64_GE;
        equality += o->arithmetic[i].opcode == SOL_MIR_OPERATION_VALUE_EQ
            || o->arithmetic[i].opcode == SOL_MIR_OPERATION_VALUE_NE;
        compound += o->arithmetic[i].compound;
    }
    /* The current E6 closure has four checked binary operations plus its one
       checked compound update; its remaining twelve operations compare. */
    CHECK(checked == 5 && comparison + equality == 12 && compound == 1);
    size_t contract = 0, refinement = 0;
    for (size_t i = 0; i < o->predicate_count; ++i) {
        contract += o->predicates[i].kind == SOL_MIR_OPERATION_PREDICATE_CONTRACT;
        refinement += o->predicates[i].kind == SOL_MIR_OPERATION_PREDICATE_REFINEMENT;
        CHECK(o->predicates[i].state == SOL_MIR_OPERATION_PREDICATE_UNRESOLVED_BODY);
    }
    CHECK(contract == 3 && refinement == 1);
    for (size_t i = 0; i < o->propagation_count; ++i) {
        const SolMirOperationPropagationPlan *plan = &o->propagations[i];
        if (plan->source_residual_field_layout == SOL_MIR_OPERATION_NONE) {
            CHECK(plan->destination_residual_field_layout == SOL_MIR_OPERATION_NONE
                && plan->source_residual_field_offset == SOL_MIR_LAYOUT_OFFSET_NONE
                && plan->destination_residual_field_offset
                    == SOL_MIR_LAYOUT_OFFSET_NONE);
            continue;
        }
        CHECK(p.representation.fields[plan->source_residual_field_layout].type
            == p.representation.fields[plan->destination_residual_field_layout].type);
    }
    size_t first_length = 0, second_length = 0;
    char *first = render(o, &first_length);
    SolMirOperations repeated; sol_mir_operations_init(&repeated);
    SolMirOperationsBuildRequest request = {&p.layout, NULL};
    CHECK(sol_mir_operations_build(&request, &repeated, &p.diagnostics)
        == SOL_MIR_OPERATIONS_BUILD_SUCCEEDED);
    char *second = render(&repeated, &second_length);
    CHECK(first != NULL && second != NULL && first_length == second_length
        && memcmp(first, second, first_length) == 0);
    CHECK(first != NULL && strstr(first, "access_step ") != NULL
        && strstr(first, "construct_write ") != NULL
        && strstr(first, "pattern_node ") != NULL
        && strstr(first, "path_step ") != NULL
        && strstr(first, "equality_node ") != NULL
        && strstr(first, "equality_child ") != NULL
        && strstr(first, "provenance ") != NULL);
    free(first); free(second); sol_mir_operations_free(&repeated);

    SolMirOperationsLimits exact = exact_limits(o);
    size_t prerequisite_work = p.layout.usage.validation_work;
    size_t local_work = o->usage.validation_work - prerequisite_work;
    size_t local_scratch = o->provenance_count > p.representation.recipe_count
        ? o->provenance_count : p.representation.recipe_count;
    CHECK(o->usage.validation_work > prerequisite_work && local_work != 0);
    CHECK(p.layout.usage.validation_scratch_bytes > local_scratch
        && o->usage.validation_scratch_bytes
            == p.layout.usage.validation_scratch_bytes);
    request.limits = &exact;
    CHECK(sol_mir_operations_build(&request, &repeated, &p.diagnostics)
        == SOL_MIR_OPERATIONS_BUILD_SUCCEEDED);
    CHECK(sol_mir_operations_validate(&repeated, NULL));
    sol_mir_operations_free(&repeated);
    SolMirOperationsLimits local_only = exact;
    local_only.max_validation_work = local_work;
    request.limits = &local_only;
    CHECK(sol_mir_operations_build(&request, &repeated, &p.diagnostics)
        == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED
        && repeated.layout == NULL);
    local_only = exact;
    local_only.max_validation_scratch_bytes = local_scratch;
    request.limits = &local_only;
    CHECK(sol_mir_operations_build(&request, &repeated, &p.diagnostics)
        == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED
        && repeated.layout == NULL);
    request.limits = &exact;
#define LESS(member) do { SolMirOperationsLimits less = exact; --less.member; \
    request.limits = &less; CHECK(sol_mir_operations_build(&request, &repeated, \
        &p.diagnostics) == (exact.member == 1 \
            ? SOL_MIR_OPERATIONS_BUILD_INVALID_ARGUMENT \
            : SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED)); \
    CHECK(repeated.layout == NULL); } while (0)
    LESS(max_access_plans); LESS(max_access_steps); LESS(max_constructors);
    LESS(max_construct_operands); LESS(max_pattern_tests);
    LESS(max_pattern_extractions); LESS(max_pattern_nodes); LESS(max_path_steps);
    LESS(max_propagations); LESS(max_arithmetic); LESS(max_equality_nodes);
    LESS(max_equality_children);
    LESS(max_snapshots); LESS(max_predicates); LESS(max_recipe_ids);
    LESS(max_provenance); LESS(max_owned_bytes); LESS(max_build_scratch_bytes);
    LESS(max_build_work); LESS(max_validation_scratch_bytes);
    LESS(max_validation_work);
#undef LESS
    request.limits = NULL;

#define MUTATE(arena, index, member, value) do { \
    unsigned char saved[sizeof(o->arena[index])]; \
    memcpy(saved, &o->arena[index], sizeof(saved)); \
    o->arena[index].member = (value); CHECK(!sol_mir_operations_validate(o, NULL)); \
    memcpy(&o->arena[index], saved, sizeof(saved)); } while (0)
    MUTATE(access_plans, 0, place, SOL_MIR_OPERATION_NONE);
    MUTATE(access_plans, 0, image, SOL_MIR_OPERATION_NONE);
    MUTATE(access_plans, 0, local, SOL_MIR_OPERATION_NONE);
    MUTATE(access_plans, 0, steps.count, o->access_plans[0].steps.count + 1);
    MUTATE(access_steps, 0, object_offset, o->access_steps[0].object_offset ^ 1u);
    MUTATE(constructors, 0, kind, (SolMirOperationConstructKind)99);
    MUTATE(constructors, 0, result_recipe, SOL_MIR_RECIPE_NONE);
    MUTATE(construct_operands, 0, layout_field, SOL_MIR_OPERATION_NONE);
    MUTATE(construct_operands, 0, temporary, SOL_MIR_OPERATION_NONE);
    MUTATE(pattern_tests, 0, nodes.offset, SOL_MIR_OPERATION_NONE);
    MUTATE(pattern_nodes, 0, kind, (SolMirOperationPatternKind)99);
    MUTATE(pattern_nodes, 0, boolean, !o->pattern_nodes[0].boolean);
    MUTATE(path_steps, 0, field_layout, SOL_MIR_OPERATION_NONE);
    MUTATE(pattern_extractions, 0, copy_kind, SOL_MIR_COPY_FORBIDDEN);
    MUTATE(pattern_extractions, 0, path.offset, SOL_MIR_OPERATION_NONE);
    MUTATE(propagations, 0, success_variant_layout, SOL_MIR_OPERATION_NONE);
    MUTATE(propagations, 0, success_tag, o->propagations[0].success_tag ^ 1u);
    MUTATE(propagations, 0, success_field_layout, SOL_MIR_OPERATION_NONE);
    MUTATE(propagations, 0, success_field_offset,
        o->propagations[0].success_field_offset ^ 1u);
    MUTATE(propagations, 0, source_residual_variant_layout,
        SOL_MIR_OPERATION_NONE);
    MUTATE(propagations, 0, source_residual_tag,
        o->propagations[0].source_residual_tag ^ 1u);
    MUTATE(propagations, 0, destination_residual_variant_layout,
        SOL_MIR_OPERATION_NONE);
    MUTATE(propagations, 0, destination_residual_tag,
        o->propagations[0].destination_residual_tag ^ 1u);
    size_t result_propagation = 0;
    while (result_propagation < o->propagation_count
        && o->propagations[result_propagation].source_residual_field_layout
            == SOL_MIR_OPERATION_NONE) ++result_propagation;
    CHECK(result_propagation < o->propagation_count);
    if (result_propagation < o->propagation_count) {
        MUTATE(propagations, result_propagation, source_residual_field_layout,
            SOL_MIR_OPERATION_NONE);
        MUTATE(propagations, result_propagation, source_residual_field_recipe,
            SOL_MIR_RECIPE_NONE);
        MUTATE(propagations, result_propagation, source_residual_field_offset,
            o->propagations[result_propagation].source_residual_field_offset ^ 1u);
        MUTATE(propagations, result_propagation, destination_residual_field_layout,
            SOL_MIR_OPERATION_NONE);
        MUTATE(propagations, result_propagation,
            destination_residual_field_recipe, SOL_MIR_RECIPE_NONE);
        MUTATE(propagations, result_propagation,
            destination_residual_field_offset,
            o->propagations[result_propagation].destination_residual_field_offset ^ 1u);
    }
    MUTATE(propagations, 0, residual_edge, SOL_MIR_OPERATION_NONE);
    MUTATE(arithmetic, 0, opcode, (SolMirOperationOpcode)99);
    MUTATE(arithmetic, 0, instruction, SOL_MIR_OPERATION_NONE);
    size_t binary_operation = 0;
    while (binary_operation < o->arithmetic_count
        && o->arithmetic[binary_operation].left == SOL_MIR_MATERIALIZED_NONE)
        ++binary_operation;
    CHECK(binary_operation < o->arithmetic_count);
    if (binary_operation < o->arithmetic_count)
        MUTATE(arithmetic, binary_operation, left, SOL_MIR_OPERATION_NONE);
    MUTATE(arithmetic, 0, failures, o->arithmetic[0].failures ^ 1u);
    MUTATE(equality_nodes, 0, kind, (SolMirOperationEqualityKind)99);
    MUTATE(equality_children, 0, recipe, SOL_MIR_RECIPE_NONE);
    MUTATE(equality_children, 0, field_layout, SOL_MIR_OPERATION_NONE);
    MUTATE(snapshots, 0, slot, 2);
    MUTATE(snapshots, 0, context, SOL_MIR_OPERATION_NONE);
    MUTATE(snapshots, 0, path.count, o->snapshots[0].path.count + 1);
    MUTATE(predicates, 0, state, (SolMirOperationPredicateState)99);
    MUTATE(predicates, 0, context, SOL_MIR_OPERATION_NONE);
    MUTATE(predicates, 0, output_recipe, SOL_MIR_RECIPE_NONE);
    MUTATE(provenance, 0, source_expression,
        o->provenance[0].source_expression ^ 1u);
    size_t capability = 0;
    while (capability < o->constructor_count
        && o->constructors[capability].kind
            != SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY) ++capability;
    if (capability < o->constructor_count) {
        MUTATE(constructors, capability, capability_rule,
            SOL_MIR_OPERATION_CAPABILITY_NONE);
        MUTATE(constructors, capability, capability_source_operand,
            SOL_MIR_OPERATION_NONE);
        MUTATE(constructors, capability, inherited_root,
            SOL_MIR_MATERIALIZED_NONE);
    }
#undef MUTATE
    size_t saved_usage = o->usage.constructors; ++o->usage.constructors;
    CHECK(!sol_mir_operations_validate(o, NULL)); o->usage.constructors = saved_usage;
    saved_usage = o->usage.validation_work; ++o->usage.validation_work;
    CHECK(!sol_mir_operations_validate(o, NULL));
    o->usage.validation_work = saved_usage;
    if (saved_usage != 0) {
        --o->usage.validation_work;
        CHECK(!sol_mir_operations_validate(o, NULL));
        o->usage.validation_work = saved_usage;
    }
    size_t saved_validation_limit = o->limits.max_validation_work;
    o->limits.max_validation_work = local_work;
    CHECK(!sol_mir_operations_validate(o, NULL));
    o->limits.max_validation_work = saved_validation_limit;
    size_t saved_validation_scratch_limit
        = o->limits.max_validation_scratch_bytes;
    o->limits.max_validation_scratch_bytes = local_scratch;
    CHECK(!sol_mir_operations_validate(o, NULL));
    o->limits.max_validation_scratch_bytes
        = saved_validation_scratch_limit;
    saved_usage = o->usage.build_work;
    CHECK(saved_usage < o->limits.max_build_work);
    if (saved_usage < o->limits.max_build_work) {
        ++o->usage.build_work;
        CHECK(!sol_mir_operations_validate(o, NULL));
        o->usage.build_work = saved_usage;
    }
    CHECK(saved_usage != 0);
    if (saved_usage != 0) {
        --o->usage.build_work;
        CHECK(!sol_mir_operations_validate(o, NULL));
        o->usage.build_work = saved_usage;
    }
    size_t independently_expected = 0;
    CHECK(sol_mir_operations_internal_expected_build_work(o,
        &independently_expected) && independently_expected == saved_usage);
    SolMirOperationConstructPlan saved_constructor = o->constructors[0];
    o->constructors[0].result_recipe = SOL_MIR_RECIPE_NONE;
    size_t expected_after_output_mutation = 0;
    CHECK(sol_mir_operations_internal_expected_build_work(o,
        &expected_after_output_mutation)
        && expected_after_output_mutation == independently_expected);
    o->constructors[0] = saved_constructor;
    size_t saved_capacity = o->constructor_capacity; ++o->constructor_capacity;
    CHECK(!sol_mir_operations_validate(o, NULL)); o->constructor_capacity = saved_capacity;
    SolMirOperationConstructPlan *saved_pointer = o->constructors;
    o->constructors = (SolMirOperationConstructPlan *)(void *)o->access_plans;
    CHECK(!sol_mir_operations_validate(o, NULL)); o->constructors = saved_pointer;
#define REJECT_HEADER(singular, usage_field) do { \
    if (o->singular##_count != 0) { \
        size_t saved_count = o->singular##_count; --o->singular##_count; \
        CHECK(!sol_mir_operations_validate(o, NULL)); \
        o->singular##_count = saved_count; \
        size_t saved_arena_capacity = o->singular##_capacity; \
        ++o->singular##_capacity; CHECK(!sol_mir_operations_validate(o, NULL)); \
        o->singular##_capacity = saved_arena_capacity; \
        size_t saved_arena_usage = o->usage.usage_field; \
        ++o->usage.usage_field; CHECK(!sol_mir_operations_validate(o, NULL)); \
        o->usage.usage_field = saved_arena_usage; \
    } \
} while (0)
    REJECT_HEADER(access_plan, access_plans); REJECT_HEADER(access_step, access_steps);
    REJECT_HEADER(constructor, constructors);
    REJECT_HEADER(construct_operand, construct_operands);
    REJECT_HEADER(pattern_test, pattern_tests);
    REJECT_HEADER(pattern_extraction, pattern_extractions);
    REJECT_HEADER(pattern_node, pattern_nodes); REJECT_HEADER(path_step, path_steps);
    REJECT_HEADER(propagation, propagations); REJECT_HEADER(arithmetic, arithmetic);
    REJECT_HEADER(equality_node, equality_nodes);
    REJECT_HEADER(equality_child, equality_children);
    REJECT_HEADER(snapshot, snapshots); REJECT_HEADER(predicate, predicates);
    REJECT_HEADER(provenance, provenance);
#undef REJECT_HEADER
#define ALIAS_ACCESS(pointer) do { \
    SolMirOperationAccessPlan *saved_alias = o->access_plans; \
    o->access_plans = (SolMirOperationAccessPlan *)(void *)(pointer); \
    CHECK(!sol_mir_operations_validate(o, NULL)); o->access_plans = saved_alias; \
} while (0)
    ALIAS_ACCESS(p.layout.types); ALIAS_ACCESS(p.representation.recipes);
    ALIAS_ACCESS(p.materialization.type_ids); ALIAS_ACCESS(p.materialization.edges);
    ALIAS_ACCESS(p.materialization.semantic_sites); ALIAS_ACCESS(p.plan.types);
    ALIAS_ACCESS(p.program.templates); ALIAS_ACCESS(p.program.templates[0].mir.values);
    ALIAS_ACCESS(c.ir.patterns); ALIAS_ACCESS(c.ir.source_bytes);
#undef ALIAS_ACCESS
    char *saved_source_path = c.ir.source_path;
    c.ir.source_path = (char *)(void *)o->access_plans;
    CHECK(!sol_mir_operations_validate(o, NULL));
    c.ir.source_path = saved_source_path;
    FILE *stream = tmpfile(); CHECK(stream != NULL);
    if (stream != NULL) {
        SolMirRecipeId saved = o->constructors[0].result_recipe;
        o->constructors[0].result_recipe = SOL_MIR_RECIPE_NONE;
        CHECK(!sol_mir_operations_render(stream, o) && ftell(stream) == 0);
        o->constructors[0].result_recipe = saved; fclose(stream);
    }
    CHECK(sol_mir_operations_validate(o, NULL));
    SolMirOperationsLimits partial = exact;
    partial.max_roots = 0;
    request.limits = &partial;
    CHECK(sol_mir_operations_build(&request, &repeated, &p.diagnostics)
        == SOL_MIR_OPERATIONS_BUILD_INVALID_ARGUMENT && repeated.layout == NULL);
    size_t saved_limit = o->limits.max_roots; o->limits.max_roots = 0;
    CHECK(!sol_mir_operations_validate(o, NULL)); o->limits.max_roots = saved_limit;
    pipeline_free(&p); free_e6(&c, &package);
}

static void test_cross_recipe_result_propagation(void) {
    static const char source[] =
        "module propagation\n"
        "function adapt(value: Result<Int64, Text>) -> Result<Bool, Text> { "
        "let number = value? return ok(number > 0) }\n";
    Compilation c; bool compiled = compile_text(&c, source); CHECK(compiled);
    if (!compiled) { free_text(&c); return; }
    SolMirProgramRoot root = {callable(&c.ir, "adapt", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline p; pipeline_init(&p);
    bool built = build_pipeline(&c.ir, &root, 1, NULL, 0, &p, NULL);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &p.diagnostics);
    CHECK(built && p.operations.propagation_count == 1);
    if (built) {
        SolMirOperationPropagationPlan *plan = &p.operations.propagations[0];
        CHECK(plan->source_recipe != plan->residual_recipe
            && plan->source_residual_field_layout
                != plan->destination_residual_field_layout
            && plan->source_residual_field_offset
                == p.layout.fields[plan->source_residual_field_layout].offset
            && plan->destination_residual_field_offset
                == p.layout.fields[plan->destination_residual_field_layout].offset
            && p.representation.fields[plan->source_residual_field_layout].type
                == p.representation.fields[
                    plan->destination_residual_field_layout].type);
        size_t rendered_length = 0;
        char *rendered = render(&p.operations, &rendered_length);
        CHECK(rendered != NULL && rendered_length != 0
            && strstr(rendered, "source_residual=") != NULL
            && strstr(rendered, "destination_residual=") != NULL);
        free(rendered);
#define MUTATE_PROP(member, value) do { \
    SolMirOperationPropagationPlan saved = *plan; plan->member = (value); \
    CHECK(!sol_mir_operations_validate(&p.operations, NULL)); *plan = saved; \
} while (0)
        MUTATE_PROP(source_residual_field_layout, SOL_MIR_OPERATION_NONE);
        MUTATE_PROP(source_residual_field_offset,
            plan->source_residual_field_offset ^ UINT64_C(1));
        MUTATE_PROP(destination_residual_field_layout, SOL_MIR_OPERATION_NONE);
        MUTATE_PROP(destination_residual_field_offset,
            plan->destination_residual_field_offset ^ UINT64_C(1));
#undef MUTATE_PROP
    }
    pipeline_free(&p); free_text(&c);
}

static void test_capability_constructor_plan(void) {
    static const char source[] =
        "module capability_constructor\n"
        "capability Clock { function now() -> Int64 effects { clock.read<Self> } }\n"
        "capability Wrapped derives_from private_source: capability Clock {}\n"
        "function wrap(clock: capability Clock) -> capability Wrapped effects { pure } "
        "{ return Wrapped { private_source = clock } }\n";
    Compilation c; bool compiled = compile_text(&c, source); CHECK(compiled);
    if (!compiled) { free_text(&c); return; }
    SolMirProgramRoot root = {callable(&c.ir, "wrap", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline p; pipeline_init(&p);
    bool built = build_pipeline(&c.ir, &root, 1, NULL, 0, &p, NULL);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &p.diagnostics);
    CHECK(built && p.operations.constructor_count == 1);
    if (built) {
        SolMirOperationConstructPlan *plan = &p.operations.constructors[0];
        CHECK(plan->kind == SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY
            && plan->capability_rule != SOL_MIR_OPERATION_CAPABILITY_NONE
            && plan->capability_source_operand == 0
            && plan->inherited_root < p.materialization.local_count);
#define MUTATE_CAP(member, value) do { SolMirOperationConstructPlan saved = *plan; \
    plan->member = (value); CHECK(!sol_mir_operations_validate(&p.operations, NULL)); \
    *plan = saved; } while (0)
        MUTATE_CAP(capability_rule, SOL_MIR_OPERATION_CAPABILITY_NONE);
        MUTATE_CAP(capability_source_operand, SOL_MIR_OPERATION_NONE);
        MUTATE_CAP(inherited_root, SOL_MIR_MATERIALIZED_NONE);
#undef MUTATE_CAP
    }
    pipeline_free(&p); free_text(&c);
}

static void test_recursive_equality_graph(void) {
    static const char source[] =
        "module recursive_equality\n"
        "record Node { next: Option<Node> }\n"
        "function same(left: Node, right: Node) -> Bool { return left == right }\n";
    Compilation c; bool compiled = compile_text(&c, source); CHECK(compiled);
    if (!compiled) { free_text(&c); return; }
    SolMirProgramRoot root = {callable(&c.ir, "same", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline p; pipeline_init(&p);
    bool built = build_pipeline(&c.ir, &root, 1, NULL, 0, &p, NULL);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &p.diagnostics);
    CHECK(built && p.operations.arithmetic_count == 1
        && p.operations.equality_node_count >= 2
        && p.operations.equality_node_count < p.representation.recipe_count);
    if (built) {
        CHECK(sol_mir_operations_validate(&p.operations, NULL));
        SolMirOperationEqualityChild saved = p.operations.equality_children[0];
        p.operations.equality_children[0].variant_layout = 0;
        CHECK(!sol_mir_operations_validate(&p.operations, NULL));
        p.operations.equality_children[0] = saved;
    }
    pipeline_free(&p); free_text(&c);
}

static void test_source_search_work_is_not_an_arena_count(void) {
    static const char *sources[] = {
        "module first_variant\n"
        "enum Choice { first, second }\n"
        "function make() -> Choice { return Choice.first }\n",
        "module second_variant\n"
        "enum Choice { first, second }\n"
        "function make() -> Choice { return Choice.second }\n",
    };
    Compilation compilations[2]; Pipeline pipelines[2];
    bool built[2] = {false, false};
    for (size_t i = 0; i < 2; ++i) {
        bool compiled = compile_text(&compilations[i], sources[i]);
        CHECK(compiled); pipeline_init(&pipelines[i]);
        if (!compiled) continue;
        SolMirProgramRoot root = {callable(&compilations[i].ir, "make",
            SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
        built[i] = build_pipeline(&compilations[i].ir, &root, 1, NULL, 0,
            &pipelines[i], NULL);
        CHECK(built[i]);
    }
    if (built[0] && built[1]) {
#define SAME_ARENA(member, type, singular) \
        CHECK(pipelines[0].operations.singular##_count \
            == pipelines[1].operations.singular##_count);
        SOL_MIR_OPERATIONS_ARENAS(SAME_ARENA)
#undef SAME_ARENA
        CHECK(pipelines[0].operations.usage.build_work
            != pipelines[1].operations.usage.build_work);
    }
    for (size_t i = 0; i < 2; ++i) {
        pipeline_free(&pipelines[i]);
        free_text(&compilations[i]);
    }
}

static void test_handlers_and_unresolved_callable_rejection(void) {
    static const char handlers[] =
        "module handlers\n"
        "capability Source { function read(value: Int64) -> Int64 effects { service.read<Self> } }\n"
        "capability Provider { function read(value: Int64) -> Int64 effects { pure } }\n"
        "function root(source: capability Source, first: capability Provider, "
        "second: capability Provider) -> Int64 { return handle service.read<source> "
        "with first { handle service.read<source> with second { source.read(1) } } }\n";
    Compilation c; CHECK(compile_text(&c, handlers));
    SolIrCallableId imports[2] = {SOL_IR_NONE, SOL_IR_NONE};
    for (size_t i = 0; i < c.ir.callable_count; ++i) {
        if (c.ir.callables[i].kind != SOL_IR_CALLABLE_CAPABILITY
            || strcmp(c.ir.callables[i].name, "read") != 0) continue;
        imports[imports[0] == SOL_IR_NONE ? 0 : 1] = i;
    }
    SolMirProgramRoot root = {callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline p; pipeline_init(&p);
    bool built = build_pipeline(&c.ir, &root, 1, imports, 2, &p, NULL);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &p.diagnostics);
    CHECK(built && p.operations.handler_count == 2);
    if (built) {
        size_t roots = 0, nested = 0;
        for (size_t i = 0; i < p.operations.handler_count; ++i) {
            roots += p.operations.handlers[i].frame_parent == SOL_MIR_OPERATION_NONE;
            nested += p.operations.handlers[i].frame_parent < p.operations.handler_count;
            CHECK(p.operations.handlers[i].root_match
                == SOL_MIR_OPERATION_ROOT_TOKEN_EQUAL);
        }
        CHECK(roots == 1 && nested == 1);
        SolMirOperationRootMatchRule saved = p.operations.handlers[0].root_match;
        p.operations.handlers[0].root_match = (SolMirOperationRootMatchRule)99;
        CHECK(!sol_mir_operations_validate(&p.operations, NULL));
        p.operations.handlers[0].root_match = saved;
        SolMirOperationsLimits exact = exact_limits(&p.operations);
        SolMirOperations limited; sol_mir_operations_init(&limited);
        SolMirOperationsBuildRequest request = {&p.layout, &exact};
        CHECK(sol_mir_operations_build(&request, &limited, &p.diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_SUCCEEDED);
        sol_mir_operations_free(&limited);
        --exact.max_handlers;
        CHECK(sol_mir_operations_build(&request, &limited, &p.diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED
            && limited.layout == NULL);
        exact = exact_limits(&p.operations);
        --exact.max_recipe_ids;
        CHECK(sol_mir_operations_build(&request, &limited, &p.diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED
            && limited.layout == NULL);
    }
    pipeline_free(&p); free_text(&c);

    static const char exact_callable[] =
        "module exact_callable\n"
        "capability Base { function choose(value: Int64) -> Bool effects { pure } }\n"
        "function callback(value: Int64) -> Bool effects { pure } { return true }\n"
        "function apply(callback: function(Int64) -> Bool effects { pure }) -> Bool "
        "effects { pure } { return true }\n"
        "function root(base: capability Base) -> Bool effects { pure } "
        "requires { apply(callback) && apply(base.choose) } "
        "{ return base.choose(1) }\n";
    CHECK(compile_text(&c, exact_callable));
    root = (SolMirProgramRoot){callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    pipeline_init(&p);
    SolIrCallableId choose = callable(&c.ir, "choose", SOL_IR_CALLABLE_CAPABILITY);
    built = build_pipeline(&c.ir, &root, 1, &choose, 1, &p, NULL);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &p.diagnostics);
    CHECK(built && p.operations.callable_count == 2
        && p.operations.root_count == 1);
    if (built) {
        size_t exact = SOL_MIR_OPERATION_NONE, bound = SOL_MIR_OPERATION_NONE;
        for (size_t i = 0; i < p.operations.callable_count; ++i) {
            if (p.operations.callables[i].kind
                    == SOL_MIR_CALLABLE_PRODUCER_EXACT_FUNCTION) exact = i;
            else if (p.operations.callables[i].kind
                    == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION) bound = i;
        }
        CHECK(exact < p.operations.callable_count
            && p.operations.callables[exact].capture_kind
                == SOL_MIR_OPERATION_CAPTURE_NONE
            && p.operations.callables[exact].capture_access == SOL_MIR_OPERATION_NONE
            && p.operations.callables[exact].capture_recipe == SOL_MIR_RECIPE_NONE
            && p.operations.callables[exact].roots.count == 0);
        CHECK(bound < p.operations.callable_count
            && p.operations.callables[bound].capture_kind
                == SOL_MIR_OPERATION_CAPTURE_PLACE
            && p.operations.callables[bound].roots.count == 1);
        const SolMirOperationCallablePlan *producer = &p.operations.callables[0];
        SolMirMaterializedTargetKind saved = producer->target_kind;
        p.operations.callables[0].target_kind = (SolMirMaterializedTargetKind)99;
        CHECK(!sol_mir_operations_validate(&p.operations, NULL));
        p.operations.callables[0].target_kind = saved;
        SolMirOperationsLimits exact_limits_value = exact_limits(&p.operations);
        SolMirOperations limited; sol_mir_operations_init(&limited);
        SolMirOperationsBuildRequest limits_request
            = {&p.layout, &exact_limits_value};
        CHECK(sol_mir_operations_build(&limits_request, &limited, &p.diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_SUCCEEDED);
        sol_mir_operations_free(&limited);
        --exact_limits_value.max_callables;
        CHECK(sol_mir_operations_build(&limits_request, &limited, &p.diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED
            && limited.layout == NULL);
        exact_limits_value = exact_limits(&p.operations);
        exact_limits_value.max_roots = 0;
        CHECK(sol_mir_operations_build(&limits_request, &limited, &p.diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_INVALID_ARGUMENT
            && limited.layout == NULL);
    }
    pipeline_free(&p); free_text(&c);

    static const char unresolved[] =
        "module unresolved\n"
        "capability Base { function choose(value: Int64) -> Bool effects { pure } }\n"
        "function preserve(value: capability Base) -> capability Base effects { pure } "
        "authority { result derives_from value } { return value }\n"
        "function apply(callback: function(Int64) -> Bool effects { pure }) -> Bool "
        "effects { pure } { return true }\n"
        "function root(base: capability Base) -> Bool effects { pure } "
        "requires { apply(preserve(base).choose) } { return true }\n";
    CHECK(compile_text(&c, unresolved));
    SolIrCallableId import = callable(&c.ir, "choose", SOL_IR_CALLABLE_CAPABILITY);
    root = (SolMirProgramRoot){callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    pipeline_init(&p);
    SolMirProgramBuildRequest a = {&c.ir, &root, 1, &import, 1, NULL};
    SolMirPlanBuildRequest b = {&p.program, NULL};
    SolMirMaterializeBuildRequest d = {&p.plan, NULL};
    SolMirRepresentationBuildRequest e = {&p.materialization, NULL};
    SolMirTargetDescriptor wasm = sol_mir_target_wasm32();
    SolMirLayoutBuildRequest f = {&p.representation, &wasm, NULL};
    bool layout = sol_mir_program_build(&a, &p.program, &p.diagnostics)
            == SOL_MIR_PROGRAM_BUILD_SUCCEEDED
        && sol_mir_plan_build(&b, &p.plan, &p.diagnostics) == SOL_MIR_PLAN_BUILD_SUCCEEDED
        && sol_mir_materialize_build(&d, &p.materialization, &p.diagnostics)
            == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED
        && sol_mir_representation_build(&e, &p.representation, &p.diagnostics)
            == SOL_MIR_REPRESENTATION_BUILD_SUCCEEDED
        && sol_mir_layout_build(&f, &p.layout, &p.diagnostics)
            == SOL_MIR_LAYOUT_BUILD_SUCCEEDED;
    CHECK(layout);
    SolMirOperationsBuildRequest request = {&p.layout, NULL};
    CHECK(layout && sol_mir_operations_build(&request, &p.operations, &p.diagnostics)
        == SOL_MIR_OPERATIONS_BUILD_UNSUPPORTED);
    CHECK(p.operations.layout == NULL);
    pipeline_free(&p); free_text(&c);
}

int main(void) {
    SolMirOperations empty;
    memset(&empty, 0xa5, sizeof(empty)); sol_mir_operations_init(&empty);
    CHECK(empty.layout == NULL && !sol_mir_operations_validate(&empty, NULL));
    SolMirOperationsLimits defaults = sol_mir_operations_default_limits();
    CHECK(defaults.max_access_plans != 0 && defaults.max_validation_work != 0);
    sol_mir_operations_free(&empty);
    test_e6_operations();
    test_cross_recipe_result_propagation();
    test_capability_constructor_plan();
    test_recursive_equality_graph();
    test_source_search_work_is_not_an_arena_count();
    test_handlers_and_unresolved_callable_rejection();
    if (failures != 0) {
        fprintf(stderr, "%d MIR operations test(s) failed\n", failures); return 1;
    }
    printf("MIR operations tests passed\n"); return 0;
}
