#include "sol/mir_runtime_values.h"
#include "mir_runtime_conventions_internal.h"
#include "mir_runtime_values_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local size_t metered_validation_work;
static _Thread_local size_t metered_validation_limit;
static _Thread_local bool validation_work_exhausted;

enum { VALIDATION_HEADER_WORK = 8 };

#ifdef SOL_MIR_PLAN_TEST_HOOKS
static _Thread_local bool force_validation_allocation_failure;
static _Thread_local size_t force_validation_allocation_failure_attempt;
static _Thread_local size_t validation_allocation_attempts;

void sol_mir_runtime_values_test_force_validation_allocation_failure(bool force) {
    force_validation_allocation_failure = force;
}

void sol_mir_runtime_values_test_force_validation_allocation_failure_attempt(
    size_t attempt) {
    force_validation_allocation_failure_attempt = attempt;
}

size_t sol_mir_runtime_values_test_validation_allocation_attempts(void) {
    return validation_allocation_attempts;
}
#endif

/* Every temporary used while authenticating this owner is a declared
   validator-scratch allocation.  In particular, resource reconstruction must
   not silently bypass the deterministic scratch-failure hook. */
static unsigned char *validation_scratch_allocate(size_t bytes) {
    if (bytes == 0) return NULL;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    ++validation_allocation_attempts;
    if (force_validation_allocation_failure || validation_allocation_attempts
            == force_validation_allocation_failure_attempt) return NULL;
#endif
    return calloc(bytes, 1);
}

static bool invalid(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) sol_diagnostics_add(diagnostics,
        "SOL-MIR-RUNTIME-VALUES-002", SOL_SEVERITY_ERROR, (SolSpan){0},
        message);
    return false;
}

static bool add_size(size_t *value, size_t amount) {
    if (amount > SIZE_MAX - *value) return false;
    *value += amount;
    return true;
}

static bool validation_event(size_t amount) {
    if (!add_size(&metered_validation_work, amount)
        || metered_validation_work > metered_validation_limit) {
        validation_work_exhausted = true;
        return false;
    }
    return true;
}

static bool mul_size(size_t left, size_t right, size_t *result) {
    if (left != 0 && right > SIZE_MAX / left) return false;
    *result = left * right;
    return true;
}

static bool limits_complete(SolMirRuntimeValuesLimits value) {
    return value.max_records != 0 && value.max_allocation_plans != 0
        && value.max_copy_plans != 0 && value.max_equality_plans != 0
        && value.max_host_result_plans != 0 && value.max_host_result_requirements != 0
        && value.max_ownership_plans != 0 && value.max_ownership_variants != 0
        && value.max_owned_edges != 0
        && value.max_owned_bytes != 0
        && value.max_build_scratch_bytes != 0 && value.max_build_work != 0
        && value.max_validation_scratch_bytes != 0
        && value.max_validation_work != 0;
}

/* Kept independent from construction: validation must not trust builder logic. */
static bool reconstructed_ownership_counts(const SolMirRepresentation *r,
    size_t *variants, size_t *edges, size_t *producer_scans) {
    *variants = 0; *edges = 0; *producer_scans = 0;
    for (size_t recipe = 0; recipe < r->recipe_count; ++recipe) {
        const SolMirRecipe *item = &r->recipes[recipe];
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
                size_t fields = r->variants[item->variants.offset + v].fields.count;
                if (fields > SIZE_MAX - add) return false;
                add += fields;
            }
        } else if (item->kind == SOL_MIR_RECIPE_DISTINCT
            || item->kind == SOL_MIR_RECIPE_REFINED) add = 1;
        else if (item->kind == SOL_MIR_RECIPE_CAPABILITY)
            add = item->capability_source == SOL_MIR_RECIPE_NONE ? 0 : 1;
        else if (item->kind == SOL_MIR_RECIPE_FUNCTION) {
            if (r->callable_producer_count > SIZE_MAX - *producer_scans)
                return false;
            *producer_scans += r->callable_producer_count;
            for (size_t p = 0; p < r->callable_producer_count; ++p)
                if (r->callable_producers[p].function_recipe == recipe
                    && r->callable_producers[p].kind
                        == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION) {
                    if (add == SIZE_MAX) return false;
                    ++add;
                }
        }
        if (add > SIZE_MAX - *edges) return false;
        *edges += add;
    }
    return true;
}

static SolMirRuntimeOwnershipClass reconstructed_ownership_class(
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

static bool reconstructed_copy_class(const SolMirRecipe *recipe,
    SolMirRuntimeCopyClass *result) {
    SolMirRuntimeOwnershipClass ownership = reconstructed_ownership_class(recipe);
    if (!recipe->inhabited) {
        *result = SOL_MIR_RUNTIME_COPY_UNREACHABLE;
        return recipe->copy_kind == SOL_MIR_COPY_UNREACHABLE
            && ownership == SOL_MIR_RUNTIME_OWNERSHIP_UNREACHABLE;
    }
    if (!recipe->is_copy || recipe->kind == SOL_MIR_RECIPE_FUNCTION
        || recipe->kind == SOL_MIR_RECIPE_CAPABILITY) {
        *result = SOL_MIR_RUNTIME_COPY_FORBIDDEN;
        return recipe->copy_kind == SOL_MIR_COPY_FORBIDDEN;
    }
    if (ownership == SOL_MIR_RUNTIME_OWNERSHIP_LEAF) {
        *result = SOL_MIR_RUNTIME_COPY_TRIVIAL;
        return recipe->copy_kind == SOL_MIR_COPY_TRIVIAL;
    }
    if (ownership == SOL_MIR_RUNTIME_OWNERSHIP_TEXT) {
        *result = SOL_MIR_RUNTIME_COPY_TEXT;
        return recipe->copy_kind == SOL_MIR_COPY_TEXT;
    }
    if (ownership == SOL_MIR_RUNTIME_OWNERSHIP_PRODUCT) {
        *result = SOL_MIR_RUNTIME_COPY_PRODUCT;
        return recipe->copy_kind == SOL_MIR_COPY_AGGREGATE;
    }
    if (ownership == SOL_MIR_RUNTIME_OWNERSHIP_SUM) {
        *result = SOL_MIR_RUNTIME_COPY_SUM;
        return recipe->copy_kind == SOL_MIR_COPY_AGGREGATE;
    }
    if (ownership == SOL_MIR_RUNTIME_OWNERSHIP_WRAPPER) {
        *result = SOL_MIR_RUNTIME_COPY_WRAPPER;
        return recipe->copy_kind == SOL_MIR_COPY_WRAPPER;
    }
    return false;
}

/* Intentionally separate from construction's classifier. */
static bool reconstructed_equality_classes(const SolMirRepresentation *r,
    unsigned char *eligible, size_t *work) {
    for (size_t i = 0; i < r->recipe_count; ++i) {
        const SolMirRecipe *recipe = &r->recipes[i];
        eligible[i] = recipe->inhabited && recipe->kind != SOL_MIR_RECIPE_FUNCTION
            && recipe->kind != SOL_MIR_RECIPE_CAPABILITY;
    }
    bool changed;
    do {
        changed = false;
        for (size_t i = 0; i < r->recipe_count; ++i) {
            if (*work == SIZE_MAX) return false;
            ++*work;
            if (!eligible[i]) continue;
            const SolMirRecipe *recipe = &r->recipes[i];
            if (recipe->kind == SOL_MIR_RECIPE_ENUM
                || recipe->kind == SOL_MIR_RECIPE_OPTION
                || recipe->kind == SOL_MIR_RECIPE_RESULT) {
                for (size_t v = 0; v < recipe->variants.count; ++v) {
                    const SolMirRecipeVariant *variant = &r->variants[
                        recipe->variants.offset + v];
                    for (size_t f = 0; f < variant->fields.count; ++f) {
                        if (*work == SIZE_MAX) return false;
                        ++*work;
                        SolMirRecipeId child = r->fields[variant->fields.offset + f].type;
                        if (child >= r->recipe_count || !eligible[child]) {
                            eligible[i] = 0; changed = true; goto next_equality_recipe;
                        }
                    }
                }
            } else if (recipe->kind == SOL_MIR_RECIPE_DISTINCT
                || recipe->kind == SOL_MIR_RECIPE_REFINED) {
                if (*work == SIZE_MAX) return false;
                ++*work;
                if (recipe->backing >= r->recipe_count || !eligible[recipe->backing]) {
                    eligible[i] = 0; changed = true;
                }
            } else if (recipe->kind == SOL_MIR_RECIPE_TUPLE
                || recipe->kind == SOL_MIR_RECIPE_RECORD) {
                for (size_t f = 0; f < recipe->fields.count; ++f) {
                    if (*work == SIZE_MAX) return false;
                    ++*work;
                    SolMirRecipeId child = r->fields[recipe->fields.offset + f].type;
                    if (child >= r->recipe_count || !eligible[child]) {
                        eligible[i] = 0; changed = true; break;
                    }
                }
            }
next_equality_recipe: ;
        }
    } while (changed);
    return true;
}

static SolMirRuntimeEqualityClass reconstructed_equality_class(
    const SolMirRecipe *recipe, bool eligible) {
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

/* Separate from construction's closure calculation.  Cycles remain false: the
   finite borrowed view contract requires a proof rooted at E3 leaves. */
static bool reconstructed_host_result_classes(const SolMirRepresentation *r,
    unsigned char *eligible, size_t *work) {
    for (size_t i = 0; i < r->recipe_count; ++i) {
        const SolMirRecipe *recipe = &r->recipes[i];
        eligible[i] = recipe->inhabited && (recipe->kind == SOL_MIR_RECIPE_INT64
            || recipe->kind == SOL_MIR_RECIPE_BOOL || recipe->kind == SOL_MIR_RECIPE_TEXT
            || recipe->kind == SOL_MIR_RECIPE_UNIT);
    }
    bool changed;
    do {
        changed = false;
        for (size_t i = 0; i < r->recipe_count; ++i) {
            if (*work == SIZE_MAX) return false;
            ++*work;
            const SolMirRecipe *recipe = &r->recipes[i];
            if (eligible[i] || !recipe->inhabited || (recipe->kind
                    != SOL_MIR_RECIPE_OPTION && recipe->kind != SOL_MIR_RECIPE_RESULT))
                continue;
            bool valid = true;
            for (size_t v = 0; v < recipe->variants.count && valid; ++v) {
                const SolMirRecipeVariant *variant = &r->variants[
                    recipe->variants.offset + v];
                if (variant->fields.count > 1) valid = false;
                for (size_t f = 0; valid && f < variant->fields.count; ++f) {
                    SolMirRecipeId child = r->fields[variant->fields.offset + f].type;
                    if (child >= r->recipe_count || !eligible[child]) valid = false;
                }
            }
            if (valid) { eligible[i] = 1; changed = true; }
        }
    } while (changed);
    return true;
}

static SolMirRuntimeHostResultClass reconstructed_host_result_class(
    const SolMirRecipe *recipe, bool eligible) {
    if (!recipe->inhabited) return SOL_MIR_RUNTIME_HOST_RESULT_UNREACHABLE;
    if (!eligible) return SOL_MIR_RUNTIME_HOST_RESULT_FORBIDDEN;
    switch (recipe->kind) {
        case SOL_MIR_RECIPE_INT64: return SOL_MIR_RUNTIME_HOST_RESULT_INT64;
        case SOL_MIR_RECIPE_BOOL: return SOL_MIR_RUNTIME_HOST_RESULT_BOOL;
        case SOL_MIR_RECIPE_TEXT: return SOL_MIR_RUNTIME_HOST_RESULT_TEXT;
        case SOL_MIR_RECIPE_UNIT: return SOL_MIR_RUNTIME_HOST_RESULT_UNIT;
        case SOL_MIR_RECIPE_OPTION: return SOL_MIR_RUNTIME_HOST_RESULT_OPTION;
        case SOL_MIR_RECIPE_RESULT: return SOL_MIR_RUNTIME_HOST_RESULT_RESULT;
        default: return SOL_MIR_RUNTIME_HOST_RESULT_FORBIDDEN;
    }
}

static bool reconstructed_captured_edge_count(const SolMirRepresentation *r,
    size_t *captured) {
    *captured = 0;
    for (size_t recipe = 0; recipe < r->recipe_count; ++recipe) {
        const SolMirRecipe *item = &r->recipes[recipe];
        if (!item->inhabited || item->kind != SOL_MIR_RECIPE_FUNCTION) continue;
        for (size_t p = 0; p < r->callable_producer_count; ++p)
            if (r->callable_producers[p].function_recipe == recipe
                && r->callable_producers[p].kind
                    == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION) {
                if (*captured == SIZE_MAX) return false;
                ++*captured;
            }
    }
    return true;
}

static bool range_valid(const void *pointer, size_t count, size_t size) {
    size_t bytes;
    return count == 0 ? pointer == NULL
        : pointer != NULL && mul_size(count, size, &bytes)
            && (uintptr_t)pointer <= UINTPTR_MAX - bytes;
}

static bool overlaps(const void *left, size_t left_count, size_t left_size,
    const void *right, size_t right_count, size_t right_size) {
    if (left_count == 0 || right_count == 0) return false;
    size_t left_bytes, right_bytes;
    if (!mul_size(left_count, left_size, &left_bytes)
        || !mul_size(right_count, right_size, &right_bytes)) return true;
    uintptr_t a = (uintptr_t)left, b = (uintptr_t)right;
    if (a > UINTPTR_MAX - left_bytes || b > UINTPTR_MAX - right_bytes)
        return true;
    return a < b + right_bytes && b < a + left_bytes;
}

static bool reconstruct_usage(const SolMirRuntimeConventions *conventions,
    size_t predecessor_work, size_t predecessor_scratch, size_t alias_work,
    unsigned char *equality_eligible, SolMirRuntimeValuesUsage *usage) {
    const SolMirConcreteProgram *concrete = conventions->concrete;
    size_t records = concrete->representation.recipe_count;
    size_t owned_bytes, build_work = 0, validation_work = 0;
    size_t plan_bytes, copy_plan_bytes, equality_plan_bytes, host_plan_bytes,
        host_requirement_bytes, ownership_plan_bytes, variant_bytes, edge_bytes,
        equality_work = 0, host_result_work = 0;
    size_t variants, edges, producer_scans, captured_edges;
    bool equality_ok = records == 0 || (equality_eligible != NULL
        && reconstructed_equality_classes(&concrete->representation,
            equality_eligible, &equality_work));
    bool host_result_ok = records == 0 || (equality_eligible != NULL
        && reconstructed_host_result_classes(&concrete->representation,
            equality_eligible + records, &host_result_work));
    if (!equality_ok || !host_result_ok
        || !mul_size(records, sizeof(SolMirRuntimeRecipeOperations), &owned_bytes)
        || !mul_size(records, sizeof(SolMirRuntimeAllocationPlan), &plan_bytes)
        || !mul_size(records, sizeof(SolMirRuntimeCopyPlan), &copy_plan_bytes)
        || !mul_size(records, sizeof(SolMirRuntimeEqualityPlan), &equality_plan_bytes)
        || !mul_size(records, sizeof(SolMirRuntimeHostResultPlan), &host_plan_bytes)
        || !mul_size(concrete->linkage.host_requirement_count,
            sizeof(SolMirRuntimeHostResultRequirement), &host_requirement_bytes)
        || !mul_size(records, sizeof(SolMirRuntimeOwnershipPlan),
            &ownership_plan_bytes)
        || !reconstructed_ownership_counts(&concrete->representation, &variants,
            &edges, &producer_scans)
        || !reconstructed_captured_edge_count(&concrete->representation,
            &captured_edges)
        || captured_edges > edges
        || !mul_size(variants, sizeof(SolMirRuntimeOwnershipVariant),
            &variant_bytes)
        || !mul_size(edges, sizeof(SolMirRuntimeOwnedEdge), &edge_bytes)
        || !add_size(&owned_bytes, plan_bytes)
        || !add_size(&owned_bytes, copy_plan_bytes)
        || !add_size(&owned_bytes, equality_plan_bytes)
        || !add_size(&owned_bytes, host_plan_bytes)
        || !add_size(&owned_bytes, host_requirement_bytes)
        || !add_size(&owned_bytes, ownership_plan_bytes)
        || !add_size(&owned_bytes, variant_bytes)
        || !add_size(&owned_bytes, edge_bytes)
        || !mul_size(records, 4, &build_work)
        || !add_size(&build_work, concrete->linkage.runtime_requirement_count)
        || !add_size(&build_work, conventions->import_count)
        || !add_size(&build_work, conventions->import_count)
        || ((conventions->import_count != 0 || records != 0)
            && !add_size(&build_work, 1))
        || (records != 0 && (!add_size(&build_work, 1)
            || !add_size(&build_work, 1) || !add_size(&build_work, 1)
            || !add_size(&build_work, 1)))
        || !add_size(&build_work, equality_work)
        || (records != 0 && !add_size(&build_work, 1))
        || !add_size(&build_work, host_result_work)
        || (concrete->linkage.host_requirement_count != 0
            && !add_size(&build_work, 1))
        || !add_size(&build_work, concrete->linkage.host_requirement_count)
        || !add_size(&build_work, records)
        || !add_size(&build_work, producer_scans)
        || (records != 0 && !add_size(&build_work, 1))
        || !add_size(&build_work, records)
        || !add_size(&build_work, producer_scans)
        || !add_size(&build_work, variants)
        || !add_size(&build_work, edges)
        || (variants != 0 && !add_size(&build_work, 1))
        || (edges != 0 && !add_size(&build_work, 1))) return false;
    size_t build_scratch = conventions->import_count;
    if (!add_size(&build_scratch, records) || !add_size(&build_scratch, records))
        return false;
    size_t local_validation_scratch = records;
    if (!add_size(&local_validation_scratch, records)
        || !add_size(&local_validation_scratch, conventions->import_count)
        || !add_size(&local_validation_scratch, records))
        return false;
    size_t validation_scratch = predecessor_scratch;
    if (validation_scratch < local_validation_scratch)
        validation_scratch = local_validation_scratch;
    validation_work = predecessor_work;
    if (!add_size(&validation_work, VALIDATION_HEADER_WORK)
        || !add_size(&validation_work, alias_work)
        || !add_size(&validation_work, alias_work)
        || (local_validation_scratch != 0
            && !add_size(&validation_work, 1))
        || records > (SIZE_MAX - validation_work) / 9
        || !add_size(&validation_work, records * 9)
        || !add_size(&validation_work,
            concrete->linkage.runtime_requirement_count)
        || !add_size(&validation_work, conventions->import_count)
        || !add_size(&validation_work, variants)
        || !add_size(&validation_work, edges - captured_edges)
        || !add_size(&validation_work, producer_scans)
        || !add_size(&validation_work, equality_work)
        || !add_size(&validation_work, records)
        || !add_size(&validation_work, host_result_work)
        || !add_size(&validation_work, concrete->linkage.host_requirement_count)) return false;
    *usage = (SolMirRuntimeValuesUsage){records, records, records, records, records,
        concrete->linkage.host_requirement_count, records, variants, edges, owned_bytes, build_scratch, build_work,
        validation_scratch, validation_work};
    return true;
}

static bool usage_fits(const SolMirRuntimeValuesUsage *usage,
    const SolMirRuntimeValuesLimits *limits) {
    return usage->records <= limits->max_records
        && usage->allocation_plans <= limits->max_allocation_plans
        && usage->copy_plans <= limits->max_copy_plans
        && usage->equality_plans <= limits->max_equality_plans
        && usage->host_result_plans <= limits->max_host_result_plans
        && usage->host_result_requirements <= limits->max_host_result_requirements
        && usage->ownership_plans <= limits->max_ownership_plans
        && usage->ownership_variants <= limits->max_ownership_variants
        && usage->owned_edges <= limits->max_owned_edges
        && usage->owned_bytes <= limits->max_owned_bytes
        && usage->build_scratch_bytes <= limits->max_build_scratch_bytes
        && usage->build_work <= limits->max_build_work
        && usage->validation_scratch_bytes
            <= limits->max_validation_scratch_bytes
        && usage->validation_work <= limits->max_validation_work;
}

static bool measured_text_size(const char *text, size_t *size) {
    size_t length = 0;
    do {
        if (!validation_event(1) || length == SIZE_MAX) return false;
    } while (text[length++] != '\0');
    *size = length;
    return true;
}

static bool walk_aliases(const SolMirRuntimeValues *values,
    const SolMirRuntimeConventions *runtime, bool check) {
    const void *records = check ? values->recipe_operations : NULL;
    size_t record_count = check ? values->recipe_operation_capacity : 0;
    const void *plans = check ? values->allocation_plans : NULL;
    size_t plan_count = check ? values->allocation_plan_capacity : 0;
    const void *copy_plans = check ? values->copy_plans : NULL;
    size_t copy_plan_count = check ? values->copy_plan_capacity : 0;
    const void *equality_plans = check ? values->equality_plans : NULL;
    size_t equality_plan_count = check ? values->equality_plan_capacity : 0;
    const void *host_result_plans = check ? values->host_result_plans : NULL;
    size_t host_result_plan_count = check ? values->host_result_plan_capacity : 0;
    const void *host_result_requirements = check ? values->host_result_requirements : NULL;
    size_t host_result_requirement_count = check
        ? values->host_result_requirement_capacity : 0;
    const void *ownership_plans = check ? values->ownership_plans : NULL;
    size_t ownership_plan_count = check ? values->ownership_plan_capacity : 0;
    const void *variants = check ? values->ownership_variants : NULL;
    size_t variant_count = check ? values->ownership_variant_capacity : 0;
    const void *edges = check ? values->owned_edges : NULL;
    size_t edge_count = check ? values->owned_edge_capacity : 0;
    if (!validation_event(1)) return false;
    if (check && overlaps(records, record_count,
            sizeof(*values->recipe_operations), plans, plan_count,
            sizeof(*values->allocation_plans))) return false;
    if (check && (overlaps(records, record_count,
                sizeof(*values->recipe_operations), equality_plans, equality_plan_count,
                sizeof(*values->equality_plans))
            || overlaps(plans, plan_count, sizeof(*values->allocation_plans),
                equality_plans, equality_plan_count, sizeof(*values->equality_plans))
            || overlaps(copy_plans, copy_plan_count, sizeof(*values->copy_plans),
                equality_plans, equality_plan_count, sizeof(*values->equality_plans))
            || overlaps(equality_plans, equality_plan_count,
                sizeof(*values->equality_plans), ownership_plans,
                ownership_plan_count, sizeof(*values->ownership_plans))
            || overlaps(equality_plans, equality_plan_count,
                sizeof(*values->equality_plans), variants, variant_count,
                sizeof(*values->ownership_variants))
            || overlaps(equality_plans, equality_plan_count,
                sizeof(*values->equality_plans), edges, edge_count,
                sizeof(*values->owned_edges)))) return false;
    if (check && (overlaps(records, record_count,
                sizeof(*values->recipe_operations), copy_plans, copy_plan_count,
                sizeof(*values->copy_plans))
            || overlaps(plans, plan_count, sizeof(*values->allocation_plans),
                copy_plans, copy_plan_count, sizeof(*values->copy_plans))
            || overlaps(records, record_count,
                sizeof(*values->recipe_operations), ownership_plans,
                ownership_plan_count, sizeof(*values->ownership_plans))
            || overlaps(records, record_count,
                sizeof(*values->recipe_operations), variants, variant_count,
                sizeof(*values->ownership_variants))
            || overlaps(records, record_count,
                sizeof(*values->recipe_operations), edges, edge_count,
                sizeof(*values->owned_edges))
            || overlaps(copy_plans, copy_plan_count, sizeof(*values->copy_plans),
                ownership_plans, ownership_plan_count,
                sizeof(*values->ownership_plans))
            || overlaps(copy_plans, copy_plan_count, sizeof(*values->copy_plans),
                variants, variant_count, sizeof(*values->ownership_variants))
            || overlaps(copy_plans, copy_plan_count, sizeof(*values->copy_plans),
                edges, edge_count, sizeof(*values->owned_edges))
            || overlaps(plans, plan_count, sizeof(*values->allocation_plans),
                ownership_plans, ownership_plan_count,
                sizeof(*values->ownership_plans))
            || overlaps(plans, plan_count, sizeof(*values->allocation_plans),
                variants, variant_count, sizeof(*values->ownership_variants))
            || overlaps(plans, plan_count, sizeof(*values->allocation_plans),
                edges, edge_count, sizeof(*values->owned_edges))
            || overlaps(ownership_plans, ownership_plan_count,
                sizeof(*values->ownership_plans), variants, variant_count,
                sizeof(*values->ownership_variants))
            || overlaps(ownership_plans, ownership_plan_count,
                sizeof(*values->ownership_plans), edges, edge_count,
                sizeof(*values->owned_edges))
             || overlaps(variants, variant_count,
                sizeof(*values->ownership_variants), edges, edge_count,
                sizeof(*values->owned_edges)))) return false;
    if (!validation_event(1)) return false;
#define HOST_AGAINST(pointer, count, type) \
    if (check && (overlaps(host_result_plans, host_result_plan_count, \
            sizeof(*values->host_result_plans), (pointer), (count), sizeof(type)) \
        || overlaps(host_result_requirements, host_result_requirement_count, \
            sizeof(*values->host_result_requirements), (pointer), (count), \
            sizeof(type)))) return false
    HOST_AGAINST(records, record_count, SolMirRuntimeRecipeOperations);
    HOST_AGAINST(plans, plan_count, SolMirRuntimeAllocationPlan);
    HOST_AGAINST(copy_plans, copy_plan_count, SolMirRuntimeCopyPlan);
    HOST_AGAINST(equality_plans, equality_plan_count, SolMirRuntimeEqualityPlan);
    HOST_AGAINST(ownership_plans, ownership_plan_count, SolMirRuntimeOwnershipPlan);
    HOST_AGAINST(variants, variant_count, SolMirRuntimeOwnershipVariant);
    HOST_AGAINST(edges, edge_count, SolMirRuntimeOwnedEdge);
    if (check && overlaps(host_result_plans, host_result_plan_count,
            sizeof(*values->host_result_plans), host_result_requirements,
            host_result_requirement_count,
            sizeof(*values->host_result_requirements))) return false;
#undef HOST_AGAINST
#define AGAINST(pointer, item_count, type) \
    if (!validation_event(1) || (check && (overlaps(records, record_count, \
            sizeof(*values->recipe_operations), (pointer), (item_count), \
             sizeof(type)) || overlaps(plans, plan_count, \
              sizeof(*values->allocation_plans), (pointer), (item_count), \
              sizeof(type)) || overlaps(copy_plans, copy_plan_count, \
              sizeof(*values->copy_plans), (pointer), (item_count), \
              sizeof(type)) || overlaps(equality_plans, equality_plan_count, \
              sizeof(*values->equality_plans), (pointer), (item_count), \
              sizeof(type)) || overlaps(ownership_plans, ownership_plan_count, \
            sizeof(*values->ownership_plans), (pointer), (item_count), \
            sizeof(type)) || overlaps(variants, variant_count, \
            sizeof(*values->ownership_variants), (pointer), (item_count), \
            sizeof(type)) || overlaps(edges, edge_count, \
            sizeof(*values->owned_edges), (pointer), (item_count), \
            sizeof(type)) || overlaps(host_result_plans, host_result_plan_count, \
            sizeof(*values->host_result_plans), (pointer), (item_count), \
            sizeof(type)) || overlaps(host_result_requirements, \
            host_result_requirement_count, \
            sizeof(*values->host_result_requirements), (pointer), (item_count), \
            sizeof(type))))) return false
    AGAINST(values, 1, SolMirRuntimeValues);
    AGAINST(runtime, 1, SolMirRuntimeConventions);
    const SolMirConcreteProgram *c = runtime->concrete;
#define RUNTIME_RANGE(member, type, singular) \
    AGAINST(runtime->member, runtime->singular##_capacity, type);
    SOL_MIR_RUNTIME_CONVENTIONS_ARENAS(RUNTIME_RANGE)
#undef RUNTIME_RANGE
    AGAINST(c, 1, SolMirConcreteProgram);
    AGAINST(&c->program, 1, SolMirProgram);
    AGAINST(c->program.roots, c->program.root_count, SolMirProgramRoot);
    AGAINST(c->program.approved_imports, c->program.approved_import_count,
        SolIrCallableId);
    AGAINST(c->program.templates, c->program.template_count,
        SolMirProgramTemplate);
    AGAINST(c->program.imports, c->program.import_count, SolMirProgramImport);
    AGAINST(c->program.specializations, c->program.specialization_count,
        SolMirProgramSpecialization);
    AGAINST(c->program.references, c->program.reference_count,
        SolMirProgramReference);
    for (size_t i = 0; i < c->program.template_count; ++i) {
        if (!validation_event(1)) return false;
        const SolMir *mir = &c->program.templates[i].mir;
        AGAINST(mir, 1, SolMir);
        AGAINST(mir->blocks, mir->block_capacity, SolMirBlock);
        AGAINST(mir->instructions, mir->instruction_capacity, SolMirInstruction);
        AGAINST(mir->values, mir->value_capacity, SolMirValue);
        AGAINST(mir->parameter_values, mir->parameter_value_capacity,
            SolMirValueId);
        AGAINST(mir->edge_values, mir->edge_value_capacity, SolMirValueId);
        AGAINST(mir->call_arguments, mir->call_argument_capacity,
            SolMirCallArgument);
        AGAINST(mir->loops, mir->loop_capacity, SolMirLoop);
        AGAINST(mir->construct_operands, mir->construct_operand_capacity,
            SolMirConstructOperand);
        AGAINST(mir->temporaries, mir->temporary_capacity, SolMirTemporary);
    }
    const SolMirPlan *p = &c->plan;
    AGAINST(p, 1, SolMirPlan);
#define PLAN_RANGE(member, type, singular) \
    AGAINST(p->member, p->singular##_capacity, type)
    PLAN_RANGE(types, SolMirPlanType, type);
    PLAN_RANGE(type_components, SolMirPlanTypeId, type_component);
    PLAN_RANGE(type_parameter_accesses, SolAccessMode, type_parameter_access);
    PLAN_RANGE(effect_atoms, SolMirPlanEffectAtom, effect_atom);
    PLAN_RANGE(effect_rows, SolMirPlanEffectRow, effect_row);
    PLAN_RANGE(effect_row_atoms, size_t, effect_row_atom);
    PLAN_RANGE(instances, SolMirPlanInstance, instance);
    PLAN_RANGE(instance_type_ids, SolMirPlanTypeId, instance_type_id);
    PLAN_RANGE(instance_accesses, SolAccessMode, instance_access);
    PLAN_RANGE(dictionary_entries, SolMirPlanDictionaryEntry, dictionary_entry);
    PLAN_RANGE(imports, SolMirPlanImport, import);
    PLAN_RANGE(typed_uses, SolMirPlanTypedUse, typed_use);
    PLAN_RANGE(contexts, SolMirPlanContext, context);
    PLAN_RANGE(demands, SolMirPlanDemand, demand);
#undef PLAN_RANGE
    const SolMirMaterialization *m = &c->materialization;
    AGAINST(m, 1, SolMirMaterialization);
#define MATERIAL_RANGE(member, type, singular) \
    AGAINST(m->member, m->singular##_capacity, type);
    MATERIAL_RANGE(images, SolMirMaterializedImage, image)
    MATERIAL_RANGE(types, SolMirMaterializedType, type)
    MATERIAL_RANGE(shape_fields, SolMirMaterializedShapeField, shape_field)
    MATERIAL_RANGE(shape_variants, SolMirMaterializedShapeVariant, shape_variant)
    MATERIAL_RANGE(type_ids, SolMirMaterializedTypeId, type_id)
    MATERIAL_RANGE(accesses, SolAccessMode, access)
    MATERIAL_RANGE(overlays, SolMirMaterializedTypeOverlay, overlay)
    MATERIAL_RANGE(contexts, SolMirPlanContext, context)
    MATERIAL_RANGE(locals, SolMirMaterializedLocal, local)
    MATERIAL_RANGE(places, SolMirMaterializedPlace, place)
    MATERIAL_RANGE(projections, SolMirMaterializedProjection, projection)
    MATERIAL_RANGE(values, SolMirMaterializedValue, value)
    MATERIAL_RANGE(instructions, SolMirMaterializedInstruction, instruction)
    MATERIAL_RANGE(temporaries, SolMirMaterializedTemporary, temporary)
    MATERIAL_RANGE(construct_operands, SolMirMaterializedConstructOperand,
        construct_operand)
    MATERIAL_RANGE(call_arguments, SolMirMaterializedCallArgument, call_argument)
    MATERIAL_RANGE(blocks, SolMirMaterializedBlock, block)
    MATERIAL_RANGE(edges, SolMirMaterializedEdge, edge)
    MATERIAL_RANGE(edge_values, SolMirMaterializedValueId, edge_value)
    MATERIAL_RANGE(parameter_values, SolMirMaterializedValueId, parameter_value)
    MATERIAL_RANGE(loops, SolMirMaterializedLoop, loop)
    MATERIAL_RANGE(bindings, SolMirMaterializedBinding, binding)
    MATERIAL_RANGE(semantic_sites, SolMirMaterializedSemanticSite, semantic_site)
    MATERIAL_RANGE(receiver_roots, SolMirMaterializedLocalId, receiver_root)
    MATERIAL_RANGE(imports, SolMirMaterializedImport, import)
    MATERIAL_RANGE(handlers, SolMirMaterializedHandler, handler)
    MATERIAL_RANGE(writebacks, SolMirMaterializedWriteback, writeback)
    MATERIAL_RANGE(effect_rows, SolMirMaterializedEffectRow, effect_row)
    MATERIAL_RANGE(effect_atoms, SolMirMaterializedEffectAtom, effect_atom)
    MATERIAL_RANGE(effect_row_atoms, size_t, effect_row_atom)
    MATERIAL_RANGE(effect_names, char, effect_name)
    MATERIAL_RANGE(literal_bytes, char, literal_byte)
#undef MATERIAL_RANGE
    for (size_t i = 0; i < m->image_count; ++i) {
        if (!validation_event(1)) return false;
        const SolMir *mir = &m->images[i].topology;
        AGAINST(mir, 1, SolMir);
        AGAINST(mir->blocks, mir->block_capacity, SolMirBlock);
        AGAINST(mir->instructions, mir->instruction_capacity, SolMirInstruction);
        AGAINST(mir->values, mir->value_capacity, SolMirValue);
        AGAINST(mir->parameter_values, mir->parameter_value_capacity,
            SolMirValueId);
        AGAINST(mir->edge_values, mir->edge_value_capacity, SolMirValueId);
        AGAINST(mir->call_arguments, mir->call_argument_capacity,
            SolMirCallArgument);
        AGAINST(mir->loops, mir->loop_capacity, SolMirLoop);
        AGAINST(mir->construct_operands, mir->construct_operand_capacity,
            SolMirConstructOperand);
        AGAINST(mir->temporaries, mir->temporary_capacity, SolMirTemporary);
    }
    const SolMirRepresentation *r = &c->representation;
    AGAINST(r, 1, SolMirRepresentation);
    AGAINST(r->recipes, r->recipe_capacity, SolMirRecipe);
    AGAINST(r->fields, r->field_capacity, SolMirRecipeField);
    AGAINST(r->variants, r->variant_capacity, SolMirRecipeVariant);
    AGAINST(r->recipe_ids, r->recipe_id_capacity, SolMirRecipeId);
    AGAINST(r->accesses, r->access_capacity, SolAccessMode);
    AGAINST(r->receiver_roots, r->receiver_root_capacity,
        SolMirMaterializedLocalId);
    AGAINST(r->callable_producers, r->callable_producer_capacity,
        SolMirCallableProducer);
    const SolMirLayout *layout = &c->layout;
    AGAINST(layout, 1, SolMirLayout);
    AGAINST(layout->types, layout->type_capacity, SolMirTypeLayout);
    AGAINST(layout->fields, layout->field_capacity, SolMirFieldLayout);
    AGAINST(layout->variants, layout->variant_capacity, SolMirVariantLayout);
    AGAINST(layout->projections, layout->projection_capacity,
        SolMirProjectionMap);
    AGAINST(&c->operations, 1, SolMirOperations);
#define OP_RANGE(member, type, singular) \
    AGAINST(c->operations.member, c->operations.singular##_capacity, type);
    SOL_MIR_OPERATIONS_ARENAS(OP_RANGE)
#undef OP_RANGE
    AGAINST(&c->linkage, 1, SolMirLinkage);
#define LINK_RANGE(member, type, singular) \
    AGAINST(c->linkage.member, c->linkage.singular##_capacity, type);
    SOL_MIR_LINKAGE_ARENAS(LINK_RANGE)
#undef LINK_RANGE
    const SolIr *ir = c->program.ir;
    AGAINST(ir, 1, SolIr);
#define IR_RANGE(member, item_count, type) \
    AGAINST(ir->member, ir->item_count, type)
    IR_RANGE(definitions, definition_count, SolIrDefinition);
    IR_RANGE(callables, callable_count, SolIrCallable);
    IR_RANGE(types, type_count, SolIrType);
    IR_RANGE(type_ids, type_id_count, SolIrTypeId);
    IR_RANGE(accesses, access_count, SolAccessMode);
    IR_RANGE(members, member_count, SolIrMember);
    IR_RANGE(evidence, evidence_count, SolIrDispatchEvidence);
    IR_RANGE(locals, local_count, SolIrLocal);
    IR_RANGE(fields, field_count, SolIrField);
    IR_RANGE(variants, variant_count, SolIrVariant);
    IR_RANGE(expressions, expression_count, SolIrExpression);
    IR_RANGE(places, place_count, SolIrPlace);
    IR_RANGE(projections, projection_count, SolIrProjection);
    IR_RANGE(statements, statement_count, SolIrStatement);
    IR_RANGE(statement_ids, statement_id_count, SolIrStatementId);
    IR_RANGE(arms, arm_count, SolIrArm);
    IR_RANGE(arm_ids, arm_id_count, SolIrArmId);
    IR_RANGE(patterns, pattern_count, SolIrPattern);
    IR_RANGE(pattern_children, pattern_child_count, SolIrPatternChild);
    IR_RANGE(operands, operand_count, SolIrOperand);
    IR_RANGE(roots, root_count, SolIrLocalId);
    IR_RANGE(obligations, obligation_count, SolIrObligation);
    IR_RANGE(snapshots, snapshot_count, SolIrSnapshot);
    IR_RANGE(cleanup_locals, cleanup_local_count, SolIrLocalId);
    IR_RANGE(effects, effect_count, SolIrEffect);
    IR_RANGE(generic_parameters, generic_parameter_count, SolIrGenericParameter);
    IR_RANGE(effect_parameters, effect_parameter_count, SolIrEffectParameter);
    IR_RANGE(loop_obligations, loop_obligation_count, SolObligationId);
    IR_RANGE(unreachable_obligations, unreachable_obligation_count,
        SolObligationId);
    IR_RANGE(files, file_count, SolIrSourceFile);
#undef IR_RANGE
    size_t text_size;
    if (!measured_text_size(ir->source_path, &text_size)) return false;
    AGAINST(ir->source_path, text_size, char);
    AGAINST(ir->source_bytes, ir->source_length + 1, char);
    for (size_t i = 0; i < p->effect_atom_count; ++i) {
        if (!validation_event(1)) return false;
        AGAINST(p->effect_atoms[i].name, p->effect_atoms[i].length + 1, char);
    }
#define OPTIONAL_TEXT(array, item_count, member) do { \
    for (size_t text_i = 0; text_i < (item_count); ++text_i) { \
        if (!validation_event(1)) return false; \
        if ((array)[text_i].member != NULL) { \
            if (!measured_text_size((array)[text_i].member, &text_size)) \
                return false; \
            AGAINST((array)[text_i].member, text_size, char); \
        } \
    } \
} while (0)
    OPTIONAL_TEXT(ir->definitions, ir->definition_count, name);
    OPTIONAL_TEXT(ir->callables, ir->callable_count, name);
    OPTIONAL_TEXT(ir->locals, ir->local_count, name);
    OPTIONAL_TEXT(ir->fields, ir->field_count, name);
    OPTIONAL_TEXT(ir->variants, ir->variant_count, name);
    OPTIONAL_TEXT(ir->statements, ir->statement_count, region_label);
#undef OPTIONAL_TEXT
    for (size_t i = 0; i < ir->effect_count; ++i) {
        if (!validation_event(1)
            || !measured_text_size(ir->effects[i].name, &text_size)) return false;
        AGAINST(ir->effects[i].name, text_size, char);
    }
    for (size_t i = 0; i < ir->expression_count; ++i) {
        if (!validation_event(1)) return false;
        if (ir->expressions[i].kind == SOL_IR_EXPR_STRING) {
            if (!measured_text_size(ir->expressions[i].as.string, &text_size))
                return false;
            AGAINST(ir->expressions[i].as.string, text_size, char);
        } else if (ir->expressions[i].kind == SOL_IR_EXPR_HANDLE) {
            if (!measured_text_size(ir->expressions[i].as.handler.effect_name,
                    &text_size)) return false;
            AGAINST(ir->expressions[i].as.handler.effect_name, text_size, char);
        }
    }
    for (size_t i = 0; i < ir->generic_parameter_count; ++i) {
        if (!validation_event(1)
            || !measured_text_size(ir->generic_parameters[i].name, &text_size))
            return false;
        AGAINST(ir->generic_parameters[i].name, text_size, char);
    }
    for (size_t i = 0; i < ir->effect_parameter_count; ++i) {
        if (!validation_event(1)
            || !measured_text_size(ir->effect_parameters[i].name, &text_size))
            return false;
        AGAINST(ir->effect_parameters[i].name, text_size, char);
    }
    for (size_t i = 0; i < ir->file_count; ++i) {
        if (!validation_event(1)
            || !measured_text_size(ir->files[i].path, &text_size)) return false;
        AGAINST(ir->files[i].path, text_size, char);
    }
#undef AGAINST
    return true;
}

static uint32_t value_operation_mask(void) {
    return SOL_MIR_LINKAGE_RUNTIME_CREATE | SOL_MIR_LINKAGE_RUNTIME_COPY
        | SOL_MIR_LINKAGE_RUNTIME_DROP | SOL_MIR_LINKAGE_RUNTIME_EQUAL;
}

static SolMirRuntimeImportId record_import(
    const SolMirRuntimeRecipeOperations *record, uint32_t operation) {
    if (operation == SOL_MIR_LINKAGE_RUNTIME_CREATE) return record->create_import;
    if (operation == SOL_MIR_LINKAGE_RUNTIME_COPY) return record->copy_import;
    if (operation == SOL_MIR_LINKAGE_RUNTIME_DROP) return record->drop_import;
    if (operation == SOL_MIR_LINKAGE_RUNTIME_EQUAL) return record->equal_import;
    return SOL_MIR_RUNTIME_NONE;
}

static SolMirRuntimeImportKind import_kind(uint32_t operation) {
    if (operation == SOL_MIR_LINKAGE_RUNTIME_CREATE)
        return SOL_MIR_RUNTIME_IMPORT_RECIPE_CREATE;
    if (operation == SOL_MIR_LINKAGE_RUNTIME_COPY)
        return SOL_MIR_RUNTIME_IMPORT_RECIPE_COPY;
    if (operation == SOL_MIR_LINKAGE_RUNTIME_DROP)
        return SOL_MIR_RUNTIME_IMPORT_RECIPE_DROP;
    return SOL_MIR_RUNTIME_IMPORT_RECIPE_EQUAL;
}

static bool validate_records(const SolMirRuntimeValues *values,
    unsigned char *requirement_seen, unsigned char *import_consumed) {
    const SolMirRuntimeConventions *runtime = values->conventions;
    const SolMirLinkage *linkage = &runtime->concrete->linkage;
    for (size_t recipe = 0; recipe < values->recipe_operation_count; ++recipe) {
        if (!validation_event(1)) return false;
        const SolMirRuntimeRecipeOperations *record
            = &values->recipe_operations[recipe];
        if (record->recipe != recipe
            || (record->demanded_operations & ~value_operation_mask()) != 0)
            return false;
    }
    for (size_t i = 0; i < linkage->runtime_requirement_count; ++i) {
        if (!validation_event(1)) return false;
        const SolMirLinkageRuntimeRequirement *requirement
            = &linkage->runtime_requirements[i];
        if (requirement->recipe >= values->recipe_operation_count
            || requirement_seen[requirement->recipe]) return false;
        requirement_seen[requirement->recipe] = 1;
        if (values->recipe_operations[requirement->recipe].demanded_operations
                != (requirement->operations & value_operation_mask())) return false;
    }
    for (size_t recipe = 0; recipe < values->recipe_operation_count; ++recipe) {
        if (!validation_event(1)) return false;
        const SolMirRuntimeRecipeOperations *record
            = &values->recipe_operations[recipe];
        uint32_t expected = record->demanded_operations;
        if (!requirement_seen[recipe] && expected != 0) return false;
        const uint32_t operations[] = {SOL_MIR_LINKAGE_RUNTIME_CREATE,
            SOL_MIR_LINKAGE_RUNTIME_COPY, SOL_MIR_LINKAGE_RUNTIME_DROP,
            SOL_MIR_LINKAGE_RUNTIME_EQUAL};
        for (size_t q = 0; q < sizeof(operations) / sizeof(operations[0]); ++q) {
            if (!validation_event(1)) return false;
            size_t id = record_import(record, operations[q]);
            bool demanded = (expected & operations[q]) != 0;
            if (!demanded) {
                if (id != SOL_MIR_RUNTIME_NONE) return false;
                continue;
            }
            if (id >= runtime->import_count || import_consumed[id]) return false;
            const SolMirRuntimeImport *import = &runtime->imports[id];
            if (import->recipe != recipe
                || import->recipe_operation != operations[q]
                || import->kind != import_kind(operations[q])) return false;
            import_consumed[id] = 1;
        }
    }
    for (size_t i = 0; i < runtime->import_count; ++i) {
        if (!validation_event(1)) return false;
        bool value_import = runtime->imports[i].kind
                >= SOL_MIR_RUNTIME_IMPORT_RECIPE_CREATE
            && runtime->imports[i].kind <= SOL_MIR_RUNTIME_IMPORT_RECIPE_EQUAL;
        if (value_import != (import_consumed[i] != 0)) return false;
    }
    return true;
}

static bool validate_copy_plans(const SolMirRuntimeValues *values) {
    const SolMirRepresentation *representation
        = &values->conventions->concrete->representation;
    for (size_t recipe = 0; recipe < values->copy_plan_count; ++recipe) {
        SolMirRuntimeCopyClass classification;
        if (!validation_event(1)
            || !reconstructed_copy_class(&representation->recipes[recipe],
                &classification)
            || values->copy_plans[recipe].recipe != recipe
            || values->copy_plans[recipe].classification != classification)
            return false;
    }
    return true;
}

static bool validate_equality_plans(const SolMirRuntimeValues *values,
    unsigned char *eligible) {
    const SolMirRepresentation *representation
        = &values->conventions->concrete->representation;
    size_t work = 0;
    if ((representation->recipe_count != 0 && eligible == NULL)
        || !reconstructed_equality_classes(representation, eligible, &work)
        || !validation_event(work)) return false;
    for (size_t recipe = 0; recipe < values->equality_plan_count; ++recipe) {
        if (values->equality_plans[recipe].recipe != recipe
            || values->equality_plans[recipe].classification
                != reconstructed_equality_class(&representation->recipes[recipe],
                    eligible[recipe] != 0)) return false;
    }
    return true;
}

static bool validate_host_result_plans(const SolMirRuntimeValues *values,
    unsigned char *eligible) {
    const SolMirRepresentation *representation
        = &values->conventions->concrete->representation;
    size_t work = 0;
    if ((representation->recipe_count != 0 && eligible == NULL)
        || !reconstructed_host_result_classes(representation, eligible, &work)
        || !validation_event(work)) return false;
    for (size_t recipe = 0; recipe < values->host_result_plan_count; ++recipe) {
        if (!validation_event(1)
            || values->host_result_plans[recipe].recipe != recipe
            || values->host_result_plans[recipe].classification
                != reconstructed_host_result_class(&representation->recipes[recipe],
                    eligible[recipe] != 0)) return false;
    }
    const SolMirLinkage *linkage = &values->conventions->concrete->linkage;
    for (size_t host = 0; host < values->host_result_requirement_count; ++host) {
        if (!validation_event(1)
            || values->host_result_requirements[host].host != host
            || values->host_result_requirements[host].result
                != linkage->host_requirements[host].result
            || values->host_result_requirements[host].result
                >= values->host_result_plan_count) return false;
        SolMirRuntimeHostResultClass c = values->host_result_plans[
            values->host_result_requirements[host].result].classification;
        if (c == SOL_MIR_RUNTIME_HOST_RESULT_UNREACHABLE
            || c == SOL_MIR_RUNTIME_HOST_RESULT_FORBIDDEN) return false;
    }
    return true;
}

static bool validate_plans(const SolMirRuntimeValues *values) {
    const SolMirLayout *layout = &values->conventions->concrete->layout;
    for (size_t recipe = 0; recipe < values->allocation_plan_count; ++recipe) {
        if (!validation_event(1)) return false;
        const SolMirRuntimeAllocationPlan *plan = &values->allocation_plans[recipe];
        const SolMirTypeLayout *type = &layout->types[recipe];
        SolMirRuntimeAllocationPlanKind kind
            = SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE;
        uint64_t size = 0, alignment = 1;
        if (type->object_kind == SOL_MIR_LAYOUT_OBJECT_PRODUCT
            || type->object_kind == SOL_MIR_LAYOUT_OBJECT_SUM) {
            kind = SOL_MIR_RUNTIME_ALLOCATION_PLAN_FIXED_OBJECT;
            size = type->object_size; alignment = type->object_alignment;
        } else if (type->object_kind == SOL_MIR_LAYOUT_OBJECT_TEXT) {
            kind = SOL_MIR_RUNTIME_ALLOCATION_PLAN_TEXT;
            size = type->object_size; alignment = type->object_alignment;
        }
        if (plan->recipe != recipe || plan->kind != kind
            || plan->object_size != size || plan->object_alignment != alignment)
            return false;
    }
    return true;
}

static bool same_runtime_slice(SolMirRuntimeSlice actual, size_t offset,
    size_t count) {
    return actual.offset == offset && actual.count == count;
}

static bool validate_owned_edge(const SolMirRuntimeOwnedEdge *actual,
    SolMirRuntimeOwnedEdgeKind kind, SolMirRecipeId recipe, size_t ordinal,
    size_t producer) {
    return actual->kind == kind && actual->recipe == recipe
        && actual->ordinal == ordinal && actual->producer == producer;
}

static bool validate_ownership_plans(const SolMirRuntimeValues *values) {
    const SolMirRepresentation *r = &values->conventions->concrete->representation;
    size_t edge_at = 0, variant_at = 0;
    for (size_t recipe = 0; recipe < values->ownership_plan_count; ++recipe) {
        if (!validation_event(1)) return false;
        const SolMirRecipe *item = &r->recipes[recipe];
        const SolMirRuntimeOwnershipPlan *plan = &values->ownership_plans[recipe];
        SolMirRuntimeOwnershipClass classification
            = reconstructed_ownership_class(item);
        size_t edge_start = edge_at, variant_start = variant_at;
        if (plan->recipe != recipe || plan->classification != classification
            || plan->edges.offset != edge_start
            || plan->variants.offset != variant_start) return false;
        if (classification == SOL_MIR_RUNTIME_OWNERSHIP_PRODUCT) {
            for (size_t f = 0; f < item->fields.count; ++f) {
                if (!validation_event(1) || edge_at >= values->owned_edge_count)
                    return false;
                const SolMirRecipeField *field = &r->fields[item->fields.offset + f];
                if (!validate_owned_edge(&values->owned_edges[edge_at],
                        SOL_MIR_RUNTIME_OWNED_EDGE_FIELD, field->type,
                        field->ordinal, SOL_MIR_RUNTIME_NONE)) return false;
                ++edge_at;
            }
        } else if (classification == SOL_MIR_RUNTIME_OWNERSHIP_SUM) {
            for (size_t v = 0; v < item->variants.count; ++v) {
                if (!validation_event(1)
                    || variant_at >= values->ownership_variant_count) return false;
                const SolMirRecipeVariant *source = &r->variants[
                    item->variants.offset + v];
                const SolMirRuntimeOwnershipVariant *variant
                    = &values->ownership_variants[variant_at];
                if (variant->ordinal != source->ordinal
                    || variant->semantic_tag != source->semantic_tag
                    || !same_runtime_slice(variant->edges, edge_at,
                        source->fields.count)) return false;
                ++variant_at;
                for (size_t f = 0; f < source->fields.count; ++f) {
                    if (!validation_event(1) || edge_at >= values->owned_edge_count)
                        return false;
                    const SolMirRecipeField *field = &r->fields[
                        source->fields.offset + f];
                    if (!validate_owned_edge(&values->owned_edges[edge_at],
                            SOL_MIR_RUNTIME_OWNED_EDGE_FIELD, field->type,
                            field->ordinal, SOL_MIR_RUNTIME_NONE)) return false;
                    ++edge_at;
                }
            }
        } else if (classification == SOL_MIR_RUNTIME_OWNERSHIP_WRAPPER) {
            if (!validation_event(1) || edge_at >= values->owned_edge_count
                || !validate_owned_edge(&values->owned_edges[edge_at],
                    SOL_MIR_RUNTIME_OWNED_EDGE_BACKING, item->backing, 0,
                    SOL_MIR_RUNTIME_NONE)) return false;
            ++edge_at;
        } else if (classification == SOL_MIR_RUNTIME_OWNERSHIP_CAPABILITY
            && item->capability_source != SOL_MIR_RECIPE_NONE) {
            if (!validation_event(1) || edge_at >= values->owned_edge_count
                || !validate_owned_edge(&values->owned_edges[edge_at],
                    SOL_MIR_RUNTIME_OWNED_EDGE_PRIVATE_SOURCE,
                    item->capability_source, 0, SOL_MIR_RUNTIME_NONE)) return false;
            ++edge_at;
        } else if (classification == SOL_MIR_RUNTIME_OWNERSHIP_CALLABLE) {
            for (size_t p = 0; p < r->callable_producer_count; ++p) {
                if (!validation_event(1)) return false;
                const SolMirCallableProducer *producer = &r->callable_producers[p];
                if (producer->function_recipe == recipe && producer->kind
                        == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION) {
                    if (edge_at >= values->owned_edge_count
                        || !validate_owned_edge(&values->owned_edges[edge_at],
                            SOL_MIR_RUNTIME_OWNED_EDGE_CAPTURED_RECEIVER,
                            producer->captured_receiver_type, 0, p)) return false;
                    ++edge_at;
                }
            }
        }
        size_t expected_edges = classification == SOL_MIR_RUNTIME_OWNERSHIP_SUM
            ? 0 : edge_at - edge_start;
        size_t expected_variants = variant_at - variant_start;
        if (plan->edges.count != expected_edges
            || plan->variants.count != expected_variants) return false;
    }
    return edge_at == values->owned_edge_count
        && variant_at == values->ownership_variant_count;
}

static SolMirRuntimeValuesBuildOutcome validate_predecessor(
    const SolMirRuntimeConventions *conventions, size_t *work, size_t *scratch,
    SolDiagnostics *diagnostics) {
    SolDiagnostics local;
    sol_diagnostics_init(&local);
    bool valid = conventions != NULL
        && sol_mir_runtime_conventions_internal_validation_requirements(
            conventions, work, scratch, &local)
        && *work == conventions->usage.validation_work
        && *scratch == conventions->usage.validation_scratch_bytes;
    bool allocation_failed = local.allocation_failed;
    sol_diagnostics_free(&local);
    if (valid) return SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED;
    if (allocation_failed) {
        if (diagnostics != NULL) diagnostics->allocation_failed = true;
        invalid(diagnostics, "runtime values predecessor validation allocation failed");
        return SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED;
    }
    invalid(diagnostics, "runtime values predecessor is invalid");
    return SOL_MIR_RUNTIME_VALUES_BUILD_INVALID_CONVENTIONS;
}

static bool measure_alias_work(const SolMirRuntimeConventions *conventions,
    size_t limit, size_t *work) {
    metered_validation_work = 0;
    metered_validation_limit = limit;
    validation_work_exhausted = false;
    if (!walk_aliases(NULL, conventions, false)) return false;
    *work = metered_validation_work;
    return true;
}

SolMirRuntimeValuesBuildOutcome sol_mir_runtime_values_internal_preflight(
    const SolMirRuntimeConventions *conventions,
    const SolMirRuntimeValuesLimits *limits, SolMirRuntimeValuesUsage *usage,
    SolDiagnostics *diagnostics) {
    if (conventions == NULL || limits == NULL || usage == NULL
        || !limits_complete(*limits))
        return SOL_MIR_RUNTIME_VALUES_BUILD_INVALID_ARGUMENT;
    size_t predecessor_work = 0, predecessor_scratch = 0;
    SolMirRuntimeValuesBuildOutcome predecessor = validate_predecessor(
        conventions, &predecessor_work, &predecessor_scratch, diagnostics);
    if (predecessor != SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED)
        return predecessor;
    size_t alias_work;
    size_t records = conventions->concrete->representation.recipe_count;
    size_t reconstruction_bytes;
    if (!mul_size(records, 2, &reconstruction_bytes)) {
        invalid(diagnostics, "runtime values reconstruction scratch overflowed");
        return SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED;
    }
    unsigned char *reconstruction_scratch = validation_scratch_allocate(reconstruction_bytes);
    if (records != 0 && reconstruction_scratch == NULL) {
        if (diagnostics != NULL) diagnostics->allocation_failed = true;
        invalid(diagnostics, "runtime values validation scratch allocation failed");
        return SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED;
    }
    if (!measure_alias_work(conventions, SIZE_MAX, &alias_work)
        || !reconstruct_usage(conventions, predecessor_work,
            predecessor_scratch, alias_work, reconstruction_scratch, usage)) {
        free(reconstruction_scratch);
        invalid(diagnostics, "runtime values resource reconstruction overflowed");
        return SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED;
    }
    free(reconstruction_scratch);
    if (!usage_fits(usage, limits)) {
        invalid(diagnostics, "runtime values preflight resource limit exceeded");
        return SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED;
    }
    return SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED;
}

SolMirRuntimeValuesBuildOutcome sol_mir_runtime_values_internal_validate(
    const SolMirRuntimeValues *values, SolDiagnostics *diagnostics) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    validation_allocation_attempts = 0;
#endif
    if (values == NULL || values->conventions == NULL)
        {
            invalid(diagnostics, "runtime values owner is empty");
            return SOL_MIR_RUNTIME_VALUES_BUILD_INTERNAL_FAILED;
        }
    if (!limits_complete(values->limits)) {
        invalid(diagnostics, "runtime values limits are incomplete");
        return SOL_MIR_RUNTIME_VALUES_BUILD_INTERNAL_FAILED;
    }
    size_t predecessor_work = 0, predecessor_scratch = 0;
    SolMirRuntimeValuesBuildOutcome predecessor = validate_predecessor(
        values->conventions, &predecessor_work, &predecessor_scratch,
        diagnostics);
    if (predecessor != SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED)
        return predecessor;
    if (predecessor_work > values->limits.max_validation_work) {
        invalid(diagnostics, "runtime values predecessor work limit exceeded");
        return SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED;
    }
    metered_validation_work = 0;
    metered_validation_limit
        = values->limits.max_validation_work - predecessor_work;
    validation_work_exhausted = false;
    if (!validation_event(VALIDATION_HEADER_WORK)) {
        invalid(diagnostics, "runtime values validation work limit exceeded");
        return SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED;
    }
    size_t alias_start = metered_validation_work;
    if (!walk_aliases(NULL, values->conventions, false)) {
        invalid(diagnostics, "runtime values validation preflight work exceeded");
        return SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED;
    }
    size_t alias_work = metered_validation_work - alias_start;
    size_t reconstruction_records = values->conventions->concrete->representation
        .recipe_count, reconstruction_bytes;
    if (!mul_size(reconstruction_records, 2, &reconstruction_bytes)) {
        invalid(diagnostics, "runtime values reconstruction scratch overflowed");
        return SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED;
    }
    unsigned char *reconstruction_scratch
        = validation_scratch_allocate(reconstruction_bytes);
    if (reconstruction_records != 0 && reconstruction_scratch == NULL) {
        if (diagnostics != NULL) diagnostics->allocation_failed = true;
        invalid(diagnostics, "runtime values validation scratch allocation failed");
        return SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED;
    }
    SolMirRuntimeValuesUsage expected;
    if (!reconstruct_usage(values->conventions, predecessor_work,
            predecessor_scratch, alias_work, reconstruction_scratch, &expected)
        || values->recipe_operation_count != expected.records
        || values->recipe_operation_capacity != expected.records
            || values->allocation_plan_count != expected.allocation_plans
            || values->allocation_plan_capacity != expected.allocation_plans
            || values->copy_plan_count != expected.copy_plans
            || values->copy_plan_capacity != expected.copy_plans
            || values->equality_plan_count != expected.equality_plans
            || values->equality_plan_capacity != expected.equality_plans
            || values->host_result_plan_count != expected.host_result_plans
            || values->host_result_plan_capacity != expected.host_result_plans
            || values->host_result_requirement_count != expected.host_result_requirements
            || values->host_result_requirement_capacity != expected.host_result_requirements
            || values->ownership_plan_count != expected.ownership_plans
            || values->ownership_plan_capacity != expected.ownership_plans
            || values->ownership_variant_count != expected.ownership_variants
            || values->ownership_variant_capacity != expected.ownership_variants
            || values->owned_edge_count != expected.owned_edges
            || values->owned_edge_capacity != expected.owned_edges
        || !range_valid(values->recipe_operations,
            values->recipe_operation_capacity,
            sizeof(*values->recipe_operations))
        || !range_valid(values->allocation_plans,
            values->allocation_plan_capacity,
            sizeof(*values->allocation_plans))
        || !range_valid(values->copy_plans, values->copy_plan_capacity,
            sizeof(*values->copy_plans))
        || !range_valid(values->equality_plans, values->equality_plan_capacity,
            sizeof(*values->equality_plans))
        || !range_valid(values->host_result_plans,
            values->host_result_plan_capacity, sizeof(*values->host_result_plans))
        || !range_valid(values->host_result_requirements,
            values->host_result_requirement_capacity,
            sizeof(*values->host_result_requirements))
        || !range_valid(values->ownership_plans,
            values->ownership_plan_capacity,
            sizeof(*values->ownership_plans))
        || !range_valid(values->ownership_variants,
            values->ownership_variant_capacity,
            sizeof(*values->ownership_variants))
        || !range_valid(values->owned_edges, values->owned_edge_capacity,
            sizeof(*values->owned_edges))
        || !usage_fits(&expected, &values->limits)
        || memcmp(&expected, &values->usage, sizeof(expected)) != 0) {
        free(reconstruction_scratch);
        invalid(diagnostics, "runtime values preflight or usage is invalid");
        return SOL_MIR_RUNTIME_VALUES_BUILD_INTERNAL_FAILED;
    }
    free(reconstruction_scratch);
    if (!walk_aliases(values, values->conventions, true)) {
        invalid(diagnostics, "runtime values owner aliases predecessor data");
        return validation_work_exhausted
            ? SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED
            : SOL_MIR_RUNTIME_VALUES_BUILD_INTERNAL_FAILED;
    }
    size_t recipes = values->recipe_operation_count;
    size_t imports = values->conventions->import_count;
    size_t scratch_bytes = recipes;
    if (!add_size(&scratch_bytes, imports) || !add_size(&scratch_bytes, recipes)
        || !add_size(&scratch_bytes, recipes)) {
        invalid(diagnostics, "runtime values validation scratch overflow");
        return SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED;
    }
    unsigned char *scratch = NULL;
    if (scratch_bytes != 0) {
        if (!validation_event(1)) {
            invalid(diagnostics, "runtime values validation work limit exceeded");
            return SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED;
        }
        scratch = validation_scratch_allocate(scratch_bytes);
        if (scratch == NULL) {
            if (diagnostics != NULL) diagnostics->allocation_failed = true;
            invalid(diagnostics,
                "runtime values validation scratch allocation failed");
            return SOL_MIR_RUNTIME_VALUES_BUILD_ALLOCATION_FAILED;
        }
    }
    unsigned char *import_consumed = scratch == NULL ? NULL : scratch + recipes;
    unsigned char *equality_eligible = import_consumed == NULL ? NULL
        : import_consumed + imports;
    unsigned char *host_result_eligible = equality_eligible == NULL ? NULL
        : equality_eligible + recipes;
    bool ok = validate_records(values, scratch, import_consumed);
    if (ok) ok = validate_plans(values);
    if (ok) ok = validate_copy_plans(values);
    if (ok) ok = validate_equality_plans(values, equality_eligible);
    if (ok) ok = validate_host_result_plans(values, host_result_eligible);
    if (ok) ok = validate_ownership_plans(values);
    free(scratch);
    if (!ok) {
        invalid(diagnostics, "runtime values records or allocation plans are not reconstructive");
        return validation_work_exhausted
            ? SOL_MIR_RUNTIME_VALUES_BUILD_RESOURCE_EXHAUSTED
            : SOL_MIR_RUNTIME_VALUES_BUILD_INTERNAL_FAILED;
    }
    size_t total_work = predecessor_work;
    if (!add_size(&total_work, metered_validation_work)
        || total_work != expected.validation_work) {
        invalid(diagnostics, "runtime values validation work is not exact");
        return SOL_MIR_RUNTIME_VALUES_BUILD_INTERNAL_FAILED;
    }
    return SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED;
}

bool sol_mir_runtime_values_validate(const SolMirRuntimeValues *values,
    SolDiagnostics *diagnostics) {
    return sol_mir_runtime_values_internal_validate(values, diagnostics)
        == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
bool sol_mir_runtime_values_test_reconstruct_usage(
    const SolMirRuntimeConventions *conventions,
    const SolMirRuntimeValuesLimits *limits, SolMirRuntimeValuesUsage *usage) {
    return sol_mir_runtime_values_internal_preflight(conventions, limits, usage,
        NULL) == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED;
}
#endif
