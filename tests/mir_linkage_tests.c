#include "sol/mir_linkage.h"

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

void sol_mir_linkage_test_force_instance_digest_collision(bool force);
void sol_mir_linkage_test_force_host_digest_collision(bool force);
void sol_mir_linkage_test_force_runtime_digest_collision(bool force);
void sol_mir_linkage_test_force_table_digest_collision(bool force);
size_t sol_mir_linkage_test_last_descriptor_work_start(void);
size_t sol_mir_linkage_test_last_sort_work_start(void);
bool sol_mir_linkage_test_sha256(const void *bytes, size_t length,
    SolMirLinkageDigest *digest);
bool sol_mir_linkage_test_sha256_with_limit(const void *bytes, size_t length,
    size_t limit, SolMirLinkageDigest *digest, size_t *used);

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
    SolMirLinkage linkage;
    SolDiagnostics diagnostics;
} Pipeline;

static bool compile_text_at(Compilation *compilation, const char *path,
    const char *text) {
    memset(compilation, 0, sizeof(*compilation));
    sol_tokens_init(&compilation->tokens);
    sol_diagnostics_init(&compilation->diagnostics);
    sol_syntax_tree_init(&compilation->syntax);
    sol_hir_module_init(&compilation->hir);
    sol_type_table_init(&compilation->types);
    sol_effect_table_init(&compilation->effects);
    sol_contract_table_init(&compilation->contracts);
    sol_ir_init(&compilation->ir);
    return sol_source_from_text(&compilation->source, path, text)
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

static bool compile_text(Compilation *compilation, const char *text) {
    return compile_text_at(compilation, "linkage.sol", text);
}

static void compilation_free(Compilation *compilation) {
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

static bool compile_e6(Compilation *compilation, SolPackage *package) {
    memset(compilation, 0, sizeof(*compilation)); sol_package_init(package);
    sol_diagnostics_init(&compilation->diagnostics);
    sol_hir_module_init(&compilation->hir);
    sol_type_table_init(&compilation->types);
    sol_effect_table_init(&compilation->effects);
    sol_contract_table_init(&compilation->contracts);
    sol_ir_init(&compilation->ir);
    char message[256];
    if (!sol_package_load_directory(package,
            SOL_TEST_SOURCE_DIR "/tests/conformance/e6",
            &compilation->diagnostics, message, sizeof(message))) return false;
    SolHirFileScope *scopes = package->file_count == 0 ? NULL
        : malloc(package->file_count * sizeof(*scopes));
    if (package->file_count != 0 && scopes == NULL) return false;
    for (size_t i = 0; i < package->file_count; ++i)
        scopes[i] = (SolHirFileScope){package->files[i].module_name,
            package->files[i].import_start, package->files[i].import_count,
            package->files[i].item_start, package->files[i].item_count};
    bool ok = sol_hir_lower_scoped(&package->source, &package->syntax, scopes,
            package->file_count, &compilation->hir, &compilation->diagnostics)
        && sol_type_check(&package->source, &package->syntax,
            &compilation->hir, &compilation->types, &compilation->diagnostics)
        && sol_effect_check(&package->source, &package->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->diagnostics)
        && sol_contract_lower(&package->source, &package->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->diagnostics)
        && sol_ir_lower_scoped(&package->source, &package->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, package->files, package->file_count,
            &compilation->ir, &compilation->diagnostics);
    free(scopes); return ok;
}

static void compilation_e6_free(Compilation *compilation, SolPackage *package) {
    sol_ir_free(&compilation->ir);
    sol_contract_table_free(&compilation->contracts);
    sol_effect_table_free(&compilation->effects);
    sol_type_table_free(&compilation->types);
    sol_hir_module_free(&compilation->hir);
    sol_diagnostics_free(&compilation->diagnostics);
    sol_package_free(package);
}

static SolIrCallableId callable(const SolIr *ir, const char *name,
    SolIrCallableKind kind) {
    for (size_t i = 0; i < ir->callable_count; ++i)
        if (ir->callables[i].kind == kind
            && strcmp(ir->callables[i].name, name) == 0) return i;
    return SOL_IR_NONE;
}

static void pipeline_init(Pipeline *pipeline) {
    memset(pipeline, 0, sizeof(*pipeline));
    sol_mir_program_init(&pipeline->program);
    sol_mir_plan_init(&pipeline->plan);
    sol_mir_materialization_init(&pipeline->materialization);
    sol_mir_representation_init(&pipeline->representation);
    sol_mir_layout_init(&pipeline->layout);
    sol_mir_operations_init(&pipeline->operations);
    sol_mir_linkage_init(&pipeline->linkage);
    sol_diagnostics_init(&pipeline->diagnostics);
}

static void pipeline_free(Pipeline *pipeline) {
    sol_mir_linkage_free(&pipeline->linkage);
    sol_mir_operations_free(&pipeline->operations);
    sol_mir_layout_free(&pipeline->layout);
    sol_mir_representation_free(&pipeline->representation);
    sol_mir_materialization_free(&pipeline->materialization);
    sol_mir_plan_free(&pipeline->plan);
    sol_mir_program_free(&pipeline->program);
    sol_diagnostics_free(&pipeline->diagnostics);
}

static bool build_pipeline(const SolIr *ir, const SolMirProgramRoot *roots,
    size_t root_count, const SolIrCallableId *imports, size_t import_count,
    Pipeline *pipeline, const SolMirLinkageLimits *limits) {
    SolMirProgramBuildRequest a = {ir, roots, root_count, imports, import_count, NULL};
    SolMirPlanBuildRequest b = {&pipeline->program, NULL};
    SolMirMaterializeBuildRequest c = {&pipeline->plan, NULL};
    SolMirRepresentationBuildRequest d = {&pipeline->materialization, NULL};
    SolMirTargetDescriptor wasm = sol_mir_target_wasm32();
    SolMirLayoutBuildRequest e = {&pipeline->representation, &wasm, NULL};
    SolMirOperationsBuildRequest f = {&pipeline->layout, NULL};
    SolMirLinkageBuildRequest g = {&pipeline->operations, limits};
    return sol_mir_program_build(&a, &pipeline->program, &pipeline->diagnostics)
            == SOL_MIR_PROGRAM_BUILD_SUCCEEDED
        && sol_mir_plan_build(&b, &pipeline->plan, &pipeline->diagnostics)
            == SOL_MIR_PLAN_BUILD_SUCCEEDED
        && sol_mir_materialize_build(&c, &pipeline->materialization,
            &pipeline->diagnostics) == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED
        && sol_mir_representation_build(&d, &pipeline->representation,
            &pipeline->diagnostics) == SOL_MIR_REPRESENTATION_BUILD_SUCCEEDED
        && sol_mir_layout_build(&e, &pipeline->layout, &pipeline->diagnostics)
            == SOL_MIR_LAYOUT_BUILD_SUCCEEDED
        && sol_mir_operations_build(&f, &pipeline->operations,
            &pipeline->diagnostics) == SOL_MIR_OPERATIONS_BUILD_SUCCEEDED
        && sol_mir_linkage_build(&g, &pipeline->linkage,
            &pipeline->diagnostics) == SOL_MIR_LINKAGE_BUILD_SUCCEEDED;
}

static char *render(const SolMirLinkage *linkage) {
    FILE *stream = tmpfile();
    if (stream == NULL || !sol_mir_linkage_render(stream, linkage)
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

static SolMirLinkageLimits exact_limits(const SolMirLinkage *linkage) {
#define NONZERO(value) ((value) == 0 ? 1 : (value))
    return (SolMirLinkageLimits){
        .max_callables = NONZERO(linkage->usage.callables),
        .max_bindings = NONZERO(linkage->usage.bindings),
        .max_entry_exports = NONZERO(linkage->usage.entry_exports),
        .max_table_entries = NONZERO(linkage->usage.table_entries),
        .max_callable_values = NONZERO(linkage->usage.callable_values),
        .max_host_requirements = NONZERO(linkage->usage.host_requirements),
        .max_runtime_requirements = NONZERO(linkage->usage.runtime_requirements),
        .max_owned_bytes = NONZERO(linkage->usage.owned_bytes),
        .max_build_scratch_bytes = NONZERO(linkage->usage.build_scratch_bytes),
        .max_build_work = NONZERO(linkage->usage.build_work),
        .max_validation_scratch_bytes
            = NONZERO(linkage->usage.validation_scratch_bytes),
        .max_validation_work = NONZERO(linkage->usage.validation_work),
    };
#undef NONZERO
}

static uint32_t independent_runtime_flags(const Pipeline *pipeline,
    SolMirRecipeId recipe_id) {
    const SolMirRecipe *recipe = &pipeline->representation.recipes[recipe_id];
    uint32_t flags = 0;
    if (recipe->inhabited && !recipe->zero_sized
        && recipe->storage != SOL_MIR_STORAGE_NONE
        && recipe->storage != SOL_MIR_STORAGE_SCALAR)
        flags |= SOL_MIR_LINKAGE_RUNTIME_CREATE;
    if (recipe->copy_kind == SOL_MIR_COPY_TEXT
        || recipe->copy_kind == SOL_MIR_COPY_AGGREGATE
        || recipe->copy_kind == SOL_MIR_COPY_WRAPPER)
        flags |= SOL_MIR_LINKAGE_RUNTIME_COPY;
    if (recipe->drop_kind != SOL_MIR_DROP_NONE)
        flags |= SOL_MIR_LINKAGE_RUNTIME_DROP;
    for (size_t i = 0; i < pipeline->operations.equality_node_count; ++i)
        if (pipeline->operations.equality_nodes[i].recipe == recipe_id
            && pipeline->operations.equality_nodes[i].kind
                != SOL_MIR_OPERATION_EQUAL_SCALAR) {
            flags |= SOL_MIR_LINKAGE_RUNTIME_EQUAL; break;
        }
    for (size_t i = 0; i < pipeline->operations.callable_count; ++i)
        if (pipeline->operations.callables[i].kind
                == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION
            && pipeline->operations.callables[i].function_recipe == recipe_id) {
            flags |= SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT; break;
        }
    return flags;
}

static void check_runtime_flags(const Pipeline *pipeline,
    uint32_t *present, uint32_t *absent) {
    const uint32_t all = SOL_MIR_LINKAGE_RUNTIME_CREATE
        | SOL_MIR_LINKAGE_RUNTIME_COPY | SOL_MIR_LINKAGE_RUNTIME_DROP
        | SOL_MIR_LINKAGE_RUNTIME_EQUAL
        | SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT;
    *present = 0; *absent = 0;
    for (size_t recipe = 0; recipe < pipeline->representation.recipe_count;
            ++recipe) {
        uint32_t expected = independent_runtime_flags(pipeline, recipe);
        const SolMirLinkageRuntimeRequirement *requirement = NULL;
        for (size_t i = 0; i < pipeline->linkage.runtime_requirement_count; ++i)
            if (pipeline->linkage.runtime_requirements[i].recipe == recipe) {
                requirement = &pipeline->linkage.runtime_requirements[i]; break;
            }
        CHECK((expected != 0) == (requirement != NULL));
        if (requirement != NULL) CHECK(requirement->operations == expected);
        *present |= expected;
        *absent |= all & ~expected;
    }
}

static void digest_hex(const SolMirLinkageDigest *digest, char text[65]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < SOL_MIR_LINKAGE_DIGEST_BYTES; ++i) {
        text[i * 2] = hex[digest->bytes[i] >> 4];
        text[i * 2 + 1] = hex[digest->bytes[i] & 15u];
    }
    text[64] = '\0';
}

static void test_sha256_known_answer(void) {
    SolMirLinkageDigest digest;
    char text[65];
    CHECK(sol_mir_linkage_test_sha256("abc", 3, &digest));
    digest_hex(&digest, text);
    CHECK(strcmp(text,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        == 0);
}

static void test_sha256_exhaustion_boundaries(void) {
    static const size_t limits[] = {
        35, /* marker */
        46, /* zero padding */
        88, /* encoded length */
        96, /* final transform */
    };
    for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); ++i) {
        SolMirLinkageDigest digest = {{0}};
        size_t used = SIZE_MAX;
        CHECK(!sol_mir_linkage_test_sha256_with_limit("abc", 3, limits[i],
            &digest, &used));
        CHECK(used == limits[i]);
        SolMirLinkageDigest zero = {{0}};
        CHECK(memcmp(&digest, &zero, sizeof(digest)) == 0);
    }
    SolMirLinkageDigest digest;
    size_t used = 0;
    CHECK(sol_mir_linkage_test_sha256_with_limit("abc", 3, 224, &digest,
        &used));
    CHECK(used == 224);
}

static void check_linkage_zero(const SolMirLinkage *linkage) {
    SolMirLinkage zero;
    sol_mir_linkage_init(&zero);
    CHECK(memcmp(linkage, &zero, sizeof(*linkage)) == 0);
}

static void check_no_rendered_bytes(const SolMirLinkage *linkage) {
    FILE *stream = tmpfile();
    CHECK(stream != NULL);
    if (stream == NULL) return;
    CHECK(!sol_mir_linkage_render(stream, linkage));
    CHECK(fflush(stream) == 0 && fseek(stream, 0, SEEK_END) == 0
        && ftell(stream) == 0);
    fclose(stream);
}

static void reject_arena_header_mutations(SolMirLinkage *linkage) {
#define REJECT_HEADER(member, type, singular) do { \
    if (linkage->singular##_count != 0) { \
        size_t saved = linkage->singular##_count; \
        --linkage->singular##_count; \
        CHECK(!sol_mir_linkage_validate(linkage, NULL)); \
        linkage->singular##_count = saved; \
        saved = linkage->singular##_capacity; \
        ++linkage->singular##_capacity; \
        CHECK(!sol_mir_linkage_validate(linkage, NULL)); \
        linkage->singular##_capacity = saved; \
        saved = linkage->usage.member; \
        ++linkage->usage.member; \
        CHECK(!sol_mir_linkage_validate(linkage, NULL)); \
        linkage->usage.member = saved; \
    } \
} while (0);
    SOL_MIR_LINKAGE_ARENAS(REJECT_HEADER)
#undef REJECT_HEADER
    CHECK(sol_mir_linkage_validate(linkage, NULL));
}

static void test_entry_and_host_requirement(void) {
    static const char source[] =
        "module linkage_host\n"
        "capability Host { function send(value: Int64) -> Int64 "
        "effects { host.send<Self> } }\n"
        "@entry public function launch(host: capability Host) -> Int64 "
        "effects { host.send<host> } { return host.send(1) }\n";
    Compilation compilation;
    CHECK(compile_text(&compilation, source));
    SolIrCallableId launch = callable(&compilation.ir, "launch",
        SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId send = callable(&compilation.ir, "send",
        SOL_IR_CALLABLE_CAPABILITY);
    SolMirProgramRoot root = {launch, SOL_MIR_PROGRAM_ROOT_ENTRY};
    Pipeline pipeline; pipeline_init(&pipeline);
    bool built = build_pipeline(&compilation.ir, &root, 1, &send, 1,
        &pipeline, NULL);
    if (!built) sol_diagnostics_render_human(stderr, &compilation.source,
        &pipeline.diagnostics);
    CHECK(built);
    if (built) {
        CHECK(pipeline.linkage.callable_count == 1);
        CHECK(pipeline.linkage.entry_export_count == 1);
        CHECK(pipeline.linkage.host_requirement_count == 1);
        CHECK(pipeline.linkage.binding_count == 3);
        CHECK(strcmp(pipeline.linkage.callables[0].symbol.bytes, "") != 0);
        CHECK(memcmp(pipeline.linkage.callables[0].symbol.bytes, "sol.i1.", 7) == 0);
        CHECK(memcmp(pipeline.linkage.entry_exports[0].symbol.bytes,
            "sol.e1.", 7) == 0);
        CHECK(strlen(pipeline.linkage.callables[0].symbol.bytes)
            == SOL_MIR_LINKAGE_SYMBOL_LENGTH);
        for (size_t i = 0; i < SOL_MIR_LINKAGE_SYMBOL_LENGTH; ++i) {
            char byte = pipeline.linkage.callables[0].symbol.bytes[i];
            CHECK((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')
                || byte == '.');
        }
        CHECK(strcmp(pipeline.linkage.callables[0].symbol.bytes,
            "sol.i1.7be9a9fb11d0c13e3d8e2c02af99f6e5."
            "eff55c97f95ef84a7931b6910114f9f7b5a873567dd360c26e2dfbe239cf6b9b")
            == 0);
        CHECK(sol_mir_linkage_validate(&pipeline.linkage, NULL));
        CHECK(pipeline.operations.import_envelopes[0].host_invoke
            && pipeline.linkage.host_requirements[0].receiver
                == pipeline.operations.import_envelopes[0].receiver
            && pipeline.linkage.host_requirements[0].parameters.offset
                == pipeline.operations.import_envelopes[0].parameters.offset
            && pipeline.linkage.host_requirements[0].parameters.count
                == pipeline.operations.import_envelopes[0].parameters.count
            && pipeline.linkage.host_requirements[0].result
                == pipeline.operations.import_envelopes[0].result
            && pipeline.linkage.host_requirements[0].effects
                == pipeline.operations.import_envelopes[0].effects);
        char *text = render(&pipeline.linkage);
        CHECK(text != NULL && strstr(text, "symbol=sol.i1.") != NULL
            && strstr(text, "symbol=sol.e1.") != NULL);
        free(text);
        SolMirLinkageLimits limits = exact_limits(&pipeline.linkage);
        SolMirLinkage limited; sol_mir_linkage_init(&limited);
        SolMirLinkageBuildRequest request = {&pipeline.operations, &limits};
        CHECK(sol_mir_linkage_build(&request, &limited, &pipeline.diagnostics)
            == SOL_MIR_LINKAGE_BUILD_SUCCEEDED);
        sol_mir_linkage_free(&limited);
        size_t saved_build_work = pipeline.linkage.usage.build_work;
        pipeline.linkage.usage.build_work = saved_build_work + 1;
        CHECK(!sol_mir_linkage_validate(&pipeline.linkage, NULL));
        pipeline.linkage.usage.build_work = saved_build_work - 1;
        CHECK(!sol_mir_linkage_validate(&pipeline.linkage, NULL));
        pipeline.linkage.usage.build_work = 0;
        CHECK(!sol_mir_linkage_validate(&pipeline.linkage, NULL));
        pipeline.linkage.usage.build_work = saved_build_work;
        CHECK(sol_mir_linkage_validate(&pipeline.linkage, NULL));
        --limits.max_owned_bytes;
        CHECK(sol_mir_linkage_build(&request, &limited, &pipeline.diagnostics)
            == SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED
            && limited.operations == NULL);
        check_linkage_zero(&limited);
        SolMirLinkageSymbol saved = pipeline.linkage.callables[0].symbol;
        pipeline.linkage.callables[0].symbol.bytes[7] = 'g';
        CHECK(!sol_mir_linkage_validate(&pipeline.linkage, NULL));
        check_no_rendered_bytes(&pipeline.linkage);
        pipeline.linkage.callables[0].symbol = saved;
        pipeline.linkage.callables[0].symbol.bytes[7]
            = saved.bytes[7] == '0' ? '1' : '0';
        CHECK(!sol_mir_linkage_validate(&pipeline.linkage, NULL));
        pipeline.linkage.callables[0].symbol = saved;
        SolMirLinkageSymbol saved_entry = pipeline.linkage.entry_exports[0].symbol;
        pipeline.linkage.entry_exports[0].symbol.bytes[40]
            = saved_entry.bytes[40] == '0' ? '1' : '0';
        CHECK(!sol_mir_linkage_validate(&pipeline.linkage, NULL));
        pipeline.linkage.entry_exports[0].symbol = saved_entry;
        reject_arena_header_mutations(&pipeline.linkage);
        if (pipeline.linkage.binding_count != 0) {
            SolMirLinkageCallable *saved_pointer = pipeline.linkage.callables;
            pipeline.linkage.callables
                = (SolMirLinkageCallable *)(void *)pipeline.linkage.bindings;
            CHECK(!sol_mir_linkage_validate(&pipeline.linkage, NULL));
            pipeline.linkage.callables = saved_pointer;
            pipeline.linkage.callables
                = (SolMirLinkageCallable *)(void *)compilation.ir.source_path;
            CHECK(!sol_mir_linkage_validate(&pipeline.linkage, NULL));
            pipeline.linkage.callables = saved_pointer;
            if (pipeline.program.template_count != 0
                && pipeline.program.templates[0].mir.block_count != 0) {
                pipeline.linkage.callables = (SolMirLinkageCallable *)(void *)
                    pipeline.program.templates[0].mir.blocks;
                CHECK(!sol_mir_linkage_validate(&pipeline.linkage, NULL));
                pipeline.linkage.callables = saved_pointer;
            }
            if (pipeline.materialization.image_count != 0
                && pipeline.materialization.images[0].topology.block_count != 0) {
                pipeline.linkage.callables = (SolMirLinkageCallable *)(void *)
                    pipeline.materialization.images[0].topology.blocks;
                CHECK(!sol_mir_linkage_validate(&pipeline.linkage, NULL));
                pipeline.linkage.callables = saved_pointer;
            }
        }
        SolMirLinkageLimits partial = limits;
        partial.max_callables = 0;
        request.limits = &partial;
        CHECK(sol_mir_linkage_build(&request, &limited, &pipeline.diagnostics)
            == SOL_MIR_LINKAGE_BUILD_INVALID_ARGUMENT
            && limited.operations == NULL);
        SolMirLinkage occupied; sol_mir_linkage_init(&occupied);
        occupied.operations = &pipeline.operations;
        request.limits = NULL;
        CHECK(sol_mir_linkage_build(&request, &occupied, &pipeline.diagnostics)
            == SOL_MIR_LINKAGE_BUILD_INVALID_ARGUMENT);
        occupied.operations = NULL;
        sol_mir_linkage_free(&occupied);
        limits = exact_limits(&pipeline.linkage);
        --limits.max_build_work;
        request.limits = &limits;
        CHECK(sol_mir_linkage_build(&request, &limited, &pipeline.diagnostics)
            == SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED
            && limited.operations == NULL);
        check_linkage_zero(&limited);
        limits = exact_limits(&pipeline.linkage);
        --limits.max_validation_scratch_bytes;
        CHECK(sol_mir_linkage_build(&request, &limited, &pipeline.diagnostics)
            == SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED
            && limited.operations == NULL);
        limits = exact_limits(&pipeline.linkage);
        --limits.max_validation_work;
        CHECK(sol_mir_linkage_build(&request, &limited, &pipeline.diagnostics)
            == SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED
            && limited.operations == NULL);
        SolIrCallableId approved = pipeline.program.approved_imports[0];
        pipeline.program.approved_imports[0] = launch;
        SolMirLinkage forged; sol_mir_linkage_init(&forged);
        request = (SolMirLinkageBuildRequest){&pipeline.operations, NULL};
        CHECK(sol_mir_linkage_build(&request, &forged, &pipeline.diagnostics)
            == SOL_MIR_LINKAGE_BUILD_INVALID_OPERATIONS
            && forged.operations == NULL
            && pipeline.diagnostics.count != 0
            && strcmp(pipeline.diagnostics.items[
                    pipeline.diagnostics.count - 1].code,
                "SOL-MIR-LINKAGE-001") == 0
            && pipeline.diagnostics.items[pipeline.diagnostics.count - 1]
                .severity == SOL_SEVERITY_ERROR
            && pipeline.diagnostics.items[pipeline.diagnostics.count - 1]
                .span.start == 0
            && pipeline.diagnostics.items[pipeline.diagnostics.count - 1]
                .span.end == 0);
        pipeline.program.approved_imports[0] = approved;
        sol_mir_linkage_free(&forged);
    }
    pipeline_free(&pipeline); compilation_free(&compilation);
}

static void test_callable_table_and_collision(void) {
    static const char source[] =
        "module linkage_callable\n"
        "function identity<T>(value: T) -> T effects { pure } { return value }\n"
        "function callback(value: Int64) -> Bool effects { pure } { return true }\n"
        "function root() -> Bool effects { pure } "
        "requires { { let exact = callback identity(1) == 1 "
        "identity(true) == true } } { return callback(1) }\n";
    Compilation compilation;
    CHECK(compile_text(&compilation, source));
    SolMirProgramRoot root = {callable(&compilation.ir, "root",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline pipeline; pipeline_init(&pipeline);
    bool built = build_pipeline(&compilation.ir, &root, 1, NULL, 0,
        &pipeline, NULL);
    if (!built) sol_diagnostics_render_human(stderr, &compilation.source,
        &pipeline.diagnostics);
    CHECK(built);
    if (built) {
        CHECK(pipeline.linkage.callable_count >= 4);
        CHECK(pipeline.linkage.callable_value_count == 1);
        CHECK(pipeline.linkage.table_entry_count == 1);
        CHECK(pipeline.linkage.callable_values[0].table == 0);
        CHECK(sol_mir_linkage_validate(&pipeline.linkage, NULL));
        size_t identities = 0;
        SolIrCallableId identity = callable(&compilation.ir, "identity",
            SOL_IR_CALLABLE_FUNCTION);
        SolMirLinkageDigest first = {{0}};
        for (size_t i = 0; i < pipeline.linkage.callable_count; ++i) {
            SolMirPlanInstanceId instance = pipeline.linkage.callables[i].instance;
            if (pipeline.plan.instances[instance].callable != identity) continue;
            if (identities == 0) first = pipeline.linkage.callables[i].instance_key;
            else CHECK(memcmp(first.bytes,
                pipeline.linkage.callables[i].instance_key.bytes,
                SOL_MIR_LINKAGE_DIGEST_BYTES) != 0);
            ++identities;
        }
        CHECK(identities == 2);
        SolMirLinkageLimits limits = exact_limits(&pipeline.linkage);
        SolMirLinkage limited; sol_mir_linkage_init(&limited);
        SolMirLinkageBuildRequest request = {&pipeline.operations, &limits};
        --limits.max_table_entries;
        CHECK(sol_mir_linkage_build(&request, &limited, &pipeline.diagnostics)
            == SOL_MIR_LINKAGE_BUILD_INVALID_ARGUMENT
            && limited.operations == NULL);
    }
    pipeline_free(&pipeline);

    pipeline_init(&pipeline);
    sol_mir_linkage_test_force_instance_digest_collision(true);
    built = build_pipeline(&compilation.ir, &root, 1, NULL, 0, &pipeline, NULL);
    sol_mir_linkage_test_force_instance_digest_collision(false);
    CHECK(!built && pipeline.linkage.operations == NULL);
    pipeline_free(&pipeline); compilation_free(&compilation);
}

static void test_callable_target_deduplication(void) {
    static const char source[] =
        "module linkage_targets\n"
        "capability Base { function choose(value: Int64) -> Bool effects { pure } }\n"
        "function callback(value: Int64) -> Bool effects { pure } { return true }\n"
        "function root(base: capability Base) -> Bool effects { pure } "
        "requires { { let exact = callback let first = base.choose "
        "let second = base.choose true } } "
        "{ return callback(1) && base.choose(1) }\n";
    Compilation compilation; CHECK(compile_text(&compilation, source));
    SolIrCallableId root_id = callable(&compilation.ir, "root",
        SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId callback_id = callable(&compilation.ir, "callback",
        SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId choose = callable(&compilation.ir, "choose",
        SOL_IR_CALLABLE_CAPABILITY);
    SolMirProgramRoot root = {root_id, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline pipeline; pipeline_init(&pipeline);
    bool built = build_pipeline(&compilation.ir, &root, 1, &choose, 1,
        &pipeline, NULL);
    CHECK(built);
    if (built) {
        CHECK(pipeline.linkage.callable_value_count == 4
            && pipeline.linkage.table_entry_count == 2
            && pipeline.linkage.host_requirement_count == 1);
        size_t host_values = 0, internal_values = 0;
        SolMirLinkageTableId host_table = SOL_MIR_LINKAGE_NONE;
        SolMirLinkageCallableId callback_link = SOL_MIR_LINKAGE_NONE;
        for (size_t i = 0; i < pipeline.linkage.binding_count; ++i) {
            const SolMirMaterializedBinding *source_binding
                = &pipeline.materialization.bindings[i];
            if (source_binding->symbolic_callable == callback_id
                && pipeline.linkage.bindings[i].target_kind
                    == SOL_MIR_LINKAGE_TARGET_INTERNAL)
                callback_link = pipeline.linkage.bindings[i].internal;
            if (source_binding->symbolic_callable == choose)
                CHECK(pipeline.linkage.bindings[i].target_kind
                    == SOL_MIR_LINKAGE_TARGET_HOST);
        }
        for (size_t i = 0; i < pipeline.linkage.callable_value_count; ++i) {
            SolMirLinkageTableId table = pipeline.linkage.callable_values[i].table;
            const SolMirLinkageTableEntry *entry
                = &pipeline.linkage.table_entries[table];
            if (entry->target_kind == SOL_MIR_LINKAGE_TARGET_HOST) {
                ++host_values;
                if (host_table == SOL_MIR_LINKAGE_NONE) host_table = table;
                else CHECK(host_table == table);
            } else {
                ++internal_values;
                CHECK(entry->internal == callback_link);
            }
        }
        CHECK(host_values == 3 && internal_values == 1
            && callback_link != SOL_MIR_LINKAGE_NONE
            && sol_mir_linkage_validate(&pipeline.linkage, NULL));
        uint32_t runtime_present, runtime_absent;
        check_runtime_flags(&pipeline, &runtime_present, &runtime_absent);
        CHECK((runtime_present & SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT) != 0
            && (runtime_absent & SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT) != 0);
        SolMirLinkageLimits limits = exact_limits(&pipeline.linkage);
        SolMirLinkage limited; sol_mir_linkage_init(&limited);
        SolMirLinkageBuildRequest request = {&pipeline.operations, &limits};
        --limits.max_table_entries;
        CHECK(sol_mir_linkage_build(&request, &limited, &pipeline.diagnostics)
            == SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED
            && limited.operations == NULL);
        limits = exact_limits(&pipeline.linkage);
        --limits.max_callable_values;
        CHECK(sol_mir_linkage_build(&request, &limited, &pipeline.diagnostics)
            == SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED
            && limited.operations == NULL);
        request.limits = NULL;
        sol_mir_linkage_test_force_table_digest_collision(true);
        CHECK(sol_mir_linkage_build(&request, &limited, &pipeline.diagnostics)
            == SOL_MIR_LINKAGE_BUILD_SYMBOL_COLLISION);
        sol_mir_linkage_test_force_table_digest_collision(false);
        sol_mir_linkage_free(&limited);
    }
    pipeline_free(&pipeline); compilation_free(&compilation);
}

static void test_recursion_trait_and_import_helper(void) {
    static const char source[] =
        "module linkage_closure\n"
        "trait Score { function score(self: Self) -> Int64 effects { pure } }\n"
        "implementation Score for Int64 { function score(self: Self) -> Int64 "
        "effects { pure } { return self } }\n"
        "function positive(value: Int64) -> Bool effects { pure } { return value > 0 }\n"
        "capability Host { function echo(value: Int64) -> Int64 effects { pure } "
        "requires { positive(value) } }\n"
        "function left(value: Int64) -> Int64 effects { pure } "
        "{ return right(value) }\n"
        "function right(value: Int64) -> Int64 effects { pure } "
        "{ return left(value) }\n"
        "function score<T: Score>(value: T) -> Int64 effects { pure } "
        "{ return value.score() }\n"
        "function root(host: capability Host) -> Int64 effects { pure } "
        "{ return host.echo(left(2) + score(1)) }\n";
    Compilation compilation; CHECK(compile_text(&compilation, source));
    SolIrCallableId echo = callable(&compilation.ir, "echo",
        SOL_IR_CALLABLE_CAPABILITY);
    SolMirProgramRoot root = {callable(&compilation.ir, "root",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline pipeline; pipeline_init(&pipeline);
    bool built = build_pipeline(&compilation.ir, &root, 1, &echo, 1,
        &pipeline, NULL);
    CHECK(built);
    if (built) {
        size_t helper_images = 0, trait_images = 0;
        SolIrCallableId positive = callable(&compilation.ir, "positive",
            SOL_IR_CALLABLE_FUNCTION);
        for (size_t i = 0; i < pipeline.linkage.callable_count; ++i) {
            SolIrCallableId source_callable = pipeline.plan.instances[
                pipeline.linkage.callables[i].instance].callable;
            helper_images += source_callable == positive;
            trait_images += compilation.ir.callables[source_callable].kind
                == SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION;
        }
        CHECK(helper_images == 1 && trait_images == 1
            && pipeline.linkage.host_requirement_count == 1
            && sol_mir_linkage_validate(&pipeline.linkage, NULL));
        for (size_t i = 0; i < pipeline.linkage.binding_count; ++i)
            CHECK(pipeline.linkage.bindings[i].target_kind
                    == SOL_MIR_LINKAGE_TARGET_INTERNAL
                || pipeline.linkage.bindings[i].target_kind
                    == SOL_MIR_LINKAGE_TARGET_HOST);
    }
    pipeline_free(&pipeline); compilation_free(&compilation);
}

static void test_recursive_nominal_framing(void) {
    static const char source[] =
        "module linkage_recursive_nominal\n"
        "record Left { next: Option<Left> }\n"
        "record Right { next: Option<Right> }\n"
        "function keep<T>(value: T) -> T effects { pure } { return value }\n"
        "function keep_left(value: Left) -> Left effects { pure } "
        "{ return keep(value) }\n"
        "function keep_right(value: Right) -> Right effects { pure } "
        "{ return keep(value) }\n";
    Compilation compilation; CHECK(compile_text(&compilation, source));
    SolIrCallableId left = callable(&compilation.ir, "keep_left",
        SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId right = callable(&compilation.ir, "keep_right",
        SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId keep = callable(&compilation.ir, "keep",
        SOL_IR_CALLABLE_FUNCTION);
    SolMirProgramRoot roots[2] = {
        {left, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},
        {right, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},
    };
    Pipeline pipeline; pipeline_init(&pipeline);
    CHECK(build_pipeline(&compilation.ir, roots, 2, NULL, 0, &pipeline, NULL));
    const SolMirLinkageCallable *specializations[2] = {NULL, NULL};
    size_t specialization_count = 0;
    for (size_t i = 0; i < pipeline.linkage.callable_count; ++i) {
        const SolMirPlanInstance *instance = &pipeline.plan.instances[
            pipeline.linkage.callables[i].instance];
        SolIrCallableId source_callable = instance->callable;
        if (source_callable != keep) continue;
        if (specialization_count < 2)
            specializations[specialization_count] = &pipeline.linkage.callables[i];
        ++specialization_count;
        CHECK(instance->type_arguments.count == 1);
        if (instance->type_arguments.count == 1) {
            SolMirPlanTypeId type = pipeline.plan.instance_type_ids[
                instance->type_arguments.offset];
            CHECK(type < pipeline.plan.type_count
                && pipeline.plan.types[type].kind == SOL_IR_TYPE_NOMINAL);
        }
    }
    CHECK(specialization_count == 2 && specializations[0] != NULL
        && specializations[1] != NULL);
    if (specializations[0] != NULL && specializations[1] != NULL) {
        const SolMirPlanInstance *first = &pipeline.plan.instances[
            specializations[0]->instance];
        const SolMirPlanInstance *second = &pipeline.plan.instances[
            specializations[1]->instance];
        SolMirPlanTypeId first_type = pipeline.plan.instance_type_ids[
            first->type_arguments.offset];
        SolMirPlanTypeId second_type = pipeline.plan.instance_type_ids[
            second->type_arguments.offset];
        CHECK(first->callable == second->callable
            && first->callable == keep
            && first->receiver == second->receiver
            && first->dictionary.count == 0 && second->dictionary.count == 0
            && first->effect_tail == second->effect_tail
            && first->effects == second->effects
            && pipeline.plan.types[first_type].definition
                != pipeline.plan.types[second_type].definition
            && memcmp(specializations[0]->instance_key.bytes,
                specializations[1]->instance_key.bytes,
                SOL_MIR_LINKAGE_DIGEST_BYTES) != 0
            && strcmp(specializations[0]->symbol.bytes,
                specializations[1]->symbol.bytes) != 0);
    }
    CHECK(sol_mir_linkage_validate(&pipeline.linkage, NULL));
    SolMirLinkageLimits limits = exact_limits(&pipeline.linkage);
    SolMirLinkage limited; sol_mir_linkage_init(&limited);
    SolMirLinkageBuildRequest request = {&pipeline.operations, &limits};
    CHECK(sol_mir_linkage_build(&request, &limited, &pipeline.diagnostics)
        == SOL_MIR_LINKAGE_BUILD_SUCCEEDED);
    sol_mir_linkage_free(&limited);
    size_t descriptor_work_start
        = sol_mir_linkage_test_last_descriptor_work_start();
    size_t sort_work_start = sol_mir_linkage_test_last_sort_work_start();
    CHECK(descriptor_work_start != SIZE_MAX && descriptor_work_start != 0
        && sort_work_start != SIZE_MAX
        && sort_work_start > descriptor_work_start);
    limits = exact_limits(&pipeline.linkage);
    limits.max_build_work = descriptor_work_start;
    CHECK(sol_mir_linkage_build(&request, &limited, &pipeline.diagnostics)
        == SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED
        && limited.operations == NULL);
    check_linkage_zero(&limited);
    limits = exact_limits(&pipeline.linkage);
    limits.max_build_work = sort_work_start;
    CHECK(sol_mir_linkage_build(&request, &limited, &pipeline.diagnostics)
        == SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED
        && limited.operations == NULL);
    check_linkage_zero(&limited);
    limits = exact_limits(&pipeline.linkage);
    --limits.max_build_work;
    CHECK(sol_mir_linkage_build(&request, &limited, &pipeline.diagnostics)
        == SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED
        && limited.operations == NULL);
    check_linkage_zero(&limited);
    pipeline_free(&pipeline); compilation_free(&compilation);
}

static void test_dictionary_dense_id_independence(void) {
    static const char source[] =
        "module linkage_dictionary\n"
        "trait Identify { function identify(self: Self) -> Int64 effects { pure } }\n"
        "implementation Identify for Int64 { function identify(self: Self) -> Int64 "
        "effects { pure } { return 11 } }\n"
        "implementation Identify for Bool { function identify(self: Self) -> Int64 "
        "effects { pure } { return 22 } }\n"
        "function pair<T: Identify, U: Identify>(left: T, right: U) -> Int64 "
        "effects { pure } { return left.identify() + right.identify() }\n"
        "function root() -> Int64 effects { pure } { return pair(1, true) }\n";
    static const char inserted_source[] =
        "module linkage_dictionary\n"
        "record Unused { value: Text }\n"
        "function unused(value: Bool) -> Bool effects { pure } { return value }\n"
        "trait Identify { function identify(self: Self) -> Int64 effects { pure } }\n"
        "implementation Identify for Int64 { function identify(self: Self) -> Int64 "
        "effects { pure } { return 11 } }\n"
        "implementation Identify for Bool { function identify(self: Self) -> Int64 "
        "effects { pure } { return 22 } }\n"
        "function pair<T: Identify, U: Identify>(left: T, right: U) -> Int64 "
        "effects { pure } { return left.identify() + right.identify() }\n"
        "function root() -> Int64 effects { pure } { return pair(1, true) }\n";
    Compilation compilations[2]; Pipeline pipelines[2]; char *texts[2] = {0};
    const char *sources[2] = {source, inserted_source};
    for (size_t i = 0; i < 2; ++i) {
        CHECK(compile_text_at(&compilations[i], i == 0 ? "/a/linkage.sol"
            : "/b/linkage.sol", sources[i]));
        SolMirProgramRoot root = {callable(&compilations[i].ir, "root",
            SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
        pipeline_init(&pipelines[i]);
        CHECK(build_pipeline(&compilations[i].ir, &root, 1, NULL, 0,
            &pipelines[i], NULL));
        texts[i] = render(&pipelines[i].linkage);
    }
    CHECK(texts[0] != NULL && texts[1] != NULL
        && strcmp(texts[0], texts[1]) == 0);
    for (size_t i = 0; i < 2; ++i) {
        free(texts[i]); pipeline_free(&pipelines[i]);
        compilation_free(&compilations[i]);
    }
}

static void test_deterministic_rendering(void) {
    static const char source[] =
        "module linkage_determinism\n"
        "capability Alpha { function one(value: Int64) -> Int64 effects { pure } }\n"
        "capability Beta { function two(value: Int64) -> Int64 effects { pure } }\n"
        "function first(alpha: capability Alpha, beta: capability Beta) -> Int64 "
        "effects { pure } { return alpha.one(1) + beta.two(2) }\n"
        "function second() -> Int64 effects { pure } { return 2 }\n";
    Compilation compilation;
    CHECK(compile_text_at(&compilation, "/original/tree/linkage.sol", source));
    SolMirProgramRoot roots[2] = {
        {callable(&compilation.ir, "first", SOL_IR_CALLABLE_FUNCTION),
            SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},
        {callable(&compilation.ir, "second", SOL_IR_CALLABLE_FUNCTION),
            SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},
    };
    SolIrCallableId imports[2] = {
        callable(&compilation.ir, "one", SOL_IR_CALLABLE_CAPABILITY),
        callable(&compilation.ir, "two", SOL_IR_CALLABLE_CAPABILITY),
    };
    SolMirProgramRoot reversed_roots[2] = {roots[1], roots[0]};
    SolIrCallableId reversed_imports[2] = {imports[1], imports[0]};
    Pipeline first, second, repeat, path_mutated;
    pipeline_init(&first); pipeline_init(&second);
    pipeline_init(&repeat); pipeline_init(&path_mutated);
    CHECK(build_pipeline(&compilation.ir, roots, 2, imports, 2, &first, NULL));
    CHECK(build_pipeline(&compilation.ir, reversed_roots, 2, reversed_imports, 2,
        &second, NULL));
    CHECK(build_pipeline(&compilation.ir, roots, 2, imports, 2, &repeat, NULL));
    char *first_text = render(&first.linkage);
    char *second_text = render(&second.linkage);
    char *repeat_text = render(&repeat.linkage);
    CHECK(first_text != NULL && second_text != NULL && repeat_text != NULL
        && strcmp(first_text, second_text) == 0
        && strcmp(first_text, repeat_text) == 0);
    char *saved_path = compilation.ir.source_path;
    compilation.ir.source_path = (char *)"/mutatxxx/tree/linkage.sol";
    CHECK(build_pipeline(&compilation.ir, roots, 2, imports, 2,
        &path_mutated, NULL));
    char *path_text = render(&path_mutated.linkage);
    CHECK(first_text != NULL && path_text != NULL
        && strcmp(first_text, path_text) == 0);
    compilation.ir.source_path = saved_path;
    SolMirLinkage collided; sol_mir_linkage_init(&collided);
    SolMirLinkageBuildRequest collision_request = {&first.operations, NULL};
    sol_mir_linkage_test_force_host_digest_collision(true);
    CHECK(sol_mir_linkage_build(&collision_request, &collided,
            &first.diagnostics) == SOL_MIR_LINKAGE_BUILD_SYMBOL_COLLISION);
    sol_mir_linkage_test_force_host_digest_collision(false);
    sol_mir_linkage_free(&collided);

    static const char inserted_source[] =
        "module linkage_determinism\n"
        "record Unused { value: Bool }\n"
        "function unused(value: Bool) -> Bool effects { pure } { return value }\n"
        "capability Alpha { function one(value: Int64) -> Int64 effects { pure } }\n"
        "capability Beta { function two(value: Int64) -> Int64 effects { pure } }\n"
        "function first(alpha: capability Alpha, beta: capability Beta) -> Int64 "
        "effects { pure } { return alpha.one(1) + beta.two(2) }\n"
        "function second() -> Int64 effects { pure } { return 2 }\n";
    Compilation inserted; Pipeline inserted_pipeline;
    CHECK(compile_text_at(&inserted, "/inserted/linkage.sol", inserted_source));
    SolMirProgramRoot inserted_roots[2] = {
        {callable(&inserted.ir, "first", SOL_IR_CALLABLE_FUNCTION),
            SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},
        {callable(&inserted.ir, "second", SOL_IR_CALLABLE_FUNCTION),
            SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},
    };
    SolIrCallableId inserted_imports[2] = {
        callable(&inserted.ir, "one", SOL_IR_CALLABLE_CAPABILITY),
        callable(&inserted.ir, "two", SOL_IR_CALLABLE_CAPABILITY),
    };
    pipeline_init(&inserted_pipeline);
    CHECK(build_pipeline(&inserted.ir, inserted_roots, 2, inserted_imports, 2,
        &inserted_pipeline, NULL));
    char *inserted_text = render(&inserted_pipeline.linkage);
    CHECK(first_text != NULL && inserted_text != NULL
        && strcmp(first_text, inserted_text) == 0);
    free(inserted_text);
    pipeline_free(&inserted_pipeline); compilation_free(&inserted);
    free(first_text); free(second_text); free(repeat_text); free(path_text);
    pipeline_free(&path_mutated); pipeline_free(&repeat);
    pipeline_free(&second); pipeline_free(&first);
    compilation_free(&compilation);

    Compilation relocated[2]; Pipeline relocated_pipeline[2];
    char *relocated_text[2] = {NULL, NULL};
    const char *paths[2] = {"/checkout/a/linkage.sol", "/relocate/b/linkage.sol"};
    for (size_t i = 0; i < 2; ++i) {
        CHECK(compile_text_at(&relocated[i], paths[i], source));
        SolMirProgramRoot relocated_roots[2] = {
            {callable(&relocated[i].ir, "first", SOL_IR_CALLABLE_FUNCTION),
                SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},
            {callable(&relocated[i].ir, "second", SOL_IR_CALLABLE_FUNCTION),
                SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},
        };
        SolIrCallableId relocated_imports[2] = {
            callable(&relocated[i].ir, "one", SOL_IR_CALLABLE_CAPABILITY),
            callable(&relocated[i].ir, "two", SOL_IR_CALLABLE_CAPABILITY),
        };
        pipeline_init(&relocated_pipeline[i]);
        CHECK(build_pipeline(&relocated[i].ir, relocated_roots, 2,
            relocated_imports, 2, &relocated_pipeline[i], NULL));
        relocated_text[i] = render(&relocated_pipeline[i].linkage);
    }
    CHECK(relocated_text[0] != NULL && relocated_text[1] != NULL
        && strcmp(relocated_text[0], relocated_text[1]) == 0);
    for (size_t i = 0; i < 2; ++i) {
        free(relocated_text[i]);
        pipeline_free(&relocated_pipeline[i]);
        compilation_free(&relocated[i]);
    }
}

static void test_limits_and_destination_transactionality(void) {
    SolMirLinkage empty;
    memset(&empty, 0xa5, sizeof(empty)); sol_mir_linkage_init(&empty);
    CHECK(empty.operations == NULL && !sol_mir_linkage_validate(&empty, NULL));
    SolMirLinkageLimits defaults = sol_mir_linkage_default_limits();
    CHECK(defaults.max_callables != 0 && defaults.max_validation_work != 0);
    SolMirLinkageBuildRequest invalid = {NULL, NULL};
    CHECK(sol_mir_linkage_build(&invalid, &empty, NULL)
        == SOL_MIR_LINKAGE_BUILD_INVALID_ARGUMENT);
    sol_mir_linkage_free(&empty);
}

static void test_e6_all_roots_census(void) {
    Compilation compilation; SolPackage package;
    CHECK(compile_e6(&compilation, &package));
    const char *import_names[] = {"write", "get", "count", "read"};
    SolIrCallableId imports[4];
    for (size_t i = 0; i < 4; ++i) {
        imports[i] = callable(&compilation.ir, import_names[i],
            SOL_IR_CALLABLE_CAPABILITY);
        CHECK(imports[i] != SOL_IR_NONE);
    }
    SolMirProgramRoot roots[5];
    roots[0] = (SolMirProgramRoot){callable(&compilation.ir, "launch",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_ENTRY};
    size_t root_count = 1;
    for (size_t i = 0; i < compilation.ir.callable_count; ++i)
        if (compilation.ir.callables[i].kind == SOL_IR_CALLABLE_TEST)
            roots[root_count++] = (SolMirProgramRoot){i,
                SOL_MIR_PROGRAM_ROOT_TEST};
    CHECK(root_count == 5);
    Pipeline pipeline; pipeline_init(&pipeline);
    bool built = build_pipeline(&compilation.ir, roots, root_count, imports, 4,
        &pipeline, NULL);
    if (!built) sol_diagnostics_render_human(stderr, &package.source,
        &pipeline.diagnostics);
    CHECK(built);
    if (built) {
        CHECK(pipeline.linkage.callable_count == 14);
        CHECK(pipeline.linkage.binding_count == 28);
        CHECK(pipeline.linkage.entry_export_count == 1);
        CHECK(pipeline.linkage.host_requirement_count == 4);
        CHECK(pipeline.linkage.callable_value_count == 5);
        CHECK(pipeline.linkage.table_entry_count == 4);
        CHECK(pipeline.linkage.runtime_requirement_count == 17);
        CHECK(sol_mir_linkage_validate(&pipeline.linkage, NULL));
        uint32_t runtime_present, runtime_absent;
        check_runtime_flags(&pipeline, &runtime_present, &runtime_absent);
        CHECK((runtime_present & SOL_MIR_LINKAGE_RUNTIME_CREATE) != 0);
        CHECK((runtime_present & SOL_MIR_LINKAGE_RUNTIME_COPY) != 0);
        CHECK((runtime_present & SOL_MIR_LINKAGE_RUNTIME_DROP) != 0);
        CHECK((runtime_present & SOL_MIR_LINKAGE_RUNTIME_EQUAL) != 0);
        CHECK((runtime_absent & SOL_MIR_LINKAGE_RUNTIME_CREATE) != 0);
        CHECK((runtime_absent & SOL_MIR_LINKAGE_RUNTIME_COPY) != 0);
        CHECK((runtime_absent & SOL_MIR_LINKAGE_RUNTIME_DROP) != 0);
        CHECK((runtime_absent & SOL_MIR_LINKAGE_RUNTIME_EQUAL) != 0);
        CHECK((runtime_absent
            & SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT) != 0);
        uint32_t saved_operations
            = pipeline.linkage.runtime_requirements[0].operations;
        pipeline.linkage.runtime_requirements[0].operations
            ^= SOL_MIR_LINKAGE_RUNTIME_COPY;
        CHECK(!sol_mir_linkage_validate(&pipeline.linkage, NULL));
        pipeline.linkage.runtime_requirements[0].operations = saved_operations;
        for (size_t i = 0; i < pipeline.linkage.binding_count; ++i) {
            const SolMirLinkageBinding *binding = &pipeline.linkage.bindings[i];
            CHECK(binding->binding == i);
            if (binding->target_kind == SOL_MIR_LINKAGE_TARGET_INTERNAL)
                CHECK(binding->internal < pipeline.linkage.callable_count
                    && binding->host == SOL_MIR_LINKAGE_NONE);
            else CHECK(binding->target_kind == SOL_MIR_LINKAGE_TARGET_HOST
                    && binding->host < pipeline.linkage.host_requirement_count
                    && binding->internal == SOL_MIR_LINKAGE_NONE);
        }
        for (size_t i = 0; i < pipeline.materialization.image_count; ++i) {
            size_t matches = 0;
            for (size_t q = 0; q < pipeline.linkage.callable_count; ++q)
                matches += pipeline.linkage.callables[q].instance
                    == pipeline.materialization.images[i].instance;
            CHECK(matches == 1);
        }
        for (size_t i = 0; i < pipeline.linkage.host_requirement_count; ++i) {
            size_t references = 0;
            for (size_t q = 0; q < pipeline.linkage.binding_count; ++q)
                references += pipeline.linkage.bindings[q].target_kind
                        == SOL_MIR_LINKAGE_TARGET_HOST
                    && pipeline.linkage.bindings[q].host == i;
            CHECK(references != 0);
        }
        SolMirLinkage collided; sol_mir_linkage_init(&collided);
        SolMirLinkageBuildRequest request = {&pipeline.operations, NULL};
        sol_mir_linkage_test_force_runtime_digest_collision(true);
        CHECK(sol_mir_linkage_build(&request, &collided, &pipeline.diagnostics)
            == SOL_MIR_LINKAGE_BUILD_SYMBOL_COLLISION);
        sol_mir_linkage_test_force_runtime_digest_collision(false);
        sol_mir_linkage_free(&collided);
    }
    pipeline_free(&pipeline); compilation_e6_free(&compilation, &package);
}

int main(void) {
    test_sha256_known_answer();
    test_sha256_exhaustion_boundaries();
    test_limits_and_destination_transactionality();
    test_entry_and_host_requirement();
    test_callable_table_and_collision();
    test_callable_target_deduplication();
    test_recursion_trait_and_import_helper();
    test_recursive_nominal_framing();
    test_dictionary_dense_id_independence();
    test_deterministic_rendering();
    test_e6_all_roots_census();
    if (failures != 0) {
        fprintf(stderr, "%d MIR linkage test(s) failed\n", failures); return 1;
    }
    printf("MIR linkage tests passed\n"); return 0;
}
