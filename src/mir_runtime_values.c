#include "sol/mir_runtime_values.h"
#include "mir_linkage_internal.h"
#include "mir_runtime_values_internal.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} Buffer;

static _Thread_local size_t metered_build_work;
static _Thread_local size_t metered_build_limit;
static _Thread_local size_t persistent_allocation_attempt;

#ifdef SOL_MIR_PLAN_TEST_HOOKS
static _Thread_local bool force_allocation_failure;
static _Thread_local size_t force_persistent_allocation_failure;

void sol_mir_runtime_values_test_force_allocation_failure(bool force) {
    force_allocation_failure = force;
}

void sol_mir_runtime_values_test_force_persistent_allocation_failure(
    size_t attempt) {
    force_persistent_allocation_failure = attempt;
}
#endif

static bool report(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) sol_diagnostics_add(diagnostics,
        "SOL-MIR-RUNTIME-VALUES-001", SOL_SEVERITY_ERROR, (SolSpan){0},
        message);
    return false;
}

static bool build_event(size_t amount) {
    if (amount > SIZE_MAX - metered_build_work) {
        return false;
    }
    metered_build_work += amount;
    if (metered_build_work > metered_build_limit) {
        return false;
    }
    return true;
}

static bool limits_zero(SolMirRuntimeValuesLimits value) {
    return value.max_records == 0 && value.max_allocation_plans == 0
        && value.max_owned_bytes == 0
        && value.max_build_scratch_bytes == 0 && value.max_build_work == 0
        && value.max_validation_scratch_bytes == 0
        && value.max_validation_work == 0;
}

static bool limits_complete(SolMirRuntimeValuesLimits value) {
    return value.max_records != 0 && value.max_allocation_plans != 0
        && value.max_owned_bytes != 0
        && value.max_build_scratch_bytes != 0 && value.max_build_work != 0
        && value.max_validation_scratch_bytes != 0
        && value.max_validation_work != 0;
}

static bool usage_zero(SolMirRuntimeValuesUsage value) {
    SolMirRuntimeValuesUsage zero = {0};
    return memcmp(&value, &zero, sizeof(value)) == 0;
}

static bool owner_empty(const SolMirRuntimeValues *values) {
    return values != NULL && values->conventions == NULL
        && values->recipe_operations == NULL
        && values->recipe_operation_count == 0
        && values->recipe_operation_capacity == 0
        && values->allocation_plans == NULL
        && values->allocation_plan_count == 0
        && values->allocation_plan_capacity == 0
        && limits_zero(values->limits) && usage_zero(values->usage);
}

void sol_mir_runtime_values_init(SolMirRuntimeValues *values) {
    if (values != NULL) memset(values, 0, sizeof(*values));
}

void sol_mir_runtime_values_free(SolMirRuntimeValues *values) {
    if (values == NULL) return;
    free(values->recipe_operations);
    free(values->allocation_plans);
    sol_mir_runtime_values_init(values);
}

SolMirRuntimeValuesLimits sol_mir_runtime_values_default_limits(void) {
    return (SolMirRuntimeValuesLimits){
        .max_records = 4000000,
        .max_allocation_plans = 4000000,
        .max_owned_bytes = 512u * 1024u * 1024u,
        .max_build_scratch_bytes = 256u * 1024u * 1024u,
        .max_build_work = (size_t)1000000000ULL,
        .max_validation_scratch_bytes = 1024u * 1024u * 1024u,
        .max_validation_work = (size_t)4000000000ULL,
    };
}

static uint32_t value_operation_mask(void) {
    return SOL_MIR_LINKAGE_RUNTIME_CREATE | SOL_MIR_LINKAGE_RUNTIME_COPY
        | SOL_MIR_LINKAGE_RUNTIME_DROP | SOL_MIR_LINKAGE_RUNTIME_EQUAL;
}

static SolMirRuntimeImportId *operation_import(
    SolMirRuntimeRecipeOperations *record, SolMirRuntimeImportKind kind) {
    switch (kind) {
        case SOL_MIR_RUNTIME_IMPORT_RECIPE_CREATE: return &record->create_import;
        case SOL_MIR_RUNTIME_IMPORT_RECIPE_COPY: return &record->copy_import;
        case SOL_MIR_RUNTIME_IMPORT_RECIPE_DROP: return &record->drop_import;
        case SOL_MIR_RUNTIME_IMPORT_RECIPE_EQUAL: return &record->equal_import;
        default: return NULL;
    }
}

SolMirRuntimeValuesBuildOutcome sol_mir_runtime_values_build(
    const SolMirRuntimeValuesBuildRequest *request, SolMirRuntimeValues *output,
    SolDiagnostics *diagnostics) {
    if (request == NULL || request->conventions == NULL || !owner_empty(output)
        || (request->limits != NULL && !limits_zero(*request->limits)
            && !limits_complete(*request->limits))) {
        report(diagnostics, "invalid runtime values build request or destination");
        return SOL_MIR_RUNTIME_VALUES_BUILD_INVALID_ARGUMENT;
    }
    SolMirRuntimeValues scratch;
    sol_mir_runtime_values_init(&scratch);
    scratch.conventions = request->conventions;
    scratch.limits = request->limits == NULL || limits_zero(*request->limits)
        ? sol_mir_runtime_values_default_limits() : *request->limits;
    SolMirRuntimeValuesBuildOutcome preflight
        = sol_mir_runtime_values_internal_preflight(scratch.conventions,
            &scratch.limits, &scratch.usage, diagnostics);
    if (preflight != SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED) {
        sol_mir_runtime_values_init(&scratch);
        return preflight;
    }
    metered_build_work = 0;
    metered_build_limit = scratch.limits.max_build_work;
    persistent_allocation_attempt = 0;
    const SolMirLinkage *linkage = &scratch.conventions->concrete->linkage;
    unsigned char *consumed = NULL;
    if (scratch.usage.build_scratch_bytes != 0) {
        if (!build_event(1)) goto exhausted;
        consumed = calloc(scratch.usage.build_scratch_bytes, 1);
    }
    if (scratch.usage.build_scratch_bytes != 0 && consumed == NULL) {
        if (diagnostics != NULL) diagnostics->allocation_failed = true;
        sol_mir_runtime_values_init(&scratch);
        report(diagnostics, "runtime values build scratch allocation failed");
        return SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED;
    }
    size_t count = scratch.usage.records;
    if (count != 0 && !build_event(1)) goto exhausted;
    if (count != 0) {
        ++persistent_allocation_attempt;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        if (!force_allocation_failure && persistent_allocation_attempt
                != force_persistent_allocation_failure)
#endif
            scratch.recipe_operations = calloc(count,
                sizeof(*scratch.recipe_operations));
    }
    if (count != 0 && scratch.recipe_operations == NULL) {
        free(consumed);
        if (diagnostics != NULL) diagnostics->allocation_failed = true;
        sol_mir_runtime_values_init(&scratch);
        report(diagnostics, "runtime values persistent allocation failed");
        return SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED;
    }
    scratch.recipe_operation_capacity = count;
    for (size_t i = 0; i < count; ++i) {
        if (!build_event(1)) goto exhausted;
        scratch.recipe_operations[i] = (SolMirRuntimeRecipeOperations){
            .recipe = i,
            .create_import = SOL_MIR_RUNTIME_NONE,
            .copy_import = SOL_MIR_RUNTIME_NONE,
            .drop_import = SOL_MIR_RUNTIME_NONE,
            .equal_import = SOL_MIR_RUNTIME_NONE,
        };
        ++scratch.recipe_operation_count;
    }
    if (count != 0 && !build_event(1)) goto exhausted;
    if (count != 0) {
        ++persistent_allocation_attempt;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        if (!force_allocation_failure && persistent_allocation_attempt
                != force_persistent_allocation_failure)
#endif
            scratch.allocation_plans = calloc(count,
                sizeof(*scratch.allocation_plans));
    }
    if (count != 0 && scratch.allocation_plans == NULL) {
        free(consumed);
        if (diagnostics != NULL) diagnostics->allocation_failed = true;
        sol_mir_runtime_values_free(&scratch);
        report(diagnostics, "runtime values allocation-plan persistent allocation failed");
        return SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED;
    }
    scratch.allocation_plan_capacity = count;
    const SolMirLayout *layout = &scratch.conventions->concrete->layout;
    for (size_t i = 0; i < count; ++i) {
        if (!build_event(1)) goto exhausted;
        const SolMirTypeLayout *type = &layout->types[i];
        SolMirRuntimeAllocationPlan *plan = &scratch.allocation_plans[i];
        plan->recipe = i;
        plan->object_alignment = 1;
        if (type->object_kind == SOL_MIR_LAYOUT_OBJECT_PRODUCT
            || type->object_kind == SOL_MIR_LAYOUT_OBJECT_SUM) {
            plan->kind = SOL_MIR_RUNTIME_ALLOCATION_PLAN_FIXED_OBJECT;
            plan->object_size = type->object_size;
            plan->object_alignment = type->object_alignment;
        } else if (type->object_kind == SOL_MIR_LAYOUT_OBJECT_TEXT) {
            plan->kind = SOL_MIR_RUNTIME_ALLOCATION_PLAN_TEXT;
            plan->object_size = type->object_size;
            plan->object_alignment = type->object_alignment;
        } else {
            plan->kind = SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE;
        }
        ++scratch.allocation_plan_count;
    }
    for (size_t i = 0; i < linkage->runtime_requirement_count; ++i) {
        if (!build_event(1)) goto exhausted;
        const SolMirLinkageRuntimeRequirement *requirement
            = &linkage->runtime_requirements[i];
        if (requirement->recipe >= count
            || (requirement->operations
                & ~(value_operation_mask()
                    | SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT)) != 0) goto internal;
        scratch.recipe_operations[requirement->recipe].demanded_operations
            = requirement->operations & value_operation_mask();
    }
    for (size_t i = 0; i < scratch.conventions->import_count; ++i) {
        if (!build_event(1)) goto exhausted;
        const SolMirRuntimeImport *import = &scratch.conventions->imports[i];
        SolMirRuntimeImportId *destination = NULL;
        if (import->recipe < count)
            destination = operation_import(
                &scratch.recipe_operations[import->recipe], import->kind);
        if (destination == NULL) continue;
        if (*destination != SOL_MIR_RUNTIME_NONE) goto internal;
        *destination = i;
        consumed[i] = 1;
    }
    size_t paired = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!build_event(1)) goto exhausted;
        const SolMirRuntimeRecipeOperations *record
            = &scratch.recipe_operations[i];
#define CHECK_IMPORT(flag, member) \
        if (((record->demanded_operations & (flag)) != 0) \
                != (record->member != SOL_MIR_RUNTIME_NONE)) goto internal; \
        paired += (record->demanded_operations & (flag)) != 0
        CHECK_IMPORT(SOL_MIR_LINKAGE_RUNTIME_CREATE, create_import);
        CHECK_IMPORT(SOL_MIR_LINKAGE_RUNTIME_COPY, copy_import);
        CHECK_IMPORT(SOL_MIR_LINKAGE_RUNTIME_DROP, drop_import);
        CHECK_IMPORT(SOL_MIR_LINKAGE_RUNTIME_EQUAL, equal_import);
#undef CHECK_IMPORT
    }
    size_t consumed_count = 0;
    for (size_t i = 0; i < scratch.usage.build_scratch_bytes; ++i) {
        if (!build_event(1)) goto exhausted;
        consumed_count += consumed[i] != 0;
    }
    if (consumed_count != paired) goto internal;
    if (metered_build_work != scratch.usage.build_work) goto internal;
    free(consumed);
    consumed = NULL;
    SolMirRuntimeValuesBuildOutcome validation
        = sol_mir_runtime_values_internal_validate(&scratch, diagnostics);
    if (validation != SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED) {
        sol_mir_runtime_values_free(&scratch);
        return validation == SOL_MIR_RUNTIME_VALUES_BUILD_INVALID_CONVENTIONS
            ? SOL_MIR_RUNTIME_VALUES_BUILD_INTERNAL_FAILED : validation;
    }
    *output = scratch;
    return SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED;
internal:
    free(consumed);
    sol_mir_runtime_values_free(&scratch);
    report(diagnostics, "runtime values construction invariant failed");
    return SOL_MIR_RUNTIME_VALUES_BUILD_INTERNAL_FAILED;
exhausted:
    free(consumed);
    sol_mir_runtime_values_free(&scratch);
    report(diagnostics, "runtime values build work limit exceeded");
    return SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED;
}

static void format(Buffer *buffer, const char *pattern, ...) {
    if (buffer->failed) return;
    va_list args;
    va_start(args, pattern);
    va_list copy;
    va_copy(copy, args);
    int count = vsnprintf(NULL, 0, pattern, copy);
    va_end(copy);
    if (count < 0 || (size_t)count > SIZE_MAX - buffer->length - 1) {
        buffer->failed = true;
        va_end(args);
        return;
    }
    size_t needed = buffer->length + (size_t)count + 1;
    if (needed > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
            capacity *= 2;
        }
        char *grown = realloc(buffer->data, capacity);
        if (grown == NULL) { buffer->failed = true; va_end(args); return; }
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    (void)vsnprintf(buffer->data + buffer->length,
        buffer->capacity - buffer->length, pattern, args);
    va_end(args);
    buffer->length += (size_t)count;
}

static void render_digest(Buffer *buffer, const SolMirLinkageDigest *digest) {
    for (size_t i = 0; i < SOL_MIR_LINKAGE_DIGEST_BYTES; ++i)
        format(buffer, "%02x", digest->bytes[i]);
}

typedef struct { Buffer text; } RenderLine;

static int compare_lines(const void *left, const void *right) {
    const RenderLine *a = left;
    const RenderLine *b = right;
    return strcmp(a->text.data, b->text.data);
}

bool sol_mir_runtime_values_render(FILE *stream,
    const SolMirRuntimeValues *values) {
    if (stream == NULL || !sol_mir_runtime_values_validate(values, NULL))
        return false;
    Buffer output = {0};
    format(&output, "mir_runtime_values\n");
    format(&output, "declaration.kind=operation-demand-allocation-plan-inventory executable-operations=false allocation-plans=true\n");
    size_t count = values->recipe_operation_count;
    RenderLine *lines = count == 0 ? NULL : calloc(count, sizeof(*lines));
    if (count != 0 && lines == NULL) { free(output.data); return false; }
    const SolMirLinkage *linkage = &values->conventions->concrete->linkage;
    for (size_t i = 0; i < count; ++i) {
        const SolMirRuntimeRecipeOperations *record
            = &values->recipe_operations[i];
        SolMirLinkageDigest digest;
        if (!sol_mir_linkage_internal_recipe_key(linkage, record->recipe,
                &digest, NULL)) {
            output.failed = true;
            break;
        }
        Buffer *line = &lines[i].text;
        format(line, "recipe digest=");
        render_digest(line, &digest);
        format(line, " demands=");
        bool first = true;
#define RENDER_OPERATION(flag, name, member) do { \
    if ((record->demanded_operations & (flag)) != 0) { \
        const SolMirRuntimeImport *import \
            = &values->conventions->imports[record->member]; \
        format(line, "%s%s:%s", first ? "" : "|", (name), \
            import->symbol.bytes); \
        first = false; \
    } \
} while (0)
        RENDER_OPERATION(SOL_MIR_LINKAGE_RUNTIME_CREATE, "create", create_import);
        RENDER_OPERATION(SOL_MIR_LINKAGE_RUNTIME_COPY, "copy", copy_import);
        RENDER_OPERATION(SOL_MIR_LINKAGE_RUNTIME_DROP, "drop", drop_import);
        RENDER_OPERATION(SOL_MIR_LINKAGE_RUNTIME_EQUAL, "equal", equal_import);
#undef RENDER_OPERATION
        if (first) format(line, "none");
        const SolMirRuntimeAllocationPlan *plan = &values->allocation_plans[i];
        const char *kind = plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE
            ? "none" : plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_FIXED_OBJECT
                ? "fixed-object" : "text";
        format(line, " allocation=%s size=%" PRIu64 " alignment=%" PRIu64,
            kind, plan->object_size, plan->object_alignment);
        format(line, "\n");
    }
    for (size_t i = 0; i < count; ++i)
        if (lines[i].text.failed) output.failed = true;
    if (!output.failed && count > 1)
        qsort(lines, count, sizeof(*lines), compare_lines);
    for (size_t i = 0; !output.failed && i < count; ++i)
        format(&output, "%s", lines[i].text.data);
    for (size_t i = 0; i < count; ++i) free(lines[i].text.data);
    free(lines);
    bool ok = !output.failed && (output.length == 0
        || fwrite(output.data, output.length, 1, stream) == 1);
    free(output.data);
    return ok;
}

static bool add_u64(uint64_t left, uint64_t right, uint64_t *result) {
    if (right > UINT64_MAX - left) return false;
    *result = left + right;
    return true;
}

static bool allocation_plan_range_valid(const SolMirRuntimeAllocationPlan *plans,
    size_t count) {
    if (count == 0) return plans == NULL;
    if (plans == NULL || count > SIZE_MAX / sizeof(*plans)) return false;
    size_t bytes = count * sizeof(*plans);
    return (uintptr_t)plans <= UINTPTR_MAX - bytes;
}

static bool operation_range_valid(const SolMirRuntimeRecipeOperations *records,
    size_t count) {
    if (count == 0) return records == NULL;
    if (records == NULL || count > SIZE_MAX / sizeof(*records)) return false;
    size_t bytes = count * sizeof(*records);
    return (uintptr_t)records <= UINTPTR_MAX - bytes;
}

static bool range_valid(const void *items, size_t count, size_t item_size) {
    if (count == 0) return items == NULL;
    if (items == NULL || count > SIZE_MAX / item_size) return false;
    size_t bytes = count * item_size;
    return (uintptr_t)items <= UINTPTR_MAX - bytes;
}

static bool ranges_overlap(const void *left, size_t left_size,
    const void *right, size_t right_size) {
    uintptr_t a = (uintptr_t)left, b = (uintptr_t)right;
    if (a > UINTPTR_MAX - left_size || b > UINTPTR_MAX - right_size)
        return true;
    return a < b + right_size && b < a + left_size;
}

static bool add_size(size_t *value, size_t amount) {
    if (amount > SIZE_MAX - *value) return false;
    *value += amount;
    return true;
}

static bool mul_size(size_t left, size_t right, size_t *result) {
    if (left != 0 && right > SIZE_MAX / left) return false;
    *result = left * right;
    return true;
}

static bool owner_header_valid(const SolMirRuntimeValues *values) {
    if (values == NULL || values->conventions == NULL
        || values->conventions->concrete == NULL) return false;
    const SolMirRuntimeConventions *conventions = values->conventions;
    const SolMirConcreteProgram *concrete = conventions->concrete;
    const SolMirRepresentation *representation = &concrete->representation;
    const SolMirLayout *layout = &concrete->layout;
    if (concrete->plan.program != &concrete->program
        || concrete->materialization.plan != &concrete->plan
        || representation->materialization != &concrete->materialization
        || layout->representation != representation
        || concrete->operations.layout != layout
        || concrete->linkage.operations != &concrete->operations
        || !limits_complete(values->limits)
        || values->recipe_operation_count != values->recipe_operation_capacity
        || values->allocation_plan_count != values->allocation_plan_capacity
        || values->recipe_operation_count != values->allocation_plan_count
        || values->recipe_operation_count != representation->recipe_count
        || representation->recipe_count != representation->recipe_capacity
        || layout->type_count != layout->type_capacity
        || layout->type_count != representation->recipe_count
        || conventions->import_count != conventions->import_capacity
        || concrete->linkage.runtime_requirement_count
            != concrete->linkage.runtime_requirement_capacity
        || !operation_range_valid(values->recipe_operations,
            values->recipe_operation_capacity)
        || !allocation_plan_range_valid(values->allocation_plans,
            values->allocation_plan_capacity)
        || !range_valid(representation->recipes, representation->recipe_capacity,
            sizeof(*representation->recipes))
        || !range_valid(layout->types, layout->type_capacity,
            sizeof(*layout->types))
        || !range_valid(conventions->imports, conventions->import_capacity,
            sizeof(*conventions->imports))
        || !range_valid(concrete->linkage.runtime_requirements,
            concrete->linkage.runtime_requirement_capacity,
            sizeof(*concrete->linkage.runtime_requirements))
        || !sol_mir_target_descriptor_validate(&layout->target)) return false;
    size_t owned_bytes, plan_bytes, build_work;
    if (!mul_size(values->recipe_operation_count,
            sizeof(*values->recipe_operations), &owned_bytes)
        || !mul_size(values->allocation_plan_count,
            sizeof(*values->allocation_plans), &plan_bytes)
        || !add_size(&owned_bytes, plan_bytes)
        || !mul_size(values->recipe_operation_count, 3, &build_work)
        || !add_size(&build_work,
            concrete->linkage.runtime_requirement_count)
        || !add_size(&build_work, conventions->import_count)
        || !add_size(&build_work, conventions->import_count)
        || (conventions->import_count != 0 && !add_size(&build_work, 1))
        || (values->recipe_operation_count != 0
            && (!add_size(&build_work, 1) || !add_size(&build_work, 1))))
        return false;
    size_t local_scratch = values->recipe_operation_count;
    size_t minimum_work = conventions->usage.validation_work;
    if (!add_size(&local_scratch, conventions->import_count)
        || !add_size(&minimum_work, 8)) return false;
    size_t expected_scratch = conventions->usage.validation_scratch_bytes;
    if (expected_scratch < local_scratch) expected_scratch = local_scratch;
    return values->usage.records == values->recipe_operation_count
        && values->usage.allocation_plans == values->allocation_plan_count
        && values->usage.owned_bytes == owned_bytes
        && values->usage.build_scratch_bytes == conventions->import_count
        && values->usage.build_work == build_work
        && values->usage.validation_scratch_bytes == expected_scratch
        && values->usage.validation_work >= minimum_work
        && values->usage.records <= values->limits.max_records
        && values->usage.allocation_plans <= values->limits.max_allocation_plans
        && values->usage.owned_bytes <= values->limits.max_owned_bytes
        && values->usage.build_scratch_bytes
            <= values->limits.max_build_scratch_bytes
        && values->usage.build_work <= values->limits.max_build_work
        && values->usage.validation_scratch_bytes
            <= values->limits.max_validation_scratch_bytes
        && values->usage.validation_work <= values->limits.max_validation_work;
}

static bool plan_is_valid(const SolMirRuntimeValues *values,
    const SolMirRuntimeAllocationPlan *plan, SolMirRecipeId recipe) {
    if (plan->recipe != recipe || plan->recipe >= values->allocation_plan_count
        || plan->recipe >= values->conventions->concrete->layout.type_count)
        return false;
    const SolMirTypeLayout *type
        = &values->conventions->concrete->layout.types[plan->recipe];
    if (plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE)
        return plan->object_size == 0 && plan->object_alignment == 1
            && (type->object_kind == SOL_MIR_LAYOUT_OBJECT_NONE
                || type->object_kind == SOL_MIR_LAYOUT_OBJECT_CALLABLE
                || type->object_kind == SOL_MIR_LAYOUT_OBJECT_CAPABILITY);
    if (plan->kind != SOL_MIR_RUNTIME_ALLOCATION_PLAN_FIXED_OBJECT
        && plan->kind != SOL_MIR_RUNTIME_ALLOCATION_PLAN_TEXT) return false;
    return plan->object_size == type->object_size
        && plan->object_alignment == type->object_alignment
        && plan->object_size != 0 && plan->object_alignment != 0
        && plan->object_size <= values->conventions->concrete->layout.target.max_object_bytes
        && (plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_TEXT
            ? type->object_kind == SOL_MIR_LAYOUT_OBJECT_TEXT
            : type->object_kind == SOL_MIR_LAYOUT_OBJECT_PRODUCT
                || type->object_kind == SOL_MIR_LAYOUT_OBJECT_SUM);
}

SolMirRuntimeAllocationOutcome sol_mir_runtime_values_check_allocation(
    const SolMirRuntimeValues *values,
    const SolMirRuntimeAllocationRequest *request,
    const SolMirRuntimeAllocationQuota *quota,
    const SolMirRuntimeAllocationUsage *usage,
    SolMirRuntimeAllocationDemand *demand) {
    if (request == NULL || quota == NULL || usage == NULL || demand == NULL
        || !owner_header_valid(values)
        || ranges_overlap(demand, sizeof(*demand), values, sizeof(*values))
        || ranges_overlap(demand, sizeof(*demand), request, sizeof(*request))
        || ranges_overlap(demand, sizeof(*demand), quota, sizeof(*quota))
        || ranges_overlap(demand, sizeof(*demand), usage, sizeof(*usage))
        || ranges_overlap(demand, sizeof(*demand), values->recipe_operations,
            values->recipe_operation_count * sizeof(*values->recipe_operations))
        || ranges_overlap(demand, sizeof(*demand), values->allocation_plans,
            values->allocation_plan_count * sizeof(*values->allocation_plans))
        || request->recipe >= values->allocation_plan_count
        || usage->requests > quota->max_requests
        || usage->bytes > quota->max_bytes) {
        return SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT;
    }
    const SolMirRuntimeAllocationPlan *plan
        = &values->allocation_plans[request->recipe];
    if (!plan_is_valid(values, plan, request->recipe)
        || (plan->kind != SOL_MIR_RUNTIME_ALLOCATION_PLAN_TEXT
            && request->text_length != 0))
        return SOL_MIR_RUNTIME_ALLOCATION_INVALID_ARGUMENT;
    uint64_t pointer_max = values->conventions->concrete->layout.target.pointer_size
        == 4 ? UINT32_MAX : UINT64_MAX;
    uint64_t requests = 0, bytes = 0;
    if (plan->kind != SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE) {
        requests = 1;
        bytes = plan->object_size;
    }
    if (plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_TEXT) {
        if (request->text_length > pointer_max
            || request->text_length > values->conventions->concrete->layout.target.max_object_bytes)
            return SOL_MIR_RUNTIME_ALLOCATION_LIMIT;
        if (request->text_length != 0
            && (!add_u64(requests, 1, &requests)
                || !add_u64(bytes, request->text_length, &bytes)))
            return SOL_MIR_RUNTIME_ALLOCATION_LIMIT;
    }
    uint64_t total_requests, total_bytes;
    if (!add_u64(usage->requests, requests, &total_requests)
        || !add_u64(usage->bytes, bytes, &total_bytes)
        || total_requests > quota->max_requests || total_bytes > quota->max_bytes)
        return SOL_MIR_RUNTIME_ALLOCATION_LIMIT;
    SolMirRuntimeAllocationDemand computed = {requests, bytes};
    *demand = computed;
    return SOL_MIR_RUNTIME_ALLOCATION_SUCCEEDED;
}

bool sol_mir_runtime_allocation_outcome_failure(
    SolMirRuntimeAllocationOutcome outcome, SolMirRuntimeFailureCode *failure) {
    if (failure == NULL) return false;
    if (outcome == SOL_MIR_RUNTIME_ALLOCATION_SUCCEEDED)
        *failure = SOL_MIR_RUNTIME_FAILURE_NONE;
    else if (outcome == SOL_MIR_RUNTIME_ALLOCATION_LIMIT)
        *failure = SOL_MIR_RUNTIME_FAILURE_ALLOCATION_LIMIT;
    else if (outcome == SOL_MIR_RUNTIME_ALLOCATION_FAILED)
        *failure = SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED;
    else return false;
    return true;
}
