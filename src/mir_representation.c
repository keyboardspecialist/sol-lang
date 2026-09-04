#include "sol/mir_representation.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SolMirRepresentation *out;
    SolDiagnostics *diagnostics;
    SolMirRepresentationBuildOutcome outcome;
} Builder;

typedef struct { uintptr_t start, end; } Range;

static bool error(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) sol_diagnostics_add(diagnostics,
        "SOL-MIR-REPRESENTATION-001", SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    return false;
}

static bool fail(Builder *builder, SolMirRepresentationBuildOutcome outcome,
    const char *message) {
    builder->outcome = outcome;
    return error(builder->diagnostics, message);
}

static bool owner_empty(const SolMirRepresentation *owner) {
    if (owner == NULL) return false;
    const unsigned char *bytes = (const unsigned char *)owner;
    for (size_t i = 0; i < sizeof(*owner); ++i) if (bytes[i] != 0) return false;
    return true;
}

void sol_mir_representation_init(SolMirRepresentation *owner) {
    if (owner != NULL) memset(owner, 0, sizeof(*owner));
}

void sol_mir_representation_free(SolMirRepresentation *owner) {
    if (owner == NULL) return;
    free(owner->recipes); free(owner->fields); free(owner->variants);
    free(owner->recipe_ids); free(owner->accesses);
    free(owner->receiver_roots);
    free(owner->callable_producers);
    sol_mir_representation_init(owner);
}

SolMirRepresentationLimits sol_mir_representation_default_limits(void) {
    return (SolMirRepresentationLimits){
        .max_recipes = 12000000,
        .max_fields = 24000000,
        .max_variants = 12000000,
        .max_recipe_ids = 12000000,
        .max_callable_producers = 4000000,
        .max_receiver_roots = 8000000,
        .max_owned_bytes = 768u * 1024u * 1024u,
        .max_build_scratch_bytes = 128u * 1024u * 1024u,
        .max_build_work = 100000000,
        .max_validation_work = 1000000000,
        .max_validation_scratch_bytes = 896u * 1024u * 1024u,
    };
}

static bool limits_zero(SolMirRepresentationLimits value) {
    return value.max_recipes == 0 && value.max_fields == 0
        && value.max_variants == 0 && value.max_recipe_ids == 0
        && value.max_callable_producers == 0 && value.max_receiver_roots == 0
        && value.max_owned_bytes == 0 && value.max_build_scratch_bytes == 0
        && value.max_build_work == 0 && value.max_validation_work == 0
        && value.max_validation_scratch_bytes == 0;
}

static bool limits_complete(SolMirRepresentationLimits value) {
    return value.max_recipes != 0 && value.max_fields != 0
        && value.max_variants != 0 && value.max_recipe_ids != 0
        && value.max_callable_producers != 0 && value.max_receiver_roots != 0
        && value.max_owned_bytes != 0 && value.max_build_scratch_bytes != 0
        && value.max_build_work != 0 && value.max_validation_work != 0
        && value.max_validation_scratch_bytes != 0;
}

static bool add(size_t *value, size_t amount) {
    if (amount > SIZE_MAX - *value) return false;
    *value += amount;
    return true;
}

static bool multiply(size_t left, size_t right, size_t *result) {
    if (left != 0 && right > SIZE_MAX / left) return false;
    *result = left * right; return true;
}

static bool charge(Builder *builder, size_t amount) {
    return add(&builder->out->usage.build_work, amount)
        && builder->out->usage.build_work <= builder->out->limits.max_build_work;
}

static void *allocate(Builder *builder, size_t count, size_t size) {
    if (count == 0) return NULL;
    if (size == 0 || count > SIZE_MAX / size
        || !add(&builder->out->usage.owned_bytes, count * size)
        || builder->out->usage.owned_bytes > builder->out->limits.max_owned_bytes) {
        builder->outcome = SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED;
        return NULL;
    }
    void *result = calloc(count, size);
    if (result == NULL) {
        builder->out->usage.owned_bytes -= count * size;
        builder->outcome = SOL_MIR_REPRESENTATION_BUILD_ALLOCATION_FAILED;
    }
    return result;
}

static SolMirRecipeKind recipe_kind(const SolMirMaterializedType *type) {
    switch (type->kind) {
        case SOL_IR_TYPE_INT64: return SOL_MIR_RECIPE_INT64;
        case SOL_IR_TYPE_BOOL: return SOL_MIR_RECIPE_BOOL;
        case SOL_IR_TYPE_TEXT: return SOL_MIR_RECIPE_TEXT;
        case SOL_IR_TYPE_UNIT: return SOL_MIR_RECIPE_UNIT;
        case SOL_IR_TYPE_NEVER: return SOL_MIR_RECIPE_NEVER;
        case SOL_IR_TYPE_OPTION: return SOL_MIR_RECIPE_OPTION;
        case SOL_IR_TYPE_RESULT: return SOL_MIR_RECIPE_RESULT;
        case SOL_IR_TYPE_TUPLE: return SOL_MIR_RECIPE_TUPLE;
        case SOL_IR_TYPE_FUNCTION: return SOL_MIR_RECIPE_FUNCTION;
        case SOL_IR_TYPE_NOMINAL: break;
        default: return (SolMirRecipeKind)-1;
    }
    switch (type->nominal_category) {
        case SOL_MIR_MATERIALIZED_NOMINAL_RECORD: return SOL_MIR_RECIPE_RECORD;
        case SOL_MIR_MATERIALIZED_NOMINAL_ENUM: return SOL_MIR_RECIPE_ENUM;
        case SOL_MIR_MATERIALIZED_NOMINAL_DISTINCT: return SOL_MIR_RECIPE_DISTINCT;
        case SOL_MIR_MATERIALIZED_NOMINAL_REFINED: return SOL_MIR_RECIPE_REFINED;
        case SOL_MIR_MATERIALIZED_NOMINAL_CAPABILITY: return SOL_MIR_RECIPE_CAPABILITY;
        default: return (SolMirRecipeKind)-1;
    }
}

static bool count_graph(Builder *builder, size_t *field_count,
    size_t *variant_count, size_t *id_count, size_t *producer_count,
    size_t *root_count) {
    const SolMirMaterialization *source = builder->out->materialization;
    *field_count = 0; *variant_count = 0; *id_count = 0; *producer_count = 0;
    *root_count = 0;
    for (size_t i = 0; i < source->type_count; ++i) {
        if (!charge(builder, 1)) return fail(builder,
            SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED,
            "representation count work limit exceeded");
        const SolMirMaterializedType *type = &source->types[i];
        SolMirRecipeKind kind = recipe_kind(type);
        if ((int)kind < 0 || (kind == SOL_MIR_RECIPE_ENUM && type->nominal_open)) {
            return fail(builder, SOL_MIR_REPRESENTATION_BUILD_UNSUPPORTED,
                kind == SOL_MIR_RECIPE_ENUM
                    ? "reachable open enum has no closed canonical representation"
                    : "materialized type has no representation recipe");
        }
        size_t fields = 0, variants = 0;
        if (kind == SOL_MIR_RECIPE_TUPLE) fields = type->arguments.count;
        else if (kind == SOL_MIR_RECIPE_RECORD) fields = type->fields.count;
        else if (kind == SOL_MIR_RECIPE_ENUM) {
            variants = type->variants.count;
            for (size_t v = 0; v < type->variants.count; ++v) {
                if (!charge(builder, 1)) return fail(builder,
                    SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED,
                    "representation count work limit exceeded");
                if (!add(&fields, source->shape_variants[
                        type->variants.offset + v].fields.count)) return false;
            }
        } else if (kind == SOL_MIR_RECIPE_OPTION) { fields = 1; variants = 2; }
        else if (kind == SOL_MIR_RECIPE_RESULT) { fields = 2; variants = 2; }
        if (!add(field_count, fields) || !add(variant_count, variants)
            || (kind == SOL_MIR_RECIPE_FUNCTION
                && !add(id_count, type->parameters.count))) return false;
    }
    for (size_t i = 0; i < source->semantic_site_count; ++i) {
        if (!charge(builder, 1)) return fail(builder,
            SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED,
            "representation count work limit exceeded");
        SolMirPlanDemandKind kind = source->semantic_sites[i].kind;
        if (kind == SOL_MIR_PLAN_DEMAND_FUNCTION_VALUE
            || kind == SOL_MIR_PLAN_DEMAND_BOUND_OPERATION
            || kind == SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE) {
            if (!add(producer_count, 1)) return false;
            if (kind == SOL_MIR_PLAN_DEMAND_BOUND_OPERATION
                && !add(root_count,
                    source->semantic_sites[i].captured_receiver_roots.count))
                return false;
        }
    }
    return true;
}

static void append_field(SolMirRepresentation *out, SolIrFieldId source,
    size_t ordinal, SolMirRecipeId type) {
    out->fields[out->field_count++] = (SolMirRecipeField){source, ordinal, type};
}

static void append_variant(SolMirRepresentation *out, SolIrVariantId source,
    size_t ordinal, size_t tag, size_t field_offset, size_t field_count) {
    out->variants[out->variant_count++] = (SolMirRecipeVariant){source, ordinal,
        tag, {field_offset, field_count}};
}

static bool populate_recipes(Builder *builder) {
    SolMirRepresentation *out = builder->out;
    const SolMirMaterialization *source = out->materialization;
    for (size_t i = 0; i < source->type_count; ++i) {
        if (!charge(builder, 1)) return fail(builder,
            SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED,
            "representation build work limit exceeded");
        const SolMirMaterializedType *type = &source->types[i];
        SolMirRecipe *recipe = &out->recipes[i];
        recipe->kind = recipe_kind(type);
        recipe->concrete_definition = type->kind == SOL_IR_TYPE_NOMINAL
            ? type->definition : SOL_IR_NONE;
        recipe->result = recipe->backing = recipe->capability_source
            = SOL_MIR_RECIPE_NONE;
        recipe->effects = SOL_MIR_MATERIALIZED_NONE;
        if (recipe->kind == SOL_MIR_RECIPE_TUPLE) {
            recipe->fields = (SolMirPlanSlice){out->field_count,
                type->arguments.count};
            for (size_t f = 0; f < type->arguments.count; ++f) {
                if (!charge(builder, 1)) return fail(builder,
                    SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED,
                    "representation population work limit exceeded");
                append_field(out, SOL_IR_NONE, f,
                    source->type_ids[type->arguments.offset + f]);
            }
        } else if (recipe->kind == SOL_MIR_RECIPE_RECORD) {
            recipe->fields = (SolMirPlanSlice){out->field_count,
                type->fields.count};
            for (size_t f = 0; f < type->fields.count; ++f) {
                if (!charge(builder, 1)) return fail(builder,
                    SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED,
                    "representation population work limit exceeded");
                const SolMirMaterializedShapeField *field
                    = &source->shape_fields[type->fields.offset + f];
                append_field(out, field->source_field, field->ordinal, field->type);
            }
        } else if (recipe->kind == SOL_MIR_RECIPE_ENUM) {
            recipe->variants = (SolMirPlanSlice){out->variant_count,
                type->variants.count};
            for (size_t v = 0; v < type->variants.count; ++v) {
                if (!charge(builder, 1)) return fail(builder,
                    SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED,
                    "representation population work limit exceeded");
                const SolMirMaterializedShapeVariant *variant
                    = &source->shape_variants[type->variants.offset + v];
                size_t field_at = out->field_count;
                for (size_t f = 0; f < variant->fields.count; ++f) {
                    if (!charge(builder, 1)) return fail(builder,
                        SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED,
                        "representation population work limit exceeded");
                    const SolMirMaterializedShapeField *field
                        = &source->shape_fields[variant->fields.offset + f];
                    append_field(out, field->source_field, field->ordinal,
                        field->type);
                }
                append_variant(out, variant->source_variant, variant->ordinal,
                    variant->ordinal, field_at, variant->fields.count);
            }
        } else if (recipe->kind == SOL_MIR_RECIPE_OPTION
            || recipe->kind == SOL_MIR_RECIPE_RESULT) {
            size_t records = recipe->kind == SOL_MIR_RECIPE_OPTION ? 3 : 4;
            if (!charge(builder, records)) return fail(builder,
                SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED,
                "representation population work limit exceeded");
            recipe->variants = (SolMirPlanSlice){out->variant_count, 2};
            if (recipe->kind == SOL_MIR_RECIPE_OPTION) {
                append_variant(out, SOL_IR_NONE, 0, 0, out->field_count, 0);
                size_t at = out->field_count;
                append_field(out, SOL_IR_NONE, 0,
                    source->type_ids[type->arguments.offset]);
                append_variant(out, SOL_IR_NONE, 1, 1, at, 1);
            } else {
                size_t at = out->field_count;
                append_field(out, SOL_IR_NONE, 0,
                    source->type_ids[type->arguments.offset]);
                append_variant(out, SOL_IR_NONE, 0, 0, at, 1);
                at = out->field_count;
                append_field(out, SOL_IR_NONE, 0,
                    source->type_ids[type->arguments.offset + 1]);
                append_variant(out, SOL_IR_NONE, 1, 1, at, 1);
            }
        } else if (recipe->kind == SOL_MIR_RECIPE_DISTINCT
            || recipe->kind == SOL_MIR_RECIPE_REFINED) {
            recipe->backing = type->backing;
        } else if (recipe->kind == SOL_MIR_RECIPE_FUNCTION) {
            recipe->parameters = (SolMirPlanSlice){out->recipe_id_count,
                type->parameters.count};
            recipe->parameter_accesses = (SolMirPlanSlice){out->access_count,
                type->parameter_accesses.count};
            for (size_t p = 0; p < type->parameters.count; ++p) {
                if (!charge(builder, 1)) return fail(builder,
                    SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED,
                    "representation population work limit exceeded");
                out->recipe_ids[out->recipe_id_count++] = source->type_ids[
                    type->parameters.offset + p];
                out->accesses[out->access_count++] = source->accesses[
                    type->parameter_accesses.offset + p];
            }
            recipe->result = type->result; recipe->effects = type->effects;
        } else if (recipe->kind == SOL_MIR_RECIPE_CAPABILITY) {
            recipe->capability_source = type->capability_source;
        }
    }
    return true;
}

static bool all_fields(Builder *builder, SolMirPlanSlice fields,
    const unsigned char *facts, bool *complete) {
    const SolMirRepresentation *out = builder->out;
    for (size_t i = 0; i < fields.count; ++i) {
        if (!charge(builder, 1)) { *complete = false; return false; }
        if (!facts[out->fields[fields.offset + i].type]) return false;
    }
    return true;
}

static bool any_inhabited_variant(Builder *builder, SolMirPlanSlice variants,
    const unsigned char *facts, bool *complete) {
    const SolMirRepresentation *out = builder->out;
    for (size_t i = 0; i < variants.count; ++i) {
        if (!charge(builder, 1)) { *complete = false; return false; }
        if (all_fields(builder, out->variants[variants.offset + i].fields,
                facts, complete))
            return true;
        if (!*complete) return false;
    }
    return false;
}

static SolMirStorageKind storage_for(Builder *builder, SolMirRecipeId id,
    size_t depth, bool *complete) {
    const SolMirRepresentation *out = builder->out;
    if (!charge(builder, 1)) { *complete = false; return SOL_MIR_STORAGE_NONE; }
    if (id >= out->recipe_count || depth > out->recipe_count) return SOL_MIR_STORAGE_NONE;
    const SolMirRecipe *recipe = &out->recipes[id];
    if (!recipe->inhabited || recipe->zero_sized) return SOL_MIR_STORAGE_NONE;
    if (recipe->kind == SOL_MIR_RECIPE_INT64 || recipe->kind == SOL_MIR_RECIPE_BOOL)
        return SOL_MIR_STORAGE_SCALAR;
    if (recipe->kind == SOL_MIR_RECIPE_TEXT) return SOL_MIR_STORAGE_TEXT_HANDLE;
    if (recipe->kind == SOL_MIR_RECIPE_FUNCTION) return SOL_MIR_STORAGE_CALLABLE_HANDLE;
    if (recipe->kind == SOL_MIR_RECIPE_CAPABILITY) return SOL_MIR_STORAGE_CAPABILITY_HANDLE;
    if (recipe->kind == SOL_MIR_RECIPE_DISTINCT
        || recipe->kind == SOL_MIR_RECIPE_REFINED)
        return storage_for(builder, recipe->backing, depth + 1, complete);
    return SOL_MIR_STORAGE_AGGREGATE_VALUE;
}

static bool classify(Builder *builder) {
    SolMirRepresentation *out = builder->out;
    size_t count = out->recipe_count;
    unsigned char *inhabited = count == 0 ? NULL : calloc(count, 1);
    unsigned char *zero = count == 0 ? NULL : calloc(count, 1);
    unsigned char *copy = count == 0 ? NULL : calloc(count, 1);
    if (count != 0 && (inhabited == NULL || zero == NULL || copy == NULL)) {
        free(inhabited); free(zero); free(copy);
        return fail(builder, SOL_MIR_REPRESENTATION_BUILD_ALLOCATION_FAILED,
            "representation fixed-point allocation failed");
    }
    for (size_t i = 0; i < count; ++i) {
        if (!charge(builder, 1)) goto work_failed;
        SolMirRecipeKind kind = out->recipes[i].kind;
        copy[i] = kind != SOL_MIR_RECIPE_FUNCTION
            && kind != SOL_MIR_RECIPE_CAPABILITY;
    }
    bool changed = true;
    size_t passes = 0;
    while (changed && passes++ <= count) {
        changed = false;
        for (size_t i = 0; i < count; ++i) {
            if (!charge(builder, 1)) goto work_failed;
            SolMirRecipe *recipe = &out->recipes[i];
            bool value = false;
            bool complete = true;
            switch (recipe->kind) {
                case SOL_MIR_RECIPE_INT64: case SOL_MIR_RECIPE_BOOL:
                case SOL_MIR_RECIPE_TEXT: case SOL_MIR_RECIPE_UNIT:
                case SOL_MIR_RECIPE_FUNCTION: case SOL_MIR_RECIPE_CAPABILITY:
                case SOL_MIR_RECIPE_OPTION: value = true; break;
                case SOL_MIR_RECIPE_NEVER: value = false; break;
                case SOL_MIR_RECIPE_TUPLE: case SOL_MIR_RECIPE_RECORD:
                    value = all_fields(builder, recipe->fields, inhabited,
                        &complete); break;
                case SOL_MIR_RECIPE_ENUM: case SOL_MIR_RECIPE_RESULT:
                    value = any_inhabited_variant(builder, recipe->variants,
                        inhabited, &complete); break;
                case SOL_MIR_RECIPE_DISTINCT: case SOL_MIR_RECIPE_REFINED:
                    value = inhabited[recipe->backing] != 0; break;
            }
            if (!complete) goto work_failed;
            if (value && !inhabited[i]) { inhabited[i] = 1; changed = true; }
        }
    }
    if (changed) goto internal_failed;
    changed = true; passes = 0;
    while (changed && passes++ <= count) {
        changed = false;
        for (size_t i = 0; i < count; ++i) {
            if (!charge(builder, 1)) goto work_failed;
            SolMirRecipe *recipe = &out->recipes[i];
            bool value = false;
            bool complete = true;
            if (inhabited[i]) {
                if (recipe->kind == SOL_MIR_RECIPE_UNIT) value = true;
                else if (recipe->kind == SOL_MIR_RECIPE_TUPLE
                    || recipe->kind == SOL_MIR_RECIPE_RECORD) {
                    value = all_fields(builder, recipe->fields, zero, &complete);
                } else if (recipe->kind == SOL_MIR_RECIPE_DISTINCT
                    || recipe->kind == SOL_MIR_RECIPE_REFINED) {
                    value = zero[recipe->backing] != 0;
                }
            }
            if (!complete) goto work_failed;
            if (value && !zero[i]) { zero[i] = 1; changed = true; }
        }
    }
    if (changed) goto internal_failed;
    changed = true; passes = 0;
    while (changed && passes++ <= count) {
        changed = false;
        for (size_t i = 0; i < count; ++i) {
            if (!charge(builder, 1)) goto work_failed;
            if (!copy[i]) continue;
            SolMirRecipe *recipe = &out->recipes[i];
            bool value = true;
            bool complete = true;
            if (recipe->kind == SOL_MIR_RECIPE_TUPLE
                || recipe->kind == SOL_MIR_RECIPE_RECORD) {
                value = all_fields(builder, recipe->fields, copy, &complete);
            } else if (recipe->kind == SOL_MIR_RECIPE_ENUM
                || recipe->kind == SOL_MIR_RECIPE_OPTION
                || recipe->kind == SOL_MIR_RECIPE_RESULT) {
                for (size_t v = 0; value && v < recipe->variants.count; ++v) {
                    if (!charge(builder, 1)) { complete = false; break; }
                    value = all_fields(builder,
                        out->variants[recipe->variants.offset + v].fields,
                        copy, &complete);
                }
            } else if (recipe->kind == SOL_MIR_RECIPE_DISTINCT
                || recipe->kind == SOL_MIR_RECIPE_REFINED) {
                value = copy[recipe->backing] != 0;
            }
            if (!complete) goto work_failed;
            if (!value) { copy[i] = 0; changed = true; }
        }
    }
    if (changed) goto internal_failed;
    for (size_t i = 0; i < count; ++i) {
        if (!charge(builder, 1)) goto work_failed;
        out->recipes[i].inhabited = inhabited[i] != 0;
        out->recipes[i].zero_sized = zero[i] != 0;
        out->recipes[i].is_copy = copy[i] != 0;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!charge(builder, 1)) goto work_failed;
        SolMirRecipe *recipe = &out->recipes[i];
        if (recipe->is_copy != out->materialization->types[i].is_copy)
            goto internal_failed;
        bool complete = true;
        recipe->storage = storage_for(builder, i, 0, &complete);
        if (!complete) goto work_failed;
        if (!recipe->inhabited) recipe->copy_kind = SOL_MIR_COPY_UNREACHABLE;
        else if (!recipe->is_copy) recipe->copy_kind = SOL_MIR_COPY_FORBIDDEN;
        else if (recipe->kind == SOL_MIR_RECIPE_TEXT) recipe->copy_kind = SOL_MIR_COPY_TEXT;
        else if (recipe->kind == SOL_MIR_RECIPE_DISTINCT
            || recipe->kind == SOL_MIR_RECIPE_REFINED) recipe->copy_kind = SOL_MIR_COPY_WRAPPER;
        else if (recipe->kind == SOL_MIR_RECIPE_TUPLE
            || recipe->kind == SOL_MIR_RECIPE_RECORD
            || recipe->kind == SOL_MIR_RECIPE_ENUM
            || recipe->kind == SOL_MIR_RECIPE_OPTION
            || recipe->kind == SOL_MIR_RECIPE_RESULT) recipe->copy_kind = SOL_MIR_COPY_AGGREGATE;
        else recipe->copy_kind = SOL_MIR_COPY_TRIVIAL;
        if (!recipe->inhabited || recipe->kind == SOL_MIR_RECIPE_INT64
            || recipe->kind == SOL_MIR_RECIPE_BOOL
            || recipe->kind == SOL_MIR_RECIPE_UNIT
            || recipe->kind == SOL_MIR_RECIPE_NEVER) recipe->drop_kind = SOL_MIR_DROP_NONE;
        else if (recipe->kind == SOL_MIR_RECIPE_TEXT) recipe->drop_kind = SOL_MIR_DROP_TEXT;
        else if (recipe->kind == SOL_MIR_RECIPE_FUNCTION) recipe->drop_kind = SOL_MIR_DROP_CALLABLE;
        else if (recipe->kind == SOL_MIR_RECIPE_CAPABILITY) recipe->drop_kind = SOL_MIR_DROP_CAPABILITY;
        else if (recipe->kind == SOL_MIR_RECIPE_DISTINCT
            || recipe->kind == SOL_MIR_RECIPE_REFINED) recipe->drop_kind = SOL_MIR_DROP_WRAPPER;
        else recipe->drop_kind = SOL_MIR_DROP_AGGREGATE;
    }
    free(inhabited); free(zero); free(copy); return true;
work_failed:
    free(inhabited); free(zero); free(copy);
    return fail(builder, SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED,
        "representation fixed-point work limit exceeded");
internal_failed:
    free(inhabited); free(zero); free(copy);
    return fail(builder, SOL_MIR_REPRESENTATION_BUILD_INTERNAL_FAILED,
        "representation fixed point did not converge or disagreed with Copy");
}

static bool populate_producers(Builder *builder) {
    SolMirRepresentation *out = builder->out;
    const SolMirMaterialization *source = out->materialization;
    for (size_t i = 0; i < source->semantic_site_count; ++i) {
        if (!charge(builder, 1)) return fail(builder,
            SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED,
            "representation producer work limit exceeded");
        const SolMirMaterializedSemanticSite *site = &source->semantic_sites[i];
        if (site->kind != SOL_MIR_PLAN_DEMAND_FUNCTION_VALUE
            && site->kind != SOL_MIR_PLAN_DEMAND_BOUND_OPERATION
            && site->kind != SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE) continue;
        const SolMirMaterializedBinding *binding = &source->bindings[site->binding];
        SolMirCallableProducer *producer
            = &out->callable_producers[out->callable_producer_count++];
        *producer = (SolMirCallableProducer){
            .kind = site->kind == SOL_MIR_PLAN_DEMAND_BOUND_OPERATION
                ? SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION
                : SOL_MIR_CALLABLE_PRODUCER_EXACT_FUNCTION,
            .function_recipe = site->produced_function_type,
            .semantic_site = i,
            .binding = site->binding,
            .target_kind = binding->target_kind,
            .instance = binding->instance,
            .import = binding->import,
            .captured_receiver_type = site->captured_receiver_type,
            .captured_receiver_kind = site->captured_receiver_kind,
            .captured_receiver_expression = site->captured_receiver_expression,
            .captured_receiver_place = site->captured_receiver_place,
            .captured_receiver_temporary = site->captured_receiver_temporary,
            .captured_receiver_value = site->captured_receiver_value,
            .captured_receiver_instruction = site->captured_receiver_instruction,
            .captured_receiver_roots = site->kind
                == SOL_MIR_PLAN_DEMAND_BOUND_OPERATION
                    ? (SolMirPlanSlice){out->receiver_root_count,
                        site->captured_receiver_roots.count}
                    : (SolMirPlanSlice){0},
            .effects = site->kind == SOL_MIR_PLAN_DEMAND_BOUND_OPERATION
                ? site->operation.effects
                : out->recipes[site->produced_function_type].effects,
        };
        for (size_t r = 0; r < site->captured_receiver_roots.count; ++r) {
            if (!charge(builder, 1)) return fail(builder,
                SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED,
                "representation producer work limit exceeded");
            out->receiver_roots[out->receiver_root_count++]
                = source->receiver_roots[site->captured_receiver_roots.offset + r];
        }
    }
    return true;
}

static bool validation_requirements(const SolMirRepresentation *out,
    size_t *work, size_t *scratch) {
    const SolMirMaterialization *m = out->materialization;
    const SolMirPlan *plan = m->plan;
    const SolMirProgram *program = plan->program;
    const SolIr *ir = program->ir;
    size_t owned = out->recipe_count;
    if (!add(&owned, out->field_count) || !add(&owned, out->variant_count)
        || !add(&owned, out->recipe_id_count) || !add(&owned, out->access_count)
        || !add(&owned, out->callable_producer_count)
        || !add(&owned, out->receiver_root_count)) return false;
    size_t total = m->usage.validation_work;
    size_t borrowed = m->usage.concrete_records;
    if (!add(&borrowed, plan->type_component_count)
        || !add(&borrowed, plan->typed_use_count)
        || !add(&borrowed, plan->demand_count)
        || !add(&borrowed, program->reference_count)
        || !add(&borrowed, program->template_count)
        || !add(&borrowed, ir->type_count)
        || !add(&borrowed, ir->definition_count)
        || !add(&borrowed, ir->callable_count)
        || !add(&borrowed, ir->expression_count)
        || !add(&borrowed, ir->statement_count)
        || !add(&borrowed, ir->source_length)
        || borrowed > SIZE_MAX / 8) return false;
    if (!add(&total, out->usage.build_work)
        || owned > SIZE_MAX / 4 || !add(&total, owned * 4)
        || !add(&total, m->semantic_site_count)
        || !add(&total, borrowed * 8)) return false;
    size_t local_scratch = out->field_count;
    if (!add(&local_scratch, out->variant_count)
        || !add(&local_scratch, m->semantic_site_count)
        || !add(&local_scratch, out->receiver_root_count)) return false;
    size_t temporary = local_scratch > out->usage.build_scratch_bytes
        ? local_scratch : out->usage.build_scratch_bytes;
    size_t rebuild_scratch = out->usage.owned_bytes;
    if (!add(&rebuild_scratch, temporary)) return false;
    size_t materialization_scratch;
    if (!multiply(m->usage.validation_work, sizeof(size_t),
            &materialization_scratch)
        || !add(&materialization_scratch, m->usage.owned_bytes)) return false;
    *work = total;
    *scratch = rebuild_scratch > materialization_scratch
        ? rebuild_scratch : materialization_scratch;
    return true;
}

static bool build_scratch(Builder *builder) {
    SolMirRepresentation *out = builder->out;
    size_t fields, variants, ids, producers, roots;
    if (!count_graph(builder, &fields, &variants, &ids, &producers, &roots)) {
        if (builder->outcome == SOL_MIR_REPRESENTATION_BUILD_INTERNAL_FAILED)
            return fail(builder, SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED,
                "representation graph count overflowed");
        return false;
    }
    if (out->materialization->type_count > out->limits.max_recipes
        || fields > out->limits.max_fields || variants > out->limits.max_variants
        || ids > out->limits.max_recipe_ids
        || producers > out->limits.max_callable_producers
        || roots > out->limits.max_receiver_roots) return fail(builder,
            SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED,
            "representation graph record limit exceeded");
#define ALLOC(field, count, capacity) do { \
    out->field = allocate(builder, (count), sizeof(*out->field)); \
    out->capacity = (count); \
    if ((count) != 0 && out->field == NULL) return false; \
} while (0)
    ALLOC(recipes, out->materialization->type_count, recipe_capacity);
    ALLOC(fields, fields, field_capacity); ALLOC(variants, variants, variant_capacity);
    ALLOC(recipe_ids, ids, recipe_id_capacity); ALLOC(accesses, ids, access_capacity);
    ALLOC(receiver_roots, roots, receiver_root_capacity);
    ALLOC(callable_producers, producers, callable_producer_capacity);
#undef ALLOC
    out->recipe_count = out->recipe_capacity;
    if (!multiply(out->recipe_count, 3, &out->usage.build_scratch_bytes)
        || out->usage.build_scratch_bytes > out->limits.max_build_scratch_bytes)
        return fail(builder, SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED,
            "representation build scratch-byte limit exceeded");
    if (!populate_recipes(builder) || !classify(builder)
        || !populate_producers(builder)) return false;
    out->usage.recipes = out->recipe_count; out->usage.fields = out->field_count;
    out->usage.variants = out->variant_count;
    out->usage.recipe_ids = out->recipe_id_count;
    out->usage.callable_producers = out->callable_producer_count;
    out->usage.receiver_roots = out->receiver_root_count;
    if (out->field_count != out->field_capacity
        || out->variant_count != out->variant_capacity
        || out->recipe_id_count != out->recipe_id_capacity
        || out->access_count != out->access_capacity
        || out->receiver_root_count != out->receiver_root_capacity
        || out->callable_producer_count != out->callable_producer_capacity
        || !validation_requirements(out, &out->usage.validation_work,
            &out->usage.validation_scratch_bytes)
        || out->usage.validation_work > out->limits.max_validation_work
        || out->usage.validation_scratch_bytes
            > out->limits.max_validation_scratch_bytes) {
        return fail(builder, SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED,
            "representation validation work limit exceeded");
    }
    return true;
}

SolMirRepresentationBuildOutcome sol_mir_representation_build(
    const SolMirRepresentationBuildRequest *request, SolMirRepresentation *output,
    SolDiagnostics *diagnostics) {
    if (request == NULL || output == NULL || diagnostics == NULL
        || !owner_empty(output) || request->materialization == NULL
        || (request->limits != NULL && !limits_zero(*request->limits)
            && !limits_complete(*request->limits))) {
        error(diagnostics, "invalid representation build request or destination");
        return SOL_MIR_REPRESENTATION_BUILD_INVALID_ARGUMENT;
    }
    if (!sol_mir_materialization_validate(request->materialization, diagnostics))
        return SOL_MIR_REPRESENTATION_BUILD_INVALID_MATERIALIZATION;
    SolMirRepresentation scratch;
    sol_mir_representation_init(&scratch);
    scratch.materialization = request->materialization;
    scratch.limits = request->limits == NULL || limits_zero(*request->limits)
        ? sol_mir_representation_default_limits() : *request->limits;
    Builder builder = {&scratch, diagnostics,
        SOL_MIR_REPRESENTATION_BUILD_INTERNAL_FAILED};
    if (build_scratch(&builder)) {
        builder.outcome = SOL_MIR_REPRESENTATION_BUILD_SUCCEEDED;
        if (sol_mir_representation_validate(&scratch, diagnostics)) {
            *output = scratch; return builder.outcome;
        }
        builder.outcome = diagnostics->allocation_failed
            ? SOL_MIR_REPRESENTATION_BUILD_ALLOCATION_FAILED
            : SOL_MIR_REPRESENTATION_BUILD_INTERNAL_FAILED;
    }
    sol_mir_representation_free(&scratch);
    return builder.outcome;
}

static bool slice(SolMirPlanSlice value, size_t count) {
    return value.offset <= count && value.count <= count - value.offset;
}

static bool canonical(size_t count, size_t capacity, const void *pointer) {
    return count == capacity && ((count == 0) == (pointer == NULL));
}

static bool add_range(Range *ranges, size_t *count, const void *pointer,
    size_t items, size_t size) {
    if (items == 0) return pointer == NULL;
    if (pointer == NULL || items > SIZE_MAX / size) return false;
    uintptr_t start = (uintptr_t)pointer;
    size_t bytes = items * size;
    if (bytes > UINTPTR_MAX - start) return false;
    Range next = {start, start + bytes};
    for (size_t i = 0; i < *count; ++i)
        if (next.start < ranges[i].end && ranges[i].start < next.end) return false;
    ranges[(*count)++] = next; return true;
}

static bool overlaps(const Range *owned, size_t owned_count, const void *pointer,
    size_t items, size_t size) {
    if (items == 0) return pointer != NULL;
    if (pointer == NULL || items > SIZE_MAX / size) return true;
    uintptr_t start = (uintptr_t)pointer;
    size_t bytes = items * size;
    if (bytes > UINTPTR_MAX - start) return true;
    uintptr_t end = start + bytes;
    for (size_t i = 0; i < owned_count; ++i)
        if (start < owned[i].end && owned[i].start < end) return true;
    return false;
}

static bool shallow_graph(const SolMirRepresentation *out) {
    for (size_t i = 0; i < out->recipe_count; ++i) {
        const SolMirRecipe *recipe = &out->recipes[i];
        if ((int)recipe->kind < 0 || recipe->kind > SOL_MIR_RECIPE_CAPABILITY
            || (int)recipe->storage < 0
            || recipe->storage > SOL_MIR_STORAGE_CAPABILITY_HANDLE
            || (int)recipe->copy_kind < 0
            || recipe->copy_kind > SOL_MIR_COPY_UNREACHABLE
            || (int)recipe->drop_kind < 0
            || recipe->drop_kind > SOL_MIR_DROP_WRAPPER
            || !slice(recipe->fields, out->field_count)
            || !slice(recipe->variants, out->variant_count)
            || !slice(recipe->parameters, out->recipe_id_count)
            || !slice(recipe->parameter_accesses, out->access_count)
            || recipe->parameters.count != recipe->parameter_accesses.count
            || (recipe->result != SOL_MIR_RECIPE_NONE
                && recipe->result >= out->recipe_count)
            || (recipe->backing != SOL_MIR_RECIPE_NONE
                && recipe->backing >= out->recipe_count)
            || (recipe->capability_source != SOL_MIR_RECIPE_NONE
                && recipe->capability_source >= out->recipe_count)
            || (recipe->effects != SOL_MIR_MATERIALIZED_NONE
                && recipe->effects
                    >= out->materialization->effect_row_count)) return false;
    }
    for (size_t i = 0; i < out->field_count; ++i)
        if (out->fields[i].type >= out->recipe_count) return false;
    for (size_t i = 0; i < out->variant_count; ++i)
        if (!slice(out->variants[i].fields, out->field_count)) return false;
    for (size_t i = 0; i < out->recipe_id_count; ++i)
        if (out->recipe_ids[i] >= out->recipe_count) return false;
    for (size_t i = 0; i < out->access_count; ++i)
        if ((int)out->accesses[i] < (int)SOL_ACCESS_OWNED
            || out->accesses[i] > SOL_ACCESS_EXCLUSIVE) return false;
    for (size_t i = 0; i < out->receiver_root_count; ++i)
        if (out->receiver_roots[i] >= out->materialization->local_count) return false;
    for (size_t i = 0; i < out->callable_producer_count; ++i) {
        const SolMirCallableProducer *producer = &out->callable_producers[i];
        if ((int)producer->kind < 0
            || producer->kind > SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION
            || producer->function_recipe >= out->recipe_count
            || producer->semantic_site
                >= out->materialization->semantic_site_count
            || producer->binding >= out->materialization->binding_count
            || (int)producer->target_kind
                < (int)SOL_MIR_MATERIALIZED_TARGET_INSTANCE
            || producer->target_kind > SOL_MIR_MATERIALIZED_TARGET_IMPORT
            || (producer->captured_receiver_type != SOL_MIR_RECIPE_NONE
                && producer->captured_receiver_type >= out->recipe_count)
            || (int)producer->captured_receiver_kind
                < (int)SOL_MIR_MATERIALIZED_RECEIVER_NONE
            || producer->captured_receiver_kind
                > SOL_MIR_MATERIALIZED_RECEIVER_VALUE
            || (producer->captured_receiver_expression != SOL_IR_NONE
                && producer->captured_receiver_expression
                    >= out->materialization->plan->program->ir->expression_count)
            || (producer->captured_receiver_place != SOL_MIR_MATERIALIZED_NONE
                && producer->captured_receiver_place
                    >= out->materialization->place_count)
            || (producer->captured_receiver_temporary
                    != SOL_MIR_MATERIALIZED_NONE
                && producer->captured_receiver_temporary
                    >= out->materialization->temporary_count)
            || (producer->captured_receiver_value != SOL_MIR_MATERIALIZED_NONE
                && producer->captured_receiver_value
                    >= out->materialization->value_count)
            || (producer->captured_receiver_instruction
                    != SOL_MIR_MATERIALIZED_NONE
                && producer->captured_receiver_instruction
                    >= out->materialization->instruction_count)
            || !slice(producer->captured_receiver_roots,
                out->receiver_root_count)
            || producer->effects >= out->materialization->effect_row_count)
            return false;
    }
    return true;
}

static bool local_graph(const SolMirRepresentation *out) {
    unsigned char *fields = out->field_count == 0 ? NULL : calloc(out->field_count, 1);
    unsigned char *variants = out->variant_count == 0 ? NULL : calloc(out->variant_count, 1);
    unsigned char *sites = out->materialization->semantic_site_count == 0 ? NULL
        : calloc(out->materialization->semantic_site_count, 1);
    unsigned char *roots = out->receiver_root_count == 0 ? NULL
        : calloc(out->receiver_root_count, 1);
    bool valid = (out->field_count == 0 || fields != NULL)
        && (out->variant_count == 0 || variants != NULL)
        && (out->materialization->semantic_site_count == 0 || sites != NULL)
        && (out->receiver_root_count == 0 || roots != NULL);
    for (size_t i = 0; valid && i < out->recipe_count; ++i) {
        const SolMirRecipe *recipe = &out->recipes[i];
        if ((int)recipe->kind < 0 || recipe->kind > SOL_MIR_RECIPE_CAPABILITY
            || (int)recipe->storage < 0
            || recipe->storage > SOL_MIR_STORAGE_CAPABILITY_HANDLE
            || (int)recipe->copy_kind < 0
            || recipe->copy_kind > SOL_MIR_COPY_UNREACHABLE
            || (int)recipe->drop_kind < 0
            || recipe->drop_kind > SOL_MIR_DROP_WRAPPER
            || !slice(recipe->fields, out->field_count)
            || !slice(recipe->variants, out->variant_count)
            || !slice(recipe->parameters, out->recipe_id_count)
            || !slice(recipe->parameter_accesses, out->access_count)
            || recipe->parameters.count != recipe->parameter_accesses.count) {
            valid = false; break;
        }
        for (size_t f = 0; f < recipe->fields.count; ++f) {
            size_t slot = recipe->fields.offset + f;
            if (fields[slot] || out->fields[slot].ordinal != f
                || out->fields[slot].type >= out->recipe_count) { valid = false; break; }
            fields[slot] = 1;
        }
        for (size_t v = 0; valid && v < recipe->variants.count; ++v) {
            size_t slot = recipe->variants.offset + v;
            const SolMirRecipeVariant *variant = &out->variants[slot];
            if (variants[slot] || variant->ordinal != v || variant->semantic_tag != v
                || !slice(variant->fields, out->field_count)) { valid = false; break; }
            variants[slot] = 1;
            for (size_t f = 0; f < variant->fields.count; ++f) {
                size_t field = variant->fields.offset + f;
                if (fields[field] || out->fields[field].ordinal != f
                    || out->fields[field].type >= out->recipe_count) {
                    valid = false; break;
                }
                fields[field] = 1;
            }
        }
        for (size_t p = 0; valid && p < recipe->parameters.count; ++p)
            valid = out->recipe_ids[recipe->parameters.offset + p]
                    < out->recipe_count
                && (int)out->accesses[recipe->parameter_accesses.offset + p]
                    >= (int)SOL_ACCESS_OWNED
                && out->accesses[recipe->parameter_accesses.offset + p]
                    <= SOL_ACCESS_EXCLUSIVE;
    }
    for (size_t i = 0; valid && i < out->field_count; ++i) valid = fields[i] == 1;
    for (size_t i = 0; valid && i < out->variant_count; ++i) valid = variants[i] == 1;
    for (size_t i = 0; valid && i < out->callable_producer_count; ++i) {
        const SolMirCallableProducer *producer = &out->callable_producers[i];
        if ((int)producer->kind < 0
            || producer->kind > SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION
            || producer->semantic_site >= out->materialization->semantic_site_count
            || sites[producer->semantic_site] || producer->binding >= out->materialization->binding_count
            || producer->function_recipe >= out->recipe_count
            || out->recipes[producer->function_recipe].kind != SOL_MIR_RECIPE_FUNCTION
            || !slice(producer->captured_receiver_roots,
                out->receiver_root_count)) {
            valid = false; break;
        }
        sites[producer->semantic_site] = 1;
        const SolMirMaterializedSemanticSite *site
            = &out->materialization->semantic_sites[producer->semantic_site];
        const SolMirMaterializedBinding *binding
            = &out->materialization->bindings[producer->binding];
        valid = site->binding == producer->binding
            && site->produced_function_type == producer->function_recipe
            && binding->target_kind == producer->target_kind
            && binding->instance == producer->instance && binding->import == producer->import
            && site->captured_receiver_type == producer->captured_receiver_type
            && site->captured_receiver_kind == producer->captured_receiver_kind
            && site->captured_receiver_expression
                == producer->captured_receiver_expression
            && site->captured_receiver_place == producer->captured_receiver_place
            && site->captured_receiver_temporary
                == producer->captured_receiver_temporary
            && site->captured_receiver_value == producer->captured_receiver_value
            && site->captured_receiver_instruction
                == producer->captured_receiver_instruction
            && site->captured_receiver_roots.count
                == producer->captured_receiver_roots.count;
        for (size_t r = 0; valid && r < producer->captured_receiver_roots.count;
            ++r) {
            size_t slot = producer->captured_receiver_roots.offset + r;
            valid = !roots[slot]
                && out->receiver_roots[slot] < out->materialization->local_count
                && out->receiver_roots[slot] == out->materialization->receiver_roots[
                    site->captured_receiver_roots.offset + r];
            if (valid) roots[slot] = 1;
        }
        if (valid && producer->kind == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION) {
            valid = site->kind == SOL_MIR_PLAN_DEMAND_BOUND_OPERATION
                && producer->captured_receiver_kind
                    != SOL_MIR_MATERIALIZED_RECEIVER_NONE
                && producer->captured_receiver_roots.count != 0
                && producer->effects == site->operation.effects;
        } else if (valid) {
            valid = producer->kind == SOL_MIR_CALLABLE_PRODUCER_EXACT_FUNCTION
                && producer->captured_receiver_kind
                    == SOL_MIR_MATERIALIZED_RECEIVER_NONE
                && producer->captured_receiver_type == SOL_MIR_RECIPE_NONE
                && producer->captured_receiver_expression == SOL_IR_NONE
                && producer->captured_receiver_place == SOL_MIR_MATERIALIZED_NONE
                && producer->captured_receiver_temporary
                    == SOL_MIR_MATERIALIZED_NONE
                && producer->captured_receiver_value == SOL_MIR_MATERIALIZED_NONE
                && producer->captured_receiver_instruction
                    == SOL_MIR_MATERIALIZED_NONE
                && producer->captured_receiver_roots.count == 0;
        }
    }
    for (size_t i = 0; valid && i < out->materialization->semantic_site_count; ++i) {
        SolMirPlanDemandKind kind = out->materialization->semantic_sites[i].kind;
        bool expected = kind == SOL_MIR_PLAN_DEMAND_FUNCTION_VALUE
            || kind == SOL_MIR_PLAN_DEMAND_BOUND_OPERATION
            || kind == SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE;
        valid = sites[i] == (unsigned char)expected;
    }
    for (size_t i = 0; valid && i < out->receiver_root_count; ++i)
        valid = roots[i] == 1;
    free(fields); free(variants); free(sites); free(roots); return valid;
}

static bool arrays_equal(const SolMirRepresentation *a,
    const SolMirRepresentation *b) {
#define SAME(field, count) if (a->count != b->count || (a->count != 0 \
    && memcmp(a->field, b->field, a->count * sizeof(*a->field)) != 0)) return false
    SAME(recipes, recipe_count); SAME(fields, field_count);
    SAME(variants, variant_count); SAME(recipe_ids, recipe_id_count);
    SAME(accesses, access_count); SAME(receiver_roots, receiver_root_count);
    SAME(callable_producers, callable_producer_count);
#undef SAME
    return memcmp(&a->usage, &b->usage, sizeof(a->usage)) == 0;
}

static bool allocation_shape_equal(const SolMirRepresentation *a,
    const SolMirRepresentation *b) {
    return a->recipe_count == b->recipe_count
        && a->recipe_capacity == b->recipe_capacity
        && a->field_count == b->field_count
        && a->field_capacity == b->field_capacity
        && a->variant_count == b->variant_count
        && a->variant_capacity == b->variant_capacity
        && a->recipe_id_count == b->recipe_id_count
        && a->recipe_id_capacity == b->recipe_id_capacity
        && a->access_count == b->access_count
        && a->access_capacity == b->access_capacity
        && a->receiver_root_count == b->receiver_root_count
        && a->receiver_root_capacity == b->receiver_root_capacity
        && a->callable_producer_count == b->callable_producer_count
        && a->callable_producer_capacity == b->callable_producer_capacity
        && memcmp(&a->usage, &b->usage, sizeof(a->usage)) == 0;
}

static bool persistent_bytes(const SolMirRepresentation *out, size_t *result) {
    size_t total = 0, bytes;
#define BYTES(count, type) do { \
    if (!multiply((count), sizeof(type), &bytes) || !add(&total, bytes)) return false; \
} while (0)
    BYTES(out->recipe_capacity, *out->recipes);
    BYTES(out->field_capacity, *out->fields);
    BYTES(out->variant_capacity, *out->variants);
    BYTES(out->recipe_id_capacity, *out->recipe_ids);
    BYTES(out->access_capacity, *out->accesses);
    BYTES(out->receiver_root_capacity, *out->receiver_roots);
    BYTES(out->callable_producer_capacity, *out->callable_producers);
#undef BYTES
    *result = total; return true;
}

bool sol_mir_representation_validate(const SolMirRepresentation *out,
    SolDiagnostics *diagnostics) {
    if (out == NULL || out->materialization == NULL || !limits_complete(out->limits)
        || out->usage.recipes != out->recipe_count
        || out->usage.fields != out->field_count
        || out->usage.variants != out->variant_count
        || out->usage.recipe_ids != out->recipe_id_count
        || out->usage.callable_producers != out->callable_producer_count
        || out->usage.receiver_roots != out->receiver_root_count
        || out->usage.recipes > out->limits.max_recipes
        || out->usage.fields > out->limits.max_fields
        || out->usage.variants > out->limits.max_variants
        || out->usage.recipe_ids > out->limits.max_recipe_ids
        || out->usage.callable_producers > out->limits.max_callable_producers
        || out->usage.receiver_roots > out->limits.max_receiver_roots
        || out->usage.owned_bytes > out->limits.max_owned_bytes
        || out->usage.build_scratch_bytes > out->limits.max_build_scratch_bytes
        || out->usage.build_work > out->limits.max_build_work
        || out->usage.validation_work > out->limits.max_validation_work
        || out->usage.validation_scratch_bytes
            > out->limits.max_validation_scratch_bytes)
        return error(diagnostics, "representation header or materialization is invalid");
#define CANON(field, count, capacity) if (!canonical(out->count, out->capacity, out->field)) goto malformed
    CANON(recipes, recipe_count, recipe_capacity); CANON(fields, field_count, field_capacity);
    CANON(variants, variant_count, variant_capacity);
    CANON(recipe_ids, recipe_id_count, recipe_id_capacity);
    CANON(accesses, access_count, access_capacity);
    CANON(receiver_roots, receiver_root_count, receiver_root_capacity);
    CANON(callable_producers, callable_producer_count, callable_producer_capacity);
#undef CANON
    size_t bytes;
    if (!persistent_bytes(out, &bytes) || bytes != out->usage.owned_bytes)
        goto malformed;
    Range owned[7]; size_t owned_count = 0;
#define OWN(field, capacity) if (!add_range(owned, &owned_count, out->field, out->capacity, sizeof(*out->field))) goto malformed
    OWN(recipes, recipe_capacity); OWN(fields, field_capacity);
    OWN(variants, variant_capacity); OWN(recipe_ids, recipe_id_capacity);
    OWN(accesses, access_capacity); OWN(receiver_roots, receiver_root_capacity);
    OWN(callable_producers, callable_producer_capacity);
#undef OWN
    if (!sol_mir_materialization_validate(out->materialization, diagnostics))
        return false;
    size_t work, scratch;
    if (!validation_requirements(out, &work, &scratch)
        || work != out->usage.validation_work
        || scratch != out->usage.validation_scratch_bytes) goto malformed;
    if (out->recipe_count != out->materialization->type_count) goto malformed;
    const SolMirMaterialization *m = out->materialization;
#define BORROW(field, capacity) if (overlaps(owned, owned_count, m->field, m->capacity, sizeof(*m->field))) goto malformed
    BORROW(images, image_capacity); BORROW(types, type_capacity);
    BORROW(shape_fields, shape_field_capacity); BORROW(shape_variants, shape_variant_capacity);
    BORROW(type_ids, type_id_capacity); BORROW(accesses, access_capacity);
    BORROW(overlays, overlay_capacity); BORROW(contexts, context_capacity);
    BORROW(locals, local_capacity); BORROW(places, place_capacity);
    BORROW(projections, projection_capacity); BORROW(values, value_capacity);
    BORROW(instructions, instruction_capacity); BORROW(temporaries, temporary_capacity);
    BORROW(construct_operands, construct_operand_capacity); BORROW(call_arguments, call_argument_capacity);
    BORROW(blocks, block_capacity); BORROW(edges, edge_capacity);
    BORROW(edge_values, edge_value_capacity); BORROW(parameter_values, parameter_value_capacity);
    BORROW(loops, loop_capacity); BORROW(bindings, binding_capacity);
    BORROW(semantic_sites, semantic_site_capacity); BORROW(imports, import_capacity);
    BORROW(handlers, handler_capacity); BORROW(writebacks, writeback_capacity);
    BORROW(effect_rows, effect_row_capacity); BORROW(effect_atoms, effect_atom_capacity);
    BORROW(effect_row_atoms, effect_row_atom_capacity); BORROW(effect_names, effect_name_capacity);
    BORROW(literal_bytes, literal_byte_capacity);
    BORROW(receiver_roots, receiver_root_capacity);
#undef BORROW
    const SolMirPlan *plan = m->plan;
#define PLAN_BORROW(field, capacity) if (overlaps(owned, owned_count, plan->field, plan->capacity, sizeof(*plan->field))) goto malformed
    PLAN_BORROW(types, type_capacity); PLAN_BORROW(type_components, type_component_capacity);
    PLAN_BORROW(type_parameter_accesses, type_parameter_access_capacity);
    PLAN_BORROW(effect_atoms, effect_atom_capacity); PLAN_BORROW(effect_rows, effect_row_capacity);
    PLAN_BORROW(effect_row_atoms, effect_row_atom_capacity);
    PLAN_BORROW(instances, instance_capacity); PLAN_BORROW(instance_type_ids, instance_type_id_capacity);
    PLAN_BORROW(instance_accesses, instance_access_capacity);
    PLAN_BORROW(dictionary_entries, dictionary_entry_capacity);
    PLAN_BORROW(imports, import_capacity);
    PLAN_BORROW(typed_uses, typed_use_capacity); PLAN_BORROW(contexts, context_capacity);
    PLAN_BORROW(demands, demand_capacity);
#undef PLAN_BORROW
    const SolMirProgram *program = plan->program;
#define PROGRAM_BORROW(field, count) if (overlaps(owned, owned_count, program->field, program->count, sizeof(*program->field))) goto malformed
    PROGRAM_BORROW(roots, root_count); PROGRAM_BORROW(approved_imports, approved_import_count);
    PROGRAM_BORROW(templates, template_count); PROGRAM_BORROW(imports, import_count);
    PROGRAM_BORROW(specializations, specialization_count); PROGRAM_BORROW(references, reference_count);
#undef PROGRAM_BORROW
#define MIR_BORROW(mir, field, capacity) if (overlaps(owned, owned_count, (mir)->field, (mir)->capacity, sizeof(*(mir)->field))) goto malformed
    for (size_t i = 0; i < program->template_count; ++i) {
        const SolMir *mir = &program->templates[i].mir;
        MIR_BORROW(mir, blocks, block_capacity); MIR_BORROW(mir, instructions, instruction_capacity);
        MIR_BORROW(mir, values, value_capacity); MIR_BORROW(mir, parameter_values, parameter_value_capacity);
        MIR_BORROW(mir, edge_values, edge_value_capacity); MIR_BORROW(mir, call_arguments, call_argument_capacity);
        MIR_BORROW(mir, loops, loop_capacity); MIR_BORROW(mir, construct_operands, construct_operand_capacity);
        MIR_BORROW(mir, temporaries, temporary_capacity);
    }
    for (size_t i = 0; i < m->image_count; ++i) {
        const SolMir *mir = &m->images[i].topology;
        MIR_BORROW(mir, blocks, block_capacity); MIR_BORROW(mir, instructions, instruction_capacity);
        MIR_BORROW(mir, values, value_capacity); MIR_BORROW(mir, parameter_values, parameter_value_capacity);
        MIR_BORROW(mir, edge_values, edge_value_capacity); MIR_BORROW(mir, call_arguments, call_argument_capacity);
        MIR_BORROW(mir, loops, loop_capacity); MIR_BORROW(mir, construct_operands, construct_operand_capacity);
        MIR_BORROW(mir, temporaries, temporary_capacity);
    }
#undef MIR_BORROW
    const SolIr *ir = program->ir;
#define IR_BORROW(field, count) if (overlaps(owned, owned_count, ir->field, ir->count, sizeof(*ir->field))) goto malformed
    IR_BORROW(types, type_count); IR_BORROW(type_ids, type_id_count);
    IR_BORROW(accesses, access_count); IR_BORROW(definitions, definition_count);
    IR_BORROW(callables, callable_count); IR_BORROW(members, member_count);
    IR_BORROW(evidence, evidence_count); IR_BORROW(locals, local_count);
    IR_BORROW(fields, field_count); IR_BORROW(variants, variant_count);
    IR_BORROW(expressions, expression_count); IR_BORROW(places, place_count);
    IR_BORROW(projections, projection_count); IR_BORROW(statements, statement_count);
    IR_BORROW(statement_ids, statement_id_count); IR_BORROW(arms, arm_count);
    IR_BORROW(arm_ids, arm_id_count); IR_BORROW(patterns, pattern_count);
    IR_BORROW(pattern_children, pattern_child_count); IR_BORROW(operands, operand_count);
    IR_BORROW(roots, root_count); IR_BORROW(cleanup_locals, cleanup_local_count);
    IR_BORROW(effects, effect_count); IR_BORROW(generic_parameters, generic_parameter_count);
    IR_BORROW(effect_parameters, effect_parameter_count); IR_BORROW(obligations, obligation_count);
    IR_BORROW(snapshots, snapshot_count); IR_BORROW(loop_obligations, loop_obligation_count);
    IR_BORROW(unreachable_obligations, unreachable_obligation_count); IR_BORROW(files, file_count);
#undef IR_BORROW
#define TEXT_BORROW(pointer, count) if (overlaps(owned, owned_count, pointer, count, sizeof(char))) goto malformed
    TEXT_BORROW(ir->source_path, strlen(ir->source_path) + 1);
    TEXT_BORROW(ir->source_bytes, ir->source_length + 1);
    for (size_t i = 0; i < plan->effect_atom_count; ++i)
        TEXT_BORROW(plan->effect_atoms[i].name, plan->effect_atoms[i].length + 1);
    for (size_t i = 0; i < ir->definition_count; ++i)
        if (ir->definitions[i].name != NULL)
            TEXT_BORROW(ir->definitions[i].name, strlen(ir->definitions[i].name) + 1);
    for (size_t i = 0; i < ir->callable_count; ++i)
        if (ir->callables[i].name != NULL)
            TEXT_BORROW(ir->callables[i].name, strlen(ir->callables[i].name) + 1);
    for (size_t i = 0; i < ir->local_count; ++i)
        if (ir->locals[i].name != NULL)
            TEXT_BORROW(ir->locals[i].name, strlen(ir->locals[i].name) + 1);
    for (size_t i = 0; i < ir->field_count; ++i)
        if (ir->fields[i].name != NULL)
            TEXT_BORROW(ir->fields[i].name, strlen(ir->fields[i].name) + 1);
    for (size_t i = 0; i < ir->variant_count; ++i)
        if (ir->variants[i].name != NULL)
            TEXT_BORROW(ir->variants[i].name, strlen(ir->variants[i].name) + 1);
    for (size_t i = 0; i < ir->expression_count; ++i) {
        if (ir->expressions[i].kind == SOL_IR_EXPR_STRING) {
            TEXT_BORROW(ir->expressions[i].as.string,
                strlen(ir->expressions[i].as.string) + 1);
        } else if (ir->expressions[i].kind == SOL_IR_EXPR_HANDLE) {
            TEXT_BORROW(ir->expressions[i].as.handler.effect_name,
                strlen(ir->expressions[i].as.handler.effect_name) + 1);
        }
    }
    for (size_t i = 0; i < ir->statement_count; ++i)
        if (ir->statements[i].region_label != NULL)
            TEXT_BORROW(ir->statements[i].region_label,
                strlen(ir->statements[i].region_label) + 1);
    for (size_t i = 0; i < ir->effect_count; ++i)
        TEXT_BORROW(ir->effects[i].name, strlen(ir->effects[i].name) + 1);
    for (size_t i = 0; i < ir->generic_parameter_count; ++i)
        TEXT_BORROW(ir->generic_parameters[i].name,
            strlen(ir->generic_parameters[i].name) + 1);
    for (size_t i = 0; i < ir->effect_parameter_count; ++i)
        TEXT_BORROW(ir->effect_parameters[i].name,
            strlen(ir->effect_parameters[i].name) + 1);
    for (size_t i = 0; i < ir->file_count; ++i)
        TEXT_BORROW(ir->files[i].path, strlen(ir->files[i].path) + 1);
#undef TEXT_BORROW
    SolMirRepresentation expected;
    sol_mir_representation_init(&expected);
    expected.materialization = out->materialization; expected.limits = out->limits;
    Builder builder = {&expected, NULL, SOL_MIR_REPRESENTATION_BUILD_INTERNAL_FAILED};
    if (!build_scratch(&builder)) {
        sol_mir_representation_free(&expected); goto malformed;
    }
    if (!allocation_shape_equal(out, &expected) || !shallow_graph(out)
        || !local_graph(out)) {
        sol_mir_representation_free(&expected); goto malformed;
    }
    bool equal = arrays_equal(out, &expected);
    sol_mir_representation_free(&expected);
    if (!equal) goto malformed;
    return true;
malformed:
    return error(diagnostics, "representation graph, classification, or owned arenas are malformed");
}

typedef struct { char *data; size_t length, capacity; bool failed; } Buffer;

static void format(Buffer *buffer, const char *pattern, ...) {
    if (buffer->failed) return;
    va_list args; va_start(args, pattern); va_list copy; va_copy(copy, args);
    int count = vsnprintf(NULL, 0, pattern, copy); va_end(copy);
    if (count < 0 || (size_t)count > SIZE_MAX - buffer->length - 1) {
        buffer->failed = true; va_end(args); return;
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
        buffer->data = grown; buffer->capacity = capacity;
    }
    (void)vsnprintf(buffer->data + buffer->length,
        buffer->capacity - buffer->length, pattern, args);
    va_end(args); buffer->length += (size_t)count;
}

bool sol_mir_representation_render(FILE *stream,
    const SolMirRepresentation *out) {
    if (stream == NULL || !sol_mir_representation_validate(out, NULL)) return false;
    Buffer buffer = {0};
    format(&buffer, "mir_representation recipes=%zu fields=%zu variants=%zu producers=%zu receiver_roots=%zu build_scratch=%zu validation_work=%zu validation_scratch=%zu\n",
        out->recipe_count, out->field_count, out->variant_count,
        out->callable_producer_count, out->receiver_root_count,
        out->usage.build_scratch_bytes, out->usage.validation_work,
        out->usage.validation_scratch_bytes);
    for (size_t i = 0; i < out->recipe_count; ++i) {
        const SolMirRecipe *recipe = &out->recipes[i];
        format(&buffer, "recipe r%zu kind=%d definition=%zu inhabited=%d zero=%d storage=%d copy=%d/%d drop=%d fields=[%zu,%zu] variants=[%zu,%zu] params=[%zu,%zu] result=%zu effects=%zu backing=%zu capability_source=%zu\n",
            i, (int)recipe->kind, recipe->concrete_definition,
            recipe->inhabited, recipe->zero_sized, (int)recipe->storage,
            recipe->is_copy, (int)recipe->copy_kind, (int)recipe->drop_kind,
            recipe->fields.offset, recipe->fields.count,
            recipe->variants.offset, recipe->variants.count,
            recipe->parameters.offset, recipe->parameters.count,
            recipe->result, recipe->effects, recipe->backing,
            recipe->capability_source);
    }
    for (size_t i = 0; i < out->field_count; ++i) {
        const SolMirRecipeField *field = &out->fields[i];
        format(&buffer, "field f%zu source=%zu ordinal=%zu type=r%zu\n",
            i, field->source_field, field->ordinal, field->type);
    }
    for (size_t i = 0; i < out->variant_count; ++i) {
        const SolMirRecipeVariant *variant = &out->variants[i];
        format(&buffer, "variant v%zu source=%zu ordinal=%zu tag=%zu fields=[%zu,%zu]\n",
            i, variant->source_variant, variant->ordinal, variant->semantic_tag,
            variant->fields.offset, variant->fields.count);
    }
    for (size_t i = 0; i < out->callable_producer_count; ++i) {
        const SolMirCallableProducer *producer = &out->callable_producers[i];
        format(&buffer, "producer p%zu kind=%d function=r%zu site=s%zu binding=d%zu target=%d/%zu/%zu receiver=r%zu capture=%d:x%zu/place=%zu/temp=%zu/value=%zu/instruction=%zu roots=[%zu,%zu] effects=%zu\n",
            i, (int)producer->kind, producer->function_recipe,
            producer->semantic_site, producer->binding,
            (int)producer->target_kind, producer->instance, producer->import,
            producer->captured_receiver_type,
            (int)producer->captured_receiver_kind,
            producer->captured_receiver_expression,
            producer->captured_receiver_place,
            producer->captured_receiver_temporary,
            producer->captured_receiver_value,
            producer->captured_receiver_instruction,
            producer->captured_receiver_roots.offset,
            producer->captured_receiver_roots.count,
            producer->effects);
    }
    bool ok = !buffer.failed
        && (buffer.length == 0 || fwrite(buffer.data, buffer.length, 1, stream) == 1);
    free(buffer.data); return ok;
}
