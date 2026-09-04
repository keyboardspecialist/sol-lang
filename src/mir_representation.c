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

bool sol_mir_representation_validation_requirements(
    const SolMirRepresentation *representation, size_t *work, size_t *scratch);

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
        || !sol_mir_representation_validation_requirements(out,
            &out->usage.validation_work,
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
