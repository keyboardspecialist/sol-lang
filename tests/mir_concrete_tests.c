#include "sol/mir_concrete.h"

#include "sol/effects.h"
#include "sol/lexer.h"
#include "sol/ownership.h"
#include "sol/package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #value); \
    ++failures; \
} } while (0)

void sol_mir_linkage_test_force_runtime_digest_collision(bool force);

typedef struct {
    SolDiagnostics diagnostics;
    SolHirModule hir;
    SolTypeTable types;
    SolEffectTable effects;
    SolContractTable contracts;
    SolIr ir;
    SolPackage package;
} Compilation;

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
} TextCompilation;

typedef struct {
    SolMirProgramRoot roots[5];
    size_t root_count;
    SolIrCallableId imports[4];
} E6Request;

static SolIrCallableId callable(const SolIr *ir, const char *name,
    SolIrCallableKind kind) {
    for (size_t i = 0; i < ir->callable_count; ++i)
        if (ir->callables[i].kind == kind
            && strcmp(ir->callables[i].name, name) == 0) return i;
    return SOL_IR_NONE;
}

static bool compile_e6(Compilation *c) {
    memset(c, 0, sizeof(*c));
    sol_package_init(&c->package); sol_diagnostics_init(&c->diagnostics);
    sol_hir_module_init(&c->hir); sol_type_table_init(&c->types);
    sol_effect_table_init(&c->effects); sol_contract_table_init(&c->contracts);
    sol_ir_init(&c->ir);
    char error[256];
    if (!sol_package_load_directory(&c->package,
            SOL_TEST_SOURCE_DIR "/tests/conformance/e6", &c->diagnostics,
            error, sizeof(error))) return false;
    SolHirFileScope *scopes = c->package.file_count == 0 ? NULL
        : malloc(c->package.file_count * sizeof(*scopes));
    if (c->package.file_count != 0 && scopes == NULL) return false;
    for (size_t i = 0; i < c->package.file_count; ++i)
        scopes[i] = (SolHirFileScope){c->package.files[i].module_name,
            c->package.files[i].import_start, c->package.files[i].import_count,
            c->package.files[i].item_start, c->package.files[i].item_count};
    bool ok = sol_hir_lower_scoped(&c->package.source, &c->package.syntax,
            scopes, c->package.file_count, &c->hir, &c->diagnostics)
        && sol_type_check(&c->package.source, &c->package.syntax, &c->hir,
            &c->types, &c->diagnostics)
        && sol_effect_check(&c->package.source, &c->package.syntax, &c->hir,
            &c->types, &c->effects, &c->diagnostics)
        && sol_contract_lower(&c->package.source, &c->package.syntax, &c->hir,
            &c->types, &c->effects, &c->contracts, &c->diagnostics)
        && sol_ir_lower_scoped(&c->package.source, &c->package.syntax, &c->hir,
            &c->types, &c->effects, &c->contracts, c->package.files,
            c->package.file_count, &c->ir, &c->diagnostics);
    free(scopes); return ok;
}

static void compilation_free(Compilation *c) {
    sol_ir_free(&c->ir); sol_contract_table_free(&c->contracts);
    sol_effect_table_free(&c->effects); sol_type_table_free(&c->types);
    sol_hir_module_free(&c->hir); sol_diagnostics_free(&c->diagnostics);
    sol_package_free(&c->package);
}

static bool compile_text(TextCompilation *c, const char *text) {
    memset(c, 0, sizeof(*c)); sol_tokens_init(&c->tokens);
    sol_diagnostics_init(&c->diagnostics); sol_syntax_tree_init(&c->syntax);
    sol_hir_module_init(&c->hir); sol_type_table_init(&c->types);
    sol_effect_table_init(&c->effects); sol_contract_table_init(&c->contracts);
    sol_ir_init(&c->ir);
    return sol_source_from_text(&c->source, "mir_concrete.sol", text)
        && sol_lex(&c->source, &c->tokens, &c->diagnostics)
        && sol_parse(&c->source, &c->tokens, &c->syntax, &c->diagnostics)
        && sol_hir_lower(&c->source, &c->syntax, &c->hir, &c->diagnostics)
        && sol_type_check(&c->source, &c->syntax, &c->hir, &c->types,
            &c->diagnostics)
        && sol_effect_check(&c->source, &c->syntax, &c->hir, &c->types,
            &c->effects, &c->diagnostics)
        && sol_contract_lower(&c->source, &c->syntax, &c->hir, &c->types,
            &c->effects, &c->contracts, &c->diagnostics)
        && sol_ir_lower(&c->source, &c->syntax, &c->hir, &c->types,
            &c->effects, &c->contracts, &c->ir, &c->diagnostics);
}

static void text_compilation_free(TextCompilation *c) {
    sol_ir_free(&c->ir); sol_contract_table_free(&c->contracts);
    sol_effect_table_free(&c->effects); sol_type_table_free(&c->types);
    sol_hir_module_free(&c->hir); sol_syntax_tree_free(&c->syntax);
    sol_tokens_free(&c->tokens); sol_source_free(&c->source);
    sol_diagnostics_free(&c->diagnostics);
}

static void e6_request(const SolIr *ir, E6Request *request) {
    static const char *const imports[] = {"write", "get", "count", "read"};
    request->root_count = 1;
    request->roots[0] = (SolMirProgramRoot){callable(ir, "launch",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_ENTRY};
    for (size_t i = 0; i < ir->callable_count; ++i)
        if (ir->callables[i].kind == SOL_IR_CALLABLE_TEST)
            request->roots[request->root_count++] = (SolMirProgramRoot){i,
                SOL_MIR_PROGRAM_ROOT_TEST};
    for (size_t i = 0; i < 4; ++i)
        request->imports[i] = callable(ir, imports[i],
            SOL_IR_CALLABLE_CAPABILITY);
}

static SolMirConcreteBuildOutcome build(const SolIr *ir,
    const E6Request *e6, const SolMirConcreteLimits *limits,
    SolMirConcreteProgram *program, SolDiagnostics *diagnostics) {
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    SolMirConcreteBuildRequest request = {ir, e6->roots, e6->root_count,
        e6->imports, 4, &target, limits};
    return sol_mir_concrete_program_build(&request, program, diagnostics);
}

static char *render(const SolMirConcreteProgram *program) {
    FILE *stream = tmpfile();
    if (stream == NULL || !sol_mir_concrete_program_render(stream, program)
        || fflush(stream) != 0 || fseek(stream, 0, SEEK_END) != 0) {
        if (stream != NULL) fclose(stream); return NULL;
    }
    long end = ftell(stream);
    if (end < 0 || fseek(stream, 0, SEEK_SET) != 0) {
        fclose(stream); return NULL;
    }
    char *text = malloc((size_t)end + 1);
    if (text == NULL || fread(text, 1, (size_t)end, stream) != (size_t)end) {
        free(text); fclose(stream); return NULL;
    }
    text[end] = '\0'; fclose(stream); return text;
}

static char *relocated_path(const char *path) {
    size_t length = strlen(path);
    char *result = malloc(length + 1);
    if (result == NULL) return NULL;
    memcpy(result, path, length + 1);
    for (size_t i = 0; i < length; ++i)
        if (result[i] != '/') result[i] = result[i] == 'x' ? 'y' : 'x';
    return result;
}

static void check_zero(const SolMirConcreteProgram *program) {
    SolMirConcreteProgram zero; sol_mir_concrete_program_init(&zero);
    CHECK(memcmp(program, &zero, sizeof(zero)) == 0);
}

static void print_diagnostics(const SolDiagnostics *diagnostics) {
    for (size_t i = 0; i < diagnostics->count; ++i)
        fprintf(stderr, "%s: %s\n", diagnostics->items[i].code,
            diagnostics->items[i].message);
}

static void check_no_rendered_bytes(const SolMirConcreteProgram *program) {
    FILE *stream = tmpfile(); CHECK(stream != NULL);
    if (stream == NULL) return;
    CHECK(!sol_mir_concrete_program_render(stream, program));
    CHECK(fflush(stream) == 0 && fseek(stream, 0, SEEK_END) == 0
        && ftell(stream) == 0);
    fclose(stream);
}

static void test_lifecycle_requests_and_limits(Compilation *c,
    const E6Request *e6) {
    SolMirConcreteProgram program;
    memset(&program, 0xa5, sizeof(program));
    sol_mir_concrete_program_init(&program); check_zero(&program);
    CHECK(!sol_mir_concrete_program_validate(&program, NULL));
    sol_mir_concrete_program_free(&program); check_zero(&program);

    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    SolMirConcreteBuildRequest request = {&c->ir, e6->roots, e6->root_count,
        e6->imports, 4, &target, NULL};
    CHECK(sol_mir_concrete_program_build(NULL, &program, NULL)
        == SOL_MIR_CONCRETE_BUILD_INVALID_ARGUMENT);
    CHECK(sol_mir_concrete_program_build(&request, NULL, &c->diagnostics)
        == SOL_MIR_CONCRETE_BUILD_INVALID_ARGUMENT);
    CHECK(sol_mir_concrete_program_build(&request, &program, NULL)
        == SOL_MIR_CONCRETE_BUILD_INVALID_ARGUMENT);
    check_zero(&program);
    request.ir = NULL;
    CHECK(sol_mir_concrete_program_build(&request, &program, &c->diagnostics)
        == SOL_MIR_CONCRETE_BUILD_INVALID_ARGUMENT);
    request.ir = &c->ir; request.target = NULL;
    CHECK(sol_mir_concrete_program_build(&request, &program, &c->diagnostics)
        == SOL_MIR_CONCRETE_BUILD_INVALID_ARGUMENT);
    request.target = &target; target.pointer_size = 0;
    CHECK(sol_mir_concrete_program_build(&request, &program, &c->diagnostics)
        == SOL_MIR_CONCRETE_BUILD_INVALID_TARGET);
    target = sol_mir_target_wasm32();
    size_t saved_start = c->ir.files[0].aggregate_start;
    size_t saved_end = c->ir.files[0].aggregate_end;
    c->ir.files[0].aggregate_start = c->ir.source_length;
    c->ir.files[0].aggregate_end = c->ir.source_length;
    CHECK(sol_mir_concrete_program_build(&request, &program, &c->diagnostics)
        == SOL_MIR_CONCRETE_BUILD_INVALID_IR);
    c->ir.files[0].aggregate_start = saved_start;
    c->ir.files[0].aggregate_end = saved_end;

    SolMirConcreteLimits partial = sol_mir_concrete_default_limits();
    partial.operations.max_arithmetic = 0; request.limits = &partial;
    CHECK(sol_mir_concrete_program_build(&request, &program, &c->diagnostics)
        == SOL_MIR_CONCRETE_BUILD_INVALID_ARGUMENT);
    SolMirConcreteLimits zero = {0}; request.limits = &zero;
    SolMirConcreteBuildOutcome outcome = sol_mir_concrete_program_build(&request,
        &program, &c->diagnostics);
    if (outcome != SOL_MIR_CONCRETE_BUILD_SUCCEEDED)
        print_diagnostics(&c->diagnostics);
    CHECK(outcome == SOL_MIR_CONCRETE_BUILD_SUCCEEDED);
    if (outcome != SOL_MIR_CONCRETE_BUILD_SUCCEEDED) return;
    SolMirConcreteProgram before = program;
    CHECK(sol_mir_concrete_program_build(&request, &program, &c->diagnostics)
        == SOL_MIR_CONCRETE_BUILD_INVALID_ARGUMENT);
    CHECK(memcmp(&program, &before, sizeof(program)) == 0);
    sol_mir_concrete_program_free(&program); check_zero(&program);

    SolMirConcreteLimits limited = sol_mir_concrete_default_limits();
#define EXHAUST(member, field) do { \
    limited = sol_mir_concrete_default_limits(); limited.member.field = 1; \
    CHECK(build(&c->ir, e6, &limited, &program, &c->diagnostics) \
        == SOL_MIR_CONCRETE_BUILD_RESOURCE_EXHAUSTED); check_zero(&program); \
} while (0)
    EXHAUST(program, max_references);
    EXHAUST(plan, max_instances);
    EXHAUST(materialization, max_instances);
    EXHAUST(representation, max_recipes);
    EXHAUST(layout, max_type_layouts);
    EXHAUST(operations, max_constructors);
    EXHAUST(linkage, max_callables);
#undef EXHAUST
}

static void test_unsupported_closure(void) {
    static const char source[] =
        "module concrete_unsupported\n"
        "trait Score { function score(self: Self) -> Int64 effects { pure } }\n"
        "implementation Score for Int64 { function score(self: Self) -> Int64 "
        "effects { pure } { return self } }\n"
        "function scored<T: Score>(value: T) -> Int64 effects { pure } "
        "{ return value.score() }\n";
    TextCompilation c; CHECK(compile_text(&c, source));
    SolMirProgramRoot root = {callable(&c.ir, "scored",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    SolMirConcreteBuildRequest request = {&c.ir, &root, 1, NULL, 0, &target,
        NULL};
    SolMirConcreteProgram program; sol_mir_concrete_program_init(&program);
    CHECK(sol_mir_concrete_program_build(&request, &program, &c.diagnostics)
        == SOL_MIR_CONCRETE_BUILD_UNSUPPORTED_CLOSURE);
    check_zero(&program); text_compilation_free(&c);
}

static void check_counts(const char *stage, const size_t *actual,
    const size_t *expected, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (actual[i] == expected[i]) continue;
        fprintf(stderr, "%s census %zu: expected %zu, got %zu\n", stage, i,
            expected[i], actual[i]);
        ++failures;
    }
}

static const size_t expected_program_counts[] = {
    5, 4, 14, 4, 1, 18,
};
static const SolMirProgramUsage expected_program_usage = {19, 18, 1055};

/* types, components, type accesses, effect atoms/rows/row atoms, instances,
   instance types/accesses, dictionaries, imports, uses, contexts, demands. */
static const size_t expected_plan_counts[] = {
    21, 23, 3, 9, 7, 10, 14, 16, 14, 1, 4, 738, 18, 23,
};
static const SolMirPlanUsage expected_plan_usage = {
    14, 21, 23, 738, 18, 11022, 3,
};

/* Follows the declaration order in SolMirMaterialization. */
static const size_t expected_materialization_counts[] = {
    14, 21, 5, 3, 39, 17, 738, 18, 27, 48, 5, 173, 583, 50, 27,
    18, 91, 86, 40, 32, 0, 23, 23, 0, 4, 0, 1, 7, 9, 10, 164, 60,
};
static const SolMirMaterializeUsage expected_materialization_usage = {
    14, 1263, 761, 2042, 411948, 3295, 185, 95255944,
};

static const size_t expected_representation_counts[] = {21, 11, 9, 3, 3, 0, 0};
static const SolMirRepresentationUsage expected_representation_usage = {
    21, 11, 9, 3, 0, 0, 3348, 63, 385, 95318662, 762459500,
};

static const size_t expected_layout_counts[] = {21, 11, 9, 5};
static const SolMirLayoutUsage expected_layout_usage = {
    21, 11, 9, 5, 4232, 21, 87, 762459500, 95318796,
};

/* Follows SOL_MIR_OPERATIONS_ARENAS order. */
static const size_t expected_operations_counts[] = {
    48, 5, 28, 27, 3, 3, 12, 28, 2, 17, 26, 15, 1, 0, 0, 4, 4,
    4, 4, 10, 6, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 62,
};
static const SolMirOperationsUsage expected_operations_usage = {
    .access_plans = 48, .access_steps = 5,
    .constructors = 28, .construct_operands = 27,
    .pattern_tests = 3, .pattern_extractions = 3,
    .pattern_nodes = 12, .path_steps = 28,
    .propagations = 2, .arithmetic = 17,
    .equality_nodes = 26, .equality_children = 15,
    .snapshots = 1, .callables = 0, .handlers = 0, .predicates = 4,
    .recipe_ids = 0, .roots = 0, .provenance = 62,
    .predicate_bodies = 4, .predicate_blocks = 4, .predicate_inputs = 4,
    .predicate_values = 10, .predicate_instructions = 6,
    .predicate_edges = 0, .predicate_edge_values = 0,
    .predicate_operands = 0, .predicate_path_steps = 0,
    .predicate_pattern_nodes = 0, .import_envelopes = 4,
    .import_contract_references = 0, .literal_bytes = 0,
    .import_snapshots = 0, .owned_bytes = 20960,
    .build_scratch_bytes = 2679, .build_work = 10525,
    .validation_scratch_bytes = 762459500, .validation_work = 95357416,
};

static const size_t expected_linkage_counts[] = {14, 23, 1, 0, 0, 4, 17};
static const SolMirLinkageUsage expected_linkage_usage = {
    14, 23, 1, 0, 0, 4, 17, 4648, 0, 77697, 762459500, 95443081,
};

static void check_e6_census_and_closure(const Compilation *c,
    const SolMirConcreteProgram *p) {
    const size_t program_counts[] = {p->program.root_count,
        p->program.approved_import_count, p->program.template_count,
        p->program.import_count, p->program.specialization_count,
        p->program.reference_count};
    const size_t plan_counts[] = {p->plan.type_count,
        p->plan.type_component_count, p->plan.type_parameter_access_count,
        p->plan.effect_atom_count, p->plan.effect_row_count,
        p->plan.effect_row_atom_count, p->plan.instance_count,
        p->plan.instance_type_id_count, p->plan.instance_access_count,
        p->plan.dictionary_entry_count, p->plan.import_count,
        p->plan.typed_use_count, p->plan.context_count, p->plan.demand_count};
    const size_t materialization_counts[] = {
        p->materialization.image_count, p->materialization.type_count,
        p->materialization.shape_field_count,
        p->materialization.shape_variant_count,
        p->materialization.type_id_count, p->materialization.access_count,
        p->materialization.overlay_count, p->materialization.context_count,
        p->materialization.local_count, p->materialization.place_count,
        p->materialization.projection_count, p->materialization.value_count,
        p->materialization.instruction_count,
        p->materialization.temporary_count,
        p->materialization.construct_operand_count,
        p->materialization.call_argument_count, p->materialization.block_count,
        p->materialization.edge_count, p->materialization.edge_value_count,
        p->materialization.parameter_value_count, p->materialization.loop_count,
        p->materialization.binding_count,
        p->materialization.semantic_site_count,
        p->materialization.receiver_root_count, p->materialization.import_count,
        p->materialization.handler_count, p->materialization.writeback_count,
        p->materialization.effect_row_count,
        p->materialization.effect_atom_count,
        p->materialization.effect_row_atom_count,
        p->materialization.effect_name_count,
        p->materialization.literal_byte_count};
    const size_t representation_counts[] = {p->representation.recipe_count,
        p->representation.field_count, p->representation.variant_count,
        p->representation.recipe_id_count, p->representation.access_count,
        p->representation.receiver_root_count,
        p->representation.callable_producer_count};
    const size_t layout_counts[] = {p->layout.type_count,
        p->layout.field_count, p->layout.variant_count,
        p->layout.projection_count};
#define OP_COUNT(member, type, singular) p->operations.singular##_count,
    const size_t operations_counts[] = {SOL_MIR_OPERATIONS_ARENAS(OP_COUNT)};
#undef OP_COUNT
#define LINK_COUNT(member, type, singular) p->linkage.singular##_count,
    const size_t linkage_counts[] = {SOL_MIR_LINKAGE_ARENAS(LINK_COUNT)};
#undef LINK_COUNT
#define CHECK_TABLE(stage) check_counts(#stage, stage##_counts, \
    expected_##stage##_counts, sizeof(expected_##stage##_counts) \
        / sizeof(expected_##stage##_counts[0]))
    CHECK_TABLE(program); CHECK_TABLE(plan); CHECK_TABLE(materialization);
    CHECK_TABLE(representation); CHECK_TABLE(layout); CHECK_TABLE(operations);
    CHECK_TABLE(linkage);
#undef CHECK_TABLE

#define EQ(left, right, member) (left).member == (right).member
    CHECK(EQ(p->program.usage, expected_program_usage,
            callable_classifications)
        && EQ(p->program.usage, expected_program_usage, references)
        && EQ(p->program.usage, expected_program_usage, discovery_work));
    CHECK(EQ(p->plan.usage, expected_plan_usage, instances)
        && EQ(p->plan.usage, expected_plan_usage, concrete_types)
        && EQ(p->plan.usage, expected_plan_usage, demands)
        && EQ(p->plan.usage, expected_plan_usage, typed_uses)
        && EQ(p->plan.usage, expected_plan_usage, contexts)
        && EQ(p->plan.usage, expected_plan_usage, planning_work)
        && EQ(p->plan.usage, expected_plan_usage, substitution_depth));
#define MAT_USAGE(member) EQ(p->materialization.usage, \
    expected_materialization_usage, member)
    CHECK(MAT_USAGE(instances) && MAT_USAGE(cfg_items) && MAT_USAGE(bindings)
        && MAT_USAGE(concrete_records) && MAT_USAGE(owned_bytes)
        && MAT_USAGE(materialization_work) && MAT_USAGE(shape_resolution_work)
        && MAT_USAGE(validation_work));
#undef MAT_USAGE
#define REP_USAGE(member) EQ(p->representation.usage, \
    expected_representation_usage, member)
    CHECK(REP_USAGE(recipes) && REP_USAGE(fields) && REP_USAGE(variants)
        && REP_USAGE(recipe_ids) && REP_USAGE(callable_producers)
        && REP_USAGE(receiver_roots) && REP_USAGE(owned_bytes)
        && REP_USAGE(build_scratch_bytes) && REP_USAGE(build_work)
        && REP_USAGE(validation_work) && REP_USAGE(validation_scratch_bytes));
#undef REP_USAGE
#define LAYOUT_USAGE(member) EQ(p->layout.usage, expected_layout_usage, member)
    CHECK(LAYOUT_USAGE(type_layouts) && LAYOUT_USAGE(field_layouts)
        && LAYOUT_USAGE(variant_layouts) && LAYOUT_USAGE(projection_maps)
        && LAYOUT_USAGE(owned_bytes) && LAYOUT_USAGE(build_scratch_bytes)
        && LAYOUT_USAGE(build_work) && LAYOUT_USAGE(validation_scratch_bytes)
        && LAYOUT_USAGE(validation_work));
#undef LAYOUT_USAGE
#define OP_USAGE(member, type, singular) \
    && EQ(p->operations.usage, expected_operations_usage, member)
    CHECK(true SOL_MIR_OPERATIONS_ARENAS(OP_USAGE)
        && EQ(p->operations.usage, expected_operations_usage, owned_bytes)
        && EQ(p->operations.usage, expected_operations_usage,
            build_scratch_bytes)
        && EQ(p->operations.usage, expected_operations_usage, build_work)
        && EQ(p->operations.usage, expected_operations_usage,
            validation_scratch_bytes)
        && EQ(p->operations.usage, expected_operations_usage,
            validation_work));
#undef OP_USAGE
#define LINK_USAGE(member, type, singular) \
    && EQ(p->linkage.usage, expected_linkage_usage, member)
    CHECK(true SOL_MIR_LINKAGE_ARENAS(LINK_USAGE)
        && EQ(p->linkage.usage, expected_linkage_usage, owned_bytes)
        && EQ(p->linkage.usage, expected_linkage_usage, build_scratch_bytes)
        && EQ(p->linkage.usage, expected_linkage_usage, build_work)
        && EQ(p->linkage.usage, expected_linkage_usage,
            validation_scratch_bytes)
        && EQ(p->linkage.usage, expected_linkage_usage, validation_work));
#undef LINK_USAGE
#undef EQ

    SolIrCallableId identity = callable(&c->ir, "identity",
        SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId score = callable(&c->ir, "score", SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId score_method = callable(&c->ir, "score",
        SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION);
    bool identity_i64 = false, score_i64 = false, method_i64 = false;
    for (size_t i = 0; i < p->plan.instance_count; ++i) {
        const SolMirPlanInstance *instance = &p->plan.instances[i];
        if (instance->callable == identity && instance->type_arguments.count == 1)
            identity_i64 = p->plan.types[p->plan.instance_type_ids[
                instance->type_arguments.offset]].kind == SOL_IR_TYPE_INT64;
        if (instance->callable == score && instance->type_arguments.count == 1
            && instance->dictionary.count == 1)
            score_i64 = p->plan.types[p->plan.instance_type_ids[
                instance->type_arguments.offset]].kind == SOL_IR_TYPE_INT64;
        if (instance->callable == score_method
            && instance->receiver != SOL_MIR_PLAN_NONE)
            method_i64 = p->plan.types[instance->receiver].kind
                == SOL_IR_TYPE_INT64;
    }
    CHECK(identity_i64 && score_i64 && method_i64);
    CHECK(p->plan.dictionary_entries[0].method == score_method
        && p->plan.types[p->plan.dictionary_entries[0].type].kind
            == SOL_IR_TYPE_INT64);

    size_t contract = 0, refinement = 0;
    for (size_t i = 0; i < p->operations.predicate_count; ++i) {
        contract += p->operations.predicates[i].kind
            == SOL_MIR_OPERATION_PREDICATE_CONTRACT;
        refinement += p->operations.predicates[i].kind
            == SOL_MIR_OPERATION_PREDICATE_REFINEMENT;
    }
    CHECK(contract == 3 && refinement == 1);
    for (size_t i = 0; i < c->ir.callable_count; ++i) {
        if (c->ir.callables[i].body == SOL_IR_NONE) continue;
        bool represented = false;
        for (size_t q = 0; q < p->materialization.image_count; ++q)
            represented |= p->materialization.images[q].source_callable == i;
        CHECK(represented);
    }
    size_t instruction_kinds[SOL_MIR_INST_SCOPE_EXIT + 1] = {0};
    size_t terminator_kinds[SOL_MIR_TERM_CONTRACT_VIOLATION + 1] = {0};
    for (size_t i = 0; i < p->materialization.instruction_count; ++i)
        ++instruction_kinds[p->materialization.instructions[i].kind];
    for (size_t i = 0; i < p->materialization.block_count; ++i)
        ++terminator_kinds[p->materialization.blocks[i].terminator.kind];
    const size_t expected_instructions[] = {22, 5, 7, 2, 12, 15, 111, 111,
        26, 0, 1, 16, 0, 16, 1, 1, 1, 50, 5, 4, 3, 3, 3, 1, 0, 0, 28, 1};
    const size_t expected_terminators[] = {0, 11, 12, 17, 1, 18, 22, 0, 0,
        0, 1, 1, 2, 3, 3};
    CHECK(memcmp(instruction_kinds, expected_instructions,
        sizeof(expected_instructions)) == 0);
    CHECK(memcmp(terminator_kinds, expected_terminators,
        sizeof(expected_terminators)) == 0);
    CHECK(terminator_kinds[SOL_MIR_TERM_PANIC] != 0
        && terminator_kinds[SOL_MIR_TERM_INVOKE] != 0
        && terminator_kinds[SOL_MIR_TERM_RESUME_FAILURE] != 0
        && terminator_kinds[SOL_MIR_TERM_PROPAGATE] != 0
        && terminator_kinds[SOL_MIR_TERM_CHECK_REFINED] != 0
        && terminator_kinds[SOL_MIR_TERM_CHECK_CONTRACT] != 0
        && terminator_kinds[SOL_MIR_TERM_CONTRACT_VIOLATION] != 0);
    CHECK(instruction_kinds[SOL_MIR_INST_DROP_IF_INITIALIZED] != 0
        && instruction_kinds[SOL_MIR_INST_STORAGE_DEAD] != 0
        && instruction_kinds[SOL_MIR_INST_TEMPORARY_DROP] != 0
        && instruction_kinds[SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED] != 0);
    CHECK(instruction_kinds[SOL_MIR_INST_DROP_IF_INITIALIZED] == 111
        && instruction_kinds[SOL_MIR_INST_STORAGE_DEAD] == 111
        && instruction_kinds[SOL_MIR_INST_REGION_ENTER] == 1
        && instruction_kinds[SOL_MIR_INST_REGION_EXIT] == 1
        && instruction_kinds[SOL_MIR_INST_TEMPORARY_DROP] == 5
        && instruction_kinds[SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED] == 1
        && instruction_kinds[SOL_MIR_INST_HANDLER_ENTER] == 0
        && instruction_kinds[SOL_MIR_INST_HANDLER_EXIT] == 0);
    size_t normal_edges = 0, failure_edges = 0;
    for (size_t i = 0; i < p->materialization.block_count; ++i) {
        const SolMirMaterializedTerminator *term
            = &p->materialization.blocks[i].terminator;
        if (term->kind != SOL_MIR_TERM_INVOKE) continue;
        normal_edges += term->normal_edge != SOL_MIR_MATERIALIZED_NONE;
        failure_edges += term->failure_edge != SOL_MIR_MATERIALIZED_NONE;
    }
    CHECK(normal_edges == 18 && failure_edges == 18
        && p->materialization.writeback_count == 1
        && p->materialization.handler_count == 0);
    size_t predicate_terms[SOL_MIR_PREDICATE_TERM_FAILURE + 1] = {0};
    size_t predicate_failures[SOL_MIR_PREDICATE_FAILURE_NO_MATCH + 1] = {0};
    for (size_t i = 0; i < p->operations.predicate_block_count; ++i) {
        const SolMirPredicateTerminator *term
            = &p->operations.predicate_blocks[i].terminator;
        ++predicate_terms[term->kind];
        if (term->kind == SOL_MIR_PREDICATE_TERM_FAILURE)
            ++predicate_failures[term->failure_kind];
    }
    const size_t expected_predicate_terms[] = {4, 0, 0, 0, 0, 0, 0};
    const size_t expected_predicate_failures[] = {0, 0, 0, 0};
    size_t import_host[2] = {0};
    size_t import_contract_slices[3] = {0};
    for (size_t i = 0; i < p->operations.import_envelope_count; ++i) {
        const SolMirImportContractEnvelope *envelope
            = &p->operations.import_envelopes[i];
        ++import_host[envelope->host_invoke];
        import_contract_slices[0] += envelope->requires.count;
        import_contract_slices[1] += envelope->snapshots.count;
        import_contract_slices[2] += envelope->ensures.count;
    }
    const size_t expected_import_host[] = {0, 4};
    const size_t expected_import_contract_slices[] = {0, 0, 0};
    size_t runtime_operations[32] = {0};
    size_t runtime_storage[SOL_MIR_STORAGE_CAPABILITY_HANDLE + 1] = {0};
    size_t runtime_copy[SOL_MIR_COPY_UNREACHABLE + 1] = {0};
    size_t runtime_drop[SOL_MIR_DROP_WRAPPER + 1] = {0};
    for (size_t i = 0; i < p->linkage.runtime_requirement_count; ++i) {
        const SolMirLinkageRuntimeRequirement *requirement
            = &p->linkage.runtime_requirements[i];
        CHECK(requirement->operations < 32);
        if (requirement->operations < 32)
            ++runtime_operations[requirement->operations];
        ++runtime_storage[requirement->storage];
        ++runtime_copy[requirement->copy_kind];
        ++runtime_drop[requirement->drop_kind];
    }
    const size_t expected_runtime_operations[32] = {
        [5] = 7, [6] = 1, [7] = 4, [15] = 5,
    };
    const size_t expected_runtime_storage[] = {0, 1, 1, 8, 4, 3};
    const size_t expected_runtime_copy[] = {0, 1, 8, 1, 7, 0};
    const size_t expected_runtime_drop[] = {0, 1, 8, 4, 3, 1};
    CHECK(memcmp(predicate_terms, expected_predicate_terms,
            sizeof(expected_predicate_terms)) == 0
        && memcmp(predicate_failures, expected_predicate_failures,
            sizeof(expected_predicate_failures)) == 0
        && memcmp(import_host, expected_import_host,
            sizeof(expected_import_host)) == 0
        && memcmp(import_contract_slices, expected_import_contract_slices,
            sizeof(expected_import_contract_slices)) == 0
        && memcmp(runtime_operations, expected_runtime_operations,
            sizeof(expected_runtime_operations)) == 0
        && memcmp(runtime_storage, expected_runtime_storage,
            sizeof(expected_runtime_storage)) == 0
        && memcmp(runtime_copy, expected_runtime_copy,
            sizeof(expected_runtime_copy)) == 0
        && memcmp(runtime_drop, expected_runtime_drop,
            sizeof(expected_runtime_drop)) == 0
        && p->operations.import_contract_reference_count == 0
        && p->linkage.runtime_requirement_count == 17);
}

static void test_validation_mutations(SolMirConcreteProgram *p) {
    CHECK(sol_mir_concrete_program_validate(p, NULL));
    const SolMirProgram *program_link = p->plan.program;
    p->plan.program = NULL; CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->plan.program = program_link;
    const SolMirPlan *plan_link = p->materialization.plan;
    p->materialization.plan = NULL;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->materialization.plan = plan_link;
    const SolMirMaterialization *materialization_link
        = p->representation.materialization;
    p->representation.materialization = NULL;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->representation.materialization = materialization_link;
    const SolMirRepresentation *representation_link = p->layout.representation;
    p->layout.representation = NULL;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->layout.representation = representation_link;
    const SolMirLayout *layout_link = p->operations.layout;
    p->operations.layout = NULL;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->operations.layout = layout_link;
    const SolMirOperations *operations_link = p->linkage.operations;
    p->linkage.operations = NULL;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->linkage.operations = operations_link;
    SolMirConcreteProgram relocated = *p;
    CHECK(!sol_mir_concrete_program_validate(&relocated, NULL));

#define MUTATE(object, member, value) do { \
    unsigned char saved[sizeof((object).member)]; \
    memcpy(saved, &(object).member, sizeof(saved)); (object).member = (value); \
    CHECK(!sol_mir_concrete_program_validate(p, NULL)); \
    check_no_rendered_bytes(p); memcpy(&(object).member, saved, sizeof(saved)); \
} while (0)
    MUTATE(p->program.templates[0], callable, SOL_IR_NONE);
    MUTATE(p->plan.types[0], kind, SOL_IR_TYPE_PARAMETER);
    MUTATE(p->materialization.instructions[0], kind,
        (SolMirInstructionKind)99);
    MUTATE(p->representation.recipes[0], kind, (SolMirRecipeKind)99);
    MUTATE(p->layout.types[0], recipe, SOL_MIR_RECIPE_NONE);
    MUTATE(p->operations.constructors[0], kind,
        (SolMirOperationConstructKind)99);
    MUTATE(p->linkage.bindings[0], binding, SOL_MIR_LINKAGE_NONE);
#undef MUTATE
#define HEADER(field) do { \
    size_t saved_header = (field); ++(field); \
    CHECK(!sol_mir_concrete_program_validate(p, NULL)); (field) = saved_header; \
} while (0)
    HEADER(p->program.usage.references);
    HEADER(p->plan.usage.instances);
    HEADER(p->materialization.usage.instances);
    HEADER(p->representation.usage.recipes);
    HEADER(p->layout.usage.type_layouts);
    HEADER(p->operations.usage.constructors);
    HEADER(p->linkage.usage.callables);
#undef HEADER

    size_t saved_image_count = p->materialization.image_count;
    p->materialization.image_count = 0;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->materialization.image_count = p->plan.instance_count + 1;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->materialization.image_count = SIZE_MAX;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->materialization.image_count = saved_image_count;
    size_t saved_access_count = p->operations.access_plan_count;
    p->operations.access_plan_count = 0;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->operations.access_plan_count = p->materialization.place_count + 1;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->operations.access_plan_count = SIZE_MAX;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->operations.access_plan_count = saved_access_count;

    SolMirPlanSlice saved_slice = p->plan.instances[0].typed_uses;
    p->plan.instances[0].typed_uses = (SolMirPlanSlice){0, 0};
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->plan.instances[0].typed_uses = saved_slice;
    saved_slice = p->materialization.images[0].blocks;
    p->materialization.images[0].blocks
        = (SolMirPlanSlice){p->materialization.block_count, 1};
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->materialization.images[0].blocks
        = (SolMirPlanSlice){SIZE_MAX, SIZE_MAX};
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->materialization.images[0].blocks = saved_slice;

    size_t saved_demand = p->materialization.bindings[0].source_demand;
    p->materialization.bindings[0].source_demand
        = p->materialization.bindings[1].source_demand;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->materialization.bindings[0].source_demand = saved_demand;
    size_t invoke = 0;
    while (invoke < p->materialization.block_count
        && p->materialization.blocks[invoke].terminator.kind
            != SOL_MIR_TERM_INVOKE) ++invoke;
    CHECK(invoke < p->materialization.block_count);
    if (invoke < p->materialization.block_count) {
        size_t saved_site
            = p->materialization.blocks[invoke].terminator.callable_site;
        p->materialization.blocks[invoke].terminator.callable_site
            = p->materialization.semantic_site_count;
        CHECK(!sol_mir_concrete_program_validate(p, NULL));
        p->materialization.blocks[invoke].terminator.callable_site = saved_site;
    }

    SolMirPlanType *saved_plan_types = p->plan.types;
    p->plan.types = (SolMirPlanType *)(void *)p->program.roots;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->plan.types = saved_plan_types;
    SolMirMaterializedType *saved_materialized_types = p->materialization.types;
    p->materialization.types = (SolMirMaterializedType *)(void *)p->plan.types;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->materialization.types = saved_materialized_types;
    SolMirRecipe *saved_recipes = p->representation.recipes;
    p->representation.recipes = (SolMirRecipe *)(void *)p->materialization.types;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->representation.recipes = saved_recipes;
    SolMirTypeLayout *saved_layout_types = p->layout.types;
    p->layout.types = (SolMirTypeLayout *)(void *)p->representation.recipes;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->layout.types = saved_layout_types;
    SolMirOperationAccessPlan *saved_access_plans = p->operations.access_plans;
    p->operations.access_plans = (SolMirOperationAccessPlan *)(void *)
        p->layout.types;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->operations.access_plans = saved_access_plans;
    SolMirLinkageCallable *saved = p->linkage.callables;
    p->linkage.callables = (SolMirLinkageCallable *)(void *)
        p->operations.access_plans;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->linkage.callables = saved;

    SolIrCallableId *saved_approvals = p->program.approved_imports;
    p->program.approved_imports = (SolIrCallableId *)(void *)p->program.roots;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->program.approved_imports = saved_approvals;
    SolMirPlanTypeId *saved_components = p->plan.type_components;
    p->plan.type_components = (SolMirPlanTypeId *)(void *)p->plan.types;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->plan.type_components = saved_components;
    SolMirMaterializedTypeId *saved_type_ids = p->materialization.type_ids;
    p->materialization.type_ids = (SolMirMaterializedTypeId *)(void *)
        p->materialization.types;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->materialization.type_ids = saved_type_ids;
    SolMirRecipeField *saved_fields = p->representation.fields;
    p->representation.fields = (SolMirRecipeField *)(void *)
        p->representation.recipes;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->representation.fields = saved_fields;
    SolMirFieldLayout *saved_layout_fields = p->layout.fields;
    p->layout.fields = (SolMirFieldLayout *)(void *)p->layout.types;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->layout.fields = saved_layout_fields;
    SolMirOperationAccessStep *saved_access_steps = p->operations.access_steps;
    p->operations.access_steps = (SolMirOperationAccessStep *)(void *)
        p->operations.access_plans;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->operations.access_steps = saved_access_steps;
    SolMirLinkageBinding *saved_linkage_bindings = p->linkage.bindings;
    p->linkage.bindings = (SolMirLinkageBinding *)(void *)p->linkage.callables;
    CHECK(!sol_mir_concrete_program_validate(p, NULL));
    p->linkage.bindings = saved_linkage_bindings;
    CHECK(sol_mir_concrete_program_validate(p, NULL));
}

static void test_e6_complete_and_deterministic(Compilation *c,
    const E6Request *e6) {
    SolMirConcreteProgram first, repeat, reversed, relocated, limited;
    sol_mir_concrete_program_init(&first);
    sol_mir_concrete_program_init(&repeat);
    sol_mir_concrete_program_init(&reversed);
    sol_mir_concrete_program_init(&relocated);
    sol_mir_concrete_program_init(&limited);
    SolMirConcreteBuildOutcome outcome = build(&c->ir, e6, NULL, &first,
        &c->diagnostics);
    if (outcome != SOL_MIR_CONCRETE_BUILD_SUCCEEDED)
        print_diagnostics(&c->diagnostics);
    CHECK(outcome == SOL_MIR_CONCRETE_BUILD_SUCCEEDED);
    if (outcome != SOL_MIR_CONCRETE_BUILD_SUCCEEDED) return;
    CHECK(sol_mir_concrete_program_validate(&first, NULL));
    check_e6_census_and_closure(c, &first);
    CHECK(build(&c->ir, e6, NULL, &repeat, &c->diagnostics)
        == SOL_MIR_CONCRETE_BUILD_SUCCEEDED);
    E6Request reverse = *e6;
    for (size_t i = 0; i < e6->root_count; ++i)
        reverse.roots[i] = e6->roots[e6->root_count - i - 1];
    for (size_t i = 0; i < 4; ++i) reverse.imports[i] = e6->imports[3 - i];
    CHECK(build(&c->ir, &reverse, NULL, &reversed, &c->diagnostics)
        == SOL_MIR_CONCRETE_BUILD_SUCCEEDED);
    SolMirConcreteLimits alternate = sol_mir_concrete_default_limits();
    ++alternate.program.max_references;
    ++alternate.plan.max_instances;
    ++alternate.materialization.max_cfg_items;
    ++alternate.representation.max_recipes;
    ++alternate.layout.max_type_layouts;
    ++alternate.operations.max_constructors;
    ++alternate.linkage.max_callables;
    CHECK(build(&c->ir, e6, &alternate, &limited, &c->diagnostics)
        == SOL_MIR_CONCRETE_BUILD_SUCCEEDED);
    char *a = render(&first), *b = render(&repeat), *d = render(&reversed);
    char *l = render(&limited);
    char *saved_path = c->ir.source_path;
    char *new_source_path = relocated_path(saved_path);
    char **saved_file_paths = c->ir.file_count == 0 ? NULL
        : malloc(c->ir.file_count * sizeof(*saved_file_paths));
    char **new_file_paths = c->ir.file_count == 0 ? NULL
        : calloc(c->ir.file_count, sizeof(*new_file_paths));
    CHECK(new_source_path != NULL
        && (c->ir.file_count == 0
            || (saved_file_paths != NULL && new_file_paths != NULL)));
    if (new_source_path != NULL && (c->ir.file_count == 0
            || (saved_file_paths != NULL && new_file_paths != NULL))) {
        c->ir.source_path = new_source_path;
        for (size_t i = 0; i < c->ir.file_count; ++i) {
            saved_file_paths[i] = c->ir.files[i].path;
            new_file_paths[i] = relocated_path(saved_file_paths[i]);
            CHECK(new_file_paths[i] != NULL);
            if (new_file_paths[i] != NULL) c->ir.files[i].path = new_file_paths[i];
        }
    }
    CHECK(build(&c->ir, e6, NULL, &relocated, &c->diagnostics)
        == SOL_MIR_CONCRETE_BUILD_SUCCEEDED);
    char *r = render(&relocated);
    c->ir.source_path = saved_path;
    for (size_t i = 0; i < c->ir.file_count; ++i) {
        if (saved_file_paths != NULL) c->ir.files[i].path = saved_file_paths[i];
    }
    CHECK(a != NULL && b != NULL && d != NULL && l != NULL && r != NULL
        && strcmp(a, b) == 0 && strcmp(a, d) == 0 && strcmp(a, l) == 0
        && strcmp(a, r) == 0);
    CHECK(a != NULL && strstr(a, saved_path) == NULL
        && strstr(a, "\nlimits ") == NULL
        && strstr(a, "\npredicate_limits ") == NULL);
    for (size_t i = 0; a != NULL && saved_file_paths != NULL
            && i < c->ir.file_count; ++i) {
        CHECK(strstr(a, saved_file_paths[i]) == NULL);
        if (r != NULL && new_file_paths[i] != NULL)
            CHECK(strstr(r, new_file_paths[i]) == NULL);
    }
    if (r != NULL && new_source_path != NULL)
        CHECK(strstr(r, new_source_path) == NULL);
    for (size_t i = 0; i < c->ir.file_count; ++i) free(new_file_paths == NULL
        ? NULL : new_file_paths[i]);
    free(new_file_paths); free(saved_file_paths); free(new_source_path);
    CHECK(a != NULL && strstr(a, "mir_concrete_program") != NULL
        && strstr(a, "section=program") != NULL
        && strstr(a, "program.template") != NULL
        && strstr(a, "section=plan") != NULL
        && strstr(a, "plan.instance") != NULL
        && strstr(a, "section=materialization") != NULL
        && strstr(a, "section=representation") != NULL
        && strstr(a, "section=layout") != NULL
        && strstr(a, "section=operations") != NULL
        && strstr(a, "section=linkage") != NULL);
    free(a); free(b); free(d); free(l); free(r);
    test_validation_mutations(&first);
    sol_mir_concrete_program_free(&relocated);
    sol_mir_concrete_program_free(&limited);
    sol_mir_concrete_program_free(&reversed);
    sol_mir_concrete_program_free(&repeat);
    sol_mir_concrete_program_free(&first);
}

static void test_independent_ir_and_collision(Compilation *first,
    const E6Request *first_e6) {
    Compilation second; CHECK(compile_e6(&second));
    E6Request second_e6; e6_request(&second.ir, &second_e6);
    SolMirConcreteProgram a, b; sol_mir_concrete_program_init(&a);
    sol_mir_concrete_program_init(&b);
    CHECK(build(&first->ir, first_e6, NULL, &a, &first->diagnostics)
        == SOL_MIR_CONCRETE_BUILD_SUCCEEDED);
    CHECK(build(&second.ir, &second_e6, NULL, &b, &second.diagnostics)
        == SOL_MIR_CONCRETE_BUILD_SUCCEEDED);
    char *ar = render(&a), *br = render(&b);
    CHECK(ar != NULL && br != NULL && strcmp(ar, br) == 0);
    free(ar); free(br); sol_mir_concrete_program_free(&b);
    sol_mir_concrete_program_free(&a);

    sol_mir_linkage_test_force_runtime_digest_collision(true);
    CHECK(build(&second.ir, &second_e6, NULL, &b, &second.diagnostics)
        == SOL_MIR_CONCRETE_BUILD_SYMBOL_COLLISION);
    sol_mir_linkage_test_force_runtime_digest_collision(false);
    check_zero(&b); compilation_free(&second);
}

int main(void) {
    Compilation compilation; CHECK(compile_e6(&compilation));
    E6Request e6; e6_request(&compilation.ir, &e6);
    CHECK(e6.root_count == 5);
    for (size_t i = 0; i < 4; ++i) CHECK(e6.imports[i] != SOL_IR_NONE);
    test_lifecycle_requests_and_limits(&compilation, &e6);
    test_unsupported_closure();
    test_e6_complete_and_deterministic(&compilation, &e6);
    test_independent_ir_and_collision(&compilation, &e6);
    compilation_free(&compilation);
    if (failures != 0) {
        fprintf(stderr, "%d MIR concrete test(s) failed\n", failures); return 1;
    }
    printf("MIR concrete tests passed\n"); return 0;
}
