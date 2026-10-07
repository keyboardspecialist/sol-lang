#define SOL_MIR_PLAN_TEST_HOOKS 1
#include "sol/mir_runtime_values.h"

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

void sol_mir_runtime_values_test_force_allocation_failure(bool force);
void sol_mir_runtime_values_test_force_persistent_allocation_failure(
    size_t attempt);
void sol_mir_runtime_values_test_force_validation_allocation_failure(bool force);
void sol_mir_runtime_values_test_force_validation_allocation_failure_attempt(
    size_t attempt);
size_t sol_mir_runtime_values_test_validation_allocation_attempts(void);
size_t sol_mir_runtime_values_test_ownership_count_scans(void);
void sol_mir_runtime_values_test_reverse_captured_fragments(bool reverse);
void sol_mir_runtime_values_test_force_captured_digest_collision(bool force);
bool sol_mir_runtime_values_test_reconstruct_usage(
    const SolMirRuntimeConventions *conventions,
    const SolMirRuntimeValuesLimits *limits,
    SolMirRuntimeValuesUsage *usage);
void sol_mir_concrete_test_force_validation_allocation_failure(bool force);
size_t sol_mir_concrete_test_validation_allocation_attempts(void);

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
    sol_package_init(&c->package);
    sol_diagnostics_init(&c->diagnostics);
    sol_hir_module_init(&c->hir);
    sol_type_table_init(&c->types);
    sol_effect_table_init(&c->effects);
    sol_contract_table_init(&c->contracts);
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
    free(scopes);
    return ok;
}

static void compilation_free(Compilation *c) {
    sol_ir_free(&c->ir);
    sol_contract_table_free(&c->contracts);
    sol_effect_table_free(&c->effects);
    sol_type_table_free(&c->types);
    sol_hir_module_free(&c->hir);
    sol_diagnostics_free(&c->diagnostics);
    sol_package_free(&c->package);
}

static bool build_concrete(Compilation *c, bool reverse_tests,
    SolMirConcreteProgram *program) {
    SolMirProgramRoot roots[5];
    roots[0] = (SolMirProgramRoot){callable(&c->ir, "launch",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_ENTRY};
    size_t count = 1;
    for (size_t i = 0; i < c->ir.callable_count; ++i)
        if (c->ir.callables[i].kind == SOL_IR_CALLABLE_TEST)
            roots[count++] = (SolMirProgramRoot){i, SOL_MIR_PROGRAM_ROOT_TEST};
    if (reverse_tests)
        for (size_t left = 1, right = count - 1; left < right; ++left, --right) {
            SolMirProgramRoot temporary = roots[left];
            roots[left] = roots[right];
            roots[right] = temporary;
        }
    static const char *const names[] = {"write", "get", "count", "read"};
    SolIrCallableId imports[4];
    for (size_t i = 0; i < 4; ++i)
        imports[i] = callable(&c->ir, names[i], SOL_IR_CALLABLE_CAPABILITY);
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    SolMirConcreteBuildRequest request = {&c->ir, roots, count, imports, 4,
        &target, NULL};
    return count == 5 && sol_mir_concrete_program_build(&request, program,
        &c->diagnostics) == SOL_MIR_CONCRETE_BUILD_SUCCEEDED;
}

static bool build_values(const SolMirConcreteProgram *program,
    SolDiagnostics *diagnostics, SolMirRuntimeConventions *conventions,
    SolMirRuntimeValues *values) {
    sol_mir_runtime_conventions_init(conventions);
    sol_mir_runtime_values_init(values);
    SolMirRuntimeConventionsBuildRequest conventions_request = {program, NULL};
    SolMirRuntimeValuesBuildRequest values_request = {conventions, NULL};
    SolMirRuntimeConventionsBuildOutcome conventions_outcome
        = sol_mir_runtime_conventions_build(&conventions_request, conventions,
            diagnostics);
    SolMirRuntimeValuesBuildOutcome values_outcome
        = conventions_outcome == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        ? sol_mir_runtime_values_build(&values_request, values, diagnostics)
        : SOL_MIR_RUNTIME_VALUES_BUILD_INTERNAL_FAILED;
    if (conventions_outcome != SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        || values_outcome != SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED) {
        fprintf(stderr, "runtime build outcomes: conventions=%d values=%d\n",
            (int)conventions_outcome, (int)values_outcome);
        if (diagnostics->count != 0)
            fprintf(stderr, "runtime build diagnostic: %s\n",
                diagnostics->items[diagnostics->count - 1].message);
    }
    return conventions_outcome == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        && values_outcome == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED;
}

static char *render_values(const SolMirRuntimeValues *values) {
    FILE *stream = tmpfile();
    if (stream == NULL || !sol_mir_runtime_values_render(stream, values)
        || fflush(stream) != 0 || fseek(stream, 0, SEEK_END) != 0) {
        if (stream != NULL) fclose(stream);
        return NULL;
    }
    long end = ftell(stream);
    if (end < 0 || fseek(stream, 0, SEEK_SET) != 0) {
        fclose(stream);
        return NULL;
    }
    char *result = malloc((size_t)end + 1);
    if (result == NULL
        || fread(result, 1, (size_t)end, stream) != (size_t)end) {
        free(result);
        fclose(stream);
        return NULL;
    }
    result[end] = '\0';
    fclose(stream);
    return result;
}

static char *render_conventions(const SolMirRuntimeConventions *conventions) {
    FILE *stream = tmpfile();
    if (stream == NULL
        || !sol_mir_runtime_conventions_render(stream, conventions)
        || fflush(stream) != 0 || fseek(stream, 0, SEEK_END) != 0) {
        if (stream != NULL) fclose(stream);
        return NULL;
    }
    long end = ftell(stream);
    if (end < 0 || fseek(stream, 0, SEEK_SET) != 0) {
        fclose(stream);
        return NULL;
    }
    char *result = malloc((size_t)end + 1);
    if (result == NULL
        || fread(result, 1, (size_t)end, stream) != (size_t)end) {
        free(result);
        fclose(stream);
        return NULL;
    }
    result[end] = '\0';
    fclose(stream);
    return result;
}

static SolMirRuntimeValuesLimits exact_limits(const SolMirRuntimeValues *values) {
    return (SolMirRuntimeValuesLimits){
        .max_records = values->usage.records,
        .max_allocation_plans = values->usage.allocation_plans,
        .max_copy_plans = values->usage.copy_plans,
        .max_equality_plans = values->usage.equality_plans,
        .max_host_result_plans = values->usage.host_result_plans,
        .max_host_result_requirements = values->usage.host_result_requirements,
        .max_ownership_plans = values->usage.ownership_plans,
        .max_ownership_variants = values->usage.ownership_variants,
        .max_owned_edges = values->usage.owned_edges,
        .max_owned_bytes = values->usage.owned_bytes,
        .max_build_scratch_bytes = values->usage.build_scratch_bytes,
        .max_build_work = values->usage.build_work,
        .max_validation_scratch_bytes = values->usage.validation_scratch_bytes,
        .max_validation_work = values->usage.validation_work,
    };
}

static void check_plan_layout_classification(const SolMirRuntimeValues *values) {
    const SolMirLayout *layout = &values->conventions->concrete->layout;
    CHECK(values->allocation_plan_count == layout->type_count);
    for (size_t i = 0; i < values->allocation_plan_count; ++i) {
        const SolMirRuntimeAllocationPlan *plan = &values->allocation_plans[i];
        const SolMirTypeLayout *type = &layout->types[i];
        SolMirRuntimeAllocationPlanKind expected
            = SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE;
        if (type->object_kind == SOL_MIR_LAYOUT_OBJECT_PRODUCT
            || type->object_kind == SOL_MIR_LAYOUT_OBJECT_SUM)
            expected = SOL_MIR_RUNTIME_ALLOCATION_PLAN_FIXED_OBJECT;
        else if (type->object_kind == SOL_MIR_LAYOUT_OBJECT_TEXT)
            expected = SOL_MIR_RUNTIME_ALLOCATION_PLAN_TEXT;
        CHECK(plan->recipe == i && plan->kind == expected);
        if (expected == SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE)
            CHECK(plan->object_size == 0 && plan->object_alignment == 1);
        else
            CHECK(plan->object_size == type->object_size
                && plan->object_alignment == type->object_alignment);
    }
}

static SolMirRuntimeOwnershipClass expected_ownership_class(
    const SolMirRecipe *recipe) {
    if (!recipe->inhabited) return SOL_MIR_RUNTIME_OWNERSHIP_UNREACHABLE;
    switch (recipe->kind) {
        case SOL_MIR_RECIPE_TEXT: return SOL_MIR_RUNTIME_OWNERSHIP_TEXT;
        case SOL_MIR_RECIPE_TUPLE: case SOL_MIR_RECIPE_RECORD:
            return SOL_MIR_RUNTIME_OWNERSHIP_PRODUCT;
        case SOL_MIR_RECIPE_ENUM: case SOL_MIR_RECIPE_OPTION:
        case SOL_MIR_RECIPE_RESULT: return SOL_MIR_RUNTIME_OWNERSHIP_SUM;
        case SOL_MIR_RECIPE_DISTINCT: case SOL_MIR_RECIPE_REFINED:
            return SOL_MIR_RUNTIME_OWNERSHIP_WRAPPER;
        case SOL_MIR_RECIPE_FUNCTION: return SOL_MIR_RUNTIME_OWNERSHIP_CALLABLE;
        case SOL_MIR_RECIPE_CAPABILITY: return SOL_MIR_RUNTIME_OWNERSHIP_CAPABILITY;
        default: return SOL_MIR_RUNTIME_OWNERSHIP_LEAF;
    }
}

static SolMirRuntimeCopyClass expected_copy_class(const SolMirRecipe *recipe) {
    SolMirRuntimeOwnershipClass ownership = expected_ownership_class(recipe);
    if (!recipe->inhabited) return SOL_MIR_RUNTIME_COPY_UNREACHABLE;
    if (!recipe->is_copy || recipe->kind == SOL_MIR_RECIPE_FUNCTION
        || recipe->kind == SOL_MIR_RECIPE_CAPABILITY)
        return SOL_MIR_RUNTIME_COPY_FORBIDDEN;
    if (ownership == SOL_MIR_RUNTIME_OWNERSHIP_TEXT) return SOL_MIR_RUNTIME_COPY_TEXT;
    if (ownership == SOL_MIR_RUNTIME_OWNERSHIP_PRODUCT)
        return SOL_MIR_RUNTIME_COPY_PRODUCT;
    if (ownership == SOL_MIR_RUNTIME_OWNERSHIP_SUM) return SOL_MIR_RUNTIME_COPY_SUM;
    if (ownership == SOL_MIR_RUNTIME_OWNERSHIP_WRAPPER)
        return SOL_MIR_RUNTIME_COPY_WRAPPER;
    return SOL_MIR_RUNTIME_COPY_TRIVIAL;
}

static void check_ownership_plans(const SolMirRuntimeValues *values) {
    const SolMirRepresentation *r = &values->conventions->concrete->representation;
    CHECK(values->ownership_plan_count == r->recipe_count);
    for (size_t i = 0; i < values->ownership_plan_count; ++i) {
        const SolMirRuntimeOwnershipPlan *plan = &values->ownership_plans[i];
        const SolMirRecipe *recipe = &r->recipes[i];
        CHECK(values->copy_plans[i].recipe == i
            && values->copy_plans[i].classification == expected_copy_class(recipe));
        CHECK(plan->recipe == i
            && plan->classification == expected_ownership_class(recipe));
        if (plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_WRAPPER) {
            CHECK(plan->edges.count == 1);
            if (plan->edges.count == 1) {
                const SolMirRuntimeOwnedEdge *edge
                    = &values->owned_edges[plan->edges.offset];
                CHECK(edge->kind == SOL_MIR_RUNTIME_OWNED_EDGE_BACKING
                    && edge->recipe == recipe->backing);
            }
        }
        if (plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_CAPABILITY) {
            CHECK(plan->edges.count
                == (recipe->capability_source == SOL_MIR_RECIPE_NONE ? 0 : 1));
            if (plan->edges.count == 1)
                CHECK(values->owned_edges[plan->edges.offset].kind
                    == SOL_MIR_RUNTIME_OWNED_EDGE_PRIVATE_SOURCE);
        }
        if (plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_SUM) {
            CHECK(plan->variants.count == recipe->variants.count);
            for (size_t v = 0; v < plan->variants.count; ++v) {
                const SolMirRecipeVariant *source
                    = &r->variants[recipe->variants.offset + v];
                const SolMirRuntimeOwnershipVariant *variant
                    = &values->ownership_variants[plan->variants.offset + v];
                CHECK(variant->ordinal == source->ordinal
                    && variant->semantic_tag == source->semantic_tag
                    && variant->edges.count == source->fields.count);
            }
        }
    }
}

static void check_rejected_without_rendering(const SolMirRuntimeValues *values) {
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    FILE *stream = tmpfile();
    CHECK(stream != NULL);
    if (stream != NULL) {
        long before = ftell(stream);
        CHECK(before == 0 && !sol_mir_runtime_values_render(stream, values)
            && fflush(stream) == 0 && fseek(stream, 0, SEEK_END) == 0
            && ftell(stream) == before);
        fclose(stream);
    }
}

typedef enum {
    OWNERSHIP_ARENA_PLANS,
    OWNERSHIP_ARENA_VARIANTS,
    OWNERSHIP_ARENA_EDGES,
} OwnershipArena;

static void *ownership_arena_pointer(const SolMirRuntimeValues *values,
    OwnershipArena arena) {
    switch (arena) {
        case OWNERSHIP_ARENA_PLANS: return values->ownership_plans;
        case OWNERSHIP_ARENA_VARIANTS: return values->ownership_variants;
        case OWNERSHIP_ARENA_EDGES: return values->owned_edges;
    }
    return NULL;
}

static size_t ownership_arena_count(const SolMirRuntimeValues *values,
    OwnershipArena arena) {
    switch (arena) {
        case OWNERSHIP_ARENA_PLANS: return values->ownership_plan_count;
        case OWNERSHIP_ARENA_VARIANTS: return values->ownership_variant_count;
        case OWNERSHIP_ARENA_EDGES: return values->owned_edge_count;
    }
    return 0;
}

static size_t ownership_arena_capacity(const SolMirRuntimeValues *values,
    OwnershipArena arena) {
    switch (arena) {
        case OWNERSHIP_ARENA_PLANS: return values->ownership_plan_capacity;
        case OWNERSHIP_ARENA_VARIANTS: return values->ownership_variant_capacity;
        case OWNERSHIP_ARENA_EDGES: return values->owned_edge_capacity;
    }
    return 0;
}

static void ownership_arena_set_pointer(SolMirRuntimeValues *values,
    OwnershipArena arena, void *pointer) {
    switch (arena) {
        case OWNERSHIP_ARENA_PLANS:
            values->ownership_plans = pointer;
            break;
        case OWNERSHIP_ARENA_VARIANTS:
            values->ownership_variants = pointer;
            break;
        case OWNERSHIP_ARENA_EDGES:
            values->owned_edges = pointer;
            break;
    }
}

static void ownership_arena_set_count(SolMirRuntimeValues *values,
    OwnershipArena arena, size_t count) {
    switch (arena) {
        case OWNERSHIP_ARENA_PLANS: values->ownership_plan_count = count; break;
        case OWNERSHIP_ARENA_VARIANTS: values->ownership_variant_count = count; break;
        case OWNERSHIP_ARENA_EDGES: values->owned_edge_count = count; break;
    }
}

static void ownership_arena_set_capacity(SolMirRuntimeValues *values,
    OwnershipArena arena, size_t capacity) {
    switch (arena) {
        case OWNERSHIP_ARENA_PLANS:
            values->ownership_plan_capacity = capacity;
            break;
        case OWNERSHIP_ARENA_VARIANTS:
            values->ownership_variant_capacity = capacity;
            break;
        case OWNERSHIP_ARENA_EDGES: values->owned_edge_capacity = capacity; break;
    }
}

static void test_malformed_ownership_arenas(SolMirRuntimeValues *values) {
    SolMirConcreteProgram *concrete = (SolMirConcreteProgram *)(void *)
        values->conventions->concrete;
    const struct {
        const char *name;
        void *pointer;
    } malformed_pointers[] = {
        {"null", NULL},
        {"wrapped", (void *)(uintptr_t)(UINTPTR_MAX - 1)},
        {"owner", values},
    }, predecessor_aliases[] = {
        {"imports", (void *)values->conventions->imports},
        {"recipes", concrete->representation.recipes},
        {"fields", concrete->representation.fields},
        {"layout-types", concrete->layout.types},
        {"source-bytes", concrete->program.ir->source_bytes},
        {"recipe-operations", values->recipe_operations},
        {"allocation-plans", values->allocation_plans},
    };
    const OwnershipArena arenas[] = {OWNERSHIP_ARENA_PLANS,
        OWNERSHIP_ARENA_VARIANTS, OWNERSHIP_ARENA_EDGES};
    CHECK(values->ownership_plan_count != 0 && values->ownership_variant_count != 0
        && values->owned_edge_count != 0 && values->conventions->import_count != 0
        && concrete->representation.recipe_count != 0
        && concrete->representation.field_count != 0 && concrete->layout.type_count != 0
        && concrete->program.ir->source_bytes != NULL);
    for (size_t arena = 0; arena < sizeof(arenas) / sizeof(arenas[0]); ++arena) {
        for (size_t i = 0;
                i < sizeof(malformed_pointers) / sizeof(malformed_pointers[0]);
                ++i) {
            void *saved = ownership_arena_pointer(values, arenas[arena]);
            CHECK(malformed_pointers[i].name != NULL);
            ownership_arena_set_pointer(values, arenas[arena],
                malformed_pointers[i].pointer);
            check_rejected_without_rendering(values);
            ownership_arena_set_pointer(values, arenas[arena], saved);
        }
        for (size_t i = 0;
                i < sizeof(predecessor_aliases) / sizeof(predecessor_aliases[0]);
                ++i) {
            void *saved = ownership_arena_pointer(values, arenas[arena]);
            CHECK(predecessor_aliases[i].name != NULL);
            ownership_arena_set_pointer(values, arenas[arena],
                predecessor_aliases[i].pointer);
            check_rejected_without_rendering(values);
            ownership_arena_set_pointer(values, arenas[arena], saved);
        }
    }
    for (size_t i = 0; i < sizeof(arenas) / sizeof(arenas[0]); ++i) {
        size_t saved_count = ownership_arena_count(values, arenas[i]);
        size_t saved_capacity = ownership_arena_capacity(values, arenas[i]);
        ownership_arena_set_count(values, arenas[i], SIZE_MAX);
        check_rejected_without_rendering(values);
        ownership_arena_set_count(values, arenas[i], saved_count);
        ownership_arena_set_capacity(values, arenas[i], SIZE_MAX);
        check_rejected_without_rendering(values);
        ownership_arena_set_capacity(values, arenas[i], saved_capacity);
    }
    const struct {
        OwnershipArena destination;
        OwnershipArena source;
        const char *name;
    } overlap_cases[] = {
        {OWNERSHIP_ARENA_PLANS, OWNERSHIP_ARENA_VARIANTS, "plans-variants"},
        {OWNERSHIP_ARENA_PLANS, OWNERSHIP_ARENA_EDGES, "plans-edges"},
        {OWNERSHIP_ARENA_VARIANTS, OWNERSHIP_ARENA_EDGES, "variants-edges"},
    };
    for (size_t i = 0; i < sizeof(overlap_cases) / sizeof(overlap_cases[0]); ++i) {
        void *saved = ownership_arena_pointer(values, overlap_cases[i].destination);
        CHECK(overlap_cases[i].name != NULL);
        ownership_arena_set_pointer(values, overlap_cases[i].destination,
            ownership_arena_pointer(values, overlap_cases[i].source));
        check_rejected_without_rendering(values);
        ownership_arena_set_pointer(values, overlap_cases[i].destination, saved);
    }
    CHECK(sol_mir_runtime_values_validate(values, NULL));
}

static void test_allocation_plans(SolMirRuntimeValues *values) {
    size_t fixed = 0, text = 0, none = 0;
    uint64_t fixed_bytes = 0, text_size = 0, text_alignment = 0;
    SolMirRecipeId fixed_recipe = SOL_MIR_RECIPE_NONE;
    SolMirRecipeId text_recipe = SOL_MIR_RECIPE_NONE;
    SolMirRecipeId none_recipe = SOL_MIR_RECIPE_NONE;
    check_plan_layout_classification(values);
    for (size_t i = 0; i < values->allocation_plan_count; ++i) {
        const SolMirRuntimeAllocationPlan *plan = &values->allocation_plans[i];
        CHECK(plan->recipe == i);
        if (plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_FIXED_OBJECT) {
            ++fixed; fixed_bytes += plan->object_size; fixed_recipe = i;
        } else if (plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_TEXT) {
            ++text; text_size = plan->object_size;
            text_alignment = plan->object_alignment; text_recipe = i;
        } else {
            ++none; none_recipe = i;
            CHECK(plan->object_size == 0 && plan->object_alignment == 1);
        }
    }
    CHECK(values->allocation_plan_count == 21 && fixed == 8 && text == 1
        && none == 12 && fixed_bytes == 92 && text_size == 8
        && text_alignment == 4);
    SolMirRuntimeAllocationDemand demand = {0};
    size_t ownership_scans = sol_mir_runtime_values_test_ownership_count_scans();
    SolMirRuntimeAllocationUsage usage = {0};
    SolMirRuntimeAllocationQuota none_quota = {0, 0};
    SolMirRuntimeAllocationRequest request = {none_recipe, 0,
        SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_ALLOCATION_REQUEST_ORDINARY};
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &none_quota,
            &usage, &demand) == SOL_MIR_RUNTIME_ALLOCATION_SUCCEEDED
        && demand.requests == 0 && demand.bytes == 0);
    CHECK(sol_mir_runtime_values_test_ownership_count_scans() == ownership_scans);
    request.recipe = fixed_recipe;
    const SolMirRuntimeAllocationPlan *fixed_plan
        = &values->allocation_plans[fixed_recipe];
    SolMirRuntimeAllocationQuota fixed_quota = {1, fixed_plan->object_size};
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &fixed_quota,
            &usage, &demand) == SOL_MIR_RUNTIME_ALLOCATION_SUCCEEDED
        && demand.requests == 1 && demand.bytes == fixed_plan->object_size);
    --fixed_quota.max_bytes;
    demand = (SolMirRuntimeAllocationDemand){111, 222};
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &fixed_quota,
            &usage, &demand) == SOL_MIR_RUNTIME_ALLOCATION_LIMIT
        && demand.requests == 111 && demand.bytes == 222);
    request.recipe = text_recipe;
    request.text_length = 0;
    SolMirRuntimeAllocationQuota text_header = {1, text_size};
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &text_header,
            &usage, &demand) == SOL_MIR_RUNTIME_ALLOCATION_SUCCEEDED
        && demand.requests == 1 && demand.bytes == text_size);
    request.text_length = 7;
    SolMirRuntimeAllocationQuota text_quota = {2, text_size + 7};
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &text_quota,
            &usage, &demand) == SOL_MIR_RUNTIME_ALLOCATION_SUCCEEDED
        && demand.requests == 2 && demand.bytes == text_size + 7);
    --text_quota.max_requests;
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &text_quota,
            &usage, &demand) == SOL_MIR_RUNTIME_ALLOCATION_LIMIT);
    ++text_quota.max_requests;
    --text_quota.max_bytes;
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &text_quota,
            &usage, &demand) == SOL_MIR_RUNTIME_ALLOCATION_LIMIT);
    request.recipe = fixed_recipe;
    request.text_length = 1;
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &fixed_quota,
            &usage, &demand) == SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT);
    request.recipe = text_recipe;
    request.text_length = (uint64_t)UINT32_MAX + 1;
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &text_quota,
            &usage, &demand) == SOL_MIR_RUNTIME_ALLOCATION_LIMIT);
    request.text_length = 1;
    SolMirRuntimeAllocationQuota unlimited = {UINT64_MAX, UINT64_MAX};
    usage = (SolMirRuntimeAllocationUsage){UINT64_MAX, 0};
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &unlimited,
        &usage, &demand) == SOL_MIR_RUNTIME_ALLOCATION_LIMIT);
    usage = (SolMirRuntimeAllocationUsage){0, UINT64_MAX};
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &unlimited,
            &usage, &demand) == SOL_MIR_RUNTIME_ALLOCATION_LIMIT);
    usage = (SolMirRuntimeAllocationUsage){2, 0};
    SolMirRuntimeAllocationQuota malformed_usage = {1, UINT64_MAX};
    CHECK(sol_mir_runtime_values_check_allocation(values, &request,
            &malformed_usage, &usage, &demand)
        == SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT);
    usage = (SolMirRuntimeAllocationUsage){0, 0};
    request.recipe = none_recipe;
    request.text_length = 0;
    SolMirRuntimeAllocationUsage before_alias = usage;
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &none_quota,
            &usage, (SolMirRuntimeAllocationDemand *)(void *)&usage)
        == SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT
        && memcmp(&usage, &before_alias, sizeof(usage)) == 0);
    SolMirRuntimeAllocationPlan plan_before = values->allocation_plans[none_recipe];
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &none_quota,
            &usage, (SolMirRuntimeAllocationDemand *)(void *)
                &values->allocation_plans[none_recipe])
        == SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT
        && memcmp(&values->allocation_plans[none_recipe], &plan_before,
            sizeof(plan_before)) == 0);
    SolMirRuntimeCopyPlan copy_before = values->copy_plans[none_recipe];
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &none_quota,
            &usage, (SolMirRuntimeAllocationDemand *)(void *)
                &values->copy_plans[none_recipe])
        == SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT
        && memcmp(&values->copy_plans[none_recipe], &copy_before,
            sizeof(copy_before)) == 0);
    request.recipe = text_recipe;
    request.text_length = 17;
    uint64_t saved_object_limit
        = values->conventions->concrete->layout.target.max_object_bytes;
    ((SolMirConcreteProgram *)(void *)values->conventions->concrete)
        ->layout.target.max_object_bytes = 16;
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &unlimited,
            &usage, &demand) == SOL_MIR_RUNTIME_ALLOCATION_LIMIT);
    ((SolMirConcreteProgram *)(void *)values->conventions->concrete)
        ->layout.target.max_object_bytes = saved_object_limit;
    SolMirRuntimeValuesUsage saved_owner_usage = values->usage;
    values->usage.allocation_plans = 0;
    demand = (SolMirRuntimeAllocationDemand){333, 444};
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &unlimited,
            &usage, &demand) == SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT
        && demand.requests == 333 && demand.bytes == 444);
    values->usage = saved_owner_usage;
    SolMirRuntimeValuesLimits saved_owner_limits = values->limits;
    values->limits.max_allocation_plans = 0;
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &unlimited,
            &usage, &demand) == SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT);
    values->limits = saved_owner_limits;
    size_t saved_plan_capacity = values->allocation_plan_capacity;
    --values->allocation_plan_capacity;
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &unlimited,
            &usage, &demand) == SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT);
    values->allocation_plan_capacity = saved_plan_capacity;
    SolMirRuntimeConventions *mutable_conventions
        = (SolMirRuntimeConventions *)(void *)values->conventions;
    const SolMirConcreteProgram *saved_concrete = mutable_conventions->concrete;
    mutable_conventions->concrete = NULL;
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &unlimited,
            &usage, &demand) == SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT);
    mutable_conventions->concrete = saved_concrete;
    SolMirRuntimeFailureCode failure;
    CHECK(sol_mir_runtime_allocation_outcome_failure(
            SOL_MIR_RUNTIME_ALLOCATION_SUCCEEDED, &failure)
        && failure == SOL_MIR_RUNTIME_FAILURE_NONE);
    CHECK(sol_mir_runtime_allocation_outcome_failure(
            SOL_MIR_RUNTIME_ALLOCATION_LIMIT, &failure)
        && failure == SOL_MIR_RUNTIME_FAILURE_ALLOCATION_LIMIT);
    CHECK(sol_mir_runtime_allocation_outcome_failure(
            SOL_MIR_RUNTIME_ALLOCATION_FAILED, &failure)
        && failure == SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED);
    CHECK(!sol_mir_runtime_allocation_outcome_failure(
        SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT, &failure));
}

static SolMirRecipeId host_result_recipe(const SolMirRuntimeValues *values,
    SolMirRuntimeHostResultClass classification) {
    for (size_t i = 0; i < values->host_result_plan_count; ++i)
        if (values->host_result_plans[i].classification == classification) return i;
    return SOL_MIR_RECIPE_NONE;
}

static void test_host_result_transfer_model(const SolMirRuntimeValues *values) {
    SolMirRecipeId boolean = host_result_recipe(values,
        SOL_MIR_RUNTIME_HOST_RESULT_BOOL);
    SolMirRecipeId text = host_result_recipe(values, SOL_MIR_RUNTIME_HOST_RESULT_TEXT);
    CHECK(boolean != SOL_MIR_RECIPE_NONE && text != SOL_MIR_RECIPE_NONE);
    SolMirRuntimeAllocationQuota zero = {0, 0};
    SolMirRuntimeAllocationUsage usage = {0};
    SolMirRuntimeHostValue source = {.kind = SOL_MIR_RUNTIME_HOST_VALUE_BOOL,
        .as.bool_value = true};
    SolMirRuntimeHostOwnedValue *owned = NULL;
    SolMirRuntimeHostTransferRequest request = {values, boolean, &source, &zero,
        &usage, NULL};
    CHECK(sol_mir_runtime_values_test_transfer_host_result(&request, &owned)
            == SOL_MIR_RUNTIME_HOST_TRANSFER_SUCCEEDED
        && owned != NULL && owned->kind == SOL_MIR_RUNTIME_HOST_VALUE_BOOL
        && owned->as.bool_value && usage.requests == 0 && usage.bytes == 0);
    sol_mir_runtime_host_owned_value_free(owned);
    owned = NULL;
    source.kind = SOL_MIR_RUNTIME_HOST_VALUE_INT64;
    CHECK(sol_mir_runtime_values_test_transfer_host_result(&request, &owned)
            == SOL_MIR_RUNTIME_HOST_TRANSFER_INVALID
        && owned == NULL && usage.requests == 0 && usage.bytes == 0);
    SolMirRecipeId integer = host_result_recipe(values,
        SOL_MIR_RUNTIME_HOST_RESULT_INT64);
    SolMirRecipeId unit = host_result_recipe(values, SOL_MIR_RUNTIME_HOST_RESULT_UNIT);
    source = (SolMirRuntimeHostValue){.kind = SOL_MIR_RUNTIME_HOST_VALUE_INT64,
        .as.int64_value = -17};
    request = (SolMirRuntimeHostTransferRequest){values, integer, &source, &zero,
        &usage, NULL};
    CHECK(sol_mir_runtime_values_test_transfer_host_result(&request, &owned)
            == SOL_MIR_RUNTIME_HOST_TRANSFER_SUCCEEDED && owned != NULL
        && owned->as.int64_value == -17);
    sol_mir_runtime_host_owned_value_free(owned); owned = NULL;
    source = (SolMirRuntimeHostValue){.kind = SOL_MIR_RUNTIME_HOST_VALUE_UNIT};
    request.recipe = unit;
    CHECK(sol_mir_runtime_values_test_transfer_host_result(&request, &owned)
            == SOL_MIR_RUNTIME_HOST_TRANSFER_SUCCEEDED && owned != NULL);
    sol_mir_runtime_host_owned_value_free(owned); owned = NULL;

    uint8_t bytes[] = {'h', 'o', 's', 't'};
    source = (SolMirRuntimeHostValue){.kind = SOL_MIR_RUNTIME_HOST_VALUE_TEXT,
        .as.text = {bytes, sizeof(bytes)}};
    const SolMirRuntimeAllocationPlan *text_plan = &values->allocation_plans[text];
    SolMirRuntimeAllocationQuota quota = {2, text_plan->object_size + sizeof(bytes)};
    request = (SolMirRuntimeHostTransferRequest){values, text, &source, &quota,
        &usage, NULL};
    CHECK(sol_mir_runtime_values_test_transfer_host_result(&request, &owned)
            == SOL_MIR_RUNTIME_HOST_TRANSFER_SUCCEEDED && owned != NULL
        && owned->as.text.bytes != bytes && owned->as.text.length == sizeof(bytes)
        && memcmp(owned->as.text.bytes, bytes, sizeof(bytes)) == 0
        && usage.requests == 2 && usage.bytes == text_plan->object_size + sizeof(bytes));
    bytes[0] = 'X';
    CHECK(owned->as.text.bytes[0] == 'h');
    sol_mir_runtime_host_owned_value_free(owned);
    owned = NULL;

    /* The same borrowed argument view may be returned repeatedly, and an
       unrelated persistent host view gets an independently owned payload. */
    uint8_t persistent[] = {'s', 't', 'o', 'r', 'e'};
    usage = (SolMirRuntimeAllocationUsage){0};
    source.as.text.bytes = bytes; source.as.text.length = sizeof(bytes);
    quota = (SolMirRuntimeAllocationQuota){2, text_plan->object_size + sizeof(bytes)};
    request.quota = &quota;
    CHECK(sol_mir_runtime_values_test_transfer_host_result(&request, &owned)
            == SOL_MIR_RUNTIME_HOST_TRANSFER_SUCCEEDED && owned != NULL);
    sol_mir_runtime_host_owned_value_free(owned); owned = NULL;
    usage = (SolMirRuntimeAllocationUsage){0};
    source.as.text.bytes = persistent; source.as.text.length = sizeof(persistent);
    quota = (SolMirRuntimeAllocationQuota){2, text_plan->object_size + sizeof(persistent)};
    CHECK(sol_mir_runtime_values_test_transfer_host_result(&request, &owned)
            == SOL_MIR_RUNTIME_HOST_TRANSFER_SUCCEEDED && owned != NULL
        && owned->as.text.bytes != persistent);
    persistent[0] = 'X';
    CHECK(owned->as.text.bytes[0] == 's');
    sol_mir_runtime_host_owned_value_free(owned); owned = NULL;

    source = (SolMirRuntimeHostValue){.kind = SOL_MIR_RUNTIME_HOST_VALUE_TEXT,
        .as.text = {NULL, 0}};
    usage = (SolMirRuntimeAllocationUsage){0};
    SolMirRuntimeAllocationQuota header_only = {1, text_plan->object_size};
    request.quota = &header_only; request.usage = &usage; request.limits = NULL;
    CHECK(sol_mir_runtime_values_test_transfer_host_result(&request, &owned)
            == SOL_MIR_RUNTIME_HOST_TRANSFER_SUCCEEDED && owned != NULL
        && owned->as.text.length == 0 && owned->as.text.bytes == NULL
        && usage.requests == 1 && usage.bytes == text_plan->object_size);
    sol_mir_runtime_host_owned_value_free(owned); owned = NULL;

    usage = (SolMirRuntimeAllocationUsage){0};
    source = (SolMirRuntimeHostValue){.kind = SOL_MIR_RUNTIME_HOST_VALUE_TEXT,
        .as.text = {bytes, sizeof(bytes)}};
    quota = (SolMirRuntimeAllocationQuota){2, text_plan->object_size + sizeof(bytes)};
    request.quota = &quota;
    SolMirRuntimeHostTransferLimits fail_second = {.fail_allocation_attempt = 2};
    request.limits = &fail_second;
    CHECK(sol_mir_runtime_values_test_transfer_host_result(&request, &owned)
            == SOL_MIR_RUNTIME_HOST_TRANSFER_ALLOCATION_FAILED && owned == NULL
        && usage.requests == 1 && usage.bytes == text_plan->object_size);
    SolMirRuntimeAllocationPlan saved_text_plan = values->allocation_plans[text];
    ((SolMirRuntimeValues *)(void *)values)->allocation_plans[text].object_size = 0;
    usage = (SolMirRuntimeAllocationUsage){0};
    request.limits = NULL;
    CHECK(sol_mir_runtime_values_test_transfer_host_result(&request, &owned)
            == SOL_MIR_RUNTIME_HOST_TRANSFER_INVALID && owned == NULL
        && usage.requests == 0 && usage.bytes == 0);
    ((SolMirRuntimeValues *)(void *)values)->allocation_plans[text] = saved_text_plan;
    usage = (SolMirRuntimeAllocationUsage){0};
    source.as.text.bytes = NULL;
    CHECK(sol_mir_runtime_values_test_transfer_host_result(&request, &owned)
            == SOL_MIR_RUNTIME_HOST_TRANSFER_INVALID && owned == NULL
        && usage.requests == 0 && usage.bytes == 0);
    source.as.text.bytes = bytes;
    source.as.text.length = values->conventions->concrete->layout.target.max_object_bytes + 1;
    SolMirRuntimeAllocationQuota unlimited = {UINT64_MAX, UINT64_MAX};
    request.quota = &unlimited;
    CHECK(sol_mir_runtime_values_test_transfer_host_result(&request, &owned)
            == SOL_MIR_RUNTIME_HOST_TRANSFER_ALLOCATION_LIMIT && owned == NULL
        && usage.requests == 0 && usage.bytes == 0);
    source.as.text.length = sizeof(bytes);
}

static void test_inventory(SolMirConcreteProgram *program,
    SolDiagnostics *diagnostics, SolMirRuntimeConventions *conventions,
    SolMirRuntimeValues *values) {
    size_t edge_kinds[4] = {0}, captures = 0;
    bool capture_producers[5] = {false};
    for (size_t i = 0; i < values->owned_edge_count; ++i) {
        const SolMirRuntimeOwnedEdge *edge = &values->owned_edges[i];
        ++edge_kinds[edge->kind];
        if (edge->kind == SOL_MIR_RUNTIME_OWNED_EDGE_CAPTURED_RECEIVER) {
            ++captures;
            CHECK(edge->producer < sizeof(capture_producers) / sizeof(*capture_producers)
                && !capture_producers[edge->producer]);
            if (edge->producer < sizeof(capture_producers) / sizeof(*capture_producers))
                capture_producers[edge->producer] = true;
        }
    }
    const size_t expected_edge_kinds[] = {11, 1, 0, 5};
    CHECK(memcmp(edge_kinds, expected_edge_kinds, sizeof(edge_kinds)) == 0
        && captures == 5);
    for (size_t i = 0; i < sizeof(capture_producers) / sizeof(*capture_producers); ++i)
        CHECK(capture_producers[i]);
    CHECK(values->recipe_operation_count == 21);
    CHECK(values->usage.records == 21 && values->usage.allocation_plans == 21
        && values->usage.copy_plans == 21 && values->usage.equality_plans == 21
        && values->usage.host_result_plans == 21
        && values->usage.host_result_requirements == 4
        && values->usage.ownership_plans == 21
        && values->usage.ownership_variants == 9 && values->usage.owned_edges == 17
        && values->usage.owned_bytes == 4592
        && values->usage.build_scratch_bytes == 98
        && values->usage.build_work == 410
        /* The values validator borrows the concrete/conventions scratch peak;
         * callback provenance grows that authenticated predecessor peak. */
        && values->usage.validation_scratch_bytes == 763171604
        && values->usage.validation_work == 96030924);
    test_allocation_plans(values);
    test_host_result_transfer_model(values);
    check_ownership_plans(values);
    CHECK(values->copy_plan_count == 21 && values->copy_plan_capacity == 21);
    CHECK(values->equality_plan_count == 21 && values->equality_plan_capacity == 21);
    CHECK(values->host_result_plan_count == 21
        && values->host_result_requirement_count == 4);
    { size_t classes[8] = {0};
      for (size_t i = 0; i < values->host_result_plan_count; ++i) {
          const SolMirRuntimeHostResultPlan *plan = &values->host_result_plans[i];
          CHECK(plan->recipe == i && plan->classification
              <= SOL_MIR_RUNTIME_HOST_RESULT_RESULT);
          if (plan->classification <= SOL_MIR_RUNTIME_HOST_RESULT_RESULT)
              ++classes[plan->classification];
      }
      CHECK(classes[SOL_MIR_RUNTIME_HOST_RESULT_UNREACHABLE] == 1
          && classes[SOL_MIR_RUNTIME_HOST_RESULT_FORBIDDEN] == 14
          && classes[SOL_MIR_RUNTIME_HOST_RESULT_INT64] == 1
          && classes[SOL_MIR_RUNTIME_HOST_RESULT_BOOL] == 1
          && classes[SOL_MIR_RUNTIME_HOST_RESULT_TEXT] == 1
          && classes[SOL_MIR_RUNTIME_HOST_RESULT_UNIT] == 1
          && classes[SOL_MIR_RUNTIME_HOST_RESULT_OPTION] == 2
          && classes[SOL_MIR_RUNTIME_HOST_RESULT_RESULT] == 0);
    }
    { size_t classes[7] = {0};
      for (size_t i = 0; i < values->equality_plan_count; ++i) {
          const SolMirRuntimeEqualityPlan *plan = &values->equality_plans[i];
          CHECK(plan->recipe == i && plan->classification
              <= SOL_MIR_RUNTIME_EQUALITY_WRAPPER);
          if (plan->classification <= SOL_MIR_RUNTIME_EQUALITY_WRAPPER)
              ++classes[plan->classification];
      }
      /* E6 Wasm32: unreachable, forbidden, trivial, text, product, sum, wrapper. */
      CHECK(classes[SOL_MIR_RUNTIME_EQUALITY_UNREACHABLE] == 1
          && classes[SOL_MIR_RUNTIME_EQUALITY_FORBIDDEN] == 7
          && classes[SOL_MIR_RUNTIME_EQUALITY_TRIVIAL] == 3
          && classes[SOL_MIR_RUNTIME_EQUALITY_TEXT] == 1
          && classes[SOL_MIR_RUNTIME_EQUALITY_PRODUCT] == 3
          && classes[SOL_MIR_RUNTIME_EQUALITY_SUM] == 5
          && classes[SOL_MIR_RUNTIME_EQUALITY_WRAPPER] == 1);
    }
    { size_t classes[7] = {0};
      for (size_t i = 0; i < values->copy_plan_count; ++i) {
          const SolMirRuntimeCopyPlan *plan = &values->copy_plans[i];
          CHECK(plan->recipe == i && plan->classification
              <= SOL_MIR_RUNTIME_COPY_WRAPPER);
          if (plan->classification <= SOL_MIR_RUNTIME_COPY_WRAPPER)
              ++classes[plan->classification];
      }
      /* E6 Wasm32: unreachable, forbidden, trivial, text, product, sum, wrapper. */
      CHECK(classes[SOL_MIR_RUNTIME_COPY_UNREACHABLE] == 1
          && classes[SOL_MIR_RUNTIME_COPY_FORBIDDEN] == 7
          && classes[SOL_MIR_RUNTIME_COPY_TRIVIAL] == 3
          && classes[SOL_MIR_RUNTIME_COPY_TEXT] == 1
          && classes[SOL_MIR_RUNTIME_COPY_PRODUCT] == 3
          && classes[SOL_MIR_RUNTIME_COPY_SUM] == 5
          && classes[SOL_MIR_RUNTIME_COPY_WRAPPER] == 1);
    }
    CHECK(values->recipe_operation_count
        == program->representation.recipe_count);
    size_t bound_callable_plans = 0;
    for (size_t i = 0; i < program->operations.callable_count; ++i) {
        const SolMirOperationCallablePlan *plan = &program->operations.callables[i];
        if (plan->kind != SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION) continue;
        ++bound_callable_plans;
        CHECK(plan->function_recipe < program->layout.type_count
            && plan->capture_recipe < program->layout.type_count
            && program->layout.target.pointer_size == 4
            && program->layout.types[plan->function_recipe].object_kind
                == SOL_MIR_LAYOUT_OBJECT_CALLABLE
            && program->layout.types[plan->capture_recipe].object_kind
                == SOL_MIR_LAYOUT_OBJECT_CAPABILITY
            && program->layout.types[plan->capture_recipe].object_size == 8);
        SolMirRuntimeAllocationRequest request = {plan->function_recipe, 0, i,
            SOL_MIR_RUNTIME_ALLOCATION_REQUEST_CALLABLE};
        SolMirRuntimeAllocationQuota exact = {2, 12};
        SolMirRuntimeAllocationDemand demand = {0};
        CHECK(sol_mir_runtime_values_check_allocation(values, &request, &exact,
                &(SolMirRuntimeAllocationUsage){0}, &demand)
                == SOL_MIR_RUNTIME_ALLOCATION_SUCCEEDED
            && demand.requests == 2 && demand.bytes == 12);
        SolMirRuntimeAllocationDemand unchanged = {23, 29};
        --exact.max_requests;
        CHECK(sol_mir_runtime_values_check_allocation(values, &request, &exact,
                &(SolMirRuntimeAllocationUsage){0}, &unchanged)
                == SOL_MIR_RUNTIME_ALLOCATION_LIMIT
            && unchanged.requests == 23 && unchanged.bytes == 29);
        ++exact.max_requests;
        --exact.max_bytes;
        CHECK(sol_mir_runtime_values_check_allocation(values, &request, &exact,
                &(SolMirRuntimeAllocationUsage){0}, &unchanged)
                == SOL_MIR_RUNTIME_ALLOCATION_LIMIT
            && unchanged.requests == 23 && unchanged.bytes == 29);
    }
    CHECK(bound_callable_plans == 5);
    size_t demanding = 0, create = 0, copy = 0, drop = 0, equal = 0;
    for (size_t i = 0; i < values->recipe_operation_count; ++i) {
        const SolMirRuntimeRecipeOperations *record
            = &values->recipe_operations[i];
        CHECK(record->recipe == i);
        demanding += record->demanded_operations != 0;
#define CHECK_PAIR(flag, member, expected_kind) do { \
    bool demanded = (record->demanded_operations & (flag)) != 0; \
    CHECK(demanded == (record->member != SOL_MIR_RUNTIME_NONE)); \
    if (demanded) { \
        CHECK(record->member < conventions->import_count); \
        if (record->member < conventions->import_count) { \
            const SolMirRuntimeImport *import \
                = &conventions->imports[record->member]; \
            CHECK(import->kind == (expected_kind) && import->recipe == i \
                && import->recipe_operation == (flag)); \
        } \
    } \
} while (0)
        CHECK_PAIR(SOL_MIR_LINKAGE_RUNTIME_CREATE, create_import,
            SOL_MIR_RUNTIME_IMPORT_RECIPE_CREATE);
        CHECK_PAIR(SOL_MIR_LINKAGE_RUNTIME_COPY, copy_import,
            SOL_MIR_RUNTIME_IMPORT_RECIPE_COPY);
        CHECK_PAIR(SOL_MIR_LINKAGE_RUNTIME_DROP, drop_import,
            SOL_MIR_RUNTIME_IMPORT_RECIPE_DROP);
        CHECK_PAIR(SOL_MIR_LINKAGE_RUNTIME_EQUAL, equal_import,
            SOL_MIR_RUNTIME_IMPORT_RECIPE_EQUAL);
#undef CHECK_PAIR
        create += (record->demanded_operations
            & SOL_MIR_LINKAGE_RUNTIME_CREATE) != 0;
        copy += (record->demanded_operations
            & SOL_MIR_LINKAGE_RUNTIME_COPY) != 0;
        drop += (record->demanded_operations
            & SOL_MIR_LINKAGE_RUNTIME_DROP) != 0;
        equal += (record->demanded_operations
            & SOL_MIR_LINKAGE_RUNTIME_EQUAL) != 0;
    }
    CHECK(demanding == 17 && create == 16 && copy == 10 && drop == 17
        && equal == 5);
    size_t host = 0;
    for (size_t i = 0; i < conventions->import_count; ++i)
        host += conventions->imports[i].kind == SOL_MIR_RUNTIME_IMPORT_HOST;
    CHECK(host == 4 && sol_mir_runtime_values_validate(values, NULL));
    SolMirRuntimeValuesUsage reconstructed;
    CHECK(sol_mir_runtime_values_test_reconstruct_usage(conventions,
            &values->limits, &reconstructed)
        && memcmp(&reconstructed, &values->usage, sizeof(reconstructed)) == 0);

    char *runtime_before = render_conventions(conventions);
    char *first = render_values(values);
    char *runtime_after = render_conventions(conventions);
    CHECK(runtime_before != NULL && runtime_after != NULL
        && strcmp(runtime_before, runtime_after) == 0);
    CHECK(first != NULL
        && strstr(first, "operation-demand-allocation-plan-inventory") != NULL
        && strstr(first, "executable-operations=false") != NULL
        && strstr(first, "ownership-plans=true") != NULL
        && strstr(first, "copy-plans=true") != NULL
        && strstr(first, "equality-plans=true") != NULL
        && strstr(first, "host-result-plans=true") != NULL
        && strstr(first, "host-result-execution=false") != NULL
        && strstr(first, "equality-execution=false") != NULL
        && strstr(first, "copy-execution=false") != NULL
        && strstr(first, "move-drop-execution=false") != NULL
        && strstr(first, " recipe=") == NULL
        && strstr(first, "capacity") == NULL
        && strstr(first, "producer=") == NULL
        && strstr(first, program->program.ir->source_path) == NULL);
    if (first != NULL) {
        size_t relations = 0;
        for (const char *at = first; (at = strstr(at, "host-result requirement=")) != NULL;
                at += strlen("host-result requirement=")) ++relations;
        CHECK(relations == values->host_result_requirement_count
            && strstr(first, " result=") != NULL && strstr(first, " class=") != NULL);
    }

    SolMirRuntimeValuesLimits exact = exact_limits(values);
    SolMirRuntimeValues limited;
    sol_mir_runtime_values_init(&limited);
    SolMirRuntimeValuesBuildRequest request = {conventions, &exact};
    CHECK(sol_mir_runtime_values_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_values_validate(&limited, NULL));
    char *second = render_values(&limited);
    CHECK(first != NULL && second != NULL && strcmp(first, second) == 0);
    SolMirRuntimeValues before = limited;
    CHECK(sol_mir_runtime_values_build(&request, &limited, diagnostics)
            == SOL_MIR_RUNTIME_VALUES_BUILD_INVALID_ARGUMENT
        && memcmp(&before, &limited, sizeof(before)) == 0);
    free(second);
    sol_mir_runtime_values_free(&limited);
#define ONE_BELOW(member) do { \
    SolMirRuntimeValuesLimits one_below = exact; \
    --one_below.member; \
    request.limits = &one_below; \
    CHECK(sol_mir_runtime_values_build(&request, &limited, diagnostics) \
        == SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED); \
    CHECK(limited.conventions == NULL); \
} while (0)
    ONE_BELOW(max_records);
    ONE_BELOW(max_allocation_plans);
    ONE_BELOW(max_copy_plans);
    ONE_BELOW(max_equality_plans);
    ONE_BELOW(max_host_result_plans);
    ONE_BELOW(max_host_result_requirements);
    ONE_BELOW(max_ownership_plans);
    ONE_BELOW(max_ownership_variants);
    ONE_BELOW(max_owned_edges);
    ONE_BELOW(max_owned_bytes);
    ONE_BELOW(max_build_scratch_bytes);
    ONE_BELOW(max_build_work);
    ONE_BELOW(max_validation_scratch_bytes);
    ONE_BELOW(max_validation_work);
#undef ONE_BELOW
    SolMirRuntimeValues validation_limited = *values;
    validation_limited.limits.max_validation_work
        = values->usage.validation_work - 1;
    CHECK(!sol_mir_runtime_values_validate(&validation_limited, NULL));
    SolMirRuntimeValuesLimits partial = exact;
    partial.max_records = 0;
    request.limits = &partial;
    CHECK(sol_mir_runtime_values_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_VALUES_BUILD_INVALID_ARGUMENT);
    request.limits = NULL;
    sol_mir_runtime_values_test_force_allocation_failure(true);
    CHECK(sol_mir_runtime_values_build(&request, &limited, diagnostics)
            == SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED
        && limited.conventions == NULL);
    sol_mir_runtime_values_test_force_allocation_failure(false);
    sol_mir_runtime_values_test_force_persistent_allocation_failure(2);
    CHECK(sol_mir_runtime_values_build(&request, &limited, diagnostics)
            == SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED
        && limited.conventions == NULL);
    sol_mir_runtime_values_test_force_persistent_allocation_failure(0);
    for (size_t attempt = 3; attempt <= 9; ++attempt) {
        sol_mir_runtime_values_test_force_persistent_allocation_failure(attempt);
        CHECK(sol_mir_runtime_values_build(&request, &limited, diagnostics)
                == SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED
            && limited.conventions == NULL);
    }
    sol_mir_runtime_values_test_force_persistent_allocation_failure(0);
    diagnostics->allocation_failed = false;
    sol_mir_runtime_values_test_force_validation_allocation_failure(true);
    CHECK(!sol_mir_runtime_values_validate(values, diagnostics)
        && diagnostics->allocation_failed
        && sol_mir_runtime_values_test_validation_allocation_attempts() == 1);
    diagnostics->allocation_failed = false;
    CHECK(sol_mir_runtime_values_build(&request, &limited, diagnostics)
            == SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED
        && limited.conventions == NULL);
    CHECK(sol_mir_runtime_values_build(&request, &limited, NULL)
            == SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED
        && limited.conventions == NULL);
    sol_mir_runtime_values_test_force_validation_allocation_failure(false);
    diagnostics->allocation_failed = false;
    for (size_t attempt = 1; attempt <= 2; ++attempt) {
        sol_mir_runtime_values_test_force_validation_allocation_failure_attempt(attempt);
        CHECK(!sol_mir_runtime_values_validate(values, diagnostics)
            && diagnostics->allocation_failed
            && sol_mir_runtime_values_test_validation_allocation_attempts() == attempt);
        diagnostics->allocation_failed = false;
    }
    sol_mir_runtime_values_test_force_validation_allocation_failure_attempt(0);
    CHECK(sol_mir_runtime_values_validate(values, NULL));

    sol_mir_concrete_test_force_validation_allocation_failure(true);
    CHECK(sol_mir_runtime_values_build(&request, &limited, NULL)
            == SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED
        && limited.conventions == NULL
        && sol_mir_concrete_test_validation_allocation_attempts() != 0);
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    diagnostics->allocation_failed = false;
    CHECK(sol_mir_runtime_values_build(&request, &limited, diagnostics)
            == SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED
        && diagnostics->allocation_failed
        && limited.conventions == NULL);
    diagnostics->allocation_failed = false;
    sol_mir_concrete_test_force_validation_allocation_failure(false);

    SolMirRuntimeRecipeOperations saved = values->recipe_operations[0];
    values->recipe_operations[0].recipe = 1;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->recipe_operations[0] = saved;
    size_t demanded_record = 0;
    while (demanded_record < values->recipe_operation_count
        && values->recipe_operations[demanded_record].demanded_operations == 0)
        ++demanded_record;
    CHECK(demanded_record < values->recipe_operation_count);
    if (demanded_record < values->recipe_operation_count) {
        saved = values->recipe_operations[demanded_record];
        values->recipe_operations[demanded_record].demanded_operations
            ^= SOL_MIR_LINKAGE_RUNTIME_CREATE;
        CHECK(!sol_mir_runtime_values_validate(values, NULL));
        values->recipe_operations[demanded_record] = saved;
        SolMirRuntimeImportId *id = saved.create_import != SOL_MIR_RUNTIME_NONE
            ? &values->recipe_operations[demanded_record].create_import
            : saved.copy_import != SOL_MIR_RUNTIME_NONE
                ? &values->recipe_operations[demanded_record].copy_import
                : &values->recipe_operations[demanded_record].drop_import;
        SolMirRuntimeImportId saved_id = *id;
        *id = SOL_MIR_RUNTIME_NONE;
        CHECK(!sol_mir_runtime_values_validate(values, NULL));
        *id = saved_id;
    }
    --values->recipe_operation_count;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    ++values->recipe_operation_count;
    --values->recipe_operation_capacity;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    ++values->recipe_operation_capacity;
    --values->allocation_plan_count;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    ++values->allocation_plan_count;
    --values->allocation_plan_capacity;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    ++values->allocation_plan_capacity;
    SolMirRuntimeAllocationPlan saved_plan = values->allocation_plans[0];
    values->allocation_plans[0].recipe = 1;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->allocation_plans[0] = saved_plan;
    --values->copy_plan_count;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    ++values->copy_plan_count;
    --values->copy_plan_capacity;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    ++values->copy_plan_capacity;
    SolMirRuntimeCopyPlan saved_copy = values->copy_plans[0];
    values->copy_plans[0].recipe = 1;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    check_rejected_without_rendering(values);
    values->copy_plans[0] = saved_copy;
    values->copy_plans[0].classification = SOL_MIR_RUNTIME_COPY_FORBIDDEN;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    check_rejected_without_rendering(values);
    values->copy_plans[0] = saved_copy;
    SolMirRuntimeCopyPlan *copy_plans = values->copy_plans;
    values->copy_plans = NULL;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->copy_plans = (SolMirRuntimeCopyPlan *)(void *)values;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->copy_plans = (SolMirRuntimeCopyPlan *)(void *)values->recipe_operations;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->copy_plans = (SolMirRuntimeCopyPlan *)(void *)program->representation.recipes;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->copy_plans = (SolMirRuntimeCopyPlan *)(uintptr_t)(UINTPTR_MAX - 1);
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->copy_plans = copy_plans;
    SolMirRuntimeValuesUsage copy_usage = values->usage;
    --values->usage.copy_plans;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->usage = copy_usage;
    SolMirRuntimeEqualityPlan saved_equality = values->equality_plans[0];
    values->equality_plans[0].recipe = 1;
    check_rejected_without_rendering(values);
    values->equality_plans[0] = saved_equality;
    values->equality_plans[0].classification = SOL_MIR_RUNTIME_EQUALITY_FORBIDDEN;
    check_rejected_without_rendering(values);
    values->equality_plans[0] = saved_equality;
    --values->equality_plan_count;
    check_rejected_without_rendering(values);
    ++values->equality_plan_count;
    --values->equality_plan_capacity;
    check_rejected_without_rendering(values);
    ++values->equality_plan_capacity;
    SolMirRuntimeEqualityPlan *equality_plans = values->equality_plans;
    values->equality_plans = NULL;
    check_rejected_without_rendering(values);
    values->equality_plans = (SolMirRuntimeEqualityPlan *)(void *)values;
    check_rejected_without_rendering(values);
    values->equality_plans = (SolMirRuntimeEqualityPlan *)(void *)values->recipe_operations;
    check_rejected_without_rendering(values);
    values->equality_plans = (SolMirRuntimeEqualityPlan *)(void *)program->representation.recipes;
    check_rejected_without_rendering(values);
    values->equality_plans = (SolMirRuntimeEqualityPlan *)(uintptr_t)(UINTPTR_MAX - 1);
    check_rejected_without_rendering(values);
    values->equality_plans = equality_plans;
    SolMirRuntimeValuesUsage equality_usage = values->usage;
    --values->usage.equality_plans;
    check_rejected_without_rendering(values);
    values->usage = equality_usage;
    SolMirRuntimeHostResultPlan saved_host_plan = values->host_result_plans[0];
    values->host_result_plans[0].recipe = 1;
    check_rejected_without_rendering(values);
    values->host_result_plans[0] = saved_host_plan;
    values->host_result_plans[0].classification
        = SOL_MIR_RUNTIME_HOST_RESULT_FORBIDDEN;
    check_rejected_without_rendering(values);
    values->host_result_plans[0] = saved_host_plan;
    SolMirRuntimeHostResultPlan *host_plans = values->host_result_plans;
    values->host_result_plans = NULL;
    check_rejected_without_rendering(values);
    values->host_result_plans = (SolMirRuntimeHostResultPlan *)(void *)values;
    check_rejected_without_rendering(values);
    values->host_result_plans = (SolMirRuntimeHostResultPlan *)(void *)values->copy_plans;
    check_rejected_without_rendering(values);
    values->host_result_plans = (SolMirRuntimeHostResultPlan *)(void *)program->representation.recipes;
    check_rejected_without_rendering(values);
    values->host_result_plans = (SolMirRuntimeHostResultPlan *)(void *)conventions->imports;
    check_rejected_without_rendering(values);
    values->host_result_plans = (SolMirRuntimeHostResultPlan *)(void *)program->program.ir->source_bytes;
    check_rejected_without_rendering(values);
    values->host_result_plans = (SolMirRuntimeHostResultPlan *)(uintptr_t)(UINTPTR_MAX - 1);
    check_rejected_without_rendering(values);
    values->host_result_plans = host_plans;
    SolMirRuntimeHostResultRequirement *host_requirements
        = values->host_result_requirements;
    values->host_result_requirements = NULL;
    check_rejected_without_rendering(values);
    values->host_result_requirements
        = (SolMirRuntimeHostResultRequirement *)(void *)values->host_result_plans;
    check_rejected_without_rendering(values);
    values->host_result_requirements
        = (SolMirRuntimeHostResultRequirement *)(void *)program->representation.recipes;
    check_rejected_without_rendering(values);
    values->host_result_requirements
        = (SolMirRuntimeHostResultRequirement *)(void *)conventions->imports;
    check_rejected_without_rendering(values);
    values->host_result_requirements
        = (SolMirRuntimeHostResultRequirement *)(void *)program->program.ir->source_bytes;
    check_rejected_without_rendering(values);
    values->host_result_requirements
        = (SolMirRuntimeHostResultRequirement *)(void *)values;
    check_rejected_without_rendering(values);
    values->host_result_requirements
        = (SolMirRuntimeHostResultRequirement *)(uintptr_t)(UINTPTR_MAX - 1);
    check_rejected_without_rendering(values);
    values->host_result_requirements = host_requirements;
    if (values->host_result_requirement_count != 0) {
        SolMirRuntimeHostResultRequirement saved_host_requirement
            = values->host_result_requirements[0];
        values->host_result_requirements[0].result = SOL_MIR_RECIPE_NONE;
        check_rejected_without_rendering(values);
        values->host_result_requirements[0] = saved_host_requirement;
    }
    SolMirRuntimeValuesUsage host_usage = values->usage;
    --values->usage.host_result_plans;
    check_rejected_without_rendering(values);
    values->usage = host_usage;
    size_t saved_host_plan_count = values->host_result_plan_count;
    size_t saved_host_plan_capacity = values->host_result_plan_capacity;
    --values->host_result_plan_count;
    check_rejected_without_rendering(values);
    values->host_result_plan_count = saved_host_plan_count;
    values->host_result_plan_capacity = SIZE_MAX;
    check_rejected_without_rendering(values);
    values->host_result_plan_capacity = saved_host_plan_capacity;
    size_t saved_host_requirement_count = values->host_result_requirement_count;
    size_t saved_host_requirement_capacity = values->host_result_requirement_capacity;
    --values->host_result_requirement_count;
    check_rejected_without_rendering(values);
    values->host_result_requirement_count = saved_host_requirement_count;
    values->host_result_requirement_capacity = SIZE_MAX;
    check_rejected_without_rendering(values);
    values->host_result_requirement_capacity = saved_host_requirement_capacity;
    SolMirRuntimeOwnershipPlan saved_ownership = values->ownership_plans[0];
    values->ownership_plans[0].recipe = 1;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->ownership_plans[0] = saved_ownership;
    values->ownership_plans[0].classification
        = SOL_MIR_RUNTIME_OWNERSHIP_TEXT;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->ownership_plans[0] = saved_ownership;
    ++values->ownership_plan_count;
    ++values->ownership_plan_capacity;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    --values->ownership_plan_count;
    --values->ownership_plan_capacity;
    for (size_t i = 0; i < values->ownership_plan_count; ++i) {
        SolMirRuntimeOwnershipPlan saved_slices = values->ownership_plans[i];
        if (saved_slices.edges.count != 0) {
            ++values->ownership_plans[i].edges.offset;
            CHECK(!sol_mir_runtime_values_validate(values, NULL));
            values->ownership_plans[i] = saved_slices;
            --values->ownership_plans[i].edges.count;
            CHECK(!sol_mir_runtime_values_validate(values, NULL));
            values->ownership_plans[i] = saved_slices;
        }
        if (saved_slices.variants.count != 0) {
            ++values->ownership_plans[i].variants.offset;
            CHECK(!sol_mir_runtime_values_validate(values, NULL));
            values->ownership_plans[i] = saved_slices;
            --values->ownership_plans[i].variants.count;
            CHECK(!sol_mir_runtime_values_validate(values, NULL));
            values->ownership_plans[i] = saved_slices;
        }
    }
    if (values->ownership_variant_count != 0) {
        SolMirRuntimeOwnershipVariant saved_variant
            = values->ownership_variants[0];
        ++values->ownership_variants[0].semantic_tag;
        CHECK(!sol_mir_runtime_values_validate(values, NULL));
        values->ownership_variants[0] = saved_variant;
        ++values->ownership_variants[0].edges.offset;
        CHECK(!sol_mir_runtime_values_validate(values, NULL));
        values->ownership_variants[0] = saved_variant;
        if (saved_variant.edges.count != 0) {
            --values->ownership_variants[0].edges.count;
            CHECK(!sol_mir_runtime_values_validate(values, NULL));
            values->ownership_variants[0] = saved_variant;
        }
        --values->ownership_variant_count;
        CHECK(!sol_mir_runtime_values_validate(values, NULL));
        ++values->ownership_variant_count;
        --values->ownership_variant_capacity;
        CHECK(!sol_mir_runtime_values_validate(values, NULL));
        ++values->ownership_variant_capacity;
    }
    if (values->owned_edge_count != 0) {
        SolMirRuntimeOwnedEdge saved_edge = values->owned_edges[0];
        values->owned_edges[0].kind = SOL_MIR_RUNTIME_OWNED_EDGE_BACKING;
        CHECK(!sol_mir_runtime_values_validate(values, NULL));
        values->owned_edges[0] = saved_edge;
        values->owned_edges[0].recipe = values->ownership_plan_count;
        CHECK(!sol_mir_runtime_values_validate(values, NULL));
        values->owned_edges[0] = saved_edge;
        values->owned_edges[0].producer = 0;
        CHECK(!sol_mir_runtime_values_validate(values, NULL));
        values->owned_edges[0] = saved_edge;
        ++values->owned_edges[0].ordinal;
        CHECK(!sol_mir_runtime_values_validate(values, NULL));
        values->owned_edges[0] = saved_edge;
        --values->owned_edge_count;
        CHECK(!sol_mir_runtime_values_validate(values, NULL));
        ++values->owned_edge_count;
        --values->owned_edge_capacity;
        CHECK(!sol_mir_runtime_values_validate(values, NULL));
        ++values->owned_edge_capacity;
    }
    for (size_t i = 0; i < values->owned_edge_count; ++i) {
        if (values->owned_edges[i].kind
                != SOL_MIR_RUNTIME_OWNED_EDGE_CAPTURED_RECEIVER) continue;
        SolMirRuntimeOwnedEdge saved_edge = values->owned_edges[i];
        values->owned_edges[i].producer = values->conventions->concrete->representation
            .callable_producer_count;
        CHECK(!sol_mir_runtime_values_validate(values, NULL));
        values->owned_edges[i] = saved_edge;
        break;
    }
    SolMirRuntimeValuesUsage saved_usage = values->usage;
    --values->usage.ownership_plans;
    CHECK(!sol_mir_runtime_values_validate(values, NULL)); values->usage = saved_usage;
    --values->usage.ownership_variants;
    CHECK(!sol_mir_runtime_values_validate(values, NULL)); values->usage = saved_usage;
    --values->usage.owned_edges;
    CHECK(!sol_mir_runtime_values_validate(values, NULL)); values->usage = saved_usage;
    values->allocation_plans[0].kind
        = SOL_MIR_RUNTIME_ALLOCATION_PLAN_FIXED_OBJECT;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->allocation_plans[0] = saved_plan;
    ++values->allocation_plans[0].object_size;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->allocation_plans[0] = saved_plan;
    values->allocation_plans[0].object_alignment = 2;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    FILE *plan_stream = tmpfile();
    CHECK(plan_stream != NULL);
    if (plan_stream != NULL) {
        CHECK(!sol_mir_runtime_values_render(plan_stream, values));
        CHECK(fflush(plan_stream) == 0 && fseek(plan_stream, 0, SEEK_END) == 0
            && ftell(plan_stream) == 0);
        fclose(plan_stream);
    }
    values->allocation_plans[0] = saved_plan;
    ++values->recipe_operation_count;
    ++values->recipe_operation_capacity;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    FILE *coherent_count_stream = tmpfile();
    CHECK(coherent_count_stream != NULL);
    if (coherent_count_stream != NULL) {
        CHECK(!sol_mir_runtime_values_render(coherent_count_stream, values));
        CHECK(fflush(coherent_count_stream) == 0
            && fseek(coherent_count_stream, 0, SEEK_END) == 0
            && ftell(coherent_count_stream) == 0);
        fclose(coherent_count_stream);
    }
    --values->recipe_operation_count;
    --values->recipe_operation_capacity;
    --values->usage.build_work;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    ++values->usage.build_work;
    SolMirRuntimeRecipeOperations *records = values->recipe_operations;
    values->recipe_operations = (SolMirRuntimeRecipeOperations *)(void *)values;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->recipe_operations = (SolMirRuntimeRecipeOperations *)(void *)
        conventions->imports;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->recipe_operations = (SolMirRuntimeRecipeOperations *)(void *)
        program->representation.recipes;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->recipe_operations = (SolMirRuntimeRecipeOperations *)(void *)
        program->program.ir->source_bytes;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->recipe_operations = records;
    SolMirRuntimeAllocationPlan *plans = values->allocation_plans;
    values->allocation_plans = (SolMirRuntimeAllocationPlan *)(void *)records;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->allocation_plans = (SolMirRuntimeAllocationPlan *)(void *)values;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->allocation_plans = (SolMirRuntimeAllocationPlan *)(void *)
        program->layout.types;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->allocation_plans = (SolMirRuntimeAllocationPlan *)(uintptr_t)
        (UINTPTR_MAX - 1);
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->allocation_plans = plans;
    SolMirRuntimeOwnershipPlan *ownership_plans = values->ownership_plans;
    values->ownership_plans = (SolMirRuntimeOwnershipPlan *)(void *)records;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->ownership_plans = ownership_plans;
    SolMirRuntimeOwnershipVariant *ownership_variants = values->ownership_variants;
    values->ownership_variants = NULL;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->ownership_variants = (SolMirRuntimeOwnershipVariant *)(void *)values;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->ownership_variants = (SolMirRuntimeOwnershipVariant *)(void *)records;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->ownership_variants = ownership_variants;
    SolMirRuntimeOwnedEdge *owned_edges = values->owned_edges;
    values->owned_edges = (SolMirRuntimeOwnedEdge *)(void *)plans;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->owned_edges = owned_edges;
    values->owned_edges = (SolMirRuntimeOwnedEdge *)(void *)values;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->owned_edges = (SolMirRuntimeOwnedEdge *)(void *)program->representation.recipes;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->owned_edges = owned_edges;
    size_t saved_count = values->recipe_operation_count;
    size_t saved_capacity = values->recipe_operation_capacity;
    values->recipe_operation_count = SIZE_MAX;
    values->recipe_operation_capacity = SIZE_MAX;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    values->recipe_operation_count = saved_count;
    values->recipe_operation_capacity = saved_capacity;
    values->recipe_operations = (SolMirRuntimeRecipeOperations *)(uintptr_t)
        (UINTPTR_MAX - 1);
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    FILE *range_stream = tmpfile();
    CHECK(range_stream != NULL);
    if (range_stream != NULL) {
        CHECK(!sol_mir_runtime_values_render(range_stream, values));
        CHECK(fflush(range_stream) == 0
            && fseek(range_stream, 0, SEEK_END) == 0
            && ftell(range_stream) == 0);
        fclose(range_stream);
    }
    values->recipe_operations = records;
    SolMirRuntimeImport saved_import = conventions->imports[0];
    conventions->imports[0].recipe_operation ^= SOL_MIR_LINKAGE_RUNTIME_CREATE;
    CHECK(!sol_mir_runtime_values_validate(values, NULL));
    CHECK(sol_mir_runtime_values_build(&request, &limited, NULL)
            == SOL_MIR_RUNTIME_VALUES_BUILD_INVALID_CONVENTIONS
        && limited.conventions == NULL);
    conventions->imports[0] = saved_import;
    CHECK(sol_mir_runtime_values_validate(values, NULL));
    test_malformed_ownership_arenas(values);

    saved = values->recipe_operations[0];
    values->recipe_operations[0].recipe = 1;
    FILE *stream = tmpfile();
    CHECK(stream != NULL);
    if (stream != NULL) {
        CHECK(!sol_mir_runtime_values_render(stream, values));
        CHECK(fflush(stream) == 0 && fseek(stream, 0, SEEK_END) == 0
            && ftell(stream) == 0);
        fclose(stream);
    }
    values->recipe_operations[0] = saved;
    free(runtime_before);
    free(runtime_after);
    free(first);
}

static bool compile_text(TextCompilation *c, const char *path,
    const char *text) {
    memset(c, 0, sizeof(*c));
    sol_tokens_init(&c->tokens);
    sol_diagnostics_init(&c->diagnostics);
    sol_syntax_tree_init(&c->syntax);
    sol_hir_module_init(&c->hir);
    sol_type_table_init(&c->types);
    sol_effect_table_init(&c->effects);
    sol_contract_table_init(&c->contracts);
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

static void text_compilation_free(TextCompilation *c) {
    sol_ir_free(&c->ir);
    sol_contract_table_free(&c->contracts);
    sol_effect_table_free(&c->effects);
    sol_type_table_free(&c->types);
    sol_hir_module_free(&c->hir);
    sol_syntax_tree_free(&c->syntax);
    sol_tokens_free(&c->tokens);
    sol_source_free(&c->source);
    sol_diagnostics_free(&c->diagnostics);
}

static void test_plan_driven_trace_model(const SolMirRuntimeValues *values);
static void test_callable_trace_model(const SolMirRuntimeValues *values);
static void test_copy_transaction_model(const SolMirRuntimeValues *values);
static void test_equality_model(const SolMirRuntimeValues *values);

static void test_no_import_recipe_accounting(void) {
    static const char source[] =
        "module runtime_values_no_import\n"
        "function root(value: Int64) -> Int64 effects { pure } { return value }\n";
    TextCompilation compilation;
    SolMirConcreteProgram program;
    SolMirRuntimeConventions conventions;
    SolMirRuntimeValues values, exact, below;
    sol_mir_concrete_program_init(&program);
    sol_mir_runtime_conventions_init(&conventions);
    sol_mir_runtime_values_init(&values);
    sol_mir_runtime_values_init(&exact);
    sol_mir_runtime_values_init(&below);
    CHECK(compile_text(&compilation, "/checkout/runtime_values_no_import.sol", source));
    SolIrCallableId root = callable(&compilation.ir, "root", SOL_IR_CALLABLE_FUNCTION);
    SolMirProgramRoot request_root = {root, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    CHECK(root != SOL_IR_NONE && sol_mir_concrete_program_build(
        &(SolMirConcreteBuildRequest){&compilation.ir, &request_root, 1, NULL, 0, &target, NULL},
        &program, &compilation.diagnostics) == SOL_MIR_CONCRETE_BUILD_SUCCEEDED);
    CHECK(build_values(&program, &compilation.diagnostics, &conventions, &values));
    CHECK(conventions.import_count == 0 && values.recipe_operation_count != 0);
    CHECK(values.usage.build_work == 23);
    SolMirRuntimeValuesUsage reconstructed;
    CHECK(sol_mir_runtime_values_test_reconstruct_usage(&conventions, &values.limits,
        &reconstructed) && reconstructed.build_work == 23
        && memcmp(&reconstructed, &values.usage, sizeof reconstructed) == 0);
    SolMirRuntimeValuesLimits limits = sol_mir_runtime_values_default_limits();
    limits.max_build_work = 23;
    SolMirRuntimeValuesBuildOutcome exact_outcome = sol_mir_runtime_values_build(
        &(SolMirRuntimeValuesBuildRequest){&conventions, &limits}, &exact, NULL);
    CHECK(exact_outcome == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED);
    limits.max_build_work = 22;
    SolMirRuntimeValuesBuildOutcome below_outcome = sol_mir_runtime_values_build(
        &(SolMirRuntimeValuesBuildRequest){&conventions, &limits}, &below, NULL);
    CHECK(below_outcome == SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED
        && below.conventions == NULL && below.recipe_operations == NULL
        && below.recipe_operation_count == 0);
    sol_mir_runtime_values_free(&below); sol_mir_runtime_values_free(&exact);
    sol_mir_runtime_values_free(&values); sol_mir_runtime_conventions_free(&conventions);
    sol_mir_concrete_program_free(&program); text_compilation_free(&compilation);
}

static void test_bound_environment_exclusion(void) {
    static const char source[] =
        "module runtime_values_bound_environment\n"
        "capability Base { function choose(value: Int64) -> Bool effects { pure } }\n"
        "function callback(value: Int64) -> Bool effects { pure } { return true }\n"
        "function root(base: capability Base) -> Bool effects { pure } "
        "requires { { let exact = callback let bound = base.choose "
        "let rebound = base.choose true } } "
        "{ return base.choose(1) }\n";
    const char *paths[] = {"/checkout/a/runtime_values.sol",
        "/relocated/b/runtime_values.sol"};
    TextCompilation compilations[2];
    SolMirConcreteProgram programs[2];
    SolMirRuntimeConventions conventions[2];
    SolMirRuntimeValues values[2];
    char *rendered[2] = {NULL, NULL};
    for (size_t side = 0; side < 2; ++side) {
        sol_mir_concrete_program_init(&programs[side]);
        sol_mir_runtime_conventions_init(&conventions[side]);
        sol_mir_runtime_values_init(&values[side]);
    }
    for (size_t side = 0; side < 2; ++side) {
        TextCompilation *c = &compilations[side];
        CHECK(compile_text(c, paths[side], source));
        SolIrCallableId root = callable(&c->ir, "root",
            SOL_IR_CALLABLE_FUNCTION);
        SolIrCallableId choose = callable(&c->ir, "choose",
            SOL_IR_CALLABLE_CAPABILITY);
        SolMirProgramRoot root_request = {root,
            SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
        SolMirTargetDescriptor target = sol_mir_target_wasm32();
        SolMirConcreteBuildRequest request = {&c->ir, &root_request, 1,
            &choose, 1, &target, NULL};
        bool built = sol_mir_concrete_program_build(&request, &programs[side],
            &c->diagnostics) == SOL_MIR_CONCRETE_BUILD_SUCCEEDED;
        CHECK(built);
        CHECK(built && build_values(&programs[side], &c->diagnostics,
            &conventions[side], &values[side]));
        bool bound = false;
        for (size_t i = 0;
                i < programs[side].linkage.runtime_requirement_count; ++i)
            bound |= (programs[side].linkage.runtime_requirements[i].operations
                & SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT) != 0;
        CHECK(bound);
        /* Callable construction is a P2-plan request: every callable pays its
           target-token header, while a bound callable additionally pays its
           positive-size captured environment.  The rejected preflight is
           transactional and leaves its output untouched. */
        size_t unbound_plan = SOL_MIR_RUNTIME_NONE, bound_plan = SOL_MIR_RUNTIME_NONE;
        for (size_t plan = 0; plan < programs[side].operations.callable_count; ++plan) {
            const SolMirOperationCallablePlan *callable_plan
                = &programs[side].operations.callables[plan];
            if (callable_plan->kind == SOL_MIR_CALLABLE_PRODUCER_EXACT_FUNCTION)
                unbound_plan = plan;
            else if (callable_plan->kind == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION)
                bound_plan = plan;
        }
        CHECK(unbound_plan != SOL_MIR_RUNTIME_NONE && bound_plan != SOL_MIR_RUNTIME_NONE);
        for (size_t pass = 0; pass < 2; ++pass) {
            size_t plan = pass == 0 ? unbound_plan : bound_plan;
            if (plan == SOL_MIR_RUNTIME_NONE) continue;
            const SolMirOperationCallablePlan *callable_plan
                = &programs[side].operations.callables[plan];
            uint64_t environment = callable_plan->kind
                    == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION
                ? programs[side].layout.types[callable_plan->capture_recipe].object_size : 0;
            SolMirRuntimeAllocationDemand callable_demand = {UINT64_MAX, UINT64_MAX};
            SolMirRuntimeAllocationRequest callable_request = {callable_plan->function_recipe,
                0, plan, SOL_MIR_RUNTIME_ALLOCATION_REQUEST_CALLABLE};
            SolMirRuntimeAllocationQuota exact = {1 + (environment != 0),
                programs[side].layout.target.pointer_size + environment};
            CHECK(sol_mir_runtime_values_check_allocation(&values[side], &callable_request,
                    &exact, &(SolMirRuntimeAllocationUsage){0}, &callable_demand)
                    == SOL_MIR_RUNTIME_ALLOCATION_SUCCEEDED
                && callable_demand.requests == exact.max_requests
                && callable_demand.bytes == exact.max_bytes);
            SolMirRuntimeAllocationDemand unchanged = {91, 92};
            SolMirRuntimeAllocationQuota below = exact;
            --below.max_requests;
            CHECK(sol_mir_runtime_values_check_allocation(&values[side], &callable_request,
                    &below, &(SolMirRuntimeAllocationUsage){0}, &unchanged)
                    == SOL_MIR_RUNTIME_ALLOCATION_LIMIT
                && unchanged.requests == 91 && unchanged.bytes == 92);
            below = exact;
            --below.max_bytes;
            CHECK(sol_mir_runtime_values_check_allocation(&values[side], &callable_request,
                    &below, &(SolMirRuntimeAllocationUsage){0}, &unchanged)
                    == SOL_MIR_RUNTIME_ALLOCATION_LIMIT
                && unchanged.requests == 91 && unchanged.bytes == 92);
        }
        size_t none_recipe = SOL_MIR_RECIPE_NONE;
        for (size_t i = 0; i < values[side].allocation_plan_count; ++i)
            if (values[side].allocation_plans[i].kind
                    == SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE) {
                none_recipe = i; break;
            }
        SolMirRuntimeAllocationDemand allocation_demand = {0};
        SolMirRuntimeAllocationQuota allocation_quota = {0, 0};
        SolMirRuntimeAllocationUsage allocation_usage = {0, 0};
        size_t scans = sol_mir_runtime_values_test_ownership_count_scans();
        SolMirRuntimeAllocationRequest allocation_request = {none_recipe, 0,
            SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_ALLOCATION_REQUEST_ORDINARY};
        CHECK(none_recipe != SOL_MIR_RECIPE_NONE
            && sol_mir_runtime_values_check_allocation(&values[side],
                &allocation_request, &allocation_quota, &allocation_usage,
                &allocation_demand) == SOL_MIR_RUNTIME_ALLOCATION_SUCCEEDED
            && sol_mir_runtime_values_test_ownership_count_scans() == scans);
        /* Mode is explicit for new callers, while zero-initialized legacy
         * requests remain ordinary and may carry the inactive plan-zero. */
        SolMirRuntimeAllocationDemand unchanged = {71, 73};
        SolMirRuntimeAllocationRequest legacy = {.recipe = none_recipe,
            .text_length = 0, .callable_plan = 0};
        CHECK(sol_mir_runtime_values_check_allocation(&values[side], &legacy,
                &allocation_quota, &allocation_usage, &allocation_demand)
                == SOL_MIR_RUNTIME_ALLOCATION_SUCCEEDED);
        SolMirRuntimeAllocationRequest invalid = {none_recipe, 0,
            SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_ALLOCATION_REQUEST_CALLABLE};
        CHECK(sol_mir_runtime_values_check_allocation(&values[side], &invalid,
                &allocation_quota, &allocation_usage, &unchanged)
                == SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT
            && unchanged.requests == 71 && unchanged.bytes == 73);
        invalid.callable_plan = programs[side].operations.callable_count;
        CHECK(sol_mir_runtime_values_check_allocation(&values[side], &invalid,
                &allocation_quota, &allocation_usage, &unchanged)
                == SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT
            && unchanged.requests == 71 && unchanged.bytes == 73);
        invalid = (SolMirRuntimeAllocationRequest){none_recipe, 0,
            programs[side].operations.callable_count,
            SOL_MIR_RUNTIME_ALLOCATION_REQUEST_ORDINARY};
        CHECK(sol_mir_runtime_values_check_allocation(&values[side], &invalid,
                &allocation_quota, &allocation_usage, &unchanged)
                == SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT
            && unchanged.requests == 71 && unchanged.bytes == 73);
        invalid.mode = (SolMirRuntimeAllocationRequestMode)2;
        invalid.callable_plan = SOL_MIR_RUNTIME_NONE;
        CHECK(sol_mir_runtime_values_check_allocation(&values[side], &invalid,
                &allocation_quota, &allocation_usage, &unchanged)
                == SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT
            && unchanged.requests == 71 && unchanged.bytes == 73);
        if (unbound_plan != SOL_MIR_RUNTIME_NONE && bound_plan != SOL_MIR_RUNTIME_NONE) {
            const SolMirOperationCallablePlan *unbound
                = &programs[side].operations.callables[unbound_plan];
            invalid = (SolMirRuntimeAllocationRequest){unbound->function_recipe, 0,
                bound_plan, SOL_MIR_RUNTIME_ALLOCATION_REQUEST_CALLABLE};
            CHECK(sol_mir_runtime_values_check_allocation(&values[side], &invalid,
                    &allocation_quota, &allocation_usage, &unchanged)
                    == SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT
                && unchanged.requests == 71 && unchanged.bytes == 73);
        }
        if (programs[side].operations.callable_count != 0) {
            const SolMirOperationCallablePlan *zero_plan
                = &programs[side].operations.callables[0];
            uint64_t environment = zero_plan->kind
                    == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION
                ? programs[side].layout.types[zero_plan->capture_recipe].object_size : 0;
            SolMirRuntimeAllocationRequest plan_zero = {zero_plan->function_recipe,
                0, 0, SOL_MIR_RUNTIME_ALLOCATION_REQUEST_CALLABLE};
            SolMirRuntimeAllocationQuota quota = {1 + (environment != 0),
                programs[side].layout.target.pointer_size + environment};
            CHECK(sol_mir_runtime_values_check_allocation(&values[side], &plan_zero,
                    &quota, &allocation_usage, &allocation_demand)
                    == SOL_MIR_RUNTIME_ALLOCATION_SUCCEEDED);
            if (none_recipe != zero_plan->function_recipe) {
                invalid = (SolMirRuntimeAllocationRequest){none_recipe, 0, 0,
                    SOL_MIR_RUNTIME_ALLOCATION_REQUEST_CALLABLE};
                CHECK(sol_mir_runtime_values_check_allocation(&values[side], &invalid,
                        &allocation_quota, &allocation_usage, &unchanged)
                        == SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT
                    && unchanged.requests == 71 && unchanged.bytes == 73);
            }
        }
        for (size_t recipe = 0; recipe < values[side].allocation_plan_count; ++recipe) {
            if (values[side].allocation_plans[recipe].kind
                    != SOL_MIR_RUNTIME_ALLOCATION_PLAN_TEXT) continue;
            invalid = (SolMirRuntimeAllocationRequest){recipe, 0, 0,
                SOL_MIR_RUNTIME_ALLOCATION_REQUEST_CALLABLE};
            CHECK(sol_mir_runtime_values_check_allocation(&values[side], &invalid,
                    &allocation_quota, &allocation_usage, &unchanged)
                    == SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT
                && unchanged.requests == 71 && unchanged.bytes == 73);
            break;
        }
        for (size_t i = 0; i < values[side].recipe_operation_count; ++i)
            CHECK((values[side].recipe_operations[i].demanded_operations
                & SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT) == 0);
        size_t captured_receiver = 0;
        SolMirRecipeId captured_owner = SOL_MIR_RECIPE_NONE;
        size_t first_producer = SOL_MIR_RUNTIME_NONE;
        for (size_t i = 0; i < values[side].ownership_plan_count; ++i) {
            const SolMirRuntimeOwnershipPlan *plan
                = &values[side].ownership_plans[i];
            for (size_t edge = 0; edge < plan->edges.count; ++edge) {
                const SolMirRuntimeOwnedEdge *item = &values[side].owned_edges[
                    plan->edges.offset + edge];
                if (item->kind == SOL_MIR_RUNTIME_OWNED_EDGE_CAPTURED_RECEIVER) {
                    ++captured_receiver;
                    if (captured_owner == SOL_MIR_RECIPE_NONE) captured_owner = i;
                    CHECK(captured_owner == i);
                    if (first_producer == SOL_MIR_RUNTIME_NONE)
                        first_producer = item->producer;
                    else CHECK(item->producer != first_producer);
                    CHECK(item->producer < programs[side].representation
                        .callable_producer_count
                        && programs[side].representation.callable_producers[
                            item->producer].kind
                            == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION);
                }
            }
        }
        CHECK(captured_receiver >= 2 && captured_owner != SOL_MIR_RECIPE_NONE);
        CHECK(sol_mir_runtime_values_validate(&values[side], NULL));
        test_callable_trace_model(&values[side]);
        check_plan_layout_classification(&values[side]);
        rendered[side] = render_values(&values[side]);
        CHECK(rendered[side] != NULL);
        if (side == 0 && rendered[side] != NULL) {
            const char *prefix = "edge=captured-receiver:0:";
            const char *capture_a = strstr(rendered[side], prefix);
            const char *capture_b = capture_a == NULL ? NULL
                : strstr(capture_a + strlen(prefix), prefix);
            CHECK(capture_a != NULL && capture_b != NULL
                && strncmp(capture_a + strlen(prefix),
                    capture_b + strlen(prefix),
                    SOL_MIR_LINKAGE_DIGEST_BYTES * 2) != 0);
            sol_mir_runtime_values_test_reverse_captured_fragments(true);
            char *reversed = render_values(&values[side]);
            sol_mir_runtime_values_test_reverse_captured_fragments(false);
            CHECK(reversed != NULL && strcmp(rendered[side], reversed) == 0);
            free(reversed);
            sol_mir_runtime_values_test_force_captured_digest_collision(true);
            FILE *collision = tmpfile();
            CHECK(collision != NULL);
            if (collision != NULL) {
                CHECK(!sol_mir_runtime_values_render(collision, &values[side]));
                CHECK(fflush(collision) == 0 && fseek(collision, 0, SEEK_END) == 0
                    && ftell(collision) == 0);
                fclose(collision);
            }
            sol_mir_runtime_values_test_force_captured_digest_collision(false);
        }
    }
    CHECK(rendered[0] != NULL && rendered[1] != NULL
        && strcmp(rendered[0], rendered[1]) == 0);
    for (size_t side = 0; side < 2; ++side) {
        free(rendered[side]);
        sol_mir_runtime_values_free(&values[side]);
        sol_mir_runtime_conventions_free(&conventions[side]);
        sol_mir_concrete_program_free(&programs[side]);
        text_compilation_free(&compilations[side]);
    }
}

static void test_host_result_result_fixture(void) {
    static const char source[] =
        "module runtime_values_host_result\n"
        "capability Host { function ask() -> Result<Option<Text>, Text> effects { pure } }\n"
        "function root(host: capability Host) -> Result<Option<Text>, Text> effects { pure } "
        "{ return host.ask() }\n";
    TextCompilation compilation;
    SolMirConcreteProgram program;
    SolMirRuntimeConventions conventions;
    SolMirRuntimeValues values;
    sol_mir_concrete_program_init(&program);
    sol_mir_runtime_conventions_init(&conventions);
    sol_mir_runtime_values_init(&values);
    CHECK(compile_text(&compilation, "/fixture/host-result.sol", source));
    SolIrCallableId root = callable(&compilation.ir, "root", SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId ask = callable(&compilation.ir, "ask", SOL_IR_CALLABLE_CAPABILITY);
    SolMirProgramRoot root_request = {root, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    SolMirConcreteBuildRequest request = {&compilation.ir, &root_request, 1,
        &ask, 1, &target, NULL};
    bool built = root != SOL_IR_NONE && ask != SOL_IR_NONE
        && sol_mir_concrete_program_build(&request, &program, &compilation.diagnostics)
            == SOL_MIR_CONCRETE_BUILD_SUCCEEDED;
    CHECK(built && build_values(&program, &compilation.diagnostics, &conventions, &values));
    SolMirRecipeId result = host_result_recipe(&values,
        SOL_MIR_RUNTIME_HOST_RESULT_RESULT);
    if (result != SOL_MIR_RECIPE_NONE) {
        const SolMirRepresentation *r = &program.representation;
        const SolMirRecipe *result_recipe = &r->recipes[result];
        const SolMirRecipeVariant *result_variant = NULL;
        SolMirRecipeId option = SOL_MIR_RECIPE_NONE;
        for (size_t i = 0; i < result_recipe->variants.count; ++i) {
            const SolMirRecipeVariant *candidate = &r->variants[result_recipe->variants.offset + i];
            if (candidate->fields.count == 1 && values.host_result_plans[
                    r->fields[candidate->fields.offset].type].classification
                    == SOL_MIR_RUNTIME_HOST_RESULT_OPTION) {
                result_variant = candidate;
                option = r->fields[candidate->fields.offset].type;
            }
        }
        CHECK(result_variant != NULL && option != SOL_MIR_RECIPE_NONE);
        if (result_variant != NULL) {
            const SolMirRecipe *option_recipe = &r->recipes[option];
            const SolMirRecipeVariant *some = NULL, *none = NULL;
            for (size_t i = 0; i < option_recipe->variants.count; ++i) {
                const SolMirRecipeVariant *candidate = &r->variants[option_recipe->variants.offset + i];
                if (candidate->fields.count == 1) some = candidate;
                else if (candidate->fields.count == 0) none = candidate;
            }
            CHECK(some != NULL && none != NULL);
            if (some != NULL && none != NULL) {
                uint8_t bytes[] = {'n', '\0', 'e', 's', 't'};
                SolMirRuntimeHostValue text = {.kind = SOL_MIR_RUNTIME_HOST_VALUE_TEXT,
                    .as.text = {bytes, sizeof(bytes)}};
                SolMirRuntimeHostValue option_some = {.kind = SOL_MIR_RUNTIME_HOST_VALUE_OPTION,
                    .as.sum = {some->ordinal, &text}};
                SolMirRuntimeHostValue nested = {.kind = SOL_MIR_RUNTIME_HOST_VALUE_RESULT,
                    .as.sum = {result_variant->ordinal, &option_some}};
                const SolMirRuntimeAllocationPlan *result_plan = &values.allocation_plans[result];
                const SolMirRuntimeAllocationPlan *option_plan = &values.allocation_plans[option];
                SolMirRecipeId text_recipe = r->fields[some->fields.offset].type;
                const SolMirRuntimeAllocationPlan *text_plan = &values.allocation_plans[text_recipe];
                uint64_t expected_bytes = result_plan->object_size + option_plan->object_size
                    + text_plan->object_size + sizeof(bytes);
                SolMirRuntimeAllocationQuota exact = {4, expected_bytes};
                /* Result -> Option -> Text visits three nodes in each of the
                   shape and demand passes.  Exact limits succeed; one below
                   each independently rejects before staging. */
                const SolMirRuntimeHostTransferLimits exact_limits
                    = {.max_depth = 3, .max_nodes = 3, .max_work = 6};
                SolMirRuntimeAllocationUsage usage = {0};
                SolMirRuntimeHostOwnedValue *owned = NULL;
                SolMirRuntimeHostTransferRequest transfer = {&values, result, &nested,
                    &exact, &usage, &exact_limits};
                /* Representable malformed Option/Result views are rejected
                   during shape preflight, before the quota is consulted. */
                SolMirRuntimeHostValue wrong_leaf = {.kind = SOL_MIR_RUNTIME_HOST_VALUE_BOOL,
                    .as.bool_value = true};
                option_some.as.sum.payload = &wrong_leaf;
                transfer.limits = NULL;
                CHECK(sol_mir_runtime_values_test_transfer_host_result(&transfer, &owned)
                        == SOL_MIR_RUNTIME_HOST_TRANSFER_INVALID && owned == NULL
                    && usage.requests == 0 && usage.bytes == 0);
                option_some.as.sum.payload = &text;
                option_some.as.sum.ordinal = SIZE_MAX;
                CHECK(sol_mir_runtime_values_test_transfer_host_result(&transfer, &owned)
                        == SOL_MIR_RUNTIME_HOST_TRANSFER_INVALID && owned == NULL);
                option_some.as.sum.ordinal = some->ordinal;
                nested.as.sum.payload = NULL;
                CHECK(sol_mir_runtime_values_test_transfer_host_result(&transfer, &owned)
                        == SOL_MIR_RUNTIME_HOST_TRANSFER_INVALID && owned == NULL);
                nested.as.sum.payload = &option_some;
                CHECK(sol_mir_runtime_values_test_transfer_host_result(&transfer, &owned)
                        == SOL_MIR_RUNTIME_HOST_TRANSFER_SUCCEEDED && owned != NULL);
                sol_mir_runtime_host_owned_value_free(owned); owned = NULL;
                usage = (SolMirRuntimeAllocationUsage){0};
                transfer.limits = &exact_limits;
                const SolMirRuntimeHostTransferLimits below_depth = {.max_depth = 2};
                const SolMirRuntimeHostTransferLimits below_nodes = {.max_nodes = 2};
                const SolMirRuntimeHostTransferLimits below_work = {.max_work = 5};
                transfer.limits = &below_depth;
                CHECK(sol_mir_runtime_values_test_transfer_host_result(&transfer, &owned)
                    == SOL_MIR_RUNTIME_HOST_TRANSFER_LIMIT && owned == NULL);
                transfer.limits = &below_nodes;
                CHECK(sol_mir_runtime_values_test_transfer_host_result(&transfer, &owned)
                    == SOL_MIR_RUNTIME_HOST_TRANSFER_LIMIT && owned == NULL);
                transfer.limits = &below_work;
                CHECK(sol_mir_runtime_values_test_transfer_host_result(&transfer, &owned)
                    == SOL_MIR_RUNTIME_HOST_TRANSFER_LIMIT && owned == NULL);
                usage = (SolMirRuntimeAllocationUsage){0};
                transfer.limits = NULL;
                SolMirRuntimeAllocationQuota request_below = {3, expected_bytes};
                transfer.quota = &request_below;
                CHECK(sol_mir_runtime_values_test_transfer_host_result(&transfer, &owned)
                    == SOL_MIR_RUNTIME_HOST_TRANSFER_ALLOCATION_LIMIT && owned == NULL
                    && usage.requests == 0 && usage.bytes == 0);
                SolMirRuntimeAllocationQuota byte_below = {4, expected_bytes - 1};
                transfer.quota = &byte_below;
                CHECK(sol_mir_runtime_values_test_transfer_host_result(&transfer, &owned)
                    == SOL_MIR_RUNTIME_HOST_TRANSFER_ALLOCATION_LIMIT && owned == NULL
                    && usage.requests == 0 && usage.bytes == 0);
                for (size_t refusal = 1; refusal <= 4; ++refusal) {
                    usage = (SolMirRuntimeAllocationUsage){0};
                    owned = NULL;
                    SolMirRuntimeHostTransferLimits limits = {.fail_allocation_attempt = refusal};
                    transfer = (SolMirRuntimeHostTransferRequest){&values, result, &nested,
                        &exact, &usage, &limits};
                    CHECK(sol_mir_runtime_values_test_transfer_host_result(&transfer, &owned)
                            == SOL_MIR_RUNTIME_HOST_TRANSFER_ALLOCATION_FAILED
                        && owned == NULL && usage.requests == refusal - 1);
                    uint64_t prefix[] = {0, result_plan->object_size,
                        result_plan->object_size + option_plan->object_size,
                        result_plan->object_size + option_plan->object_size + text_plan->object_size};
                    CHECK(usage.bytes == prefix[refusal - 1]);
                }
                usage = (SolMirRuntimeAllocationUsage){0};
                owned = NULL;
                transfer = (SolMirRuntimeHostTransferRequest){&values, result, &nested,
                    &exact, &usage, NULL};
                CHECK(sol_mir_runtime_values_test_transfer_host_result(&transfer, &owned)
                        == SOL_MIR_RUNTIME_HOST_TRANSFER_SUCCEEDED && owned != NULL
                    && usage.requests == 4 && usage.bytes == expected_bytes
                    && owned->as.sum.payload != NULL
                    && owned->as.sum.payload->as.sum.payload != NULL
                    && owned->as.sum.payload->as.sum.payload->as.text.bytes != bytes
                    && memcmp(owned->as.sum.payload->as.sum.payload->as.text.bytes,
                        bytes, sizeof(bytes)) == 0);
                bytes[0] = 'X';
                CHECK(owned->as.sum.payload->as.sum.payload->as.text.bytes[0] == 'n');
                sol_mir_runtime_host_owned_value_free(owned);
                owned = NULL;

                /* Shape validation wins over quota: the malformed selected leaf
                   is still observed under an intentionally impossible quota. */
                text.as.text.bytes = NULL;
                SolMirRuntimeAllocationQuota one_below = {3, expected_bytes - 1};
                usage = (SolMirRuntimeAllocationUsage){0};
                transfer.quota = &one_below;
                transfer.limits = NULL;
                CHECK(sol_mir_runtime_values_test_transfer_host_result(&transfer, &owned)
                        == SOL_MIR_RUNTIME_HOST_TRANSFER_INVALID && owned == NULL
                    && usage.requests == 0 && usage.bytes == 0);
                text.as.text.bytes = bytes;

                /* None is a selected zero-payload Option case. */
                SolMirRuntimeHostValue option_none = {.kind = SOL_MIR_RUNTIME_HOST_VALUE_OPTION,
                    .as.sum = {none->ordinal, NULL}};
                nested.as.sum.payload = &option_none;
                SolMirRuntimeAllocationQuota none_quota = {2,
                    result_plan->object_size + option_plan->object_size};
                usage = (SolMirRuntimeAllocationUsage){0};
                transfer.quota = &none_quota;
                CHECK(sol_mir_runtime_values_test_transfer_host_result(&transfer, &owned)
                        == SOL_MIR_RUNTIME_HOST_TRANSFER_SUCCEEDED && owned != NULL
                    && usage.requests == 2);
                sol_mir_runtime_host_owned_value_free(owned);
                owned = NULL;

                for (size_t i = 0; i < result_recipe->variants.count; ++i) {
                    const SolMirRecipeVariant *other = &r->variants[
                        result_recipe->variants.offset + i];
                    if (other == result_variant || other->fields.count != 1) continue;
                    SolMirRecipeId other_recipe = r->fields[other->fields.offset].type;
                    if (values.host_result_plans[other_recipe].classification
                            != SOL_MIR_RUNTIME_HOST_RESULT_TEXT) continue;
                    SolMirRuntimeHostValue other_text = {.kind = SOL_MIR_RUNTIME_HOST_VALUE_TEXT,
                        .as.text = {bytes, sizeof(bytes)}};
                    nested = (SolMirRuntimeHostValue){.kind = SOL_MIR_RUNTIME_HOST_VALUE_RESULT,
                        .as.sum = {other->ordinal, &other_text}};
                    const SolMirRuntimeAllocationPlan *other_plan
                        = &values.allocation_plans[other_recipe];
                    SolMirRuntimeAllocationQuota other_quota = {3,
                        result_plan->object_size + other_plan->object_size + sizeof(bytes)};
                    usage = (SolMirRuntimeAllocationUsage){0};
                    transfer = (SolMirRuntimeHostTransferRequest){&values, result, &nested,
                        &other_quota, &usage, NULL};
                    CHECK(sol_mir_runtime_values_test_transfer_host_result(&transfer, &owned)
                            == SOL_MIR_RUNTIME_HOST_TRANSFER_SUCCEEDED && owned != NULL
                        && usage.requests == 3);
                    sol_mir_runtime_host_owned_value_free(owned); owned = NULL;
                }
            }
        }
    }
    sol_mir_runtime_values_free(&values);
    sol_mir_runtime_conventions_free(&conventions);
    sol_mir_concrete_program_free(&program);
    text_compilation_free(&compilation);
}
static void test_forbidden_host_result_requirement(void) {
    static const char source[] =
        "module runtime_values_forbidden_host_result\n"
        "record Pair { left: Int64, right: Int64 }\n"
        "capability Host { function ask() -> Pair effects { pure } }\n"
        "function root(host: capability Host) -> Pair effects { pure } { return host.ask() }\n";
    TextCompilation compilation;
    SolMirConcreteProgram program;
    SolMirRuntimeConventions conventions;
    SolMirRuntimeValues values;
    sol_mir_concrete_program_init(&program);
    sol_mir_runtime_conventions_init(&conventions);
    sol_mir_runtime_values_init(&values);
    CHECK(compile_text(&compilation, "/fixture/forbidden-host-result.sol", source));
    SolIrCallableId root = callable(&compilation.ir, "root", SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId ask = callable(&compilation.ir, "ask", SOL_IR_CALLABLE_CAPABILITY);
    SolMirProgramRoot root_request = {root, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    SolMirConcreteBuildRequest concrete_request = {&compilation.ir, &root_request, 1,
        &ask, 1, &target, NULL};
    bool built = root != SOL_IR_NONE && ask != SOL_IR_NONE
        && sol_mir_concrete_program_build(&concrete_request, &program,
            &compilation.diagnostics) == SOL_MIR_CONCRETE_BUILD_SUCCEEDED;
    SolMirRuntimeConventionsBuildRequest conventions_request = {&program, NULL};
    SolMirRuntimeValuesBuildRequest values_request = {&conventions, NULL};
    CHECK(built && sol_mir_runtime_conventions_build(&conventions_request,
            &conventions, &compilation.diagnostics)
            == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        && sol_mir_runtime_values_build(&values_request, &values,
            &compilation.diagnostics) == SOL_MIR_RUNTIME_VALUES_BUILD_UNSUPPORTED
        && values.conventions == NULL);
    sol_mir_runtime_values_free(&values);
    sol_mir_runtime_conventions_free(&conventions);
    sol_mir_concrete_program_free(&program);
    text_compilation_free(&compilation);
}

static void test_plan_classification_fixture(void) {
    static const char source[] =
        "module runtime_values_plan_classes\n"
        "record Empty {}\n"
        "enum Void {}\n"
        "record Pair { first: Text, second: Text }\n"
        "record Eight { a: Text, b: Text, c: Text, d: Text, e: Text, f: Text, g: Text, h: Text }\n"
        "enum Choice { left(first: Text, second: Text), right(value: Pair) }\n"
        "type ScalarWrap = distinct Int64\n"
        "type TextWrap = distinct Text\n"
        "type AggregateWrap = distinct Pair\n"
        "capability Gate { function choose(value: Int64) -> Bool effects { pure } }\n"
        "record Locked { gate: capability Gate }\n"
        "type LockedWrap = distinct Locked\n"
        "enum Mixed { clean(value: Text), blocked(gate: capability Gate) }\n"
        "enum List { nil, cons(next: List) }\n"
        "enum Tree { leaf, branch(left: Tree, right: Tree) }\n"
        "record Zeros { first: (), second: () }\n"
        "capability DerivedGate derives_from source: capability Gate { "
        "function choose(value: Int64) -> Bool effects { pure } { return true } }\n"
        "function callback(value: Int64) -> Bool effects { pure } { return true }\n"
        "function root(scalar: ScalarWrap, text: TextWrap, aggregate: AggregateWrap, "
        "empty: Empty, unit: (), impossible: Void, pair: Pair, eight: Eight, choice: Choice, locked: Locked, locked_wrap: LockedWrap, mixed: Mixed, list: List, tree: Tree, zeros: Zeros, gate: capability Gate, "
        "derived: capability DerivedGate, "
        "callback: function(Int64) -> Bool effects { pure }) -> Bool effects { pure } "
        "{ return true }\n";
    TextCompilation compilation;
    SolMirConcreteProgram program;
    SolMirRuntimeConventions conventions;
    SolMirRuntimeValues values;
    sol_mir_concrete_program_init(&program);
    sol_mir_runtime_conventions_init(&conventions);
    sol_mir_runtime_values_init(&values);
    CHECK(compile_text(&compilation, "/fixture/runtime_values_plan_classes.sol",
        source));
    SolIrCallableId root = callable(&compilation.ir, "root",
        SOL_IR_CALLABLE_FUNCTION);
    SolMirProgramRoot root_request = {root,
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    SolMirConcreteBuildRequest request = {&compilation.ir, &root_request, 1,
        NULL, 0, &target, NULL};
    bool built = root != SOL_IR_NONE
        && sol_mir_concrete_program_build(&request, &program,
            &compilation.diagnostics) == SOL_MIR_CONCRETE_BUILD_SUCCEEDED;
    CHECK(built);
    CHECK(built && build_values(&program, &compilation.diagnostics, &conventions,
        &values));
    if (values.conventions != NULL) {
        check_plan_layout_classification(&values);
        check_ownership_plans(&values);
        bool empty = false, uninhabited = false, scalar_wrapper = false;
        bool text_wrapper = false, aggregate_wrapper = false;
        bool callable_layout = false, capability_layout = false;
        bool root_capability = false, derived_capability = false;
        bool locked = false, locked_wrapper = false, mixed = false, recursive = false;
        bool non_demanded_eligible = false;
        for (size_t i = 0; i < program.representation.recipe_count; ++i) {
            const SolMirRecipe *recipe = &program.representation.recipes[i];
            const SolMirRuntimeAllocationPlan *plan = &values.allocation_plans[i];
            const SolMirRuntimeEqualityPlan *equality = &values.equality_plans[i];
            const SolMirRuntimeHostResultPlan *host = &values.host_result_plans[i];
            CHECK(equality->recipe == i);
            CHECK(host->recipe == i);
            if (recipe->kind == SOL_MIR_RECIPE_INT64)
                CHECK(host->classification == SOL_MIR_RUNTIME_HOST_RESULT_INT64);
            if (recipe->kind == SOL_MIR_RECIPE_BOOL)
                CHECK(host->classification == SOL_MIR_RUNTIME_HOST_RESULT_BOOL);
            if (recipe->kind == SOL_MIR_RECIPE_TEXT)
                CHECK(host->classification == SOL_MIR_RUNTIME_HOST_RESULT_TEXT);
            if (recipe->kind == SOL_MIR_RECIPE_UNIT)
                CHECK(host->classification == SOL_MIR_RUNTIME_HOST_RESULT_UNIT);
            if (recipe->kind == SOL_MIR_RECIPE_RECORD
                && recipe->fields.count == 0 && recipe->inhabited) {
                empty = true;
                CHECK(plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE);
                CHECK(equality->classification == SOL_MIR_RUNTIME_EQUALITY_PRODUCT);
            }
            if (recipe->kind == SOL_MIR_RECIPE_ENUM
                && recipe->variants.count == 0) {
                uninhabited = true;
                CHECK(plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE);
                CHECK(equality->classification == SOL_MIR_RUNTIME_EQUALITY_UNREACHABLE);
            }
            if (recipe->kind == SOL_MIR_RECIPE_DISTINCT
                && recipe->backing < program.representation.recipe_count) {
                SolMirRecipeKind backing
                    = program.representation.recipes[recipe->backing].kind;
                if (backing == SOL_MIR_RECIPE_INT64) {
                    scalar_wrapper = true;
                    CHECK(plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE);
                    CHECK(equality->classification == SOL_MIR_RUNTIME_EQUALITY_WRAPPER);
                } else if (backing == SOL_MIR_RECIPE_TEXT) {
                    text_wrapper = true;
                    CHECK(plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_TEXT);
                    CHECK(equality->classification == SOL_MIR_RUNTIME_EQUALITY_WRAPPER);
                } else if (backing == SOL_MIR_RECIPE_RECORD) {
                    aggregate_wrapper |= equality->classification
                        == SOL_MIR_RUNTIME_EQUALITY_WRAPPER;
                    CHECK(plan->kind
                        == SOL_MIR_RUNTIME_ALLOCATION_PLAN_FIXED_OBJECT);
                }
            }
            if (recipe->kind == SOL_MIR_RECIPE_FUNCTION) {
                callable_layout = true;
                CHECK(plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE);
                CHECK(equality->classification == SOL_MIR_RUNTIME_EQUALITY_FORBIDDEN);
            }
            if (recipe->kind == SOL_MIR_RECIPE_CAPABILITY) {
                capability_layout = true;
                CHECK(plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE);
                CHECK(equality->classification == SOL_MIR_RUNTIME_EQUALITY_FORBIDDEN);
                const SolMirRuntimeOwnershipPlan *ownership
                    = &values.ownership_plans[i];
                if (recipe->capability_source == SOL_MIR_RECIPE_NONE) {
                    root_capability = true;
                    CHECK(ownership->edges.count == 0);
                } else {
                    derived_capability = true;
                    CHECK(ownership->edges.count == 1
                        && values.owned_edges[ownership->edges.offset].kind
                            == SOL_MIR_RUNTIME_OWNED_EDGE_PRIVATE_SOURCE
                        && values.owned_edges[ownership->edges.offset].recipe
                            == recipe->capability_source);
                }
            }
            if (recipe->kind == SOL_MIR_RECIPE_RECORD && recipe->fields.count == 1
                && equality->classification == SOL_MIR_RUNTIME_EQUALITY_FORBIDDEN)
                locked = true;
            if ((recipe->kind == SOL_MIR_RECIPE_DISTINCT
                    || recipe->kind == SOL_MIR_RECIPE_REFINED)
                && equality->classification == SOL_MIR_RUNTIME_EQUALITY_FORBIDDEN)
                locked_wrapper = true;
            if (recipe->kind == SOL_MIR_RECIPE_ENUM && recipe->variants.count == 2
                && equality->classification == SOL_MIR_RUNTIME_EQUALITY_FORBIDDEN)
                mixed = true;
            if (recipe->kind == SOL_MIR_RECIPE_ENUM && recipe->variants.count == 2
                && equality->classification == SOL_MIR_RUNTIME_EQUALITY_SUM)
                recursive = true;
            for (size_t v = 0; v < recipe->variants.count; ++v) {
                const SolMirRecipeVariant *variant = &program.representation.variants[
                    recipe->variants.offset + v];
                for (size_t f = 0; f < variant->fields.count; ++f)
                    if (program.representation.fields[variant->fields.offset + f].type == i)
                        CHECK(host->classification == SOL_MIR_RUNTIME_HOST_RESULT_FORBIDDEN);
            }
            if (values.recipe_operations[i].demanded_operations == 0
                && equality->classification != SOL_MIR_RUNTIME_EQUALITY_FORBIDDEN
                && equality->classification != SOL_MIR_RUNTIME_EQUALITY_UNREACHABLE)
                non_demanded_eligible = true;
        }
        CHECK(empty && uninhabited && scalar_wrapper && text_wrapper && aggregate_wrapper
            && callable_layout && capability_layout
            && root_capability && derived_capability && locked && locked_wrapper
            && mixed && recursive && non_demanded_eligible);
        test_plan_driven_trace_model(&values);
        test_copy_transaction_model(&values);
        test_equality_model(&values);
    }
    sol_mir_runtime_values_free(&values);
    sol_mir_runtime_conventions_free(&conventions);
    sol_mir_concrete_program_free(&program);
    text_compilation_free(&compilation);
}

/* Test-only represented instance. It reads static plan slices but is not a
   runtime value, allocator, or executor. Edge pointer identity makes every
   simulated traversal independently observable. */
typedef enum {
    DROP_TRACE_DESCEND,
    DROP_TRACE_TEXT_PAYLOAD_RELEASE,
    DROP_TRACE_TEXT_HEADER_RELEASE,
    DROP_TRACE_OUTER_RELEASE,
    DROP_TRACE_HANDLE_INVALIDATION,
    DROP_TRACE_CAPTURE_RECEIVER,
} DropTraceKind;

typedef struct {
    DropTraceKind kind;
    SolMirRecipeId recipe;
    const SolMirRuntimeOwnedEdge *edge;
} DropTraceEvent;

typedef struct {
    const SolMirRuntimeValues *values;
    SolMirRecipeId recipe;
    bool root_available;
    bool children[32];
    size_t active_variant;
    size_t selected_producer;
    const SolMirRuntimeOwnedEdge *selected_capture;
    uint64_t text_length;
    SolMirRuntimeAllocationUsage allocation_before;
    SolMirRuntimeAllocationUsage allocation_after;
    DropTraceEvent trace[128];
    size_t trace_length;
    bool traversal_guarded;
} RepresentedInstance;

static void instance_event(RepresentedInstance *instance, DropTraceKind kind,
    SolMirRecipeId recipe, const SolMirRuntimeOwnedEdge *edge) {
    if (instance->trace_length < sizeof(instance->trace) / sizeof(instance->trace[0]))
        instance->trace[instance->trace_length++] = (DropTraceEvent){kind, recipe,
            edge};
}

static void instance_clear_trace(RepresentedInstance *instance) {
    instance->trace_length = 0;
}

static void instance_init(RepresentedInstance *instance,
    const SolMirRuntimeValues *values, SolMirRecipeId recipe) {
    memset(instance, 0, sizeof(*instance));
    instance->values = values;
    instance->recipe = recipe;
    instance->root_available = true;
    instance->selected_producer = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < sizeof(instance->children) / sizeof(instance->children[0]); ++i)
        instance->children[i] = true;
    instance->allocation_before = instance->allocation_after
        = (SolMirRuntimeAllocationUsage){7, 19};
}

static void move_root(RepresentedInstance *source, RepresentedInstance *destination) {
    *destination = *source;
    source->root_available = false;
    destination->root_available = true;
}

static void move_projected(RepresentedInstance *instance, size_t child) {
    if (child < sizeof(instance->children) / sizeof(instance->children[0]))
        instance->children[child] = false;
}

static void repair_exact_hole(RepresentedInstance *instance, size_t child) {
    if (child < sizeof(instance->children) / sizeof(instance->children[0]))
        instance->children[child] = true;
}

static void repair_covering_ancestor(RepresentedInstance *instance) {
    instance->root_available = true;
    for (size_t i = 0; i < sizeof(instance->children) / sizeof(instance->children[0]); ++i)
        instance->children[i] = true;
}

enum { INSTANCE_MAX_DROP_DEPTH = 32 };

static bool drop_recipe(RepresentedInstance *instance, SolMirRecipeId recipe,
    const SolMirRuntimeOwnedEdge *origin, SolMirRecipeId *ancestors,
    size_t depth);

static bool instance_edges(RepresentedInstance *instance, SolMirRecipeId owner,
    SolMirRuntimeSlice slice, SolMirRecipeId *ancestors, size_t depth) {
    for (size_t i = 0; i < slice.count; ++i) {
        const SolMirRuntimeOwnedEdge *edge = &instance->values->owned_edges[
            slice.offset + i];
        bool available = owner != instance->recipe || (i
            < sizeof(instance->children) / sizeof(instance->children[0])
            && instance->children[i]);
        if (!available) continue;
        instance_event(instance, DROP_TRACE_DESCEND, edge->recipe, edge);
        if (!drop_recipe(instance, edge->recipe, edge, ancestors, depth))
            return false;
    }
    return true;
}

static bool drop_recipe(RepresentedInstance *instance, SolMirRecipeId recipe,
    const SolMirRuntimeOwnedEdge *origin, SolMirRecipeId *ancestors,
    size_t depth) {
    if (recipe >= instance->values->ownership_plan_count
        || depth == INSTANCE_MAX_DROP_DEPTH) {
        instance->traversal_guarded = true;
        return false;
    }
    for (size_t i = 0; i < depth; ++i)
        if (ancestors[i] == recipe) {
            instance->traversal_guarded = true;
            return false;
        }
    ancestors[depth] = recipe;
    const SolMirRuntimeOwnershipPlan *plan = &instance->values->ownership_plans[recipe];
    if (plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_PRODUCT) {
        if (!instance_edges(instance, recipe, plan->edges, ancestors, depth + 1))
            return false;
        instance_event(instance, DROP_TRACE_OUTER_RELEASE, recipe, origin);
    } else if (plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_SUM) {
        if (recipe != instance->recipe || instance->active_variant >= plan->variants.count) {
            instance->traversal_guarded = true;
            return false;
        }
        {
            const SolMirRuntimeOwnershipVariant *variant
                = &instance->values->ownership_variants[plan->variants.offset
                    + instance->active_variant];
            if (!instance_edges(instance, recipe, variant->edges, ancestors,
                    depth + 1)) return false;
            instance_event(instance, DROP_TRACE_OUTER_RELEASE, recipe, origin);
        }
    } else if (plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_TEXT) {
        if (instance->text_length != 0)
            instance_event(instance, DROP_TRACE_TEXT_PAYLOAD_RELEASE, recipe, origin);
        instance_event(instance, DROP_TRACE_TEXT_HEADER_RELEASE, recipe, origin);
    } else if (plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_WRAPPER) {
        if (!instance_edges(instance, recipe, plan->edges, ancestors, depth + 1))
            return false;
    } else if (plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_CALLABLE) {
        /* Selection supplies one authenticated edge; it never scans captures. */
        if (instance->selected_capture != NULL
            && instance->selected_capture->kind
                == SOL_MIR_RUNTIME_OWNED_EDGE_CAPTURED_RECEIVER
            && instance->selected_capture->producer == instance->selected_producer)
            instance_event(instance, DROP_TRACE_CAPTURE_RECEIVER,
                instance->selected_capture->recipe, instance->selected_capture);
        instance_event(instance, DROP_TRACE_HANDLE_INVALIDATION, recipe, origin);
    } else if (plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_CAPABILITY) {
        if (!instance_edges(instance, recipe, plan->edges, ancestors, depth + 1))
            return false;
        if (plan->edges.count != 0)
            instance_event(instance, DROP_TRACE_HANDLE_INVALIDATION, recipe, origin);
    }
    return true;
}

static void drop_trace(RepresentedInstance *instance) {
    SolMirRecipeId ancestors[INSTANCE_MAX_DROP_DEPTH];
    instance->traversal_guarded = false;
    if (!instance->root_available) return;
    (void)drop_recipe(instance, instance->recipe, NULL, ancestors, 0);
}

static bool trace_has_edge(const RepresentedInstance *instance,
    const SolMirRuntimeOwnedEdge *edge) {
    for (size_t i = 0; i < instance->trace_length; ++i)
        if (instance->trace[i].edge == edge) return true;
    return false;
}

static size_t check_text_descendant(const RepresentedInstance *instance,
    size_t at, const SolMirRuntimeOwnedEdge *edge) {
    CHECK(at + 2 < instance->trace_length
        && instance->trace[at].kind == DROP_TRACE_DESCEND
        && instance->trace[at].edge == edge
        && instance->trace[at + 1].kind == DROP_TRACE_TEXT_PAYLOAD_RELEASE
        && instance->trace[at + 1].recipe == edge->recipe
        && instance->trace[at + 1].edge == edge
        && instance->trace[at + 2].kind == DROP_TRACE_TEXT_HEADER_RELEASE
        && instance->trace[at + 2].recipe == edge->recipe
        && instance->trace[at + 2].edge == edge);
    return at + 3;
}

static void check_product_text_postorder(const RepresentedInstance *instance,
    const SolMirRuntimeOwnershipPlan *plan, size_t first_child) {
    size_t at = 0;
    for (size_t child = first_child; child < plan->edges.count; ++child) {
        const SolMirRuntimeOwnedEdge *edge = &instance->values->owned_edges[
            plan->edges.offset + child];
        CHECK(instance->values->ownership_plans[edge->recipe].classification
            == SOL_MIR_RUNTIME_OWNERSHIP_TEXT);
        at = check_text_descendant(instance, at, edge);
    }
    CHECK(at + 1 == instance->trace_length
        && instance->trace[at].kind == DROP_TRACE_OUTER_RELEASE
        && instance->trace[at].recipe == plan->recipe
        && instance->trace[at].edge == NULL && !instance->traversal_guarded);
}

static bool is_two_text_field_slice(const SolMirRuntimeValues *values,
    SolMirRuntimeSlice slice) {
    if (slice.count != 2) return false;
    for (size_t i = 0; i < slice.count; ++i) {
        const SolMirRuntimeOwnedEdge *edge = &values->owned_edges[slice.offset + i];
        if (edge->kind != SOL_MIR_RUNTIME_OWNED_EDGE_FIELD
            || edge->recipe >= values->ownership_plan_count
            || values->ownership_plans[edge->recipe].classification
                != SOL_MIR_RUNTIME_OWNERSHIP_TEXT) return false;
    }
    return true;
}

static void test_plan_driven_trace_model(const SolMirRuntimeValues *values) {
    SolMirRecipeId product = SOL_MIR_RECIPE_NONE, sum = SOL_MIR_RECIPE_NONE;
    SolMirRecipeId text = SOL_MIR_RECIPE_NONE, wrapper = SOL_MIR_RECIPE_NONE;
    SolMirRecipeId root_capability = SOL_MIR_RECIPE_NONE;
    SolMirRecipeId derived_capability = SOL_MIR_RECIPE_NONE;
    size_t sum_active_variant = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < values->ownership_plan_count; ++i) {
        const SolMirRuntimeOwnershipPlan *plan = &values->ownership_plans[i];
        if (plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_PRODUCT
            && is_two_text_field_slice(values, plan->edges)) product = i;
        if (plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_SUM
            && plan->variants.count == 2)
            for (size_t variant = 0; variant < plan->variants.count; ++variant) {
                const SolMirRuntimeOwnershipVariant *candidate
                    = &values->ownership_variants[plan->variants.offset + variant];
                if (is_two_text_field_slice(values, candidate->edges)) {
                    sum = i;
                    sum_active_variant = variant;
                    break;
                }
            }
        if (plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_TEXT) text = i;
        if (plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_WRAPPER
            && plan->edges.count == 1
            && values->ownership_plans[values->owned_edges[plan->edges.offset].recipe]
                .classification == SOL_MIR_RUNTIME_OWNERSHIP_TEXT) wrapper = i;
        if (plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_CAPABILITY) {
            if (plan->edges.count == 0) root_capability = i;
            if (plan->edges.count == 1) derived_capability = i;
        }
    }
    CHECK(product != SOL_MIR_RECIPE_NONE && sum != SOL_MIR_RECIPE_NONE
        && text != SOL_MIR_RECIPE_NONE && wrapper != SOL_MIR_RECIPE_NONE
        && root_capability != SOL_MIR_RECIPE_NONE
        && derived_capability != SOL_MIR_RECIPE_NONE
        && sum_active_variant != SOL_MIR_RUNTIME_NONE);
    if (product == SOL_MIR_RECIPE_NONE || sum == SOL_MIR_RECIPE_NONE
        || text == SOL_MIR_RECIPE_NONE || wrapper == SOL_MIR_RECIPE_NONE
        || root_capability == SOL_MIR_RECIPE_NONE
        || derived_capability == SOL_MIR_RECIPE_NONE
        || sum_active_variant == SOL_MIR_RUNTIME_NONE) return;

    RepresentedInstance instance, destination;
    const SolMirRuntimeOwnershipPlan *product_plan
        = &values->ownership_plans[product];
    CHECK(product_plan->edges.count <= sizeof(instance.children)
        / sizeof(instance.children[0]));
    instance_init(&instance, values, product);
    instance.text_length = 1;
    drop_trace(&instance);
    check_product_text_postorder(&instance, product_plan, 0);
    move_projected(&instance, 0);
    CHECK(!instance.children[0] && instance.children[1]);
    instance_clear_trace(&instance);
    drop_trace(&instance);
    check_product_text_postorder(&instance, product_plan, 1);
    repair_exact_hole(&instance, 0);
    CHECK(instance.children[0] && instance.children[1]);
    instance_clear_trace(&instance);
    drop_trace(&instance);
    check_product_text_postorder(&instance, product_plan, 0);
    move_projected(&instance, 0);
    repair_covering_ancestor(&instance);
    CHECK(instance.root_available && instance.children[0] && instance.children[1]);
    instance_clear_trace(&instance);
    drop_trace(&instance);
    check_product_text_postorder(&instance, product_plan, 0);
    instance_init(&instance, values, product);
    instance.text_length = 1;
    move_root(&instance, &destination);
    CHECK(!instance.root_available && destination.root_available
        && memcmp(&instance.allocation_before, &destination.allocation_after,
            sizeof(instance.allocation_before)) == 0);
    drop_trace(&instance);
    CHECK(instance.trace_length == 0);
    instance_clear_trace(&destination);
    drop_trace(&destination);
    check_product_text_postorder(&destination, product_plan, 0);
    CHECK(memcmp(&destination.allocation_before, &destination.allocation_after,
        sizeof(destination.allocation_before)) == 0);

    const SolMirRuntimeOwnershipPlan *wrapper_plan
        = &values->ownership_plans[wrapper];
    const SolMirRuntimeOwnedEdge *backing = &values->owned_edges[
        wrapper_plan->edges.offset];
    CHECK(backing->kind == SOL_MIR_RUNTIME_OWNED_EDGE_BACKING);
    instance_init(&instance, values, wrapper);
    instance.text_length = 1;
    drop_trace(&instance);
    CHECK(instance.trace_length == 3 && !instance.traversal_guarded);
    CHECK(check_text_descendant(&instance, 0, backing) == instance.trace_length);

    const SolMirRuntimeOwnershipPlan *root_plan
        = &values->ownership_plans[root_capability];
    const SolMirRuntimeOwnershipPlan *derived_plan
        = &values->ownership_plans[derived_capability];
    CHECK(root_plan->edges.count == 0 && derived_plan->edges.count == 1);
    instance_init(&instance, values, root_capability);
    drop_trace(&instance);
    CHECK(instance.trace_length == 0);
    const SolMirRuntimeOwnedEdge *private_source = &values->owned_edges[
        derived_plan->edges.offset];
    CHECK(private_source->kind == SOL_MIR_RUNTIME_OWNED_EDGE_PRIVATE_SOURCE);
    instance_init(&instance, values, derived_capability);
    drop_trace(&instance);
    CHECK(instance.trace_length == 2 && !instance.traversal_guarded
        && instance.trace[0].kind == DROP_TRACE_DESCEND
        && instance.trace[0].edge == private_source
        && instance.trace[1].kind == DROP_TRACE_HANDLE_INVALIDATION
        && instance.trace[1].recipe == derived_capability);

    const SolMirRuntimeOwnershipPlan *sum_plan = &values->ownership_plans[sum];
    const SolMirRuntimeOwnershipVariant *active = &values->ownership_variants[
        sum_plan->variants.offset + sum_active_variant];
    CHECK(is_two_text_field_slice(values, active->edges));
    instance_init(&instance, values, sum);
    instance.text_length = 1;
    instance.active_variant = sum_active_variant;
    drop_trace(&instance);
    size_t at = 0;
    for (size_t edge = 0; edge < active->edges.count; ++edge) {
        const SolMirRuntimeOwnedEdge *item = &values->owned_edges[
            active->edges.offset + edge];
        CHECK(values->ownership_plans[item->recipe].classification
            == SOL_MIR_RUNTIME_OWNERSHIP_TEXT);
        at = check_text_descendant(&instance, at, item);
    }
    for (size_t variant = 0; variant < sum_plan->variants.count; ++variant) {
        if (variant == sum_active_variant) continue;
        const SolMirRuntimeOwnershipVariant *inactive = &values->ownership_variants[
            sum_plan->variants.offset + variant];
        for (size_t edge = 0; edge < inactive->edges.count; ++edge)
            CHECK(!trace_has_edge(&instance, &values->owned_edges[
                inactive->edges.offset + edge]));
    }
    CHECK(at + 1 == instance.trace_length && !instance.traversal_guarded
        && instance.trace[at].kind == DROP_TRACE_OUTER_RELEASE
        && instance.trace[at].recipe == sum);

    instance_init(&instance, values, text);
    drop_trace(&instance);
    CHECK(instance.trace_length == 1
        && instance.trace[0].kind == DROP_TRACE_TEXT_HEADER_RELEASE);
    instance_init(&instance, values, text);
    instance.text_length = 1;
    drop_trace(&instance);
    CHECK(instance.trace_length == 2
        && instance.trace[0].kind == DROP_TRACE_TEXT_PAYLOAD_RELEASE
        && instance.trace[1].kind == DROP_TRACE_TEXT_HEADER_RELEASE);
}

static void test_callable_trace_model(const SolMirRuntimeValues *values) {
    const SolMirRepresentation *representation
        = &values->conventions->concrete->representation;
    size_t exact_producer = SOL_MIR_RUNTIME_NONE;
    SolMirRecipeId exact_recipe = SOL_MIR_RECIPE_NONE;
    for (size_t producer = 0; producer < representation->callable_producer_count;
            ++producer)
        if (representation->callable_producers[producer].kind
                == SOL_MIR_CALLABLE_PRODUCER_EXACT_FUNCTION) {
            exact_producer = producer;
            exact_recipe = representation->callable_producers[producer].function_recipe;
            break;
        }
    CHECK(exact_producer != SOL_MIR_RUNTIME_NONE
        && exact_recipe < values->ownership_plan_count);
    if (exact_producer == SOL_MIR_RUNTIME_NONE
        || exact_recipe >= values->ownership_plan_count) return;
    const SolMirRuntimeOwnershipPlan *exact_plan
        = &values->ownership_plans[exact_recipe];
    CHECK(exact_plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_CALLABLE
        && exact_plan->edges.count == 0);
    for (size_t edge = 0; edge < exact_plan->edges.count; ++edge)
        CHECK(values->owned_edges[exact_plan->edges.offset + edge].producer
            != exact_producer);
    RepresentedInstance instance;
    instance_init(&instance, values, exact_recipe);
    instance.selected_producer = exact_producer;
    drop_trace(&instance);
    CHECK(instance.trace_length == 1 && !instance.traversal_guarded
        && instance.trace[0].kind == DROP_TRACE_HANDLE_INVALIDATION
        && instance.trace[0].recipe == exact_recipe);

    SolMirRecipeId recipe = SOL_MIR_RECIPE_NONE;
    for (size_t i = 0; i < values->ownership_plan_count; ++i)
        if (values->ownership_plans[i].classification
                == SOL_MIR_RUNTIME_OWNERSHIP_CALLABLE
            && values->ownership_plans[i].edges.count >= 2) {
            recipe = i;
            break;
        }
    CHECK(recipe != SOL_MIR_RECIPE_NONE);
    if (recipe == SOL_MIR_RECIPE_NONE) return;
    const SolMirRuntimeOwnershipPlan *plan = &values->ownership_plans[recipe];
    size_t bound_producers = 0;
    for (size_t producer = 0; producer < representation->callable_producer_count;
            ++producer) {
        const SolMirCallableProducer *item
            = &representation->callable_producers[producer];
        if (item->function_recipe != recipe) continue;
        bound_producers += item->kind == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION;
    }
    CHECK(bound_producers >= 2 && bound_producers == plan->edges.count);
    for (size_t selected = 0; selected < plan->edges.count; ++selected) {
        const SolMirRuntimeOwnedEdge *edge = &values->owned_edges[
            plan->edges.offset + selected];
        CHECK(edge->kind == SOL_MIR_RUNTIME_OWNED_EDGE_CAPTURED_RECEIVER
            && edge->producer < representation->callable_producer_count
            && representation->callable_producers[edge->producer].kind
                == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION
            && representation->callable_producers[edge->producer].function_recipe
                == recipe);
        instance_init(&instance, values, recipe);
        instance.selected_producer = edge->producer;
        instance.selected_capture = edge;
        drop_trace(&instance);
        CHECK(instance.trace_length == 2
            && instance.trace[0].kind == DROP_TRACE_CAPTURE_RECEIVER
            && instance.trace[0].edge == edge
            && instance.trace[1].kind == DROP_TRACE_HANDLE_INVALIDATION);
        for (size_t other = 0; other < plan->edges.count; ++other)
            if (other != selected)
                CHECK(!trace_has_edge(&instance, &values->owned_edges[
                    plan->edges.offset + other]));
    }
}

/* Test-only finite-instance copy transaction model. It deliberately models
   observable requests and cleanup without creating a production runtime value,
   allocator, or executor. */
typedef struct CopyModelNode CopyModelNode;
typedef struct CopyModelDestination CopyModelDestination;

struct CopyModelNode {
    SolMirRecipeId recipe;
    bool live;
    bool initialized;
    bool available;
    size_t active_variant;
    size_t semantic_tag;
    uint64_t scalar;
    /* Test-only bytes standing in for inactive sum representation. Equality
       must neither validate nor compare them outside the active child slice. */
    uint64_t inactive_noise;
    uint64_t text_length;
    char text_bytes[16];
    uintptr_t physical_id;
    CopyModelNode *children[8];
    size_t child_count;
};

#define COPY_MODEL_CHILD_CAPACITY \
    (sizeof(((CopyModelNode *)0)->children) / sizeof(((CopyModelNode *)0)->children[0]))
#define COPY_MODEL_EVENT_CAPACITY 128

typedef struct {
    bool initialized;
    bool freed;
    const CopyModelNode *source;
    uint64_t bytes;
    int kind;
} CopyModelAllocation;

struct CopyModelDestination {
    bool empty;
    bool published;
    bool overlaps_source;
    size_t publications;
    size_t copied_nodes;
    uintptr_t copied_id[64];
    CopyModelNode nodes[64];
    CopyModelNode *root;
    CopyModelAllocation allocations[128];
    size_t allocation_count;
};

typedef enum {
    COPY_MODEL_STAGE_OUTER,
    COPY_MODEL_STAGE_TEXT_HEADER,
    COPY_MODEL_STAGE_TEXT_PAYLOAD,
    COPY_MODEL_ROLLBACK_OUTER,
    COPY_MODEL_ROLLBACK_TEXT_HEADER,
    COPY_MODEL_ROLLBACK_TEXT_PAYLOAD,
    COPY_MODEL_PUBLISH,
} CopyModelEventKind;

typedef struct {
    CopyModelEventKind kind;
    uintptr_t physical_id;
    size_t allocation;
} CopyModelEvent;

typedef struct {
    const CopyModelNode *node;
    CopyModelNode *destination;
    size_t outer_allocation;
    size_t header_allocation;
    size_t payload_allocation;
    bool outer;
    bool header;
    bool payload;
} CopyModelStaged;

typedef struct {
    const SolMirRuntimeValues *values;
    SolMirRuntimeAllocationQuota quota;
    SolMirRuntimeAllocationUsage usage;
    size_t max_depth, max_nodes, max_work;
    size_t depth, peak_depth, nodes, work;
    size_t request_at, refuse_at;
    const CopyModelNode *seen_nodes[64];
    uintptr_t seen_physical[64];
    size_t seen_count, physical_count;
    CopyModelStaged staged[64];
    size_t staged_count;
    CopyModelEvent events[COPY_MODEL_EVENT_CAPACITY];
    size_t max_events;
    size_t event_count;
} CopyModel;

static bool copy_model_add_u64(uint64_t *value, uint64_t amount) {
    if (amount > UINT64_MAX - *value) return false;
    *value += amount;
    return true;
}

static bool copy_model_event(CopyModel *model, CopyModelEventKind kind,
    const CopyModelNode *node) {
    if (model->event_count >= model->max_events
        || model->event_count >= sizeof(model->events) / sizeof(model->events[0]))
        return false;
    model->events[model->event_count++] = (CopyModelEvent){kind,
        node->physical_id, SIZE_MAX};
    return true;
}

static CopyModelStaged *copy_model_staged(CopyModel *model,
    CopyModelDestination *destination, const CopyModelNode *node) {
    for (size_t i = 0; i < model->staged_count; ++i)
        if (model->staged[i].node == node) return &model->staged[i];
    if (model->staged_count == sizeof(model->staged) / sizeof(model->staged[0])
        || model->staged_count >= sizeof(destination->nodes) / sizeof(destination->nodes[0]))
        return NULL;
    CopyModelStaged *staged = &model->staged[model->staged_count];
    CopyModelNode *copy = &destination->nodes[model->staged_count];
    *copy = (CopyModelNode){.recipe = node->recipe,
        .physical_id = (uintptr_t)(void *)copy};
    *staged = (CopyModelStaged){node, copy, SIZE_MAX, SIZE_MAX, SIZE_MAX,
        false, false, false};
    ++model->staged_count;
    return staged;
}

static CopyModelStaged *copy_model_find_staged(CopyModel *model,
    const CopyModelNode *node) {
    for (size_t i = 0; i < model->staged_count; ++i)
        if (model->staged[i].node == node) return &model->staged[i];
    return NULL;
}
static bool copy_model_seen(const CopyModel *model, const CopyModelNode *node) {
    for (size_t i = 0; i < model->seen_count; ++i)
        if (model->seen_nodes[i] == node) return true;
    return false;
}

static bool copy_model_physical_seen(const CopyModel *model, uintptr_t id) {
    for (size_t i = 0; i < model->physical_count; ++i)
        if (model->seen_physical[i] == id) return true;
    return false;
}

static bool copy_model_visit(CopyModel *model, const CopyModelNode *node,
    bool preflight, SolMirRuntimeAllocationUsage *aggregate);

static bool copy_model_children(CopyModel *model, const CopyModelNode *node,
    SolMirRuntimeSlice edges, bool preflight,
    SolMirRuntimeAllocationUsage *aggregate) {
    if (node->child_count > COPY_MODEL_CHILD_CAPACITY
        || edges.count > COPY_MODEL_CHILD_CAPACITY
        || node->child_count != edges.count) return false;
    for (size_t i = 0; i < edges.count; ++i) {
        const SolMirRuntimeOwnedEdge *edge = &model->values->owned_edges[
            edges.offset + i];
        if (edge->kind != SOL_MIR_RUNTIME_OWNED_EDGE_FIELD
            && edge->kind != SOL_MIR_RUNTIME_OWNED_EDGE_BACKING) return false;
        if (node->children[i] == NULL || node->children[i]->recipe != edge->recipe
            || !copy_model_visit(model, node->children[i], preflight, aggregate))
            return false;
    }
    return true;
}

static bool copy_model_account(CopyModel *model, const CopyModelNode *node,
    SolMirRuntimeAllocationUsage *aggregate) {
    SolMirRuntimeAllocationRequest request = {node->recipe, node->text_length,
        SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_ALLOCATION_REQUEST_ORDINARY};
    SolMirRuntimeAllocationQuota unlimited = {UINT64_MAX, UINT64_MAX};
    SolMirRuntimeAllocationDemand demand;
    if (sol_mir_runtime_values_check_allocation(model->values, &request,
            &unlimited, aggregate, &demand) != SOL_MIR_RUNTIME_ALLOCATION_SUCCEEDED
        || !copy_model_add_u64(&aggregate->requests, demand.requests)
        || !copy_model_add_u64(&aggregate->bytes, demand.bytes)) return false;
    return true;
}

static bool copy_model_visit(CopyModel *model, const CopyModelNode *node,
    bool preflight, SolMirRuntimeAllocationUsage *aggregate) {
    if (node == NULL || !node->live || !node->initialized || !node->available
        || node->child_count > COPY_MODEL_CHILD_CAPACITY || model->depth >= model->max_depth || model->nodes >= model->max_nodes
        || model->work >= model->max_work || copy_model_seen(model, node)
        || model->seen_count == sizeof(model->seen_nodes) / sizeof(model->seen_nodes[0]))
        return false;
    ++model->depth; ++model->nodes; ++model->work;
    if (model->depth > model->peak_depth) model->peak_depth = model->depth;
    model->seen_nodes[model->seen_count++] = node;
    if (node->recipe >= model->values->copy_plan_count) return false;
    const SolMirRuntimeCopyPlan *copy = &model->values->copy_plans[node->recipe];
    const SolMirRuntimeOwnershipPlan *ownership
        = &model->values->ownership_plans[node->recipe];
    if (copy->classification == SOL_MIR_RUNTIME_COPY_UNREACHABLE
        || copy->classification == SOL_MIR_RUNTIME_COPY_FORBIDDEN) return false;
    /* A wrapper is a view of precisely its backing physical object. */
    if (copy->classification != SOL_MIR_RUNTIME_COPY_WRAPPER) {
        if (node->physical_id == 0 || copy_model_physical_seen(model,
                node->physical_id) || model->physical_count
                == sizeof(model->seen_physical) / sizeof(model->seen_physical[0]))
            return false;
        model->seen_physical[model->physical_count++] = node->physical_id;
    }
    if (copy->classification == SOL_MIR_RUNTIME_COPY_TEXT
        && node->text_length > sizeof(node->text_bytes)) return false;
    bool needs_outer = copy->classification == SOL_MIR_RUNTIME_COPY_PRODUCT
        || copy->classification == SOL_MIR_RUNTIME_COPY_SUM
        || copy->classification == SOL_MIR_RUNTIME_COPY_TEXT;
    if (needs_outer && !copy_model_account(model, node, aggregate)) return false;
    bool ok = false;
    if (copy->classification == SOL_MIR_RUNTIME_COPY_TRIVIAL)
        ok = node->child_count == 0;
    else if (copy->classification == SOL_MIR_RUNTIME_COPY_TEXT)
        ok = node->child_count == 0;
    else if (copy->classification == SOL_MIR_RUNTIME_COPY_PRODUCT)
        ok = copy_model_children(model, node, ownership->edges, preflight,
            aggregate);
    else if (copy->classification == SOL_MIR_RUNTIME_COPY_SUM) {
        if (node->active_variant >= ownership->variants.count) ok = false;
        else {
            const SolMirRuntimeOwnershipVariant *variant
                = &model->values->ownership_variants[ownership->variants.offset
                    + node->active_variant];
            ok = node->semantic_tag == variant->semantic_tag
                && copy_model_children(model, node, variant->edges, preflight,
                    aggregate);
        }
    } else if (copy->classification == SOL_MIR_RUNTIME_COPY_WRAPPER) {
        ok = ownership->edges.count == 1 && node->child_count == 1
            && node->children[0] != NULL
            && node->children[0]->recipe == model->values->owned_edges[
                ownership->edges.offset].recipe
            && node->physical_id == node->children[0]->physical_id
            && copy_model_visit(model, node->children[0], preflight, aggregate);
    }
    --model->depth;
    return ok;
}

static void copy_model_copy_metadata(CopyModelNode *destination,
    const CopyModelNode *source) {
    destination->live = source->live;
    destination->initialized = source->initialized;
    destination->available = source->available;
    destination->active_variant = source->active_variant;
    destination->semantic_tag = source->semantic_tag;
    destination->scalar = source->scalar;
    destination->text_length = source->text_length;
}

static bool copy_model_stage_request(CopyModel *model,
    CopyModelDestination *destination, const CopyModelNode *node,
    CopyModelEventKind kind, uint64_t requests, uint64_t bytes,
    CopyModelStaged *staged) {
    if (requests == 0) return true;
    ++model->request_at;
    if (model->request_at == model->refuse_at
        || destination->allocation_count >= sizeof(destination->allocations)
            / sizeof(destination->allocations[0])) return false;
    if (!copy_model_add_u64(&model->usage.requests, requests)
        || !copy_model_add_u64(&model->usage.bytes, bytes)
        || model->usage.requests > model->quota.max_requests
        || model->usage.bytes > model->quota.max_bytes) return false;
    size_t allocation = destination->allocation_count++;
    destination->allocations[allocation] = (CopyModelAllocation){true, false,
        node, bytes, kind};
    if (kind == COPY_MODEL_STAGE_OUTER) {
        staged->outer = true; staged->outer_allocation = allocation;
    } else if (kind == COPY_MODEL_STAGE_TEXT_HEADER) {
        staged->header = true; staged->header_allocation = allocation;
    } else {
        staged->payload = true; staged->payload_allocation = allocation;
    }
    if (!copy_model_event(model, kind, node)) return false;
    model->events[model->event_count - 1].allocation = allocation;
    return true;
}

static bool copy_model_stage(CopyModel *model, CopyModelDestination *destination,
    const CopyModelNode *node) {
    const SolMirRuntimeCopyPlan *copy = &model->values->copy_plans[node->recipe];
    const SolMirRuntimeAllocationPlan *plan = &model->values->allocation_plans[
        node->recipe];
    CopyModelStaged *staged = copy_model_staged(model, destination, node);
    if (staged == NULL) return false;
    if (copy->classification == SOL_MIR_RUNTIME_COPY_TEXT) {
        if (!copy_model_stage_request(model, destination, node,
                COPY_MODEL_STAGE_TEXT_HEADER, 1, plan->object_size, staged))
            return false;
        copy_model_copy_metadata(staged->destination, node);
        if (node->text_length != 0) {
            if (!copy_model_stage_request(model, destination, node,
                    COPY_MODEL_STAGE_TEXT_PAYLOAD, 1, node->text_length, staged))
                return false;
            memcpy(staged->destination->text_bytes, node->text_bytes,
                (size_t)node->text_length);
        }
        return true;
    }
    if (copy->classification == SOL_MIR_RUNTIME_COPY_PRODUCT
        || copy->classification == SOL_MIR_RUNTIME_COPY_SUM) {
        if (plan->kind != SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE
            && !copy_model_stage_request(model, destination, node,
                COPY_MODEL_STAGE_OUTER, 1, plan->object_size, staged)) return false;
        copy_model_copy_metadata(staged->destination, node);
        SolMirRuntimeSlice edges = copy->classification == SOL_MIR_RUNTIME_COPY_SUM
            ? model->values->ownership_variants[model->values->ownership_plans[
                node->recipe].variants.offset + node->active_variant].edges
            : model->values->ownership_plans[node->recipe].edges;
        if (edges.count > COPY_MODEL_CHILD_CAPACITY) return false;
        for (size_t i = 0; i < edges.count; ++i) {
            if (!copy_model_stage(model, destination, node->children[i])) return false;
            CopyModelStaged *child = copy_model_find_staged(model, node->children[i]);
            if (child == NULL) return false;
            staged->destination->children[i] = child->destination;
            ++staged->destination->child_count;
        }
    } else if (copy->classification == SOL_MIR_RUNTIME_COPY_WRAPPER) {
        if (!copy_model_stage(model, destination, node->children[0])) return false;
        CopyModelStaged *child = copy_model_find_staged(model, node->children[0]);
        if (child == NULL) return false;
        copy_model_copy_metadata(staged->destination, node);
        staged->destination->children[0] = child->destination;
        staged->destination->child_count = 1;
        staged->destination->physical_id = child->destination->physical_id;
    } else copy_model_copy_metadata(staged->destination, node);
    return true;
}
static bool copy_model_rollback(CopyModel *model, CopyModelDestination *destination,
    const CopyModelNode *node) {
    bool ok = true;
    const SolMirRuntimeCopyPlan *copy = &model->values->copy_plans[node->recipe];
    SolMirRuntimeSlice edges = {0, 0};
    if (copy->classification == SOL_MIR_RUNTIME_COPY_PRODUCT)
        edges = model->values->ownership_plans[node->recipe].edges;
    else if (copy->classification == SOL_MIR_RUNTIME_COPY_SUM)
        edges = model->values->ownership_variants[model->values->ownership_plans[
            node->recipe].variants.offset + node->active_variant].edges;
    else if (copy->classification == SOL_MIR_RUNTIME_COPY_WRAPPER)
        edges = model->values->ownership_plans[node->recipe].edges;
    for (size_t i = 0; i < edges.count; ++i)
        if (!copy_model_rollback(model, destination, node->children[i])) ok = false;
    CopyModelStaged *staged = copy_model_find_staged(model, node);
    if (staged == NULL) return ok;
#define COPY_MODEL_FREE(member, event_kind) do { \
    if (staged->member) { \
        size_t allocation = staged->member##_allocation; \
        if (allocation < destination->allocation_count \
            && destination->allocations[allocation].initialized \
            && !destination->allocations[allocation].freed) { \
            destination->allocations[allocation].freed = true; \
            if (!copy_model_event(model, (event_kind), node)) ok = false; \
            else model->events[model->event_count - 1].allocation = allocation; \
        } \
        staged->member = false; \
    } \
} while (0)
    COPY_MODEL_FREE(payload, COPY_MODEL_ROLLBACK_TEXT_PAYLOAD);
    COPY_MODEL_FREE(header, COPY_MODEL_ROLLBACK_TEXT_HEADER);
    COPY_MODEL_FREE(outer, COPY_MODEL_ROLLBACK_OUTER);
#undef COPY_MODEL_FREE
    return ok;
}

static bool copy_model_run(const SolMirRuntimeValues *values, CopyModelNode *root,
    CopyModelDestination *destination, SolMirRuntimeAllocationQuota quota,
    size_t max_depth, size_t max_nodes, size_t max_work, size_t max_events,
    size_t refuse_at, CopyModel *result) {
    CopyModel model = {0};
    model.values = values; model.quota = quota; model.max_depth = max_depth;
    model.max_nodes = max_nodes; model.max_work = max_work;
    model.max_events = max_events < COPY_MODEL_EVENT_CAPACITY
        ? max_events : COPY_MODEL_EVENT_CAPACITY;
    model.refuse_at = refuse_at;
    SolMirRuntimeAllocationUsage aggregate = {0};
    if (destination == NULL || !destination->empty || destination->published
        || destination->overlaps_source || !copy_model_visit(&model, root, true,
            &aggregate) || aggregate.requests > quota.max_requests
        || aggregate.bytes > quota.max_bytes || model.max_events == 0
        || aggregate.requests > (model.max_events - 1) / 2) {
        *result = model; return false;
    }
    /* Fresh traversal state makes staging independent from preflight scratch. */
    model.depth = model.nodes = model.work = model.seen_count = model.physical_count = 0;
    if (!copy_model_visit(&model, root, false, &(SolMirRuntimeAllocationUsage){0})
        || !copy_model_stage(&model, destination, root)) {
        (void)copy_model_rollback(&model, destination, root);
        *result = model; return false;
    }
    CopyModelStaged *staged_root = copy_model_find_staged(&model, root);
    if (staged_root == NULL) { copy_model_rollback(&model, destination, root); *result = model; return false; }
    if (!copy_model_event(&model, COPY_MODEL_PUBLISH, root)) {
        (void)copy_model_rollback(&model, destination, root);
        *result = model;
        return false;
    }
    destination->root = staged_root->destination;
    destination->published = true; destination->empty = false;
    ++destination->publications;
    destination->copied_nodes = model.staged_count;
    for (size_t i = 0; i < model.staged_count; ++i)
        destination->copied_id[i] = (uintptr_t)(void *)model.staged[i].destination;
    *result = model;
    return true;
}

static CopyModelDestination copy_model_destination(bool empty, bool overlaps) {
    return (CopyModelDestination){.empty = empty, .overlaps_source = overlaps};
}

static void copy_model_node(CopyModelNode *node, SolMirRecipeId recipe,
    uintptr_t physical_id) {
    memset(node, 0, sizeof(*node));
    node->recipe = recipe; node->live = node->initialized = node->available = true;
    node->physical_id = physical_id;
}

static bool copy_model_equal(const CopyModelNode *source,
    const CopyModelNode *destination) {
    if (source == NULL || destination == NULL || source->recipe != destination->recipe
        || source->scalar != destination->scalar
        || source->active_variant != destination->active_variant
        || source->semantic_tag != destination->semantic_tag
        || source->text_length != destination->text_length
        || source->text_length > sizeof(source->text_bytes)
        || memcmp(source->text_bytes, destination->text_bytes,
            (size_t)source->text_length) != 0
        || source->child_count != destination->child_count
        || source->physical_id == destination->physical_id) return false;
    for (size_t i = 0; i < source->child_count; ++i)
        if (!copy_model_equal(source->children[i], destination->children[i]))
            return false;
    return true;
}

static bool copy_model_rollback_exact(const CopyModelDestination *destination,
    const CopyModel *model, size_t successful_requests) {
    if (destination->allocation_count != successful_requests) return false;
    size_t rollback_events = 0;
    uint64_t bytes = 0;
    for (size_t i = 0; i < destination->allocation_count; ++i)
        if (!destination->allocations[i].initialized
            || !destination->allocations[i].freed
            || !copy_model_add_u64(&bytes, destination->allocations[i].bytes))
            return false;
    for (size_t i = 0; i < model->event_count; ++i) {
        CopyModelEventKind kind = model->events[i].kind;
        if (kind < COPY_MODEL_ROLLBACK_OUTER
            || kind > COPY_MODEL_ROLLBACK_TEXT_PAYLOAD) continue;
        if (model->events[i].allocation >= destination->allocation_count
            || !destination->allocations[model->events[i].allocation].freed)
            return false;
        ++rollback_events;
    }
    return rollback_events == successful_requests
        && model->usage.requests == successful_requests
        && model->usage.bytes == bytes;
}

static bool copy_model_cleanup_is(const CopyModel *model,
    const CopyModelEventKind *kinds, const uintptr_t *identities, size_t count) {
    size_t at = 0;
    for (size_t i = 0; i < model->event_count; ++i) {
        CopyModelEventKind kind = model->events[i].kind;
        if (kind < COPY_MODEL_ROLLBACK_OUTER
            || kind > COPY_MODEL_ROLLBACK_TEXT_PAYLOAD) continue;
        if (at == count || kind != kinds[at]
            || model->events[i].physical_id != identities[at]) return false;
        ++at;
    }
    return at == count;
}

static size_t copy_model_count_events(const CopyModel *model, CopyModelEventKind kind) {
    size_t count = 0;
    for (size_t i = 0; i < model->event_count; ++i) count += model->events[i].kind == kind;
    return count;
}

static void test_copy_transaction_model(const SolMirRuntimeValues *values) {
    SolMirRecipeId product = SOL_MIR_RECIPE_NONE, sum = SOL_MIR_RECIPE_NONE;
    SolMirRecipeId text = SOL_MIR_RECIPE_NONE, wrapper = SOL_MIR_RECIPE_NONE;
    SolMirRecipeId trivial = SOL_MIR_RECIPE_NONE, forbidden = SOL_MIR_RECIPE_NONE;
    SolMirRecipeId callable_forbidden = SOL_MIR_RECIPE_NONE;
    SolMirRecipeId capability_forbidden = SOL_MIR_RECIPE_NONE;
    SolMirRecipeId aggregate_forbidden = SOL_MIR_RECIPE_NONE;
    SolMirRecipeId capacity_product = SOL_MIR_RECIPE_NONE;
    size_t sum_variant = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < values->copy_plan_count; ++i) {
        SolMirRuntimeCopyClass c = values->copy_plans[i].classification;
        const SolMirRuntimeOwnershipPlan *ownership = &values->ownership_plans[i];
        if (c == SOL_MIR_RUNTIME_COPY_TEXT) text = i;
        if (c == SOL_MIR_RUNTIME_COPY_TRIVIAL) trivial = i;
        if (c == SOL_MIR_RUNTIME_COPY_FORBIDDEN) forbidden = i;
        if (c == SOL_MIR_RUNTIME_COPY_FORBIDDEN
            && values->conventions->concrete->representation.recipes[i].kind
                == SOL_MIR_RECIPE_FUNCTION) callable_forbidden = i;
        if (c == SOL_MIR_RUNTIME_COPY_FORBIDDEN
            && values->conventions->concrete->representation.recipes[i].kind
                == SOL_MIR_RECIPE_CAPABILITY) capability_forbidden = i;
        if (c == SOL_MIR_RUNTIME_COPY_FORBIDDEN
            && (values->conventions->concrete->representation.recipes[i].kind
                    == SOL_MIR_RECIPE_RECORD
                || values->conventions->concrete->representation.recipes[i].kind
                    == SOL_MIR_RECIPE_TUPLE)) aggregate_forbidden = i;
        if (c == SOL_MIR_RUNTIME_COPY_PRODUCT && ownership->edges.count == 2)
            product = i;
        if (c == SOL_MIR_RUNTIME_COPY_PRODUCT
            && ownership->edges.count == COPY_MODEL_CHILD_CAPACITY)
            capacity_product = i;
        if (c == SOL_MIR_RUNTIME_COPY_SUM && ownership->variants.count != 0) {
            for (size_t v = 0; v < ownership->variants.count; ++v) {
                const SolMirRuntimeOwnershipVariant *variant
                    = &values->ownership_variants[ownership->variants.offset + v];
                if (variant->edges.count == 1 && values->owned_edges[
                        variant->edges.offset].recipe == product) {
                    sum = i; sum_variant = v;
                }
            }
        }
        if (c == SOL_MIR_RUNTIME_COPY_WRAPPER && ownership->edges.count == 1
            && values->owned_edges[ownership->edges.offset].recipe == text)
            wrapper = i;
    }
    CHECK(product != SOL_MIR_RECIPE_NONE && sum != SOL_MIR_RECIPE_NONE
        && text != SOL_MIR_RECIPE_NONE && wrapper != SOL_MIR_RECIPE_NONE
        && trivial != SOL_MIR_RECIPE_NONE && forbidden != SOL_MIR_RECIPE_NONE
        && callable_forbidden != SOL_MIR_RECIPE_NONE
        && capability_forbidden != SOL_MIR_RECIPE_NONE
        && aggregate_forbidden != SOL_MIR_RECIPE_NONE
        && capacity_product != SOL_MIR_RECIPE_NONE
        && sum_variant != SOL_MIR_RUNTIME_NONE);
    if (product == SOL_MIR_RECIPE_NONE || sum == SOL_MIR_RECIPE_NONE
        || text == SOL_MIR_RECIPE_NONE || wrapper == SOL_MIR_RECIPE_NONE
        || trivial == SOL_MIR_RECIPE_NONE || forbidden == SOL_MIR_RECIPE_NONE
        || callable_forbidden == SOL_MIR_RECIPE_NONE
        || capability_forbidden == SOL_MIR_RECIPE_NONE
        || aggregate_forbidden == SOL_MIR_RECIPE_NONE
        || capacity_product == SOL_MIR_RECIPE_NONE
        || sum_variant == SOL_MIR_RUNTIME_NONE) return;

    CopyModelNode text_a, text_b, sum_node, root, wrap, zero;
    copy_model_node(&text_a, text, 1); text_a.text_length = 2;
    memcpy(text_a.text_bytes, "ok", 3); text_a.scalar = 17;
    copy_model_node(&text_b, text, 2); text_b.text_length = 0; text_b.scalar = 23;
    copy_model_node(&root, product, 3); root.scalar = 41; root.children[0] = &text_a;
    root.children[1] = &text_b; root.child_count = 2;
    copy_model_node(&sum_node, sum, 4); sum_node.scalar = 51;
    sum_node.active_variant = sum_variant;
    sum_node.semantic_tag = values->ownership_variants[values->ownership_plans[sum]
        .variants.offset + sum_variant].semantic_tag;
    sum_node.children[0] = &root; sum_node.child_count = 1;
    /* Choice::right contains Pair; its active finite instance nests the product. */
    const SolMirRuntimeOwnershipPlan *root_plan = &values->ownership_plans[product];
    if (values->owned_edges[root_plan->edges.offset].recipe != text
        || values->owned_edges[root_plan->edges.offset + 1].recipe != text) return;
    CopyModelDestination destination = copy_model_destination(true, false);
    CopyModel result;
    SolMirRuntimeAllocationQuota unlimited = {UINT64_MAX, UINT64_MAX};
    CopyModelNode root_before = root, sum_before = sum_node, a_before = text_a,
        b_before = text_b;
    CHECK(copy_model_run(values, &sum_node, &destination, unlimited, 256, 1048576,
        4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result) && destination.published && !destination.empty
        && destination.publications == 1 && destination.copied_nodes == 4
        && destination.root != NULL && copy_model_equal(&sum_node, destination.root)
        && memcmp(&root, &root_before, sizeof(root)) == 0
        && memcmp(&sum_node, &sum_before, sizeof(sum_node)) == 0
        && memcmp(&text_a, &a_before, sizeof(text_a)) == 0
        && memcmp(&text_b, &b_before, sizeof(text_b)) == 0
        && result.usage.requests == 5 && result.usage.bytes
            == values->allocation_plans[product].object_size
                + values->allocation_plans[sum].object_size
                + values->allocation_plans[text].object_size * 2 + 2
        && result.event_count != 0 && result.events[0].kind == COPY_MODEL_STAGE_OUTER
        && result.events[1].kind == COPY_MODEL_STAGE_OUTER
        && result.events[2].kind == COPY_MODEL_STAGE_TEXT_HEADER
        && result.events[3].kind == COPY_MODEL_STAGE_TEXT_PAYLOAD
        && result.events[4].kind == COPY_MODEL_STAGE_TEXT_HEADER
        && result.events[result.event_count - 1].kind == COPY_MODEL_PUBLISH);
    size_t required_depth = result.peak_depth, required_nodes = result.nodes;
    size_t required_work = result.work;
    CopyModelNode *destination_sum = destination.root;
    CopyModelNode *destination_product = destination_sum->children[0];
    CopyModelNode *destination_text = destination_product->children[0];
    uint64_t saved_scalar = root.scalar;
    char saved_text[sizeof(text_a.text_bytes)];
    memcpy(saved_text, text_a.text_bytes, sizeof(saved_text));
    size_t saved_tag = sum_node.semantic_tag;
    ++root.scalar; text_a.text_bytes[0] = 'x'; ++sum_node.semantic_tag;
    CHECK(destination_product->scalar == saved_scalar
        && destination_text->text_bytes[0] == saved_text[0]
        && destination_sum->semantic_tag == saved_tag);
    root.scalar = saved_scalar;
    memcpy(text_a.text_bytes, saved_text, sizeof(saved_text));
    sum_node.semantic_tag = saved_tag;
    CHECK(!copy_model_run(values, &sum_node, &destination, unlimited, 256, 1048576,
        4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result));

    /* Exact and one-below quota, depth, node, and work limits reject before stage. */
    destination = copy_model_destination(true, false);
    CHECK(copy_model_run(values, &sum_node, &destination, unlimited, 256, 1048576,
        4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result));
    SolMirRuntimeAllocationQuota exact = {result.usage.requests, result.usage.bytes};
    destination = copy_model_destination(true, false);
    CHECK(copy_model_run(values, &sum_node, &destination, exact, required_depth,
        required_nodes, required_work, COPY_MODEL_EVENT_CAPACITY, 0, &result));
    size_t exact_events = exact.max_requests * 2 + 1;
    destination = copy_model_destination(true, false);
    CHECK(copy_model_run(values, &sum_node, &destination, exact, required_depth,
        required_nodes, required_work, exact_events, 0, &result));
    destination = copy_model_destination(true, false);
    CHECK(!copy_model_run(values, &sum_node, &destination, exact, required_depth,
        required_nodes, required_work, exact_events - 1, 0, &result)
        && result.request_at == 0 && result.event_count == 0);
    SolMirRuntimeAllocationQuota below = exact; --below.max_bytes;
    destination = copy_model_destination(true, false);
    CHECK(!copy_model_run(values, &sum_node, &destination, below, 256, 1048576,
        4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result) && result.usage.requests == 0 && !destination.published);
    destination = copy_model_destination(true, false);
    CHECK(!copy_model_run(values, &sum_node, &destination, exact, required_depth - 1,
        required_nodes, required_work, COPY_MODEL_EVENT_CAPACITY, 0, &result) && result.request_at == 0);
    destination = copy_model_destination(true, false);
    CHECK(!copy_model_run(values, &sum_node, &destination, exact, required_depth,
        required_nodes - 1, required_work, COPY_MODEL_EVENT_CAPACITY, 0, &result) && result.request_at == 0);
    destination = copy_model_destination(true, false);
    CHECK(!copy_model_run(values, &sum_node, &destination, exact, required_depth,
        required_nodes, required_work - 1, COPY_MODEL_EVENT_CAPACITY, 0, &result) && result.request_at == 0);

    /* Every primitive staged request can refuse. Successful charges persist and
       cleanup is ownership postorder rather than reverse allocation order. */
    for (size_t refusal = 1; refusal <= exact.max_requests; ++refusal) {
        destination = copy_model_destination(true, false);
        CHECK(!copy_model_run(values, &sum_node, &destination, unlimited, 256,
            1048576, 4000000, COPY_MODEL_EVENT_CAPACITY, refusal, &result) && !destination.published
            && memcmp(&root, &root_before, sizeof(root)) == 0
            && memcmp(&sum_node, &sum_before, sizeof(sum_node)) == 0
            && memcmp(&text_a, &a_before, sizeof(text_a)) == 0
            && memcmp(&text_b, &b_before, sizeof(text_b)) == 0
            && result.request_at == refusal && result.usage.requests == refusal - 1
            && copy_model_count_events(&result, COPY_MODEL_PUBLISH) == 0
            && copy_model_rollback_exact(&destination, &result, refusal - 1));
        if (refusal == 1)
            CHECK(destination.nodes[0].scalar == 0
                && destination.nodes[0].semantic_tag == 0
                && destination.nodes[0].child_count == 0);
        if (refusal == 3)
            CHECK(destination.nodes[2].text_length == 0
                && destination.nodes[2].scalar == 0
                && destination.nodes[2].text_bytes[0] == '\0');
        if (refusal == 4)
            CHECK(destination.nodes[2].text_length == text_a.text_length
                && destination.nodes[2].text_bytes[0] == '\0'
                && destination.nodes[1].child_count == 0);
        const CopyModelEventKind cleanup_kinds[][4] = {
            {0, 0, 0, 0},
            {COPY_MODEL_ROLLBACK_OUTER, 0, 0, 0},
            {COPY_MODEL_ROLLBACK_OUTER, COPY_MODEL_ROLLBACK_OUTER, 0, 0},
            {COPY_MODEL_ROLLBACK_TEXT_HEADER, COPY_MODEL_ROLLBACK_OUTER,
                COPY_MODEL_ROLLBACK_OUTER, 0},
            {COPY_MODEL_ROLLBACK_TEXT_PAYLOAD, COPY_MODEL_ROLLBACK_TEXT_HEADER,
                COPY_MODEL_ROLLBACK_OUTER, COPY_MODEL_ROLLBACK_OUTER},
        };
        const uintptr_t cleanup_ids[][4] = {
            {0, 0, 0, 0}, {sum_node.physical_id, 0, 0, 0},
            {root.physical_id, sum_node.physical_id, 0, 0},
            {text_a.physical_id, root.physical_id, sum_node.physical_id, 0},
            {text_a.physical_id, text_a.physical_id, root.physical_id,
                sum_node.physical_id},
        };
        CHECK(copy_model_cleanup_is(&result, cleanup_kinds[refusal - 1],
            cleanup_ids[refusal - 1], refusal - 1));
        CopyModelDestination retry = copy_model_destination(true, false);
        CHECK(copy_model_run(values, &sum_node, &retry, unlimited, 256, 1048576,
            4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result) && retry.published
            && copy_model_equal(&sum_node, retry.root));
    }

    /* Every invalid input starts with, and changes exactly one fact from, this
       known-valid tree. */
    const SolMirRecipeId forbidden_recipes[] = {forbidden, callable_forbidden,
        capability_forbidden, aggregate_forbidden};
#define RESET_NESTED() do { \
    root = root_before; sum_node = sum_before; text_a = a_before; text_b = b_before; \
    destination = copy_model_destination(true, false); \
    CHECK(copy_model_run(values, &sum_node, &destination, unlimited, 256, 1048576, \
        4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result)); \
} while (0)
    RESET_NESTED();
    destination = copy_model_destination(false, false);
    CHECK(!copy_model_run(values, &sum_node, &destination, unlimited, 256, 1048576,
        4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result));
    RESET_NESTED();
    destination = copy_model_destination(true, true);
    CHECK(!copy_model_run(values, &sum_node, &destination, unlimited, 256, 1048576,
        4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result));
    RESET_NESTED();
    root.children[1] = NULL;
    destination = copy_model_destination(true, false);
    CHECK(!copy_model_run(values, &sum_node, &destination, unlimited, 256, 1048576,
        4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result) && result.request_at == 0);
    RESET_NESTED();
    ++sum_node.semantic_tag;
    destination = copy_model_destination(true, false);
    CHECK(!copy_model_run(values, &sum_node, &destination, unlimited, 256, 1048576,
        4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result) && result.request_at == 0);
    RESET_NESTED();
    sum_node.active_variant = values->ownership_plans[sum].variants.count;
    destination = copy_model_destination(true, false);
    CHECK(!copy_model_run(values, &sum_node, &destination, unlimited, 256, 1048576,
        4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result));
    RESET_NESTED();
    text_a.available = false;
    destination = copy_model_destination(true, false);
    CHECK(!copy_model_run(values, &sum_node, &destination, unlimited, 256, 1048576,
        4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result));
    RESET_NESTED();
    text_a.text_length = sizeof(text_a.text_bytes) + 1;
    CopyModelNode oversized_text = text_a;
    destination = copy_model_destination(true, false);
    CHECK(!copy_model_run(values, &sum_node, &destination, unlimited, 256, 1048576,
        4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result)
        && result.request_at == 0 && result.usage.requests == 0
        && result.usage.bytes == 0 && result.event_count == 0
        && !destination.published && memcmp(&root, &root_before, sizeof(root)) == 0
        && memcmp(&sum_node, &sum_before, sizeof(sum_node)) == 0
        && memcmp(&text_a, &oversized_text, sizeof(text_a)) == 0);
    RESET_NESTED();
    text_a.text_length = (uint64_t)UINT32_MAX + 1;
    destination = copy_model_destination(true, false);
    CHECK(!copy_model_run(values, &sum_node, &destination, unlimited, 256, 1048576,
        4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result) && result.request_at == 0 && result.usage.requests == 0);
    RESET_NESTED();
    uint64_t saved_object_limit = values->conventions->concrete->layout.target.max_object_bytes;
    ((SolMirConcreteProgram *)(void *)values->conventions->concrete)->layout.target
        .max_object_bytes = 1;
    destination = copy_model_destination(true, false);
    CHECK(!copy_model_run(values, &sum_node, &destination, unlimited, 256, 1048576,
        4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result) && result.request_at == 0 && !destination.published);
    ((SolMirConcreteProgram *)(void *)values->conventions->concrete)->layout.target
        .max_object_bytes = saved_object_limit;
    RESET_NESTED();
    text_b.physical_id = text_a.physical_id;
    destination = copy_model_destination(true, false);
    CHECK(!copy_model_run(values, &sum_node, &destination, unlimited, 256, 1048576,
        4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result) && result.request_at == 0);
    RESET_NESTED();
    CopyModelNode capacity_root;
    CopyModelNode capacity_children[COPY_MODEL_CHILD_CAPACITY];
    copy_model_node(&capacity_root, capacity_product, 90);
    for (size_t i = 0; i < COPY_MODEL_CHILD_CAPACITY; ++i) {
        copy_model_node(&capacity_children[i], text, 100 + i);
        capacity_root.children[i] = &capacity_children[i];
    }
    capacity_root.child_count = COPY_MODEL_CHILD_CAPACITY;
    destination = copy_model_destination(true, false);
    CHECK(copy_model_run(values, &capacity_root, &destination, unlimited, 256,
        1048576, 4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result));
    capacity_root.child_count = COPY_MODEL_CHILD_CAPACITY + 1;
    destination = copy_model_destination(true, false);
    CHECK(!copy_model_run(values, &capacity_root, &destination, unlimited, 256,
        1048576, 4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result)
        && result.request_at == 0);
    RESET_NESTED();
    copy_model_node(&zero, trivial, 9);
    destination = copy_model_destination(true, false);
    CHECK(copy_model_run(values, &zero, &destination, (SolMirRuntimeAllocationQuota){0, 0},
        256, 1048576, 4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result) && result.usage.requests == 0);
    RESET_NESTED();
    copy_model_node(&wrap, wrapper, 1); wrap.children[0] = &text_a;
    wrap.child_count = 1;
    destination = copy_model_destination(true, false);
    CHECK(copy_model_run(values, &wrap, &destination, unlimited, 256, 1048576,
        4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result) && copy_model_count_events(&result,
            COPY_MODEL_STAGE_TEXT_HEADER) == 1 && copy_model_count_events(&result,
            COPY_MODEL_STAGE_OUTER) == 0);
    destination = copy_model_destination(true, false);
    CHECK(!copy_model_run(values, &wrap, &destination, unlimited, 256, 1048576,
        4000000, COPY_MODEL_EVENT_CAPACITY, 1, &result) && copy_model_count_events(&result,
            COPY_MODEL_ROLLBACK_TEXT_HEADER) == 0);
    for (size_t i = 0; i < sizeof(forbidden_recipes) / sizeof(forbidden_recipes[0]); ++i) {
        RESET_NESTED();
        copy_model_node(&zero, forbidden_recipes[i], 10 + i);
        destination = copy_model_destination(true, false);
        CHECK(!copy_model_run(values, &zero, &destination, unlimited, 256, 1048576,
            4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result) && result.request_at == 0);
    }
#undef RESET_NESTED
}

static void test_recursive_copy_transaction_model(void) {
    static const char source[] =
        "module runtime_values_recursive_copy\n"
        "enum List { nil, cons(next: List) }\n"
        "function root(value: List) -> Bool effects { pure } { return true }\n";
    TextCompilation compilation;
    SolMirConcreteProgram program;
    SolMirRuntimeConventions conventions;
    SolMirRuntimeValues values;
    sol_mir_concrete_program_init(&program);
    sol_mir_runtime_conventions_init(&conventions);
    sol_mir_runtime_values_init(&values);
    CHECK(compile_text(&compilation, "/fixture/runtime_values_recursive_copy.sol",
        source));
    SolIrCallableId root = callable(&compilation.ir, "root",
        SOL_IR_CALLABLE_FUNCTION);
    SolMirProgramRoot root_request = {root, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    SolMirConcreteBuildRequest request = {&compilation.ir, &root_request, 1,
        NULL, 0, &target, NULL};
    bool built = root != SOL_IR_NONE && sol_mir_concrete_program_build(&request,
        &program, &compilation.diagnostics) == SOL_MIR_CONCRETE_BUILD_SUCCEEDED;
    CHECK(built && build_values(&program, &compilation.diagnostics, &conventions,
        &values));
    SolMirRecipeId list = SOL_MIR_RECIPE_NONE;
    size_t nil = SOL_MIR_RUNTIME_NONE, cons = SOL_MIR_RUNTIME_NONE;
    if (values.conventions != NULL) {
        for (size_t i = 0; i < values.copy_plan_count; ++i) {
            const SolMirRuntimeOwnershipPlan *ownership = &values.ownership_plans[i];
            if (values.copy_plans[i].classification != SOL_MIR_RUNTIME_COPY_SUM)
                continue;
            for (size_t v = 0; v < ownership->variants.count; ++v) {
                const SolMirRuntimeOwnershipVariant *variant = &values.ownership_variants[
                    ownership->variants.offset + v];
                if (variant->edges.count == 0) nil = v;
                else if (variant->edges.count == 1 && values.owned_edges[
                        variant->edges.offset].recipe == i) cons = v;
            }
            if (nil != SOL_MIR_RUNTIME_NONE && cons != SOL_MIR_RUNTIME_NONE) {
                list = i; break;
            }
            nil = cons = SOL_MIR_RUNTIME_NONE;
        }
        CHECK(list != SOL_MIR_RECIPE_NONE);
        if (list != SOL_MIR_RECIPE_NONE) {
            const SolMirRuntimeOwnershipPlan *ownership = &values.ownership_plans[list];
            CopyModelNode tail, finite;
            copy_model_node(&tail, list, 71); tail.active_variant = nil;
            tail.semantic_tag = values.ownership_variants[ownership->variants.offset
                + nil].semantic_tag;
            copy_model_node(&finite, list, 72); finite.active_variant = cons;
            finite.semantic_tag = values.ownership_variants[ownership->variants.offset
                + cons].semantic_tag;
            finite.children[0] = &tail; finite.child_count = 1;
            CopyModelDestination destination = copy_model_destination(true, false);
            CopyModel result;
            SolMirRuntimeAllocationQuota unlimited = {UINT64_MAX, UINT64_MAX};
            CHECK(copy_model_run(&values, &finite, &destination, unlimited, 256,
                1048576, 4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result) && destination.published
                && copy_model_equal(&finite, destination.root));
            CopyModelNode chain[COPY_MODEL_EVENT_CAPACITY / 2];
            for (size_t i = 0; i < COPY_MODEL_EVENT_CAPACITY / 2 - 2; ++i) {
                copy_model_node(&chain[i], list, 200 + i);
                chain[i].active_variant = cons;
                chain[i].semantic_tag = values.ownership_variants[
                    ownership->variants.offset + cons].semantic_tag;
                chain[i].children[0] = &chain[i + 1]; chain[i].child_count = 1;
            }
            copy_model_node(&chain[COPY_MODEL_EVENT_CAPACITY / 2 - 2], list,
                200 + COPY_MODEL_EVENT_CAPACITY / 2 - 2);
            chain[COPY_MODEL_EVENT_CAPACITY / 2 - 2].active_variant = nil;
            chain[COPY_MODEL_EVENT_CAPACITY / 2 - 2].semantic_tag
                = values.ownership_variants[ownership->variants.offset + nil].semantic_tag;
            destination = copy_model_destination(true, false);
            CHECK(copy_model_run(&values, &chain[0], &destination, unlimited, 256,
                1048576, 4000000, SIZE_MAX, 0, &result)
                && destination.published);
            /* 64 allocations require 129 possible stage/rollback/publication
               events, exceeding the fixed physical event store of 128 even
               when the caller asks for SIZE_MAX. */
            CopyModelNode too_many[COPY_MODEL_EVENT_CAPACITY / 2];
            for (size_t i = 0; i < COPY_MODEL_EVENT_CAPACITY / 2 - 1; ++i) {
                copy_model_node(&too_many[i], list, 400 + i);
                too_many[i].active_variant = cons;
                too_many[i].semantic_tag = values.ownership_variants[
                    ownership->variants.offset + cons].semantic_tag;
                too_many[i].children[0] = &too_many[i + 1];
                too_many[i].child_count = 1;
            }
            copy_model_node(&too_many[COPY_MODEL_EVENT_CAPACITY / 2 - 1], list,
                400 + COPY_MODEL_EVENT_CAPACITY / 2 - 1);
            too_many[COPY_MODEL_EVENT_CAPACITY / 2 - 1].active_variant = nil;
            too_many[COPY_MODEL_EVENT_CAPACITY / 2 - 1].semantic_tag
                = values.ownership_variants[ownership->variants.offset + nil].semantic_tag;
            destination = copy_model_destination(true, false);
            CHECK(!copy_model_run(&values, &too_many[0], &destination, unlimited,
                256, 1048576, 4000000, SIZE_MAX, 0, &result)
                && result.request_at == 0 && result.usage.requests == 0
                && result.event_count == 0 && !destination.published);
            finite.children[0] = &finite;
            destination = copy_model_destination(true, false);
            CHECK(!copy_model_run(&values, &finite, &destination, unlimited, 256,
                1048576, 4000000, COPY_MODEL_EVENT_CAPACITY, 0, &result) && result.request_at == 0
                && !destination.published);
        }
    }
    sol_mir_runtime_values_free(&values);
    sol_mir_runtime_conventions_free(&conventions);
    sol_mir_concrete_program_free(&program);
    text_compilation_free(&compilation);
}

/* Test-only finite exclusive-tree equality model.  It has no production
   storage, allocation, quota, or executor connection.  Both inputs are fully
   validated before this model compares either value. */
typedef enum {
    EQUALITY_MODEL_EQUAL,
    EQUALITY_MODEL_NOT_EQUAL,
    EQUALITY_MODEL_INVALID,
    EQUALITY_MODEL_LIMIT,
} EqualityModelOutcome;

typedef struct {
    const SolMirRuntimeValues *values;
    const CopyModelNode **nodes;
    uintptr_t *physical;
    size_t scratch_capacity;
    size_t node_count, physical_count, depth, work;
    size_t max_depth, max_nodes, max_work;
    EqualityModelOutcome outcome;
} EqualityModel;

static bool equality_model_tick(EqualityModel *model, size_t amount) {
    if (amount > SIZE_MAX - model->work || model->work + amount > model->max_work) {
        model->outcome = EQUALITY_MODEL_LIMIT; return false;
    }
    model->work += amount;
    return true;
}

static bool equality_model_seen(const EqualityModel *model, const CopyModelNode *node,
    bool physical) {
    if (physical) for (size_t i = 0; i < model->physical_count; ++i)
        if (model->physical[i] == node->physical_id) return true;
    if (!physical) for (size_t i = 0; i < model->node_count; ++i)
        if (model->nodes[i] == node) return true;
    return false;
}

static bool equality_model_validate(EqualityModel *model, const CopyModelNode *node) {
    if (node == NULL || !node->live || !node->initialized || !node->available
        || node->recipe >= model->values->equality_plan_count
        || model->depth >= model->max_depth || model->node_count >= model->max_nodes) {
        model->outcome = (model->depth >= model->max_depth
            || model->node_count >= model->max_nodes) ? EQUALITY_MODEL_LIMIT
                : EQUALITY_MODEL_INVALID;
        return false;
    }
    if (!equality_model_tick(model, 1)) return false;
    if (model->node_count == model->scratch_capacity
        || equality_model_seen(model, node, false)) {
        model->outcome = model->node_count == model->scratch_capacity
            ? EQUALITY_MODEL_LIMIT : EQUALITY_MODEL_INVALID;
        return false;
    }
    model->nodes[model->node_count++] = node;
    const SolMirRuntimeEqualityPlan *plan = &model->values->equality_plans[node->recipe];
    if (plan->classification == SOL_MIR_RUNTIME_EQUALITY_UNREACHABLE
        || plan->classification == SOL_MIR_RUNTIME_EQUALITY_FORBIDDEN) {
        model->outcome = EQUALITY_MODEL_INVALID; return false;
    }
    const SolMirRuntimeAllocationPlan *allocation
        = &model->values->allocation_plans[node->recipe];
    if (plan->classification != SOL_MIR_RUNTIME_EQUALITY_WRAPPER
        && allocation->kind != SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE) {
        if (node->physical_id == 0 || equality_model_seen(model, node, true)
            || model->physical_count == model->scratch_capacity) {
            model->outcome = model->physical_count == model->scratch_capacity
                ? EQUALITY_MODEL_LIMIT : EQUALITY_MODEL_INVALID;
            return false;
        }
        model->physical[model->physical_count++] = node->physical_id;
    }
    const SolMirRuntimeOwnershipPlan *ownership = &model->values->ownership_plans[node->recipe];
    SolMirRuntimeSlice edges = ownership->edges;
    if (plan->classification == SOL_MIR_RUNTIME_EQUALITY_TRIVIAL) {
        SolMirRecipeKind kind = model->values->conventions->concrete->representation
            .recipes[node->recipe].kind;
        if (node->child_count != 0 || (kind == SOL_MIR_RECIPE_BOOL
                && node->scalar > 1)) {
            model->outcome = EQUALITY_MODEL_INVALID; return false;
        }
    } else if (plan->classification == SOL_MIR_RUNTIME_EQUALITY_TEXT) {
        if (node->child_count != 0 || node->text_length > sizeof(node->text_bytes)
            || !equality_model_tick(model, (size_t)node->text_length)) return false;
    } else if (plan->classification == SOL_MIR_RUNTIME_EQUALITY_SUM) {
        if (node->active_variant >= ownership->variants.count) {
            model->outcome = EQUALITY_MODEL_INVALID; return false;
        }
        const SolMirRuntimeOwnershipVariant *variant = &model->values->ownership_variants[
            ownership->variants.offset + node->active_variant];
        if (node->semantic_tag != variant->semantic_tag) {
            model->outcome = EQUALITY_MODEL_INVALID; return false;
        }
        edges = variant->edges;
    } else if (plan->classification == SOL_MIR_RUNTIME_EQUALITY_WRAPPER) {
        if (ownership->edges.count != 1 || node->child_count != 1 || node->children[0] == NULL
            || node->children[0]->physical_id != node->physical_id) {
            model->outcome = EQUALITY_MODEL_INVALID; return false;
        }
    }
    if (plan->classification == SOL_MIR_RUNTIME_EQUALITY_PRODUCT
        || plan->classification == SOL_MIR_RUNTIME_EQUALITY_SUM
        || plan->classification == SOL_MIR_RUNTIME_EQUALITY_WRAPPER) {
        if (node->child_count != edges.count || edges.count > COPY_MODEL_CHILD_CAPACITY) {
            model->outcome = EQUALITY_MODEL_INVALID; return false;
        }
        ++model->depth;
        for (size_t i = 0; i < edges.count; ++i) {
            const SolMirRuntimeOwnedEdge *edge = &model->values->owned_edges[edges.offset + i];
            if (node->children[i] == NULL || node->children[i]->recipe != edge->recipe
                || !equality_model_validate(model, node->children[i])) {
                --model->depth; return false;
            }
        }
        --model->depth;
    }
    return true;
}

static bool equality_model_compare(EqualityModel *model, const CopyModelNode *left,
    const CopyModelNode *right) {
    if (!equality_model_tick(model, 1) || left->recipe != right->recipe) return false;
    const SolMirRuntimeEqualityPlan *plan = &model->values->equality_plans[left->recipe];
    if (plan->classification == SOL_MIR_RUNTIME_EQUALITY_TRIVIAL) {
        SolMirRecipeKind kind = model->values->conventions->concrete->representation
            .recipes[left->recipe].kind;
        return kind != SOL_MIR_RECIPE_INT64 && kind != SOL_MIR_RECIPE_BOOL
            ? true : left->scalar == right->scalar;
    }
    if (plan->classification == SOL_MIR_RUNTIME_EQUALITY_TEXT) {
        if (left->text_length != right->text_length
            || !equality_model_tick(model, (size_t)left->text_length)) return false;
        return memcmp(left->text_bytes, right->text_bytes, (size_t)left->text_length) == 0;
    }
    const SolMirRuntimeOwnershipPlan *ownership = &model->values->ownership_plans[left->recipe];
    SolMirRuntimeSlice edges = ownership->edges;
    if (plan->classification == SOL_MIR_RUNTIME_EQUALITY_SUM) {
        if (left->semantic_tag != right->semantic_tag) return false;
        edges = model->values->ownership_variants[ownership->variants.offset
            + left->active_variant].edges;
    }
    if (plan->classification == SOL_MIR_RUNTIME_EQUALITY_WRAPPER)
        return equality_model_compare(model, left->children[0], right->children[0]);
    for (size_t i = 0; i < edges.count; ++i)
        if (!equality_model_compare(model, left->children[i], right->children[i])) return false;
    return true;
}

static EqualityModelOutcome equality_model_run(const SolMirRuntimeValues *values,
    const CopyModelNode *left, const CopyModelNode *right, size_t depth,
    size_t nodes, size_t work, const CopyModelNode **node_scratch,
    uintptr_t *physical_scratch, size_t scratch_capacity) {
    EqualityModel model = {.values = values, .max_depth = depth, .max_nodes = nodes,
        .max_work = work, .nodes = node_scratch, .physical = physical_scratch,
        .scratch_capacity = scratch_capacity, .outcome = EQUALITY_MODEL_INVALID};
    if (depth > 256 || nodes > 1048576 || work > 4000000)
        return EQUALITY_MODEL_INVALID;
    if ((nodes != 0 && (node_scratch == NULL || physical_scratch == NULL))
        || scratch_capacity == 0) return EQUALITY_MODEL_LIMIT;
    if (!equality_model_validate(&model, left)) return model.outcome;
    model.node_count = model.physical_count = 0;
    if (!equality_model_validate(&model, right)) return model.outcome;
    if (left->recipe != right->recipe) return EQUALITY_MODEL_INVALID;
    if (!equality_model_compare(&model, left, right))
        return model.outcome == EQUALITY_MODEL_LIMIT ? model.outcome : EQUALITY_MODEL_NOT_EQUAL;
    return EQUALITY_MODEL_EQUAL;
}

static void test_equality_model(const SolMirRuntimeValues *values) {
    SolMirRecipeId product = SOL_MIR_RECIPE_NONE, text = SOL_MIR_RECIPE_NONE;
    for (size_t i = 0; i < values->equality_plan_count; ++i) {
        if (values->equality_plans[i].classification == SOL_MIR_RUNTIME_EQUALITY_TEXT)
            text = i;
        if (values->equality_plans[i].classification == SOL_MIR_RUNTIME_EQUALITY_PRODUCT
            && is_two_text_field_slice(values, values->ownership_plans[i].edges))
            product = i;
    }
    CHECK(product != SOL_MIR_RECIPE_NONE && text != SOL_MIR_RECIPE_NONE);
    if (product == SOL_MIR_RECIPE_NONE || text == SOL_MIR_RECIPE_NONE) return;
    const CopyModelNode *node_scratch[1024];
    uintptr_t physical_scratch[1024];
    SolMirRecipeId integer = SOL_MIR_RECIPE_NONE, boolean = SOL_MIR_RECIPE_NONE;
    SolMirRecipeId unit = SOL_MIR_RECIPE_NONE, empty_product = SOL_MIR_RECIPE_NONE;
    const SolMirRepresentation *representation
        = &values->conventions->concrete->representation;
    for (size_t i = 0; i < values->equality_plan_count; ++i) {
        if (representation->recipes[i].kind == SOL_MIR_RECIPE_INT64) integer = i;
        if (representation->recipes[i].kind == SOL_MIR_RECIPE_BOOL) boolean = i;
        if (representation->recipes[i].kind == SOL_MIR_RECIPE_UNIT) unit = i;
        if ((representation->recipes[i].kind == SOL_MIR_RECIPE_TUPLE
                || representation->recipes[i].kind == SOL_MIR_RECIPE_RECORD)
            && representation->recipes[i].fields.count == 0
            && values->equality_plans[i].classification
                == SOL_MIR_RUNTIME_EQUALITY_PRODUCT) empty_product = i;
    }
    CHECK(integer != SOL_MIR_RECIPE_NONE && boolean != SOL_MIR_RECIPE_NONE
        && empty_product != SOL_MIR_RECIPE_NONE);
    if (integer != SOL_MIR_RECIPE_NONE && boolean != SOL_MIR_RECIPE_NONE
        && empty_product != SOL_MIR_RECIPE_NONE) {
        CopyModelNode left_scalar, right_scalar, left_unit, right_unit, left_empty,
            right_empty;
        copy_model_node(&left_scalar, integer, 80); copy_model_node(&right_scalar, integer, 81);
        left_scalar.scalar = right_scalar.scalar = 7;
        CHECK(equality_model_run(values, &left_scalar, &right_scalar, 256, 1048576,
            4000000, node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_EQUAL);
        right_scalar.scalar = 8;
        CHECK(equality_model_run(values, &left_scalar, &right_scalar, 256, 1048576,
            4000000, node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_NOT_EQUAL);
        copy_model_node(&left_scalar, boolean, 82); copy_model_node(&right_scalar, boolean, 83);
        left_scalar.scalar = 0; right_scalar.scalar = 1;
        CHECK(equality_model_run(values, &left_scalar, &right_scalar, 256, 1048576,
            4000000, node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_NOT_EQUAL);
        right_scalar.scalar = 2;
        CHECK(equality_model_run(values, &left_scalar, &right_scalar, 256, 1048576,
            4000000, node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_INVALID);
        if (unit != SOL_MIR_RECIPE_NONE) {
            copy_model_node(&left_unit, unit, 84); copy_model_node(&right_unit, unit, 85);
            left_unit.scalar = 1; right_unit.scalar = 2;
            CHECK(equality_model_run(values, &left_unit, &right_unit, 256, 1048576,
                4000000, node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_EQUAL);
        }
        copy_model_node(&left_empty, empty_product, 86);
        copy_model_node(&right_empty, empty_product, 87);
        left_empty.scalar = 3; right_empty.scalar = 9;
        CHECK(equality_model_run(values, &left_empty, &right_empty, 256, 1048576,
            4000000, node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_EQUAL);
    }
    CopyModelNode a, b, c, d, left, right;
    copy_model_node(&a, text, 1); copy_model_node(&b, text, 2);
    copy_model_node(&c, text, 3); copy_model_node(&d, text, 4);
    a.text_length = b.text_length = c.text_length = d.text_length = 2;
    memcpy(a.text_bytes, "a\0", 2); memcpy(b.text_bytes, "b\0", 2);
    memcpy(c.text_bytes, "a\0", 2); memcpy(d.text_bytes, "b\0", 2);
    copy_model_node(&left, product, 10); copy_model_node(&right, product, 11);
    left.children[0] = &a; left.children[1] = &b; left.child_count = 2;
    right.children[0] = &c; right.children[1] = &d; right.child_count = 2;
    CopyModelNode zero_text, zero_text_other;
    copy_model_node(&zero_text, text, 90); copy_model_node(&zero_text_other, text, 91);
    CHECK(equality_model_run(values, &zero_text, &zero_text_other, 256, 1048576,
        4000000, node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_EQUAL);
    zero_text_other.text_length = 1; zero_text_other.text_bytes[0] = 'a';
    CHECK(equality_model_run(values, &zero_text, &zero_text_other, 256, 1048576,
        4000000, node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_NOT_EQUAL);
    SolMirRecipeId zeros = SOL_MIR_RECIPE_NONE;
    for (size_t i = 0; i < values->equality_plan_count; ++i) {
        const SolMirRuntimeOwnershipPlan *ownership = &values->ownership_plans[i];
        if (values->equality_plans[i].classification == SOL_MIR_RUNTIME_EQUALITY_PRODUCT
            && ownership->edges.count == 2
            && values->allocation_plans[i].kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE
            && values->allocation_plans[values->owned_edges[ownership->edges.offset].recipe]
                .kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE
            && values->allocation_plans[values->owned_edges[ownership->edges.offset + 1].recipe]
                .kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE) { zeros = i; break; }
    }
    CHECK(zeros != SOL_MIR_RECIPE_NONE);
    if (zeros != SOL_MIR_RECIPE_NONE) {
        const SolMirRuntimeOwnershipPlan *ownership = &values->ownership_plans[zeros];
        SolMirRecipeId child = values->owned_edges[ownership->edges.offset].recipe;
        CopyModelNode z0, z1, z2, z3, zl, zr;
        copy_model_node(&z0, child, 0); copy_model_node(&z1, child, 0);
        copy_model_node(&z2, child, 0); copy_model_node(&z3, child, 0);
        z0.scalar = 1; z1.scalar = 2; z2.scalar = 3; z3.scalar = 4;
        copy_model_node(&zl, zeros, 0); copy_model_node(&zr, zeros, 0);
        zl.children[0] = &z0; zl.children[1] = &z1;
        zr.children[0] = &z2; zr.children[1] = &z3;
        zl.child_count = zr.child_count = 2;
        CHECK(equality_model_run(values, &zl, &zr, 256, 1048576, 4000000,
            node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_EQUAL);
    }
    CHECK(equality_model_run(values, &left, &right, 256, 1048576, 4000000,
        node_scratch, physical_scratch, 1024)
        == EQUALITY_MODEL_EQUAL);
    CopyModelNode snapshot_left = left, snapshot_right = right, snapshot_a = a,
        snapshot_b = b, snapshot_c = c, snapshot_d = d;
    SolMirRuntimeValuesUsage snapshot_values_usage = values->usage;
    SolMirRuntimeAllocationUsage snapshot_allocation_usage = {17, 29};
    SolMirRuntimeAllocationQuota snapshot_allocation_quota = {31, 47};
    CHECK(equality_model_run(values, &left, &right, 256, 1048576, 4000000,
        node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_EQUAL);
    CHECK(equality_model_run(values, &left, &left, 256, 1048576, 4000000,
        node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_EQUAL);
    CHECK(memcmp(&left, &snapshot_left, sizeof(left)) == 0
        && memcmp(&right, &snapshot_right, sizeof(right)) == 0
        && memcmp(&a, &snapshot_a, sizeof(a)) == 0
        && memcmp(&b, &snapshot_b, sizeof(b)) == 0
        && memcmp(&c, &snapshot_c, sizeof(c)) == 0
        && memcmp(&d, &snapshot_d, sizeof(d)) == 0
        && memcmp(&values->usage, &snapshot_values_usage,
            sizeof(snapshot_values_usage)) == 0
        && snapshot_allocation_usage.requests == 17
        && snapshot_allocation_usage.bytes == 29
        && snapshot_allocation_quota.max_requests == 31
        && snapshot_allocation_quota.max_bytes == 47);
    b.physical_id = a.physical_id;
    CHECK(equality_model_run(values, &left, &right, 256, 1048576, 4000000,
        node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_INVALID);
    b.physical_id = snapshot_b.physical_id;
    /* Two three-node validations and three paired comparisons: 21 exact work. */
    CHECK(equality_model_run(values, &left, &right, 256, 1048576, 21,
        node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_EQUAL);
    CHECK(equality_model_run(values, &left, &right, 256, 1048576, 20,
        node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_LIMIT);
    CHECK(equality_model_run(values, &a, &left, 256, 1048576, 4000000,
        node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_INVALID);
    SolMirRecipeId wrapper = SOL_MIR_RECIPE_NONE;
    for (size_t i = 0; i < values->equality_plan_count; ++i)
        if (values->equality_plans[i].classification == SOL_MIR_RUNTIME_EQUALITY_WRAPPER
            && values->owned_edges[values->ownership_plans[i].edges.offset].recipe == text) {
            wrapper = i; break;
        }
    CHECK(wrapper != SOL_MIR_RECIPE_NONE);
    if (wrapper != SOL_MIR_RECIPE_NONE) {
        CopyModelNode wrapped_left, wrapped_right, backing_left, backing_right;
        copy_model_node(&backing_left, text, 300); copy_model_node(&backing_right, text, 301);
        backing_left.text_length = backing_right.text_length = 1;
        backing_left.text_bytes[0] = backing_right.text_bytes[0] = 'w';
        copy_model_node(&wrapped_left, wrapper, 300); copy_model_node(&wrapped_right, wrapper, 301);
        wrapped_left.children[0] = &backing_left; wrapped_right.children[0] = &backing_right;
        wrapped_left.child_count = wrapped_right.child_count = 1;
        CHECK(equality_model_run(values, &wrapped_left, &wrapped_right, 256, 1048576,
            4000000, node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_EQUAL);
        backing_right.text_bytes[0] = 'x';
        CHECK(equality_model_run(values, &wrapped_left, &wrapped_right, 256, 1048576,
            4000000, node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_NOT_EQUAL);
    }
    SolMirRecipeId callable = SOL_MIR_RECIPE_NONE, capability = SOL_MIR_RECIPE_NONE;
    SolMirRecipeId forbidden_composite = SOL_MIR_RECIPE_NONE;
    for (size_t i = 0; i < values->equality_plan_count; ++i) {
        SolMirRecipeKind kind = representation->recipes[i].kind;
        if (kind == SOL_MIR_RECIPE_FUNCTION) callable = i;
        else if (kind == SOL_MIR_RECIPE_CAPABILITY) capability = i;
        else if (values->equality_plans[i].classification
                == SOL_MIR_RUNTIME_EQUALITY_FORBIDDEN
            && (kind == SOL_MIR_RECIPE_RECORD || kind == SOL_MIR_RECIPE_ENUM))
            forbidden_composite = i;
    }
    const SolMirRecipeId forbidden[] = {callable, capability, forbidden_composite};
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        CHECK(forbidden[i] != SOL_MIR_RECIPE_NONE);
        if (forbidden[i] != SOL_MIR_RECIPE_NONE) {
            CopyModelNode invalid; copy_model_node(&invalid, forbidden[i], 400 + i);
            CHECK(equality_model_run(values, &invalid, &invalid, 256, 1048576,
                4000000, node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_INVALID);
        }
    }
    SolMirRecipeId sum = SOL_MIR_RECIPE_NONE;
    size_t active = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < values->equality_plan_count; ++i) {
        const SolMirRuntimeOwnershipPlan *ownership = &values->ownership_plans[i];
        if (values->equality_plans[i].classification != SOL_MIR_RUNTIME_EQUALITY_SUM)
            continue;
        for (size_t variant = 0; variant < ownership->variants.count; ++variant)
            if (is_two_text_field_slice(values, values->ownership_variants[
                    ownership->variants.offset + variant].edges)) {
                sum = i; active = variant; break;
            }
        if (sum != SOL_MIR_RECIPE_NONE) break;
    }
    CHECK(sum != SOL_MIR_RECIPE_NONE);
    if (sum != SOL_MIR_RECIPE_NONE) {
        const SolMirRuntimeOwnershipPlan *ownership = &values->ownership_plans[sum];
        CopyModelNode sum_left, sum_right;
        copy_model_node(&sum_left, sum, 500); copy_model_node(&sum_right, sum, 501);
        sum_left.active_variant = sum_right.active_variant = active;
        sum_left.semantic_tag = sum_right.semantic_tag = values->ownership_variants[
            ownership->variants.offset + active].semantic_tag;
        sum_left.children[0] = &a; sum_left.children[1] = &b;
        sum_right.children[0] = &c; sum_right.children[1] = &d;
        sum_left.child_count = sum_right.child_count = 2;
        sum_left.inactive_noise = UINT64_MAX;
        sum_right.inactive_noise = 0;
        CHECK(equality_model_run(values, &sum_left, &sum_right, 256, 1048576,
            4000000, node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_EQUAL);
        sum_right.children[1] = &a;
        CHECK(equality_model_run(values, &sum_left, &sum_right, 256, 1048576,
            4000000, node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_NOT_EQUAL);
        sum_right.children[1] = NULL; sum_right.child_count = 2;
        CHECK(equality_model_run(values, &sum_left, &sum_right, 256, 1048576,
            4000000, node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_INVALID);
        sum_right.children[1] = &d;
        sum_right.semantic_tag = SIZE_MAX;
        CHECK(equality_model_run(values, &sum_left, &sum_right, 256, 1048576,
            4000000, node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_INVALID);
    }
    d.text_bytes[0] = 'x';
    CHECK(equality_model_run(values, &left, &right, 256, 1048576, 4000000,
        node_scratch, physical_scratch, 1024)
        == EQUALITY_MODEL_NOT_EQUAL);
    /* A malformed later child still invalidates an otherwise unequal pair. */
    d.live = false;
    CHECK(equality_model_run(values, &left, &right, 256, 1048576, 4000000,
        node_scratch, physical_scratch, 1024)
        == EQUALITY_MODEL_INVALID);
    d.live = true; d.text_bytes[0] = 'b';
    right.children[0] = &a; /* Cross-operand sharing is permitted. */
    CHECK(equality_model_run(values, &left, &right, 256, 1048576, 4000000,
        node_scratch, physical_scratch, 1024)
        == EQUALITY_MODEL_EQUAL);
    right.children[0] = &c; left.children[1] = &a;
    CHECK(equality_model_run(values, &left, &right, 256, 1048576, 4000000,
        node_scratch, physical_scratch, 1024)
        == EQUALITY_MODEL_INVALID);
    left.children[1] = &b;
    CHECK(equality_model_run(values, &left, &right, 1, 1048576, 4000000,
        node_scratch, physical_scratch, 1024)
        == EQUALITY_MODEL_LIMIT);
    CHECK(equality_model_run(values, &left, &right, 256, 1, 4000000,
        node_scratch, physical_scratch, 1024)
        == EQUALITY_MODEL_LIMIT);
    CHECK(equality_model_run(values, &left, &right, 256, 1048576, 1,
        node_scratch, physical_scratch, 1024)
        == EQUALITY_MODEL_LIMIT);
    CHECK(equality_model_run(values, &left, &right, 257, 1048576, 4000000,
        node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_INVALID);
    CHECK(equality_model_run(values, &left, &right, 256, 1048577, 4000000,
        node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_INVALID);
    CHECK(equality_model_run(values, &left, &right, 256, 1048576, 4000001,
        node_scratch, physical_scratch, 1024) == EQUALITY_MODEL_INVALID);
    SolMirRecipeId recursive = SOL_MIR_RECIPE_NONE;
    size_t leaf = SOL_MIR_RUNTIME_NONE, branch = SOL_MIR_RUNTIME_NONE;
    for (size_t recipe = 0; recipe < values->equality_plan_count; ++recipe) {
        const SolMirRuntimeOwnershipPlan *ownership = &values->ownership_plans[recipe];
        if (values->equality_plans[recipe].classification != SOL_MIR_RUNTIME_EQUALITY_SUM)
            continue;
        for (size_t variant = 0; variant < ownership->variants.count; ++variant) {
            const SolMirRuntimeOwnershipVariant *candidate = &values->ownership_variants[
                ownership->variants.offset + variant];
            if (candidate->edges.count == 0) leaf = variant;
            else if (candidate->edges.count == 2 && values->owned_edges[
                    candidate->edges.offset].recipe == recipe
                && values->owned_edges[candidate->edges.offset + 1].recipe == recipe)
                branch = variant;
        }
        if (leaf != SOL_MIR_RUNTIME_NONE && branch != SOL_MIR_RUNTIME_NONE) {
            recursive = recipe; break;
        }
        leaf = branch = SOL_MIR_RUNTIME_NONE;
    }
    CHECK(recursive != SOL_MIR_RECIPE_NONE);
    if (recursive != SOL_MIR_RECIPE_NONE) {
        enum { EQUALITY_LONG_NODES = 513 };
        CopyModelNode long_left[EQUALITY_LONG_NODES], long_right[EQUALITY_LONG_NODES];
        const SolMirRuntimeOwnershipPlan *ownership = &values->ownership_plans[recursive];
        for (size_t i = 0; i < EQUALITY_LONG_NODES; ++i) {
            copy_model_node(&long_left[i], recursive, 1000 + i);
            copy_model_node(&long_right[i], recursive, 2000 + i);
            long_left[i].active_variant = long_right[i].active_variant
                = i < 256 ? branch : leaf;
            long_left[i].semantic_tag = long_right[i].semantic_tag
                = values->ownership_variants[ownership->variants.offset
                    + long_left[i].active_variant].semantic_tag;
            if (i < 256) {
                long_left[i].children[0] = &long_left[i * 2 + 1];
                long_left[i].children[1] = &long_left[i * 2 + 2];
                long_right[i].children[0] = &long_right[i * 2 + 1];
                long_right[i].children[1] = &long_right[i * 2 + 2];
                long_left[i].child_count = long_right[i].child_count = 2;
            }
        }
        const CopyModelNode *long_scratch[EQUALITY_LONG_NODES];
        uintptr_t long_physical[EQUALITY_LONG_NODES];
        CHECK(equality_model_run(values, &long_left[0], &long_right[0], 256,
            EQUALITY_LONG_NODES, 4000000, long_scratch, long_physical,
            EQUALITY_LONG_NODES) == EQUALITY_MODEL_EQUAL);
        CHECK(equality_model_run(values, &long_left[256], &long_right[256], 256,
            EQUALITY_LONG_NODES, 4000000, long_scratch, long_physical,
            EQUALITY_LONG_NODES) == EQUALITY_MODEL_EQUAL);
        CHECK(equality_model_run(values, &long_left[0], &long_right[256], 256,
            EQUALITY_LONG_NODES, 4000000, long_scratch, long_physical,
            EQUALITY_LONG_NODES) == EQUALITY_MODEL_NOT_EQUAL);
        CHECK(equality_model_run(values, &long_left[0], &long_right[0], 256,
            EQUALITY_LONG_NODES - 1, 4000000, long_scratch, long_physical,
            EQUALITY_LONG_NODES) == EQUALITY_MODEL_LIMIT);
    }
    SolMirRecipeId chain_recipe = SOL_MIR_RECIPE_NONE;
    size_t chain_leaf = SOL_MIR_RUNTIME_NONE, chain_next = SOL_MIR_RUNTIME_NONE;
    for (size_t recipe = 0; recipe < values->equality_plan_count; ++recipe) {
        const SolMirRuntimeOwnershipPlan *ownership = &values->ownership_plans[recipe];
        if (values->equality_plans[recipe].classification != SOL_MIR_RUNTIME_EQUALITY_SUM)
            continue;
        for (size_t variant = 0; variant < ownership->variants.count; ++variant) {
            const SolMirRuntimeOwnershipVariant *candidate = &values->ownership_variants[
                ownership->variants.offset + variant];
            if (candidate->edges.count == 0) chain_leaf = variant;
            else if (candidate->edges.count == 1 && values->owned_edges[
                    candidate->edges.offset].recipe == recipe) chain_next = variant;
        }
        if (chain_leaf != SOL_MIR_RUNTIME_NONE && chain_next != SOL_MIR_RUNTIME_NONE) {
            chain_recipe = recipe; break;
        }
        chain_leaf = chain_next = SOL_MIR_RUNTIME_NONE;
    }
    CHECK(chain_recipe != SOL_MIR_RECIPE_NONE);
    if (chain_recipe != SOL_MIR_RECIPE_NONE) {
        enum { EQUALITY_DEPTH_NODES = 256 };
        CopyModelNode chain_left[EQUALITY_DEPTH_NODES], chain_right[EQUALITY_DEPTH_NODES];
        const SolMirRuntimeOwnershipPlan *ownership = &values->ownership_plans[chain_recipe];
        for (size_t i = 0; i < EQUALITY_DEPTH_NODES; ++i) {
            copy_model_node(&chain_left[i], chain_recipe, 3000 + i);
            copy_model_node(&chain_right[i], chain_recipe, 4000 + i);
            chain_left[i].active_variant = chain_right[i].active_variant
                = i + 1 == EQUALITY_DEPTH_NODES ? chain_leaf : chain_next;
            chain_left[i].semantic_tag = chain_right[i].semantic_tag
                = values->ownership_variants[ownership->variants.offset
                    + chain_left[i].active_variant].semantic_tag;
            if (i + 1 < EQUALITY_DEPTH_NODES) {
                chain_left[i].children[0] = &chain_left[i + 1];
                chain_right[i].children[0] = &chain_right[i + 1];
                chain_left[i].child_count = chain_right[i].child_count = 1;
            }
        }
        const CopyModelNode *chain_scratch[EQUALITY_DEPTH_NODES];
        uintptr_t chain_physical[EQUALITY_DEPTH_NODES];
        CHECK(equality_model_run(values, &chain_left[0], &chain_right[0], 256,
            EQUALITY_DEPTH_NODES, 4000000, chain_scratch, chain_physical,
            EQUALITY_DEPTH_NODES) == EQUALITY_MODEL_EQUAL);
        CHECK(equality_model_run(values, &chain_left[0], &chain_right[0], 255,
            EQUALITY_DEPTH_NODES, 4000000, chain_scratch, chain_physical,
            EQUALITY_DEPTH_NODES) == EQUALITY_MODEL_LIMIT);
        chain_left[1].children[0] = &chain_left[0];
        CHECK(equality_model_run(values, &chain_left[0], &chain_right[0], 256,
            EQUALITY_DEPTH_NODES, 4000000, chain_scratch, chain_physical,
            EQUALITY_DEPTH_NODES) == EQUALITY_MODEL_INVALID);
    }
}

int main(void) {
    Compilation compilation;
    CHECK(compile_e6(&compilation));
    SolMirConcreteProgram programs[2];
    SolMirRuntimeConventions conventions[2];
    SolMirRuntimeValues values[2];
    char *rendered[2] = {NULL, NULL};
    for (size_t i = 0; i < 2; ++i) {
        sol_mir_concrete_program_init(&programs[i]);
        sol_mir_runtime_conventions_init(&conventions[i]);
        sol_mir_runtime_values_init(&values[i]);
    }
    for (size_t i = 0; i < 2; ++i) {
        bool built = build_concrete(&compilation, i != 0, &programs[i]);
        CHECK(built);
        CHECK(built && build_values(&programs[i], &compilation.diagnostics,
            &conventions[i], &values[i]));
        if (values[i].conventions != NULL) rendered[i] = render_values(&values[i]);
        CHECK(rendered[i] != NULL);
    }
    if (values[0].conventions != NULL)
        test_inventory(&programs[0], &compilation.diagnostics,
            &conventions[0], &values[0]);
    CHECK(rendered[0] != NULL && rendered[1] != NULL
        && strcmp(rendered[0], rendered[1]) == 0);
    for (size_t i = 0; i < 2; ++i) {
        free(rendered[i]);
        sol_mir_runtime_values_free(&values[i]);
        sol_mir_runtime_conventions_free(&conventions[i]);
        sol_mir_concrete_program_free(&programs[i]);
    }
    compilation_free(&compilation);
    test_no_import_recipe_accounting();
    test_bound_environment_exclusion();
    test_host_result_result_fixture();
    test_forbidden_host_result_requirement();
    test_plan_classification_fixture();
    test_recursive_copy_transaction_model();
    if (failures != 0) {
        fprintf(stderr, "%d runtime values test(s) failed\n", failures);
        return 1;
    }
    return 0;
}
