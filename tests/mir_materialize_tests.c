#include "sol/mir_materialize.h"

#include "sol/effects.h"
#include "sol/lexer.h"
#include "sol/ownership.h"
#include "sol/package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

bool sol_mir_materialization_validate_concrete(
    const SolMirMaterialization *owner, SolDiagnostics *diagnostics);
bool sol_mir_materialization_test_path_frontier(void);
bool sol_mir_materialization_test_callable_site_target_equality(void);
bool sol_mir_materialization_validation_work(
    const SolMirMaterialization *owner, size_t *work);

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
        "let message = \"owned literal\" "
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
    CHECK(first.binding_count == plan.demand_count);
    CHECK(first.import_count == plan.import_count);
    CHECK(first.block_count != 0 && first.edge_count != 0
        && first.parameter_value_count != 0);
    CHECK(first.literal_byte_count == strlen("owned literal") + 1);
    CHECK(strcmp(first.literal_bytes, "owned literal") == 0);
    CHECK(sol_mir_materialization_validate(&first, NULL));
    bool contract_edge_corrupted = false;
    for (size_t i = 0; i < first.block_count; ++i) {
        SolMirMaterializedTerminator *term = &first.blocks[i].terminator;
        if (!contract_edge_corrupted
            && term->kind == SOL_MIR_TERM_CHECK_CONTRACT) {
            size_t saved = term->violation_edge;
            term->violation_edge = term->satisfied_edge;
            CHECK(!sol_mir_materialization_validate_concrete(&first, NULL));
            term->violation_edge = saved;
            contract_edge_corrupted = true;
        }
    }
    CHECK(contract_edge_corrupted);
    bool snapshot_type_corrupted = false;
    for (size_t i = 0; i < first.instruction_count; ++i) {
        SolMirMaterializedInstruction *instruction = &first.instructions[i];
        if (instruction->kind != SOL_MIR_INST_CAPTURE_SNAPSHOT) continue;
        size_t saved = instruction->type;
        instruction->type = (saved + 1) % first.type_count;
        CHECK(!sol_mir_materialization_validate_concrete(&first, NULL));
        instruction->type = saved; snapshot_type_corrupted = true; break;
    }
    CHECK(snapshot_type_corrupted);
    CHECK(sol_mir_materialization_validate_concrete(&first, NULL));
    size_t validation_limit = first.limits.max_validation_work;
    first.limits.max_validation_work = first.usage.validation_work - 1;
    CHECK(!sol_mir_materialization_validate_concrete(&first, NULL));
    CHECK(!sol_mir_materialization_validate(&first, NULL));
    first.limits.max_validation_work = validation_limit;
    size_t concrete_records = first.usage.concrete_records;
    first.usage.concrete_records = SIZE_MAX;
    size_t overflow_work = 0;
    CHECK(!sol_mir_materialization_validation_work(&first, &overflow_work));
    CHECK(!sol_mir_materialization_validate_concrete(&first, NULL));
    first.usage.concrete_records = concrete_records;
    SolMirTerminatorKind concrete_kind = first.blocks[0].terminator.kind;
    first.blocks[0].terminator.kind = SOL_MIR_TERM_INVALID;
    CHECK(!sol_mir_materialization_validate(&first, NULL));
    first.blocks[0].terminator.kind = concrete_kind;
    SolSpan concrete_span = first.blocks[0].terminator.span;
    ++first.blocks[0].terminator.span.end;
    CHECK(!sol_mir_materialization_validate(&first, NULL));
    first.blocks[0].terminator.span = concrete_span;
    if (first.image_count > 1 && first.images[0].values.count != 0
        && first.images[1].blocks.count != 0) {
        SolMirMaterializedBlockId saved = first.values[
            first.images[0].values.offset].block;
        first.values[first.images[0].values.offset].block
            = first.images[1].blocks.offset;
        CHECK(!sol_mir_materialization_validate(&first, NULL));
        first.values[first.images[0].values.offset].block = saved;
    }
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
    for (size_t index = 0; index < first.binding_count; ++index) {
        if (first.bindings[index].kind != SOL_MIR_PLAN_DEMAND_INVOKE
            && first.bindings[index].kind != SOL_MIR_PLAN_DEMAND_CALLBACK) continue;
        recursion = recursion
            || (first.bindings[index].target_kind
                    == SOL_MIR_MATERIALIZED_TARGET_INSTANCE
                && first.bindings[index].instance
                    < first.image_count
                && first.images[first.bindings[index].instance].source_callable
                    == callable(&compilation.ir, "recurse",
                        SOL_IR_CALLABLE_FUNCTION));
    }
    CHECK(recursion);
    size_t first_length = 0;
    char *first_text = render(&first, &first_length);
    CHECK(first_text != NULL && first_length != 0);
    if (first_text != NULL) {
        CHECK(strstr(first_text, "type t") != NULL);
        CHECK(strstr(first_text, "context k") != NULL);
        CHECK(strstr(first_text, "  local l") != NULL);
        CHECK(strstr(first_text, "  place p") != NULL);
        CHECK(strstr(first_text, "  value v") != NULL);
        CHECK(strstr(first_text, "  instruction n") != NULL);
    }

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

    SolMirMaterializeLimits exact = {
        .max_instances = first.usage.instances,
        .max_cfg_items = first.usage.cfg_items,
        .max_bindings = first.usage.bindings,
        .max_concrete_records = first.usage.concrete_records,
        .max_owned_bytes = first.usage.owned_bytes,
        .max_materialization_work = first.usage.materialization_work,
        .max_shape_resolution_work = first.usage.shape_resolution_work,
        .max_validation_work = first.usage.validation_work,
    };
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
    LIMIT_FAIL(max_concrete_records);
    LIMIT_FAIL(max_owned_bytes);
    LIMIT_FAIL(max_materialization_work);
    LIMIT_FAIL(max_shape_resolution_work);
    LIMIT_FAIL(max_validation_work);
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
    SolMirPlanContextId saved_context = first.overlays[0].context;
    first.overlays[0].context = SOL_MIR_PLAN_NONE;
    CHECK(!sol_mir_materialization_validate(&first, NULL));
    first.overlays[0].context = saved_context;
    bool saved_copy = first.types[0].is_copy;
    first.types[0].is_copy = !saved_copy;
    CHECK(!sol_mir_materialization_validate(&first, NULL));
    first.types[0].is_copy = saved_copy;
    SolMirMaterializedType *saved_types = first.types;
    first.types = (SolMirMaterializedType *)(void *)first.images;
    CHECK(!sol_mir_materialization_validate(&first, NULL));
    first.types = saved_types;
    empty = tmpfile();
    CHECK(empty != NULL);
    if (empty != NULL) {
        first.types[0].is_copy = !saved_copy;
        CHECK(!sol_mir_materialization_render(empty, &first));
        CHECK(ftell(empty) == 0);
        first.types[0].is_copy = saved_copy;
        fclose(empty);
    }
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
    for (size_t index = 0; index < materialization.binding_count; ++index) {
        const SolMirMaterializedBinding *binding = &materialization.bindings[index];
        if (binding->kind != SOL_MIR_PLAN_DEMAND_INVOKE
            && binding->kind != SOL_MIR_PLAN_DEMAND_CALLBACK) continue;
        saw_import = saw_import
            || binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_IMPORT;
        saw_method = saw_method || (binding->dispatch_trait != SOL_IR_NONE
            && binding->dispatch_requirement != SOL_IR_NONE
            && binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE);
    }
    CHECK(saw_import && saw_method);
    CHECK(sol_mir_materialization_validate(&materialization, NULL));
    bool bad_edge = false, bad_instance = false, bad_import = false;
    for (size_t i = 0; i < materialization.block_count && !bad_edge; ++i) {
        SolMirMaterializedTerminator *term = &materialization.blocks[i].terminator;
        size_t *edge = NULL;
        if (term->kind == SOL_MIR_TERM_GOTO) edge = &term->edge;
        else if (term->kind == SOL_MIR_TERM_INVOKE) edge = &term->failure_edge;
        else if (term->kind == SOL_MIR_TERM_BRANCH) edge = &term->true_edge;
        if (edge == NULL) continue;
        size_t saved = *edge; *edge = SIZE_MAX;
        CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
        *edge = saved; bad_edge = true;
    }
    for (size_t i = 0; i < materialization.binding_count; ++i) {
        SolMirMaterializedBinding *binding = &materialization.bindings[i];
        if (!bad_instance
            && binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE) {
            size_t saved = binding->instance; binding->instance = SIZE_MAX;
            CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
            binding->instance = saved; bad_instance = true;
        }
        if (!bad_import
            && binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_IMPORT) {
            size_t saved = binding->import; binding->import = SIZE_MAX;
            CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
            binding->import = saved; bad_import = true;
        }
    }
    CHECK(bad_edge && bad_instance && bad_import);
    if (materialization.access_count != 0) {
        SolAccessMode saved = materialization.accesses[0];
        materialization.accesses[0] = (SolAccessMode)99;
        CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
        materialization.accesses[0] = saved;
    }
    bool receiver_access = false;
    for (size_t i = 0; i < materialization.local_count; ++i) {
        SolMirMaterializedLocal *local = &materialization.locals[i];
        if (local->kind != SOL_MIR_MATERIALIZED_LOCAL_RECEIVER) continue;
        SolAccessMode saved = local->access;
        local->access = saved == SOL_ACCESS_SHARED
            ? SOL_ACCESS_EXCLUSIVE : SOL_ACCESS_SHARED;
        CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
        local->access = saved; receiver_access = true; break;
    }
    CHECK(receiver_access);
    if (materialization.binding_count != 0) {
        SolMirPlanInstanceId saved = materialization.bindings[0].instance;
        materialization.bindings[0].instance = SOL_MIR_PLAN_NONE;
        CHECK(!sol_mir_materialization_validate(&materialization, NULL));
        materialization.bindings[0].instance = saved;
    }
    sol_mir_materialization_free(&materialization);
    sol_mir_plan_free(&plan);
    sol_mir_program_free(&program);
    sol_diagnostics_free(&diagnostics);
    free_compilation(&compilation);
}

static void test_handlers_are_materialized(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module handlers\n"
        "capability Source { function read() -> Int64 "
        "effects { service.read<Self> } }\n"
        "capability Provider { function read() -> Int64 effects { pure } }\n"
        "function leaf() -> Int64 effects { pure } { return 1 }\n"
        "function helper() -> Int64 effects { pure } { return leaf() }\n"
        "function root(source: capability Source, first: capability Provider, "
        "second: capability Provider) -> Int64 { "
        "return helper() + handle service.read<source> with first { "
        "handle service.read<source> with second { source.read() } } }\n"));
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
    CHECK(outcome == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED);
    CHECK(materialization.handler_count == 2);
    CHECK(materialization.handlers[0].source_binding < materialization.binding_count);
    CHECK(materialization.handlers[0].provider_binding < materialization.binding_count);
    CHECK(materialization.handlers[0].authority < materialization.place_count);
    CHECK(materialization.handlers[0].provider < materialization.place_count);
    CHECK(sol_mir_materialization_validate(&materialization, NULL));
    CHECK(sol_mir_materialization_validate_concrete(&materialization, NULL));
    SolMirPlanContextId saved_handler_context = materialization.handlers[0].context;
    materialization.handlers[0].context = materialization.context_count;
    CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
    materialization.handlers[0].context = saved_handler_context;
    size_t saved_source_effect = materialization.handlers[0].source_effect;
    materialization.handlers[0].source_effect = SOL_IR_NONE;
    CHECK(sol_mir_materialization_validate_concrete(&materialization, NULL));
    materialization.handlers[0].source_effect = saved_source_effect;
    SolIrCallableId saved_symbolic = materialization.bindings[
        materialization.handlers[0].source_binding].symbolic_callable;
    materialization.bindings[materialization.handlers[0].source_binding]
        .symbolic_callable = SOL_IR_NONE;
    CHECK(sol_mir_materialization_validate_concrete(&materialization, NULL));
    materialization.bindings[materialization.handlers[0].source_binding]
        .symbolic_callable = saved_symbolic;
    SolMirMaterializedPlaceId operation_root
        = materialization.handlers[0].operation.root;
    materialization.handlers[0].operation.root = materialization.place_count;
    CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
    materialization.handlers[0].operation.root = operation_root;
    SolMirMaterializedEffectRowId operation_effects
        = materialization.handlers[0].operation.effects;
    materialization.handlers[0].operation.effects = materialization.effect_row_count;
    CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
    materialization.handlers[0].operation.effects = operation_effects;
    SolMirMaterializedTargetKind operation_target
        = materialization.handlers[0].operation.target_kind;
    materialization.handlers[0].operation.target_kind
        = operation_target == SOL_MIR_MATERIALIZED_TARGET_INSTANCE
            ? SOL_MIR_MATERIALIZED_TARGET_IMPORT
            : SOL_MIR_MATERIALIZED_TARGET_INSTANCE;
    CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
    materialization.handlers[0].operation.target_kind = operation_target;
    const SolMirMaterializedBinding *handler_source = &materialization.bindings[
        materialization.handlers[0].source_binding];
    if (handler_source->target_kind == SOL_MIR_MATERIALIZED_TARGET_IMPORT) {
        size_t target = handler_source->import;
        size_t saved = materialization.imports[target].effects;
        materialization.imports[target].effects = materialization.effect_row_count;
        CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
        materialization.imports[target].effects = saved;
    }
    SolMirMaterializedBindingId saved_provider
        = materialization.handlers[0].provider_binding;
    materialization.handlers[0].provider_binding
        = materialization.handlers[0].source_binding;
    CHECK(!sol_mir_materialization_validate(&materialization, NULL));
    materialization.handlers[0].provider_binding = saved_provider;
    SolMirMaterializedPlaceId saved_place = materialization.handlers[0].provider;
    materialization.handlers[0].provider = materialization.handlers[0].authority;
    CHECK(!sol_mir_materialization_validate(&materialization, NULL));
    materialization.handlers[0].provider = saved_place;
    bool changed_marker = false;
    for (size_t i = 0; i < materialization.instruction_count; ++i) {
        if (materialization.instructions[i].kind != SOL_MIR_INST_HANDLER_ENTER) continue;
        SolMirMaterializedHandlerId saved = materialization.instructions[i].handler;
        materialization.instructions[i].handler = materialization.handler_count;
        CHECK(!sol_mir_materialization_validate(&materialization, NULL));
        materialization.instructions[i].handler = saved;
        changed_marker = true;
        break;
    }
    CHECK(changed_marker && sol_mir_materialization_validate(&materialization, NULL));
    sol_mir_materialization_free(&materialization);
    sol_mir_plan_free(&plan);
    sol_mir_program_free(&program);
    sol_diagnostics_free(&diagnostics);
    free_compilation(&compilation);
}

static size_t other_value(const SolMirMaterialization *owner, size_t image) {
    for (size_t id = 0; id < owner->image_count; ++id) {
        if (id != image && owner->images[id].values.count != 0) {
            return owner->images[id].values.offset;
        }
    }
    return SOL_MIR_MATERIALIZED_NONE;
}

static size_t other_temporary(const SolMirMaterialization *owner, size_t image) {
    for (size_t id = 0; id < owner->image_count; ++id) {
        if (id != image && owner->images[id].temporaries.count != 0) {
            return owner->images[id].temporaries.offset;
        }
    }
    return SOL_MIR_MATERIALIZED_NONE;
}

static size_t other_place(const SolMirMaterialization *owner, size_t image) {
    for (size_t id = 0; id < owner->image_count; ++id) {
        if (id != image && owner->images[id].places.count != 0) {
            return owner->images[id].places.offset;
        }
    }
    return SOL_MIR_MATERIALIZED_NONE;
}

static size_t other_construct_operands(const SolMirMaterialization *owner,
    size_t image) {
    for (size_t id = 0; id < owner->image_count; ++id) {
        if (id != image && owner->images[id].construct_operands.count != 0) {
            return owner->images[id].construct_operands.offset;
        }
    }
    return SOL_MIR_MATERIALIZED_NONE;
}

static void test_refinement_contexts_copy_and_concrete_records(void) {
    Compilation compilation;
    bool compiled = compile_text(&compilation,
        "module concrete_records\n"
        "capability Token {}\n"
        "record Box<T> { value: T }\n"
        "type Identity<T> = refined T where true\n"
        "function project_int(box: Box<Int64>) -> Int64 { "
        "let pair = (box.value, 0) return pair.0 }\n"
        "function project_bool(box: Box<Bool>) -> Bool { "
        "let pair = (box.value, false) return pair.0 }\n"
        "function root(token: capability Token, flag: Bool) -> Int64 { "
        "let copied = Box<Int64> { value = 1 } "
        "let owned = Box<capability Token> { value = token } "
        "let projected = project_int(copied) "
        "let truth = project_bool(Box<Bool> { value = true }) "
        "let sum = projected + 1 "
        "let passthrough = { sum } "
        "if flag { let first = Identity<Int64>(passthrough) return 1 } "
        "else { let second = Identity<Bool>(truth) return 2 } }\n");
    if (!compiled) sol_diagnostics_render_human(stderr, &compilation.source,
        &compilation.diagnostics);
    CHECK(compiled);
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    SolMirProgram program;
    SolMirPlan plan;
    SolMirMaterialization materialization;
    sol_mir_program_init(&program);
    sol_mir_plan_init(&plan);
    sol_mir_materialization_init(&materialization);
    SolMirMaterializeBuildOutcome outcome;
    bool built = build_all(&compilation.ir,
        callable(&compilation.ir, "root", SOL_IR_CALLABLE_FUNCTION), NULL, 0,
        &program, &plan, &materialization, NULL, &diagnostics, &outcome);
    CHECK(built);
    if (!built || outcome != SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED) {
        sol_diagnostics_render_human(stderr, &compilation.source, &diagnostics);
    }
    CHECK(outcome == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED);
    size_t refinement_contexts = 0;
    for (size_t context = 0; context < plan.context_count; ++context) {
        refinement_contexts += plan.contexts[context].kind
            == SOL_MIR_PLAN_CONTEXT_REFINEMENT;
    }
    CHECK(refinement_contexts == 2);
    CHECK(materialization.overlay_count == plan.typed_use_count);
    bool copy_box = false;
    bool owned_box = false;
    for (size_t type = 0; type < materialization.type_count; ++type) {
        const SolMirMaterializedType *item = &materialization.types[type];
        if (item->kind != SOL_IR_TYPE_NOMINAL || item->arguments.count != 1) continue;
        SolMirMaterializedTypeId argument
            = materialization.type_ids[item->arguments.offset];
        if (materialization.types[argument].kind == SOL_IR_TYPE_INT64) {
            copy_box = copy_box || item->is_copy;
        }
        if (materialization.types[argument].kind == SOL_IR_TYPE_NOMINAL
            && materialization.plan->program->ir->definitions[
                materialization.types[argument].definition].kind
                == SOL_IR_DEFINITION_CAPABILITY) {
            owned_box = owned_box || !item->is_copy;
        }
    }
    CHECK(copy_box && owned_box);
    CHECK(materialization.local_count != 0 && materialization.place_count != 0
        && materialization.projection_count != 0
        && materialization.value_count != 0
        && materialization.instruction_count != 0
        && materialization.temporary_count != 0
        && materialization.construct_operand_count != 0);
    for (size_t left = 0; left < materialization.place_count; ++left) {
        for (size_t right = left + 1; right < materialization.place_count; ++right) {
            CHECK(materialization.places[left].instance
                    != materialization.places[right].instance
                || materialization.places[left].source_place
                    != materialization.places[right].source_place
                || materialization.places[left].local
                    != materialization.places[right].local);
        }
    }
    CHECK(sol_mir_materialization_validate(&materialization, NULL));
    bool mutated_binary = false;
    bool mutated_expression = false;
    bool mutated_temporary = false;
    bool mutated_place = false;
    bool mutated_construct = false;
    bool mutated_source = false;
    for (size_t image = 0; image < materialization.image_count; ++image) {
        SolMirMaterializedImage *item = &materialization.images[image];
        size_t foreign_value = other_value(&materialization, image);
        size_t foreign_temporary = other_temporary(&materialization, image);
        size_t foreign_place = other_place(&materialization, image);
        size_t foreign_operands
            = other_construct_operands(&materialization, image);
        for (size_t offset = 0; offset < item->instructions.count; ++offset) {
            SolMirMaterializedInstruction *instruction
                = &materialization.instructions[item->instructions.offset + offset];
            if (!mutated_binary && instruction->kind == SOL_MIR_INST_BINARY
                && foreign_value != SOL_MIR_MATERIALIZED_NONE) {
                size_t saved = instruction->left;
                instruction->left = foreign_value;
                CHECK(!sol_mir_materialization_validate(&materialization, NULL));
                instruction->left = saved;
                mutated_binary = true;
            }
            if (!mutated_expression
                && instruction->kind == SOL_MIR_INST_EXPRESSION_RESULT
                && foreign_value != SOL_MIR_MATERIALIZED_NONE) {
                size_t saved = instruction->left;
                instruction->left = foreign_value;
                CHECK(!sol_mir_materialization_validate(&materialization, NULL));
                instruction->left = saved;
                mutated_expression = true;
            }
            if (!mutated_temporary
                && instruction->temporary != SOL_MIR_MATERIALIZED_NONE
                && foreign_temporary != SOL_MIR_MATERIALIZED_NONE) {
                size_t saved = instruction->temporary;
                instruction->temporary = foreign_temporary;
                CHECK(!sol_mir_materialization_validate(&materialization, NULL));
                instruction->temporary = saved;
                mutated_temporary = true;
            }
            if (!mutated_place && instruction->place != SOL_MIR_MATERIALIZED_NONE
                && foreign_place != SOL_MIR_MATERIALIZED_NONE) {
                size_t saved = instruction->place;
                instruction->place = foreign_place;
                CHECK(!sol_mir_materialization_validate(&materialization, NULL));
                instruction->place = saved;
                mutated_place = true;
            }
            if (!mutated_construct && instruction->kind == SOL_MIR_INST_CONSTRUCT
                && instruction->construct_operands.count != 0
                && foreign_operands != SOL_MIR_MATERIALIZED_NONE) {
                size_t saved_offset = instruction->construct_operands.offset;
                SolIrDefinitionId saved_definition
                    = instruction->construct_definition;
                instruction->construct_operands.offset = foreign_operands;
                CHECK(!sol_mir_materialization_validate(&materialization, NULL));
                instruction->construct_operands.offset = saved_offset;
                instruction->construct_definition = saved_definition == SOL_IR_NONE
                    ? 0 : SOL_IR_NONE;
                CHECK(!sol_mir_materialization_validate(&materialization, NULL));
                instruction->construct_definition = saved_definition;
                size_t saved_roots = instruction->source_capability_roots.count;
                ++instruction->source_capability_roots.count;
                CHECK(!sol_mir_materialization_validate(&materialization, NULL));
                instruction->source_capability_roots.count = saved_roots;
                mutated_construct = true;
            }
            if (!mutated_source
                && instruction->source_expression != SOL_IR_NONE) {
                size_t saved = instruction->source_expression;
                instruction->source_expression = SOL_IR_NONE;
                CHECK(!sol_mir_materialization_validate(&materialization, NULL));
                instruction->source_expression = saved;
                mutated_source = true;
            }
        }
    }
    CHECK(mutated_binary);
    CHECK(mutated_expression);
    CHECK(mutated_temporary);
    CHECK(mutated_place);
    CHECK(mutated_construct);
    CHECK(mutated_source);
    bool mutated_construct_temporary = false;
    for (size_t image = 0; image < materialization.image_count
        && !mutated_construct_temporary; ++image) {
        SolMirMaterializedImage *item = &materialization.images[image];
        if (item->temporaries.count < 2 || item->construct_operands.count == 0) {
            continue;
        }
        SolMirMaterializedConstructOperand *operand
            = &materialization.construct_operands[
                item->construct_operands.offset];
        size_t saved = operand->temporary;
        size_t relative = saved - item->temporaries.offset;
        operand->temporary = item->temporaries.offset
            + (relative + 1) % item->temporaries.count;
        CHECK(!sol_mir_materialization_validate(&materialization, NULL));
        operand->temporary = saved;
        mutated_construct_temporary = true;
    }
    CHECK(mutated_construct_temporary);
    bool mutated_projection = false;
    for (size_t left = 0; left < materialization.place_count; ++left) {
        if (materialization.places[left].projections.count == 0) continue;
        for (size_t right = left + 1; right < materialization.place_count; ++right) {
            if (materialization.places[right].instance
                    == materialization.places[left].instance
                || materialization.places[right].projections.count == 0) continue;
            size_t saved = materialization.places[left].projections.offset;
            materialization.places[left].projections.offset
                = materialization.places[right].projections.offset;
            CHECK(!sol_mir_materialization_validate(&materialization, NULL));
            materialization.places[left].projections.offset = saved;
            mutated_projection = true;
            break;
        }
        if (mutated_projection) break;
    }
    CHECK(mutated_projection);
    if (materialization.place_count != 0) {
        SolMirMaterializedTypeId saved = materialization.places[0].final_type;
        materialization.places[0].final_type = materialization.type_count;
        CHECK(!sol_mir_materialization_validate(&materialization, NULL));
        materialization.places[0].final_type = saved;
    }
    if (materialization.instruction_count != 0) {
        SolMirMaterializedPlaceId saved = materialization.instructions[0].place;
        materialization.instructions[0].place = materialization.place_count;
        CHECK(!sol_mir_materialization_validate(&materialization, NULL));
        materialization.instructions[0].place = saved;
    }
    sol_mir_materialization_free(&materialization);
    sol_mir_plan_free(&plan);
    sol_mir_program_free(&program);
    sol_diagnostics_free(&diagnostics);
    free_compilation(&compilation);
}

static void test_recursive_nominal_planning_and_copy_cycle(void) {
    Compilation compilation;
    bool compiled = compile_text(&compilation,
        "module recursive_nominals\n"
        "capability Token {}\n"
        "record Node { next: Option<Node> }\n"
        "enum Chain<T> { end, next(value: Chain<T>), item(value: T) }\n"
        "function root(node: Node, chain: Chain<Int64>, "
        "owned: Chain<capability Token>) -> Int64 { return 0 }\n");
    if (!compiled) sol_diagnostics_render_human(stderr, &compilation.source,
        &compilation.diagnostics);
    CHECK(compiled);
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    SolMirProgram program;
    SolMirPlan plan;
    SolMirMaterialization materialization;
    sol_mir_program_init(&program);
    sol_mir_plan_init(&plan);
    sol_mir_materialization_init(&materialization);
    SolMirMaterializeBuildOutcome outcome;
    CHECK(build_all(&compilation.ir,
        callable(&compilation.ir, "root", SOL_IR_CALLABLE_FUNCTION), NULL, 0,
        &program, &plan, &materialization, NULL, &diagnostics, &outcome));
    if (outcome != SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED) {
        sol_diagnostics_render_human(stderr, &compilation.source, &diagnostics);
    }
    CHECK(outcome == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED);
    bool node_cycle = false;
    bool generic_copy_cycle = false;
    bool generic_noncopy_cycle = false;
    bool node_shape = false, chain_shape = false;
    for (size_t id = 0; id < materialization.type_count; ++id) {
        const SolMirMaterializedType *type = &materialization.types[id];
        if (type->kind != SOL_IR_TYPE_NOMINAL) continue;
        const char *name = compilation.ir.definitions[type->definition].name;
        for (size_t component = 0; component < type->ownership_components.count;
            ++component) {
            SolMirMaterializedTypeId child = materialization.type_ids[
                type->ownership_components.offset + component];
            if (strcmp(name, "Node") == 0) {
                const SolMirMaterializedType *option
                    = &materialization.types[child];
                node_cycle = option->kind == SOL_IR_TYPE_OPTION
                    && option->ownership_components.count == 1
                    && materialization.type_ids[
                        option->ownership_components.offset] == id
                    && type->is_copy && option->is_copy;
                node_shape = type->fields.count == 1
                    && materialization.shape_fields[type->fields.offset].ordinal == 0
                    && materialization.shape_fields[type->fields.offset].type == child;
            } else if (strcmp(name, "Chain") == 0) {
                bool self_cycle = child == id;
                SolMirMaterializedTypeId argument = materialization.type_ids[
                    type->arguments.offset];
                if (materialization.types[argument].kind == SOL_IR_TYPE_INT64) {
                    generic_copy_cycle = generic_copy_cycle
                        || (self_cycle && type->is_copy);
                } else {
                    generic_noncopy_cycle = generic_noncopy_cycle
                        || (self_cycle && !type->is_copy);
                }
                chain_shape = chain_shape || (type->variants.count == 3
                    && materialization.shape_variants[type->variants.offset].ordinal == 0
                    && materialization.shape_variants[type->variants.offset + 1].ordinal == 1
                    && materialization.shape_variants[type->variants.offset + 2].ordinal == 2);
            }
        }
    }
    CHECK(node_cycle && generic_copy_cycle && generic_noncopy_cycle);
    CHECK(node_shape && chain_shape && materialization.shape_field_count != 0
        && materialization.shape_variant_count != 0);
    CHECK(sol_mir_plan_validate(&plan, NULL));
    CHECK(sol_mir_materialization_validate(&materialization, NULL));
    bool capability_copy = false, category = false, fixed_point = false;
    for (size_t id = 0; id < materialization.type_count; ++id) {
        SolMirMaterializedType *type = &materialization.types[id];
        if (!capability_copy && type->nominal_category
                == SOL_MIR_MATERIALIZED_NOMINAL_CAPABILITY) {
            type->is_copy = true;
            CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
            type->is_copy = false; capability_copy = true;
        }
        if (!category && type->kind == SOL_IR_TYPE_NOMINAL && type->is_copy
            && type->nominal_category != SOL_MIR_MATERIALIZED_NOMINAL_CAPABILITY) {
            SolMirMaterializedNominalCategory saved = type->nominal_category;
            type->nominal_category = SOL_MIR_MATERIALIZED_NOMINAL_CAPABILITY;
            CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
            type->nominal_category = saved; category = true;
        }
        if (!fixed_point && type->kind == SOL_IR_TYPE_NOMINAL && !type->is_copy
            && type->nominal_category != SOL_MIR_MATERIALIZED_NOMINAL_CAPABILITY) {
            type->is_copy = true;
            CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
            type->is_copy = false; fixed_point = true;
        }
    }
    CHECK(capability_copy && category && fixed_point);
    if (materialization.shape_field_count != 0) {
        size_t saved = materialization.shape_fields[0].ordinal;
        ++materialization.shape_fields[0].ordinal;
        CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
        materialization.shape_fields[0].ordinal = saved;
        SolIrFieldId source = materialization.shape_fields[0].source_field;
        materialization.shape_fields[0].source_field = SOL_IR_NONE;
        CHECK(!sol_mir_materialization_validate(&materialization, NULL));
        materialization.shape_fields[0].source_field = source;
    }
    CHECK(sol_mir_materialization_validate_concrete(&materialization, NULL));
    sol_mir_materialization_free(&materialization);
    sol_mir_plan_free(&plan);
    sol_mir_program_free(&program);
    sol_diagnostics_free(&diagnostics);
    free_compilation(&compilation);
}

static void test_exclusive_writeback_order(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module writeback\n"
        "function update(first: inout Int64, second: inout Int64) -> Int64 "
        "effects { pure } { first = 3 second = 4 return first }\n"
        "function root() -> Int64 effects { pure } { "
        "var first = 1 var second = 2 return update(first, second) }\n"));
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    SolMirProgram program;
    SolMirPlan plan;
    SolMirMaterialization materialization;
    sol_mir_program_init(&program);
    sol_mir_plan_init(&plan);
    sol_mir_materialization_init(&materialization);
    SolMirMaterializeBuildOutcome outcome;
    bool built = build_all(&compilation.ir,
        callable(&compilation.ir, "root", SOL_IR_CALLABLE_FUNCTION), NULL, 0,
        &program, &plan, &materialization, NULL, &diagnostics, &outcome);
    if (!built || outcome != SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED) {
        sol_diagnostics_render_human(stderr, &compilation.source, &diagnostics);
    }
    CHECK(built);
    CHECK(outcome == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED);
    bool found = false;
    for (size_t i = 0; i < materialization.block_count; ++i) {
        const SolMirMaterializedTerminator *term
            = &materialization.blocks[i].terminator;
        if (term->kind != SOL_MIR_TERM_INVOKE || term->writebacks.count != 2) continue;
        const SolMirMaterializedWriteback *first
            = &materialization.writebacks[term->writebacks.offset];
        const SolMirMaterializedWriteback *second = first + 1;
        CHECK(!first->receiver && first->formal == 0);
        CHECK(!second->receiver && second->formal == 1);
        CHECK(first->place < materialization.place_count
            && second->place < materialization.place_count);
        CHECK(first->type == materialization.places[first->place].final_type);
        CHECK(second->type == materialization.places[second->place].final_type);
        found = true;
    }
    CHECK(found && sol_mir_materialization_validate(&materialization, NULL));
    bool coordinated_access = false;
    for (size_t image = 0; image < materialization.image_count; ++image) {
        SolMirMaterializedImage *target = &materialization.images[image];
        if (target->parameter_accesses.count != 2
            || materialization.accesses[target->parameter_accesses.offset]
                != SOL_ACCESS_EXCLUSIVE) continue;
        for (size_t b = 0; b < materialization.block_count; ++b) {
            SolMirMaterializedTerminator *term = &materialization.blocks[b].terminator;
            if (term->kind != SOL_MIR_TERM_INVOKE
                || materialization.bindings[term->binding].target_kind
                    != SOL_MIR_MATERIALIZED_TARGET_INSTANCE
                || materialization.bindings[term->binding].instance != image) continue;
            SolAccessMode *signature
                = &materialization.accesses[target->parameter_accesses.offset];
            SolMirMaterializedCallArgument *argument
                = &materialization.call_arguments[term->arguments.offset];
            SolAccessMode saved_signature = *signature, saved_argument = argument->access;
            *signature = SOL_ACCESS_SHARED; argument->access = SOL_ACCESS_SHARED;
            CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
            *signature = saved_signature; argument->access = saved_argument;
            coordinated_access = true; break;
        }
    }
    CHECK(coordinated_access);
    if (found) {
        for (size_t i = 0; i < materialization.block_count; ++i) {
            SolMirMaterializedTerminator *term = &materialization.blocks[i].terminator;
            if (term->kind != SOL_MIR_TERM_INVOKE || term->writebacks.count != 2) continue;
            size_t saved = materialization.writebacks[term->writebacks.offset].formal;
            materialization.writebacks[term->writebacks.offset].formal = 1;
            CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
            materialization.writebacks[term->writebacks.offset].formal = saved;
            SolIrCallKind call_kind = term->call_kind;
            term->call_kind = SOL_IR_CALL_CALLBACK;
            CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
            term->call_kind = call_kind;
            size_t effects = term->effects;
            term->effects = materialization.effect_row_count;
            CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
            term->effects = effects;
            size_t failure_edge = term->failure_edge;
            term->failure_edge = SOL_MIR_MATERIALIZED_NONE;
            CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
            term->failure_edge = failure_edge;
            break;
        }
    }
    sol_mir_materialization_free(&materialization);
    sol_mir_plan_free(&plan);
    sol_mir_program_free(&program);
    sol_diagnostics_free(&diagnostics);
    free_compilation(&compilation);
}

static void test_copy_specialization_normalizes_move(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module copy_specialization\n"
        "function identity<T>(value: T) -> T { return value }\n"
        "function root() -> Text { return identity(\"copy text\") }\n"));
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    SolMirProgram program; SolMirPlan plan; SolMirMaterialization materialization;
    sol_mir_program_init(&program); sol_mir_plan_init(&plan);
    sol_mir_materialization_init(&materialization);
    SolMirMaterializeBuildOutcome outcome;
    CHECK(build_all(&compilation.ir,
        callable(&compilation.ir, "root", SOL_IR_CALLABLE_FUNCTION), NULL, 0,
        &program, &plan, &materialization, NULL, &diagnostics, &outcome));
    CHECK(outcome == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED);
    bool normalized = false;
    for (size_t image = 0; image < materialization.image_count; ++image) {
        SolMirMaterializedImage *item = &materialization.images[image];
        for (size_t i = 0; i < item->instructions.count; ++i) {
            size_t id = item->instructions.offset + i;
            SolMirMaterializedInstruction *instruction
                = &materialization.instructions[id];
            size_t source = id - item->instructions.offset;
            if (instruction->kind != SOL_MIR_INST_LOAD_COPY
                || item->topology.instructions[source].kind != SOL_MIR_INST_LOAD_MOVE
                || !materialization.types[instruction->type].is_copy) continue;
            instruction->kind = SOL_MIR_INST_LOAD_MOVE;
            CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
            CHECK(!sol_mir_materialization_validate(&materialization, NULL));
            instruction->kind = SOL_MIR_INST_LOAD_COPY;
            normalized = true;
        }
    }
    CHECK(normalized);
    CHECK(sol_mir_materialization_validate_concrete(&materialization, NULL));
    CHECK(sol_mir_materialization_validate(&materialization, NULL));
    sol_mir_materialization_free(&materialization); sol_mir_plan_free(&plan);
    sol_mir_program_free(&program); sol_diagnostics_free(&diagnostics);
    free_compilation(&compilation);
}

static void test_partial_path_restore_and_loop_metadata(void) {
    Compilation compilation;
    bool compiled = compile_text(&compilation,
        "module path_restore\n"
        "capability Token {}\n"
        "record Inner { b: capability Token, number: Int64 }\n"
        "record Outer { a: Inner, sibling: Int64 }\n"
        "function root(token: capability Token, flag: Bool) -> Int64 { "
        "var x = Outer { a = Inner { b = token, number = 1 }, sibling = 2 } "
        "let moved = x.a.b let sibling = x.sibling "
        "var count = 0 "
        "while count < sibling decreases { sibling - count } { "
        "count += 1 if flag { continue } else {} } return count }\n");
    if (!compiled) sol_diagnostics_render_human(stderr, &compilation.source,
        &compilation.diagnostics);
    CHECK(compiled);
    CHECK(sol_mir_materialization_test_path_frontier());
    SolDiagnostics diagnostics; sol_diagnostics_init(&diagnostics);
    SolMirProgram program; SolMirPlan plan; SolMirMaterialization materialization;
    sol_mir_program_init(&program); sol_mir_plan_init(&plan);
    sol_mir_materialization_init(&materialization);
    SolMirMaterializeBuildOutcome outcome;
    bool built = build_all(&compilation.ir,
        callable(&compilation.ir, "root", SOL_IR_CALLABLE_FUNCTION), NULL, 0,
        &program, &plan, &materialization, NULL, &diagnostics, &outcome);
    if (!built || outcome != SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED) {
        sol_diagnostics_render_human(stderr, &compilation.source, &diagnostics);
    }
    CHECK(built && outcome == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED);
    CHECK(materialization.loop_count == 1
        && sol_mir_materialization_validate_concrete(&materialization, NULL));
    if (materialization.loop_count != 0) {
        size_t saved = materialization.loops[0].header;
        materialization.loops[0].header = materialization.loops[0].exit;
        CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
        materialization.loops[0].header = saved;
    }
    bool transfer_loop = false;
    for (size_t b = 0; b < materialization.block_count; ++b) {
        SolMirMaterializedTerminator *term = &materialization.blocks[b].terminator;
        if (term->kind != SOL_MIR_TERM_CONTINUE) continue;
        size_t saved = term->loop;
        term->loop = SOL_MIR_MATERIALIZED_NONE;
        CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
        term->loop = saved; transfer_loop = true; break;
    }
    CHECK(transfer_loop);
    bool changed_edge = false;
    for (size_t b = 0; b < materialization.block_count; ++b) {
        SolMirMaterializedTerminator *term = &materialization.blocks[b].terminator;
        if (term->kind != SOL_MIR_TERM_BRANCH) continue;
        size_t saved = term->false_edge;
        term->false_edge = term->true_edge;
        CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
        term->false_edge = saved; changed_edge = true; break;
    }
    CHECK(changed_edge && sol_mir_materialization_validate(&materialization, NULL));
    sol_mir_materialization_free(&materialization); sol_mir_plan_free(&plan);
    sol_mir_program_free(&program); sol_diagnostics_free(&diagnostics);
    free_compilation(&compilation);
}

static void test_refinement_predicate_site_and_temp_suffix(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module refinement_site\n"
        "function positive(value: Int64) -> Bool { return value > 0 }\n"
        "type Positive = refined Int64 where positive(self)\n"
        "function root() -> Positive { return Positive(1) }\n"));
    SolDiagnostics diagnostics; sol_diagnostics_init(&diagnostics);
    SolMirProgram program; SolMirPlan plan; SolMirMaterialization materialization;
    sol_mir_program_init(&program); sol_mir_plan_init(&plan);
    sol_mir_materialization_init(&materialization);
    SolMirMaterializeBuildOutcome outcome;
    CHECK(build_all(&compilation.ir,
        callable(&compilation.ir, "root", SOL_IR_CALLABLE_FUNCTION), NULL, 0,
        &program, &plan, &materialization, NULL, &diagnostics, &outcome));
    CHECK(outcome == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED);
    bool predicate = false, missing_predicate = false, temporary = false;
    for (size_t i = 0; i < materialization.binding_count; ++i) {
        if (materialization.bindings[i].kind != SOL_MIR_PLAN_DEMAND_PREDICATE) continue;
        size_t site = materialization.bindings[i].site;
        CHECK(materialization.semantic_sites[site].source_obligation != SOL_IR_NONE);
        SolMirPlanContextId saved = materialization.semantic_sites[site].context;
        materialization.semantic_sites[site].context = materialization.context_count;
        CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
        materialization.semantic_sites[site].context = saved;
        SolMirPlanContextId saved_binding_context
            = materialization.bindings[i].context;
        materialization.semantic_sites[site].context
            = materialization.context_count;
        materialization.bindings[i].context = materialization.context_count;
        CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
        materialization.semantic_sites[site].context = saved;
        materialization.bindings[i].context = saved_binding_context;
        SolMirMaterializedBindingId saved_binding
            = materialization.semantic_sites[site].binding;
        materialization.semantic_sites[site].binding = materialization.binding_count;
        CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
        materialization.semantic_sites[site].binding = saved_binding;
        size_t saved_block = materialization.semantic_sites[site].block;
        materialization.semantic_sites[site].block = SOL_MIR_MATERIALIZED_NONE;
        CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
        materialization.semantic_sites[site].block = saved_block;
        predicate = true; missing_predicate = true; break;
    }
    for (size_t i = 0; i < materialization.instruction_count; ++i) {
        SolMirMaterializedInstruction *instruction = &materialization.instructions[i];
        if (instruction->kind != SOL_MIR_INST_TEMPORARY_DROP) continue;
        size_t saved = instruction->preserve_depth;
        instruction->preserve_depth = materialization.temporary_count;
        CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
        instruction->preserve_depth = saved; temporary = true; break;
    }
    CHECK(predicate && missing_predicate);
    CHECK(temporary || materialization.temporary_count != 0);
    CHECK(sol_mir_materialization_validate(&materialization, NULL));
    sol_mir_materialization_free(&materialization); sol_mir_plan_free(&plan);
    sol_mir_program_free(&program); sol_diagnostics_free(&diagnostics);
    free_compilation(&compilation);
}

static void test_non_cfg_predicate_function_site(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module predicate_value_site\n"
        "function predicate() -> Bool effects { pure } { return true }\n"
        "function accepts(callback: function() -> Bool effects { pure }) "
        "-> Bool effects { pure } { return true }\n"
        "function root() -> Int64 effects { pure } "
        "requires { accepts(predicate) } { return 1 }\n"));
    SolDiagnostics diagnostics; sol_diagnostics_init(&diagnostics);
    SolMirProgram program; SolMirPlan plan; SolMirMaterialization materialization;
    sol_mir_program_init(&program); sol_mir_plan_init(&plan);
    sol_mir_materialization_init(&materialization);
    SolMirMaterializeBuildOutcome outcome;
    CHECK(build_all(&compilation.ir,
        callable(&compilation.ir, "root", SOL_IR_CALLABLE_FUNCTION), NULL, 0,
        &program, &plan, &materialization, NULL, &diagnostics, &outcome));
    CHECK(outcome == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED);
    bool predicate = false, function = false;
    for (size_t i = 0; i < materialization.semantic_site_count; ++i) {
        SolMirMaterializedSemanticSite *site = &materialization.semantic_sites[i];
        if (site->kind != SOL_MIR_PLAN_DEMAND_PREDICATE
            && site->kind != SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE) continue;
        CHECK(site->producer_kind == SOL_MIR_MATERIALIZED_PRODUCER_PREDICATE
            && site->source_obligation != SOL_IR_NONE
            && site->context < materialization.context_count
            && materialization.contexts[site->context].kind
                == SOL_MIR_PLAN_CONTEXT_CONTRACT);
        predicate |= site->kind == SOL_MIR_PLAN_DEMAND_PREDICATE;
        function |= site->kind == SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE;
        size_t saved = site->block;
        const SolMirMaterializedImage *image = &materialization.images[site->parent];
        site->block = image->blocks.offset
            + ((saved - image->blocks.offset + 1) % image->blocks.count);
        CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
        site->block = saved;
    }
    CHECK(predicate && function);
    CHECK(sol_mir_materialization_validate_concrete(&materialization, NULL));
    sol_mir_materialization_free(&materialization); sol_mir_plan_free(&plan);
    sol_mir_program_free(&program); sol_diagnostics_free(&diagnostics);
    free_compilation(&compilation);
}

static void test_multiple_predicate_dependencies(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module predicate_dependencies\n"
        "function magnitude(value: Int64) -> Int64 effects { pure } "
        "{ return value }\n"
        "function low(value: Int64) -> Bool effects { pure } { return value < 100 }\n"
        "function accepts(callback: function(Int64) -> Bool effects { pure }) "
        "-> Bool effects { pure } { return true }\n"
        "type Small = refined Int64 where magnitude(self) > 0 && low(self)\n"
        "function root(value: Int64) -> Int64 effects { pure } "
        "requires { magnitude(value) > 0 && low(value) && accepts(low) } "
        "{ let small = Small(value) return value }\n"));
    SolDiagnostics diagnostics; sol_diagnostics_init(&diagnostics);
    SolMirProgram program; SolMirPlan plan; SolMirMaterialization materialization;
    sol_mir_program_init(&program); sol_mir_plan_init(&plan);
    sol_mir_materialization_init(&materialization);
    SolMirMaterializeBuildOutcome outcome;
    bool built = build_all(&compilation.ir,
        callable(&compilation.ir, "root", SOL_IR_CALLABLE_FUNCTION), NULL, 0,
        &program, &plan, &materialization, NULL, &diagnostics, &outcome);
    if (!built || outcome != SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED)
        sol_diagnostics_render_human(stderr, &compilation.source, &diagnostics);
    CHECK(built && outcome == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED);
    size_t int_helpers = 0, contract_helpers = 0, refinement_helpers = 0;
    size_t callable_values = 0;
    SolMirMaterializedSemanticSiteId contract_site = SOL_MIR_MATERIALIZED_NONE;
    SolMirMaterializedSemanticSiteId refinement_site = SOL_MIR_MATERIALIZED_NONE;
    for (size_t i = 0; i < materialization.semantic_site_count; ++i) {
        const SolMirMaterializedSemanticSite *site = &materialization.semantic_sites[i];
        if (site->context >= materialization.context_count) continue;
        const SolMirPlanContext *context = &materialization.contexts[site->context];
        if (site->kind == SOL_MIR_PLAN_DEMAND_PREDICATE) {
            const SolMirMaterializedBinding *binding
                = &materialization.bindings[site->binding];
            SolMirMaterializedTypeId result = binding->target_kind
                    == SOL_MIR_MATERIALIZED_TARGET_INSTANCE
                ? materialization.images[binding->instance].result
                : materialization.imports[binding->import].result;
            int_helpers += materialization.types[result].kind == SOL_IR_TYPE_INT64;
            if (context->kind == SOL_MIR_PLAN_CONTEXT_CONTRACT) {
                ++contract_helpers; contract_site = i;
            } else if (context->kind == SOL_MIR_PLAN_CONTEXT_REFINEMENT) {
                ++refinement_helpers; refinement_site = i;
            }
            CHECK(site->producer_kind == SOL_MIR_MATERIALIZED_PRODUCER_PREDICATE);
        } else if (site->kind == SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE) {
            ++callable_values;
        }
    }
    CHECK(int_helpers == 2 && contract_helpers == 3 && refinement_helpers == 2
        && callable_values == 1);
    CHECK(sol_mir_materialization_validate_concrete(&materialization, NULL));
    if (contract_site != SOL_MIR_MATERIALIZED_NONE
        && refinement_site != SOL_MIR_MATERIALIZED_NONE) {
        SolMirMaterializedSemanticSite *left
            = &materialization.semantic_sites[contract_site];
        SolMirMaterializedSemanticSite *right
            = &materialization.semantic_sites[refinement_site];
        SolMirPlanContextId saved = left->context;
        left->context = right->context;
        CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
        left->context = saved;
        SolObligationId obligation = right->source_obligation;
        right->source_obligation = SOL_IR_NONE;
        CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
        right->source_obligation = obligation;
        size_t block = left->block; left->block = right->block;
        CHECK(!sol_mir_materialization_validate_concrete(&materialization, NULL));
        left->block = block;
    }
    CHECK(sol_mir_materialization_validate_concrete(&materialization, NULL));
    SolMirProgram repeat_program; SolMirPlan repeat_plan;
    SolMirMaterialization repeat;
    sol_mir_program_init(&repeat_program); sol_mir_plan_init(&repeat_plan);
    sol_mir_materialization_init(&repeat);
    CHECK(build_all(&compilation.ir,
        callable(&compilation.ir, "root", SOL_IR_CALLABLE_FUNCTION), NULL, 0,
        &repeat_program, &repeat_plan, &repeat, NULL, &diagnostics, &outcome));
    size_t first_length = 0, repeat_length = 0;
    char *first_text = render(&materialization, &first_length);
    char *repeat_text = render(&repeat, &repeat_length);
    CHECK(first_text != NULL && repeat_text != NULL && first_length == repeat_length
        && memcmp(first_text, repeat_text, first_length) == 0);
    free(first_text); free(repeat_text);
    sol_mir_materialization_free(&repeat); sol_mir_plan_free(&repeat_plan);
    sol_mir_program_free(&repeat_program);
    sol_mir_materialization_free(&materialization); sol_mir_plan_free(&plan);
    sol_mir_program_free(&program); sol_diagnostics_free(&diagnostics);
    free_compilation(&compilation);
}

static bool compile_e6(Compilation *compilation, SolPackage *package) {
    memset(compilation, 0, sizeof(*compilation));
    sol_package_init(package);
    sol_diagnostics_init(&compilation->diagnostics);
    sol_hir_module_init(&compilation->hir);
    sol_type_table_init(&compilation->types);
    sol_effect_table_init(&compilation->effects);
    sol_contract_table_init(&compilation->contracts);
    sol_ir_init(&compilation->ir);
    char error[256];
    if (!sol_package_load_directory(package,
            SOL_TEST_SOURCE_DIR "/tests/conformance/e6",
            &compilation->diagnostics, error, sizeof(error))) return false;
    SolHirFileScope *scopes = package->file_count == 0 ? NULL
        : malloc(package->file_count * sizeof(*scopes));
    if (package->file_count != 0 && scopes == NULL) return false;
    for (size_t i = 0; i < package->file_count; ++i) {
        scopes[i] = (SolHirFileScope){package->files[i].module_name,
            package->files[i].import_start, package->files[i].import_count,
            package->files[i].item_start, package->files[i].item_count};
    }
    bool valid = sol_hir_lower_scoped(&package->source, &package->syntax, scopes,
            package->file_count, &compilation->hir, &compilation->diagnostics)
        && sol_type_check(&package->source, &package->syntax, &compilation->hir,
            &compilation->types, &compilation->diagnostics)
        && sol_effect_check(&package->source, &package->syntax, &compilation->hir,
            &compilation->types, &compilation->effects,
            &compilation->diagnostics)
        && sol_contract_lower(&package->source, &package->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->diagnostics)
        && sol_ir_lower_scoped(&package->source, &package->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, package->files, package->file_count,
            &compilation->ir, &compilation->diagnostics);
    free(scopes);
    return valid;
}

static void free_e6(Compilation *compilation, SolPackage *package) {
    sol_ir_free(&compilation->ir);
    sol_contract_table_free(&compilation->contracts);
    sol_effect_table_free(&compilation->effects);
    sol_type_table_free(&compilation->types);
    sol_hir_module_free(&compilation->hir);
    sol_diagnostics_free(&compilation->diagnostics);
    sol_package_free(package);
}

static bool build_roots(const SolIr *ir, const SolMirProgramRoot *roots,
    size_t root_count, const SolIrCallableId *imports, size_t import_count,
    SolMirProgram *program, SolMirPlan *plan,
    SolMirMaterialization *materialization, SolDiagnostics *diagnostics) {
    SolMirProgramBuildRequest program_request
        = {ir, roots, root_count, imports, import_count, NULL};
    SolMirPlanBuildRequest plan_request = {program, NULL};
    SolMirMaterializeBuildRequest materialize_request = {plan, NULL};
    return sol_mir_program_build(&program_request, program, diagnostics)
            == SOL_MIR_PROGRAM_BUILD_SUCCEEDED
        && sol_mir_plan_build(&plan_request, plan, diagnostics)
            == SOL_MIR_PLAN_BUILD_SUCCEEDED
        && sol_mir_materialize_build(&materialize_request, materialization,
            diagnostics) == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED;
}

static size_t concrete_invokes(const SolMirMaterialization *owner) {
    size_t count = 0;
    for (size_t i = 0; i < owner->block_count; ++i) {
        count += owner->blocks[i].terminator.kind == SOL_MIR_TERM_INVOKE;
    }
    return count;
}

static void test_e6_concrete_closure_and_determinism(void) {
    Compilation compilation;
    SolPackage package;
    CHECK(compile_e6(&compilation, &package));
    const SolIr *ir = &compilation.ir;
    SolIrCallableId imports[4];
    const char *names[] = {"write", "get", "count", "read"};
    for (size_t i = 0; i < 4; ++i) {
        imports[i] = callable(ir, names[i], SOL_IR_CALLABLE_CAPABILITY);
        CHECK(imports[i] != SOL_IR_NONE);
    }
    SolMirProgramRoot roots[5];
    roots[0] = (SolMirProgramRoot){callable(ir, "launch",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_ENTRY};
    size_t root_count = 1;
    for (SolIrCallableId id = 0; id < ir->callable_count; ++id) {
        if (ir->callables[id].kind == SOL_IR_CALLABLE_TEST) {
            roots[root_count++] = (SolMirProgramRoot){id,
                SOL_MIR_PROGRAM_ROOT_TEST};
        }
    }
    CHECK(root_count == 5);
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    SolMirProgram entry_program, all_program, repeat_program;
    SolMirPlan entry_plan, all_plan, repeat_plan;
    SolMirMaterialization entry, all, repeat;
    sol_mir_program_init(&entry_program); sol_mir_program_init(&all_program);
    sol_mir_program_init(&repeat_program); sol_mir_plan_init(&entry_plan);
    sol_mir_plan_init(&all_plan); sol_mir_plan_init(&repeat_plan);
    sol_mir_materialization_init(&entry); sol_mir_materialization_init(&all);
    sol_mir_materialization_init(&repeat);
    CHECK(build_roots(ir, roots, 1, imports, 4, &entry_program, &entry_plan,
        &entry, &diagnostics));
    CHECK(entry.image_count == 8 && entry.import_count == 4
        && concrete_invokes(&entry) == 12);
    CHECK(entry.binding_count == entry_plan.demand_count
        && sol_mir_materialization_validate(&entry, NULL));
    CHECK(build_roots(ir, roots, root_count, imports, 4, &all_program, &all_plan,
        &all, &diagnostics));
    CHECK(all.image_count == 14 && all.import_count == 4
        && concrete_invokes(&all) == 18);
    CHECK(all.binding_count == all_plan.demand_count
        && sol_mir_materialization_validate(&all, NULL));
    size_t storage_cardinality = 0;
    for (size_t i = 0; i < all.image_count; ++i) {
        const SolMirMaterializedImage *image = &all.images[i];
        size_t projections = 0;
        for (size_t p = 0; p < image->places.count; ++p)
            projections += all.places[image->places.offset + p].projections.count;
        size_t facts = image->locals.count + image->places.count + 1;
        size_t path = image->places.count * (projections + 1) + 1;
        size_t scan = image->blocks.count
            + image->instructions.count * path
            + image->call_arguments.count * path
            + 3 * image->blocks.count * facts;
        storage_cardinality += (image->blocks.count * facts + 1) * scan;
    }
    size_t recomputed_work = 0;
    CHECK(sol_mir_materialization_validation_work(&all, &recomputed_work)
        && recomputed_work == all.usage.validation_work
        && recomputed_work >= storage_cardinality);
    size_t instruction_kinds[SOL_MIR_INST_CAPTURE_SNAPSHOT + 1] = {0};
    size_t terminator_kinds[SOL_MIR_TERM_CONTRACT_VIOLATION + 1] = {0};
    size_t demand_kinds[SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE + 1] = {0};
    for (size_t i = 0; i < all.instruction_count; ++i)
        ++instruction_kinds[all.instructions[i].kind];
    for (size_t i = 0; i < all.block_count; ++i)
        ++terminator_kinds[all.blocks[i].terminator.kind];
    for (size_t i = 0; i < all.binding_count; ++i)
        ++demand_kinds[all.bindings[i].kind];
    const size_t expected_instructions[] = {22, 5, 7, 2, 12, 15, 111, 111,
        26, 0, 1, 16, 0, 16, 1, 1, 1, 50, 5, 4, 3, 3, 3, 1, 0, 0, 28, 1};
    const size_t expected_terminators[] = {0, 11, 12, 17, 1, 18, 22, 0, 0,
        0, 1, 1, 2, 3, 3};
    const size_t expected_demands[] = {5, 18, 0, 0, 0, 0, 0, 0, 0};
    CHECK(all.instruction_count == 445 && all.block_count == 91
        && all.binding_count == 23);
    CHECK(memcmp(instruction_kinds, expected_instructions,
        sizeof(expected_instructions)) == 0);
    CHECK(memcmp(terminator_kinds, expected_terminators,
        sizeof(expected_terminators)) == 0);
    CHECK(memcmp(demand_kinds, expected_demands, sizeof(expected_demands)) == 0);
    bool corrupted_temp_suffix = false;
    for (size_t i = 0; i < all.instruction_count; ++i) {
        SolMirMaterializedInstruction *instruction = &all.instructions[i];
        if (instruction->kind != SOL_MIR_INST_TEMPORARY_DROP) continue;
        size_t saved = instruction->preserve_depth;
        instruction->preserve_depth = all.temporary_count;
        CHECK(!sol_mir_materialization_validate_concrete(&all, NULL));
        instruction->preserve_depth = saved;
        corrupted_temp_suffix = true; break;
    }
    CHECK(corrupted_temp_suffix);
    bool corrupted_propagate = false, corrupted_panic = false;
    for (size_t image = 0; image < all.image_count; ++image) {
        const SolMirMaterializedImage *item = &all.images[image];
        for (size_t b = 0; b < item->blocks.count; ++b) {
            SolMirMaterializedTerminator *term
                = &all.blocks[item->blocks.offset + b].terminator;
            if (!corrupted_propagate && term->kind == SOL_MIR_TERM_PROPAGATE) {
                SolIrPropagationKind saved = term->propagation_kind;
                term->propagation_kind = (SolIrPropagationKind)99;
                CHECK(!sol_mir_materialization_validate_concrete(&all, NULL));
                term->propagation_kind = saved; corrupted_propagate = true;
            }
            if (!corrupted_panic && term->kind == SOL_MIR_TERM_PANIC) {
                size_t replacement = SOL_MIR_MATERIALIZED_NONE;
                for (size_t v = 0; v < item->values.count; ++v) {
                    size_t id = item->values.offset + v;
                    if (all.types[all.values[id].type].kind != SOL_IR_TYPE_TEXT) {
                        replacement = id; break;
                    }
                }
                if (replacement != SOL_MIR_MATERIALIZED_NONE) {
                    size_t saved = term->value;
                    term->value = replacement;
                    CHECK(!sol_mir_materialization_validate_concrete(&all, NULL));
                    term->value = saved; corrupted_panic = true;
                }
            }
        }
    }
    CHECK(corrupted_propagate && corrupted_panic);
    for (SolIrCallableId id = 0; id < ir->callable_count; ++id) {
        if (ir->callables[id].body == SOL_IR_NONE) continue;
        bool represented = false;
        for (size_t image = 0; image < all.image_count; ++image) {
            represented |= all.images[image].source_callable == id;
        }
        CHECK(represented);
    }
    CHECK(build_roots(ir, roots, root_count, imports, 4, &repeat_program,
        &repeat_plan, &repeat, &diagnostics));
    size_t all_length = 0, repeat_length = 0;
    char *all_text = render(&all, &all_length);
    char *repeat_text = render(&repeat, &repeat_length);
    CHECK(all_text != NULL && repeat_text != NULL && all_length == repeat_length
        && memcmp(all_text, repeat_text, all_length) == 0);
    free(all_text); free(repeat_text);

    bool corrupted = false;
    for (size_t i = 0; i < all.block_count && !corrupted; ++i) {
        SolMirMaterializedBlock *block = &all.blocks[i];
        if (block->instructions.count < 2) continue;
        SolMirMaterializedInstruction *second
            = &all.instructions[block->instructions.offset + 1];
        if (second->kind != SOL_MIR_INST_BINARY) continue;
        size_t saved = second->left;
        second->left = second->result;
        CHECK(!sol_mir_materialization_validate(&all, NULL));
        second->left = saved;
        corrupted = true;
    }
    CHECK(corrupted && sol_mir_materialization_validate(&all, NULL));
    if (all.binding_count != 0) {
        size_t saved = all.bindings[0].source_demand;
        all.bindings[0].source_demand = all.binding_count;
        CHECK(!sol_mir_materialization_validate(&all, NULL));
        all.bindings[0].source_demand = saved;
    }
    if (all.semantic_site_count != 0) {
        size_t saved = all.semantic_sites[0].binding;
        all.semantic_sites[0].binding = all.binding_count;
        CHECK(!sol_mir_materialization_validate_concrete(&all, NULL));
        all.semantic_sites[0].binding = saved;
    }
    size_t saved_validation_work = all.usage.validation_work;
    ++all.usage.validation_work;
    CHECK(!sol_mir_materialization_validate_concrete(&all, NULL));
    all.usage.validation_work = saved_validation_work;
    if (all.image_count > 1 && all.images[1].blocks.count != 0) {
        size_t saved = all.images[1].entry;
        all.images[1].entry = all.images[0].entry;
        CHECK(!sol_mir_materialization_validate(&all, NULL));
        all.images[1].entry = saved;
    }
    CHECK(sol_mir_materialization_validate(&all, NULL));
    sol_mir_materialization_free(&repeat); sol_mir_materialization_free(&all);
    sol_mir_materialization_free(&entry); sol_mir_plan_free(&repeat_plan);
    sol_mir_plan_free(&all_plan); sol_mir_plan_free(&entry_plan);
    sol_mir_program_free(&repeat_program); sol_mir_program_free(&all_program);
    sol_mir_program_free(&entry_program); sol_diagnostics_free(&diagnostics);
    free_e6(&compilation, &package);
}

int main(void) {
    CHECK(sol_mir_materialization_test_callable_site_target_equality());
    test_generic_recursion_lifecycle_limits_and_render();
    test_evidence_and_import_bindings();
    test_handlers_are_materialized();
    test_refinement_contexts_copy_and_concrete_records();
    test_recursive_nominal_planning_and_copy_cycle();
    test_exclusive_writeback_order();
    test_copy_specialization_normalizes_move();
    test_partial_path_restore_and_loop_metadata();
    test_refinement_predicate_site_and_temp_suffix();
    test_non_cfg_predicate_function_site();
    test_multiple_predicate_dependencies();
    test_e6_concrete_closure_and_determinism();
    if (failures != 0) {
        fprintf(stderr, "%d MIR materialization test(s) failed\n", failures);
        return 1;
    }
    printf("MIR materialization tests passed\n");
    return 0;
}
