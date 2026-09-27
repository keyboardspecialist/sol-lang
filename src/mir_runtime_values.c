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
static _Thread_local size_t ownership_count_scans;
static _Thread_local bool reverse_captured_fragments;
static _Thread_local bool force_captured_digest_collision;

void sol_mir_runtime_values_test_force_allocation_failure(bool force) {
    force_allocation_failure = force;
}

void sol_mir_runtime_values_test_force_persistent_allocation_failure(
    size_t attempt) {
    force_persistent_allocation_failure = attempt;
}

size_t sol_mir_runtime_values_test_ownership_count_scans(void) {
    return ownership_count_scans;
}

void sol_mir_runtime_values_test_reverse_captured_fragments(bool reverse) {
    reverse_captured_fragments = reverse;
}

void sol_mir_runtime_values_test_force_captured_digest_collision(bool force) {
    force_captured_digest_collision = force;
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
        && value.max_copy_plans == 0 && value.max_equality_plans == 0
        && value.max_ownership_plans == 0 && value.max_ownership_variants == 0
        && value.max_owned_edges == 0
        && value.max_owned_bytes == 0
        && value.max_build_scratch_bytes == 0 && value.max_build_work == 0
        && value.max_validation_scratch_bytes == 0
        && value.max_validation_work == 0;
}

static bool limits_complete(SolMirRuntimeValuesLimits value) {
    return value.max_records != 0 && value.max_allocation_plans != 0
        && value.max_copy_plans != 0 && value.max_equality_plans != 0
        && value.max_ownership_plans != 0 && value.max_ownership_variants != 0
        && value.max_owned_edges != 0
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
        && values->copy_plans == NULL
        && values->copy_plan_count == 0
        && values->copy_plan_capacity == 0
        && values->equality_plans == NULL
        && values->equality_plan_count == 0
        && values->equality_plan_capacity == 0
        && values->ownership_plans == NULL
        && values->ownership_plan_count == 0
        && values->ownership_plan_capacity == 0
        && values->ownership_variants == NULL
        && values->ownership_variant_count == 0
        && values->ownership_variant_capacity == 0
        && values->owned_edges == NULL
        && values->owned_edge_count == 0
        && values->owned_edge_capacity == 0
        && limits_zero(values->limits) && usage_zero(values->usage);
}

void sol_mir_runtime_values_init(SolMirRuntimeValues *values) {
    if (values != NULL) memset(values, 0, sizeof(*values));
}

void sol_mir_runtime_values_free(SolMirRuntimeValues *values) {
    if (values == NULL) return;
    free(values->recipe_operations);
    free(values->allocation_plans);
    free(values->copy_plans);
    free(values->equality_plans);
    free(values->ownership_plans);
    free(values->ownership_variants);
    free(values->owned_edges);
    sol_mir_runtime_values_init(values);
}

SolMirRuntimeValuesLimits sol_mir_runtime_values_default_limits(void) {
    return (SolMirRuntimeValuesLimits){
        .max_records = 4000000,
        .max_allocation_plans = 4000000,
        .max_copy_plans = 4000000,
        .max_equality_plans = 4000000,
        .max_ownership_plans = 4000000,
        .max_ownership_variants = 4000000,
        .max_owned_edges = 16000000,
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

/* This is deliberately construction-local. Validation reconstructs the same
   facts from P2 independently rather than calling a builder helper. */
static bool ownership_counts(const SolMirRepresentation *representation,
    size_t *variants, size_t *edges, size_t *producer_scans) {
    *variants = 0; *edges = 0; *producer_scans = 0;
    for (size_t recipe = 0; recipe < representation->recipe_count; ++recipe) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        ++ownership_count_scans;
#endif
        const SolMirRecipe *item = &representation->recipes[recipe];
        size_t add = 0;
        if (!item->inhabited) continue;
        if (item->kind == SOL_MIR_RECIPE_TUPLE
            || item->kind == SOL_MIR_RECIPE_RECORD) add = item->fields.count;
        else if (item->kind == SOL_MIR_RECIPE_ENUM
            || item->kind == SOL_MIR_RECIPE_OPTION
            || item->kind == SOL_MIR_RECIPE_RESULT) {
            if (item->variants.count > SIZE_MAX - *variants) return false;
            *variants += item->variants.count;
            for (size_t v = 0; v < item->variants.count; ++v) {
                const SolMirRecipeVariant *variant = &representation->variants[
                    item->variants.offset + v];
                if (variant->fields.count > SIZE_MAX - add) return false;
                add += variant->fields.count;
            }
        } else if (item->kind == SOL_MIR_RECIPE_DISTINCT
            || item->kind == SOL_MIR_RECIPE_REFINED) add = 1;
        else if (item->kind == SOL_MIR_RECIPE_CAPABILITY)
            add = item->capability_source == SOL_MIR_RECIPE_NONE ? 0 : 1;
        else if (item->kind == SOL_MIR_RECIPE_FUNCTION) {
            if (representation->callable_producer_count
                    > SIZE_MAX - *producer_scans) return false;
            *producer_scans += representation->callable_producer_count;
            for (size_t p = 0; p < representation->callable_producer_count; ++p) {
                const SolMirCallableProducer *producer
                    = &representation->callable_producers[p];
                if (producer->function_recipe == recipe
                    && producer->kind
                        == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION) {
                    if (add == SIZE_MAX) return false;
                    ++add;
                }
            }
        }
        if (add > SIZE_MAX - *edges) return false;
        *edges += add;
    }
    return true;
}

static SolMirRuntimeOwnershipClass ownership_class(const SolMirRecipe *recipe) {
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

/* Deliberately independent of the ownership builder: P2 Copy facts and the
   already-authenticated ownership category must agree rather than one deriving
   the other. */
static bool copy_class(const SolMirRecipe *recipe,
    SolMirRuntimeOwnershipClass ownership, SolMirRuntimeCopyClass *result) {
    if (!recipe->inhabited) {
        *result = SOL_MIR_RUNTIME_COPY_UNREACHABLE;
        return recipe->copy_kind == SOL_MIR_COPY_UNREACHABLE
            && ownership == SOL_MIR_RUNTIME_OWNERSHIP_UNREACHABLE;
    }
    if (!recipe->is_copy || recipe->kind == SOL_MIR_RECIPE_FUNCTION
        || recipe->kind == SOL_MIR_RECIPE_CAPABILITY) {
        *result = SOL_MIR_RUNTIME_COPY_FORBIDDEN;
        return recipe->copy_kind == SOL_MIR_COPY_FORBIDDEN
            && (ownership == SOL_MIR_RUNTIME_OWNERSHIP_CALLABLE
                || ownership == SOL_MIR_RUNTIME_OWNERSHIP_CAPABILITY
                || !recipe->is_copy);
    }
    switch (ownership) {
        case SOL_MIR_RUNTIME_OWNERSHIP_LEAF:
            *result = SOL_MIR_RUNTIME_COPY_TRIVIAL;
            return recipe->copy_kind == SOL_MIR_COPY_TRIVIAL;
        case SOL_MIR_RUNTIME_OWNERSHIP_TEXT:
            *result = SOL_MIR_RUNTIME_COPY_TEXT;
            return recipe->copy_kind == SOL_MIR_COPY_TEXT;
        case SOL_MIR_RUNTIME_OWNERSHIP_PRODUCT:
            *result = SOL_MIR_RUNTIME_COPY_PRODUCT;
            return recipe->copy_kind == SOL_MIR_COPY_AGGREGATE;
        case SOL_MIR_RUNTIME_OWNERSHIP_SUM:
            *result = SOL_MIR_RUNTIME_COPY_SUM;
            return recipe->copy_kind == SOL_MIR_COPY_AGGREGATE;
        case SOL_MIR_RUNTIME_OWNERSHIP_WRAPPER:
            *result = SOL_MIR_RUNTIME_COPY_WRAPPER;
            return recipe->copy_kind == SOL_MIR_COPY_WRAPPER;
        default: return false;
    }
}

/* Structural equality is independent of Copy and import demand.  Starting
   with every non-forbidden inhabited recipe and removing composites containing
   an ineligible child computes the greatest fixed point. */
static bool equality_classes(const SolMirRepresentation *representation,
    unsigned char *eligible, size_t *work) {
    size_t count = representation->recipe_count;
    for (size_t i = 0; i < count; ++i) {
        const SolMirRecipe *recipe = &representation->recipes[i];
        eligible[i] = recipe->inhabited && recipe->kind != SOL_MIR_RECIPE_FUNCTION
            && recipe->kind != SOL_MIR_RECIPE_CAPABILITY;
    }
    bool changed;
    do {
        changed = false;
        for (size_t i = 0; i < count; ++i) {
            if (*work == SIZE_MAX) return false;
            ++*work;
            if (!eligible[i]) continue;
            const SolMirRecipe *recipe = &representation->recipes[i];
            SolMirPlanSlice children = {0};
            bool wrapper = recipe->kind == SOL_MIR_RECIPE_DISTINCT
                || recipe->kind == SOL_MIR_RECIPE_REFINED;
            if (recipe->kind == SOL_MIR_RECIPE_TUPLE
                || recipe->kind == SOL_MIR_RECIPE_RECORD) children = recipe->fields;
            else if (recipe->kind == SOL_MIR_RECIPE_ENUM
                || recipe->kind == SOL_MIR_RECIPE_OPTION
                || recipe->kind == SOL_MIR_RECIPE_RESULT) {
                for (size_t v = 0; v < recipe->variants.count; ++v) {
                    const SolMirRecipeVariant *variant = &representation->variants[
                        recipe->variants.offset + v];
                    for (size_t f = 0; f < variant->fields.count; ++f) {
                        if (*work == SIZE_MAX) return false;
                        ++*work;
                        SolMirRecipeId child = representation->fields[
                            variant->fields.offset + f].type;
                        if (child >= count || !eligible[child]) {
                            eligible[i] = 0; changed = true; goto next_recipe;
                        }
                    }
                }
            } else if (wrapper) {
                if (*work == SIZE_MAX) return false;
                ++*work;
                if (recipe->backing >= count || !eligible[recipe->backing]) {
                    eligible[i] = 0; changed = true;
                }
            } else {
                goto next_recipe;
            }
            for (size_t f = 0; f < children.count; ++f) {
                if (*work == SIZE_MAX) return false;
                ++*work;
                SolMirRecipeId child = representation->fields[children.offset + f].type;
                if (child >= count || !eligible[child]) {
                    eligible[i] = 0; changed = true; break;
                }
            }
next_recipe: ;
        }
    } while (changed);
    return true;
}

static SolMirRuntimeEqualityClass equality_class(const SolMirRecipe *recipe,
    bool eligible) {
    if (!recipe->inhabited) return SOL_MIR_RUNTIME_EQUALITY_UNREACHABLE;
    if (!eligible) return SOL_MIR_RUNTIME_EQUALITY_FORBIDDEN;
    switch (recipe->kind) {
        case SOL_MIR_RECIPE_TEXT: return SOL_MIR_RUNTIME_EQUALITY_TEXT;
        case SOL_MIR_RECIPE_TUPLE: case SOL_MIR_RECIPE_RECORD:
            return SOL_MIR_RUNTIME_EQUALITY_PRODUCT;
        case SOL_MIR_RECIPE_ENUM: case SOL_MIR_RECIPE_OPTION:
        case SOL_MIR_RECIPE_RESULT: return SOL_MIR_RUNTIME_EQUALITY_SUM;
        case SOL_MIR_RECIPE_DISTINCT: case SOL_MIR_RECIPE_REFINED:
            return SOL_MIR_RUNTIME_EQUALITY_WRAPPER;
        default: return SOL_MIR_RUNTIME_EQUALITY_TRIVIAL;
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
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    ownership_count_scans = 0;
#endif
    const SolMirLinkage *linkage = &scratch.conventions->concrete->linkage;
    unsigned char *consumed = NULL;
    unsigned char *equality_eligible = NULL;
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
    equality_eligible = consumed == NULL ? NULL : consumed + scratch.conventions->import_count;
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
    if (count != 0 && !build_event(1)) goto exhausted;
    if (count != 0) {
        ++persistent_allocation_attempt;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        if (!force_allocation_failure && persistent_allocation_attempt
                != force_persistent_allocation_failure)
#endif
            scratch.copy_plans = calloc(count, sizeof(*scratch.copy_plans));
    }
    if (count != 0 && scratch.copy_plans == NULL) {
        free(consumed);
        if (diagnostics != NULL) diagnostics->allocation_failed = true;
        sol_mir_runtime_values_free(&scratch);
        report(diagnostics, "runtime values copy-plan persistent allocation failed");
        return SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED;
    }
    scratch.copy_plan_capacity = count;
    for (size_t i = 0; i < count; ++i) {
        SolMirRuntimeCopyClass classification;
        if (!build_event(1) || !copy_class(
                &scratch.conventions->concrete->representation.recipes[i],
                ownership_class(&scratch.conventions->concrete->representation.recipes[i]),
                &classification)) goto internal;
        scratch.copy_plans[i] = (SolMirRuntimeCopyPlan){i, classification};
        ++scratch.copy_plan_count;
    }
    if (count != 0 && !build_event(1)) goto exhausted;
    if (count != 0) {
        ++persistent_allocation_attempt;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        if (!force_allocation_failure && persistent_allocation_attempt
                != force_persistent_allocation_failure)
#endif
            scratch.equality_plans = calloc(count, sizeof(*scratch.equality_plans));
    }
    if (count != 0 && scratch.equality_plans == NULL) {
        free(consumed);
        if (diagnostics != NULL) diagnostics->allocation_failed = true;
        sol_mir_runtime_values_free(&scratch);
        report(diagnostics, "runtime values equality-plan persistent allocation failed");
        return SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED;
    }
    scratch.equality_plan_capacity = count;
    size_t equality_work = 0;
    if ((count != 0 && equality_eligible == NULL)
        || !equality_classes(&scratch.conventions->concrete->representation,
            equality_eligible, &equality_work) || !build_event(equality_work)) goto internal;
    for (size_t i = 0; i < count; ++i) {
        scratch.equality_plans[i] = (SolMirRuntimeEqualityPlan){i, equality_class(
            &scratch.conventions->concrete->representation.recipes[i],
            equality_eligible[i] != 0)};
        ++scratch.equality_plan_count;
    }
    size_t ownership_variants, owned_edges, producer_scans;
    const SolMirRepresentation *representation
        = scratch.conventions->concrete->layout.representation;
    if (!ownership_counts(representation, &ownership_variants, &owned_edges,
            &producer_scans)
        || !build_event(count) || !build_event(producer_scans)) goto internal;
    if (count != 0 && !build_event(1)) goto exhausted;
    if (count != 0) {
        ++persistent_allocation_attempt;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        if (!force_allocation_failure && persistent_allocation_attempt
                != force_persistent_allocation_failure)
#endif
            scratch.ownership_plans = calloc(count,
                sizeof(*scratch.ownership_plans));
    }
    if (count != 0 && scratch.ownership_plans == NULL) {
        free(consumed);
        if (diagnostics != NULL) diagnostics->allocation_failed = true;
        sol_mir_runtime_values_free(&scratch);
        report(diagnostics, "runtime values ownership-plan persistent allocation failed");
        return SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED;
    }
    scratch.ownership_plan_capacity = count;
    if (ownership_variants != 0 && !build_event(1)) goto exhausted;
    if (ownership_variants != 0) {
        ++persistent_allocation_attempt;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        if (!force_allocation_failure && persistent_allocation_attempt
                != force_persistent_allocation_failure)
#endif
            scratch.ownership_variants = calloc(ownership_variants,
                sizeof(*scratch.ownership_variants));
    }
    if (ownership_variants != 0 && scratch.ownership_variants == NULL) {
        free(consumed);
        if (diagnostics != NULL) diagnostics->allocation_failed = true;
        sol_mir_runtime_values_free(&scratch);
        report(diagnostics, "runtime values ownership-variant persistent allocation failed");
        return SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED;
    }
    scratch.ownership_variant_capacity = ownership_variants;
    if (owned_edges != 0 && !build_event(1)) goto exhausted;
    if (owned_edges != 0) {
        ++persistent_allocation_attempt;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        if (!force_allocation_failure && persistent_allocation_attempt
                != force_persistent_allocation_failure)
#endif
            scratch.owned_edges = calloc(owned_edges, sizeof(*scratch.owned_edges));
    }
    if (owned_edges != 0 && scratch.owned_edges == NULL) {
        free(consumed);
        if (diagnostics != NULL) diagnostics->allocation_failed = true;
        sol_mir_runtime_values_free(&scratch);
        report(diagnostics, "runtime values owned-edge persistent allocation failed");
        return SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED;
    }
    scratch.owned_edge_capacity = owned_edges;
    if (!build_event(count) || !build_event(producer_scans)
        || !build_event(ownership_variants) || !build_event(owned_edges))
        goto exhausted;
    for (size_t recipe = 0; recipe < count; ++recipe) {
        const SolMirRecipe *item = &representation->recipes[recipe];
        SolMirRuntimeOwnershipPlan *plan = &scratch.ownership_plans[recipe];
        *plan = (SolMirRuntimeOwnershipPlan){recipe, ownership_class(item),
            {scratch.owned_edge_count, 0}, {scratch.ownership_variant_count, 0}};
        if (plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_PRODUCT) {
            for (size_t f = 0; f < item->fields.count; ++f) {
                const SolMirRecipeField *field = &representation->fields[
                    item->fields.offset + f];
                scratch.owned_edges[scratch.owned_edge_count++]
                    = (SolMirRuntimeOwnedEdge){SOL_MIR_RUNTIME_OWNED_EDGE_FIELD,
                        field->type, field->ordinal, SOL_MIR_RUNTIME_NONE};
                ++plan->edges.count;
            }
        } else if (plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_SUM) {
            for (size_t v = 0; v < item->variants.count; ++v) {
                const SolMirRecipeVariant *source = &representation->variants[
                    item->variants.offset + v];
                SolMirRuntimeOwnershipVariant *variant
                    = &scratch.ownership_variants[scratch.ownership_variant_count++];
                *variant = (SolMirRuntimeOwnershipVariant){source->ordinal,
                    source->semantic_tag, {scratch.owned_edge_count, 0}};
                ++plan->variants.count;
                for (size_t f = 0; f < source->fields.count; ++f) {
                    const SolMirRecipeField *field = &representation->fields[
                        source->fields.offset + f];
                    scratch.owned_edges[scratch.owned_edge_count++]
                        = (SolMirRuntimeOwnedEdge){SOL_MIR_RUNTIME_OWNED_EDGE_FIELD,
                            field->type, field->ordinal, SOL_MIR_RUNTIME_NONE};
                    ++variant->edges.count;
                }
            }
        } else if (plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_WRAPPER) {
            scratch.owned_edges[scratch.owned_edge_count++]
                = (SolMirRuntimeOwnedEdge){SOL_MIR_RUNTIME_OWNED_EDGE_BACKING,
                    item->backing, 0, SOL_MIR_RUNTIME_NONE};
            ++plan->edges.count;
        } else if (plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_CAPABILITY
            && item->capability_source != SOL_MIR_RECIPE_NONE) {
            scratch.owned_edges[scratch.owned_edge_count++]
                = (SolMirRuntimeOwnedEdge){SOL_MIR_RUNTIME_OWNED_EDGE_PRIVATE_SOURCE,
                    item->capability_source, 0, SOL_MIR_RUNTIME_NONE};
            ++plan->edges.count;
        } else if (plan->classification == SOL_MIR_RUNTIME_OWNERSHIP_CALLABLE) {
            for (size_t p = 0; p < representation->callable_producer_count; ++p) {
                const SolMirCallableProducer *producer
                    = &representation->callable_producers[p];
                if (producer->function_recipe == recipe && producer->kind
                        == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION) {
                    scratch.owned_edges[scratch.owned_edge_count++]
                        = (SolMirRuntimeOwnedEdge){
                            SOL_MIR_RUNTIME_OWNED_EDGE_CAPTURED_RECEIVER,
                            producer->captured_receiver_type, 0, p};
                    ++plan->edges.count;
                }
            }
        }
        ++scratch.ownership_plan_count;
    }
    if (scratch.ownership_variant_count != ownership_variants
        || scratch.owned_edge_count != owned_edges) goto internal;
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
    for (size_t i = 0; i < scratch.conventions->import_count; ++i) {
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

static void producer_u64(SolMirLinkageSha256 *sha, const char *label,
    uint64_t value) {
    uint8_t bytes[8];
    for (size_t i = 0; i < sizeof(bytes); ++i)
        bytes[i] = (uint8_t)(value >> (56 - i * 8));
    sol_mir_linkage_internal_sha256_write(sha, label, strlen(label));
    sol_mir_linkage_internal_sha256_write(sha, bytes, sizeof(bytes));
}

static void producer_bytes(SolMirLinkageSha256 *sha, const char *label,
    const void *bytes, size_t length) {
    producer_u64(sha, "label-length", strlen(label));
    sol_mir_linkage_internal_sha256_write(sha, label, strlen(label));
    producer_u64(sha, "value-length", length);
    sol_mir_linkage_internal_sha256_write(sha, bytes, length);
}

static void producer_span(SolMirLinkageSha256 *sha, const char *label,
    SolSpan span) {
    producer_bytes(sha, label, "span", 4);
    producer_u64(sha, "span-start", span.start);
    producer_u64(sha, "span-end", span.end);
}

static bool producer_source_content(const SolIr *ir, size_t file,
    SolMirLinkageDigest *result) {
    if (file >= ir->file_count) return false;
    const SolIrSourceFile *source = &ir->files[file];
    if (source->aggregate_start > source->aggregate_end
        || source->aggregate_end > ir->source_length) return false;
    SolMirLinkageSha256 sha;
    sol_mir_linkage_internal_sha256_init(&sha);
    sol_mir_linkage_internal_sha256_write(&sha,
        ir->source_bytes + source->aggregate_start,
        source->aggregate_end - source->aggregate_start);
    return sol_mir_linkage_internal_sha256_finish(&sha, result);
}

static bool render_captured_producer_key(const SolMirRuntimeValues *values,
    const SolMirRuntimeOwnedEdge *edge, SolMirLinkageDigest *result) {
    const SolMirConcreteProgram *concrete = values->conventions->concrete;
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirLinkage *linkage = &concrete->linkage;
    const SolIr *ir = concrete->program.ir;
    if (edge->producer >= r->callable_producer_count) return false;
    const SolMirCallableProducer *producer = &r->callable_producers[edge->producer];
    if (producer->kind != SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION
        || producer->semantic_site >= m->semantic_site_count
        || producer->binding >= m->binding_count) return false;
    const SolMirMaterializedSemanticSite *site = &m->semantic_sites[
        producer->semantic_site];
    const SolMirMaterializedBinding *binding = &m->bindings[producer->binding];
    if (site->binding != producer->binding || site->parent >= m->plan->instance_count
        || site->context >= m->context_count || (site->source_definition
            != SOL_IR_NONE && site->source_definition >= ir->definition_count)
        || (site->source_obligation != SOL_IR_NONE
                && site->source_obligation >= ir->obligation_count))
        return false;
    SolMirLinkageDigest parent, site_content, context_content;
    if (!sol_mir_linkage_internal_instance_key(linkage, site->parent, &parent,
            NULL)) return false;
    if (!producer_source_content(ir, site->source.file, &site_content)) return false;
    SolMirLinkageSha256 sha;
    sol_mir_linkage_internal_sha256_init(&sha);
    producer_bytes(&sha, "domain", "sol.runtime-values.captured-producer.v1", 39);
    producer_bytes(&sha, "parent-instance", parent.bytes, sizeof(parent.bytes));
    producer_bytes(&sha, "source-content", site_content.bytes,
        sizeof(site_content.bytes));
    producer_u64(&sha, "site-start", site->source.start);
    producer_u64(&sha, "site-end", site->source.end);
    const SolMirPlanContext *context = &m->contexts[site->context];
    if (!producer_source_content(ir, context->source.file, &context_content))
        return false;
    producer_u64(&sha, "context-kind", context->kind);
    producer_bytes(&sha, "context-content", context_content.bytes,
        sizeof(context_content.bytes));
    producer_u64(&sha, "context-start", context->source.start);
    producer_u64(&sha, "context-end", context->source.end);
    if (site->source_definition != SOL_IR_NONE) {
        const SolSemanticId semantic
            = ir->definitions[site->source_definition].semantic_id;
        producer_u64(&sha, "definition-high", semantic.high);
        producer_u64(&sha, "definition-low", semantic.low);
    } else producer_bytes(&sha, "definition", "none", 4);
    if (site->source_obligation != SOL_IR_NONE) {
        const SolIrObligation *obligation = &ir->obligations[site->source_obligation];
        if (obligation->predicate >= ir->expression_count) return false;
        producer_span(&sha, "obligation-predicate",
            ir->expressions[obligation->predicate].span);
    } else producer_bytes(&sha, "obligation-predicate", "none", 4);
    bool target_found = false;
    for (size_t i = 0; i < linkage->binding_count; ++i) {
        const SolMirLinkageBinding *target = &linkage->bindings[i];
        if (target->binding != producer->binding) continue;
        target_found = true;
        producer_u64(&sha, "target-kind", target->target_kind);
        if (target->target_kind == SOL_MIR_LINKAGE_TARGET_INTERNAL) {
            if (target->internal >= linkage->callable_count) return false;
            producer_bytes(&sha, "target-symbol",
                linkage->callables[target->internal].symbol.bytes,
                strlen(linkage->callables[target->internal].symbol.bytes));
        } else if (target->target_kind == SOL_MIR_LINKAGE_TARGET_HOST) {
            if (target->host >= linkage->host_requirement_count) return false;
            const SolMirLinkageHostRequirement *host
                = &linkage->host_requirements[target->host];
            producer_u64(&sha, "host-semantic-high", host->semantic_id.high);
            producer_u64(&sha, "host-semantic-low", host->semantic_id.low);
            producer_bytes(&sha, "host-requirement", host->requirement_key.bytes,
                sizeof(host->requirement_key.bytes));
        } else return false;
        break;
    }
    return target_found && binding->target_kind == producer->target_kind
        && sol_mir_linkage_internal_sha256_finish(&sha, result);
}

static const char *ownership_class_name(SolMirRuntimeOwnershipClass value) {
    static const char *const names[] = {"unreachable", "leaf", "text",
        "product", "sum", "wrapper", "callable", "capability"};
    return value <= SOL_MIR_RUNTIME_OWNERSHIP_CAPABILITY ? names[value] : NULL;
}

static const char *owned_edge_name(SolMirRuntimeOwnedEdgeKind value) {
    static const char *const names[] = {"field", "backing", "private-source",
        "captured-receiver"};
    return value <= SOL_MIR_RUNTIME_OWNED_EDGE_CAPTURED_RECEIVER
        ? names[value] : NULL;
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
    format(&output, "declaration.kind=operation-demand-allocation-plan-inventory executable-operations=false allocation-plans=true ownership-plans=true copy-plans=true equality-plans=true equality-execution=false copy-execution=false move-drop-execution=false\n");
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
        const SolMirRuntimeOwnershipPlan *ownership
            = &values->ownership_plans[i];
        const char *classification = ownership_class_name(ownership->classification);
        if (classification == NULL) { output.failed = true; break; }
        format(line, " ownership=%s", classification);
        const SolMirRuntimeCopyPlan *copy_plan = &values->copy_plans[i];
        static const char *const copy_names[] = {"unreachable", "forbidden",
            "trivial", "text", "product", "sum", "wrapper"};
        if (copy_plan->recipe != i
            || copy_plan->classification > SOL_MIR_RUNTIME_COPY_WRAPPER) {
            output.failed = true; break;
        }
        format(line, " copy=%s", copy_names[copy_plan->classification]);
        static const char *const equality_names[] = {"unreachable", "forbidden",
            "trivial", "text", "product", "sum", "wrapper"};
        const SolMirRuntimeEqualityPlan *equality = &values->equality_plans[i];
        if (equality->recipe != i || equality->classification
                > SOL_MIR_RUNTIME_EQUALITY_WRAPPER) { output.failed = true; break; }
        format(line, " equality=%s", equality_names[equality->classification]);
        if (ownership->classification == SOL_MIR_RUNTIME_OWNERSHIP_CALLABLE) {
            RenderLine *captures = ownership->edges.count == 0 ? NULL
                : calloc(ownership->edges.count, sizeof(*captures));
            if (ownership->edges.count != 0 && captures == NULL) output.failed = true;
            for (size_t edge_i = 0; !output.failed
                    && edge_i < ownership->edges.count; ++edge_i) {
                const SolMirRuntimeOwnedEdge *edge = &values->owned_edges[
                    ownership->edges.offset
#ifdef SOL_MIR_PLAN_TEST_HOOKS
                    + (reverse_captured_fragments
                        ? ownership->edges.count - edge_i - 1 : edge_i)
#else
                    + edge_i
#endif
                    ];
                SolMirLinkageDigest target, capture;
                if (edge->kind != SOL_MIR_RUNTIME_OWNED_EDGE_CAPTURED_RECEIVER
                    || !sol_mir_linkage_internal_recipe_key(linkage, edge->recipe,
                        &target, NULL)
                    || !render_captured_producer_key(values, edge, &capture)) {
                    output.failed = true; break;
                }
#ifdef SOL_MIR_PLAN_TEST_HOOKS
                if (force_captured_digest_collision) memset(&capture, 0,
                    sizeof(capture));
#endif
                format(&captures[edge_i].text, " edge=captured-receiver:0:");
                render_digest(&captures[edge_i].text, &capture);
                format(&captures[edge_i].text, ":");
                render_digest(&captures[edge_i].text, &target);
                if (captures[edge_i].text.failed) output.failed = true;
            }
            if (!output.failed && ownership->edges.count > 1)
                qsort(captures, ownership->edges.count, sizeof(*captures),
                    compare_lines);
            for (size_t edge_i = 1; !output.failed
                    && edge_i < ownership->edges.count; ++edge_i)
                if (memcmp(captures[edge_i - 1].text.data
                        + strlen(" edge=captured-receiver:0:"),
                        captures[edge_i].text.data
                        + strlen(" edge=captured-receiver:0:"),
                        SOL_MIR_LINKAGE_DIGEST_BYTES * 2) == 0)
                    output.failed = true;
            for (size_t edge_i = 0; !output.failed
                    && edge_i < ownership->edges.count; ++edge_i)
                format(line, "%s", captures[edge_i].text.data);
            for (size_t edge_i = 0; edge_i < ownership->edges.count; ++edge_i)
                free(captures[edge_i].text.data);
            free(captures);
        } else for (size_t edge_i = 0; edge_i < ownership->edges.count; ++edge_i) {
            const SolMirRuntimeOwnedEdge *edge = &values->owned_edges[
                ownership->edges.offset + edge_i];
            SolMirLinkageDigest target;
            const char *edge_name = owned_edge_name(edge->kind);
            if (edge_name == NULL || !sol_mir_linkage_internal_recipe_key(linkage,
                    edge->recipe, &target, NULL)) { output.failed = true; break; }
            format(line, " edge=%s:%zu:", edge_name, edge->ordinal);
            render_digest(line, &target);
        }
        for (size_t variant_i = 0; !output.failed
                && variant_i < ownership->variants.count; ++variant_i) {
            const SolMirRuntimeOwnershipVariant *variant
                = &values->ownership_variants[ownership->variants.offset + variant_i];
            format(line, " variant=%zu:%zu", variant->ordinal, variant->semantic_tag);
            for (size_t edge_i = 0; edge_i < variant->edges.count; ++edge_i) {
                const SolMirRuntimeOwnedEdge *edge = &values->owned_edges[
                    variant->edges.offset + edge_i];
                SolMirLinkageDigest target;
                const char *edge_name = owned_edge_name(edge->kind);
                if (edge_name == NULL || !sol_mir_linkage_internal_recipe_key(linkage,
                        edge->recipe, &target, NULL)) {
                    output.failed = true; break;
                }
                format(line, " edge=%s:%zu:", edge_name, edge->ordinal);
                render_digest(line, &target);
            }
        }
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
        || values->copy_plan_count != values->copy_plan_capacity
        || values->equality_plan_count != values->equality_plan_capacity
        || values->ownership_plan_count != values->ownership_plan_capacity
        || values->ownership_variant_count != values->ownership_variant_capacity
        || values->owned_edge_count != values->owned_edge_capacity
        || values->recipe_operation_count != values->allocation_plan_count
        || values->recipe_operation_count != values->copy_plan_count
        || values->recipe_operation_count != values->equality_plan_count
        || values->recipe_operation_count != values->ownership_plan_count
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
        || !range_valid(values->copy_plans, values->copy_plan_capacity,
            sizeof(*values->copy_plans))
        || !range_valid(values->equality_plans, values->equality_plan_capacity,
            sizeof(*values->equality_plans))
        || !range_valid(values->ownership_plans, values->ownership_plan_capacity,
            sizeof(*values->ownership_plans))
        || !range_valid(values->ownership_variants,
            values->ownership_variant_capacity,
            sizeof(*values->ownership_variants))
        || !range_valid(values->owned_edges, values->owned_edge_capacity,
            sizeof(*values->owned_edges))
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
    size_t owned_bytes, plan_bytes, copy_plan_bytes, equality_plan_bytes,
        ownership_plan_bytes, variant_bytes, edge_bytes, build_work;
    if (!mul_size(values->recipe_operation_count,
            sizeof(*values->recipe_operations), &owned_bytes)
        || !mul_size(values->allocation_plan_count,
            sizeof(*values->allocation_plans), &plan_bytes)
        || !mul_size(values->copy_plan_count, sizeof(*values->copy_plans),
            &copy_plan_bytes)
        || !mul_size(values->equality_plan_count, sizeof(*values->equality_plans),
            &equality_plan_bytes)
        || !mul_size(values->ownership_plan_count,
            sizeof(*values->ownership_plans), &ownership_plan_bytes)
        || !mul_size(values->ownership_variant_count,
            sizeof(*values->ownership_variants), &variant_bytes)
        || !mul_size(values->owned_edge_count, sizeof(*values->owned_edges),
            &edge_bytes)
        || !add_size(&owned_bytes, plan_bytes)
        || !add_size(&owned_bytes, copy_plan_bytes)
        || !add_size(&owned_bytes, equality_plan_bytes)
        || !add_size(&owned_bytes, ownership_plan_bytes)
        || !add_size(&owned_bytes, variant_bytes)
        || !add_size(&owned_bytes, edge_bytes)
        || !mul_size(values->recipe_operation_count, 4, &build_work)
        || !add_size(&build_work,
            concrete->linkage.runtime_requirement_count)
        || !add_size(&build_work, conventions->import_count)
        || !add_size(&build_work, conventions->import_count)
        || (conventions->import_count != 0 && !add_size(&build_work, 1))
        || (values->recipe_operation_count != 0
            && (!add_size(&build_work, 1) || !add_size(&build_work, 1)
                || !add_size(&build_work, 1)))
        || !add_size(&build_work, values->recipe_operation_count)
        || (values->ownership_plan_count != 0 && !add_size(&build_work, 1))
        || !add_size(&build_work, values->ownership_plan_count)
        || !add_size(&build_work, values->ownership_variant_count)
        || !add_size(&build_work, values->owned_edge_count)
        || (values->ownership_variant_count != 0 && !add_size(&build_work, 1))
        || (values->owned_edge_count != 0 && !add_size(&build_work, 1)))
        return false;
    size_t local_scratch = values->recipe_operation_count;
    size_t minimum_work = conventions->usage.validation_work;
    if (!add_size(&local_scratch, conventions->import_count)
        || !add_size(&minimum_work, 8)) return false;
    size_t expected_scratch = conventions->usage.validation_scratch_bytes;
    if (expected_scratch < local_scratch) expected_scratch = local_scratch;
    return values->usage.records == values->recipe_operation_count
        && values->usage.allocation_plans == values->allocation_plan_count
        && values->usage.copy_plans == values->copy_plan_count
        && values->usage.equality_plans == values->equality_plan_count
        && values->usage.ownership_plans == values->ownership_plan_count
        && values->usage.ownership_variants == values->ownership_variant_count
        && values->usage.owned_edges == values->owned_edge_count
        && values->usage.owned_bytes == owned_bytes
        && values->usage.build_scratch_bytes
            == conventions->import_count + values->recipe_operation_count
        /* This O(1) checker establishes only a structural lower bound. Full
           validation independently authenticates the exact construction work. */
        && values->usage.build_work >= build_work
        && values->usage.validation_scratch_bytes == expected_scratch
        && values->usage.validation_work >= minimum_work
        && values->usage.records <= values->limits.max_records
        && values->usage.allocation_plans <= values->limits.max_allocation_plans
        && values->usage.copy_plans <= values->limits.max_copy_plans
        && values->usage.equality_plans <= values->limits.max_equality_plans
        && values->usage.ownership_plans <= values->limits.max_ownership_plans
        && values->usage.ownership_variants
            <= values->limits.max_ownership_variants
        && values->usage.owned_edges <= values->limits.max_owned_edges
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
        || ranges_overlap(demand, sizeof(*demand), values->copy_plans,
            values->copy_plan_count * sizeof(*values->copy_plans))
        || ranges_overlap(demand, sizeof(*demand), values->equality_plans,
            values->equality_plan_count * sizeof(*values->equality_plans))
        || ranges_overlap(demand, sizeof(*demand), values->ownership_plans,
            values->ownership_plan_count * sizeof(*values->ownership_plans))
        || ranges_overlap(demand, sizeof(*demand), values->ownership_variants,
            values->ownership_variant_count * sizeof(*values->ownership_variants))
        || ranges_overlap(demand, sizeof(*demand), values->owned_edges,
            values->owned_edge_count * sizeof(*values->owned_edges))
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
