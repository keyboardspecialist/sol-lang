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

static void check_ownership_plans(const SolMirRuntimeValues *values) {
    const SolMirRepresentation *r = &values->conventions->concrete->representation;
    CHECK(values->ownership_plan_count == r->recipe_count);
    for (size_t i = 0; i < values->ownership_plan_count; ++i) {
        const SolMirRuntimeOwnershipPlan *plan = &values->ownership_plans[i];
        const SolMirRecipe *recipe = &r->recipes[i];
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
    SolMirRuntimeAllocationRequest request = {none_recipe, 0};
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

static void test_inventory(SolMirConcreteProgram *program,
    SolDiagnostics *diagnostics, SolMirRuntimeConventions *conventions,
    SolMirRuntimeValues *values) {
    CHECK(values->recipe_operation_count == 21);
    CHECK(values->usage.records == 21 && values->usage.allocation_plans == 21
        && values->usage.ownership_plans == 21
        && values->usage.ownership_variants == 9 && values->usage.owned_edges == 12
        && values->usage.owned_bytes == 3360
        && values->usage.build_scratch_bytes == 52
        && values->usage.build_work == 253
        && values->usage.validation_scratch_bytes == 584692564
        && values->usage.validation_work == 73654894);
    test_allocation_plans(values);
    check_ownership_plans(values);
    CHECK(values->recipe_operation_count
        == program->representation.recipe_count);
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
        && strstr(first, "move-drop-execution=false") != NULL
        && strstr(first, " recipe=") == NULL
        && strstr(first, "capacity") == NULL
        && strstr(first, "producer=") == NULL
        && strstr(first, program->program.ir->source_path) == NULL);

    SolMirRuntimeValuesLimits exact = exact_limits(values);
    SolMirRuntimeValues limited;
    sol_mir_runtime_values_init(&limited);
    SolMirRuntimeValuesBuildRequest request = {conventions, &exact};
    CHECK(sol_mir_runtime_values_build(&request, &limited, diagnostics)
        == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED);
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
    for (size_t attempt = 3; attempt <= 5; ++attempt) {
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
        SolMirRuntimeAllocationRequest allocation_request = {none_recipe, 0};
        CHECK(none_recipe != SOL_MIR_RECIPE_NONE
            && sol_mir_runtime_values_check_allocation(&values[side],
                &allocation_request, &allocation_quota, &allocation_usage,
                &allocation_demand) == SOL_MIR_RUNTIME_ALLOCATION_SUCCEEDED
            && sol_mir_runtime_values_test_ownership_count_scans() == scans);
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

static void test_plan_classification_fixture(void) {
    static const char source[] =
        "module runtime_values_plan_classes\n"
        "record Empty {}\n"
        "enum Void {}\n"
        "record Pair { first: Text, second: Text }\n"
        "enum Choice { left(first: Text, second: Text), right(value: Pair) }\n"
        "type ScalarWrap = distinct Int64\n"
        "type TextWrap = distinct Text\n"
        "type AggregateWrap = distinct Pair\n"
        "capability Gate { function choose(value: Int64) -> Bool effects { pure } }\n"
        "capability DerivedGate derives_from source: capability Gate { "
        "function choose(value: Int64) -> Bool effects { pure } { return true } }\n"
        "function callback(value: Int64) -> Bool effects { pure } { return true }\n"
        "function root(scalar: ScalarWrap, text: TextWrap, aggregate: AggregateWrap, "
        "empty: Empty, impossible: Void, pair: Pair, choice: Choice, gate: capability Gate, "
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
        for (size_t i = 0; i < program.representation.recipe_count; ++i) {
            const SolMirRecipe *recipe = &program.representation.recipes[i];
            const SolMirRuntimeAllocationPlan *plan = &values.allocation_plans[i];
            if (recipe->kind == SOL_MIR_RECIPE_RECORD
                && recipe->fields.count == 0) {
                empty = true;
                CHECK(plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE);
            }
            if (recipe->kind == SOL_MIR_RECIPE_ENUM
                && recipe->variants.count == 0) {
                uninhabited = true;
                CHECK(plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE);
            }
            if (recipe->kind == SOL_MIR_RECIPE_DISTINCT
                && recipe->backing < program.representation.recipe_count) {
                SolMirRecipeKind backing
                    = program.representation.recipes[recipe->backing].kind;
                if (backing == SOL_MIR_RECIPE_INT64) {
                    scalar_wrapper = true;
                    CHECK(plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE);
                } else if (backing == SOL_MIR_RECIPE_TEXT) {
                    text_wrapper = true;
                    CHECK(plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_TEXT);
                } else if (backing == SOL_MIR_RECIPE_RECORD) {
                    aggregate_wrapper = true;
                    CHECK(plan->kind
                        == SOL_MIR_RUNTIME_ALLOCATION_PLAN_FIXED_OBJECT);
                }
            }
            if (recipe->kind == SOL_MIR_RECIPE_FUNCTION) {
                callable_layout = true;
                CHECK(plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE);
            }
            if (recipe->kind == SOL_MIR_RECIPE_CAPABILITY) {
                capability_layout = true;
                CHECK(plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE);
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
        }
        CHECK(empty && uninhabited && scalar_wrapper && text_wrapper && aggregate_wrapper
            && callable_layout && capability_layout
            && root_capability && derived_capability);
        test_plan_driven_trace_model(&values);
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
    test_bound_environment_exclusion();
    test_plan_classification_fixture();
    if (failures != 0) {
        fprintf(stderr, "%d runtime values test(s) failed\n", failures);
        return 1;
    }
    return 0;
}
