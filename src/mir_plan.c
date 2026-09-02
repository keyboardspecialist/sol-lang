#include "sol/mir_plan.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SolIrTypeKind kind;
    SolIrDefinitionId definition;
    SolMirPlanTypeId *arguments;
    size_t argument_count;
    SolMirPlanTypeId *parameters;
    SolAccessMode *accesses;
    size_t parameter_count;
    SolMirPlanTypeId result;
    SolMirPlanEffectRowId effects;
} RawType;

typedef struct {
    size_t *atoms;
    size_t count;
} RawRow;

typedef struct {
    size_t generic_ordinal;
    SolIrDefinitionId trait;
    SolIrCallableId requirement;
    SolMirPlanTypeId type;
    SolIrDefinitionId implementation;
    SolIrCallableId method;
} RawDictionary;

typedef struct {
    SolIrCallableId callable;
    SolMirPlanTypeId receiver;
    SolMirPlanTypeId *arguments;
    size_t argument_count;
    RawDictionary *dictionary;
    size_t dictionary_count;
    SolMirPlanEffectRowId effect_tail;
    SolMirPlanEffectRowId effects;
    SolMirPlanTypedUse *uses;
    size_t use_count;
    size_t use_capacity;
    SolMirPlanInstanceId parent;
    bool active;
    bool scanned;
} RawInstance;

typedef struct {
    SolIrCallableId callable;
    SolMirPlanTypeId receiver;
    SolMirPlanTypeId *parameters;
    SolAccessMode *accesses;
    size_t parameter_count;
    SolMirPlanTypeId result;
    SolMirPlanEffectRowId effects;
} RawImport;

typedef struct Builder Builder;

typedef struct {
    Builder *builder;
    RawInstance *instance;
    SolMirPlanInstanceId instance_id;
    SolIrDefinitionId nominal;
    const SolMirPlanTypeId *nominal_arguments;
    size_t nominal_argument_count;
    SolMirPlanTypeId self_type;
    size_t context;
} Environment;

struct Builder {
    const SolMirProgram *program;
    const SolIr *ir;
    SolMirPlan *plan;
    SolDiagnostics *diagnostics;
    SolMirPlanBuildOutcome outcome;
    RawType *types;
    size_t type_count;
    size_t type_capacity;
    SolMirPlanEffectAtom *atoms;
    size_t atom_count;
    size_t atom_capacity;
    RawRow *rows;
    size_t row_count;
    size_t row_capacity;
    RawInstance *instances;
    size_t instance_count;
    size_t instance_capacity;
    RawImport *imports;
    size_t import_count;
    size_t import_capacity;
    SolMirPlanDemand *demands;
    size_t demand_count;
    size_t demand_capacity;
};

static bool plan_empty(const SolMirPlan *plan) {
    if (plan == NULL) return false;
    const unsigned char *bytes = (const unsigned char *)plan;
    for (size_t index = 0; index < sizeof(*plan); ++index) {
        if (bytes[index] != 0) return false;
    }
    return true;
}

void sol_mir_plan_init(SolMirPlan *plan) {
    if (plan != NULL) memset(plan, 0, sizeof(*plan));
}

void sol_mir_plan_free(SolMirPlan *plan) {
    if (plan == NULL) return;
    for (size_t index = 0; plan->effect_atoms != NULL
        && index < plan->effect_atom_capacity; ++index) {
        free(plan->effect_atoms[index].name);
    }
    free(plan->types);
    free(plan->type_components);
    free(plan->type_parameter_accesses);
    free(plan->effect_atoms);
    free(plan->effect_rows);
    free(plan->effect_row_atoms);
    free(plan->instances);
    free(plan->instance_type_ids);
    free(plan->instance_accesses);
    free(plan->dictionary_entries);
    free(plan->imports);
    free(plan->typed_uses);
    free(plan->demands);
    sol_mir_plan_init(plan);
}

SolMirPlanLimits sol_mir_plan_default_limits(void) {
    return (SolMirPlanLimits){4096, 65536, 131072, 1000000, 4000000, 256};
}

static bool limits_zero(SolMirPlanLimits limits) {
    return limits.max_instances == 0 && limits.max_concrete_types == 0
        && limits.max_demands == 0 && limits.max_typed_uses == 0
        && limits.max_planning_work == 0 && limits.max_substitution_depth == 0;
}

static bool limits_complete(SolMirPlanLimits limits) {
    return limits.max_instances != 0 && limits.max_concrete_types != 0
        && limits.max_demands != 0 && limits.max_typed_uses != 0
        && limits.max_planning_work != 0 && limits.max_substitution_depth != 0;
}

static void report(Builder *builder, const char *message) {
    if (builder->diagnostics != NULL) {
        sol_diagnostics_add(builder->diagnostics, "SOL-MIR-PLAN-001",
            SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    }
}

static bool fail(Builder *builder, SolMirPlanBuildOutcome outcome,
    const char *message) {
    if (builder->outcome == SOL_MIR_PLAN_BUILD_ALLOCATION_FAILED
        || builder->outcome == SOL_MIR_PLAN_BUILD_INTERNAL_FAILED
        || builder->outcome == SOL_MIR_PLAN_BUILD_SUCCEEDED) {
        builder->outcome = outcome;
    }
    report(builder, message);
    return false;
}

static bool grow(void **items, size_t *capacity, size_t needed, size_t size) {
    if (needed <= *capacity) return true;
    size_t next = *capacity == 0 ? 8 : *capacity;
    while (next < needed) {
        if (next > SIZE_MAX / 2) { next = needed; break; }
        next *= 2;
    }
    if (size == 0 || next > SIZE_MAX / size) return false;
    void *value = realloc(*items, next * size);
    if (value == NULL) return false;
    *items = value;
    *capacity = next;
    return true;
}

static void *allocate(size_t count, size_t size) {
    if (count == 0) return NULL;
    if (size == 0 || count > SIZE_MAX / size) return NULL;
    return calloc(count, size);
}

static char *copy_string(const char *text) {
    size_t length = strlen(text);
    if (length == SIZE_MAX) return NULL;
    char *copy = malloc(length + 1);
    if (copy != NULL) memcpy(copy, text, length + 1);
    return copy;
}

static bool charge(Builder *builder, size_t count) {
    size_t used = builder->plan->usage.planning_work;
    if (count > builder->plan->limits.max_planning_work - used) {
        return fail(builder, SOL_MIR_PLAN_BUILD_RESOURCE_EXHAUSTED,
            "monomorphic planning work limit exceeded");
    }
    builder->plan->usage.planning_work += count;
    return true;
}

static const SolMirProgramTemplate *find_template(const Builder *builder,
    SolIrCallableId callable) {
    size_t low = 0;
    size_t high = builder->program->template_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (builder->program->templates[middle].callable < callable) low = middle + 1;
        else high = middle;
    }
    return low < builder->program->template_count
            && builder->program->templates[low].callable == callable
        ? &builder->program->templates[low] : NULL;
}

static bool source_for(Builder *builder, SolIrCallableId callable,
    SolIrExpressionId expression, SolSpan span, SolMirProgramSource *source) {
    *source = (SolMirProgramSource){callable, expression, 0, span.start, span.end};
    for (size_t file = 0; file < builder->ir->file_count; ++file) {
        const SolIrSourceFile *item = &builder->ir->files[file];
        if (span.start >= item->aggregate_start && span.end <= item->aggregate_end) {
            source->file = file;
            source->start = span.start - item->aggregate_start;
            source->end = span.end - item->aggregate_start;
            return true;
        }
    }
    return fail(builder, SOL_MIR_PLAN_BUILD_INVALID_PROGRAM,
        "plan demand span has no owning source file");
}

static int atom_compare_value(const SolMirPlanEffectAtom *left,
    const SolMirPlanEffectAtom *right) {
    size_t common = left->length < right->length
        ? left->length : right->length;
    int name = common == 0 ? 0 : memcmp(left->name, right->name, common);
    if (name != 0) return name;
    if (left->length != right->length) {
        return left->length < right->length ? -1 : 1;
    }
    if (left->authority != right->authority) {
        return left->authority < right->authority ? -1 : 1;
    }
    return left->ordinal < right->ordinal ? -1 : left->ordinal > right->ordinal;
}

static bool intern_atom(Builder *builder, const char *name,
    SolMirPlanEffectAuthority authority, size_t ordinal, size_t *result) {
    size_t length = strlen(name);
    SolMirPlanEffectAtom candidate = {(char *)name, length, authority, ordinal};
    for (size_t index = 0; index < builder->atom_count; ++index) {
        if (!charge(builder, 1)) return false;
        if (atom_compare_value(&candidate, &builder->atoms[index]) == 0) {
            *result = index;
            return true;
        }
    }
    if (!grow((void **)&builder->atoms, &builder->atom_capacity,
            builder->atom_count + 1, sizeof(*builder->atoms))) return false;
    char *owned = copy_string(name);
    if (owned == NULL) return false;
    builder->atoms[builder->atom_count]
        = (SolMirPlanEffectAtom){owned, length, authority, ordinal};
    *result = builder->atom_count++;
    return true;
}

static int size_compare(const void *left, const void *right) {
    size_t a = *(const size_t *)left;
    size_t b = *(const size_t *)right;
    return a < b ? -1 : a > b;
}

static bool intern_row(Builder *builder, size_t *atoms, size_t count,
    SolMirPlanEffectRowId *result) {
    if (count > 1) qsort(atoms, count, sizeof(*atoms), size_compare);
    size_t unique = 0;
    for (size_t index = 0; index < count; ++index) {
        if (unique == 0 || atoms[index] != atoms[unique - 1]) {
            atoms[unique++] = atoms[index];
        }
    }
    count = unique;
    for (size_t index = 0; index < builder->row_count; ++index) {
        if (!charge(builder, 1)) return false;
        if (builder->rows[index].count == count
            && (count == 0 || memcmp(builder->rows[index].atoms, atoms,
                count * sizeof(*atoms)) == 0)) {
            *result = index;
            return true;
        }
    }
    if (!grow((void **)&builder->rows, &builder->row_capacity,
            builder->row_count + 1, sizeof(*builder->rows))) return false;
    size_t *owned = allocate(count, sizeof(*owned));
    if (count != 0 && owned == NULL) return false;
    if (count != 0) memcpy(owned, atoms, count * sizeof(*owned));
    builder->rows[builder->row_count] = (RawRow){owned, count};
    *result = builder->row_count++;
    return true;
}

static bool authority_for(const Environment *environment,
    SolIrAuthorityKind kind, SolIrLocalId local,
    SolMirPlanEffectAuthority fallback, SolMirPlanEffectAuthority *authority,
    size_t *ordinal) {
    const SolIr *ir = environment->builder->ir;
    const SolIrCallable *callable = &ir->callables[environment->instance->callable];
    *ordinal = 0;
    if (kind == SOL_IR_AUTHORITY_NONE) {
        *authority = SOL_MIR_PLAN_EFFECT_AUTHORITY_NONE;
        return true;
    }
    if (kind == SOL_IR_AUTHORITY_SELF || local == callable->receiver) {
        *authority = SOL_MIR_PLAN_EFFECT_AUTHORITY_RECEIVER;
        return true;
    }
    for (size_t index = 0; index < callable->parameters.count; ++index) {
        if (ir->roots[callable->parameters.offset + index] == local) {
            *authority = SOL_MIR_PLAN_EFFECT_AUTHORITY_PARAMETER;
            *ordinal = index;
            return true;
        }
    }
    /* Local provenance is intentionally not part of reusable instance identity. */
    *authority = fallback;
    return true;
}

static bool row_from_source(Environment *environment, SolIrSlice slice,
    SolIrEffectParameterId tail, bool tail_independent,
    SolMirPlanEffectRowId *result) {
    Builder *builder = environment->builder;
    size_t extra = 0;
    if (tail != SOL_IR_NONE) {
        const SolIrCallable *callable
            = &builder->ir->callables[environment->instance->callable];
        if (tail != callable->effect_parameter) {
            return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
                "effect tail is not bound by the current instance");
        }
        extra = builder->rows[environment->instance->effect_tail].count;
    }
    if (slice.count > SIZE_MAX - extra) return false;
    size_t count = slice.count + extra;
    size_t *atoms = allocate(count, sizeof(*atoms));
    if (count != 0 && atoms == NULL) return false;
    size_t used = 0;
    for (size_t index = 0; index < slice.count; ++index) {
        const SolIrEffect *source
            = &builder->ir->effects[slice.offset + index];
        SolMirPlanEffectAuthority authority;
        size_t ordinal;
        if (!authority_for(environment, source->authority_kind, source->authority,
                tail_independent ? SOL_MIR_PLAN_EFFECT_AUTHORITY_TAIL
                    : SOL_MIR_PLAN_EFFECT_AUTHORITY_NONE,
                &authority, &ordinal)
            || !intern_atom(builder, source->name, authority, ordinal,
                &atoms[used++])) {
            free(atoms);
            return false;
        }
    }
    if (tail != SOL_IR_NONE) {
        const RawRow *row = &builder->rows[environment->instance->effect_tail];
        for (size_t index = 0; index < row->count; ++index) {
            const SolMirPlanEffectAtom *atom
                = &builder->atoms[row->atoms[index]];
            if (atom->authority == SOL_MIR_PLAN_EFFECT_AUTHORITY_NONE) {
                atoms[used++] = row->atoms[index];
            } else if (!intern_atom(builder, atom->name,
                    SOL_MIR_PLAN_EFFECT_AUTHORITY_TAIL, 0,
                    &atoms[used++])) {
                free(atoms);
                return false;
            }
        }
    }
    bool ok = intern_row(builder, atoms, used, result);
    free(atoms);
    return ok;
}

static bool fixed_authority(Builder *builder, SolIrCallableId callable_id,
    const SolIrEffect *effect, SolMirPlanEffectAuthority *authority,
    size_t *ordinal) {
    const SolIrCallable *callable = &builder->ir->callables[callable_id];
    *ordinal = 0;
    if (effect->authority_kind == SOL_IR_AUTHORITY_NONE) {
        *authority = SOL_MIR_PLAN_EFFECT_AUTHORITY_NONE;
        return true;
    }
    if (effect->authority_kind == SOL_IR_AUTHORITY_SELF
        || effect->authority == callable->receiver) {
        *authority = SOL_MIR_PLAN_EFFECT_AUTHORITY_RECEIVER;
        return true;
    }
    for (size_t index = 0; index < callable->parameters.count; ++index) {
        if (builder->ir->roots[callable->parameters.offset + index]
                == effect->authority) {
            *authority = SOL_MIR_PLAN_EFFECT_AUTHORITY_PARAMETER;
            *ordinal = index;
            return true;
        }
    }
    return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
        "fixed effect authority is not a callee receiver or formal parameter");
}

static bool row_for_callable(Builder *builder, SolIrCallableId callable_id,
    SolMirPlanEffectRowId tail, SolMirPlanEffectRowId *result) {
    const SolIrCallable *callable = &builder->ir->callables[callable_id];
    const RawRow *tail_row = &builder->rows[tail];
    if (callable->effects.count > SIZE_MAX - tail_row->count) return false;
    size_t capacity = callable->effects.count + tail_row->count;
    size_t *atoms = allocate(capacity, sizeof(*atoms));
    if (capacity != 0 && atoms == NULL) return false;
    size_t count = 0;
    for (size_t index = 0; index < callable->effects.count; ++index) {
        const SolIrEffect *effect
            = &builder->ir->effects[callable->effects.offset + index];
        SolMirPlanEffectAuthority authority;
        size_t ordinal;
        if (!fixed_authority(builder, callable_id, effect, &authority, &ordinal)
            || !intern_atom(builder, effect->name, authority, ordinal,
                &atoms[count++])) {
            free(atoms);
            return false;
        }
    }
    if (tail_row->count != 0) {
        memcpy(atoms + count, tail_row->atoms,
            tail_row->count * sizeof(*atoms));
        count += tail_row->count;
    }
    bool ok = intern_row(builder, atoms, count, result);
    free(atoms);
    return ok;
}

static bool substitute_type(Environment *environment, SolIrTypeId source,
    size_t depth, SolMirPlanTypeId *result);

static bool append_inferred_tail(Builder *builder, const RawRow *actual,
    const RawRow *fixed, size_t **atoms, size_t *count, size_t *capacity) {
    for (size_t index = 0; index < actual->count; ++index) {
        size_t atom_id = actual->atoms[index];
        bool fixed_atom = false;
        for (size_t candidate = 0; candidate < fixed->count; ++candidate) {
            fixed_atom = fixed_atom || fixed->atoms[candidate] == atom_id;
        }
        if (fixed_atom) continue;
        if (builder->atoms[atom_id].authority
                != SOL_MIR_PLAN_EFFECT_AUTHORITY_NONE) {
            return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
                "authority-bearing inferred effect tail is unsupported");
        }
        if (!grow((void **)atoms, capacity, *count + 1, sizeof(**atoms))) {
            return false;
        }
        (*atoms)[(*count)++] = atom_id;
    }
    return true;
}

static bool tail_for_call(Environment *environment, SolIrCallableId target,
    const SolIrExpression *call, SolMirPlanEffectRowId *result) {
    Builder *builder = environment->builder;
    const SolIrCallable *callee = &builder->ir->callables[target];
    size_t no_atoms = 0;
    if (callee->effect_parameter == SOL_IR_NONE) {
        return intern_row(builder, &no_atoms, 0, result);
    }
    if (call == NULL || call->kind != SOL_IR_EXPR_CALL) {
        return fail(builder, SOL_MIR_PLAN_BUILD_INVALID_PROGRAM,
            "effect-polymorphic demand has no exact source call expression");
    }
    size_t *atoms = NULL;
    size_t count = 0;
    size_t capacity = 0;
    bool found_callback = false;
    for (size_t formal = 0; formal < callee->parameters.count; ++formal) {
        SolIrLocalId local
            = builder->ir->roots[callee->parameters.offset + formal];
        SolIrTypeId formal_id = builder->ir->locals[local].type;
        if (formal_id >= builder->ir->type_count) goto invalid;
        const SolIrType *formal_type = &builder->ir->types[formal_id];
        if (formal_type->kind != SOL_IR_TYPE_FUNCTION
            || formal_type->effect_parameter != callee->effect_parameter) continue;
        found_callback = true;
        SolIrExpressionId actual_id = SOL_IR_NONE;
        for (size_t operand = 0; operand < call->as.call.operands.count; ++operand) {
            const SolIrOperand *item = &builder->ir->operands[
                call->as.call.operands.offset + operand];
            if (item->formal == formal) {
                actual_id = item->value;
                break;
            }
        }
        if (actual_id >= builder->ir->expression_count) goto invalid;
        SolMirPlanTypeId concrete_id;
        if (!substitute_type(environment,
                builder->ir->expressions[actual_id].type, 1,
                &concrete_id)) goto failed;
        const RawType *concrete = &builder->types[concrete_id];
        if (concrete->kind != SOL_IR_TYPE_FUNCTION
            || concrete->effects >= builder->row_count) goto invalid;
        SolMirPlanEffectRowId fixed_id;
        if (!row_from_source(environment, formal_type->effects, SOL_IR_NONE,
                true, &fixed_id)
            || !append_inferred_tail(builder, &builder->rows[concrete->effects],
                &builder->rows[fixed_id], &atoms, &count, &capacity)) goto failed;
    }
    if (!found_callback) {
        free(atoms);
        return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
            "effect parameter has no callback formal inference source");
    }
    if (call->as.call.effect_parameter != SOL_IR_NONE) {
        const SolIrCallable *caller
            = &builder->ir->callables[environment->instance->callable];
        if (call->as.call.effect_parameter != caller->effect_parameter) {
            free(atoms);
            return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
                "forwarded effect tail is outside the caller instance");
        }
        const RawRow *forwarded
            = &builder->rows[environment->instance->effect_tail];
        RawRow empty = {0};
        if (!append_inferred_tail(builder, forwarded, &empty,
                &atoms, &count, &capacity)) goto failed;
    }
    bool ok = intern_row(builder, atoms, count, result);
    free(atoms);
    return ok;
invalid:
    fail(builder, SOL_MIR_PLAN_BUILD_INVALID_PROGRAM,
        "effect callback inference metadata is malformed");
failed:
    free(atoms);
    return false;
}

static bool raw_type_equal(const RawType *left, const RawType *right) {
    return left->kind == right->kind && left->definition == right->definition
        && left->argument_count == right->argument_count
        && left->parameter_count == right->parameter_count
        && left->result == right->result && left->effects == right->effects
        && (left->argument_count == 0 || memcmp(left->arguments, right->arguments,
            left->argument_count * sizeof(*left->arguments)) == 0)
        && (left->parameter_count == 0 || (memcmp(left->parameters,
            right->parameters, left->parameter_count * sizeof(*left->parameters)) == 0
            && memcmp(left->accesses, right->accesses,
                left->parameter_count * sizeof(*left->accesses)) == 0));
}

static bool intern_type(Builder *builder, RawType *candidate,
    SolMirPlanTypeId *result) {
    for (size_t index = 0; index < builder->type_count; ++index) {
        if (!charge(builder, 1)) return false;
        if (raw_type_equal(candidate, &builder->types[index])) {
            free(candidate->arguments);
            free(candidate->parameters);
            free(candidate->accesses);
            *result = index;
            return true;
        }
    }
    if (builder->type_count == builder->plan->limits.max_concrete_types) {
        return fail(builder, SOL_MIR_PLAN_BUILD_RESOURCE_EXHAUSTED,
            "concrete type limit exceeded");
    }
    if (!grow((void **)&builder->types, &builder->type_capacity,
            builder->type_count + 1, sizeof(*builder->types))) return false;
    builder->types[builder->type_count] = *candidate;
    *result = builder->type_count++;
    builder->plan->usage.concrete_types = builder->type_count;
    return true;
}

static bool substitute_type(Environment *environment, SolIrTypeId source,
    size_t depth, SolMirPlanTypeId *result) {
    Builder *builder = environment->builder;
    if (depth > builder->plan->limits.max_substitution_depth) {
        return fail(builder, SOL_MIR_PLAN_BUILD_RESOURCE_EXHAUSTED,
            "type substitution depth limit exceeded");
    }
    if (depth > builder->plan->usage.substitution_depth) {
        builder->plan->usage.substitution_depth = depth;
    }
    if (!charge(builder, 1) || source >= builder->ir->type_count) return false;
    const SolIrType *type = &builder->ir->types[source];
    if (type->kind == SOL_IR_TYPE_PARAMETER) {
        const SolIrCallable *callable
            = &builder->ir->callables[environment->instance->callable];
        if (type->definition >= callable->generic_parameters.offset
            && type->definition - callable->generic_parameters.offset
                < environment->instance->argument_count) {
            *result = environment->instance->arguments[
                type->definition - callable->generic_parameters.offset];
            return true;
        }
        if (environment->nominal != SOL_IR_NONE
            && type->definition >= builder->ir->definitions[
                environment->nominal].generic_parameters.offset
            && type->definition - builder->ir->definitions[
                environment->nominal].generic_parameters.offset
                < environment->nominal_argument_count) {
            *result = environment->nominal_arguments[type->definition
                - builder->ir->definitions[environment->nominal]
                    .generic_parameters.offset];
            return true;
        }
        return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
            "concrete type contains an unbound source type parameter");
    }
    if (type->kind == SOL_IR_TYPE_SELF) {
        SolMirPlanTypeId self = environment->self_type != SOL_MIR_PLAN_NONE
            ? environment->self_type : environment->instance->receiver;
        if (self == SOL_MIR_PLAN_NONE) {
            return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
                "concrete type contains Self without a receiver context");
        }
        *result = self;
        return true;
    }
    RawType candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.kind = type->kind;
    candidate.definition = type->definition;
    candidate.result = SOL_MIR_PLAN_NONE;
    candidate.effects = SOL_MIR_PLAN_NONE;
    if (type->kind == SOL_IR_TYPE_FUNCTION && type->definition != SOL_IR_NONE) {
        if (type->definition >= builder->ir->definition_count) return false;
        SolIrCallableId exact_id
            = builder->ir->definitions[type->definition].callable;
        if (exact_id >= builder->ir->callable_count) return false;
        const SolIrCallable *exact = &builder->ir->callables[exact_id];
        if (exact->generic_parameters.count != 0
            || exact->effect_parameters.count != 0) {
            return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
                "generic exact function value has no closed instance context");
        }
        RawInstance exact_instance;
        memset(&exact_instance, 0, sizeof(exact_instance));
        exact_instance.callable = exact_id;
        exact_instance.receiver = SOL_MIR_PLAN_NONE;
        exact_instance.effect_tail = 0;
        exact_instance.effects = 0;
        Environment exact_environment = {builder, &exact_instance,
            environment->instance_id, SOL_IR_NONE, NULL, 0,
            SOL_MIR_PLAN_NONE, environment->context};
        candidate.parameter_count = exact->parameters.count;
        candidate.parameters = allocate(candidate.parameter_count,
            sizeof(*candidate.parameters));
        candidate.accesses = allocate(candidate.parameter_count,
            sizeof(*candidate.accesses));
        if (candidate.parameter_count != 0
            && (candidate.parameters == NULL || candidate.accesses == NULL)) {
            goto failed;
        }
        for (size_t index = 0; index < candidate.parameter_count; ++index) {
            SolIrLocalId local
                = builder->ir->roots[exact->parameters.offset + index];
            candidate.accesses[index] = builder->ir->locals[local].access;
            if (!substitute_type(&exact_environment,
                    builder->ir->locals[local].type, depth + 1,
                    &candidate.parameters[index])) goto failed;
        }
        if (!substitute_type(&exact_environment, exact->result, depth + 1,
                &candidate.result)
            || !row_for_callable(builder, exact_id, 0,
                &candidate.effects)) goto failed;
        if (intern_type(builder, &candidate, result)) return true;
        goto failed;
    }
    candidate.argument_count = type->argument_count;
    candidate.arguments = allocate(candidate.argument_count,
        sizeof(*candidate.arguments));
    if (candidate.argument_count != 0 && candidate.arguments == NULL) return false;
    for (size_t index = 0; index < candidate.argument_count; ++index) {
        if (!substitute_type(environment,
                builder->ir->type_ids[type->argument_offset + index], depth + 1,
                &candidate.arguments[index])) goto failed;
    }
    if (type->kind == SOL_IR_TYPE_FUNCTION) {
        candidate.parameter_count = type->parameter_count;
        candidate.parameters = allocate(candidate.parameter_count,
            sizeof(*candidate.parameters));
        candidate.accesses = allocate(candidate.parameter_count,
            sizeof(*candidate.accesses));
        if (candidate.parameter_count != 0
            && (candidate.parameters == NULL || candidate.accesses == NULL)) goto failed;
        for (size_t index = 0; index < candidate.parameter_count; ++index) {
            if (!substitute_type(environment,
                    builder->ir->type_ids[type->parameter_offset + index], depth + 1,
                    &candidate.parameters[index])) goto failed;
            candidate.accesses[index]
                = builder->ir->accesses[type->parameter_access_offset + index];
        }
        if (!substitute_type(environment, type->result, depth + 1,
                &candidate.result)
            || !row_from_source(environment, type->effects,
                type->effect_parameter, true, &candidate.effects)) goto failed;
    }
    if (intern_type(builder, &candidate, result)) return true;
failed:
    free(candidate.arguments);
    free(candidate.parameters);
    free(candidate.accesses);
    return false;
}

static int dictionary_compare(const void *left, const void *right) {
    const RawDictionary *a = left;
    const RawDictionary *b = right;
#define CMP(field) if (a->field != b->field) return a->field < b->field ? -1 : 1
    CMP(generic_ordinal); CMP(trait); CMP(requirement); CMP(type);
    CMP(implementation); CMP(method);
#undef CMP
    return 0;
}

static bool dictionary_equal(const RawInstance *instance,
    const RawDictionary *dictionary, size_t count) {
    return instance->dictionary_count == count
        && (count == 0 || memcmp(instance->dictionary, dictionary,
            count * sizeof(*dictionary)) == 0);
}

static bool instance_key_equal(const RawInstance *instance,
    SolIrCallableId callable, SolMirPlanTypeId receiver,
    const SolMirPlanTypeId *arguments, size_t argument_count,
    const RawDictionary *dictionary, size_t dictionary_count,
    SolMirPlanEffectRowId effect_tail, SolMirPlanEffectRowId effects) {
    return instance->callable == callable && instance->receiver == receiver
        && instance->argument_count == argument_count
        && instance->effect_tail == effect_tail && instance->effects == effects
        && (argument_count == 0 || memcmp(instance->arguments, arguments,
            argument_count * sizeof(*arguments)) == 0)
        && dictionary_equal(instance, dictionary, dictionary_count);
}

static bool type_properly_contains(const Builder *builder,
    SolMirPlanTypeId container, SolMirPlanTypeId contained, size_t depth) {
    if (container == contained) return false;
    if (container >= builder->type_count || contained >= builder->type_count
        || depth > builder->type_count) return false;
    const RawType *type = &builder->types[container];
    for (size_t index = 0; index < type->argument_count; ++index) {
        SolMirPlanTypeId child = type->arguments[index];
        if (child == contained || type_properly_contains(builder, child,
                contained, depth + 1)) return true;
    }
    for (size_t index = 0; index < type->parameter_count; ++index) {
        SolMirPlanTypeId child = type->parameters[index];
        if (child == contained || type_properly_contains(builder, child,
                contained, depth + 1)) return true;
    }
    return type->result != SOL_MIR_PLAN_NONE
        && (type->result == contained || type_properly_contains(builder,
            type->result, contained, depth + 1));
}

static bool instance_grows_ancestor(const Builder *builder,
    const RawInstance *ancestor, SolMirPlanTypeId receiver,
    const SolMirPlanTypeId *arguments, size_t argument_count) {
    if (ancestor->receiver != SOL_MIR_PLAN_NONE
        && receiver != SOL_MIR_PLAN_NONE
        && type_properly_contains(builder, receiver, ancestor->receiver, 0)) {
        return true;
    }
    size_t count = ancestor->argument_count < argument_count
        ? ancestor->argument_count : argument_count;
    for (size_t index = 0; index < count; ++index) {
        if (type_properly_contains(builder, arguments[index],
                ancestor->arguments[index], 0)) return true;
    }
    return false;
}

static bool repeated_growth_for_parent(const Builder *builder,
    SolMirPlanInstanceId parent, SolIrCallableId callable,
    SolMirPlanTypeId receiver, const SolMirPlanTypeId *arguments,
    size_t argument_count) {
    SolMirPlanInstanceId nearest = SOL_MIR_PLAN_NONE;
    for (SolMirPlanInstanceId current = parent;
        current != SOL_MIR_PLAN_NONE;) {
        const RawInstance *instance = &builder->instances[current];
        if (instance->callable == callable) {
            if (nearest == SOL_MIR_PLAN_NONE) {
                nearest = current;
                if (!instance_grows_ancestor(builder, instance, receiver,
                        arguments, argument_count)) return false;
            } else {
                const RawInstance *later = &builder->instances[nearest];
                return instance_grows_ancestor(builder, instance,
                    later->receiver, later->arguments, later->argument_count);
            }
        }
        current = instance->parent;
    }
    return false;
}

static bool add_use(Environment *environment, SolMirPlanTypedUseKind kind,
    size_t source, size_t ordinal, SolIrTypeId symbolic, SolAccessMode access) {
    RawInstance *instance = environment->instance;
    for (size_t index = 0; index < instance->use_count; ++index) {
        SolMirPlanTypedUse *use = &instance->uses[index];
        if (use->kind == kind && use->source == source && use->ordinal == ordinal
            && use->context == environment->context) {
            SolMirPlanTypeId type;
            if (!substitute_type(environment, symbolic, 1, &type)) return false;
            if (type != use->type || access != use->access) {
                return fail(environment->builder,
                    SOL_MIR_PLAN_BUILD_INTERNAL_FAILED,
                    "typed-use identity collides with a different substitution");
            }
            return true;
        }
    }
    if (environment->builder->plan->usage.typed_uses
            == environment->builder->plan->limits.max_typed_uses) {
        return fail(environment->builder, SOL_MIR_PLAN_BUILD_RESOURCE_EXHAUSTED,
            "typed substitution entry limit exceeded");
    }
    SolMirPlanTypeId type;
    if (!substitute_type(environment, symbolic, 1, &type)
        || !grow((void **)&instance->uses, &instance->use_capacity,
            instance->use_count + 1, sizeof(*instance->uses))) return false;
    instance->uses[instance->use_count++]
        = (SolMirPlanTypedUse){kind, source, ordinal, environment->context,
            type, access};
    ++environment->builder->plan->usage.typed_uses;
    return true;
}

static bool add_local_use(Environment *environment, SolIrLocalId local) {
    if (local == SOL_IR_NONE) return true;
    if (local >= environment->builder->ir->local_count) return false;
    const SolIrLocal *item = &environment->builder->ir->locals[local];
    return add_use(environment, SOL_MIR_PLAN_USE_LOCAL, local, 0,
        item->type, item->access);
}

static bool add_place_uses(Environment *environment, SolIrPlaceId id) {
    if (id == SOL_IR_NONE) return true;
    const SolIr *ir = environment->builder->ir;
    if (id >= ir->place_count) return false;
    const SolIrPlace *place = &ir->places[id];
    SolIrTypeId root_type;
    if (place->root_kind == SOL_IR_PLACE_ROOT_LOCAL) {
        if (!add_local_use(environment, place->local)) return false;
        root_type = ir->locals[place->local].type;
    } else {
        root_type = ir->expressions[place->temporary].type;
    }
    if (!add_use(environment, SOL_MIR_PLAN_USE_PLACE_ROOT, id, 0,
            root_type, SOL_ACCESS_OWNED)) return false;
    for (size_t index = 0; index < place->projections.count; ++index) {
        const SolIrProjection *projection
            = &ir->projections[place->projections.offset + index];
        if (!add_use(environment, SOL_MIR_PLAN_USE_PLACE_PROJECTION, id, index,
                projection->type, SOL_ACCESS_OWNED)) return false;
    }
    return add_use(environment, SOL_MIR_PLAN_USE_PLACE_FINAL, id, 0,
        place->type, SOL_ACCESS_OWNED);
}

static bool same_program_source(SolMirProgramSource a,
    SolMirProgramSource b) {
    return a.callable == b.callable && a.expression == b.expression
        && a.file == b.file && a.start == b.start && a.end == b.end;
}

static bool add_demand(Builder *builder, SolMirPlanDemand demand) {
    for (size_t index = 0; index < builder->demand_count; ++index) {
        const SolMirPlanDemand *item = &builder->demands[index];
        if (item->kind == demand.kind && item->parent == demand.parent
            && same_program_source(item->source, demand.source)
            && item->symbolic_target == demand.symbolic_target
            && item->instance == demand.instance && item->import == demand.import
            && item->dispatch_trait == demand.dispatch_trait
            && item->dispatch_requirement == demand.dispatch_requirement
            && item->context == demand.context) return true;
    }
    if (builder->demand_count == builder->plan->limits.max_demands) {
        return fail(builder, SOL_MIR_PLAN_BUILD_RESOURCE_EXHAUSTED,
            "instance demand limit exceeded");
    }
    if (!grow((void **)&builder->demands, &builder->demand_capacity,
            builder->demand_count + 1, sizeof(*builder->demands))) return false;
    builder->demands[builder->demand_count++] = demand;
    builder->plan->usage.demands = builder->demand_count;
    return true;
}

static bool scan_instance(Builder *builder, SolMirPlanInstanceId instance_id);

static bool add_instance(Builder *builder, SolIrCallableId callable,
    SolMirPlanTypeId receiver, const SolMirPlanTypeId *arguments,
    size_t argument_count, RawDictionary *dictionary, size_t dictionary_count,
    SolMirPlanEffectRowId effect_tail, SolMirPlanEffectRowId effects,
    SolMirPlanInstanceId parent, SolMirPlanInstanceId *result) {
    if (dictionary_count > 1) qsort(dictionary, dictionary_count,
        sizeof(*dictionary), dictionary_compare);
    for (size_t index = 1; index < dictionary_count; ++index) {
        if (dictionary[index - 1].generic_ordinal == dictionary[index].generic_ordinal
            && dictionary[index - 1].trait == dictionary[index].trait
            && dictionary[index - 1].requirement == dictionary[index].requirement) {
            return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
                "resolved evidence dictionary contains a duplicate key");
        }
    }
    if (repeated_growth_for_parent(builder, parent, callable, receiver,
            arguments, argument_count)) {
        return fail(builder, SOL_MIR_PLAN_BUILD_EXPANDING_RECURSION,
            "callable repeats strict concrete type growth on one ancestry");
    }
    for (size_t index = 0; index < builder->instance_count; ++index) {
        if (!charge(builder, 1)) return false;
        if (instance_key_equal(&builder->instances[index], callable, receiver,
                arguments, argument_count, dictionary, dictionary_count,
                effect_tail, effects)) {
            *result = index;
            return true;
        }
    }
    if (builder->instance_count == builder->plan->limits.max_instances) {
        return fail(builder, SOL_MIR_PLAN_BUILD_RESOURCE_EXHAUSTED,
            "monomorphic instance limit exceeded");
    }
    if (callable >= builder->ir->callable_count || find_template(builder, callable) == NULL) {
        return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
            "executable instance has no bodyful symbolic MIR template");
    }
    const SolIrCallable *metadata = &builder->ir->callables[callable];
    if (metadata->generic_parameters.count != argument_count) {
        return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
            "callable instance type argument set is incomplete");
    }
    if (!grow((void **)&builder->instances, &builder->instance_capacity,
            builder->instance_count + 1, sizeof(*builder->instances))) return false;
    RawInstance item;
    memset(&item, 0, sizeof(item));
    item.callable = callable;
    item.receiver = receiver;
    item.argument_count = argument_count;
    item.arguments = allocate(argument_count, sizeof(*item.arguments));
    item.dictionary_count = dictionary_count;
    item.dictionary = allocate(dictionary_count, sizeof(*item.dictionary));
    if ((argument_count != 0 && item.arguments == NULL)
        || (dictionary_count != 0 && item.dictionary == NULL)) {
        free(item.arguments); free(item.dictionary); return false;
    }
    if (argument_count != 0) memcpy(item.arguments, arguments,
        argument_count * sizeof(*arguments));
    if (dictionary_count != 0) memcpy(item.dictionary, dictionary,
        dictionary_count * sizeof(*dictionary));
    item.effect_tail = effect_tail;
    item.effects = effects;
    item.parent = parent;
    *result = builder->instance_count;
    builder->instances[builder->instance_count++] = item;
    builder->plan->usage.instances = builder->instance_count;
    return scan_instance(builder, *result);
}

static bool resolve_evidence(Environment *environment,
    const SolIrDispatchEvidence *source, RawDictionary *resolved) {
    Builder *builder = environment->builder;
    const SolIrDispatchEvidence *selected = source;
    if (source->forwarded) {
        const SolIrCallable *callable
            = &builder->ir->callables[environment->instance->callable];
        if (source->parameter < callable->generic_parameters.offset
            || source->parameter - callable->generic_parameters.offset
                >= callable->generic_parameters.count) {
            return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
                "forwarded evidence names a foreign generic parameter");
        }
        size_t ordinal = source->parameter - callable->generic_parameters.offset;
        selected = NULL;
        for (size_t index = 0; index < environment->instance->dictionary_count;
            ++index) {
            RawDictionary *entry = &environment->instance->dictionary[index];
            if (entry->generic_ordinal == ordinal && entry->trait == source->trait
                && entry->requirement == source->requirement) {
                if (selected != NULL) {
                    return fail(builder,
                        SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
                        "forwarded evidence dictionary lookup is ambiguous");
                }
                resolved->trait = entry->trait;
                resolved->requirement = entry->requirement;
                resolved->type = entry->type;
                resolved->implementation = entry->implementation;
                resolved->method = entry->method;
                return true;
            }
        }
        return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
            "forwarded evidence has no current instance dictionary entry");
    }
    if (selected->method >= builder->ir->callable_count
        || builder->ir->callables[selected->method].body == SOL_IR_NONE) {
        return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
            "resolved executable requirement has no method body");
    }
    resolved->trait = selected->trait;
    resolved->requirement = selected->requirement;
    resolved->implementation = selected->implementation;
    resolved->method = selected->method;
    return substitute_type(environment, selected->type, 1, &resolved->type);
}

static bool build_dictionary(Environment *environment, SolIrCallableId target,
    SolIrSlice evidence_slice, const SolMirPlanTypeId *arguments,
    RawDictionary **output, size_t *output_count) {
    Builder *builder = environment->builder;
    const SolIrCallable *callable = &builder->ir->callables[target];
    RawDictionary *entries = allocate(evidence_slice.count, sizeof(*entries));
    if (evidence_slice.count != 0 && entries == NULL) return false;
    for (size_t index = 0; index < evidence_slice.count; ++index) {
        const SolIrDispatchEvidence *source
            = &builder->ir->evidence[evidence_slice.offset + index];
        if (source->binding < callable->generic_parameters.offset
            || source->binding - callable->generic_parameters.offset
                >= callable->generic_parameters.count) {
            free(entries);
            return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
                "invocation evidence binding is outside its callee");
        }
        entries[index].generic_ordinal
            = source->binding - callable->generic_parameters.offset;
        if (!resolve_evidence(environment, source, &entries[index])
            || entries[index].type != arguments[entries[index].generic_ordinal]) {
            free(entries);
            return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
                "resolved evidence type does not match the concrete type argument");
        }
    }
    if (evidence_slice.count > 1) qsort(entries, evidence_slice.count,
        sizeof(*entries), dictionary_compare);
    size_t expected = 0;
    for (size_t ordinal = 0; ordinal < callable->generic_parameters.count;
        ++ordinal) {
        SolIrDefinitionId trait = builder->ir->generic_parameters[
            callable->generic_parameters.offset + ordinal].trait_bound;
        if (trait == SOL_IR_NONE) continue;
        SolIrSlice members = builder->ir->definitions[trait].members;
        for (size_t member = 0; member < members.count; ++member) {
            SolIrCallableId requirement
                = builder->ir->members[members.offset + member].callable;
            size_t found = 0;
            for (size_t index = 0; index < evidence_slice.count; ++index) {
                found += entries[index].generic_ordinal == ordinal
                    && entries[index].trait == trait
                    && entries[index].requirement == requirement;
            }
            if (found != 1) {
                free(entries);
                return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
                    "callee evidence dictionary is incomplete or contains extras");
            }
            ++expected;
        }
    }
    if (expected != evidence_slice.count) {
        free(entries);
        return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
            "callee evidence dictionary contains an extra entry");
    }
    *output = entries;
    *output_count = evidence_slice.count;
    return true;
}

static bool import_key_equal(const RawImport *item, SolIrCallableId callable,
    SolMirPlanTypeId receiver, const SolMirPlanTypeId *parameters,
    const SolAccessMode *accesses, size_t parameter_count,
    SolMirPlanTypeId result, SolMirPlanEffectRowId effects) {
    return item->callable == callable && item->receiver == receiver
        && item->parameter_count == parameter_count && item->result == result
        && item->effects == effects
        && (parameter_count == 0 || (memcmp(item->parameters, parameters,
            parameter_count * sizeof(*parameters)) == 0
            && memcmp(item->accesses, accesses,
                parameter_count * sizeof(*accesses)) == 0));
}

static bool add_import(Environment *environment, SolIrCallableId callable,
    SolMirPlanTypeId receiver, SolMirPlanEffectRowId effects,
    SolMirPlanImportId *result) {
    Builder *builder = environment->builder;
    if (callable >= builder->ir->callable_count) return false;
    const SolIrCallable *metadata = &builder->ir->callables[callable];
    bool approved = false;
    for (size_t index = 0; index < builder->program->import_count; ++index) {
        approved = approved || builder->program->imports[index].callable == callable;
    }
    if (!approved || metadata->kind != SOL_IR_CALLABLE_CAPABILITY
        || metadata->body != SOL_IR_NONE || metadata->generic_parameters.count != 0
        || metadata->effect_parameters.count != 0) {
        return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
            "typed import is not an existing symbolic-program import");
    }
    RawInstance temporary;
    memset(&temporary, 0, sizeof(temporary));
    temporary.callable = callable;
    temporary.receiver = receiver;
    temporary.effect_tail = 0;
    temporary.effects = effects;
    Environment imported = {builder, &temporary, environment->instance_id,
        SOL_IR_NONE, NULL, 0, SOL_MIR_PLAN_NONE, environment->context};
    size_t count = metadata->parameters.count;
    SolMirPlanTypeId *parameters = allocate(count, sizeof(*parameters));
    SolAccessMode *accesses = allocate(count, sizeof(*accesses));
    if (count != 0 && (parameters == NULL || accesses == NULL)) {
        free(parameters); free(accesses); return false;
    }
    for (size_t index = 0; index < count; ++index) {
        SolIrLocalId local = builder->ir->roots[metadata->parameters.offset + index];
        accesses[index] = builder->ir->locals[local].access;
        if (!substitute_type(&imported, builder->ir->locals[local].type, 1,
                &parameters[index])) {
            free(parameters); free(accesses); return false;
        }
    }
    SolMirPlanTypeId result_type;
    if (!substitute_type(&imported, metadata->result, 1, &result_type)) {
        free(parameters); free(accesses); return false;
    }
    for (size_t index = 0; index < builder->import_count; ++index) {
        if (import_key_equal(&builder->imports[index], callable, receiver,
                parameters, accesses, count, result_type, effects)) {
            free(parameters); free(accesses); *result = index; return true;
        }
    }
    if (!grow((void **)&builder->imports, &builder->import_capacity,
            builder->import_count + 1, sizeof(*builder->imports))) {
        free(parameters); free(accesses); return false;
    }
    builder->imports[builder->import_count] = (RawImport){callable, receiver,
        parameters, accesses, count, result_type, effects};
    *result = builder->import_count++;
    return true;
}

static bool program_has_invoke_reference(const Builder *builder,
    SolIrCallableId parent, SolIrExpressionId expression,
    SolIrCallableId target) {
    for (size_t index = 0; index < builder->program->reference_count; ++index) {
        const SolMirProgramReference *reference
            = &builder->program->references[index];
        if (reference->kind == SOL_MIR_PROGRAM_REFERENCE_INVOKE
            && reference->source.callable == parent
            && reference->source.expression == expression
            && reference->target == target) return true;
    }
    return false;
}

static bool plan_target(Environment *environment, SolMirPlanDemandKind kind,
    SolIrExpressionId expression_id, SolSpan span, SolIrCallKind call_kind,
    SolIrCallableId symbolic_target, SolIrSlice type_arguments,
    SolIrSlice evidence, SolIrExpressionId receiver_expression,
    SolMirPlanInstanceId *child_result, SolMirPlanImportId *import_result,
    SolIrDefinitionId *dispatch_trait, SolIrCallableId *dispatch_requirement) {
    Builder *builder = environment->builder;
    *child_result = SOL_MIR_PLAN_NONE;
    *import_result = SOL_MIR_PLAN_NONE;
    *dispatch_trait = SOL_IR_NONE;
    *dispatch_requirement = SOL_IR_NONE;
    SolIrCallableId target = symbolic_target;
    SolMirPlanTypeId receiver = SOL_MIR_PLAN_NONE;
    RawDictionary selected_method;
    memset(&selected_method, 0, sizeof(selected_method));
    if (call_kind == SOL_IR_CALL_METHOD) {
        if (evidence.count != 1
            || !resolve_evidence(environment,
                &builder->ir->evidence[evidence.offset], &selected_method)) {
            return false;
        }
        target = selected_method.method;
        receiver = selected_method.type;
        *dispatch_trait = selected_method.trait;
        *dispatch_requirement = selected_method.requirement;
    } else if (receiver_expression != SOL_IR_NONE) {
        if (receiver_expression >= builder->ir->expression_count
            || !substitute_type(environment,
                builder->ir->expressions[receiver_expression].type, 1,
                &receiver)) return false;
    }
    if (target >= builder->ir->callable_count) {
        return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
            "demand has no statically known callable target");
    }
    const SolIrCallable *callee = &builder->ir->callables[target];
    SolMirPlanEffectRowId effect_tail;
    SolMirPlanEffectRowId closed_effects;
    const SolIrExpression *source_call = expression_id < builder->ir->expression_count
        ? &builder->ir->expressions[expression_id] : NULL;
    if (!tail_for_call(environment, target, source_call, &effect_tail)
        || !row_for_callable(builder, target, effect_tail,
            &closed_effects)) return false;
    if (callee->body == SOL_IR_NONE && callee->kind == SOL_IR_CALLABLE_CAPABILITY) {
        return add_import(environment, target, receiver, closed_effects, import_result);
    }
    size_t count = callee->generic_parameters.count;
    if (type_arguments.count != count) {
        return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
            "demand has an incomplete concrete type argument set");
    }
    SolMirPlanTypeId *arguments = allocate(count, sizeof(*arguments));
    if (count != 0 && arguments == NULL) return false;
    for (size_t index = 0; index < count; ++index) {
        if (!substitute_type(environment,
                builder->ir->type_ids[type_arguments.offset + index], 1,
                &arguments[index])) {
            free(arguments); return false;
        }
    }
    RawDictionary *dictionary = NULL;
    size_t dictionary_count = 0;
    if (call_kind != SOL_IR_CALL_METHOD
        && !build_dictionary(environment, target, evidence, arguments,
            &dictionary, &dictionary_count)) {
        free(arguments); return false;
    }
    bool ok = add_instance(builder, target, receiver, arguments, count,
        dictionary, dictionary_count, effect_tail, closed_effects,
        environment->instance_id, child_result);
    free(arguments);
    free(dictionary);
    (void)kind;
    (void)expression_id;
    (void)span;
    return ok;
}

static const SolMirProgramReference *find_program_reference(
    const Builder *builder, SolMirProgramReferenceKind kind,
    SolIrCallableId parent, SolIrExpressionId expression,
    SolIrCallableId target) {
    for (size_t index = 0; index < builder->program->reference_count; ++index) {
        const SolMirProgramReference *item
            = &builder->program->references[index];
        if (item->kind == kind && item->source.callable == parent
            && item->source.expression == expression
            && item->target == target) return item;
    }
    return NULL;
}

static bool plan_callable_value(Environment *environment,
    SolIrExpressionId expression_id, bool predicate, bool bound_operation) {
    Builder *builder = environment->builder;
    const SolIrExpression *expression = &builder->ir->expressions[expression_id];
    SolIrCallableId target;
    SolIrExpressionId receiver_expression = SOL_IR_NONE;
    SolMirProgramReferenceKind reference_kind;
    SolMirPlanDemandKind demand_kind;
    if (bound_operation) {
        target = expression->as.operation.callable;
        receiver_expression = expression->as.operation.receiver;
        reference_kind = SOL_MIR_PROGRAM_REFERENCE_BOUND_OPERATION;
        demand_kind = SOL_MIR_PLAN_DEMAND_BOUND_OPERATION;
    } else {
        if (expression->as.definition >= builder->ir->definition_count) return false;
        target = builder->ir->definitions[expression->as.definition].callable;
        reference_kind = predicate
            ? SOL_MIR_PROGRAM_REFERENCE_PREDICATE_FUNCTION_VALUE
            : SOL_MIR_PROGRAM_REFERENCE_FUNCTION_VALUE;
        demand_kind = predicate
            ? SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE
            : SOL_MIR_PLAN_DEMAND_FUNCTION_VALUE;
    }
    const SolMirProgramReference *reference = find_program_reference(builder,
        reference_kind, environment->instance->callable, expression_id, target);
    if (reference == NULL) return true;
    if (target >= builder->ir->callable_count) return false;
    const SolIrCallable *callable = &builder->ir->callables[target];
    if (callable->generic_parameters.count != 0
        || callable->effect_parameters.count != 0
        || callable->effect_parameter != SOL_IR_NONE) {
        return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
            "generic callable value has no closed handle context");
    }
    SolMirPlanTypeId receiver = SOL_MIR_PLAN_NONE;
    if (receiver_expression != SOL_IR_NONE
        && !substitute_type(environment,
            builder->ir->expressions[receiver_expression].type, 1,
            &receiver)) return false;
    size_t no_atoms = 0;
    SolMirPlanEffectRowId empty;
    SolMirPlanEffectRowId effects;
    if (!intern_row(builder, &no_atoms, 0, &empty)
        || !row_for_callable(builder, target, empty, &effects)) return false;
    SolMirPlanInstanceId child = SOL_MIR_PLAN_NONE;
    SolMirPlanImportId imported = SOL_MIR_PLAN_NONE;
    if (callable->body == SOL_IR_NONE
        && callable->kind == SOL_IR_CALLABLE_CAPABILITY) {
        if (!add_import(environment, target, receiver, effects, &imported)) {
            return false;
        }
    } else if (!add_instance(builder, target, receiver, NULL, 0, NULL, 0,
            empty, effects, environment->instance_id, &child)) {
        return false;
    }
    SolMirPlanDemand demand;
    memset(&demand, 0, sizeof(demand));
    demand.kind = demand_kind;
    demand.parent = environment->instance_id;
    demand.source = reference->source;
    demand.symbolic_target = target;
    demand.instance = child;
    demand.import = imported;
    demand.dispatch_trait = SOL_IR_NONE;
    demand.dispatch_requirement = SOL_IR_NONE;
    demand.context = environment->context;
    return add_demand(builder, demand);
}

static bool plan_call(Environment *environment, SolMirPlanDemandKind kind,
    SolIrExpressionId expression_id, SolSpan span,
    SolIrCallableId static_override) {
    Builder *builder = environment->builder;
    if (expression_id >= builder->ir->expression_count) return false;
    const SolIrExpression *expression = &builder->ir->expressions[expression_id];
    if (expression->kind != SOL_IR_EXPR_CALL) {
        return fail(builder, SOL_MIR_PLAN_BUILD_INVALID_PROGRAM,
            "MIR invoke source is not an owning-IR call expression");
    }
    SolIrCallKind call_kind = expression->as.call.kind;
    SolIrCallableId target = static_override != SOL_IR_NONE
        ? static_override : expression->as.call.callable;
    SolIrExpressionId receiver_expression = expression->as.call.receiver;
    if (call_kind == SOL_IR_CALL_CAPABILITY
        && expression->as.call.callee < builder->ir->expression_count) {
        const SolIrExpression *operation
            = &builder->ir->expressions[expression->as.call.callee];
        if (operation->kind == SOL_IR_EXPR_BOUND_OPERATION) {
            receiver_expression = operation->as.operation.receiver;
        }
    }
    SolMirPlanInstanceId child;
    SolMirPlanImportId imported;
    SolIrDefinitionId trait;
    SolIrCallableId requirement;
    if (!plan_target(environment, kind, expression_id, span, call_kind, target,
            expression->as.call.type_arguments, expression->as.call.evidence,
            receiver_expression, &child, &imported, &trait, &requirement)) return false;
    SolMirPlanDemand demand;
    memset(&demand, 0, sizeof(demand));
    demand.kind = kind;
    demand.parent = environment->instance_id;
    demand.symbolic_target = target;
    demand.instance = child;
    demand.import = imported;
    demand.dispatch_trait = trait;
    demand.dispatch_requirement = requirement;
    demand.context = environment->context;
    if (!source_for(builder, environment->instance->callable, expression_id,
            span, &demand.source)) return false;
    return add_demand(builder, demand);
}

static bool scan_expression(Environment *environment, SolIrExpressionId id,
    bool executable_predicate, size_t depth);

static bool scan_pattern(Environment *environment, SolIrPatternId id,
    size_t depth) {
    Builder *builder = environment->builder;
    if (id == SOL_IR_NONE) return true;
    if (id >= builder->ir->pattern_count || depth > builder->ir->pattern_count) {
        return false;
    }
    const SolIrPattern *pattern = &builder->ir->patterns[id];
    if (!add_use(environment, SOL_MIR_PLAN_USE_PATTERN, id, 0, pattern->type,
            SOL_ACCESS_OWNED)
        || !add_local_use(environment, pattern->binding)) return false;
    for (size_t index = 0; index < pattern->children.count; ++index) {
        if (!scan_pattern(environment, builder->ir->pattern_children[
                pattern->children.offset + index].pattern, depth + 1)) return false;
    }
    return true;
}

static bool scan_statement(Environment *environment, SolIrStatementId id,
    bool executable_predicate, size_t depth) {
    Builder *builder = environment->builder;
    if (id >= builder->ir->statement_count) return false;
    const SolIrStatement *statement = &builder->ir->statements[id];
    if (!add_local_use(environment, statement->local)) return false;
    if (statement->target != SOL_IR_NONE
        && !scan_expression(environment, statement->target,
            executable_predicate, depth + 1)) return false;
    if (statement->expression != SOL_IR_NONE
        && !scan_expression(environment, statement->expression,
            executable_predicate, depth + 1)) return false;
    if (statement->condition != SOL_IR_NONE
        && !scan_expression(environment, statement->condition,
            executable_predicate, depth + 1)) return false;
    return true;
}

static bool add_handler_demand(Environment *environment,
    SolIrExpressionId expression_id, SolIrCallableId target,
    SolIrExpressionId receiver_expression, SolMirPlanDemandKind kind) {
    Builder *builder = environment->builder;
    SolMirPlanInstanceId child;
    SolMirPlanImportId imported;
    SolIrDefinitionId trait;
    SolIrCallableId requirement;
    if (!plan_target(environment, kind, expression_id,
            builder->ir->expressions[expression_id].span,
            SOL_IR_CALL_CAPABILITY, target, (SolIrSlice){0},
            (SolIrSlice){0}, receiver_expression, &child, &imported,
            &trait, &requirement)) return false;
    SolMirPlanDemand demand;
    memset(&demand, 0, sizeof(demand));
    demand.kind = kind;
    demand.parent = environment->instance_id;
    demand.symbolic_target = target;
    demand.instance = child;
    demand.import = imported;
    demand.dispatch_trait = trait;
    demand.dispatch_requirement = requirement;
    demand.context = environment->context;
    if (!source_for(builder, environment->instance->callable, expression_id,
            builder->ir->expressions[expression_id].span, &demand.source)) return false;
    return add_demand(builder, demand);
}

static bool scan_expression(Environment *environment, SolIrExpressionId id,
    bool executable_predicate, size_t depth) {
    Builder *builder = environment->builder;
    const SolIr *ir = builder->ir;
    if (id == SOL_IR_NONE) return true;
    if (id >= ir->expression_count || depth > ir->expression_count) return false;
    const SolIrExpression *expression = &ir->expressions[id];
    if (expression->kind != SOL_IR_EXPR_COMPILE_TIME_HEAD
        && !add_use(environment, SOL_MIR_PLAN_USE_EXPRESSION, id, 0,
            expression->type, SOL_ACCESS_OWNED)) return false;
#define SCAN(child) do { if (!scan_expression(environment, (child), \
        executable_predicate, depth + 1)) return false; } while (0)
    switch (expression->kind) {
        case SOL_IR_EXPR_DEFINITION:
            if (expression->as.definition < ir->definition_count
                && ir->definitions[expression->as.definition].callable
                    != SOL_IR_NONE
                && !plan_callable_value(environment, id,
                    executable_predicate, false)) return false;
            break;
        case SOL_IR_EXPR_PLACE:
            if (!add_place_uses(environment, expression->as.place)) return false;
            break;
        case SOL_IR_EXPR_UNARY: SCAN(expression->as.unary.operand); break;
        case SOL_IR_EXPR_BINARY:
            SCAN(expression->as.binary.left);
            SCAN(expression->as.binary.right);
            break;
        case SOL_IR_EXPR_BOUND_OPERATION:
            if (!plan_callable_value(environment, id,
                    executable_predicate, true)) return false;
            SCAN(expression->as.operation.receiver);
            break;
        case SOL_IR_EXPR_CALL: {
            if (executable_predicate && expression->as.call.kind
                    <= SOL_IR_CALL_METHOD) {
                SolIrCallableId override = SOL_IR_NONE;
                SolMirPlanDemandKind kind = SOL_MIR_PLAN_DEMAND_PREDICATE;
                if (expression->as.call.kind == SOL_IR_CALL_CALLBACK) {
                    if (expression->as.call.callee >= ir->expression_count) return false;
                    const SolIrExpression *callee
                        = &ir->expressions[expression->as.call.callee];
                    if (callee->kind == SOL_IR_EXPR_DEFINITION
                        && callee->as.definition < ir->definition_count) {
                        override = ir->definitions[callee->as.definition].callable;
                    } else if (callee->kind == SOL_IR_EXPR_BOUND_OPERATION) {
                        override = callee->as.operation.callable;
                    } else {
                        return fail(builder,
                            SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
                            "predicate callback has no exact static producer");
                    }
                    kind = SOL_MIR_PLAN_DEMAND_CALLBACK;
                }
                if (!plan_call(environment, kind, id, expression->span,
                        override)) return false;
            }
            if (expression->as.call.callee != SOL_IR_NONE
                && (expression->as.call.kind == SOL_IR_CALL_CALLBACK
                    || expression->as.call.kind == SOL_IR_CALL_CAPABILITY)) {
                SCAN(expression->as.call.callee);
            }
            if (expression->as.call.receiver != SOL_IR_NONE) {
                SCAN(expression->as.call.receiver);
            }
            for (size_t index = 0; index < expression->as.call.operands.count;
                ++index) {
                const SolIrOperand *operand
                    = &ir->operands[expression->as.call.operands.offset + index];
                if (!add_use(environment, SOL_MIR_PLAN_USE_SOURCE_OPERAND,
                        expression->as.call.operands.offset + index,
                        operand->formal, ir->expressions[operand->value].type,
                        operand->access)) return false;
                SCAN(operand->value);
            }
            break;
        }
        case SOL_IR_EXPR_RECORD:
            for (size_t index = 0; index < expression->as.record.fields.count;
                ++index) {
                const SolIrOperand *operand
                    = &ir->operands[expression->as.record.fields.offset + index];
                if (!add_use(environment, SOL_MIR_PLAN_USE_SOURCE_OPERAND,
                        expression->as.record.fields.offset + index,
                        operand->formal, ir->expressions[operand->value].type,
                        operand->access)) return false;
                SCAN(operand->value);
            }
            break;
        case SOL_IR_EXPR_TUPLE:
            for (size_t index = 0; index < expression->as.tuple.operands.count;
                ++index) {
                const SolIrOperand *operand
                    = &ir->operands[expression->as.tuple.operands.offset + index];
                if (!add_use(environment, SOL_MIR_PLAN_USE_SOURCE_OPERAND,
                        expression->as.tuple.operands.offset + index,
                        operand->formal, ir->expressions[operand->value].type,
                        operand->access)) return false;
                SCAN(operand->value);
            }
            break;
        case SOL_IR_EXPR_IF:
            SCAN(expression->as.if_expr.condition);
            SCAN(expression->as.if_expr.then_branch);
            SCAN(expression->as.if_expr.else_branch);
            break;
        case SOL_IR_EXPR_MATCH:
            SCAN(expression->as.match_expr.scrutinee);
            for (size_t index = 0; index < expression->as.match_expr.arms.count;
                ++index) {
                SolIrArmId arm_id
                    = ir->arm_ids[expression->as.match_expr.arms.offset + index];
                const SolIrArm *arm = &ir->arms[arm_id];
                if (!scan_pattern(environment, arm->pattern, depth + 1)) return false;
                for (size_t local = 0; local < arm->bindings.count; ++local) {
                    if (!add_local_use(environment,
                            ir->roots[arm->bindings.offset + local])) return false;
                }
                if (arm->guard != SOL_IR_NONE) SCAN(arm->guard);
                SCAN(arm->body);
            }
            break;
        case SOL_IR_EXPR_BLOCK:
            for (size_t index = 0; index < expression->as.block.statements.count;
                ++index) {
                if (!scan_statement(environment, ir->statement_ids[
                        expression->as.block.statements.offset + index],
                        executable_predicate, depth + 1)) return false;
            }
            break;
        case SOL_IR_EXPR_PROPAGATE: SCAN(expression->as.propagate.operand); break;
        case SOL_IR_EXPR_HANDLE:
            if (!add_handler_demand(environment, id, expression->as.handler.source,
                    expression->as.handler.authority,
                    SOL_MIR_PLAN_DEMAND_HANDLER_SOURCE)
                || !add_handler_demand(environment, id,
                    expression->as.handler.provider_callable,
                    expression->as.handler.provider,
                    SOL_MIR_PLAN_DEMAND_HANDLER_PROVIDER)) return false;
            SCAN(expression->as.handler.authority);
            SCAN(expression->as.handler.provider);
            SCAN(expression->as.handler.body);
            break;
        default: break;
    }
#undef SCAN
    return true;
}

static bool scan_mir_place(Environment *environment, SolMirPlace place) {
    return add_local_use(environment, place.local)
        && add_place_uses(environment, place.source_place);
}

static bool scan_instruction_places(Environment *environment,
    const SolMirInstruction *instruction) {
    switch (instruction->kind) {
        case SOL_MIR_INST_LOAD_COPY:
        case SOL_MIR_INST_LOAD_MOVE:
        case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
            return scan_mir_place(environment, instruction->as.place);
        case SOL_MIR_INST_LOAD_UPDATE:
            return scan_mir_place(environment, instruction->as.update_load.place);
        case SOL_MIR_INST_STORE:
            return scan_mir_place(environment, instruction->as.store.place);
        case SOL_MIR_INST_COMPOUND_UPDATE:
            return scan_mir_place(environment,
                instruction->as.compound_update.place);
        case SOL_MIR_INST_PARAMETER_LIVE:
        case SOL_MIR_INST_STORAGE_LIVE:
        case SOL_MIR_INST_DROP_IF_INITIALIZED:
        case SOL_MIR_INST_STORAGE_DEAD:
            return add_local_use(environment, instruction->as.local);
        default: return true;
    }
}

static bool invoke_authentic(const Builder *builder, SolIrCallableId parent,
    const SolMirTerminator *term) {
    if (term->as.invoke.source_expression >= builder->ir->expression_count) return false;
    const SolIrExpression *source
        = &builder->ir->expressions[term->as.invoke.source_expression];
    if (source->kind != SOL_IR_EXPR_CALL || source->as.call.kind != term->as.invoke.kind
        || source->as.call.callable != term->as.invoke.callable
        || source->as.call.type_arguments.offset != term->as.invoke.type_arguments.offset
        || source->as.call.type_arguments.count != term->as.invoke.type_arguments.count
        || source->as.call.effects.offset != term->as.invoke.effects.offset
        || source->as.call.effects.count != term->as.invoke.effects.count
        || source->as.call.effect_parameter != term->as.invoke.effect_parameter
        || source->as.call.evidence.offset != term->as.invoke.evidence.offset
        || source->as.call.evidence.count != term->as.invoke.evidence.count) return false;
    return program_has_invoke_reference(builder, parent,
        term->as.invoke.source_expression, term->as.invoke.callable);
}

static bool scan_predicate_obligation(Environment *environment,
    const SolIrObligation *obligation, Environment *predicate_environment) {
    if (!add_use(predicate_environment, SOL_MIR_PLAN_USE_OBLIGATION_PREDICATE,
            obligation->id, 0,
            predicate_environment->builder->ir->expressions[
                obligation->predicate].type, SOL_ACCESS_OWNED)) return false;
    if (obligation->result_available
        && !add_use(predicate_environment, SOL_MIR_PLAN_USE_OBLIGATION_RESULT,
            obligation->id, 0, obligation->result_type,
            SOL_ACCESS_OWNED)) return false;
    for (size_t index = 0; index < obligation->snapshots.count; ++index) {
        SolIrSnapshotId snapshot_id
            = obligation->snapshots.offset + index;
        const SolIrSnapshot *snapshot
            = &environment->builder->ir->snapshots[snapshot_id];
        if (!add_use(predicate_environment, SOL_MIR_PLAN_USE_SNAPSHOT,
                snapshot_id, 0, snapshot->type, SOL_ACCESS_OWNED)
            || !scan_expression(predicate_environment, snapshot->operand,
                false, 0)) return false;
    }
    return scan_expression(predicate_environment, obligation->predicate, true, 0);
}

static bool scan_instance(Builder *builder, SolMirPlanInstanceId instance_id) {
    RawInstance *instance = &builder->instances[instance_id];
    if (instance->scanned || instance->active) return true;
    instance->active = true;
    const SolMirProgramTemplate *template = find_template(builder, instance->callable);
    if (template == NULL) return false;
    const SolMir *mir = &template->mir;
    const SolIrCallable *callable = &builder->ir->callables[instance->callable];
    Environment environment = {builder, instance, instance_id, SOL_IR_NONE,
        NULL, 0, SOL_MIR_PLAN_NONE, 0};
    if (callable->receiver != SOL_IR_NONE) {
        if (!add_use(&environment, SOL_MIR_PLAN_USE_RECEIVER,
                callable->receiver, 0, builder->ir->locals[callable->receiver].type,
                callable->receiver_access)) return false;
    } else if (instance->receiver != SOL_MIR_PLAN_NONE) {
        return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
            "receiver type is present on a receiverless callable");
    }
    for (size_t index = 0; index < callable->parameters.count; ++index) {
        SolIrLocalId local
            = builder->ir->roots[callable->parameters.offset + index];
        if (!add_use(&environment, SOL_MIR_PLAN_USE_PARAMETER, local, index,
                builder->ir->locals[local].type,
                builder->ir->locals[local].access)
            || !add_local_use(&environment, local)) return false;
    }
    if (!add_use(&environment, SOL_MIR_PLAN_USE_RESULT, instance->callable, 0,
            callable->result, SOL_ACCESS_OWNED)) return false;
    for (size_t value = 0; value < mir->value_count; ++value) {
        if (!add_use(&environment, SOL_MIR_PLAN_USE_MIR_VALUE, value, 0,
                mir->values[value].type, SOL_ACCESS_OWNED)) return false;
    }
    for (size_t instruction = 0; instruction < mir->instruction_count;
        ++instruction) {
        const SolMirInstruction *item = &mir->instructions[instruction];
        if (item->type != SOL_IR_NONE
            && !add_use(&environment, SOL_MIR_PLAN_USE_MIR_INSTRUCTION,
                instruction, 0, item->type, SOL_ACCESS_OWNED)) return false;
        if (!scan_instruction_places(&environment, item)) return false;
    }
    for (size_t temporary = 0; temporary < mir->temporary_count; ++temporary) {
        if (!add_use(&environment, SOL_MIR_PLAN_USE_MIR_TEMPORARY, temporary, 0,
                mir->temporaries[temporary].type, SOL_ACCESS_OWNED)) return false;
    }
    for (size_t operand = 0; operand < mir->construct_operand_count; ++operand) {
        const SolMirConstructOperand *item = &mir->construct_operands[operand];
        if (!add_use(&environment, SOL_MIR_PLAN_USE_CONSTRUCT_OPERAND, operand,
                item->formal, builder->ir->expressions[item->source_expression].type,
                SOL_ACCESS_OWNED)) return false;
    }
    if (!scan_expression(&environment, callable->body, false, 0)) return false;
    for (size_t block = 0; block < mir->block_count; ++block) {
        const SolMirTerminator *term = &mir->blocks[block].terminator;
        if (term->kind != SOL_MIR_TERM_INVOKE) continue;
        if (!invoke_authentic(builder, instance->callable, term)) {
            return fail(builder, SOL_MIR_PLAN_BUILD_INVALID_PROGRAM,
                "instance invoke is not an authentic P2.1 source reference");
        }
        SolIrCallableId override = SOL_IR_NONE;
        SolMirPlanDemandKind kind = SOL_MIR_PLAN_DEMAND_INVOKE;
        if (term->as.invoke.kind == SOL_IR_CALL_CALLBACK) {
            if (term->as.invoke.callee >= mir->temporary_count) return false;
            SolIrExpressionId producer
                = mir->temporaries[term->as.invoke.callee].source_expression;
            if (producer >= builder->ir->expression_count) return false;
            const SolIrExpression *source = &builder->ir->expressions[producer];
            if (source->kind == SOL_IR_EXPR_DEFINITION
                && source->as.definition < builder->ir->definition_count) {
                override = builder->ir->definitions[source->as.definition].callable;
            } else if (source->kind == SOL_IR_EXPR_BOUND_OPERATION) {
                override = source->as.operation.callable;
            } else {
                return fail(builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
                    "callback invoke has no exact static producer");
            }
            kind = SOL_MIR_PLAN_DEMAND_CALLBACK;
        }
        if (!plan_call(&environment, kind, term->as.invoke.source_expression,
                term->span, override)) return false;
        instance = &builder->instances[instance_id];
        environment.instance = instance;
    }
    for (size_t obligation = 0; obligation < builder->ir->obligation_count;
        ++obligation) {
        const SolIrObligation *item = &builder->ir->obligations[obligation];
        if (item->owner_kind == SOL_CONTRACT_OWNER_ITEM
            && item->owner == callable->owner
            && !scan_predicate_obligation(&environment, item,
                &environment)) return false;
    }
    for (size_t block = 0; block < mir->block_count; ++block) {
        const SolMirTerminator *term = &mir->blocks[block].terminator;
        if (term->kind != SOL_MIR_TERM_CHECK_REFINED) continue;
        SolMirPlanTypeId nominal_type;
        if (!substitute_type(&environment,
                builder->ir->expressions[term->as.check_refined.source_expression].type,
                1, &nominal_type)) return false;
        RawType *nominal = &builder->types[nominal_type];
        Environment predicate = environment;
        predicate.nominal = term->as.check_refined.definition;
        predicate.nominal_arguments = nominal->arguments;
        predicate.nominal_argument_count = nominal->argument_count;
        predicate.self_type = nominal_type;
        predicate.context = block + 1;
        for (size_t obligation = 0; obligation < builder->ir->obligation_count;
            ++obligation) {
            const SolIrObligation *item = &builder->ir->obligations[obligation];
            if (item->owner_kind == SOL_CONTRACT_OWNER_TYPE
                && item->owner == term->as.check_refined.definition
                && !scan_predicate_obligation(&environment, item,
                    &predicate)) return false;
        }
    }
    for (size_t index = 0; index < builder->ir->loop_obligation_count; ++index) {
        const SolIrLoopObligation *item = &builder->ir->loop_obligations[index];
        if (item->callable == instance->callable) {
            if (!add_use(&environment, SOL_MIR_PLAN_USE_LOOP_OBLIGATION,
                    item->id, 0, item->expression_type, SOL_ACCESS_OWNED)
                || !scan_expression(&environment, item->expression, false, 0)) return false;
        }
    }
    for (size_t index = 0; index < builder->ir->unreachable_obligation_count;
        ++index) {
        const SolIrUnreachableObligation *item
            = &builder->ir->unreachable_obligations[index];
        if (item->callable == instance->callable) {
            if (!add_use(&environment, SOL_MIR_PLAN_USE_UNREACHABLE_PROOF,
                    item->id, 0, item->proof_type, SOL_ACCESS_OWNED)
                || !scan_expression(&environment, item->proof, false, 0)) return false;
        }
    }
    instance = &builder->instances[instance_id];
    instance->active = false;
    instance->scanned = true;
    return true;
}

static void free_builder(Builder *builder, bool atoms_published) {
    for (size_t index = 0; index < builder->type_count; ++index) {
        free(builder->types[index].arguments);
        free(builder->types[index].parameters);
        free(builder->types[index].accesses);
    }
    for (size_t index = 0; index < builder->row_count; ++index) {
        free(builder->rows[index].atoms);
    }
    for (size_t index = 0; index < builder->instance_count; ++index) {
        free(builder->instances[index].arguments);
        free(builder->instances[index].dictionary);
        free(builder->instances[index].uses);
    }
    for (size_t index = 0; index < builder->import_count; ++index) {
        free(builder->imports[index].parameters);
        free(builder->imports[index].accesses);
    }
    if (!atoms_published) {
        for (size_t index = 0; index < builder->atom_count; ++index) {
            free(builder->atoms[index].name);
        }
    }
    free(builder->types);
    free(builder->atoms);
    free(builder->rows);
    free(builder->instances);
    free(builder->imports);
    free(builder->demands);
}

static int compare_atom_id(const Builder *builder, size_t left, size_t right) {
    return atom_compare_value(&builder->atoms[left], &builder->atoms[right]);
}

static int compare_row_id(const Builder *builder, size_t left, size_t right) {
    const RawRow *a = &builder->rows[left];
    const RawRow *b = &builder->rows[right];
    size_t count = a->count < b->count ? a->count : b->count;
    for (size_t index = 0; index < count; ++index) {
        int atom = compare_atom_id(builder, a->atoms[index], b->atoms[index]);
        if (atom != 0) return atom;
    }
    return a->count < b->count ? -1 : a->count > b->count;
}

static int compare_type_id(const Builder *builder, size_t left, size_t right,
    size_t depth) {
    if (left == right) return 0;
    if (depth > builder->type_count) return left < right ? -1 : 1;
    const RawType *a = &builder->types[left];
    const RawType *b = &builder->types[right];
#define CMP(field) if (a->field != b->field) return a->field < b->field ? -1 : 1
    CMP(kind); CMP(definition); CMP(argument_count);
#undef CMP
    for (size_t index = 0; index < a->argument_count; ++index) {
        int child = compare_type_id(builder, a->arguments[index],
            b->arguments[index], depth + 1);
        if (child != 0) return child;
    }
    if (a->parameter_count != b->parameter_count) {
        return a->parameter_count < b->parameter_count ? -1 : 1;
    }
    for (size_t index = 0; index < a->parameter_count; ++index) {
        if (a->accesses[index] != b->accesses[index]) {
            return a->accesses[index] < b->accesses[index] ? -1 : 1;
        }
        int child = compare_type_id(builder, a->parameters[index],
            b->parameters[index], depth + 1);
        if (child != 0) return child;
    }
    if (a->result != b->result) {
        if (a->result == SOL_MIR_PLAN_NONE) return -1;
        if (b->result == SOL_MIR_PLAN_NONE) return 1;
        int child = compare_type_id(builder, a->result, b->result, depth + 1);
        if (child != 0) return child;
    }
    if (a->effects == b->effects) return 0;
    if (a->effects == SOL_MIR_PLAN_NONE) return -1;
    if (b->effects == SOL_MIR_PLAN_NONE) return 1;
    return compare_row_id(builder, a->effects, b->effects);
}

static bool canonicalize_atoms(Builder *builder) {
    size_t *order = allocate(builder->atom_count, sizeof(*order));
    size_t *remap = allocate(builder->atom_count, sizeof(*remap));
    SolMirPlanEffectAtom *sorted
        = allocate(builder->atom_count, sizeof(*sorted));
    if (builder->atom_count != 0
        && (order == NULL || remap == NULL || sorted == NULL)) {
        free(order); free(remap); free(sorted); return false;
    }
    for (size_t index = 0; index < builder->atom_count; ++index) order[index] = index;
    for (size_t index = 1; index < builder->atom_count; ++index) {
        size_t value = order[index];
        size_t at = index;
        while (at != 0 && compare_atom_id(builder, value, order[at - 1]) < 0) {
            order[at] = order[at - 1]; --at;
        }
        order[at] = value;
    }
    for (size_t index = 0; index < builder->atom_count; ++index) {
        sorted[index] = builder->atoms[order[index]];
        remap[order[index]] = index;
    }
    for (size_t row = 0; row < builder->row_count; ++row) {
        for (size_t atom = 0; atom < builder->rows[row].count; ++atom) {
            builder->rows[row].atoms[atom]
                = remap[builder->rows[row].atoms[atom]];
        }
        if (builder->rows[row].count > 1) qsort(builder->rows[row].atoms,
            builder->rows[row].count, sizeof(*builder->rows[row].atoms), size_compare);
    }
    free(builder->atoms);
    builder->atoms = sorted;
    builder->atom_capacity = builder->atom_count;
    free(order); free(remap);
    return true;
}

static bool canonicalize_rows(Builder *builder) {
    size_t count = builder->row_count;
    size_t *order = allocate(count, sizeof(*order));
    size_t *remap = allocate(count, sizeof(*remap));
    RawRow *sorted = allocate(count, sizeof(*sorted));
    if (count != 0 && (order == NULL || remap == NULL || sorted == NULL)) {
        free(order); free(remap); free(sorted); return false;
    }
    for (size_t index = 0; index < count; ++index) order[index] = index;
    for (size_t index = 1; index < count; ++index) {
        size_t value = order[index];
        size_t at = index;
        while (at != 0 && compare_row_id(builder, value, order[at - 1]) < 0) {
            order[at] = order[at - 1]; --at;
        }
        order[at] = value;
    }
    for (size_t index = 0; index < count; ++index) {
        sorted[index] = builder->rows[order[index]];
        remap[order[index]] = index;
    }
    for (size_t type = 0; type < builder->type_count; ++type) {
        if (builder->types[type].effects != SOL_MIR_PLAN_NONE) {
            builder->types[type].effects = remap[builder->types[type].effects];
        }
    }
    for (size_t index = 0; index < builder->instance_count; ++index) {
        builder->instances[index].effect_tail
            = remap[builder->instances[index].effect_tail];
        builder->instances[index].effects = remap[builder->instances[index].effects];
    }
    for (size_t index = 0; index < builder->import_count; ++index) {
        builder->imports[index].effects = remap[builder->imports[index].effects];
    }
    free(builder->rows);
    builder->rows = sorted;
    builder->row_capacity = count;
    free(order); free(remap);
    return true;
}

static void remap_type_id(SolMirPlanTypeId *id, const size_t *remap) {
    if (*id != SOL_MIR_PLAN_NONE) *id = remap[*id];
}

static bool canonicalize_types(Builder *builder) {
    size_t count = builder->type_count;
    size_t *order = allocate(count, sizeof(*order));
    size_t *remap = allocate(count, sizeof(*remap));
    RawType *sorted = allocate(count, sizeof(*sorted));
    if (count != 0 && (order == NULL || remap == NULL || sorted == NULL)) {
        free(order); free(remap); free(sorted); return false;
    }
    for (size_t index = 0; index < count; ++index) order[index] = index;
    for (size_t index = 1; index < count; ++index) {
        size_t value = order[index];
        size_t at = index;
        while (at != 0
            && compare_type_id(builder, value, order[at - 1], 0) < 0) {
            order[at] = order[at - 1]; --at;
        }
        order[at] = value;
    }
    for (size_t index = 0; index < count; ++index) {
        sorted[index] = builder->types[order[index]];
        remap[order[index]] = index;
    }
    for (size_t index = 0; index < count; ++index) {
        RawType *type = &sorted[index];
        for (size_t child = 0; child < type->argument_count; ++child) {
            remap_type_id(&type->arguments[child], remap);
        }
        for (size_t child = 0; child < type->parameter_count; ++child) {
            remap_type_id(&type->parameters[child], remap);
        }
        remap_type_id(&type->result, remap);
    }
    for (size_t index = 0; index < builder->instance_count; ++index) {
        RawInstance *instance = &builder->instances[index];
        remap_type_id(&instance->receiver, remap);
        for (size_t child = 0; child < instance->argument_count; ++child) {
            remap_type_id(&instance->arguments[child], remap);
        }
        for (size_t entry = 0; entry < instance->dictionary_count; ++entry) {
            remap_type_id(&instance->dictionary[entry].type, remap);
        }
        for (size_t use = 0; use < instance->use_count; ++use) {
            remap_type_id(&instance->uses[use].type, remap);
        }
    }
    for (size_t index = 0; index < builder->import_count; ++index) {
        RawImport *item = &builder->imports[index];
        remap_type_id(&item->receiver, remap);
        for (size_t child = 0; child < item->parameter_count; ++child) {
            remap_type_id(&item->parameters[child], remap);
        }
        remap_type_id(&item->result, remap);
    }
    free(builder->types);
    builder->types = sorted;
    builder->type_capacity = count;
    free(order); free(remap);
    return true;
}

static int raw_instance_compare(const RawInstance *a, const RawInstance *b) {
#define CMP(field) if (a->field != b->field) return a->field < b->field ? -1 : 1
    CMP(callable); CMP(receiver); CMP(argument_count);
#undef CMP
    for (size_t index = 0; index < a->argument_count; ++index) {
        if (a->arguments[index] != b->arguments[index]) {
            return a->arguments[index] < b->arguments[index] ? -1 : 1;
        }
    }
    if (a->effect_tail != b->effect_tail) {
        return a->effect_tail < b->effect_tail ? -1 : 1;
    }
    if (a->effects != b->effects) return a->effects < b->effects ? -1 : 1;
    if (a->dictionary_count != b->dictionary_count) {
        return a->dictionary_count < b->dictionary_count ? -1 : 1;
    }
    for (size_t index = 0; index < a->dictionary_count; ++index) {
        int item = dictionary_compare(&a->dictionary[index], &b->dictionary[index]);
        if (item != 0) return item;
    }
    return 0;
}

static bool canonicalize_instances(Builder *builder) {
    size_t count = builder->instance_count;
    size_t *old = allocate(count, sizeof(*old));
    size_t *remap = allocate(count, sizeof(*remap));
    if (count != 0 && (old == NULL || remap == NULL)) {
        free(old); free(remap); return false;
    }
    for (size_t index = 0; index < count; ++index) old[index] = index;
    for (size_t index = 1; index < count; ++index) {
        RawInstance value = builder->instances[index];
        size_t old_value = old[index];
        size_t at = index;
        while (at != 0
            && raw_instance_compare(&value, &builder->instances[at - 1]) < 0) {
            builder->instances[at] = builder->instances[at - 1];
            old[at] = old[at - 1];
            --at;
        }
        builder->instances[at] = value;
        old[at] = old_value;
    }
    for (size_t index = 0; index < count; ++index) remap[old[index]] = index;
    for (size_t index = 0; index < builder->demand_count; ++index) {
        if (builder->demands[index].parent != SOL_MIR_PLAN_NONE) {
            builder->demands[index].parent = remap[builder->demands[index].parent];
        }
        if (builder->demands[index].instance != SOL_MIR_PLAN_NONE) {
            builder->demands[index].instance
                = remap[builder->demands[index].instance];
        }
    }
    free(old); free(remap);
    return true;
}

static int raw_import_compare(const RawImport *a, const RawImport *b) {
#define CMP(field) if (a->field != b->field) return a->field < b->field ? -1 : 1
    CMP(callable); CMP(receiver); CMP(parameter_count);
#undef CMP
    for (size_t index = 0; index < a->parameter_count; ++index) {
        if (a->parameters[index] != b->parameters[index]) {
            return a->parameters[index] < b->parameters[index] ? -1 : 1;
        }
        if (a->accesses[index] != b->accesses[index]) {
            return a->accesses[index] < b->accesses[index] ? -1 : 1;
        }
    }
    if (a->result != b->result) return a->result < b->result ? -1 : 1;
    return a->effects < b->effects ? -1 : a->effects > b->effects;
}

static bool canonicalize_imports(Builder *builder) {
    size_t count = builder->import_count;
    size_t *old = allocate(count, sizeof(*old));
    size_t *remap = allocate(count, sizeof(*remap));
    if (count != 0 && (old == NULL || remap == NULL)) {
        free(old); free(remap); return false;
    }
    for (size_t index = 0; index < count; ++index) old[index] = index;
    for (size_t index = 1; index < count; ++index) {
        RawImport value = builder->imports[index];
        size_t old_value = old[index];
        size_t at = index;
        while (at != 0 && raw_import_compare(&value,
                &builder->imports[at - 1]) < 0) {
            builder->imports[at] = builder->imports[at - 1];
            old[at] = old[at - 1]; --at;
        }
        builder->imports[at] = value;
        old[at] = old_value;
    }
    for (size_t index = 0; index < count; ++index) remap[old[index]] = index;
    for (size_t index = 0; index < builder->demand_count; ++index) {
        if (builder->demands[index].import != SOL_MIR_PLAN_NONE) {
            builder->demands[index].import = remap[builder->demands[index].import];
        }
    }
    free(old); free(remap);
    return true;
}

static int use_compare(const void *left, const void *right) {
    const SolMirPlanTypedUse *a = left;
    const SolMirPlanTypedUse *b = right;
#define CMP(field) if (a->field != b->field) return a->field < b->field ? -1 : 1
    CMP(kind); CMP(source); CMP(ordinal); CMP(context); CMP(type); CMP(access);
#undef CMP
    return 0;
}

static int source_compare(SolMirProgramSource a, SolMirProgramSource b) {
#define CMP(field) if (a.field != b.field) return a.field < b.field ? -1 : 1
    CMP(callable); CMP(expression); CMP(file); CMP(start); CMP(end);
#undef CMP
    return 0;
}

static int demand_compare(const void *left, const void *right) {
    const SolMirPlanDemand *a = left;
    const SolMirPlanDemand *b = right;
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    if (a->parent != b->parent) return a->parent < b->parent ? -1 : 1;
    int source = source_compare(a->source, b->source);
    if (source != 0) return source;
#define CMP(field) if (a->field != b->field) return a->field < b->field ? -1 : 1
    CMP(symbolic_target); CMP(instance); CMP(import);
    CMP(dispatch_trait); CMP(dispatch_requirement); CMP(context);
#undef CMP
    return 0;
}

static SolMirPlanTypeId find_use_type(const RawInstance *instance,
    SolMirPlanTypedUseKind kind, size_t source, size_t ordinal) {
    for (size_t index = 0; index < instance->use_count; ++index) {
        const SolMirPlanTypedUse *use = &instance->uses[index];
        if (use->kind == kind && use->source == source
            && use->ordinal == ordinal) return use->type;
    }
    return SOL_MIR_PLAN_NONE;
}

static bool validate_expansion_path(Builder *builder,
    SolMirPlanInstanceId *path, size_t depth, SolMirPlanInstanceId current) {
    path[depth++] = current;
    for (size_t edge = 0; edge < builder->demand_count; ++edge) {
        const SolMirPlanDemand *demand = &builder->demands[edge];
        if (demand->parent != current
            || demand->instance == SOL_MIR_PLAN_NONE) continue;
        SolMirPlanInstanceId child_id = demand->instance;
        const RawInstance *child = &builder->instances[child_id];
        size_t last = SIZE_MAX;
        size_t previous = SIZE_MAX;
        for (size_t index = depth; index != 0; --index) {
            if (builder->instances[path[index - 1]].callable
                    != child->callable) continue;
            if (last == SIZE_MAX) last = index - 1;
            else { previous = index - 1; break; }
        }
        if (last != SIZE_MAX
            && instance_grows_ancestor(builder,
                &builder->instances[path[last]], child->receiver,
                child->arguments, child->argument_count)
            && previous != SIZE_MAX) {
            const RawInstance *later = &builder->instances[path[last]];
            if (instance_grows_ancestor(builder,
                    &builder->instances[path[previous]], later->receiver,
                    later->arguments, later->argument_count)) {
                return fail(builder, SOL_MIR_PLAN_BUILD_EXPANDING_RECURSION,
                    "demand graph repeats strict concrete type growth on one ancestry");
            }
        }
        bool cycle = false;
        for (size_t index = 0; index < depth; ++index) {
            cycle = cycle || path[index] == child_id;
        }
        if (!cycle && !validate_expansion_path(builder, path, depth,
                child_id)) return false;
    }
    return true;
}

static bool validate_expansion_graph(Builder *builder) {
    SolMirPlanInstanceId *path = allocate(builder->instance_count,
        sizeof(*path));
    if (builder->instance_count != 0 && path == NULL) return false;
    for (size_t edge = 0; edge < builder->demand_count; ++edge) {
        const SolMirPlanDemand *demand = &builder->demands[edge];
        if (demand->kind == SOL_MIR_PLAN_DEMAND_ROOT
            && demand->instance != SOL_MIR_PLAN_NONE
            && !validate_expansion_path(builder, path, 0,
                demand->instance)) {
            free(path);
            return false;
        }
    }
    free(path);
    return true;
}

static bool publish(Builder *builder) {
    SolMirPlan *plan = builder->plan;
    if (!canonicalize_atoms(builder) || !canonicalize_rows(builder)
        || !canonicalize_types(builder) || !canonicalize_instances(builder)
        || !canonicalize_imports(builder)) return false;
    for (size_t index = 0; index < builder->instance_count; ++index) {
        RawInstance *instance = &builder->instances[index];
        if (instance->use_count > 1) qsort(instance->uses, instance->use_count,
            sizeof(*instance->uses), use_compare);
    }
    if (builder->demand_count > 1) qsort(builder->demands,
        builder->demand_count, sizeof(*builder->demands), demand_compare);
    plan->types = allocate(builder->type_count, sizeof(*plan->types));
    plan->type_count = builder->type_count;
    plan->type_capacity = plan->type_count;
    for (size_t index = 0; index < builder->type_count; ++index) {
        RawType *source = &builder->types[index];
        if (source->argument_count > SIZE_MAX - source->parameter_count
            || plan->type_component_count > SIZE_MAX
                - source->argument_count - source->parameter_count
            || plan->type_parameter_access_count > SIZE_MAX
                - source->parameter_count) return false;
        plan->type_component_count += source->argument_count
            + source->parameter_count;
        plan->type_parameter_access_count += source->parameter_count;
    }
    plan->type_components = allocate(plan->type_component_count,
        sizeof(*plan->type_components));
    plan->type_component_capacity = plan->type_component_count;
    plan->type_parameter_accesses = allocate(plan->type_parameter_access_count,
        sizeof(*plan->type_parameter_accesses));
    plan->type_parameter_access_capacity = plan->type_parameter_access_count;
    if ((plan->type_count != 0 && plan->types == NULL)
        || (plan->type_component_count != 0 && plan->type_components == NULL)
        || (plan->type_parameter_access_count != 0
            && plan->type_parameter_accesses == NULL)) return false;
    size_t component = 0;
    size_t access = 0;
    for (size_t index = 0; index < builder->type_count; ++index) {
        RawType *source = &builder->types[index];
        SolMirPlanType *target = &plan->types[index];
        target->kind = source->kind;
        target->definition = source->definition;
        target->argument_offset = component;
        target->argument_count = source->argument_count;
        if (source->argument_count != 0) memcpy(plan->type_components + component,
            source->arguments, source->argument_count * sizeof(*source->arguments));
        component += source->argument_count;
        target->parameter_offset = component;
        target->parameter_count = source->parameter_count;
        if (source->parameter_count != 0) memcpy(plan->type_components + component,
            source->parameters, source->parameter_count * sizeof(*source->parameters));
        component += source->parameter_count;
        target->parameter_access_offset = access;
        if (source->parameter_count != 0) memcpy(
            plan->type_parameter_accesses + access, source->accesses,
            source->parameter_count * sizeof(*source->accesses));
        access += source->parameter_count;
        target->result = source->result;
        target->effects = source->effects;
    }
    plan->effect_atoms = builder->atoms;
    plan->effect_atom_count = builder->atom_count;
    plan->effect_atom_capacity = plan->effect_atom_count;
    builder->atoms = NULL;
    plan->effect_rows = allocate(builder->row_count, sizeof(*plan->effect_rows));
    plan->effect_row_count = builder->row_count;
    plan->effect_row_capacity = plan->effect_row_count;
    for (size_t index = 0; index < builder->row_count; ++index) {
        if (plan->effect_row_atom_count > SIZE_MAX - builder->rows[index].count) {
            return false;
        }
        plan->effect_row_atom_count += builder->rows[index].count;
    }
    plan->effect_row_atoms = allocate(plan->effect_row_atom_count,
        sizeof(*plan->effect_row_atoms));
    plan->effect_row_atom_capacity = plan->effect_row_atom_count;
    if ((plan->effect_row_count != 0 && plan->effect_rows == NULL)
        || (plan->effect_row_atom_count != 0 && plan->effect_row_atoms == NULL)) {
        return false;
    }
    size_t row_atom = 0;
    for (size_t index = 0; index < builder->row_count; ++index) {
        plan->effect_rows[index]
            = (SolMirPlanEffectRow){row_atom, builder->rows[index].count};
        if (builder->rows[index].count != 0) memcpy(plan->effect_row_atoms + row_atom,
            builder->rows[index].atoms,
            builder->rows[index].count * sizeof(*builder->rows[index].atoms));
        row_atom += builder->rows[index].count;
    }
    plan->instances = allocate(builder->instance_count, sizeof(*plan->instances));
    plan->instance_count = builder->instance_count;
    plan->instance_capacity = plan->instance_count;
    for (size_t index = 0; index < builder->instance_count; ++index) {
        const RawInstance *item = &builder->instances[index];
        const SolIrCallable *callable = &builder->ir->callables[item->callable];
        if (plan->instance_type_id_count > SIZE_MAX - item->argument_count
                - callable->parameters.count
            || plan->instance_access_count > SIZE_MAX - callable->parameters.count
            || plan->dictionary_entry_count > SIZE_MAX - item->dictionary_count
            || plan->typed_use_count > SIZE_MAX - item->use_count) return false;
        plan->instance_type_id_count += item->argument_count
            + callable->parameters.count;
        plan->instance_access_count += callable->parameters.count;
        plan->dictionary_entry_count += item->dictionary_count;
        plan->typed_use_count += item->use_count;
    }
    for (size_t index = 0; index < builder->import_count; ++index) {
        if (plan->instance_type_id_count > SIZE_MAX
                - builder->imports[index].parameter_count
            || plan->instance_access_count > SIZE_MAX
                - builder->imports[index].parameter_count) return false;
        plan->instance_type_id_count += builder->imports[index].parameter_count;
        plan->instance_access_count += builder->imports[index].parameter_count;
    }
    plan->instance_type_ids = allocate(plan->instance_type_id_count,
        sizeof(*plan->instance_type_ids));
    plan->instance_type_id_capacity = plan->instance_type_id_count;
    plan->instance_accesses = allocate(plan->instance_access_count,
        sizeof(*plan->instance_accesses));
    plan->instance_access_capacity = plan->instance_access_count;
    plan->dictionary_entries = allocate(plan->dictionary_entry_count,
        sizeof(*plan->dictionary_entries));
    plan->dictionary_entry_capacity = plan->dictionary_entry_count;
    plan->typed_uses = allocate(plan->typed_use_count, sizeof(*plan->typed_uses));
    plan->typed_use_capacity = plan->typed_use_count;
    if ((plan->instance_count != 0 && plan->instances == NULL)
        || (plan->instance_type_id_count != 0 && plan->instance_type_ids == NULL)
        || (plan->instance_access_count != 0 && plan->instance_accesses == NULL)
        || (plan->dictionary_entry_count != 0 && plan->dictionary_entries == NULL)
        || (plan->typed_use_count != 0 && plan->typed_uses == NULL)) return false;
    size_t type_id = 0;
    size_t instance_access = 0;
    size_t dictionary = 0;
    size_t typed_use = 0;
    for (size_t index = 0; index < builder->instance_count; ++index) {
        RawInstance *source = &builder->instances[index];
        const SolIrCallable *callable = &builder->ir->callables[source->callable];
        SolMirPlanInstance *target = &plan->instances[index];
        target->callable = source->callable;
        target->receiver = source->receiver;
        target->type_arguments = (SolMirPlanSlice){type_id, source->argument_count};
        if (source->argument_count != 0) memcpy(plan->instance_type_ids + type_id,
            source->arguments, source->argument_count * sizeof(*source->arguments));
        type_id += source->argument_count;
        target->parameter_types
            = (SolMirPlanSlice){type_id, callable->parameters.count};
        target->parameter_accesses
            = (SolMirPlanSlice){instance_access, callable->parameters.count};
        for (size_t parameter = 0; parameter < callable->parameters.count;
            ++parameter) {
            SolIrLocalId local
                = builder->ir->roots[callable->parameters.offset + parameter];
            plan->instance_type_ids[type_id++] = find_use_type(source,
                SOL_MIR_PLAN_USE_PARAMETER, local, parameter);
            plan->instance_accesses[instance_access++]
                = builder->ir->locals[local].access;
        }
        target->result = find_use_type(source, SOL_MIR_PLAN_USE_RESULT,
            source->callable, 0);
        target->effect_tail = source->effect_tail;
        target->effects = source->effects;
        target->dictionary
            = (SolMirPlanSlice){dictionary, source->dictionary_count};
        for (size_t entry = 0; entry < source->dictionary_count; ++entry) {
            RawDictionary *raw = &source->dictionary[entry];
            plan->dictionary_entries[dictionary++] = (SolMirPlanDictionaryEntry){
                raw->generic_ordinal, raw->trait, raw->requirement, raw->type,
                raw->implementation, raw->method,
            };
        }
        target->typed_uses = (SolMirPlanSlice){typed_use, source->use_count};
        if (source->use_count != 0) memcpy(plan->typed_uses + typed_use,
            source->uses, source->use_count * sizeof(*source->uses));
        typed_use += source->use_count;
    }
    plan->imports = allocate(builder->import_count, sizeof(*plan->imports));
    plan->import_count = builder->import_count;
    plan->import_capacity = plan->import_count;
    if (plan->import_count != 0 && plan->imports == NULL) return false;
    for (size_t index = 0; index < builder->import_count; ++index) {
        RawImport *source = &builder->imports[index];
        size_t old_types = type_id;
        size_t old_accesses = instance_access;
        if (source->parameter_count != 0) {
            memcpy(plan->instance_type_ids + old_types, source->parameters,
                source->parameter_count * sizeof(*source->parameters));
            memcpy(plan->instance_accesses + old_accesses, source->accesses,
                source->parameter_count * sizeof(*source->accesses));
        }
        type_id += source->parameter_count;
        instance_access += source->parameter_count;
        plan->imports[index] = (SolMirPlanImport){source->callable,
            source->receiver, {old_types, source->parameter_count},
            {old_accesses, source->parameter_count}, source->result, source->effects};
    }
    plan->demands = builder->demands;
    plan->demand_count = builder->demand_count;
    plan->demand_capacity = plan->demand_count;
    builder->demands = NULL;
    plan->usage.instances = plan->instance_count;
    plan->usage.concrete_types = plan->type_count;
    plan->usage.demands = plan->demand_count;
    plan->usage.typed_uses = plan->typed_use_count;
    return true;
}

static SolMirPlanBuildOutcome build_scratch(const SolMirPlanBuildRequest *request,
    SolMirPlan *plan, SolDiagnostics *diagnostics) {
    Builder builder;
    memset(&builder, 0, sizeof(builder));
    builder.program = request->program;
    builder.ir = request->program->ir;
    builder.plan = plan;
    builder.diagnostics = diagnostics;
    builder.outcome = SOL_MIR_PLAN_BUILD_ALLOCATION_FAILED;
    plan->program = request->program;
    plan->limits = request->limits == NULL || limits_zero(*request->limits)
        ? sol_mir_plan_default_limits() : *request->limits;
    builder.instances = allocate(plan->limits.max_instances,
        sizeof(*builder.instances));
    if (builder.instances == NULL) goto done;
    builder.instance_capacity = plan->limits.max_instances;
    size_t no_atoms = 0;
    SolMirPlanEffectRowId empty_row;
    if (!intern_row(&builder, &no_atoms, 0, &empty_row)) goto done;
    for (size_t root_index = 0; root_index < request->program->root_count;
        ++root_index) {
        const SolMirProgramRoot *root = &request->program->roots[root_index];
        const SolIrCallable *callable = &builder.ir->callables[root->callable];
        if (callable->generic_parameters.count != 0
            || callable->effect_parameters.count != 0
            || callable->effect_parameter != SOL_IR_NONE
            || callable->receiver != SOL_IR_NONE) {
            fail(&builder, SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
                "contextless generic, effect-polymorphic, or receiver fixture root is not monomorphic");
            goto done;
        }
        RawInstance temporary;
        memset(&temporary, 0, sizeof(temporary));
        temporary.callable = root->callable;
        temporary.receiver = SOL_MIR_PLAN_NONE;
        temporary.effect_tail = empty_row;
        temporary.effects = empty_row;
        SolMirPlanEffectRowId effects;
        if (!row_for_callable(&builder, root->callable, empty_row,
                &effects)) goto done;
        SolMirPlanInstanceId instance;
        if (!add_instance(&builder, root->callable, SOL_MIR_PLAN_NONE, NULL, 0,
                NULL, 0, empty_row, effects, SOL_MIR_PLAN_NONE,
                &instance)) goto done;
        SolMirPlanDemand demand;
        memset(&demand, 0, sizeof(demand));
        demand.kind = SOL_MIR_PLAN_DEMAND_ROOT;
        demand.parent = SOL_MIR_PLAN_NONE;
        demand.symbolic_target = root->callable;
        demand.instance = instance;
        demand.import = SOL_MIR_PLAN_NONE;
        demand.dispatch_trait = SOL_IR_NONE;
        demand.dispatch_requirement = SOL_IR_NONE;
        SolIrExpressionId body = callable->body;
        SolSpan span = body < builder.ir->expression_count
            ? builder.ir->expressions[body].span : callable->span;
        if (!source_for(&builder, root->callable, body, span, &demand.source)
            || !add_demand(&builder, demand)) goto done;
    }
    if (!validate_expansion_graph(&builder) || !publish(&builder)) goto done;
    builder.outcome = SOL_MIR_PLAN_BUILD_SUCCEEDED;
done:
    free_builder(&builder, plan->effect_atoms != NULL);
    return builder.outcome;
}

SolMirPlanBuildOutcome sol_mir_plan_build(
    const SolMirPlanBuildRequest *request, SolMirPlan *plan,
    SolDiagnostics *diagnostics) {
    if (request == NULL || plan == NULL || diagnostics == NULL
        || !plan_empty(plan) || request->program == NULL
        || (request->limits != NULL && !limits_zero(*request->limits)
            && !limits_complete(*request->limits))) {
        if (diagnostics != NULL) sol_diagnostics_add(diagnostics,
            "SOL-MIR-PLAN-001", SOL_SEVERITY_ERROR, (SolSpan){0},
            "invalid monomorphic MIR plan build request or destination");
        return SOL_MIR_PLAN_BUILD_INVALID_ARGUMENT;
    }
    if (!sol_mir_program_validate(request->program, diagnostics)) {
        return diagnostics->allocation_failed
            ? SOL_MIR_PLAN_BUILD_ALLOCATION_FAILED
            : SOL_MIR_PLAN_BUILD_INVALID_PROGRAM;
    }
    SolMirPlan scratch;
    sol_mir_plan_init(&scratch);
    SolMirPlanBuildOutcome outcome = build_scratch(request, &scratch, diagnostics);
    if (outcome == SOL_MIR_PLAN_BUILD_SUCCEEDED) {
        if (!sol_mir_plan_validate(&scratch, diagnostics)) {
            outcome = diagnostics->allocation_failed
                ? SOL_MIR_PLAN_BUILD_ALLOCATION_FAILED
                : SOL_MIR_PLAN_BUILD_INTERNAL_FAILED;
        } else {
            *plan = scratch;
            return outcome;
        }
    }
    sol_mir_plan_free(&scratch);
    return outcome;
}

typedef struct {
    uintptr_t start;
    uintptr_t end;
} PlanRange;

typedef struct {
    PlanRange *items;
    size_t count;
    size_t capacity;
} PlanRanges;

static bool validation_error(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) sol_diagnostics_add(diagnostics,
        "SOL-MIR-PLAN-002", SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    return false;
}

static bool canonical_arena(size_t count, size_t capacity,
    const void *pointer) {
    return count <= capacity && ((capacity == 0) == (pointer == NULL));
}

static bool add_range(PlanRanges *ranges, const void *pointer, size_t count,
    size_t size, SolDiagnostics *diagnostics) {
    if (count == 0) return true;
    if (pointer == NULL || size == 0 || count > SIZE_MAX / size) {
        return validation_error(diagnostics,
            "monomorphic MIR plan owned range is malformed");
    }
    size_t bytes = count * size;
    uintptr_t start = (uintptr_t)pointer;
    if (bytes > UINTPTR_MAX - start) {
        return validation_error(diagnostics,
            "monomorphic MIR plan owned range overflows");
    }
    uintptr_t end = start + bytes;
    for (size_t index = 0; index < ranges->count; ++index) {
        if (start < ranges->items[index].end
            && ranges->items[index].start < end) {
            return validation_error(diagnostics,
                "monomorphic MIR plan owned ranges overlap");
        }
    }
    if (!grow((void **)&ranges->items, &ranges->capacity, ranges->count + 1,
            sizeof(*ranges->items))) {
        return validation_error(diagnostics,
            "allocation failed while checking monomorphic plan ranges");
    }
    ranges->items[ranges->count++] = (PlanRange){start, end};
    return true;
}

static bool slices_valid(const SolMirPlan *plan, SolDiagnostics *diagnostics) {
    size_t expected_components = 0;
    size_t expected_type_accesses = 0;
    for (size_t index = 0; index < plan->type_count; ++index) {
        const SolMirPlanType *type = &plan->types[index];
        if (type->kind == SOL_IR_TYPE_PARAMETER || type->kind == SOL_IR_TYPE_SELF
            || type->kind > SOL_IR_TYPE_SELF
            || type->argument_offset != expected_components
            || type->argument_count > plan->type_component_count - expected_components) {
            return validation_error(diagnostics,
                "concrete type table contains a symbolic or malformed type");
        }
        expected_components += type->argument_count;
        if (type->parameter_offset != expected_components
            || type->parameter_count > plan->type_component_count - expected_components
            || type->parameter_access_offset != expected_type_accesses
            || type->parameter_count > plan->type_parameter_access_count
                - expected_type_accesses) return false;
        expected_components += type->parameter_count;
        expected_type_accesses += type->parameter_count;
        if ((type->result != SOL_MIR_PLAN_NONE && type->result >= plan->type_count)
            || (type->effects != SOL_MIR_PLAN_NONE
                && type->effects >= plan->effect_row_count)) return false;
        for (size_t child = type->argument_offset;
            child < type->parameter_offset + type->parameter_count; ++child) {
            if (plan->type_components[child] >= plan->type_count) return false;
        }
    }
    if (expected_components != plan->type_component_count
        || expected_type_accesses != plan->type_parameter_access_count) return false;
    size_t expected_atoms = 0;
    for (size_t index = 0; index < plan->effect_row_count; ++index) {
        const SolMirPlanEffectRow *row = &plan->effect_rows[index];
        if (row->atom_offset != expected_atoms
            || row->atom_count > plan->effect_row_atom_count - expected_atoms) return false;
        for (size_t atom = 0; atom < row->atom_count; ++atom) {
            size_t id = plan->effect_row_atoms[row->atom_offset + atom];
            if (id >= plan->effect_atom_count
                || (atom != 0 && plan->effect_row_atoms[
                    row->atom_offset + atom - 1] >= id)) return false;
        }
        expected_atoms += row->atom_count;
    }
    if (expected_atoms != plan->effect_row_atom_count) return false;
    for (size_t index = 0; index < plan->effect_atom_count; ++index) {
        const SolMirPlanEffectAtom *atom = &plan->effect_atoms[index];
        if (atom->name == NULL || atom->length == SIZE_MAX
            || atom->name[atom->length] != '\0'
            || atom->authority
                > SOL_MIR_PLAN_EFFECT_AUTHORITY_TAIL
            || (index != 0 && atom_compare_value(&plan->effect_atoms[index - 1],
                &plan->effect_atoms[index]) >= 0)) return false;
        for (size_t byte = 0; byte < atom->length; ++byte) {
            if (atom->name[byte] == '\0') return false;
        }
    }
    for (size_t index = 0; index < plan->instance_type_id_count; ++index) {
        if (plan->instance_type_ids[index] >= plan->type_count) return false;
    }
    for (size_t index = 0; index < plan->dictionary_entry_count; ++index) {
        if (plan->dictionary_entries[index].type >= plan->type_count) return false;
    }
    for (size_t index = 0; index < plan->typed_use_count; ++index) {
        if (plan->typed_uses[index].type >= plan->type_count
            || plan->typed_uses[index].kind > SOL_MIR_PLAN_USE_UNREACHABLE_PROOF) {
            return false;
        }
    }
    for (size_t index = 0; index < plan->instance_count; ++index) {
        const SolMirPlanInstance *item = &plan->instances[index];
#define SLICE_OK(slice, total) ((slice).offset <= (total) \
        && (slice).count <= (total) - (slice).offset)
        if (item->callable >= plan->program->ir->callable_count
            || (item->receiver != SOL_MIR_PLAN_NONE
                && item->receiver >= plan->type_count)
            || item->result >= plan->type_count
            || item->effect_tail >= plan->effect_row_count
            || item->effects >= plan->effect_row_count
            || !SLICE_OK(item->type_arguments, plan->instance_type_id_count)
            || !SLICE_OK(item->parameter_types, plan->instance_type_id_count)
            || !SLICE_OK(item->parameter_accesses, plan->instance_access_count)
            || !SLICE_OK(item->dictionary, plan->dictionary_entry_count)
            || !SLICE_OK(item->typed_uses, plan->typed_use_count)) return false;
#undef SLICE_OK
    }
    for (size_t index = 0; index < plan->import_count; ++index) {
        const SolMirPlanImport *item = &plan->imports[index];
        if (item->callable >= plan->program->ir->callable_count
            || (item->receiver != SOL_MIR_PLAN_NONE
                && item->receiver >= plan->type_count)
            || item->result >= plan->type_count
            || item->effects >= plan->effect_row_count
            || item->parameter_types.offset > plan->instance_type_id_count
            || item->parameter_types.count > plan->instance_type_id_count
                - item->parameter_types.offset
            || item->parameter_accesses.offset > plan->instance_access_count
            || item->parameter_accesses.count > plan->instance_access_count
                - item->parameter_accesses.offset) return false;
    }
    for (size_t index = 0; index < plan->demand_count; ++index) {
        const SolMirPlanDemand *item = &plan->demands[index];
        if (item->kind > SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE
            || (item->parent != SOL_MIR_PLAN_NONE
                && item->parent >= plan->instance_count)
            || (item->instance != SOL_MIR_PLAN_NONE
                && item->instance >= plan->instance_count)
            || (item->import != SOL_MIR_PLAN_NONE
                && item->import >= plan->import_count)
            || (item->instance == SOL_MIR_PLAN_NONE)
                == (item->import == SOL_MIR_PLAN_NONE)) return false;
    }
    return true;
}

static bool memory_valid(const SolMirPlan *plan, SolDiagnostics *diagnostics) {
    PlanRanges ranges = {0};
#define RANGE(field, capacity) if (!add_range(&ranges, plan->field, \
        plan->capacity, \
        sizeof(*plan->field), diagnostics)) goto invalid
    RANGE(types, type_capacity);
    RANGE(type_components, type_component_capacity);
    RANGE(type_parameter_accesses, type_parameter_access_capacity);
    RANGE(effect_atoms, effect_atom_capacity);
    RANGE(effect_rows, effect_row_capacity);
    RANGE(effect_row_atoms, effect_row_atom_capacity);
    RANGE(instances, instance_capacity);
    RANGE(instance_type_ids, instance_type_id_capacity);
    RANGE(instance_accesses, instance_access_capacity);
    RANGE(dictionary_entries, dictionary_entry_capacity);
    RANGE(imports, import_capacity);
    RANGE(typed_uses, typed_use_capacity);
    RANGE(demands, demand_capacity);
#undef RANGE
    for (size_t index = 0; index < plan->effect_atom_count; ++index) {
        if (plan->effect_atoms[index].name == NULL
            || plan->effect_atoms[index].length == SIZE_MAX
            || !add_range(&ranges, plan->effect_atoms[index].name,
                plan->effect_atoms[index].length + 1, sizeof(char),
                diagnostics)) goto invalid;
    }
    free(ranges.items);
    return true;
invalid:
    free(ranges.items);
    return false;
}

static bool limits_equal(SolMirPlanLimits a, SolMirPlanLimits b) {
    return a.max_instances == b.max_instances
        && a.max_concrete_types == b.max_concrete_types
        && a.max_demands == b.max_demands
        && a.max_typed_uses == b.max_typed_uses
        && a.max_planning_work == b.max_planning_work
        && a.max_substitution_depth == b.max_substitution_depth;
}

static bool usage_equal(SolMirPlanUsage a, SolMirPlanUsage b) {
    return a.instances == b.instances
        && a.concrete_types == b.concrete_types
        && a.demands == b.demands && a.typed_uses == b.typed_uses
        && a.planning_work == b.planning_work
        && a.substitution_depth == b.substitution_depth;
}

static bool slice_equal(SolMirPlanSlice a, SolMirPlanSlice b) {
    return a.offset == b.offset && a.count == b.count;
}

static bool source_equal(SolMirProgramSource a, SolMirProgramSource b) {
    return a.callable == b.callable && a.expression == b.expression
        && a.file == b.file && a.start == b.start && a.end == b.end;
}

static bool plans_equal(const SolMirPlan *a, const SolMirPlan *b) {
    if (a->program != b->program || !limits_equal(a->limits, b->limits)
        || !usage_equal(a->usage, b->usage)
        || a->type_count != b->type_count
        || a->type_capacity != b->type_capacity
        || a->type_component_count != b->type_component_count
        || a->type_component_capacity != b->type_component_capacity
        || a->type_parameter_access_count != b->type_parameter_access_count
        || a->type_parameter_access_capacity != b->type_parameter_access_capacity
        || a->effect_atom_count != b->effect_atom_count
        || a->effect_atom_capacity != b->effect_atom_capacity
        || a->effect_row_count != b->effect_row_count
        || a->effect_row_capacity != b->effect_row_capacity
        || a->effect_row_atom_count != b->effect_row_atom_count
        || a->effect_row_atom_capacity != b->effect_row_atom_capacity
        || a->instance_count != b->instance_count
        || a->instance_capacity != b->instance_capacity
        || a->instance_type_id_count != b->instance_type_id_count
        || a->instance_type_id_capacity != b->instance_type_id_capacity
        || a->instance_access_count != b->instance_access_count
        || a->instance_access_capacity != b->instance_access_capacity
        || a->dictionary_entry_count != b->dictionary_entry_count
        || a->dictionary_entry_capacity != b->dictionary_entry_capacity
        || a->import_count != b->import_count
        || a->import_capacity != b->import_capacity
        || a->typed_use_count != b->typed_use_count
        || a->typed_use_capacity != b->typed_use_capacity
        || a->demand_count != b->demand_count
        || a->demand_capacity != b->demand_capacity) return false;
    for (size_t index = 0; index < a->type_count; ++index) {
        const SolMirPlanType *x = &a->types[index];
        const SolMirPlanType *y = &b->types[index];
        if (x->kind != y->kind || x->definition != y->definition
            || x->argument_offset != y->argument_offset
            || x->argument_count != y->argument_count
            || x->parameter_offset != y->parameter_offset
            || x->parameter_count != y->parameter_count
            || x->parameter_access_offset != y->parameter_access_offset
            || x->result != y->result || x->effects != y->effects) return false;
    }
    for (size_t index = 0; index < a->type_component_count; ++index) {
        if (a->type_components[index] != b->type_components[index]) return false;
    }
    for (size_t index = 0; index < a->type_parameter_access_count; ++index) {
        if (a->type_parameter_accesses[index]
            != b->type_parameter_accesses[index]) return false;
    }
    for (size_t index = 0; index < a->effect_atom_count; ++index) {
        if (a->effect_atoms[index].length != b->effect_atoms[index].length
            || a->effect_atoms[index].authority != b->effect_atoms[index].authority
            || a->effect_atoms[index].ordinal != b->effect_atoms[index].ordinal
            || (a->effect_atoms[index].length != 0
                && memcmp(a->effect_atoms[index].name,
                    b->effect_atoms[index].name,
                    a->effect_atoms[index].length) != 0)) return false;
    }
    for (size_t index = 0; index < a->effect_row_count; ++index) {
        if (a->effect_rows[index].atom_offset != b->effect_rows[index].atom_offset
            || a->effect_rows[index].atom_count
                != b->effect_rows[index].atom_count) return false;
    }
    for (size_t index = 0; index < a->effect_row_atom_count; ++index) {
        if (a->effect_row_atoms[index] != b->effect_row_atoms[index]) return false;
    }
    for (size_t index = 0; index < a->instance_count; ++index) {
        const SolMirPlanInstance *x = &a->instances[index];
        const SolMirPlanInstance *y = &b->instances[index];
        if (x->callable != y->callable || x->receiver != y->receiver
            || !slice_equal(x->type_arguments, y->type_arguments)
            || !slice_equal(x->dictionary, y->dictionary)
            || !slice_equal(x->parameter_types, y->parameter_types)
            || !slice_equal(x->parameter_accesses, y->parameter_accesses)
            || x->result != y->result || x->effect_tail != y->effect_tail
            || x->effects != y->effects
            || !slice_equal(x->typed_uses, y->typed_uses)) return false;
    }
    for (size_t index = 0; index < a->instance_type_id_count; ++index) {
        if (a->instance_type_ids[index] != b->instance_type_ids[index]) return false;
    }
    for (size_t index = 0; index < a->instance_access_count; ++index) {
        if (a->instance_accesses[index] != b->instance_accesses[index]) return false;
    }
    for (size_t index = 0; index < a->dictionary_entry_count; ++index) {
        const SolMirPlanDictionaryEntry *x = &a->dictionary_entries[index];
        const SolMirPlanDictionaryEntry *y = &b->dictionary_entries[index];
        if (x->generic_ordinal != y->generic_ordinal || x->trait != y->trait
            || x->requirement != y->requirement || x->type != y->type
            || x->implementation != y->implementation || x->method != y->method) {
            return false;
        }
    }
    for (size_t index = 0; index < a->import_count; ++index) {
        const SolMirPlanImport *x = &a->imports[index];
        const SolMirPlanImport *y = &b->imports[index];
        if (x->callable != y->callable || x->receiver != y->receiver
            || !slice_equal(x->parameter_types, y->parameter_types)
            || !slice_equal(x->parameter_accesses, y->parameter_accesses)
            || x->result != y->result || x->effects != y->effects) return false;
    }
    for (size_t index = 0; index < a->typed_use_count; ++index) {
        const SolMirPlanTypedUse *x = &a->typed_uses[index];
        const SolMirPlanTypedUse *y = &b->typed_uses[index];
        if (x->kind != y->kind || x->source != y->source
            || x->ordinal != y->ordinal || x->context != y->context
            || x->type != y->type
            || x->access != y->access) return false;
    }
    for (size_t index = 0; index < a->demand_count; ++index) {
        const SolMirPlanDemand *x = &a->demands[index];
        const SolMirPlanDemand *y = &b->demands[index];
        if (x->kind != y->kind || x->parent != y->parent
            || !source_equal(x->source, y->source)
            || x->symbolic_target != y->symbolic_target
            || x->instance != y->instance || x->import != y->import
            || x->dispatch_trait != y->dispatch_trait
            || x->dispatch_requirement != y->dispatch_requirement
            || x->context != y->context) return false;
    }
    return true;
}

bool sol_mir_plan_validate(const SolMirPlan *plan, SolDiagnostics *diagnostics) {
    SolDiagnostics local;
    bool owns_diagnostics = diagnostics == NULL;
    if (owns_diagnostics) {
        sol_diagnostics_init(&local);
        diagnostics = &local;
    }
    bool header = plan != NULL && plan->program != NULL
        && limits_complete(plan->limits) && plan->instance_count != 0
        && plan->usage.instances == plan->instance_count
        && plan->usage.concrete_types == plan->type_count
        && plan->usage.demands == plan->demand_count
        && plan->usage.typed_uses == plan->typed_use_count
        && plan->usage.instances <= plan->limits.max_instances
        && plan->usage.concrete_types <= plan->limits.max_concrete_types
        && plan->usage.demands <= plan->limits.max_demands
        && plan->usage.typed_uses <= plan->limits.max_typed_uses
        && plan->usage.planning_work <= plan->limits.max_planning_work
        && plan->usage.substitution_depth <= plan->limits.max_substitution_depth
        && canonical_arena(plan->type_count, plan->type_capacity, plan->types)
        && canonical_arena(plan->type_component_count,
            plan->type_component_capacity, plan->type_components)
        && canonical_arena(plan->type_parameter_access_count,
            plan->type_parameter_access_capacity, plan->type_parameter_accesses)
        && canonical_arena(plan->effect_atom_count,
            plan->effect_atom_capacity, plan->effect_atoms)
        && canonical_arena(plan->effect_row_count,
            plan->effect_row_capacity, plan->effect_rows)
        && canonical_arena(plan->effect_row_atom_count,
            plan->effect_row_atom_capacity, plan->effect_row_atoms)
        && canonical_arena(plan->instance_count,
            plan->instance_capacity, plan->instances)
        && canonical_arena(plan->instance_type_id_count,
            plan->instance_type_id_capacity, plan->instance_type_ids)
        && canonical_arena(plan->instance_access_count,
            plan->instance_access_capacity, plan->instance_accesses)
        && canonical_arena(plan->dictionary_entry_count,
            plan->dictionary_entry_capacity, plan->dictionary_entries)
        && canonical_arena(plan->import_count,
            plan->import_capacity, plan->imports)
        && canonical_arena(plan->typed_use_count,
            plan->typed_use_capacity, plan->typed_uses)
        && canonical_arena(plan->demand_count,
            plan->demand_capacity, plan->demands);
    if (!header) {
        validation_error(diagnostics,
            "monomorphic MIR plan count, pointer, usage, or limit header is noncanonical");
        if (owns_diagnostics) sol_diagnostics_free(&local);
        return false;
    }
    if (!sol_mir_program_validate(plan->program, diagnostics)
        || !memory_valid(plan, diagnostics)) {
        if (owns_diagnostics) sol_diagnostics_free(&local);
        return false;
    }
    if (!slices_valid(plan, diagnostics)) {
        validation_error(diagnostics,
            "monomorphic MIR plan contains a malformed canonical slice or id");
        if (owns_diagnostics) sol_diagnostics_free(&local);
        return false;
    }
    SolMirPlanBuildRequest request = {plan->program, &plan->limits};
    SolMirPlan expected;
    sol_mir_plan_init(&expected);
    SolMirPlanBuildOutcome outcome = build_scratch(&request, &expected, diagnostics);
    bool valid = outcome == SOL_MIR_PLAN_BUILD_SUCCEEDED
        && plans_equal(plan, &expected);
    sol_mir_plan_free(&expected);
    if (!valid) validation_error(diagnostics,
        "monomorphic MIR plan is not the canonical authentic plan");
    if (owns_diagnostics) sol_diagnostics_free(&local);
    return valid;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
SolMirPlanBuildOutcome sol_mir_plan_test_recursion_classification(int mode) {
    SolMirPlanTypeId option_argument = 1;
    SolMirPlanTypeId nested_argument = 2;
    RawType types[4];
    memset(types, 0, sizeof(types));
    types[0].kind = SOL_IR_TYPE_BOOL;
    types[1].kind = SOL_IR_TYPE_INT64;
    types[2].kind = SOL_IR_TYPE_OPTION;
    for (size_t index = 0; index < 3; ++index) {
        types[index].result = SOL_MIR_PLAN_NONE;
    }
    types[2].arguments = &option_argument;
    types[2].argument_count = 1;
    types[3].kind = SOL_IR_TYPE_OPTION;
    types[3].result = SOL_MIR_PLAN_NONE;
    types[3].arguments = &nested_argument;
    types[3].argument_count = 1;
    RawInstance instances[6];
    memset(instances, 0, sizeof(instances));
    SolMirPlanTypeId bool_argument = 0;
    SolMirPlanTypeId int_argument = 1;
    SolMirPlanTypeId option = 2;
    Builder builder;
    memset(&builder, 0, sizeof(builder));
    builder.types = types;
    builder.type_count = 4;
    builder.instances = instances;
    if (mode <= 1 || mode >= 4) {
        size_t base = mode >= 4 ? 1 : 0;
        if (base != 0) {
            instances[0].callable = 9;
            instances[0].arguments = &bool_argument;
            instances[0].argument_count = 1;
            instances[0].parent = SOL_MIR_PLAN_NONE;
        }
        instances[base].callable = 7;
        instances[base].arguments = &int_argument;
        instances[base].argument_count = 1;
        instances[base].parent = SOL_MIR_PLAN_NONE;
        instances[base + 1].callable = 7;
        instances[base + 1].arguments = &option;
        instances[base + 1].argument_count = 1;
        instances[base + 1].parent = base;
        builder.instance_count = base + 2;
        bool finite = mode == 0 || mode == 5;
        SolMirPlanTypeId child = finite ? option : 3;
        return repeated_growth_for_parent(&builder, base + 1, 7,
                SOL_MIR_PLAN_NONE, &child, 1)
            ? SOL_MIR_PLAN_BUILD_EXPANDING_RECURSION
            : SOL_MIR_PLAN_BUILD_SUCCEEDED;
    }
    instances[0].callable = 7;
    instances[0].arguments = mode == 2 ? &bool_argument : &int_argument;
    instances[0].argument_count = 1;
    instances[0].parent = SOL_MIR_PLAN_NONE;
    instances[1].callable = 8;
    instances[1].arguments = mode == 2 ? &bool_argument : &int_argument;
    instances[1].argument_count = 1;
    instances[1].parent = 0;
    instances[2].callable = 7;
    instances[2].arguments = mode == 2 ? &int_argument : &option;
    instances[2].argument_count = 1;
    instances[2].parent = 1;
    instances[3].callable = 8;
    instances[3].arguments = mode == 2 ? &int_argument : &option;
    instances[3].argument_count = 1;
    instances[3].parent = 2;
    builder.instance_count = 4;
    SolMirPlanTypeId child = mode == 2 ? int_argument : 3;
    return repeated_growth_for_parent(&builder, 3, 7,
            SOL_MIR_PLAN_NONE, &child, 1)
        ? SOL_MIR_PLAN_BUILD_EXPANDING_RECURSION
        : SOL_MIR_PLAN_BUILD_SUCCEEDED;
}


static bool test_row_has(const Builder *builder, SolMirPlanEffectRowId row_id,
    const char *name, SolMirPlanEffectAuthority authority, size_t ordinal) {
    const RawRow *row = &builder->rows[row_id];
    size_t length = strlen(name);
    for (size_t index = 0; index < row->count; ++index) {
        const SolMirPlanEffectAtom *atom = &builder->atoms[row->atoms[index]];
        if (atom->length == length && atom->authority == authority
            && atom->ordinal == ordinal
            && memcmp(atom->name, name, length) == 0) return true;
    }
    return false;
}

bool sol_mir_plan_test_effect_normalization(void) {
    SolIrEffect effects[2] = {
        {(char *)"panic", SOL_IR_AUTHORITY_NONE, SOL_IR_NONE},
        {(char *)"authority", SOL_IR_AUTHORITY_LOCAL, 0},
    };
    SolIrCallable callables[2];
    memset(callables, 0, sizeof(callables));
    callables[0].receiver = SOL_IR_NONE;
    callables[1].receiver = SOL_IR_NONE;
    callables[0].effect_parameter = 0;
    callables[1].parameters = (SolIrSlice){0, 1};
    callables[1].effects = (SolIrSlice){0, 1};
    callables[1].effect_parameter = 1;
    SolIrType types[6];
    memset(types, 0, sizeof(types));
    for (size_t index = 0; index < 6; ++index) {
        types[index].definition = SOL_IR_NONE;
        types[index].result = SOL_IR_NONE;
        types[index].effect_parameter = SOL_IR_NONE;
    }
    types[0].kind = SOL_IR_TYPE_INT64;
    types[1].kind = SOL_IR_TYPE_FUNCTION;
    types[1].result = 0;
    types[1].effect_parameter = 1;
    types[2].kind = SOL_IR_TYPE_FUNCTION;
    types[2].result = 0;
    types[3].kind = SOL_IR_TYPE_FUNCTION;
    types[3].result = 0;
    types[3].effects = (SolIrSlice){0, 1};
    types[4].kind = SOL_IR_TYPE_FUNCTION;
    types[4].result = 0;
    types[4].effect_parameter = 0;
    types[5].kind = SOL_IR_TYPE_FUNCTION;
    types[5].result = 0;
    types[5].effects = (SolIrSlice){1, 1};
    SolIrLocal locals[2];
    memset(locals, 0, sizeof(locals));
    locals[0].type = 1;
    locals[1].type = 1;
    SolIrLocalId roots[2] = {0, 1};
    SolIrExpression expressions[4];
    memset(expressions, 0, sizeof(expressions));
    expressions[0].type = 2;
    expressions[1].type = 3;
    expressions[2].type = 4;
    expressions[3].type = 5;
    SolIrOperand operands[2] = {
        {0, 0, SOL_ACCESS_OWNED},
        {1, 1, SOL_ACCESS_OWNED},
    };
    SolIr ir;
    memset(&ir, 0, sizeof(ir));
    ir.effects = effects;
    ir.effect_count = 2;
    ir.callables = callables;
    ir.callable_count = 2;
    ir.types = types;
    ir.type_count = 6;
    ir.locals = locals;
    ir.local_count = 2;
    ir.roots = roots;
    ir.root_count = 2;
    ir.expressions = expressions;
    ir.expression_count = 4;
    ir.operands = operands;
    ir.operand_count = 2;
    SolMirPlan plan;
    sol_mir_plan_init(&plan);
    plan.limits = sol_mir_plan_default_limits();
    Builder builder;
    memset(&builder, 0, sizeof(builder));
    builder.ir = &ir;
    builder.plan = &plan;
    builder.outcome = SOL_MIR_PLAN_BUILD_ALLOCATION_FAILED;
    size_t no_atoms = 0;
    SolMirPlanEffectRowId empty;
    size_t forwarded_atom = 0;
    size_t forwarded_atoms[1];
    SolMirPlanEffectRowId forwarded = 0;
    bool valid = intern_row(&builder, &no_atoms, 0, &empty)
        && intern_atom(&builder, "forwarded",
            SOL_MIR_PLAN_EFFECT_AUTHORITY_NONE, 0, &forwarded_atom);
    forwarded_atoms[0] = forwarded_atom;
    valid = valid && intern_row(&builder, forwarded_atoms, 1, &forwarded);
    RawInstance caller;
    memset(&caller, 0, sizeof(caller));
    caller.callable = 0;
    caller.effect_tail = forwarded;
    Environment environment = {&builder, &caller, 0, SOL_IR_NONE, NULL, 0,
        SOL_MIR_PLAN_NONE, 0};
    SolIrExpression call;
    memset(&call, 0, sizeof(call));
    call.kind = SOL_IR_EXPR_CALL;
    call.as.call.operands = (SolIrSlice){0, 1};
    call.as.call.effect_parameter = SOL_IR_NONE;
    SolMirPlanEffectRowId quiet_tail = SOL_MIR_PLAN_NONE;
    SolMirPlanEffectRowId quiet_full = SOL_MIR_PLAN_NONE;
    SolMirPlanEffectRowId noisy_tail = SOL_MIR_PLAN_NONE;
    SolMirPlanEffectRowId noisy_full = SOL_MIR_PLAN_NONE;
    SolMirPlanEffectRowId union_tail = SOL_MIR_PLAN_NONE;
    SolMirPlanEffectRowId forwarded_tail = SOL_MIR_PLAN_NONE;
    if (valid) valid = tail_for_call(&environment, 1, &call, &quiet_tail)
        && row_for_callable(&builder, 1, quiet_tail, &quiet_full);
    operands[0].value = 1;
    if (valid) valid = tail_for_call(&environment, 1, &call, &noisy_tail)
        && row_for_callable(&builder, 1, noisy_tail, &noisy_full);
    callables[1].parameters.count = 2;
    call.as.call.operands.count = 2;
    operands[0].value = 0;
    if (valid) valid = tail_for_call(&environment, 1, &call, &union_tail);
    callables[1].parameters.count = 1;
    call.as.call.operands.count = 1;
    operands[0].value = 2;
    call.as.call.effect_parameter = 0;
    if (valid) valid = tail_for_call(&environment, 1, &call, &forwarded_tail);
    valid = valid && builder.rows[empty].count == 0
        && builder.rows[forwarded].count == 1
        && builder.rows[quiet_tail].count == 0
        && builder.rows[quiet_full].count == 1
        && builder.rows[noisy_tail].count == 1
        && builder.rows[noisy_full].count == 1
        && noisy_full == quiet_full
        && builder.rows[union_tail].count == 1
        && builder.rows[forwarded_tail].count == 1
        && test_row_has(&builder, noisy_tail, "panic",
            SOL_MIR_PLAN_EFFECT_AUTHORITY_NONE, 0)
        && test_row_has(&builder, union_tail, "panic",
            SOL_MIR_PLAN_EFFECT_AUTHORITY_NONE, 0)
        && test_row_has(&builder, forwarded_tail, "forwarded",
            SOL_MIR_PLAN_EFFECT_AUTHORITY_NONE, 0);
    call.as.call.effect_parameter = SOL_IR_NONE;
    operands[0].value = 3;
    SolMirPlanEffectRowId rejected = SOL_MIR_PLAN_NONE;
    bool authority_rejected = !tail_for_call(&environment, 1, &call, &rejected)
        && builder.outcome == SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED;
    valid = valid && authority_rejected;
    free_builder(&builder, false);
    return valid;
}
#endif
