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
    SolMirRuntimeAllocationUsage usage = {0};
    SolMirRuntimeAllocationQuota none_quota = {0, 0};
    SolMirRuntimeAllocationRequest request = {none_recipe, 0};
    CHECK(sol_mir_runtime_values_check_allocation(values, &request, &none_quota,
            &usage, &demand) == SOL_MIR_RUNTIME_ALLOCATION_SUCCEEDED
        && demand.requests == 0 && demand.bytes == 0);
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
        && values->usage.owned_bytes == 1680
        && values->usage.build_scratch_bytes == 52
        && values->usage.build_work == 187
        && values->usage.validation_scratch_bytes == 584692564
        && values->usage.validation_work == 73654852);
    test_allocation_plans(values);
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
        && strstr(first, " recipe=") == NULL
        && strstr(first, "capacity") == NULL
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

static void test_bound_environment_exclusion(void) {
    static const char source[] =
        "module runtime_values_bound_environment\n"
        "capability Base { function choose(value: Int64) -> Bool effects { pure } }\n"
        "function callback(value: Int64) -> Bool effects { pure } { return true }\n"
        "function root(base: capability Base) -> Bool effects { pure } "
        "requires { { let exact = callback let bound = base.choose "
        "exact(1) && bound(1) } } { return base.choose(1) }\n";
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
        for (size_t i = 0; i < values[side].recipe_operation_count; ++i)
            CHECK((values[side].recipe_operations[i].demanded_operations
                & SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT) == 0);
        CHECK(sol_mir_runtime_values_validate(&values[side], NULL));
        check_plan_layout_classification(&values[side]);
        rendered[side] = render_values(&values[side]);
        CHECK(rendered[side] != NULL);
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
        "record Pair { value: Int64 }\n"
        "type ScalarWrap = distinct Int64\n"
        "type TextWrap = distinct Text\n"
        "type AggregateWrap = distinct Pair\n"
        "capability Gate { function open() -> () effects { pure } }\n"
        "function callback(value: Int64) -> Bool effects { pure } { return true }\n"
        "function root(scalar: ScalarWrap, text: TextWrap, aggregate: AggregateWrap, "
        "empty: Empty, impossible: Void, gate: capability Gate, "
        "callback: function(Int64) -> Bool effects { pure }) -> () effects { pure } "
        "{ return () }\n";
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
        bool empty = false, uninhabited = false, scalar_wrapper = false;
        bool text_wrapper = false, aggregate_wrapper = false;
        bool callable_layout = false, capability_layout = false;
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
            }
        }
        CHECK(empty && uninhabited && scalar_wrapper && text_wrapper
            && aggregate_wrapper && callable_layout && capability_layout);
    }
    sol_mir_runtime_values_free(&values);
    sol_mir_runtime_conventions_free(&conventions);
    sol_mir_concrete_program_free(&program);
    text_compilation_free(&compilation);
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
