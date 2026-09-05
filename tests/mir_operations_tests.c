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
        .max_predicate_bodies = NONZERO(o->usage.predicate_bodies),
        .max_predicate_blocks = NONZERO(o->usage.predicate_blocks),
        .max_predicate_inputs = NONZERO(o->usage.predicate_inputs),
        .max_predicate_values = NONZERO(o->usage.predicate_values),
        .max_predicate_instructions = NONZERO(o->usage.predicate_instructions),
        .max_import_envelopes = NONZERO(o->usage.import_envelopes),
        .max_import_contract_references
            = NONZERO(o->usage.import_contract_references),
        .max_import_snapshots = NONZERO(o->usage.import_snapshots),
        .max_literal_bytes = NONZERO(o->usage.literal_bytes),
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
        CHECK(o->predicates[i].body < o->predicate_body_count);
    }
    CHECK(contract == 3 && refinement == 1);
    CHECK(o->predicate_body_count == 4 && o->predicate_block_count == 4
        && o->predicate_input_count == 4 && o->predicate_value_count == 10
        && o->predicate_instruction_count == 6
        && o->import_envelope_count == 4);
    size_t predicate_constants = 0, predicate_comparisons = 0;
    for (size_t i = 0; i < o->predicate_instruction_count; ++i) {
        predicate_constants += o->predicate_instructions[i].kind
                == SOL_MIR_PREDICATE_INST_I64
            || o->predicate_instructions[i].kind == SOL_MIR_PREDICATE_INST_BOOL;
        predicate_comparisons += o->predicate_instructions[i].kind
                == SOL_MIR_PREDICATE_INST_BINARY
            && o->predicate_instructions[i].opcode >= SOL_MIR_OPERATION_I64_LT
            && o->predicate_instructions[i].opcode <= SOL_MIR_OPERATION_VALUE_NE;
    }
    CHECK(predicate_constants == 3 && predicate_comparisons == 3);
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
    LESS(max_predicate_bodies); LESS(max_predicate_blocks);
    LESS(max_predicate_inputs); LESS(max_predicate_values);
    LESS(max_predicate_instructions); LESS(max_import_envelopes);
    LESS(max_import_contract_references); LESS(max_import_snapshots);
    LESS(max_literal_bytes);
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
    MUTATE(predicates, 0, body, SOL_MIR_OPERATION_NONE);
    MUTATE(predicates, 0, context, SOL_MIR_OPERATION_NONE);
    MUTATE(predicates, 0, output_recipe, SOL_MIR_RECIPE_NONE);
    MUTATE(predicate_bodies, 0, entry, SOL_MIR_OPERATION_NONE);
    MUTATE(predicate_blocks, 0, terminator.value, SOL_MIR_OPERATION_NONE);
    MUTATE(predicate_values, 0, recipe, SOL_MIR_RECIPE_NONE);
    MUTATE(predicate_instructions, 0, result, SOL_MIR_OPERATION_NONE);
    MUTATE(import_envelopes, 0, import, SOL_MIR_OPERATION_NONE);
    MUTATE(import_envelopes, 0, host_invoke, false);
    size_t integer_instruction = 0;
    while (integer_instruction < o->predicate_instruction_count
        && o->predicate_instructions[integer_instruction].kind
            != SOL_MIR_PREDICATE_INST_I64) ++integer_instruction;
    CHECK(integer_instruction < o->predicate_instruction_count);
    if (integer_instruction < o->predicate_instruction_count)
        MUTATE(predicate_instructions, integer_instruction, integer, 42);
    MUTATE(predicate_inputs, 0, ordinal, o->predicate_inputs[0].ordinal + 1);
    MUTATE(predicate_inputs, 0, kind, SOL_MIR_PREDICATE_INPUT_SNAPSHOT);
    MUTATE(predicate_inputs, 0, access, SOL_ACCESS_SHARED);
    size_t binary_predicate = 0;
    while (binary_predicate < o->predicate_instruction_count
        && o->predicate_instructions[binary_predicate].kind
            != SOL_MIR_PREDICATE_INST_BINARY) ++binary_predicate;
    CHECK(binary_predicate < o->predicate_instruction_count);
    if (binary_predicate < o->predicate_instruction_count) {
        MUTATE(predicate_instructions, binary_predicate, opcode,
            SOL_MIR_OPERATION_I64_ADD);
        MUTATE(predicate_instructions, binary_predicate, failures,
            SOL_MIR_OPERATION_FAILURE_OVERFLOW);
    }
    MUTATE(predicate_bodies, 0, phase, SOL_CONTRACT_ENSURES);
    MUTATE(predicate_bodies, 0, outcome, SOL_CONTRACT_OUTCOME_FAILURE);
    MUTATE(predicate_bodies, 0, context, o->predicate_bodies[1].context);
    MUTATE(predicate_bodies, 0, owner_kind, SOL_MIR_PREDICATE_OWNER_IMPORT);
    if (o->predicate_body_count > 1) {
        SolMirPredicateBody first_body = o->predicate_bodies[0];
        o->predicate_bodies[0] = o->predicate_bodies[1];
        o->predicate_bodies[1] = first_body;
        CHECK(!sol_mir_operations_validate(o, NULL));
        o->predicate_bodies[1] = o->predicate_bodies[0];
        o->predicate_bodies[0] = first_body;
    }
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
    REJECT_HEADER(predicate_body, predicate_bodies);
    REJECT_HEADER(predicate_block, predicate_blocks);
    REJECT_HEADER(predicate_input, predicate_inputs);
    REJECT_HEADER(predicate_value, predicate_values);
    REJECT_HEADER(predicate_instruction, predicate_instructions);
    REJECT_HEADER(import_envelope, import_envelopes);
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
        "requires { apply(base.choose) } "
        "{ return base.choose(1) }\n";
    CHECK(compile_text(&c, exact_callable));
    root = (SolMirProgramRoot){callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    pipeline_init(&p);
    SolIrCallableId choose = callable(&c.ir, "choose", SOL_IR_CALLABLE_CAPABILITY);
    built = build_pipeline(&c.ir, &root, 1, &choose, 1, &p, NULL);
    CHECK(!built && p.operations.layout == NULL
        && p.representation.callable_producer_count == 1);
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

static void test_bodyless_import_contract(void) {
    static const char source[] =
        "module import_contract\n"
        "capability ContractHost { function echo(value: Int64) -> Int64 "
        "effects { pure } requires { value > 0 } "
        "ensures { result >= old(value) result >= old(value) } }\n"
        "function root(host: capability ContractHost) -> Int64 effects { pure } "
        "{ return host.echo(7) }\n";
    Compilation c; bool compiled = compile_text(&c, source); CHECK(compiled);
    if (!compiled) {
        sol_diagnostics_render_human(stderr, &c.source, &c.diagnostics);
        free_text(&c); return;
    }
    SolIrCallableId echo = callable(&c.ir, "echo", SOL_IR_CALLABLE_CAPABILITY);
    SolMirProgramRoot root = {callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline p; pipeline_init(&p);
    bool built = build_pipeline(&c.ir, &root, 1, &echo, 1, &p, NULL);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &p.diagnostics);
    CHECK(built);
    if (built) {
        CHECK(p.plan.import_count == 1 && p.plan.imports[0].contexts.count == 3);
        CHECK(p.materialization.imports[0].contexts.count == 3
            && p.materialization.imports[0].overlays.count != 0);
        CHECK(p.operations.import_envelope_count == 1
            && p.operations.import_envelopes[0].requires.count == 1
            && p.operations.import_envelopes[0].ensures.count == 2
            && p.operations.import_envelopes[0].snapshots.count == 2
            && p.operations.predicate_body_count == 3);
        CHECK(p.operations.import_snapshots[0].slot == 0
            && p.operations.import_snapshots[1].slot == 1);
        CHECK(sol_mir_operations_validate(&p.operations, NULL));
        SolMirOperationsLimits exact = exact_limits(&p.operations);
        SolMirOperations limited; sol_mir_operations_init(&limited);
        SolMirOperationsBuildRequest request = {&p.layout, &exact};
        CHECK(sol_mir_operations_build(&request, &limited, &p.diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_SUCCEEDED);
        sol_mir_operations_free(&limited);
        --exact.max_import_snapshots;
        CHECK(sol_mir_operations_build(&request, &limited, &p.diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED);
        exact = exact_limits(&p.operations);
        --exact.max_import_contract_references;
        CHECK(sol_mir_operations_build(&request, &limited, &p.diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED);
        SolMirImportSnapshotCapture saved = p.operations.import_snapshots[0];
        p.operations.import_snapshots[0].slot = 1;
        CHECK(!sol_mir_operations_validate(&p.operations, NULL));
        p.operations.import_snapshots[0] = saved;
        SolMirOperationProvenance provenance
            = p.operations.provenance[saved.provenance];
        p.operations.provenance[saved.provenance].source_snapshot
            = p.operations.provenance[
                p.operations.import_snapshots[1].provenance].source_snapshot;
        CHECK(!sol_mir_operations_validate(&p.operations, NULL));
        p.operations.provenance[saved.provenance] = provenance;
    }
    pipeline_free(&p); free_text(&c);
}

static void test_p2_6b1_predicate_rejections(void) {
    static const char *sources[] = {
        "module short_circuit\nfunction root(value: Int64) -> Bool effects { pure } "
            "requires { false && (1 / 0 > value) } { return true }\n",
        "module projected\nrecord Box { value: Int64 }\n"
            "function root(box: Box) -> Bool effects { pure } "
            "requires { box.value > 0 } { return true }\n",
        "module projected_snapshot\nrecord Box { value: Int64 }\n"
            "function root(box: Box) -> Int64 effects { pure } "
            "ensures { result >= old(box.value) } { return box.value }\n",
        "module direct_call\nfunction helper(value: Int64) -> Bool effects { pure } "
            "{ return value > 0 }\nfunction root(value: Int64) -> Bool effects { pure } "
            "requires { helper(value) } { return true }\n",
        "module function_value\nfunction helper(value: Int64) -> Bool effects { pure } "
            "{ return value > 0 }\nfunction apply(callback: function(Int64) -> Bool "
            "effects { pure }) -> Bool effects { pure } { return callback(1) }\n"
            "function root() -> Bool effects { pure } "
            "requires { apply(helper) } { return true }\n",
        "module aggregate\nrecord Box { value: Int64 }\n"
            "function root() -> Bool effects { pure } "
            "requires { Box { value = 1 } == Box { value = 1 } } { return true }\n",
        "module conditional\nfunction root() -> Bool effects { pure } "
            "requires { if true { true } else { false } } { return true }\n",
        "module matching\nfunction root(value: Bool) -> Bool effects { pure } "
            "requires { match value { true => true false => false } } { return true }\n",
        "module local_block\nfunction root() -> Bool effects { pure } "
            "requires { { let value = true value } } { return true }\n",
        "module nested_refined\ntype Positive = refined Int64 where self > 0\n"
            "function root(value: Int64) -> Bool effects { pure } "
            "requires { Positive(value) == Positive(value) } { return true }\n",
    };
    for (size_t i = 0; i < sizeof(sources) / sizeof(*sources); ++i) {
        Compilation c; bool compiled = compile_text(&c, sources[i]); CHECK(compiled);
        if (!compiled) { free_text(&c); continue; }
        SolMirProgramRoot root = {callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
            SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
        Pipeline p; pipeline_init(&p);
        bool built = build_pipeline(&c.ir, &root, 1, NULL, 0, &p, NULL);
        CHECK(!built && p.operations.layout == NULL);
        pipeline_free(&p); free_text(&c);
    }
}

static void test_predicate_literal_authentication_and_rendering(void) {
    static const char *sources[] = {
        "module text_a\nfunction root() -> Bool effects { pure } "
            "requires { \"a\" == \"a\" } { return true }\n",
        "module text_b\nfunction root() -> Bool effects { pure } "
            "requires { \"b\" == \"b\" } { return true }\n",
    };
    Compilation c[2]; Pipeline p[2]; char *text[2] = {NULL, NULL};
    size_t length[2] = {0, 0};
    for (size_t i = 0; i < 2; ++i) {
        bool compiled = compile_text(&c[i], sources[i]); CHECK(compiled);
        pipeline_init(&p[i]);
        if (!compiled) continue;
        SolMirProgramRoot root = {callable(&c[i].ir, "root",
            SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
        bool built = build_pipeline(&c[i].ir, &root, 1, NULL, 0, &p[i], NULL);
        CHECK(built && p[i].operations.literal_byte_count == 2);
        if (built) text[i] = render(&p[i].operations, &length[i]);
    }
    CHECK(text[0] != NULL && text[1] != NULL
        && (length[0] != length[1] || memcmp(text[0], text[1], length[0]) != 0)
        && strstr(text[0], "predicate_literal_bytes=6161") != NULL);
    if (p[0].operations.literal_byte_count != 0) {
        char saved = p[0].operations.literal_bytes[0];
        p[0].operations.literal_bytes[0] = 'z';
        CHECK(!sol_mir_operations_validate(&p[0].operations, NULL));
        p[0].operations.literal_bytes[0] = saved;
        SolMirOperationsLimits exact = exact_limits(&p[0].operations);
        SolMirOperations limited; sol_mir_operations_init(&limited);
        SolMirOperationsBuildRequest request = {&p[0].layout, &exact};
        --exact.max_literal_bytes;
        CHECK(sol_mir_operations_build(&request, &limited, &p[0].diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED);
    }
    for (size_t i = 0; i < 2; ++i) {
        free(text[i]); pipeline_free(&p[i]); free_text(&c[i]);
    }
}

static void test_import_contract_helper_is_retained_then_rejected(void) {
    static const char source[] =
        "module import_helper\n"
        "function positive(value: Int64) -> Bool effects { pure } "
        "{ return value > 0 }\n"
        "capability ContractHost { function echo(value: Int64) -> Int64 "
        "effects { pure } requires { positive(value) } }\n"
        "function root(host: capability ContractHost) -> Int64 effects { pure } "
        "{ return host.echo(7) }\n";
    Compilation c; bool compiled = compile_text(&c, source); CHECK(compiled);
    if (!compiled) { free_text(&c); return; }
    SolIrCallableId echo = callable(&c.ir, "echo", SOL_IR_CALLABLE_CAPABILITY);
    SolMirProgramRoot root = {callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline p; pipeline_init(&p);
    SolMirProgramBuildRequest a = {&c.ir, &root, 1, &echo, 1, NULL};
    SolMirPlanBuildRequest b = {&p.program, NULL};
    SolMirMaterializeBuildRequest d = {&p.plan, NULL};
    CHECK(sol_mir_program_build(&a, &p.program, &p.diagnostics)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(sol_mir_plan_build(&b, &p.plan, &p.diagnostics)
        == SOL_MIR_PLAN_BUILD_SUCCEEDED);
    SolMirMaterializeBuildOutcome materialized
        = sol_mir_materialize_build(&d, &p.materialization, &p.diagnostics);
    if (materialized != SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED)
        sol_diagnostics_render_human(stderr, &c.source, &p.diagnostics);
    CHECK(materialized == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED);
    CHECK(sol_mir_plan_validate(&p.plan, NULL)
        && sol_mir_materialization_validate(&p.materialization, NULL));
    size_t import_owned = 0;
    for (size_t i = 0; i < p.materialization.binding_count; ++i) {
        const SolMirMaterializedBinding *binding = &p.materialization.bindings[i];
        if (binding->owner_kind != SOL_MIR_PLAN_DEMAND_OWNER_IMPORT) continue;
        ++import_owned;
        CHECK(binding->parent == SOL_MIR_PLAN_NONE
            && binding->parent_import < p.materialization.import_count
            && p.materialization.semantic_sites[binding->site].block
                == SOL_MIR_MATERIALIZED_NONE);
    }
    CHECK(import_owned == 1);
    SolMirRepresentationBuildRequest e = {&p.materialization, NULL};
    SolMirTargetDescriptor wasm = sol_mir_target_wasm32();
    SolMirLayoutBuildRequest f = {&p.representation, &wasm, NULL};
    CHECK(sol_mir_representation_build(&e, &p.representation, &p.diagnostics)
        == SOL_MIR_REPRESENTATION_BUILD_SUCCEEDED);
    CHECK(sol_mir_layout_build(&f, &p.layout, &p.diagnostics)
        == SOL_MIR_LAYOUT_BUILD_SUCCEEDED);
    SolMirOperationsBuildRequest g = {&p.layout, NULL};
    CHECK(sol_mir_operations_build(&g, &p.operations, &p.diagnostics)
        == SOL_MIR_OPERATIONS_BUILD_UNSUPPORTED
        && p.operations.layout == NULL);
    size_t owned_demand = SOL_MIR_OPERATION_NONE;
    for (size_t i = 0; i < p.plan.demand_count; ++i)
        if (p.plan.demands[i].owner_kind == SOL_MIR_PLAN_DEMAND_OWNER_IMPORT)
            owned_demand = i;
    CHECK(owned_demand < p.plan.demand_count);
    if (owned_demand < p.plan.demand_count) {
        SolMirPlanDemand saved = p.plan.demands[owned_demand];
        p.plan.demands[owned_demand].owner_kind
            = SOL_MIR_PLAN_DEMAND_OWNER_INSTANCE;
        CHECK(!sol_mir_plan_validate(&p.plan, NULL));
        p.plan.demands[owned_demand] = saved;
        size_t material_binding = 0;
        while (material_binding < p.materialization.binding_count
            && p.materialization.bindings[material_binding].source_demand
                != owned_demand) ++material_binding;
        CHECK(material_binding < p.materialization.binding_count);
        SolMirMaterializedBinding binding
            = p.materialization.bindings[material_binding];
        p.materialization.bindings[material_binding].owner_kind
            = SOL_MIR_PLAN_DEMAND_OWNER_ROOT;
        CHECK(!sol_mir_materialization_validate(&p.materialization, NULL));
        p.materialization.bindings[material_binding] = binding;
        p.plan.demands[owned_demand].parent_import = SOL_MIR_PLAN_NONE;
        CHECK(!sol_mir_plan_validate(&p.plan, NULL));
        p.plan.demands[owned_demand] = saved;
    }
    pipeline_free(&p); free_text(&c);
}

static void test_multiple_import_context_canonicalization(void) {
    static const char source[] =
        "module import_order\n"
        "capability Alpha { function first(value: Int64) -> Int64 effects { pure } "
            "requires { value > 0 } }\n"
        "capability Zeta { function second(value: Int64) -> Int64 effects { pure } "
            "requires { value >= 0 } }\n"
        "function root(alpha: capability Alpha, zeta: capability Zeta) -> Int64 "
            "effects { pure } { return zeta.second(2) + alpha.first(1) }\n";
    Compilation c; bool compiled = compile_text(&c, source); CHECK(compiled);
    if (!compiled) { free_text(&c); return; }
    SolIrCallableId first = callable(&c.ir, "first", SOL_IR_CALLABLE_CAPABILITY);
    SolIrCallableId second = callable(&c.ir, "second", SOL_IR_CALLABLE_CAPABILITY);
    SolIrCallableId approved[2] = {second, first};
    SolMirProgramRoot root = {callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline p; pipeline_init(&p);
    bool built = build_pipeline(&c.ir, &root, 1, approved, 2, &p, NULL);
    CHECK(built && p.plan.import_count == 2);
    if (built) {
        CHECK(p.plan.imports[0].callable < p.plan.imports[1].callable
            && p.plan.imports[0].contexts.count == 1
            && p.plan.imports[1].contexts.count == 1
            && p.plan.imports[0].contexts.offset
                + p.plan.imports[0].contexts.count
                == p.plan.imports[1].contexts.offset
            && p.plan.imports[0].typed_uses.offset
                + p.plan.imports[0].typed_uses.count
                == p.plan.imports[1].typed_uses.offset);
        SolMirPlanSlice uses = p.plan.imports[0].typed_uses;
        --p.plan.imports[0].typed_uses.count;
        CHECK(!sol_mir_plan_validate(&p.plan, NULL));
        p.plan.imports[0].typed_uses = uses;
        SolMirPlanContext context
            = p.plan.contexts[p.plan.imports[0].contexts.offset];
        p.plan.contexts[p.plan.imports[0].contexts.offset].import = 1;
        CHECK(!sol_mir_plan_validate(&p.plan, NULL));
        p.plan.contexts[p.plan.imports[0].contexts.offset] = context;
        p.plan.contexts[p.plan.imports[0].contexts.offset].target_kind
            = SOL_MIR_PLAN_TARGET_INSTANCE;
        CHECK(!sol_mir_plan_validate(&p.plan, NULL));
        p.plan.contexts[p.plan.imports[0].contexts.offset] = context;
        SolMirPlanTypedUse use
            = p.plan.typed_uses[p.plan.imports[0].typed_uses.offset];
        p.plan.typed_uses[p.plan.imports[0].typed_uses.offset].context
            = p.plan.imports[1].contexts.offset;
        CHECK(!sol_mir_plan_validate(&p.plan, NULL));
        p.plan.typed_uses[p.plan.imports[0].typed_uses.offset] = use;
        SolMirMaterializedTypeOverlay overlay
            = p.materialization.overlays[p.materialization.imports[0].overlays.offset];
        p.materialization.overlays[p.materialization.imports[0].overlays.offset].context
            = p.materialization.imports[1].contexts.offset;
        CHECK(!sol_mir_materialization_validate(&p.materialization, NULL));
        p.materialization.overlays[p.materialization.imports[0].overlays.offset]
            = overlay;
        p.materialization.overlays[p.materialization.imports[0].overlays.offset].type
            = (overlay.type + 1) % p.materialization.type_count;
        CHECK(!sol_mir_materialization_validate(&p.materialization, NULL));
        p.materialization.overlays[p.materialization.imports[0].overlays.offset]
            = overlay;
        SolMirPlanSlice contexts = p.materialization.imports[0].contexts;
        p.materialization.imports[0].contexts = p.materialization.imports[1].contexts;
        CHECK(!sol_mir_materialization_validate(&p.materialization, NULL));
        p.materialization.imports[0].contexts = contexts;
        size_t context_id = p.materialization.imports[0].contexts.offset;
        SolMirPlanContext material_context = p.materialization.contexts[context_id];
        p.materialization.contexts[context_id].import = 1;
        CHECK(!sol_mir_materialization_validate(&p.materialization, NULL));
        p.materialization.contexts[context_id] = material_context;
    }
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
    test_bodyless_import_contract();
    test_import_contract_helper_is_retained_then_rejected();
    test_multiple_import_context_canonicalization();
    test_p2_6b1_predicate_rejections();
    test_predicate_literal_authentication_and_rendering();
    if (failures != 0) {
        fprintf(stderr, "%d MIR operations test(s) failed\n", failures); return 1;
    }
    printf("MIR operations tests passed\n"); return 0;
}
