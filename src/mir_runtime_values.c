#include "sol/mir_runtime_values.h"
#include "mir_linkage_internal.h"
#include "mir_runtime_values_internal.h"

#include <stdarg.h>
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

#ifdef SOL_MIR_PLAN_TEST_HOOKS
static _Thread_local bool force_allocation_failure;

void sol_mir_runtime_values_test_force_allocation_failure(bool force) {
    force_allocation_failure = force;
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
    return value.max_records == 0 && value.max_owned_bytes == 0
        && value.max_build_scratch_bytes == 0 && value.max_build_work == 0
        && value.max_validation_scratch_bytes == 0
        && value.max_validation_work == 0;
}

static bool limits_complete(SolMirRuntimeValuesLimits value) {
    return value.max_records != 0 && value.max_owned_bytes != 0
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
        && limits_zero(values->limits) && usage_zero(values->usage);
}

void sol_mir_runtime_values_init(SolMirRuntimeValues *values) {
    if (values != NULL) memset(values, 0, sizeof(*values));
}

void sol_mir_runtime_values_free(SolMirRuntimeValues *values) {
    if (values == NULL) return;
    free(values->recipe_operations);
    sol_mir_runtime_values_init(values);
}

SolMirRuntimeValuesLimits sol_mir_runtime_values_default_limits(void) {
    return (SolMirRuntimeValuesLimits){
        .max_records = 4000000,
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
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (!force_allocation_failure)
#endif
        scratch.recipe_operations = count == 0 ? NULL
            : calloc(count, sizeof(*scratch.recipe_operations));
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
    format(&output, "declaration.kind=operation-demand-inventory executable-operations=false allocation-plans=false\n");
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
