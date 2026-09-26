#include "sol/mir_runtime_conventions.h"

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

void sol_mir_runtime_conventions_test_force_host_collision(bool force);
void sol_mir_runtime_conventions_test_force_recipe_collision(bool force);
void sol_mir_runtime_conventions_test_force_host_symbol_collision(bool force);
void sol_mir_runtime_conventions_test_force_recipe_symbol_collision(bool force);
void sol_mir_runtime_conventions_test_force_allocation_failure(bool force);
size_t sol_mir_runtime_conventions_test_observed_validation_work(void);
typedef enum {
    SOL_MIR_RUNTIME_BUILD_TEST_HASH,
    SOL_MIR_RUNTIME_BUILD_TEST_TARGET,
    SOL_MIR_RUNTIME_BUILD_TEST_DIGEST,
    SOL_MIR_RUNTIME_BUILD_TEST_SORT,
    SOL_MIR_RUNTIME_BUILD_TEST_CATEGORY_COUNT,
} SolMirRuntimeBuildTestCategory;
void sol_mir_runtime_conventions_test_force_build_category(
    SolMirRuntimeBuildTestCategory category);
size_t sol_mir_runtime_conventions_test_build_category_attempts(
    SolMirRuntimeBuildTestCategory category);
size_t sol_mir_runtime_conventions_test_build_events_after_exhaustion(void);
bool sol_mir_runtime_conventions_test_reconstruct_usage(
    const SolMirConcreteProgram *concrete,
    const SolMirRuntimeConventionsLimits *limits,
    SolMirRuntimeConventionsUsage *usage);
void sol_mir_concrete_test_force_validation_allocation_failure(bool force);
size_t sol_mir_concrete_test_validation_allocation_attempts(void);
SolMirRuntimeConventionsBuildOutcome
sol_mir_runtime_conventions_test_classify_call_kind(SolIrCallKind kind);
SolMirRuntimeResultClass
sol_mir_runtime_conventions_test_classify_result_kind(SolMirRecipeKind kind);
bool sol_mir_runtime_conventions_test_indirect_target_valid(
    const SolMirRuntimeCall *call, size_t table, size_t table_count);
bool sol_mir_runtime_conventions_test_build_call_failure_mask(
    const SolMirRuntimeCall *call, const SolMirLinkage *linkage, uint32_t *mask);
bool sol_mir_runtime_conventions_test_validate_call_failure_mask(
    const SolMirRuntimeCall *call, const SolMirLinkage *linkage, uint32_t *mask);

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

static bool compile_text_at(TextCompilation *c, const char *path,
    const char *text) {
    memset(c, 0, sizeof(*c)); sol_tokens_init(&c->tokens);
    sol_diagnostics_init(&c->diagnostics); sol_syntax_tree_init(&c->syntax);
    sol_hir_module_init(&c->hir); sol_type_table_init(&c->types);
    sol_effect_table_init(&c->effects); sol_contract_table_init(&c->contracts);
    sol_ir_init(&c->ir);
    return sol_source_from_text(&c->source, path, text)
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

static bool compile_text(TextCompilation *c, const char *text) {
    return compile_text_at(c, "runtime_conventions.sol", text);
}

static void text_compilation_free(TextCompilation *c) {
    sol_ir_free(&c->ir); sol_contract_table_free(&c->contracts);
    sol_effect_table_free(&c->effects); sol_type_table_free(&c->types);
    sol_hir_module_free(&c->hir); sol_syntax_tree_free(&c->syntax);
    sol_tokens_free(&c->tokens); sol_source_free(&c->source);
    sol_diagnostics_free(&c->diagnostics);
}

static SolMirConcreteBuildOutcome build_text_concrete_root_outcome(
    TextCompilation *c, const char *root_name,
    SolMirProgramRootKind root_kind,
    const SolIrCallableId *imports, size_t import_count,
    SolMirConcreteProgram *program) {
    SolMirProgramRoot root = {callable(&c->ir, root_name,
        SOL_IR_CALLABLE_FUNCTION), root_kind};
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    SolMirConcreteBuildRequest request = {&c->ir, &root, 1, imports,
        import_count, &target, NULL};
    return sol_mir_concrete_program_build(&request, program, &c->diagnostics);
}

static bool build_text_concrete_root(TextCompilation *c, const char *root_name,
    SolMirProgramRootKind root_kind,
    const SolIrCallableId *imports, size_t import_count,
    SolMirConcreteProgram *program) {
    return build_text_concrete_root_outcome(c, root_name, root_kind, imports,
        import_count, program) == SOL_MIR_CONCRETE_BUILD_SUCCEEDED;
}

static bool build_text_concrete(TextCompilation *c, const char *root_name,
    const SolIrCallableId *imports, size_t import_count,
    SolMirConcreteProgram *program) {
    return build_text_concrete_root(c, root_name,
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE, imports, import_count, program);
}

static bool build_text_concrete_roots(TextCompilation *c,
    const char *const *root_names, size_t root_count,
    const SolIrCallableId *imports, size_t import_count,
    SolMirConcreteProgram *program) {
    if (root_count > 4) return false;
    SolMirProgramRoot roots[4];
    for (size_t i = 0; i < root_count; ++i)
        roots[i] = (SolMirProgramRoot){callable(&c->ir, root_names[i],
            SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    SolMirConcreteBuildRequest request = {&c->ir, roots, root_count, imports,
        import_count, &target, NULL};
    return sol_mir_concrete_program_build(&request, program, &c->diagnostics)
        == SOL_MIR_CONCRETE_BUILD_SUCCEEDED;
}

static bool build_concrete(Compilation *c, SolMirConcreteProgram *program) {
    SolMirProgramRoot roots[5]; size_t root_count = 1;
    roots[0] = (SolMirProgramRoot){callable(&c->ir, "launch",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_ENTRY};
    for (size_t i = 0; i < c->ir.callable_count; ++i)
        if (c->ir.callables[i].kind == SOL_IR_CALLABLE_TEST)
            roots[root_count++] = (SolMirProgramRoot){i,
                SOL_MIR_PROGRAM_ROOT_TEST};
    static const char *const names[] = {"write", "get", "count", "read"};
    SolIrCallableId imports[4];
    for (size_t i = 0; i < 4; ++i)
        imports[i] = callable(&c->ir, names[i], SOL_IR_CALLABLE_CAPABILITY);
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    SolMirConcreteBuildRequest request = {&c->ir, roots, root_count, imports,
        4, &target, NULL};
    return root_count == 5 && sol_mir_concrete_program_build(&request, program,
        &c->diagnostics) == SOL_MIR_CONCRETE_BUILD_SUCCEEDED;
}

static char *render(const SolMirRuntimeConventions *owner) {
    FILE *stream = tmpfile();
    if (stream == NULL || !sol_mir_runtime_conventions_render(stream, owner)
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

static bool expression_source(const SolIr *ir, SolIrCallableId callable_id,
    SolIrExpressionId expression_id, SolMirRuntimeSource *source) {
    if (callable_id >= ir->callable_count || expression_id >= ir->expression_count)
        return false;
    SolSpan span = ir->expressions[expression_id].span;
    for (size_t i = 0; i < ir->file_count; ++i)
        if (span.start >= ir->files[i].aggregate_start
            && span.end <= ir->files[i].aggregate_end) {
            *source = (SolMirRuntimeSource){i,
                span.start - ir->files[i].aggregate_start,
                span.end - ir->files[i].aggregate_start};
            return true;
        }
    return false;
}

static SolMirRuntimeConventionsLimits exact_limits(
    const SolMirRuntimeConventions *owner) {
#define NONZERO(value) ((value) == 0 ? 1 : (value))
    return (SolMirRuntimeConventionsLimits){
        .max_signatures = NONZERO(owner->usage.signatures),
        .max_signature_slots = NONZERO(owner->usage.signature_slots),
        .max_calls = NONZERO(owner->usage.calls),
        .max_operands = NONZERO(owner->usage.operands),
        .max_writebacks = NONZERO(owner->usage.writebacks),
        .max_entries = NONZERO(owner->usage.entries),
        .max_imports = NONZERO(owner->usage.imports),
        .max_failure_sites = NONZERO(owner->usage.failure_sites),
        .max_owned_bytes = NONZERO(owner->usage.owned_bytes),
        .max_build_scratch_bytes = NONZERO(owner->usage.build_scratch_bytes),
        .max_build_work = NONZERO(owner->usage.build_work),
        .max_validation_scratch_bytes
            = NONZERO(owner->usage.validation_scratch_bytes),
        .max_validation_work = NONZERO(owner->usage.validation_work),
    };
#undef NONZERO
}

static void check_all_call_sources(const SolMirRuntimeConventions *owner) {
    for (size_t i = 0; i < owner->call_count; ++i) {
        const SolMirRuntimeCall *call = &owner->calls[i];
        CHECK(call->failure_site < owner->failure_site_count);
        if (call->failure_site >= owner->failure_site_count) continue;
        SolMirRuntimeFailureRecord failure = {
            .code = SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT,
            .source = owner->failure_sites[call->failure_site].source,
            .detail_kind = SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE,
        };
        CHECK(sol_mir_runtime_failure_record_validate(owner, &failure));
    }
}

static bool failure_for_code(const SolMirRuntimeConventions *owner,
    SolMirRuntimeFailureCode code, SolMirRuntimeFailureRecord *failure) {
    uint32_t bit = UINT32_C(1) << ((unsigned)code - 1);
    for (size_t i = 0; i < owner->failure_site_count; ++i)
        if ((owner->failure_sites[i].allowed_codes & bit) != 0) {
            *failure = (SolMirRuntimeFailureRecord){
                .code = code, .source = owner->failure_sites[i].source,
                .detail_kind = code == SOL_MIR_RUNTIME_FAILURE_PANIC
                    ? SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT
                    : code == SOL_MIR_RUNTIME_FAILURE_HOST_ERROR
                        ? SOL_MIR_RUNTIME_FAILURE_DETAIL_HOST_BYTES
                        : SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE,
            };
            return true;
        }
    return false;
}

static bool expression_failure_source_valid(const SolMirConcreteProgram *program,
    const SolMirRuntimeConventions *owner,
    SolIrCallableId callable_id, SolIrExpressionId expression_id,
    SolMirRuntimeFailureCode code) {
    SolMirRuntimeFailureRecord failure = {.code = code,
        .detail_kind = code == SOL_MIR_RUNTIME_FAILURE_PANIC
            ? SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT
            : SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE};
    return expression_source(program->program.ir, callable_id, expression_id,
            &failure.source)
        && sol_mir_runtime_failure_record_validate(owner, &failure);
}

static void test_validation_work_deltas(SolMirConcreteProgram *program,
    const SolMirRuntimeConventions *owner) {
    SolMirRuntimeConventionsUsage baseline;
    CHECK(sol_mir_runtime_conventions_test_reconstruct_usage(program,
        &owner->limits, &baseline));
    CHECK(memcmp(&baseline, &owner->usage, sizeof(baseline)) == 0);
}

static bool build_text_runtime(TextCompilation *compilation, const char *source,
    const char *const *import_names, size_t import_count,
    SolMirConcreteProgram *program, SolMirRuntimeConventions *owner) {
    sol_mir_concrete_program_init(program);
    sol_mir_runtime_conventions_init(owner);
    if (!compile_text(compilation, source)) return false;
    SolIrCallableId imports[4];
    if (import_count > sizeof(imports) / sizeof(imports[0])) return false;
    for (size_t i = 0; i < import_count; ++i) {
        imports[i] = callable(&compilation->ir, import_names[i],
            SOL_IR_CALLABLE_CAPABILITY);
        if (imports[i] == SOL_IR_NONE) return false;
    }
    if (!build_text_concrete(compilation, "root", imports, import_count, program))
        return false;
    SolMirRuntimeConventionsBuildRequest request = {program, NULL};
    return sol_mir_runtime_conventions_build(&request, owner,
        &compilation->diagnostics) == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED;
}

static bool runtime_usage_observed(const SolMirConcreteProgram *program,
    const SolMirRuntimeConventions *owner) {
    SolMirRuntimeConventionsUsage reconstructed;
    return sol_mir_runtime_conventions_test_reconstruct_usage(program,
            &owner->limits, &reconstructed)
        && memcmp(&reconstructed, &owner->usage, sizeof(reconstructed)) == 0
        && sol_mir_runtime_conventions_validate(owner, NULL)
        && sol_mir_runtime_conventions_test_observed_validation_work()
            == owner->usage.validation_work;
}

static size_t operation_count(const SolMirConcreteProgram *program) {
    size_t result = 0;
    for (size_t i = 0; i < program->linkage.runtime_requirement_count; ++i) {
        uint32_t operations = program->linkage.runtime_requirements[i].operations;
        for (; operations != 0; operations &= operations - 1) ++result;
    }
    return result;
}

static size_t image_parameter_count(const SolMirConcreteProgram *program) {
    size_t result = 0;
    for (size_t i = 0; i < program->materialization.image_count; ++i)
        result += program->materialization.images[i].parameter_types.count;
    return result;
}

static void test_valid_owner_work_deltas(void) {
    static const char *const sources[][2] = {
        {
            "module runtime_operation_delta\n"
            "record Box { value: Int64 }\n"
            "function root(first: borrow Box, second: borrow Box) -> Bool "
            "effects { pure } { return true }\n",
            "module runtime_operation_delta\n"
            "record Box { value: Int64 }\n"
            "function root(first: borrow Box, second: borrow Box) -> Bool "
            "effects { pure } { return first == second }\n",
        },
        {
            "module runtime_image_delta\n"
            "function root() -> Int64 effects { pure } { return 1 }\n",
            "module runtime_image_delta\n"
            "function root(value: borrow Int64) -> Int64 effects { pure } "
            "{ return 1 }\n",
        },
        {
            "module runtime_candidate_delta\n"
            "capability First { function first() -> Int64 effects { pure } }\n"
            "function root(value: capability First) -> Int64 effects { pure } "
            "{ return value.first() }\n",
            "module runtime_candidate_delta\n"
            "capability First { function first() -> Int64 effects { pure } "
            "function second() -> Int64 effects { pure } }\n"
            "function root(value: capability First) -> Int64 effects { pure } "
            "{ return value.first() + value.second() }\n",
        },
    };
    static const char *const first_imports[] = {"first"};
    static const char *const second_imports[] = {"first", "second"};
    for (size_t pair = 0; pair < 3; ++pair) {
        TextCompilation compilations[2];
        SolMirConcreteProgram programs[2];
        SolMirRuntimeConventions owners[2];
        bool built[2] = {false, false};
        for (size_t side = 0; side < 2; ++side) {
            const char *const *imports = pair == 2
                ? (side == 0 ? first_imports : second_imports) : NULL;
            size_t import_count = pair == 2 ? side + 1 : 0;
            built[side] = build_text_runtime(&compilations[side],
                sources[pair][side], imports, import_count,
                &programs[side], &owners[side]);
            if (!built[side]) sol_diagnostics_render_human(stderr,
                &compilations[side].source, &compilations[side].diagnostics);
            CHECK(built[side]);
            if (built[side]) CHECK(runtime_usage_observed(&programs[side],
                &owners[side]));
        }
        if (built[0] && built[1]) {
            static const size_t expected_deltas[] = {3651, 663, 16428};
            CHECK(owners[1].usage.validation_work
                    - owners[0].usage.validation_work == expected_deltas[pair]);
            if (pair == 0) {
                CHECK(operation_count(&programs[1])
                        == operation_count(&programs[0]) + 1
                    && owners[1].import_count == owners[0].import_count + 1);
            } else if (pair == 1) {
                CHECK(image_parameter_count(&programs[1])
                    == image_parameter_count(&programs[0]) + 1);
            } else {
                size_t first_pairs = owners[0].import_count
                    * (owners[0].import_count - 1) / 2;
                size_t second_pairs = owners[1].import_count
                    * (owners[1].import_count - 1) / 2;
                CHECK(owners[1].import_count == owners[0].import_count + 1
                    && second_pairs == first_pairs + owners[0].import_count);
            }
        }
        for (size_t side = 0; side < 2; ++side) {
            if (built[side]) sol_mir_runtime_conventions_free(&owners[side]);
            sol_mir_concrete_program_free(&programs[side]);
            text_compilation_free(&compilations[side]);
        }
    }
}

static void test_taxonomy_and_exit(const SolMirRuntimeConventions *owner) {
    static const char *const names[] = {
        NULL, "SOL-RUNTIME-PANIC", "SOL-RUNTIME-INTEGER-OVERFLOW",
        "SOL-RUNTIME-DIVISION-BY-ZERO", "SOL-RUNTIME-ALLOCATION-FAILED",
        "SOL-RUNTIME-ALLOCATION-LIMIT", "SOL-RUNTIME-STEP-LIMIT",
        "SOL-RUNTIME-CALL-DEPTH-LIMIT", "SOL-RUNTIME-VALUE-LIMIT",
        "SOL-RUNTIME-TEXT-LIMIT", "SOL-RUNTIME-HOST-CALL-LIMIT",
        "SOL-RUNTIME-NO-MATCH", "SOL-RUNTIME-REACHED-UNREACHABLE",
        "SOL-RUNTIME-REQUIRE-VIOLATION", "SOL-RUNTIME-ENSURE-VIOLATION",
        "SOL-RUNTIME-REFINEMENT-VIOLATION", "SOL-RUNTIME-HOST-ERROR",
    };
    CHECK(sol_mir_runtime_failure_name(SOL_MIR_RUNTIME_FAILURE_NONE) == NULL);
    for (size_t i = 1; i < sizeof(names) / sizeof(names[0]); ++i)
        CHECK(strcmp(sol_mir_runtime_failure_name((SolMirRuntimeFailureCode)i),
            names[i]) == 0);
    CHECK(sol_mir_runtime_failure_name((SolMirRuntimeFailureCode)17) == NULL);
    CHECK(sol_mir_runtime_conventions_test_classify_call_kind(
            SOL_IR_CALL_CALLBACK)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_conventions_test_classify_call_kind(
            (SolIrCallKind)99)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_UNSUPPORTED_CALLABLE);
    CHECK(sol_mir_runtime_conventions_test_classify_result_kind(
            SOL_MIR_RECIPE_NEVER) == SOL_MIR_RUNTIME_RESULT_NEVER);
    SolMirRuntimeCall indirect_call = {
        .target_kind = SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE,
        .internal = SOL_MIR_LINKAGE_NONE, .host = SOL_MIR_LINKAGE_NONE,
        .table = 0,
    };
    CHECK(sol_mir_runtime_conventions_test_indirect_target_valid(
        &indirect_call, 0, 1));
    indirect_call.internal = 0;
    CHECK(!sol_mir_runtime_conventions_test_indirect_target_valid(
        &indirect_call, 0, 1));
    SolMirRuntimeExit exit;
    const int64_t values[] = {-1, 0, 1, 2, 255, 256};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        CHECK(sol_mir_runtime_exit_map(owner, 0, values[i], NULL, &exit));
        if (values[i] >= 0 && values[i] <= 255) {
            CHECK(exit.kind == SOL_MIR_RUNTIME_EXIT_APPLICATION
                && exit.driver_status == values[i]
                && exit.application_status == values[i]);
        } else CHECK(exit.kind == SOL_MIR_RUNTIME_EXIT_BOUNDARY_ERROR
            && exit.driver_status == 1
            && strcmp(exit.boundary_code, "SOL-RUN-002") == 0);
    }
    CHECK(owner->call_count != 0);
    if (owner->call_count != 0) {
        SolMirRuntimeFailureRecord failure = {0};
        CHECK(failure_for_code(owner, SOL_MIR_RUNTIME_FAILURE_PANIC, &failure));
        CHECK(sol_mir_runtime_failure_record_validate(owner, &failure));
        CHECK(sol_mir_runtime_exit_map(owner, 0, 0, &failure, &exit)
            && exit.kind == SOL_MIR_RUNTIME_EXIT_RUNTIME_FAILURE
            && exit.driver_status == 1
            && exit.failure_code == SOL_MIR_RUNTIME_FAILURE_PANIC
            && exit.boundary_code == NULL);
        uint8_t detail[SOL_MIR_RUNTIME_HOST_DETAIL_MAX];
        memset(detail, 'x', sizeof(detail));
        failure.bytes = detail; failure.length = 1;
        CHECK(sol_mir_runtime_failure_record_validate(owner, &failure));
        failure.detail_kind = SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE;
        CHECK(!sol_mir_runtime_failure_record_validate(owner, &failure));
        failure.detail_kind = SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT;
        failure.bytes = NULL;
        CHECK(!sol_mir_runtime_failure_record_validate(owner, &failure));
        failure.bytes = detail; failure.length = 0;
        CHECK(!sol_mir_runtime_failure_record_validate(owner, &failure));
        detail[0] = '\0'; failure.length = 1;
        CHECK(!sol_mir_runtime_failure_record_validate(owner, &failure));
        detail[0] = 'x';
        CHECK(failure_for_code(owner, SOL_MIR_RUNTIME_FAILURE_HOST_ERROR,
            &failure));
        failure.bytes = detail;
        failure.length = sizeof(detail);
        CHECK(sol_mir_runtime_failure_record_validate(owner, &failure));
        detail[1] = '\0';
        CHECK(!sol_mir_runtime_failure_record_validate(owner, &failure));
        detail[1] = 'x';
        failure.length = SOL_MIR_RUNTIME_HOST_DETAIL_MAX + 1;
        CHECK(!sol_mir_runtime_failure_record_validate(owner, &failure));
        failure.length = 1; ++failure.source.end;
        CHECK(!sol_mir_runtime_failure_record_validate(owner, &failure));
    }
}

static void test_e6(const SolMirConcreteProgram *program,
    SolDiagnostics *diagnostics) {
    SolMirRuntimeConventions owner;
    sol_mir_runtime_conventions_init(&owner);
    SolMirRuntimeConventionsBuildRequest request = {program, NULL};
    SolMirRuntimeConventionsBuildOutcome build_outcome
        = sol_mir_runtime_conventions_build(&request, &owner, diagnostics);
    CHECK(build_outcome == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
    if (owner.concrete == NULL) return;
    CHECK(sol_mir_runtime_conventions_validate(&owner, NULL));
    CHECK(owner.signature_count == 18 && owner.signature_slot_count == 19);
    CHECK(owner.call_count == 18 && owner.operand_count == 24
        && owner.writeback_count == 1 && owner.entry_count == 1);
    CHECK(owner.import_count == 52);
    size_t origins[3] = {0}, targets[3] = {0}, call_kinds[4] = {0};
    size_t result_classes[3] = {0}, accesses[3] = {0}, import_kinds[6] = {0};
    for (size_t i = 0; i < owner.signature_count; ++i) {
        ++origins[owner.signatures[i].origin];
        ++result_classes[owner.signatures[i].result_class];
    }
    for (size_t i = 0; i < owner.signature_slot_count; ++i)
        ++accesses[owner.signature_slots[i].access];
    bool method_receiver = false;
    bool direct_callee_rejected = false;
    bool method_callee_rejected = false;
    bool call_site_accepted = false;
    bool method_receiver_rejected = false;
    for (size_t i = 0; i < owner.call_count; ++i) {
        ++targets[owner.calls[i].target_kind];
        ++call_kinds[owner.calls[i].call_kind];
        CHECK(owner.calls[i].failure_edge != SOL_MIR_RUNTIME_NONE);
        method_receiver |= owner.calls[i].call_kind == SOL_IR_CALL_METHOD
            && owner.calls[i].operands.count != 0
            && owner.signature_slots[owner.operands[
                owner.calls[i].operands.offset].signature_slot].role
                == SOL_MIR_RUNTIME_SLOT_RECEIVER;
        const SolMirRuntimeCall *runtime_call = &owner.calls[i];
        SolIrExpressionId source_id;
        SolIrCallableId source_callable;
        if (runtime_call->owner_kind == SOL_MIR_RUNTIME_CALL_OWNER_IMAGE) {
            const SolMirMaterializedTerminator *term
                = &program->materialization.blocks[runtime_call->block].terminator;
            source_id = term->source_expression;
            source_callable = program->materialization.images[
                runtime_call->image].source_callable;
        } else {
            const SolMirPredicateTerminator *term
                = &program->operations.predicate_blocks[
                    runtime_call->block].terminator;
            source_id = program->materialization.bindings[
                term->binding].source.expression;
            source_callable = program->materialization.bindings[
                term->binding].source.callable;
        }
        if (source_id >= program->program.ir->expression_count) continue;
        const SolIrExpression *source_expression =
            &program->program.ir->expressions[source_id];
        if (source_expression->kind != SOL_IR_EXPR_CALL) continue;
        call_site_accepted |= expression_failure_source_valid(program, &owner,
            source_callable, source_id,
            SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT);
        if (source_expression->as.call.kind == SOL_IR_CALL_FUNCTION) {
            direct_callee_rejected
                |= !expression_failure_source_valid(program, &owner,
                    source_callable,
                    source_expression->as.call.callee,
                    SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT);
        } else if (source_expression->as.call.kind == SOL_IR_CALL_METHOD) {
            method_callee_rejected
                |= !expression_failure_source_valid(program, &owner,
                    source_callable,
                    source_expression->as.call.callee,
                    SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT);
            method_receiver_rejected
                |= !expression_failure_source_valid(program, &owner,
                    source_callable,
                    source_expression->as.call.receiver,
                    SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT);
        }
    }
    for (size_t i = 0; i < owner.import_count; ++i)
        ++import_kinds[owner.imports[i].kind];
    const size_t expected_origins[] = {14, 4, 0};
    const size_t expected_targets[] = {13, 5, 0};
    const size_t expected_call_kinds[] = {12, 0, 5, 1};
    const size_t expected_result_classes[] = {15, 3, 0};
    const size_t expected_accesses[] = {14, 4, 1};
    const size_t expected_import_kinds[] = {4, 16, 10, 17, 5, 0};
    const SolMirRuntimeConventionsUsage expected_usage = {
        .signatures = 18, .signature_slots = 19, .calls = 18,
        .operands = 24, .writebacks = 1, .entries = 1, .imports = 52,
        .failure_sites = 29, .owned_bytes = 16328,
        .build_scratch_bytes = 21, .build_work = 9427,
        .validation_scratch_bytes = 584692564,
        .validation_work = 73649839,
    };
    CHECK(memcmp(origins, expected_origins, sizeof(origins)) == 0);
    CHECK(memcmp(targets, expected_targets, sizeof(targets)) == 0);
    CHECK(memcmp(call_kinds, expected_call_kinds, sizeof(call_kinds)) == 0);
    CHECK(memcmp(result_classes, expected_result_classes,
        sizeof(result_classes)) == 0);
    CHECK(memcmp(accesses, expected_accesses, sizeof(accesses)) == 0);
    CHECK(memcmp(import_kinds, expected_import_kinds,
        sizeof(import_kinds)) == 0);
    CHECK(method_receiver);
    CHECK(direct_callee_rejected);
    CHECK(method_callee_rejected);
    CHECK(call_site_accepted);
    CHECK(method_receiver_rejected);
    check_all_call_sources(&owner);
    CHECK(memcmp(&owner.usage, &expected_usage, sizeof(expected_usage)) == 0);
    CHECK(sol_mir_runtime_conventions_test_observed_validation_work()
        == owner.usage.validation_work);
    test_validation_work_deltas((SolMirConcreteProgram *)(void *)program,
        &owner);
    CHECK(strcmp(owner.imports[0].symbol.bytes,
        "sol.h1.423d5b4db606d2261bc308ccc0768ca3.05dec151972c668fc84d4ca9369d22317bb58c7a75ac50fad1809d4d51f9775b") == 0);
    CHECK(strcmp(owner.imports[4].symbol.bytes,
        "sol.r1.copy.0050a06f5367da2ca3fecae829980836edff0726ce5269184a273720c0825f24") == 0);
    bool per_operation_distinct = false;
    for (size_t i = 0; i < owner.import_count; ++i)
        for (size_t q = i + 1; q < owner.import_count; ++q)
            if (owner.imports[i].recipe != SOL_MIR_RECIPE_NONE
                && owner.imports[i].recipe == owner.imports[q].recipe
                && owner.imports[i].kind != owner.imports[q].kind) {
                CHECK(memcmp(&owner.imports[i].identity,
                    &owner.imports[q].identity,
                    sizeof(owner.imports[i].identity)) != 0);
                per_operation_distinct = true;
            }
    CHECK(per_operation_distinct);
    CHECK(owner.entries[0].result_class == SOL_MIR_RUNTIME_RESULT_VALUE
        && program->representation.recipes[
            owner.signatures[owner.entries[0].signature].result].kind
                == SOL_MIR_RECIPE_INT64);
    char *first = render(&owner); CHECK(first != NULL);
    if (first != NULL) {
        CHECK(strstr(first, "SOL-RUNTIME-HOST-ERROR") != NULL);
        CHECK(strstr(first, "SOL-RUN-002") != NULL);
        static const char *const unstable_labels[] = {" image=", " predicate=",
            " block=", " recipe=", " callable=",
            " expression=", " file=", " binding=", " host=", " table="};
        for (size_t i = 0; i < sizeof(unstable_labels)
                / sizeof(unstable_labels[0]); ++i)
            CHECK(strstr(first, unstable_labels[i]) == NULL);
        CHECK(strstr(first, program->program.ir->source_path) == NULL);
    }
    test_taxonomy_and_exit(&owner);

    SolMirRuntimeConventionsLimits exact = exact_limits(&owner);
    SolMirRuntimeConventions limited; sol_mir_runtime_conventions_init(&limited);
    SolMirRuntimeConventionsLimits between = exact;
    const size_t former_conservative_validation_work = 159461329;
    CHECK(exact.max_validation_work < former_conservative_validation_work);
    between.max_validation_work += (former_conservative_validation_work
        - between.max_validation_work) / 2;
    request.limits = &between;
    CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
    sol_mir_runtime_conventions_free(&limited);
    request.limits = &exact;
    CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
    char *second = render(&limited);
    CHECK(first != NULL && second != NULL && strcmp(first, second) == 0);
    SolMirRuntimeConventions before = limited;
    CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INVALID_ARGUMENT
        && memcmp(&before, &limited, sizeof(limited)) == 0);
    free(second); sol_mir_runtime_conventions_free(&limited);
    --exact.max_operands;
    CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED
        && limited.concrete == NULL);
    exact = exact_limits(&owner);
    sol_mir_concrete_test_force_validation_allocation_failure(true);
#define ONE_BELOW(member) do { \
    SolMirRuntimeConventionsLimits one_below = exact; \
    --one_below.member; request.limits = &one_below; \
    CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics) \
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED); \
    CHECK(limited.concrete == NULL \
        && sol_mir_concrete_test_validation_allocation_attempts() == 0); \
} while (0)
    ONE_BELOW(max_signatures);
    ONE_BELOW(max_signature_slots);
    ONE_BELOW(max_calls);
    ONE_BELOW(max_operands);
    ONE_BELOW(max_imports);
    ONE_BELOW(max_failure_sites);
    ONE_BELOW(max_owned_bytes);
    ONE_BELOW(max_build_scratch_bytes);
    ONE_BELOW(max_build_work);
    ONE_BELOW(max_validation_scratch_bytes);
    ONE_BELOW(max_validation_work);
#undef ONE_BELOW
    SolMirRuntimeConventionsLimits zero_writebacks = exact;
    zero_writebacks.max_writebacks = 0; request.limits = &zero_writebacks;
    CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INVALID_ARGUMENT
        && sol_mir_concrete_test_validation_allocation_attempts() == 0);
    SolMirRuntimeConventionsLimits zero_entries = exact;
    zero_entries.max_entries = 0; request.limits = &zero_entries;
    CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INVALID_ARGUMENT
        && sol_mir_concrete_test_validation_allocation_attempts() == 0);
    exact = exact_limits(&owner); exact.max_calls = 0; request.limits = &exact;
    CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INVALID_ARGUMENT
        && sol_mir_concrete_test_validation_allocation_attempts() == 0);
    sol_mir_concrete_test_force_validation_allocation_failure(false);

    size_t saved = owner.calls[0].failure_edge;
    owner.calls[0].failure_edge = SOL_MIR_RUNTIME_NONE;
    CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
    FILE *stream = tmpfile(); CHECK(stream != NULL);
    if (stream != NULL) {
        CHECK(!sol_mir_runtime_conventions_render(stream, &owner));
        CHECK(fflush(stream) == 0 && fseek(stream, 0, SEEK_END) == 0
            && ftell(stream) == 0);
        fclose(stream);
    }
    owner.calls[0].failure_edge = saved;
    size_t saved_validation_work = owner.usage.validation_work;
    --owner.usage.validation_work;
    CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
    owner.usage.validation_work = saved_validation_work;
    size_t saved_build_work = owner.usage.build_work;
    --owner.usage.build_work;
    CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
    owner.usage.build_work = saved_build_work;

    SolMirLinkageSymbol saved_symbol = owner.imports[0].symbol;
    memset(owner.imports[0].symbol.bytes, 'x',
        sizeof(owner.imports[0].symbol.bytes));
    CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
    owner.imports[0].symbol = saved_symbol;
    owner.imports[0].identity.bytes[0] ^= 1;
    CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
    owner.imports[0].identity.bytes[0] ^= 1;

    SolMirRuntimeCall saved_call = owner.calls[0];
    owner.calls[0].normal_edge = SOL_MIR_RUNTIME_NONE;
    CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
    owner.calls[0] = saved_call;
    owner.calls[0].failure_site = owner.failure_site_count;
    CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
    owner.calls[0] = saved_call;
    owner.calls[0].result = (SolMirRuntimeValueRef){
        SOL_MIR_RUNTIME_VALUE_NONE, SOL_MIR_RUNTIME_NONE};
    CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
    owner.calls[0] = saved_call;
    if (owner.writeback_count != 0) {
        SolMirRuntimeWriteback saved_writeback = owner.writebacks[0];
        owner.writebacks[0].operand = SOL_MIR_RUNTIME_NONE;
        CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
        owner.writebacks[0] = saved_writeback;
    }
    SolMirRuntimeFailureSite saved_site = owner.failure_sites[0];
    owner.failure_sites[0].origin_kind
        = (SolMirRuntimeFailureOriginKind)99;
    CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
    owner.failure_sites[0] = saved_site;
    owner.failure_sites[0].allowed_codes ^= UINT32_C(1);
    CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
    owner.failure_sites[0] = saved_site;
    ++owner.failure_sites[0].source.end;
    CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
    owner.failure_sites[0] = saved_site;

    SolMirRuntimeFailureRecord forged_failure = {0};
    CHECK(failure_for_code(&owner, SOL_MIR_RUNTIME_FAILURE_PANIC,
        &forged_failure));
    ++forged_failure.source.end;
    CHECK(!sol_mir_runtime_failure_record_validate(&owner, &forged_failure));
    forged_failure.source
        = owner.failure_sites[owner.calls[0].failure_site].source;
    CHECK(!sol_mir_runtime_failure_record_validate(&owner, &forged_failure));
    CHECK(failure_for_code(&owner, SOL_MIR_RUNTIME_FAILURE_PANIC,
        &forged_failure));
    forged_failure.detail_kind = SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT;
    static const uint8_t oversized_detail[SOL_MIR_RUNTIME_HOST_DETAIL_MAX + 1]
        = {0};
    forged_failure.bytes = oversized_detail;
    forged_failure.length = sizeof(oversized_detail);
    CHECK(!sol_mir_runtime_failure_record_validate(&owner, &forged_failure));
    SolMirRuntimeExit untouched = {.kind = SOL_MIR_RUNTIME_EXIT_BOUNDARY_ERROR,
        .driver_status = 77, .application_status = 88,
        .boundary_code = "unchanged",
        .failure_code = SOL_MIR_RUNTIME_FAILURE_HOST_ERROR};
    SolMirRuntimeExit before_exit = untouched;
    CHECK(!sol_mir_runtime_exit_map(&owner, 0, 0, &forged_failure, &untouched)
        && memcmp(&untouched, &before_exit, sizeof(untouched)) == 0);
    CHECK(failure_for_code(&owner, SOL_MIR_RUNTIME_FAILURE_PANIC,
        &forged_failure));
    --owner.failure_site_count;
    CHECK(!sol_mir_runtime_failure_record_validate(&owner, &forged_failure)
        && !sol_mir_runtime_exit_map(&owner, 0, 0, &forged_failure, &untouched)
        && memcmp(&untouched, &before_exit, sizeof(untouched)) == 0);
    ++owner.failure_site_count;

    SolMirRuntimeSignature *signatures = owner.signatures;
    owner.signatures = (SolMirRuntimeSignature *)(void *)program->linkage.callables;
    CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
    owner.signatures = signatures;
    owner.signatures = (SolMirRuntimeSignature *)(void *)&owner;
    CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
    owner.signatures = signatures;
    owner.signatures = (SolMirRuntimeSignature *)(void *)program->operations.callables;
    CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
    owner.signatures = signatures;
    owner.signatures = (SolMirRuntimeSignature *)(void *)program->materialization.blocks;
    CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
    owner.signatures = signatures;
    owner.signatures = (SolMirRuntimeSignature *)(void *)program->program.ir->expressions;
    CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
    owner.signatures = signatures;
    owner.signatures = (SolMirRuntimeSignature *)(void *)program->program.ir->source_bytes;
    CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
    owner.signatures = signatures;

#define RUNTIME_OWNER_ALIAS(member, type) do { \
    type *saved_pointer = owner.member; \
    owner.member = (type *)(void *)&owner; \
    CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL)); \
    owner.member = saved_pointer; \
} while (0)
    RUNTIME_OWNER_ALIAS(signatures, SolMirRuntimeSignature);
    RUNTIME_OWNER_ALIAS(signature_slots, SolMirRuntimeSignatureSlot);
    RUNTIME_OWNER_ALIAS(calls, SolMirRuntimeCall);
    RUNTIME_OWNER_ALIAS(operands, SolMirRuntimeOperand);
    RUNTIME_OWNER_ALIAS(writebacks, SolMirRuntimeWriteback);
    RUNTIME_OWNER_ALIAS(entries, SolMirRuntimeEntry);
    RUNTIME_OWNER_ALIAS(imports, SolMirRuntimeImport);
    RUNTIME_OWNER_ALIAS(failure_sites, SolMirRuntimeFailureSite);
#undef RUNTIME_OWNER_ALIAS
#define BORROWED_ALIAS(pointer) do { \
    if ((pointer) != NULL) { \
        owner.signatures = (SolMirRuntimeSignature *)(void *)(pointer); \
        CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL)); \
        owner.signatures = signatures; \
    } \
} while (0)
    BORROWED_ALIAS(program->program.roots);
    BORROWED_ALIAS(program->plan.types);
    BORROWED_ALIAS(program->materialization.images);
    BORROWED_ALIAS(program->representation.recipes);
    BORROWED_ALIAS(program->layout.types);
    BORROWED_ALIAS(program->operations.access_plans);
    BORROWED_ALIAS(program->linkage.callables);
    const SolIr *ir = program->program.ir;
    BORROWED_ALIAS(ir->definitions); BORROWED_ALIAS(ir->callables);
    BORROWED_ALIAS(ir->types); BORROWED_ALIAS(ir->type_ids);
    BORROWED_ALIAS(ir->accesses); BORROWED_ALIAS(ir->members);
    BORROWED_ALIAS(ir->evidence); BORROWED_ALIAS(ir->locals);
    BORROWED_ALIAS(ir->fields); BORROWED_ALIAS(ir->variants);
    BORROWED_ALIAS(ir->expressions); BORROWED_ALIAS(ir->places);
    BORROWED_ALIAS(ir->projections); BORROWED_ALIAS(ir->statements);
    BORROWED_ALIAS(ir->statement_ids); BORROWED_ALIAS(ir->arms);
    BORROWED_ALIAS(ir->arm_ids); BORROWED_ALIAS(ir->patterns);
    BORROWED_ALIAS(ir->pattern_children); BORROWED_ALIAS(ir->operands);
    BORROWED_ALIAS(ir->roots); BORROWED_ALIAS(ir->obligations);
    BORROWED_ALIAS(ir->snapshots); BORROWED_ALIAS(ir->cleanup_locals);
    BORROWED_ALIAS(ir->effects); BORROWED_ALIAS(ir->generic_parameters);
    BORROWED_ALIAS(ir->effect_parameters); BORROWED_ALIAS(ir->loop_obligations);
    BORROWED_ALIAS(ir->unreachable_obligations); BORROWED_ALIAS(ir->files);
    BORROWED_ALIAS(ir->source_path); BORROWED_ALIAS(ir->source_bytes);
    if (ir->file_count != 0) BORROWED_ALIAS(ir->files[0].path);
    for (size_t i = 0; i < ir->definition_count; ++i)
        if (ir->definitions[i].name != NULL) {
            BORROWED_ALIAS(ir->definitions[i].name); break;
        }
    for (size_t i = 0; i < ir->expression_count; ++i)
        if (ir->expressions[i].kind == SOL_IR_EXPR_STRING) {
            BORROWED_ALIAS(ir->expressions[i].as.string); break;
        }
#undef BORROWED_ALIAS

    SolMirConcreteProgram *mutable_program = (SolMirConcreteProgram *)(void *)program;
    size_t predecessor_count = mutable_program->operations.callable_count;
    size_t predecessor_capacity = mutable_program->operations.callable_capacity;
    if (predecessor_capacity != SIZE_MAX) {
        mutable_program->operations.callable_count = predecessor_capacity + 1;
        CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
        mutable_program->operations.callable_count = predecessor_count;
    }
    mutable_program->operations.callable_capacity = SIZE_MAX;
    CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
    mutable_program->operations.callable_capacity = predecessor_capacity;

    request.limits = NULL;
    for (SolMirRuntimeBuildTestCategory category
            = SOL_MIR_RUNTIME_BUILD_TEST_HASH;
        category < SOL_MIR_RUNTIME_BUILD_TEST_CATEGORY_COUNT; ++category) {
        sol_mir_runtime_conventions_test_force_build_category(category);
        CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
            == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED);
        CHECK(limited.concrete == NULL
            && sol_mir_runtime_conventions_test_build_category_attempts(category)
                == 1
            && sol_mir_runtime_conventions_test_build_events_after_exhaustion()
                == 0);
    }
    sol_mir_runtime_conventions_test_force_build_category(
        SOL_MIR_RUNTIME_BUILD_TEST_CATEGORY_COUNT);

    sol_mir_concrete_test_force_validation_allocation_failure(true);
    SolMirRuntimeFailureRecord allocation_free_failure = {0};
    SolMirRuntimeExit allocation_free_exit;
    CHECK(failure_for_code(&owner, SOL_MIR_RUNTIME_FAILURE_PANIC,
        &allocation_free_failure)
        && sol_mir_runtime_failure_record_validate(&owner,
            &allocation_free_failure)
        && sol_mir_runtime_exit_map(&owner, 0, 0, &allocation_free_failure,
            &allocation_free_exit)
        && sol_mir_concrete_test_validation_allocation_attempts() == 0);
    sol_mir_runtime_conventions_test_force_allocation_failure(true);
    sol_mir_runtime_conventions_test_force_host_collision(true);
    request.limits = NULL;
    CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SYMBOL_COLLISION);
    sol_mir_runtime_conventions_test_force_host_collision(false);
    size_t saved_linkage_validation_work
        = mutable_program->linkage.usage.validation_work;
    size_t saved_linkage_validation_limit
        = mutable_program->linkage.limits.max_validation_work;
    mutable_program->linkage.usage.validation_work = SIZE_MAX;
    mutable_program->linkage.limits.max_validation_work = SIZE_MAX;
    sol_mir_runtime_conventions_test_force_host_collision(true);
    CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SYMBOL_COLLISION
        && sol_mir_concrete_test_validation_allocation_attempts() == 0);
    sol_mir_runtime_conventions_test_force_host_collision(false);
    mutable_program->linkage.usage.validation_work
        = saved_linkage_validation_work;
    mutable_program->linkage.limits.max_validation_work
        = saved_linkage_validation_limit;
    sol_mir_runtime_conventions_test_force_recipe_collision(true);
    CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SYMBOL_COLLISION);
    sol_mir_runtime_conventions_test_force_recipe_collision(false);
    sol_mir_runtime_conventions_test_force_host_symbol_collision(true);
    CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SYMBOL_COLLISION);
    sol_mir_runtime_conventions_test_force_host_symbol_collision(false);
    sol_mir_runtime_conventions_test_force_recipe_symbol_collision(true);
    CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SYMBOL_COLLISION);
    sol_mir_runtime_conventions_test_force_recipe_symbol_collision(false);
    CHECK(sol_mir_concrete_test_validation_allocation_attempts() == 0);
    exact = exact_limits(&owner); --exact.max_validation_work;
    request.limits = &exact;
    CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED
        && sol_mir_concrete_test_validation_allocation_attempts() == 0);
    request.limits = NULL;
    size_t host_capacity = mutable_program->linkage.host_requirement_capacity;
    SolMirLinkageHostRequirement *hosts
        = mutable_program->linkage.host_requirements;
    CHECK(mutable_program->linkage.host_requirement_count != 0);
    if (mutable_program->linkage.host_requirement_count != 0) {
        mutable_program->linkage.host_requirement_capacity
            = mutable_program->linkage.host_requirement_count - 1;
        CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
            == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INVALID_CONCRETE_PROGRAM
            && sol_mir_concrete_test_validation_allocation_attempts() == 0);
        mutable_program->linkage.host_requirement_capacity = host_capacity;
        mutable_program->linkage.host_requirements = NULL;
        CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
            == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INVALID_CONCRETE_PROGRAM
            && sol_mir_concrete_test_validation_allocation_attempts() == 0);
        mutable_program->linkage.host_requirements = hosts;
        mutable_program->linkage.host_requirement_capacity = SIZE_MAX;
        CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
            == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INVALID_CONCRETE_PROGRAM
            && sol_mir_concrete_test_validation_allocation_attempts() == 0);
        mutable_program->linkage.host_requirement_capacity = host_capacity;
    }
    CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_ALLOCATION_FAILED
        && sol_mir_concrete_test_validation_allocation_attempts() != 0);
    diagnostics->allocation_failed = false;
    sol_mir_concrete_test_force_validation_allocation_failure(false);
    exact = exact_limits(&owner); --exact.max_build_work;
    request.limits = &exact;
    CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED);
    exact = exact_limits(&owner); --exact.max_validation_scratch_bytes;
    request.limits = &exact;
    CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED);
    request.limits = NULL;
    CHECK(sol_mir_runtime_conventions_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_ALLOCATION_FAILED
        && limited.concrete == NULL);
    sol_mir_runtime_conventions_test_force_allocation_failure(false);

    free(first); sol_mir_runtime_conventions_free(&owner);
}

static void test_supported_call_vocabulary(void) {
    static const char source[] =
        "module runtime_call_vocabulary\n"
        "function owned(value: Text) -> Int64 effects { pure } { return 1 }\n"
        "function shared(value: borrow Text) -> Int64 effects { pure } { return 2 }\n"
        "function update(first: inout Int64, second: inout Int64) -> () "
        "effects { pure } { first = 7 second = 8 }\n"
        "function root() -> Int64 effects { pure } { owned(\"owned\") "
        "let text = \"shared\" let read = shared(text) "
        "var first = read var second = 2 update(first, second) return first }\n";
    TextCompilation c; CHECK(compile_text(&c, source));
    SolMirConcreteProgram program; sol_mir_concrete_program_init(&program);
    bool built = build_text_concrete(&c, "root", NULL, 0, &program);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &c.diagnostics);
    CHECK(built);
    SolMirRuntimeConventions owner; sol_mir_runtime_conventions_init(&owner);
    SolMirRuntimeConventionsBuildRequest request = {&program, NULL};
    CHECK(built && sol_mir_runtime_conventions_build(&request, &owner,
        &c.diagnostics) == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
    if (owner.concrete != NULL) {
        size_t access[3] = {0}; bool ordered_writebacks = false;
        size_t unit_call = SOL_MIR_RUNTIME_NONE;
        for (size_t i = 0; i < owner.operand_count; ++i) {
            size_t slot = owner.operands[i].signature_slot;
            CHECK(slot < owner.signature_slot_count);
            if (slot < owner.signature_slot_count)
                ++access[owner.signature_slots[slot].access];
        }
        for (size_t i = 0; i < owner.call_count; ++i) {
            const SolMirRuntimeCall *call = &owner.calls[i];
            CHECK(call->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL
                && call->owner_kind == SOL_MIR_RUNTIME_CALL_OWNER_IMAGE);
            if (call->writebacks.count == 2) {
                const SolMirRuntimeWriteback *w
                    = &owner.writebacks[call->writebacks.offset];
                ordered_writebacks = !w[0].receiver && w[0].formal == 0
                    && !w[1].receiver && w[1].formal == 1
                    && owner.signatures[call->signature].result_class
                        == SOL_MIR_RUNTIME_RESULT_UNIT
                    && call->result.kind == SOL_MIR_RUNTIME_VALUE_NONE;
            }
            if (owner.signatures[call->signature].result_class
                    == SOL_MIR_RUNTIME_RESULT_UNIT)
                unit_call = i;
        }
        CHECK(access[SOL_ACCESS_OWNED] != 0
            && access[SOL_ACCESS_SHARED] != 0
            && access[SOL_ACCESS_EXCLUSIVE] >= 2
            && ordered_writebacks);
        CHECK(sol_mir_runtime_conventions_validate(&owner, NULL));
        CHECK(unit_call != SOL_MIR_RUNTIME_NONE);
        if (unit_call != SOL_MIR_RUNTIME_NONE) {
            SolMirRuntimeValueRef saved = owner.calls[unit_call].result;
            owner.calls[unit_call].result = (SolMirRuntimeValueRef){
                SOL_MIR_RUNTIME_VALUE_MATERIALIZED_VALUE, 0};
            CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
            owner.calls[unit_call].result = saved;
        }
        SolMirRuntimeConventionsLimits limits = exact_limits(&owner);
        limits.max_writebacks = 1;
        SolMirRuntimeConventions limited;
        sol_mir_runtime_conventions_init(&limited);
        SolMirRuntimeConventionsBuildRequest limited_request
            = {&program, &limits};
        CHECK(sol_mir_runtime_conventions_build(&limited_request, &limited,
                &c.diagnostics)
            == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED);
        sol_mir_runtime_conventions_free(&limited);
    }
    sol_mir_runtime_conventions_free(&owner);
    sol_mir_concrete_program_free(&program); text_compilation_free(&c);
}

static void test_predicate_calls_and_callback_boundary(void) {
    static const char predicate_source[] =
        "module runtime_predicate_calls\n"
        "function positive(value: Int64) -> Bool effects { pure } { return value > 0 }\n"
        "function unused() -> Bool effects { pure } { return false }\n"
        "function root() -> Int64 effects { pure }\n"
        "requires { positive(1) && positive(2) }\n"
        "{ return 1 }\n";
    TextCompilation c; CHECK(compile_text(&c, predicate_source));
    SolMirConcreteProgram program; sol_mir_concrete_program_init(&program);
    bool built = build_text_concrete(&c, "root", NULL, 0, &program);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &c.diagnostics);
    CHECK(built);
    SolMirRuntimeConventions owner; sol_mir_runtime_conventions_init(&owner);
    SolMirRuntimeConventionsBuildRequest request = {&program, NULL};
    SolMirRuntimeConventionsBuildOutcome outcome = built
        ? sol_mir_runtime_conventions_build(&request, &owner, &c.diagnostics)
        : SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INTERNAL_FAILED;
    CHECK(outcome == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
    bool predicate_direct = false;
    size_t predicate_sources[2] = {SOL_IR_NONE, SOL_IR_NONE};
    size_t predicate_source_count = 0;
    for (size_t i = 0; i < owner.call_count; ++i) {
        const SolMirRuntimeCall *call = &owner.calls[i];
        predicate_direct |= call->owner_kind
                == SOL_MIR_RUNTIME_CALL_OWNER_PREDICATE
            && call->call_kind == SOL_IR_CALL_FUNCTION
            && call->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL;
        if (call->owner_kind == SOL_MIR_RUNTIME_CALL_OWNER_PREDICATE) {
            const SolMirPredicateTerminator *term
                = &program.operations.predicate_blocks[call->block].terminator;
            CHECK(term->binding < program.materialization.binding_count);
            if (term->binding < program.materialization.binding_count) {
                SolMirProgramSource expected
                    = program.materialization.bindings[term->binding].source;
                SolMirRuntimeSource expected_runtime;
                CHECK(call->failure_site < owner.failure_site_count
                    && expression_source(&c.ir, expected.callable,
                        expected.expression, &expected_runtime)
                    && memcmp(&owner.failure_sites[call->failure_site].source,
                        &expected_runtime, sizeof(expected_runtime)) == 0);
            }
            if (predicate_source_count < 2)
                predicate_sources[predicate_source_count++]
                    = program.materialization.bindings[term->binding]
                        .source.expression;
        }
    }
    CHECK(predicate_direct && predicate_source_count == 2
        && predicate_sources[0] != predicate_sources[1]);
    for (size_t i = 0; i < owner.call_count; ++i) {
        if (owner.calls[i].owner_kind
                != SOL_MIR_RUNTIME_CALL_OWNER_PREDICATE) continue;
        SolMirRuntimeFailureRecord failure = {
            .code = SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT,
            .source = owner.failure_sites[owner.calls[i].failure_site].source,
            .detail_kind = SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE,
        };
        CHECK(sol_mir_runtime_failure_record_validate(&owner, &failure));
        break;
    }
    if (program.operations.predicate_body_count != 0) {
        const SolMirPredicateBody *body = &program.operations.predicate_bodies[0];
        const SolMirPlanContext *context
            = &program.materialization.contexts[body->context];
        SolIrExpressionId predicate
            = c.ir.obligations[context->obligation].predicate;
        SolIrCallableId predicate_callable = body->owner_kind
                == SOL_MIR_PREDICATE_OWNER_INSTANCE
            ? program.materialization.images[body->instance].source_callable
            : program.materialization.imports[body->import].source_callable;
        SolMirRuntimeFailureRecord provenance = {
            .code = SOL_MIR_RUNTIME_FAILURE_REQUIRE_VIOLATION,
            .detail_kind = SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE,
        };
        CHECK(c.ir.expressions[predicate].kind != SOL_IR_EXPR_CALL
            && expression_source(&c.ir, predicate_callable, predicate,
                &provenance.source)
            && sol_mir_runtime_failure_record_validate(&owner, &provenance));
        SolIrCallableId unused = callable(&c.ir, "unused",
            SOL_IR_CALLABLE_FUNCTION);
        bool unused_materialized = false;
        for (size_t i = 0; i < program.materialization.image_count; ++i)
            unused_materialized |= program.materialization.images[i].source_callable
                == unused;
        CHECK(unused < c.ir.callable_count && !unused_materialized
            && c.ir.expressions[c.ir.callables[unused].body].kind
                != SOL_IR_EXPR_CALL
            && expression_source(&c.ir, unused, c.ir.callables[unused].body,
                &provenance.source)
            && !sol_mir_runtime_failure_record_validate(&owner, &provenance));
    }
    CHECK(owner.concrete == NULL
        || sol_mir_runtime_conventions_validate(&owner, NULL));
    sol_mir_runtime_conventions_free(&owner);
    sol_mir_concrete_program_free(&program); text_compilation_free(&c);

    static const char callback_source[] =
        "module runtime_indirect_calls\n"
        "enum Choice { yes(value: Int64), no }\n"
        "type Meter = distinct Int64\n"
        "function increment(value: Int64) -> Int64 effects { pure } { return value + 1 }\n"
        "function apply(value: Int64, callback: function(Int64) -> Int64 effects { pure }) "
        "-> Int64 effects { pure } { return callback(value) }\n"
        "function make_choice() -> Choice { return Choice.yes(value = 1) }\n"
        "function make_meter() -> Meter { return Meter(1) }\n"
        "function root() -> Int64 effects { pure } { return apply(1, increment) }\n";
    TextCompilation callback; CHECK(compile_text(&callback, callback_source));
    sol_mir_concrete_program_init(&program);
    CHECK(build_text_concrete_root_outcome(&callback, "root",
            SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE, NULL, 0, &program)
        == SOL_MIR_CONCRETE_BUILD_UNSUPPORTED_CLOSURE);
    sol_mir_concrete_program_free(&program); text_compilation_free(&callback);
}

static void test_indirect_predicate_and_bound_environment(void) {
    static const char source[] =
        "module runtime_bound_environment\n"
        "capability Base { function choose(value: Int64) -> Bool effects { pure } }\n"
        "function callback(value: Int64) -> Bool effects { pure } { return true }\n"
        "function root(base: capability Base) -> Bool effects { pure } "
        "requires { { let exact = callback let first = base.choose "
        "let second = base.choose exact(1) && first(1) && second(2) } } "
        "{ return callback(1) && base.choose(1) }\n";
    TextCompilation c; CHECK(compile_text(&c, source));
    SolIrCallableId choose = callable(&c.ir, "choose", SOL_IR_CALLABLE_CAPABILITY);
    SolMirConcreteProgram program; sol_mir_concrete_program_init(&program);
    bool built = build_text_concrete(&c, "root", &choose, 1, &program);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &c.diagnostics);
    CHECK(built);
    SolMirRuntimeConventions owner; sol_mir_runtime_conventions_init(&owner);
    SolMirRuntimeConventionsBuildRequest request = {&program, NULL};
    CHECK(built && sol_mir_runtime_conventions_build(&request, &owner,
        &c.diagnostics) == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
    bool bound_environment = false;
    for (size_t i = 0; i < owner.import_count; ++i)
        bound_environment |= owner.imports[i].kind
            == SOL_MIR_RUNTIME_IMPORT_RECIPE_BOUND_ENVIRONMENT;
    bool capability_callee_rejected = false;
    SolIrCallableId root = callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION);
    for (size_t i = 0; i < c.ir.expression_count; ++i) {
        const SolIrExpression *expression = &c.ir.expressions[i];
        if (expression->kind != SOL_IR_EXPR_CALL) continue;
        if (expression->as.call.kind == SOL_IR_CALL_CAPABILITY)
            capability_callee_rejected |= !expression_failure_source_valid(
                &program, &owner, root,
                expression->as.call.callee,
                SOL_MIR_RUNTIME_FAILURE_REQUIRE_VIOLATION);
    }
    CHECK(bound_environment);
    CHECK(capability_callee_rejected);
    const uint32_t host_codes
        = UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_HOST_CALL_LIMIT - 1)
        | UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_HOST_ERROR - 1);
    const uint32_t call_depth
        = UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT - 1);
    bool indirect_internal = false, indirect_host = false;
    for (size_t i = 0; i < program.linkage.table_entry_count; ++i) {
        SolMirRuntimeCall call = {
            .target_kind = SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE,
            .internal = SOL_MIR_LINKAGE_NONE,
            .host = SOL_MIR_LINKAGE_NONE,
            .table = i,
        };
        bool targets_host
            = program.linkage.table_entries[i].target_kind
                == SOL_MIR_LINKAGE_TARGET_HOST;
        indirect_internal |= !targets_host;
        indirect_host |= targets_host;
        uint32_t build_mask = 0, validate_mask = 0;
        CHECK(sol_mir_runtime_conventions_test_build_call_failure_mask(
                &call, &program.linkage, &build_mask)
            && sol_mir_runtime_conventions_test_validate_call_failure_mask(
                &call, &program.linkage, &validate_mask));
        uint32_t expected = call_depth | (targets_host ? host_codes : 0);
        CHECK(build_mask == expected && validate_mask == expected);
    }
    CHECK(indirect_internal && indirect_host);
    CHECK(sol_mir_runtime_conventions_validate(&owner, NULL));
    CHECK(sol_mir_runtime_conventions_test_observed_validation_work()
        == owner.usage.validation_work);
    const uint32_t all_operations = SOL_MIR_LINKAGE_RUNTIME_CREATE
        | SOL_MIR_LINKAGE_RUNTIME_COPY | SOL_MIR_LINKAGE_RUNTIME_DROP
        | SOL_MIR_LINKAGE_RUNTIME_EQUAL
        | SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT;
    bool sparse = false, mutated = false;
    for (size_t i = 0; i < program.linkage.runtime_requirement_count; ++i) {
        SolMirLinkageRuntimeRequirement *requirement
            = &program.linkage.runtime_requirements[i];
        uint32_t missing = all_operations & ~requirement->operations;
        sparse |= missing != 0;
        if (!mutated && missing != 0) {
            uint32_t saved = requirement->operations;
            requirement->operations |= missing & (0u - missing);
            CHECK(!sol_mir_runtime_conventions_validate(&owner, NULL));
            requirement->operations = saved;
            mutated = true;
        }
    }
    CHECK(sparse && mutated && sol_mir_runtime_conventions_validate(&owner, NULL));
    CHECK(sol_mir_runtime_conventions_test_observed_validation_work()
        == owner.usage.validation_work);
    sol_mir_runtime_conventions_free(&owner);
    sol_mir_concrete_program_free(&program); text_compilation_free(&c);
}

static void test_unit_entry_exit(void) {
    static const char source[] =
        "module runtime_unit_entry\n"
        "@entry\n"
        "public function launch() -> () effects { pure } { return () }\n";
    TextCompilation c; CHECK(compile_text(&c, source));
    SolMirConcreteProgram program; sol_mir_concrete_program_init(&program);
    bool built = build_text_concrete_root(&c, "launch",
        SOL_MIR_PROGRAM_ROOT_ENTRY, NULL, 0, &program);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &c.diagnostics);
    CHECK(built);
    SolMirRuntimeConventions owner; sol_mir_runtime_conventions_init(&owner);
    SolMirRuntimeConventionsBuildRequest request = {&program, NULL};
    CHECK(built && sol_mir_runtime_conventions_build(&request, &owner,
        &c.diagnostics) == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
    if (owner.entry_count == 1) {
        SolMirRuntimeExit exit = {.kind = (SolMirRuntimeExitKind)99};
        CHECK(owner.entries[0].result_class == SOL_MIR_RUNTIME_RESULT_UNIT);
        CHECK(sol_mir_runtime_exit_map(&owner, 0, 99, NULL, &exit)
            && exit.kind == SOL_MIR_RUNTIME_EXIT_APPLICATION
            && exit.driver_status == 0 && exit.application_status == 0);
        SolMirRuntimeExit unchanged = exit;
        CHECK(!sol_mir_runtime_exit_map(&owner, 1, 0, NULL, &exit)
            && memcmp(&exit, &unchanged, sizeof(exit)) == 0);
    }
    sol_mir_runtime_conventions_free(&owner);
    sol_mir_concrete_program_free(&program); text_compilation_free(&c);
}

static void test_stable_rendering(void) {
    static const char source[] =
        "module runtime_render_stable\n"
        "capability Clock { function read() -> Int64 effects { pure } }\n"
        "function first(clock: capability Clock) -> Int64 effects { pure } "
        "{ return clock.read() }\n"
        "function second() -> Int64 effects { pure } { return 2 }\n";
    static const char inserted_source[] =
        "module runtime_render_stable\n"
        "record Unused { value: Bool }\n"
        "function unused(value: Bool) -> Bool effects { pure } { return value }\n"
        "capability Clock { function read() -> Int64 effects { pure } }\n"
        "function first(clock: capability Clock) -> Int64 effects { pure } "
        "{ return clock.read() }\n"
        "function second() -> Int64 effects { pure } { return 2 }\n";
    static const char *const normal_roots[] = {"first", "second"};
    static const char *const reversed_roots[] = {"second", "first"};
    const char *texts[] = {source, source, source, inserted_source};
    const char *paths[] = {"/checkout/a/runtime.sol", "/checkout/a/runtime.sol",
        "/relocated/b/runtime.sol", "/inserted/c/runtime.sol"};
    const char *const *orders[] = {normal_roots, reversed_roots, normal_roots,
        normal_roots};
    TextCompilation compilations[4];
    SolMirConcreteProgram programs[4];
    SolMirRuntimeConventions owners[4];
    char *rendered[4] = {NULL, NULL, NULL, NULL};
    for (size_t i = 0; i < 4; ++i) {
        CHECK(compile_text_at(&compilations[i], paths[i], texts[i]));
        sol_mir_concrete_program_init(&programs[i]);
        SolIrCallableId import = callable(&compilations[i].ir, "read",
            SOL_IR_CALLABLE_CAPABILITY);
        bool built = build_text_concrete_roots(&compilations[i], orders[i], 2,
            &import, 1, &programs[i]);
        CHECK(built);
        sol_mir_runtime_conventions_init(&owners[i]);
        SolMirRuntimeConventionsBuildRequest request = {&programs[i], NULL};
        CHECK(built && sol_mir_runtime_conventions_build(&request, &owners[i],
            &compilations[i].diagnostics)
            == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
        if (owners[i].concrete != NULL) rendered[i] = render(&owners[i]);
        CHECK(rendered[i] != NULL);
    }
    CHECK(rendered[0] != NULL && rendered[1] != NULL
        && rendered[2] != NULL && rendered[3] != NULL
        && strcmp(rendered[0], rendered[1]) == 0
        && strcmp(rendered[0], rendered[2]) == 0
        && strcmp(rendered[0], rendered[3]) == 0);
    for (size_t i = 0; i < 4; ++i) {
        free(rendered[i]); sol_mir_runtime_conventions_free(&owners[i]);
        sol_mir_concrete_program_free(&programs[i]);
        text_compilation_free(&compilations[i]);
    }
}

static void test_never_callable_boundary(void) {
    static const char source[] =
        "module runtime_never_boundary\n"
        "function stop() -> Never effects { panic } { panic \"stop\" }\n"
        "function root() -> Int64 effects { panic } { stop() return 0 }\n";
    TextCompilation c;
    CHECK(!compile_text(&c, source));
    bool unresolved_never = false;
    for (size_t i = 0; i < c.diagnostics.count; ++i)
        unresolved_never |= strcmp(c.diagnostics.items[i].code,
            "SOL-TYPE-009") == 0;
    CHECK(unresolved_never);
    text_compilation_free(&c);
}

static void test_computed_place_call_provenance(void) {
    static const char source[] =
        "module runtime_computed_place_provenance\n"
        "record Box { value: Int64 }\n"
        "function make(value: Int64) -> Box effects { pure } "
        "{ return Box { value = value } }\n"
        "function computed() -> Int64 effects { pure } "
        "{ return make(1).value }\n"
        "function positive(value: Int64) -> Bool effects { pure } "
        "{ return value > 0 }\n"
        "function root(box: Box) -> Int64 effects { pure }\n"
        "requires { positive(box.value) }\n"
        "{ return 0 }\n";
    TextCompilation c; CHECK(compile_text(&c, source));
    SolMirConcreteProgram program; sol_mir_concrete_program_init(&program);
    bool built = build_text_concrete(&c, "root", NULL, 0, &program);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &c.diagnostics);
    CHECK(built);
    SolMirRuntimeConventions owner; sol_mir_runtime_conventions_init(&owner);
    SolMirRuntimeConventionsBuildRequest request = {&program, NULL};
    CHECK(built && sol_mir_runtime_conventions_build(&request, &owner,
        &c.diagnostics) == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
    bool computed_root = false, projected_local = false;
    bool computed_call_rejected = false, projected_local_owned = false;
    SolIrCallableId computed = callable(&c.ir, "computed",
        SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId root = callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION);
    for (size_t i = 0; i < c.ir.expression_count; ++i) {
        const SolIrExpression *expression = &c.ir.expressions[i];
        if (expression->kind != SOL_IR_EXPR_PLACE
            || expression->as.place >= c.ir.place_count) continue;
        const SolIrPlace *place = &c.ir.places[expression->as.place];
        bool computed_place = place->root_kind == SOL_IR_PLACE_ROOT_TEMPORARY
            && place->temporary < c.ir.expression_count
            && c.ir.expressions[place->temporary].kind == SOL_IR_EXPR_CALL
            && place->projections.count != 0;
        computed_root |= computed_place;
        if (computed_place && computed < c.ir.callable_count)
            computed_call_rejected
                |= !expression_failure_source_valid(&program, &owner, computed,
                    place->temporary, SOL_MIR_RUNTIME_FAILURE_PANIC);
        projected_local |= place->root_kind == SOL_IR_PLACE_ROOT_LOCAL
            && place->projections.count != 0;
        if (place->root_kind == SOL_IR_PLACE_ROOT_LOCAL
            && place->projections.count != 0 && root < c.ir.callable_count)
            projected_local_owned |= !expression_failure_source_valid(&program,
                &owner, root, i, SOL_MIR_RUNTIME_FAILURE_REQUIRE_VIOLATION);
    }
    CHECK(computed_root && computed_call_rejected && projected_local
        && projected_local_owned && owner.call_count != 0);
    if (owner.concrete != NULL) check_all_call_sources(&owner);
    sol_mir_runtime_conventions_free(&owner);
    sol_mir_concrete_program_free(&program); text_compilation_free(&c);
}

static void test_deep_expression_provenance(void) {
    enum { TERM_COUNT = 192, SOURCE_CAPACITY = 4096 };
    char *source = malloc(SOURCE_CAPACITY);
    CHECK(source != NULL);
    if (source == NULL) return;
    size_t used = (size_t)snprintf(source, SOURCE_CAPACITY,
        "module runtime_deep_provenance\n"
        "function root() -> Int64 effects { pure } { return 1");
    for (size_t i = 1; i < TERM_COUNT && used < SOURCE_CAPACITY; ++i)
        used += (size_t)snprintf(source + used, SOURCE_CAPACITY - used, " + 1");
    if (used < SOURCE_CAPACITY)
        used += (size_t)snprintf(source + used, SOURCE_CAPACITY - used, " }\n");
    CHECK(used < SOURCE_CAPACITY);
    TextCompilation c;
    SolMirConcreteProgram program;
    SolMirRuntimeConventions owner;
    bool built = used < SOURCE_CAPACITY
        && build_text_runtime(&c, source, NULL, 0, &program, &owner);
    if (!built && used < SOURCE_CAPACITY)
        sol_diagnostics_render_human(stderr, &c.source, &c.diagnostics);
    CHECK(built);
    if (built) {
        SolIrCallableId root = callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION);
        SolIrExpressionId first = SOL_IR_NONE;
        for (size_t i = 0; i < c.ir.expression_count; ++i)
            if (c.ir.expressions[i].kind == SOL_IR_EXPR_INTEGER) {
                first = i;
                break;
            }
        CHECK(root != SOL_IR_NONE && first != SOL_IR_NONE
            && !expression_failure_source_valid(&program, &owner, root, first,
                SOL_MIR_RUNTIME_FAILURE_PANIC));
        sol_mir_runtime_conventions_free(&owner);
        sol_mir_concrete_program_free(&program);
        text_compilation_free(&c);
    }
    free(source);
}

static void test_rich_predicate_failure_sites(void) {
    static const char source[] =
        "module runtime_rich_predicate_sites\n"
        "record Box { value: Int64 }\n"
        "enum Choice { yes(value: Int64), no }\n"
        "type Positive = refined Int64 where 10 / self > 0\n"
        "function helper(value: Int64) -> Bool effects { pure } { return true }\n"
        "function root(value: Int64, box: Box) -> Bool effects { pure } "
        "requires { helper(value) && Positive(value) == Positive(value) && "
        "box.value > 0 && match Choice.yes(value) { "
        "yes(item) if item > 0 => true _ => false } } { return true }\n";
    TextCompilation c;
    SolMirConcreteProgram program;
    SolMirRuntimeConventions owner;
    bool built = build_text_runtime(&c, source, NULL, 0, &program, &owner);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &c.diagnostics);
    CHECK(built);
    if (!built) return;
    size_t predicate_arithmetic = 0, predicate_calls = 0;
    size_t predicate_no_match = 0, predicate_results = 0;
    for (size_t i = 0; i < owner.failure_site_count; ++i) {
        const SolMirRuntimeFailureSite *site = &owner.failure_sites[i];
        predicate_arithmetic += site->origin_kind
            == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC;
        predicate_calls += site->origin_kind
            == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_CALL;
        predicate_no_match += site->origin_kind
            == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_NO_MATCH;
        predicate_results += site->origin_kind
            == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT;
        for (unsigned code = SOL_MIR_RUNTIME_FAILURE_PANIC;
                code <= SOL_MIR_RUNTIME_FAILURE_HOST_ERROR; ++code) {
            if ((site->allowed_codes & (UINT32_C(1) << (code - 1))) == 0)
                continue;
            SolMirRuntimeFailureRecord failure = {
                .code = (SolMirRuntimeFailureCode)code,
                .source = site->source,
                .detail_kind = code == SOL_MIR_RUNTIME_FAILURE_PANIC
                    ? SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT
                    : code == SOL_MIR_RUNTIME_FAILURE_HOST_ERROR
                        ? SOL_MIR_RUNTIME_FAILURE_DETAIL_HOST_BYTES
                        : SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE,
            };
            CHECK(sol_mir_runtime_failure_record_validate(&owner, &failure));
        }
    }
    CHECK(predicate_arithmetic != 0 && predicate_calls != 0
        && predicate_no_match != 0 && predicate_results >= 2
        && sol_mir_runtime_conventions_validate(&owner, NULL));
    SolIrCallableId root = callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION);
    bool projected_rejected = false;
    for (size_t i = 0; i < c.ir.expression_count; ++i) {
        const SolIrExpression *expression = &c.ir.expressions[i];
        if (expression->kind != SOL_IR_EXPR_PLACE
            || expression->as.place >= c.ir.place_count) continue;
        const SolIrPlace *place = &c.ir.places[expression->as.place];
        if (place->root_kind == SOL_IR_PLACE_ROOT_LOCAL
            && place->projections.count != 0)
            projected_rejected |= !expression_failure_source_valid(&program,
                &owner, root, i, SOL_MIR_RUNTIME_FAILURE_REQUIRE_VIOLATION);
    }
    CHECK(projected_rejected);
    sol_mir_runtime_conventions_free(&owner);
    sol_mir_concrete_program_free(&program);
    text_compilation_free(&c);
}

static void test_import_preflight_stress(void) {
    enum { IMPORT_COUNT = 16, SOURCE_CAPACITY = 32768 };
    char *source = malloc(SOURCE_CAPACITY);
    SolIrCallableId imports[IMPORT_COUNT];
    CHECK(source != NULL);
    if (source == NULL) return;
    size_t used = (size_t)snprintf(source, SOURCE_CAPACITY,
        "module runtime_import_stress\ncapability Host {\n");
    for (size_t i = 0; i < IMPORT_COUNT && used < SOURCE_CAPACITY; ++i) {
        int written = snprintf(source + used, SOURCE_CAPACITY - used,
            "function operation%zu() -> Int64 effects { pure }\n", i);
        CHECK(written >= 0 && (size_t)written < SOURCE_CAPACITY - used);
        if (written < 0 || (size_t)written >= SOURCE_CAPACITY - used) {
            used = SOURCE_CAPACITY; break;
        }
        used += (size_t)written;
    }
    if (used < SOURCE_CAPACITY) {
        int written = snprintf(source + used, SOURCE_CAPACITY - used,
            "}\nfunction root(host: capability Host) -> Int64 effects { pure } {\n");
        CHECK(written >= 0 && (size_t)written < SOURCE_CAPACITY - used);
        if (written < 0 || (size_t)written >= SOURCE_CAPACITY - used)
            used = SOURCE_CAPACITY;
        else used += (size_t)written;
    }
    for (size_t i = 0; i < IMPORT_COUNT && used < SOURCE_CAPACITY; ++i) {
        int written = snprintf(source + used, SOURCE_CAPACITY - used,
            "host.operation%zu()\n", i);
        CHECK(written >= 0 && (size_t)written < SOURCE_CAPACITY - used);
        if (written < 0 || (size_t)written >= SOURCE_CAPACITY - used) {
            used = SOURCE_CAPACITY; break;
        }
        used += (size_t)written;
    }
    if (used < SOURCE_CAPACITY) {
        int written = snprintf(source + used, SOURCE_CAPACITY - used,
            "return 0\n}\n");
        CHECK(written >= 0 && (size_t)written < SOURCE_CAPACITY - used);
        if (written < 0 || (size_t)written >= SOURCE_CAPACITY - used)
            used = SOURCE_CAPACITY;
    }
    TextCompilation c;
    bool compiled = used < SOURCE_CAPACITY && compile_text(&c, source);
    CHECK(compiled);
    free(source);
    if (!compiled) return;
    for (size_t i = 0; i < IMPORT_COUNT; ++i) {
        char name[32]; (void)snprintf(name, sizeof(name), "operation%zu", i);
        imports[i] = callable(&c.ir, name, SOL_IR_CALLABLE_CAPABILITY);
        CHECK(imports[i] != SOL_IR_NONE);
    }
    SolMirConcreteProgram program; sol_mir_concrete_program_init(&program);
    SolMirProgramRoot root = {callable(&c.ir, "root",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    SolMirConcreteLimits concrete_limits = sol_mir_concrete_default_limits();
    concrete_limits.representation.max_validation_work = SIZE_MAX;
    SolMirConcreteBuildRequest concrete_request = {&c.ir, &root, 1, imports,
        IMPORT_COUNT, &target, &concrete_limits};
    bool built = sol_mir_concrete_program_build(&concrete_request, &program,
        &c.diagnostics) == SOL_MIR_CONCRETE_BUILD_SUCCEEDED;
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &c.diagnostics);
    CHECK(built);
    SolMirRuntimeConventions owner; sol_mir_runtime_conventions_init(&owner);
    SolMirRuntimeConventionsBuildRequest request = {&program, NULL};
    CHECK(built && sol_mir_runtime_conventions_build(&request, &owner,
        &c.diagnostics) == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
    CHECK(owner.import_count >= IMPORT_COUNT
        && sol_mir_runtime_conventions_validate(&owner, NULL));
    SolMirRuntimeConventionsLimits low = exact_limits(&owner);
    low.max_validation_work = 64;
    SolMirRuntimeConventions limited; sol_mir_runtime_conventions_init(&limited);
    request.limits = &low;
    CHECK(sol_mir_runtime_conventions_build(&request, &limited, &c.diagnostics)
        == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED
        && limited.concrete == NULL);
    sol_mir_runtime_conventions_free(&limited);
    sol_mir_runtime_conventions_free(&owner);
    sol_mir_concrete_program_free(&program); text_compilation_free(&c);
}

int main(void) {
    Compilation compilation; CHECK(compile_e6(&compilation));
    SolMirConcreteProgram program; sol_mir_concrete_program_init(&program);
    CHECK(build_concrete(&compilation, &program));
    if (program.program.ir != NULL) test_e6(&program, &compilation.diagnostics);
    if (failures != 0)
        sol_diagnostics_render_human(stderr, &compilation.package.source,
            &compilation.diagnostics);
    sol_mir_concrete_program_free(&program);
    compilation_free(&compilation);
    test_supported_call_vocabulary();
    test_predicate_calls_and_callback_boundary();
    test_indirect_predicate_and_bound_environment();
    test_unit_entry_exit();
    test_stable_rendering();
    test_never_callable_boundary();
    test_computed_place_call_provenance();
    test_deep_expression_provenance();
    test_rich_predicate_failure_sites();
    test_valid_owner_work_deltas();
    test_import_preflight_stress();
    if (failures != 0) {
        fprintf(stderr, "%d runtime conventions test(s) failed\n", failures);
        return 1;
    }
    return 0;
}
