#include "sol/mir_materialize.h"

#include "sol/effects.h"
#include "sol/lexer.h"
#include "sol/ownership.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
            #condition); \
        ++failures; \
    } \
} while (0)

typedef struct {
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree syntax;
    SolHirModule hir;
    SolTypeTable types;
    SolEffectTable effects;
    SolContractTable contracts;
    SolIr ir;
} Compilation;

static bool compile_text(Compilation *compilation, const char *text) {
    memset(compilation, 0, sizeof(*compilation));
    sol_tokens_init(&compilation->tokens);
    sol_diagnostics_init(&compilation->diagnostics);
    sol_syntax_tree_init(&compilation->syntax);
    sol_hir_module_init(&compilation->hir);
    sol_type_table_init(&compilation->types);
    sol_effect_table_init(&compilation->effects);
    sol_contract_table_init(&compilation->contracts);
    sol_ir_init(&compilation->ir);
    return sol_source_from_text(&compilation->source, "materialize.sol", text)
        && sol_lex(&compilation->source, &compilation->tokens,
            &compilation->diagnostics)
        && sol_parse(&compilation->source, &compilation->tokens,
            &compilation->syntax, &compilation->diagnostics)
        && sol_hir_lower(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->diagnostics)
        && sol_type_check(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->diagnostics)
        && sol_effect_check(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->diagnostics)
        && sol_contract_lower(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->diagnostics)
        && sol_ir_lower(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->ir,
            &compilation->diagnostics);
}

static void free_compilation(Compilation *compilation) {
    sol_ir_free(&compilation->ir);
    sol_contract_table_free(&compilation->contracts);
    sol_effect_table_free(&compilation->effects);
    sol_type_table_free(&compilation->types);
    sol_hir_module_free(&compilation->hir);
    sol_syntax_tree_free(&compilation->syntax);
    sol_tokens_free(&compilation->tokens);
    sol_source_free(&compilation->source);
    sol_diagnostics_free(&compilation->diagnostics);
}

static SolIrCallableId callable(const SolIr *ir, const char *name,
    SolIrCallableKind kind) {
    for (size_t id = 0; id < ir->callable_count; ++id) {
        if (ir->callables[id].kind == kind
            && strcmp(ir->callables[id].name, name) == 0) return id;
    }
    return SOL_IR_NONE;
}

static bool build_all(const SolIr *ir, SolIrCallableId root,
    const SolIrCallableId *imports, size_t import_count, SolMirProgram *program,
    SolMirPlan *plan, SolMirMaterialization *materialization,
    const SolMirMaterializeLimits *limits, SolDiagnostics *diagnostics,
    SolMirMaterializeBuildOutcome *outcome) {
    SolMirProgramRoot program_root
        = {root, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirProgramBuildRequest program_request
        = {ir, &program_root, 1, imports, import_count, NULL};
    SolMirPlanBuildRequest plan_request = {program, NULL};
    SolMirMaterializeBuildRequest materialize_request = {plan, limits};
    if (sol_mir_program_build(&program_request, program, diagnostics)
            != SOL_MIR_PROGRAM_BUILD_SUCCEEDED
        || sol_mir_plan_build(&plan_request, plan, diagnostics)
            != SOL_MIR_PLAN_BUILD_SUCCEEDED) return false;
    *outcome = sol_mir_materialize_build(&materialize_request,
        materialization, diagnostics);
    return true;
}

static char *render(const SolMirMaterialization *materialization, size_t *length) {
    FILE *stream = tmpfile();
    if (stream == NULL || !sol_mir_materialization_render(stream,
            materialization) || fflush(stream) != 0 || fseek(stream, 0, SEEK_END) != 0) {
        if (stream != NULL) fclose(stream);
        return NULL;
    }
    long end = ftell(stream);
    if (end < 0 || fseek(stream, 0, SEEK_SET) != 0) { fclose(stream); return NULL; }
    char *text = malloc((size_t)end + 1);
    if (text == NULL) { fclose(stream); return NULL; }
    size_t read = fread(text, 1, (size_t)end, stream);
    fclose(stream);
    if (read != (size_t)end) { free(text); return NULL; }
    text[read] = '\0';
    *length = read;
    return text;
}

static void test_generic_recursion_lifecycle_limits_and_render(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module materialize\n"
        "function identity<T>(value: T) -> T effects { pure } { return value }\n"
        "function recurse(value: Int64) -> Int64 effects { pure } "
        "requires { value >= 0 } ensures { result <= old(value) } { "
        "if value > 0 { return recurse(value - 1) } else { return 0 } }\n"
        "function root(flag: Bool) -> Int64 effects { pure } { "
        "if flag { return identity(1) } else { "
        "if identity(true) { return recurse(2) } else { return 0 } } }\n"));
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    SolMirProgram program;
    SolMirPlan plan;
    SolMirMaterialization first;
    sol_mir_program_init(&program);
    sol_mir_plan_init(&plan);
    sol_mir_materialization_init(&first);
    SolMirMaterializeBuildOutcome outcome;
    CHECK(build_all(&compilation.ir,
        callable(&compilation.ir, "root", SOL_IR_CALLABLE_FUNCTION), NULL, 0,
        &program, &plan, &first, NULL, &diagnostics, &outcome));
    CHECK(outcome == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED);
    CHECK(first.image_count == plan.instance_count && first.image_count == 4);
    CHECK(sol_mir_materialization_validate(&first, NULL));
    size_t identity_count = 0;
    SolMirMaterializedTypeId identity_results[2] = {0};
    for (size_t id = 0; id < first.image_count; ++id) {
        CHECK(first.images[id].instance == id);
        const SolMirProgramTemplate *template = NULL;
        for (size_t item = 0; item < program.template_count; ++item) {
            if (program.templates[item].callable == first.images[id].source_callable) {
                template = &program.templates[item];
            }
        }
        CHECK(template != NULL);
        if (template != NULL && first.images[id].topology.block_count != 0) {
            CHECK(first.images[id].topology.blocks != template->mir.blocks);
            for (size_t other = 0; other < id; ++other) {
                CHECK(first.images[id].topology.blocks
                    != first.images[other].topology.blocks);
            }
        }
        if (first.images[id].source_callable
            == callable(&compilation.ir, "identity", SOL_IR_CALLABLE_FUNCTION)) {
            identity_results[identity_count++] = first.images[id].result;
        }
    }
    CHECK(identity_count == 2 && identity_results[0] != identity_results[1]);
    bool saw_snapshot = false;
    bool saw_obligation = false;
    for (size_t index = 0; index < first.overlay_count; ++index) {
        saw_snapshot = saw_snapshot || first.overlays[index].kind
            == SOL_MIR_PLAN_USE_SNAPSHOT;
        saw_obligation = saw_obligation || first.overlays[index].kind
            == SOL_MIR_PLAN_USE_OBLIGATION_PREDICATE;
    }
    CHECK(saw_snapshot && saw_obligation);
    bool recursion = false;
    for (size_t index = 0; index < first.invoke_binding_count; ++index) {
        recursion = recursion
            || (first.invoke_bindings[index].target_kind
                    == SOL_MIR_MATERIALIZED_TARGET_INSTANCE
                && first.invoke_bindings[index].instance
                    < first.image_count
                && first.images[first.invoke_bindings[index].instance].source_callable
                    == callable(&compilation.ir, "recurse",
                        SOL_IR_CALLABLE_FUNCTION));
    }
    CHECK(recursion);
    size_t first_length = 0;
    char *first_text = render(&first, &first_length);
    CHECK(first_text != NULL && first_length != 0);

    SolMirMaterialization second;
    sol_mir_materialization_init(&second);
    SolMirMaterializeBuildRequest repeat_request = {&plan, NULL};
    CHECK(sol_mir_materialize_build(&repeat_request, &second, &diagnostics)
        == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED);
    size_t second_length = 0;
    char *second_text = render(&second, &second_length);
    CHECK(second_text != NULL && first_length == second_length
        && memcmp(first_text, second_text, first_length) == 0);
    free(first_text); free(second_text);

    SolMirMaterializeLimits exact = {first.usage.instances,
        first.usage.cfg_items, first.usage.bindings, first.usage.owned_bytes,
        first.usage.materialization_work};
    SolMirMaterialization limited;
    sol_mir_materialization_init(&limited);
    SolMirMaterializeBuildRequest limited_request = {&plan, &exact};
    CHECK(sol_mir_materialize_build(&limited_request, &limited, &diagnostics)
        == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED);
    sol_mir_materialization_free(&limited);
#define LIMIT_FAIL(field) do { \
    SolMirMaterializeLimits less = exact; --less.field; \
    limited_request.limits = &less; \
    CHECK(sol_mir_materialize_build(&limited_request, &limited, &diagnostics) \
        == SOL_MIR_MATERIALIZE_BUILD_RESOURCE_EXHAUSTED); \
    CHECK(limited.plan == NULL); \
} while (0)
    LIMIT_FAIL(max_instances);
    LIMIT_FAIL(max_cfg_items);
    LIMIT_FAIL(max_bindings);
    LIMIT_FAIL(max_owned_bytes);
    LIMIT_FAIL(max_materialization_work);
#undef LIMIT_FAIL

    SolMirMaterializedTypeId saved_type = plan.typed_uses[0].type;
    plan.typed_uses[0].type = SOL_MIR_PLAN_NONE;
    limited_request.limits = NULL;
    CHECK(sol_mir_materialize_build(&limited_request, &limited, &diagnostics)
        == SOL_MIR_MATERIALIZE_BUILD_INVALID_PLAN);
    CHECK(limited.plan == NULL);
    plan.typed_uses[0].type = saved_type;

    SolMirPlanInstanceId saved_demand = plan.demands[0].instance;
    plan.demands[0].instance = SOL_MIR_PLAN_NONE;
    CHECK(sol_mir_materialize_build(&limited_request, &limited, &diagnostics)
        == SOL_MIR_MATERIALIZE_BUILD_INVALID_PLAN);
    CHECK(limited.plan == NULL);
    plan.demands[0].instance = saved_demand;

    SolMirBlock *saved_blocks = first.images[0].topology.blocks;
    first.images[0].topology.blocks = first.images[1].topology.blocks;
    CHECK(!sol_mir_materialization_validate(&first, NULL));
    FILE *empty = tmpfile();
    CHECK(empty != NULL);
    if (empty != NULL) {
        CHECK(!sol_mir_materialization_render(empty, &first));
        CHECK(ftell(empty) == 0);
        fclose(empty);
    }
    first.images[0].topology.blocks = saved_blocks;
    CHECK(sol_mir_materialization_validate(&first, NULL));
    const SolMirProgramTemplate *borrowed_template = NULL;
    for (size_t index = 0; index < program.template_count; ++index) {
        if (program.templates[index].callable == first.images[0].source_callable) {
            borrowed_template = &program.templates[index];
        }
    }
    CHECK(borrowed_template != NULL);
    if (borrowed_template != NULL) {
        first.images[0].topology.blocks = borrowed_template->mir.blocks;
        CHECK(!sol_mir_materialization_validate(&first, NULL));
        first.images[0].topology.blocks = saved_blocks;
    }
    size_t image_capacity = first.image_capacity;
    ++first.image_capacity;
    CHECK(!sol_mir_materialization_validate(&first, NULL));
    first.image_capacity = image_capacity;
    SolMirMaterializedTypeId *owned_types = first.type_ids;
    first.type_ids = plan.instance_type_ids;
    CHECK(!sol_mir_materialization_validate(&first, NULL));
    first.type_ids = compilation.ir.type_ids;
    CHECK(!sol_mir_materialization_validate(&first, NULL));
    first.type_ids = owned_types;
    SolMirPlanSlice saved_slice = first.images[0].overlays;
    first.images[0].overlays.count = first.overlay_count + 1;
    CHECK(!sol_mir_materialization_validate(&first, NULL));
    first.images[0].overlays = saved_slice;
    SolMirMaterializedTypeId saved_overlay = first.overlays[0].type;
    first.overlays[0].type = plan.type_count;
    CHECK(!sol_mir_materialization_validate(&first, NULL));
    first.overlays[0].type = saved_overlay;
    CHECK(sol_mir_materialization_validate(&first, NULL));
    sol_mir_materialization_free(&limited);
    sol_mir_materialization_free(&second);
    sol_mir_materialization_free(&first);
    sol_mir_plan_free(&plan);
    sol_mir_program_free(&program);
    sol_diagnostics_free(&diagnostics);
    free_compilation(&compilation);
}

static void test_evidence_and_import_bindings(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module dispatch\n"
        "capability Host { function read() -> Int64 effects { service.read<Self> } }\n"
        "trait Score { function score(self: Self) -> Int64 effects { pure } }\n"
        "implementation Score for Int64 { function score(self: Self) -> Int64 "
        "effects { pure } { return self } }\n"
        "function scored<T: Score>(value: T) -> Int64 effects { pure } "
        "{ return value.score() }\n"
        "function forward<U: Score>(value: U) -> Int64 effects { pure } "
        "{ return scored(value) }\n"
        "function root(host: capability Host) -> Int64 "
        "effects { service.read<host> } { return forward(4) + host.read() }\n"));
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    SolMirProgram program;
    SolMirPlan plan;
    SolMirMaterialization materialization;
    sol_mir_program_init(&program);
    sol_mir_plan_init(&plan);
    sol_mir_materialization_init(&materialization);
    SolIrCallableId import = callable(&compilation.ir, "read",
        SOL_IR_CALLABLE_CAPABILITY);
    SolMirMaterializeBuildOutcome outcome;
    CHECK(build_all(&compilation.ir,
        callable(&compilation.ir, "root", SOL_IR_CALLABLE_FUNCTION), &import, 1,
        &program, &plan, &materialization, NULL, &diagnostics, &outcome));
    CHECK(outcome == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED);
    bool saw_import = false;
    bool saw_method = false;
    for (size_t index = 0; index < materialization.invoke_binding_count; ++index) {
        const SolMirMaterializedInvokeBinding *binding
            = &materialization.invoke_bindings[index];
        saw_import = saw_import
            || binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_IMPORT;
        saw_method = saw_method || (binding->dispatch_trait != SOL_IR_NONE
            && binding->dispatch_requirement != SOL_IR_NONE
            && binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE);
    }
    CHECK(saw_import && saw_method);
    CHECK(sol_mir_materialization_validate(&materialization, NULL));
    if (materialization.invoke_binding_count != 0) {
        SolMirPlanInstanceId saved = materialization.invoke_bindings[0].instance;
        materialization.invoke_bindings[0].instance = SOL_MIR_PLAN_NONE;
        CHECK(!sol_mir_materialization_validate(&materialization, NULL));
        materialization.invoke_bindings[0].instance = saved;
    }
    sol_mir_materialization_free(&materialization);
    sol_mir_plan_free(&plan);
    sol_mir_program_free(&program);
    sol_diagnostics_free(&diagnostics);
    free_compilation(&compilation);
}

static void test_handler_boundary_is_transactional(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module handlers\n"
        "capability Source { function read() -> Int64 "
        "effects { service.read<Self> } }\n"
        "capability Provider { function read() -> Int64 effects { pure } }\n"
        "function root(source: capability Source, provider: capability Provider) "
        "-> Int64 { return handle service.read<source> with provider { "
        "source.read() } }\n"));
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    SolMirProgram program;
    SolMirPlan plan;
    SolMirMaterialization materialization;
    sol_mir_program_init(&program);
    sol_mir_plan_init(&plan);
    sol_mir_materialization_init(&materialization);
    SolIrCallableId imports[] = {
        callable(&compilation.ir, "read", SOL_IR_CALLABLE_CAPABILITY),
        SOL_IR_NONE,
    };
    for (size_t id = imports[0] + 1; id < compilation.ir.callable_count; ++id) {
        if (compilation.ir.callables[id].kind == SOL_IR_CALLABLE_CAPABILITY
            && strcmp(compilation.ir.callables[id].name, "read") == 0) {
            imports[1] = id;
        }
    }
    SolMirMaterializeBuildOutcome outcome;
    CHECK(build_all(&compilation.ir,
        callable(&compilation.ir, "root", SOL_IR_CALLABLE_FUNCTION), imports, 2,
        &program, &plan, &materialization, NULL, &diagnostics, &outcome));
    CHECK(outcome == SOL_MIR_MATERIALIZE_BUILD_UNSUPPORTED_OR_UNRESOLVED);
    CHECK(materialization.plan == NULL && materialization.images == NULL);
    sol_mir_materialization_free(&materialization);
    sol_mir_plan_free(&plan);
    sol_mir_program_free(&program);
    sol_diagnostics_free(&diagnostics);
    free_compilation(&compilation);
}

int main(void) {
    test_generic_recursion_lifecycle_limits_and_render();
    test_evidence_and_import_bindings();
    test_handler_boundary_is_transactional();
    if (failures != 0) {
        fprintf(stderr, "%d MIR materialization test(s) failed\n", failures);
        return 1;
    }
    printf("MIR materialization tests passed\n");
    return 0;
}
