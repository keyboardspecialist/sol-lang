#include "sol/mir_runtime_conventions.h"
#include "mir_linkage_internal.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SolMirRuntimeConventions *out;
    SolDiagnostics *diagnostics;
    SolMirRuntimeConventionsBuildOutcome outcome;
} Builder;

bool sol_mir_runtime_conventions_internal_validation_requirements(
    const SolMirRuntimeConventions *owner, size_t *work, size_t *scratch,
    SolDiagnostics *diagnostics);
SolMirRuntimeConventionsBuildOutcome sol_mir_runtime_conventions_internal_preflight(
    const SolMirConcreteProgram *concrete,
    const SolMirRuntimeConventionsLimits *limits,
    SolMirRuntimeConventionsUsage *usage,
    SolDiagnostics *diagnostics, bool meter_validation);

typedef struct { char *data; size_t length, capacity; bool failed; } Buffer;

static _Thread_local size_t metered_build_work;
static _Thread_local size_t metered_build_limit;
static _Thread_local bool build_work_exhausted;

#ifdef SOL_MIR_PLAN_TEST_HOOKS
static _Thread_local SolMirRuntimeBuildTestCategory forced_build_category
    = SOL_MIR_RUNTIME_BUILD_TEST_CATEGORY_COUNT;
static _Thread_local size_t build_category_attempts[
    SOL_MIR_RUNTIME_BUILD_TEST_CATEGORY_COUNT];
static _Thread_local size_t build_events_after_exhaustion;
#endif

static bool build_event(size_t amount) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (build_work_exhausted) {
        ++build_events_after_exhaustion;
        return false;
    }
#endif
    if (amount > SIZE_MAX - metered_build_work) {
        build_work_exhausted = true;
        return false;
    }
    metered_build_work += amount;
    if (metered_build_work > metered_build_limit) {
        build_work_exhausted = true;
        return false;
    }
    return true;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
static bool build_category_event(SolMirRuntimeBuildTestCategory category,
    size_t amount) {
    ++build_category_attempts[category];
    if (category == forced_build_category) {
        build_work_exhausted = true;
        return false;
    }
    return build_event(amount);
}
#else
static bool build_category_event(size_t category, size_t amount) {
    (void)category;
    return build_event(amount);
}
#define SOL_MIR_RUNTIME_BUILD_TEST_HASH 0
#define SOL_MIR_RUNTIME_BUILD_TEST_TARGET 1
#define SOL_MIR_RUNTIME_BUILD_TEST_DIGEST 2
#define SOL_MIR_RUNTIME_BUILD_TEST_SORT 3
#endif

#ifdef SOL_MIR_PLAN_TEST_HOOKS
_Thread_local bool sol_mir_runtime_force_host_collision;
_Thread_local bool sol_mir_runtime_force_recipe_collision;
_Thread_local bool sol_mir_runtime_force_host_symbol_collision;
_Thread_local bool sol_mir_runtime_force_recipe_symbol_collision;
static _Thread_local bool force_allocation_failure;

void sol_mir_runtime_conventions_test_force_build_category(
    SolMirRuntimeBuildTestCategory category) {
    forced_build_category = category;
    memset(build_category_attempts, 0, sizeof(build_category_attempts));
    build_events_after_exhaustion = 0;
}

size_t sol_mir_runtime_conventions_test_build_category_attempts(
    SolMirRuntimeBuildTestCategory category) {
    return category < SOL_MIR_RUNTIME_BUILD_TEST_CATEGORY_COUNT
        ? build_category_attempts[category] : 0;
}

size_t sol_mir_runtime_conventions_test_build_events_after_exhaustion(void) {
    return build_events_after_exhaustion;
}

void sol_mir_runtime_conventions_test_force_host_collision(bool force) {
    sol_mir_runtime_force_host_collision = force;
}

void sol_mir_runtime_conventions_test_force_recipe_collision(bool force) {
    sol_mir_runtime_force_recipe_collision = force;
}

void sol_mir_runtime_conventions_test_force_host_symbol_collision(bool force) {
    sol_mir_runtime_force_host_symbol_collision = force;
}

void sol_mir_runtime_conventions_test_force_recipe_symbol_collision(bool force) {
    sol_mir_runtime_force_recipe_symbol_collision = force;
}

void sol_mir_runtime_conventions_test_force_allocation_failure(bool force) {
    force_allocation_failure = force;
}
#endif

static SolMirRuntimeConventionsBuildOutcome classify_call_kind(
    SolIrCallKind kind) {
    switch (kind) {
        case SOL_IR_CALL_FUNCTION:
        case SOL_IR_CALL_CALLBACK:
        case SOL_IR_CALL_CAPABILITY:
        case SOL_IR_CALL_METHOD:
            return SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED;
        case SOL_IR_CALL_BUILTIN_OK:
        case SOL_IR_CALL_BUILTIN_ERR:
        case SOL_IR_CALL_BUILTIN_SOME:
        case SOL_IR_CALL_BUILTIN_NONE:
        case SOL_IR_CALL_ENUM_CONSTRUCTOR:
        case SOL_IR_CALL_DISTINCT_CONSTRUCTOR:
            return SOL_MIR_RUNTIME_CONVENTIONS_BUILD_UNSUPPORTED_CALLABLE;
    }
    return SOL_MIR_RUNTIME_CONVENTIONS_BUILD_UNSUPPORTED_CALLABLE;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
SolMirRuntimeConventionsBuildOutcome
sol_mir_runtime_conventions_test_classify_call_kind(SolIrCallKind kind) {
    return classify_call_kind(kind);
}
#endif

static bool report(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) sol_diagnostics_add(diagnostics,
        "SOL-MIR-RUNTIME-CONVENTIONS-001", SOL_SEVERITY_ERROR, (SolSpan){0},
        message);
    return false;
}

static bool fail(Builder *builder, SolMirRuntimeConventionsBuildOutcome outcome,
    const char *message) {
    builder->outcome = outcome;
    return report(builder->diagnostics, message);
}

static bool add_size(size_t *value, size_t amount) {
    if (amount > SIZE_MAX - *value) return false;
    *value += amount;
    return true;
}

static bool add_resource_size(size_t *value, size_t amount) {
    if (add_size(value, amount)) return true;
    build_work_exhausted = true;
    return false;
}

static bool mul_size(size_t left, size_t right, size_t *result) {
    if (left != 0 && right > SIZE_MAX / left) return false;
    *result = left * right;
    return true;
}

static bool limits_zero(SolMirRuntimeConventionsLimits v) {
    return v.max_signatures == 0 && v.max_signature_slots == 0
        && v.max_calls == 0 && v.max_operands == 0 && v.max_writebacks == 0
        && v.max_entries == 0 && v.max_imports == 0
        && v.max_failure_sites == 0 && v.max_owned_bytes == 0
        && v.max_build_scratch_bytes == 0 && v.max_build_work == 0
        && v.max_validation_scratch_bytes == 0
        && v.max_validation_work == 0;
}

static bool limits_complete(SolMirRuntimeConventionsLimits v) {
#define REQUIRED(member) v.member != 0
    return REQUIRED(max_signatures) && REQUIRED(max_signature_slots)
        && REQUIRED(max_calls) && REQUIRED(max_operands)
        && REQUIRED(max_writebacks) && REQUIRED(max_entries)
        && REQUIRED(max_imports) && REQUIRED(max_failure_sites)
        && REQUIRED(max_owned_bytes)
        && REQUIRED(max_build_scratch_bytes) && REQUIRED(max_build_work)
        && REQUIRED(max_validation_scratch_bytes)
        && REQUIRED(max_validation_work);
#undef REQUIRED
}

static bool owner_empty(const SolMirRuntimeConventions *owner) {
    if (owner == NULL || owner->concrete != NULL) return false;
#define EMPTY(member, type, singular) \
    if (owner->member != NULL || owner->singular##_count != 0 \
        || owner->singular##_capacity != 0) return false;
    SOL_MIR_RUNTIME_CONVENTIONS_ARENAS(EMPTY)
#undef EMPTY
    SolMirRuntimeConventionsUsage zero = {0};
    return limits_zero(owner->limits)
        && memcmp(&owner->usage, &zero, sizeof(zero)) == 0;
}

void sol_mir_runtime_conventions_init(SolMirRuntimeConventions *owner) {
    if (owner != NULL) memset(owner, 0, sizeof(*owner));
}

void sol_mir_runtime_conventions_free(SolMirRuntimeConventions *owner) {
    if (owner == NULL) return;
#define FREE(member, type, singular) free(owner->member);
    SOL_MIR_RUNTIME_CONVENTIONS_ARENAS(FREE)
#undef FREE
    sol_mir_runtime_conventions_init(owner);
}

SolMirRuntimeConventionsLimits sol_mir_runtime_conventions_default_limits(void) {
    return (SolMirRuntimeConventionsLimits){
        .max_signatures = 4000000,
        .max_signature_slots = 16000000,
        .max_calls = 16000000,
        .max_operands = 64000000,
        .max_writebacks = 32000000,
        .max_entries = 1,
        .max_imports = 4000000,
        .max_failure_sites = 32000000,
        .max_owned_bytes = 1024u * 1024u * 1024u,
        .max_build_scratch_bytes = 256u * 1024u * 1024u,
        .max_build_work = (size_t)4000000000ULL,
        .max_validation_scratch_bytes = 1024u * 1024u * 1024u,
        .max_validation_work = (size_t)4000000000ULL,
    };
}

static void *allocate(Builder *builder, size_t count, size_t size) {
    if (count == 0) return NULL;
    if (!build_event(1)) {
        fail(builder, SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED,
            "runtime conventions build work limit exceeded");
        return NULL;
    }
    size_t bytes;
    if (!mul_size(count, size, &bytes)
        || !add_size(&builder->out->usage.owned_bytes, bytes)
        || builder->out->usage.owned_bytes
            > builder->out->limits.max_owned_bytes) {
        fail(builder, SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED,
            "runtime conventions owned-byte limit exceeded");
        return NULL;
    }
    void *value = NULL;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (!force_allocation_failure) value = calloc(count, size);
#else
    value = calloc(count, size);
#endif
    if (value == NULL) {
        builder->out->usage.owned_bytes -= bytes;
        if (builder->diagnostics != NULL)
            builder->diagnostics->allocation_failed = true;
        fail(builder, SOL_MIR_RUNTIME_CONVENTIONS_BUILD_ALLOCATION_FAILED,
            "runtime conventions persistent allocation failed");
    }
    return value;
}

static bool runtime_source_from_span(const SolIr *ir, SolSpan span,
    SolMirRuntimeSource *source) {
    if (span.start > span.end) return false;
    bool found = false;
    SolMirRuntimeSource result = {0};
    for (size_t i = 0; i < ir->file_count; ++i) {
        if (!build_event(1)) return false;
        const SolIrSourceFile *file = &ir->files[i];
        if (span.start < file->aggregate_start || span.end > file->aggregate_end)
            continue;
        if (found) return false;
        result = (SolMirRuntimeSource){i,
            span.start - file->aggregate_start,
            span.end - file->aggregate_start};
        found = true;
    }
    if (found) *source = result;
    return found;
}

static SolMirRuntimeResultClass classify_result_kind(SolMirRecipeKind kind) {
    if (kind == SOL_MIR_RECIPE_UNIT) return SOL_MIR_RUNTIME_RESULT_UNIT;
    if (kind == SOL_MIR_RECIPE_NEVER) return SOL_MIR_RUNTIME_RESULT_NEVER;
    return SOL_MIR_RUNTIME_RESULT_VALUE;
}

static SolMirRuntimeResultClass result_class(const SolMirRepresentation *r,
    SolMirRecipeId recipe) {
    if (recipe >= r->recipe_count) return (SolMirRuntimeResultClass)99;
    return classify_result_kind(r->recipes[recipe].kind);
}

static uint32_t failure_code_bit(SolMirRuntimeFailureCode code) {
    return UINT32_C(1) << ((unsigned)code - 1);
}

static bool build_call_failure_mask(const SolMirRuntimeCall *call,
    const SolMirLinkage *linkage, uint32_t *mask) {
    bool targets_host;
    if (call->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL) {
        targets_host = false;
    } else if (call->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_HOST) {
        targets_host = true;
    } else if (call->target_kind == SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE) {
        if (call->table >= linkage->table_entry_count) return false;
        SolMirLinkageTargetKind target
            = linkage->table_entries[call->table].target_kind;
        if (target != SOL_MIR_LINKAGE_TARGET_INTERNAL
            && target != SOL_MIR_LINKAGE_TARGET_HOST) return false;
        targets_host = target == SOL_MIR_LINKAGE_TARGET_HOST;
    } else return false;
    *mask = failure_code_bit(SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT);
    if (targets_host) {
        *mask |= failure_code_bit(SOL_MIR_RUNTIME_FAILURE_HOST_CALL_LIMIT);
        *mask |= failure_code_bit(SOL_MIR_RUNTIME_FAILURE_HOST_ERROR);
    }
    return true;
}

static uint32_t arithmetic_failure_mask(unsigned failures) {
    uint32_t mask = 0;
    if ((failures & SOL_MIR_OPERATION_FAILURE_OVERFLOW) != 0)
        mask |= failure_code_bit(SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW);
    if ((failures & SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO) != 0)
        mask |= failure_code_bit(SOL_MIR_RUNTIME_FAILURE_DIVISION_BY_ZERO);
    return mask;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
SolMirRuntimeResultClass sol_mir_runtime_conventions_test_classify_result_kind(
    SolMirRecipeKind kind) {
    return classify_result_kind(kind);
}

bool sol_mir_runtime_conventions_test_build_call_failure_mask(
    const SolMirRuntimeCall *call, const SolMirLinkage *linkage,
    uint32_t *mask) {
    return call != NULL && linkage != NULL && mask != NULL
        && build_call_failure_mask(call, linkage, mask);
}
#endif

static bool popcount_operations(uint32_t operations, size_t *result) {
    size_t count = 0;
    for (uint32_t bit = 1; bit <= SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT;
        bit <<= 1) {
        if (!build_event(1)) return false;
        count += (operations & bit) != 0;
    }
    *result = count;
    return true;
}

typedef struct {
    size_t signatures, slots, calls, operands, writebacks, entries, imports;
    size_t failure_sites;
} Counts;

static bool count_records(const SolMirConcreteProgram *c, unsigned char *indirect,
    Counts *counts, SolMirRuntimeConventionsBuildOutcome *outcome) {
    const SolMirMaterialization *m = &c->materialization;
    const SolMirRepresentation *r = &c->representation;
    const SolMirOperations *o = &c->operations;
    const SolMirLinkage *l = &c->linkage;
    memset(counts, 0, sizeof(*counts));
    if (!add_resource_size(&counts->signatures, l->callable_count)
        || !add_resource_size(&counts->signatures,
            l->host_requirement_count)) return false;
    counts->entries = l->entry_export_count;
    if (counts->entries > 1) {
        *outcome = SOL_MIR_RUNTIME_CONVENTIONS_BUILD_UNSUPPORTED_CALLABLE;
        return false;
    }
    for (size_t i = 0; i < l->entry_export_count; ++i) {
        if (!build_event(1)) return false;
        if (l->entry_exports[i].callable >= l->callable_count) return false;
        SolMirPlanInstanceId instance
            = l->callables[l->entry_exports[i].callable].instance;
        const SolMirMaterializedImage *image = NULL;
        for (size_t q = 0; q < m->image_count; ++q) {
            if (!build_event(1)) return false;
            if (m->images[q].instance == instance) image = &m->images[q];
        }
        if (image == NULL || image->receiver != SOL_MIR_RECIPE_NONE)
            goto unsupported_entry;
        for (size_t q = 0; q < image->parameter_types.count; ++q) {
            if (!build_event(1)) return false;
            SolMirRecipeId recipe
                = m->type_ids[image->parameter_types.offset + q];
            if (recipe >= r->recipe_count
                || m->accesses[image->parameter_accesses.offset + q]
                    != SOL_ACCESS_OWNED
                || r->recipes[recipe].kind != SOL_MIR_RECIPE_CAPABILITY)
                goto unsupported_entry;
        }
        if (image->result >= r->recipe_count
            || (r->recipes[image->result].kind != SOL_MIR_RECIPE_UNIT
                && r->recipes[image->result].kind != SOL_MIR_RECIPE_INT64))
            goto unsupported_entry;
        continue;
unsupported_entry:
        *outcome = SOL_MIR_RUNTIME_CONVENTIONS_BUILD_UNSUPPORTED_CALLABLE;
        return false;
    }
    for (size_t i = 0; i < l->runtime_requirement_count; ++i) {
        size_t operation_count;
        if (!build_event(1)
            || !popcount_operations(l->runtime_requirements[i].operations,
                &operation_count)
            || !add_resource_size(&counts->imports, operation_count))
            return false;
    }
    if (!add_resource_size(&counts->imports, l->host_requirement_count))
        return false;
    for (size_t i = 0; i < m->block_count; ++i) {
        if (!build_event(1)) return false;
        const SolMirMaterializedTerminator *term = &m->blocks[i].terminator;
        if (term->kind != SOL_MIR_TERM_INVOKE) {
            if ((term->kind == SOL_MIR_TERM_PANIC
                    || term->kind == SOL_MIR_TERM_MATCH_FAILURE
                    || term->kind == SOL_MIR_TERM_UNREACHABLE)
                && !add_resource_size(&counts->failure_sites, 1)) return false;
            continue;
        }
        if (classify_call_kind(term->call_kind)
                != SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED) {
            *outcome = SOL_MIR_RUNTIME_CONVENTIONS_BUILD_UNSUPPORTED_CALLABLE;
            return false;
        }
        if (!add_resource_size(&counts->calls, 1)
            || !add_resource_size(&counts->failure_sites, 1)
            || !add_resource_size(&counts->operands, term->arguments.count)
            || !add_resource_size(&counts->writebacks,
                term->writebacks.count)) return false;
        if (term->receiver.source_expression != SOL_IR_NONE
            && !add_resource_size(&counts->operands, 1)) return false;
        if (term->call_kind == SOL_IR_CALL_CALLBACK) {
            SolMirRecipeId recipe = m->temporaries[term->callee].type;
            if (recipe >= r->recipe_count) return false;
            indirect[recipe] = 1;
        }
    }
    for (size_t i = 0; i < o->predicate_block_count; ++i) {
        if (!build_event(1)) return false;
        const SolMirPredicateTerminator *term
            = &o->predicate_blocks[i].terminator;
        if (term->kind != SOL_MIR_PREDICATE_TERM_INVOKE) {
            if (term->kind == SOL_MIR_PREDICATE_TERM_FAILURE
                && term->failure_kind == SOL_MIR_PREDICATE_FAILURE_NO_MATCH
                && !add_resource_size(&counts->failure_sites, 1)) return false;
            continue;
        }
        if (classify_call_kind(term->call_kind)
                != SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED) {
            *outcome = SOL_MIR_RUNTIME_CONVENTIONS_BUILD_UNSUPPORTED_CALLABLE;
            return false;
        }
        if (term->result_recipe >= r->recipe_count
            || r->recipes[term->result_recipe].kind == SOL_MIR_RECIPE_NEVER) {
            *outcome = SOL_MIR_RUNTIME_CONVENTIONS_BUILD_UNSUPPORTED_CALLABLE;
            return false;
        }
        if (!add_resource_size(&counts->calls, 1)
            || !add_resource_size(&counts->failure_sites, 1)
            || !add_resource_size(&counts->operands, term->arguments.count))
            return false;
        if (term->receiver != SOL_MIR_OPERATION_NONE
            || term->call_kind == SOL_IR_CALL_CAPABILITY) {
            if (!add_resource_size(&counts->operands, 1)) return false;
        }
        if (term->call_kind == SOL_IR_CALL_CALLBACK) {
            SolMirRecipeId recipe = o->predicate_values[term->callee].recipe;
            if (recipe >= r->recipe_count) return false;
            indirect[recipe] = 1;
        }
    }
    for (size_t i = 0; i < o->arithmetic_count; ++i) {
        if (!build_event(1)) return false;
        if (arithmetic_failure_mask(o->arithmetic[i].failures) != 0
            && !add_resource_size(&counts->failure_sites, 1)) return false;
    }
    for (size_t i = 0; i < o->predicate_instruction_count; ++i) {
        if (!build_event(1)) return false;
        if (arithmetic_failure_mask(o->predicate_instructions[i].failures) != 0
            && !add_resource_size(&counts->failure_sites, 1)) return false;
    }
    for (size_t i = 0; i < o->predicate_body_count; ++i) {
        if (!build_event(1)
            || !add_resource_size(&counts->failure_sites, 1)) return false;
    }
    for (size_t i = 0; i < r->recipe_count; ++i) {
        if (!build_event(1)) return false;
        if (!indirect[i]) continue;
        const SolMirRecipe *recipe = &r->recipes[i];
        if (recipe->kind != SOL_MIR_RECIPE_FUNCTION) return false;
        if (!add_resource_size(&counts->signatures, 1)
            || !add_resource_size(&counts->slots, recipe->parameters.count))
            return false;
    }
    for (size_t i = 0; i < l->callable_count; ++i) {
        if (!build_event(1)) return false;
        const SolMirMaterializedImage *image = NULL;
        for (size_t q = 0; q < m->image_count; ++q) {
            if (!build_event(1)) return false;
            if (m->images[q].instance == l->callables[i].instance) image = &m->images[q];
        }
        if (image == NULL
            || !add_resource_size(&counts->slots,
                image->parameter_types.count)
            || (image->receiver != SOL_MIR_MATERIALIZED_NONE
                && !add_resource_size(&counts->slots, 1))) return false;
    }
    for (size_t i = 0; i < l->host_requirement_count; ++i) {
        if (!build_event(1)) return false;
        if (!add_resource_size(&counts->slots,
                l->host_requirements[i].parameters.count)
            || (l->host_requirements[i].receiver != SOL_MIR_RECIPE_NONE
                && !add_resource_size(&counts->slots, 1)))
            return false;
    }
    return true;
}

static bool append_slot(SolMirRuntimeConventions *out, SolMirRuntimeSlotRole role,
    size_t formal, SolMirRecipeId recipe, SolAccessMode access) {
    if (!build_event(1)) return false;
    if (out->signature_slot_count >= out->signature_slot_capacity) return false;
    out->signature_slots[out->signature_slot_count++]
        = (SolMirRuntimeSignatureSlot){role, formal, recipe, access};
    return true;
}

static bool append_signature(SolMirRuntimeConventions *out,
    SolMirRuntimeSignature signature, SolMirRecipeId receiver,
    SolAccessMode receiver_access, SolMirPlanSlice parameters,
    SolMirPlanSlice accesses, const SolMirRecipeId *recipe_ids,
    const SolAccessMode *access_values) {
    signature.slots.offset = out->signature_slot_count;
    signature.slots.count = parameters.count
        + (receiver != SOL_MIR_RECIPE_NONE);
    if (receiver != SOL_MIR_RECIPE_NONE
        && !append_slot(out, SOL_MIR_RUNTIME_SLOT_RECEIVER,
            SOL_MIR_RUNTIME_NONE, receiver, receiver_access)) return false;
    for (size_t i = 0; i < parameters.count; ++i) {
        if (!build_event(1)) return false;
        if (!append_slot(out, SOL_MIR_RUNTIME_SLOT_PARAMETER, i,
                recipe_ids[parameters.offset + i],
                access_values[accesses.offset + i])) return false;
    }
    if (!build_event(1)) return false;
    if (out->signature_count >= out->signature_capacity) return false;
    out->signatures[out->signature_count++] = signature;
    return true;
}

static bool populate_signatures(SolMirRuntimeConventions *out,
    const unsigned char *indirect) {
    const SolMirConcreteProgram *c = out->concrete;
    const SolMirMaterialization *m = &c->materialization;
    const SolMirRepresentation *r = &c->representation;
    const SolMirLinkage *l = &c->linkage;
    for (size_t i = 0; i < l->callable_count; ++i) {
        if (!build_event(1)) return false;
        const SolMirMaterializedImage *image = NULL;
        for (size_t q = 0; q < m->image_count; ++q) {
            if (!build_event(1)) return false;
            if (m->images[q].instance == l->callables[i].instance) image = &m->images[q];
        }
        if (image == NULL) return false;
        SolMirRuntimeSignature signature = {
            .origin = SOL_MIR_RUNTIME_SIGNATURE_INTERNAL,
            .internal = i, .host = SOL_MIR_RUNTIME_NONE,
            .function_recipe = SOL_MIR_RECIPE_NONE,
            .result = image->result,
            .result_class = result_class(r, image->result),
            .effects = image->effects,
        };
        if (!append_signature(out, signature, image->receiver,
                image->receiver_access, image->parameter_types,
                image->parameter_accesses, m->type_ids, m->accesses)) return false;
    }
    for (size_t i = 0; i < l->host_requirement_count; ++i) {
        if (!build_event(1)) return false;
        const SolMirLinkageHostRequirement *host = &l->host_requirements[i];
        SolMirRuntimeSignature signature = {
            .origin = SOL_MIR_RUNTIME_SIGNATURE_HOST,
            .internal = SOL_MIR_RUNTIME_NONE, .host = i,
            .function_recipe = SOL_MIR_RECIPE_NONE,
            .result = host->result,
            .result_class = result_class(r, host->result),
            .effects = host->effects,
        };
        if (!append_signature(out, signature, host->receiver,
                host->receiver_access, host->parameters,
                host->parameter_accesses, m->type_ids, m->accesses)) return false;
    }
    for (size_t i = 0; i < r->recipe_count; ++i) {
        if (!build_event(1)) return false;
        if (!indirect[i]) continue;
        const SolMirRecipe *recipe = &r->recipes[i];
        SolMirRuntimeSignature signature = {
            .origin = SOL_MIR_RUNTIME_SIGNATURE_FUNCTION_RECIPE,
            .internal = SOL_MIR_RUNTIME_NONE, .host = SOL_MIR_RUNTIME_NONE,
            .function_recipe = i, .result = recipe->result,
            .result_class = result_class(r, recipe->result),
            .effects = recipe->effects,
        };
        if (!append_signature(out, signature, SOL_MIR_RECIPE_NONE,
                SOL_ACCESS_OWNED, recipe->parameters,
                recipe->parameter_accesses, r->recipe_ids, r->accesses)) return false;
    }
    return true;
}

static bool direct_signature(const SolMirRuntimeConventions *out,
    SolMirMaterializedBindingId binding, SolMirRuntimeSignatureId *result) {
    if (!build_event(1)) return false;
    const SolMirLinkage *l = &out->concrete->linkage;
    if (binding >= l->binding_count) return false;
    const SolMirLinkageBinding *target = &l->bindings[binding];
    *result = target->target_kind == SOL_MIR_LINKAGE_TARGET_INTERNAL
        ? target->internal : l->callable_count + target->host;
    return true;
}

static bool indirect_signature(const SolMirRuntimeConventions *out,
    SolMirRecipeId recipe, SolMirRuntimeSignatureId *result) {
    for (size_t i = 0; i < out->signature_count; ++i) {
        if (!build_event(1)) return false;
        if (out->signatures[i].origin
                == SOL_MIR_RUNTIME_SIGNATURE_FUNCTION_RECIPE
            && out->signatures[i].function_recipe == recipe) {
            *result = i;
            return true;
        }
    }
    return false;
}

static bool table_for_site(const SolMirConcreteProgram *c,
    SolMirMaterializedSemanticSiteId site, SolMirLinkageTableId *result) {
    for (size_t i = 0; i < c->operations.callable_count; ++i) {
        if (!build_event(1)) return false;
        if (c->operations.callables[i].semantic_site == site) {
            *result = c->linkage.callable_values[i].table;
            return true;
        }
    }
    return false;
}

static bool table_for_predicate_value(const SolMirConcreteProgram *c,
    SolMirPredicateValueId value, SolMirLinkageTableId *result) {
    const SolMirOperations *o = &c->operations;
    if (value >= o->predicate_value_count) return false;
    const SolMirPredicateValue *v = &o->predicate_values[value];
    if (v->kind != SOL_MIR_PREDICATE_VALUE_INSTRUCTION
        || v->definition >= o->predicate_instruction_count)
        return false;
    const SolMirPredicateInstruction *instruction
        = &o->predicate_instructions[v->definition];
    if (instruction->kind != SOL_MIR_PREDICATE_INST_FUNCTION
        && instruction->kind != SOL_MIR_PREDICATE_INST_BOUND_OPERATION)
        return false;
    for (size_t i = 0; i < o->callable_count; ++i) {
        if (!build_event(1)) return false;
        const SolMirOperationCallablePlan *plan = &o->callables[i];
        if (plan->function_recipe == v->recipe
            && c->materialization.semantic_sites[plan->semantic_site].binding
                == instruction->binding)
            {
                *result = c->linkage.callable_values[i].table;
                return true;
            }
    }
    return false;
}

static bool set_direct_target(const SolMirRuntimeConventions *out,
    SolMirMaterializedBindingId binding, SolMirRuntimeCall *call) {
    if (!build_category_event(SOL_MIR_RUNTIME_BUILD_TEST_TARGET, 1)) return false;
    const SolMirLinkageBinding *target = &out->concrete->linkage.bindings[binding];
    call->internal = SOL_MIR_LINKAGE_NONE;
    call->host = SOL_MIR_LINKAGE_NONE;
    call->table = SOL_MIR_LINKAGE_NONE;
    if (target->target_kind == SOL_MIR_LINKAGE_TARGET_INTERNAL) {
        call->target_kind = SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL;
        call->internal = target->internal;
    } else {
        call->target_kind = SOL_MIR_RUNTIME_TARGET_DIRECT_HOST;
        call->host = target->host;
    }
    return true;
}

static bool append_operand(SolMirRuntimeConventions *out,
    SolMirRuntimeSignatureSlotId slot, SolMirRuntimeValueKind kind, size_t id) {
    if (!build_event(1)) return false;
    if (out->operand_count >= out->operand_capacity) return false;
    out->operands[out->operand_count++] = (SolMirRuntimeOperand){slot, {kind, id}};
    return true;
}

static bool populate_image_calls(SolMirRuntimeConventions *out) {
    const SolMirConcreteProgram *c = out->concrete;
    const SolMirMaterialization *m = &c->materialization;
    const SolMirRepresentation *r = &c->representation;
    for (size_t image_id = 0; image_id < m->image_count; ++image_id) {
        if (!build_event(1)) return false;
        const SolMirMaterializedImage *image = &m->images[image_id];
        for (size_t q = 0; q < image->blocks.count; ++q) {
            if (!build_event(1)) return false;
            size_t block_id = image->blocks.offset + q;
            const SolMirMaterializedTerminator *term
                = &m->blocks[block_id].terminator;
            if (term->kind != SOL_MIR_TERM_INVOKE) continue;
            SolMirRuntimeCall call = {0};
            call.owner_kind = SOL_MIR_RUNTIME_CALL_OWNER_IMAGE;
            call.image = image_id; call.predicate = SOL_MIR_RUNTIME_NONE;
            call.block = block_id; call.call_kind = term->call_kind;
            call.callee = (SolMirRuntimeValueRef){SOL_MIR_RUNTIME_VALUE_NONE,
                SOL_MIR_RUNTIME_NONE};
            call.operands.offset = out->operand_count;
            call.writebacks.offset = out->writeback_count;
            call.normal_edge = term->normal_edge;
            call.failure_edge = term->failure_edge;
            call.failure_site = out->call_count;
            if (term->call_kind == SOL_IR_CALL_CALLBACK) {
                SolMirRecipeId recipe = m->temporaries[term->callee].type;
                if (!indirect_signature(out, recipe, &call.signature))
                    return false;
                call.target_kind = SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE;
                call.internal = SOL_MIR_LINKAGE_NONE;
                call.host = SOL_MIR_LINKAGE_NONE;
                if (!table_for_site(c, term->callable_site, &call.table))
                    return false;
                call.callee = (SolMirRuntimeValueRef){
                    SOL_MIR_RUNTIME_VALUE_MATERIALIZED_TEMPORARY, term->callee};
            } else {
                if (!direct_signature(out, term->binding, &call.signature)
                    || !set_direct_target(out, term->binding, &call))
                    return false;
            }
            if (call.signature >= out->signature_count
                || (term->call_kind == SOL_IR_CALL_CALLBACK
                    && call.table >= c->linkage.table_entry_count)) return false;
            const SolMirRuntimeSignature *signature
                = &out->signatures[call.signature];
            size_t operand_ordinal = 0;
            if (term->receiver.source_expression != SOL_IR_NONE) {
                SolMirRuntimeValueKind kind = term->receiver.access
                        == SOL_ACCESS_OWNED
                    ? SOL_MIR_RUNTIME_VALUE_MATERIALIZED_TEMPORARY
                    : SOL_MIR_RUNTIME_VALUE_MATERIALIZED_PLACE;
                size_t id = term->receiver.access == SOL_ACCESS_OWNED
                    ? term->receiver.temporary : term->receiver.place;
                if (!append_operand(out, signature->slots.offset, kind, id))
                    return false;
                ++operand_ordinal;
            }
            for (size_t a = 0; a < term->arguments.count; ++a) {
                if (!build_event(1)) return false;
                const SolMirMaterializedCallArgument *argument
                    = &m->call_arguments[term->arguments.offset + a];
                SolMirRuntimeValueKind kind = argument->access == SOL_ACCESS_OWNED
                    ? SOL_MIR_RUNTIME_VALUE_MATERIALIZED_TEMPORARY
                    : SOL_MIR_RUNTIME_VALUE_MATERIALIZED_PLACE;
                size_t id = argument->access == SOL_ACCESS_OWNED
                    ? argument->temporary : argument->place;
                if (!append_operand(out, signature->slots.offset
                        + operand_ordinal++, kind, id)) return false;
            }
            call.operands.count = out->operand_count - call.operands.offset;
            SolMirRuntimeResultClass class_ = signature->result_class;
            call.result = class_ == SOL_MIR_RUNTIME_RESULT_VALUE
                ? (SolMirRuntimeValueRef){SOL_MIR_RUNTIME_VALUE_MATERIALIZED_VALUE,
                    term->result}
                : (SolMirRuntimeValueRef){SOL_MIR_RUNTIME_VALUE_NONE,
                    SOL_MIR_RUNTIME_NONE};
            for (size_t w = 0; w < term->writebacks.count; ++w) {
                if (!build_event(1)) return false;
                const SolMirMaterializedWriteback *source
                    = &m->writebacks[term->writebacks.offset + w];
                size_t relative = source->receiver ? 0
                    : (signature->slots.count != term->arguments.count) + source->formal;
                if (relative >= call.operands.count
                    || out->writeback_count >= out->writeback_capacity) return false;
                out->writebacks[out->writeback_count++] = (SolMirRuntimeWriteback){
                    call.operands.offset + relative, source->receiver,
                    source->formal, source->place, source->type};
            }
            call.writebacks.count = out->writeback_count - call.writebacks.offset;
            if (out->call_count >= out->call_capacity) return false;
            if (!build_event(1)) return false;
            out->calls[out->call_count++] = call;
        }
    }
    (void)r;
    return true;
}

static bool populate_predicate_calls(SolMirRuntimeConventions *out) {
    const SolMirConcreteProgram *c = out->concrete;
    const SolMirOperations *o = &c->operations;
    for (size_t body_id = 0; body_id < o->predicate_body_count; ++body_id) {
        if (!build_event(1)) return false;
        const SolMirPredicateBody *body = &o->predicate_bodies[body_id];
        for (size_t q = 0; q < body->blocks.count; ++q) {
            if (!build_event(1)) return false;
            size_t block_id = body->blocks.offset + q;
            const SolMirPredicateTerminator *term
                = &o->predicate_blocks[block_id].terminator;
            if (term->kind != SOL_MIR_PREDICATE_TERM_INVOKE) continue;
            SolMirRuntimeCall call = {0};
            call.owner_kind = SOL_MIR_RUNTIME_CALL_OWNER_PREDICATE;
            call.image = SOL_MIR_RUNTIME_NONE; call.predicate = body_id;
            call.block = block_id; call.call_kind = term->call_kind;
            if (term->binding >= c->materialization.binding_count) return false;
            call.callee = (SolMirRuntimeValueRef){SOL_MIR_RUNTIME_VALUE_NONE,
                SOL_MIR_RUNTIME_NONE};
            call.operands.offset = out->operand_count;
            call.writebacks = (SolMirRuntimeSlice){out->writeback_count, 0};
            call.normal_edge = term->normal_edge;
            call.failure_edge = term->failure_edge;
            call.failure_site = out->call_count;
            if (term->call_kind == SOL_IR_CALL_CALLBACK) {
                SolMirRecipeId recipe = o->predicate_values[term->callee].recipe;
                if (!indirect_signature(out, recipe, &call.signature))
                    return false;
                call.target_kind = SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE;
                call.internal = SOL_MIR_LINKAGE_NONE;
                call.host = SOL_MIR_LINKAGE_NONE;
                if (!table_for_predicate_value(c, term->callee, &call.table))
                    return false;
                call.callee = (SolMirRuntimeValueRef){
                    SOL_MIR_RUNTIME_VALUE_PREDICATE_VALUE, term->callee};
            } else {
                if (!direct_signature(out, term->binding, &call.signature)
                    || !set_direct_target(out, term->binding, &call))
                    return false;
            }
            if (call.signature >= out->signature_count
                || (term->call_kind == SOL_IR_CALL_CALLBACK
                    && call.table >= c->linkage.table_entry_count)) return false;
            const SolMirRuntimeSignature *signature
                = &out->signatures[call.signature];
            size_t ordinal = 0;
            if (term->receiver != SOL_MIR_OPERATION_NONE
                || term->call_kind == SOL_IR_CALL_CAPABILITY) {
                size_t receiver = term->call_kind == SOL_IR_CALL_CAPABILITY
                    ? term->callee : term->receiver;
                SolMirRuntimeValueKind kind = term->call_kind
                        == SOL_IR_CALL_CAPABILITY
                    ? SOL_MIR_RUNTIME_VALUE_BOUND_RECEIVER
                    : SOL_MIR_RUNTIME_VALUE_PREDICATE_VALUE;
                if (!append_operand(out, signature->slots.offset, kind, receiver))
                    return false;
                ++ordinal;
            }
            for (size_t a = 0; a < term->arguments.count; ++a) {
                if (!build_event(1)) return false;
                const SolMirPredicateOperand *argument
                    = &o->predicate_operands[term->arguments.offset + a];
                if (!append_operand(out, signature->slots.offset + ordinal++,
                        SOL_MIR_RUNTIME_VALUE_PREDICATE_VALUE,
                        argument->value)) return false;
            }
            call.operands.count = out->operand_count - call.operands.offset;
            call.result = signature->result_class == SOL_MIR_RUNTIME_RESULT_VALUE
                ? (SolMirRuntimeValueRef){SOL_MIR_RUNTIME_VALUE_PREDICATE_VALUE,
                    term->result}
                : (SolMirRuntimeValueRef){SOL_MIR_RUNTIME_VALUE_NONE,
                    SOL_MIR_RUNTIME_NONE};
            if (out->call_count >= out->call_capacity) return false;
            if (!build_event(1)) return false;
            out->calls[out->call_count++] = call;
        }
    }
    return true;
}

static bool append_failure_site(SolMirRuntimeConventions *out,
    SolMirRuntimeFailureOriginKind origin_kind, size_t owner, size_t block,
    size_t instruction, SolSpan span, uint32_t allowed_codes) {
    if (!build_event(1) || allowed_codes == 0
        || out->failure_site_count >= out->failure_site_capacity) return false;
    SolMirRuntimeSource source;
    if (!runtime_source_from_span(out->concrete->program.ir, span, &source))
        return false;
    out->failure_sites[out->failure_site_count++] = (SolMirRuntimeFailureSite){
        origin_kind, owner, block, instruction, source, allowed_codes};
    return true;
}

static SolMirOperationOpcode predicate_source_opcode(SolTokenKind token,
    bool unary, unsigned *failures) {
    *failures = SOL_MIR_OPERATION_FAILURE_NONE;
    switch (token) {
        case SOL_TOKEN_BANG: return SOL_MIR_OPERATION_BOOL_NOT;
        case SOL_TOKEN_MINUS:
            *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW;
            return unary ? SOL_MIR_OPERATION_I64_NEG : SOL_MIR_OPERATION_I64_SUB;
        case SOL_TOKEN_PLUS:
            *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW;
            return SOL_MIR_OPERATION_I64_ADD;
        case SOL_TOKEN_STAR:
            *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW;
            return SOL_MIR_OPERATION_I64_MUL;
        case SOL_TOKEN_SLASH:
            *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW
                | SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO;
            return SOL_MIR_OPERATION_I64_DIV;
        case SOL_TOKEN_PERCENT:
            *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW
                | SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO;
            return SOL_MIR_OPERATION_I64_REM;
        case SOL_TOKEN_LESS: return SOL_MIR_OPERATION_I64_LT;
        case SOL_TOKEN_LESS_EQUAL: return SOL_MIR_OPERATION_I64_LE;
        case SOL_TOKEN_GREATER: return SOL_MIR_OPERATION_I64_GT;
        case SOL_TOKEN_GREATER_EQUAL: return SOL_MIR_OPERATION_I64_GE;
        case SOL_TOKEN_EQUAL_EQUAL: return SOL_MIR_OPERATION_VALUE_EQ;
        case SOL_TOKEN_BANG_EQUAL: return SOL_MIR_OPERATION_VALUE_NE;
        default: return (SolMirOperationOpcode)-1;
    }
}

typedef struct {
    SolMirRuntimeConventions *out;
    const SolMirOperations *operations;
    const SolIr *ir;
    const SolMirPredicateBody *body;
    size_t body_id;
    size_t allocated_blocks;
    size_t block;
    size_t instruction_at;
} PredicateSiteReplay;

static size_t replay_new_block(PredicateSiteReplay *replay) {
    if (replay->allocated_blocks >= replay->body->blocks.count)
        return SOL_MIR_OPERATION_NONE;
    size_t block = replay->body->blocks.offset + replay->allocated_blocks++;
    return replay->operations->predicate_blocks[block].body == replay->body_id
        ? block : SOL_MIR_OPERATION_NONE;
}

static bool replay_start_block(PredicateSiteReplay *replay, size_t block) {
    if (block < replay->body->blocks.offset
        || block >= replay->body->blocks.offset + replay->body->blocks.count)
        return false;
    replay->block = block;
    replay->instruction_at = 0;
    return true;
}

static const SolMirPredicateInstruction *replay_instruction(
    PredicateSiteReplay *replay, SolMirPredicateInstructionKind kind) {
    const SolMirPredicateBlock *block
        = &replay->operations->predicate_blocks[replay->block];
    if (replay->instruction_at >= block->instructions.count) return NULL;
    size_t id = block->instructions.offset + replay->instruction_at++;
    const SolMirPredicateInstruction *instruction
        = &replay->operations->predicate_instructions[id];
    return instruction->block == replay->block && instruction->kind == kind
        ? instruction : NULL;
}

static bool replay_edge_target(const PredicateSiteReplay *replay, size_t edge,
    size_t target) {
    return edge < replay->operations->predicate_edge_count
        && replay->operations->predicate_edges[edge].source == replay->block
        && replay->operations->predicate_edges[edge].target == target;
}

static bool replay_end_block(PredicateSiteReplay *replay,
    SolMirPredicateTerminatorKind kind) {
    const SolMirPredicateBlock *block
        = &replay->operations->predicate_blocks[replay->block];
    return replay->instruction_at == block->instructions.count
        && block->terminator.kind == kind;
}

static bool replay_failure_block(PredicateSiteReplay *replay, size_t block,
    SolMirPredicateFailureKind kind) {
    const SolMirPredicateBlock *failure
        = &replay->operations->predicate_blocks[block];
    return failure->body == replay->body_id && failure->instructions.count == 0
        && failure->terminator.kind == SOL_MIR_PREDICATE_TERM_FAILURE
        && failure->terminator.failure_kind == kind;
}

static bool replay_predicate_expression(PredicateSiteReplay *replay,
    SolIrExpressionId expression, size_t depth);

static bool replay_predicate_match(PredicateSiteReplay *replay,
    SolIrExpressionId expression, const SolIrExpression *source, size_t depth) {
    if (!replay_predicate_expression(replay,
            source->as.match_expr.scrutinee, depth + 1)) return false;
    size_t join = replay_new_block(replay);
    if (join == SOL_MIR_OPERATION_NONE) return false;
    size_t next = replay->block;
    for (size_t i = 0; i < source->as.match_expr.arms.count; ++i) {
        if (i != 0 && !replay_start_block(replay, next)) return false;
        size_t arm_id = replay->ir->arm_ids[source->as.match_expr.arms.offset + i];
        const SolIrArm *arm = &replay->ir->arms[arm_id];
        if (replay_instruction(replay, SOL_MIR_PREDICATE_INST_PATTERN_TEST)
                == NULL) return false;
        size_t arm_block = replay_new_block(replay);
        size_t miss_block = replay_new_block(replay);
        if (arm_block == SOL_MIR_OPERATION_NONE
            || miss_block == SOL_MIR_OPERATION_NONE
            || !replay_end_block(replay, SOL_MIR_PREDICATE_TERM_BRANCH))
            return false;
        const SolMirPredicateTerminator *branch
            = &replay->operations->predicate_blocks[replay->block].terminator;
        if (!replay_edge_target(replay, branch->true_edge, arm_block)
            || !replay_edge_target(replay, branch->false_edge, miss_block)
            || !replay_start_block(replay, arm_block)) return false;
        for (size_t q = 0; q < arm->bindings.count; ++q)
            if (replay_instruction(replay,
                    SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT) == NULL) return false;
        if (arm->guard != SOL_IR_NONE) {
            if (!replay_predicate_expression(replay, arm->guard, depth + 1))
                return false;
            size_t body_block = replay_new_block(replay);
            if (body_block == SOL_MIR_OPERATION_NONE
                || !replay_end_block(replay, SOL_MIR_PREDICATE_TERM_BRANCH))
                return false;
            branch = &replay->operations->predicate_blocks[replay->block].terminator;
            if (!replay_edge_target(replay, branch->true_edge, body_block)
                || !replay_edge_target(replay, branch->false_edge, miss_block)
                || !replay_start_block(replay, body_block)) return false;
        }
        if (!replay_predicate_expression(replay, arm->body, depth + 1)
            || !replay_end_block(replay, SOL_MIR_PREDICATE_TERM_JUMP))
            return false;
        const SolMirPredicateTerminator *jump
            = &replay->operations->predicate_blocks[replay->block].terminator;
        if (!replay_edge_target(replay, jump->edge, join)) return false;
        next = miss_block;
    }
    if (!replay_start_block(replay, next)
        || !replay_end_block(replay, SOL_MIR_PREDICATE_TERM_FAILURE)) return false;
    const SolMirPredicateTerminator *failure
        = &replay->operations->predicate_blocks[next].terminator;
    if (failure->failure_kind != SOL_MIR_PREDICATE_FAILURE_NO_MATCH
        || !append_failure_site(replay->out,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_NO_MATCH,
            replay->body_id, next, SOL_MIR_RUNTIME_NONE,
            replay->ir->expressions[expression].span,
            failure_code_bit(SOL_MIR_RUNTIME_FAILURE_NO_MATCH))) return false;
    return replay_start_block(replay, join);
}

static bool replay_predicate_expression(PredicateSiteReplay *replay,
    SolIrExpressionId expression, size_t depth) {
    if (expression >= replay->ir->expression_count
        || depth > replay->ir->expression_count) return false;
    const SolIrExpression *source = &replay->ir->expressions[expression];
    if (source->kind == SOL_IR_EXPR_PLACE) {
        const SolIrPlace *place = &replay->ir->places[source->as.place];
        if (place->root_kind == SOL_IR_PLACE_ROOT_TEMPORARY
            && !replay_predicate_expression(replay, place->temporary, depth + 1))
            return false;
        return place->projections.count == 0
            || replay_instruction(replay, SOL_MIR_PREDICATE_INST_PROJECT) != NULL;
    }
    if (source->kind == SOL_IR_EXPR_RESULT
        || source->kind == SOL_IR_EXPR_SNAPSHOT_READ
        || source->kind == SOL_IR_EXPR_REFINEMENT_SELF) return true;
    if (source->kind == SOL_IR_EXPR_BINARY
        && (source->as.binary.operator_kind == SOL_TOKEN_AMP_AMP
            || source->as.binary.operator_kind == SOL_TOKEN_PIPE_PIPE)) {
        if (!replay_predicate_expression(replay, source->as.binary.left,
                depth + 1)) return false;
        size_t branch_block = replay->block;
        size_t rhs = replay_new_block(replay), shortcut = replay_new_block(replay);
        size_t join = replay_new_block(replay);
        if (rhs == SOL_MIR_OPERATION_NONE || shortcut == SOL_MIR_OPERATION_NONE
            || join == SOL_MIR_OPERATION_NONE
            || !replay_end_block(replay, SOL_MIR_PREDICATE_TERM_BRANCH))
            return false;
        const SolMirPredicateTerminator *branch
            = &replay->operations->predicate_blocks[branch_block].terminator;
        bool is_and = source->as.binary.operator_kind == SOL_TOKEN_AMP_AMP;
        if (!replay_edge_target(replay, branch->true_edge,
                is_and ? rhs : shortcut)
            || !replay_edge_target(replay, branch->false_edge,
                is_and ? shortcut : rhs)
            || !replay_start_block(replay, rhs)
            || !replay_predicate_expression(replay, source->as.binary.right,
                depth + 1)
            || !replay_end_block(replay, SOL_MIR_PREDICATE_TERM_JUMP))
            return false;
        const SolMirPredicateTerminator *jump
            = &replay->operations->predicate_blocks[replay->block].terminator;
        if (!replay_edge_target(replay, jump->edge, join)
            || !replay_start_block(replay, shortcut)
            || replay_instruction(replay, SOL_MIR_PREDICATE_INST_BOOL) == NULL
            || !replay_end_block(replay, SOL_MIR_PREDICATE_TERM_JUMP)) return false;
        jump = &replay->operations->predicate_blocks[replay->block].terminator;
        return replay_edge_target(replay, jump->edge, join)
            && replay_start_block(replay, join);
    }
    if (source->kind == SOL_IR_EXPR_IF) {
        if (!replay_predicate_expression(replay, source->as.if_expr.condition,
                depth + 1)) return false;
        size_t branch_block = replay->block;
        size_t then_block = replay_new_block(replay);
        size_t else_block = replay_new_block(replay);
        size_t join = replay_new_block(replay);
        if (then_block == SOL_MIR_OPERATION_NONE
            || else_block == SOL_MIR_OPERATION_NONE
            || join == SOL_MIR_OPERATION_NONE
            || !replay_end_block(replay, SOL_MIR_PREDICATE_TERM_BRANCH))
            return false;
        const SolMirPredicateTerminator *branch
            = &replay->operations->predicate_blocks[branch_block].terminator;
        if (!replay_edge_target(replay, branch->true_edge, then_block)
            || !replay_edge_target(replay, branch->false_edge, else_block)
            || !replay_start_block(replay, then_block)
            || !replay_predicate_expression(replay,
                source->as.if_expr.then_branch, depth + 1)
            || !replay_end_block(replay, SOL_MIR_PREDICATE_TERM_JUMP))
            return false;
        const SolMirPredicateTerminator *jump
            = &replay->operations->predicate_blocks[replay->block].terminator;
        if (!replay_edge_target(replay, jump->edge, join)
            || !replay_start_block(replay, else_block)
            || !replay_predicate_expression(replay,
                source->as.if_expr.else_branch, depth + 1)
            || !replay_end_block(replay, SOL_MIR_PREDICATE_TERM_JUMP))
            return false;
        jump = &replay->operations->predicate_blocks[replay->block].terminator;
        return replay_edge_target(replay, jump->edge, join)
            && replay_start_block(replay, join);
    }
    if (source->kind == SOL_IR_EXPR_MATCH)
        return replay_predicate_match(replay, expression, source, depth);
    if (source->kind == SOL_IR_EXPR_BLOCK) {
        for (size_t i = 0; i < source->as.block.statements.count; ++i) {
            const SolIrStatement *statement = &replay->ir->statements[
                replay->ir->statement_ids[source->as.block.statements.offset + i]];
            if (!replay_predicate_expression(replay, statement->expression,
                    depth + 1)) return false;
            if (statement->kind == SOL_IR_STATEMENT_RETURN) break;
        }
        if (source->as.block.statements.count == 0)
            return replay_instruction(replay, SOL_MIR_PREDICATE_INST_UNIT) != NULL;
        return true;
    }
    if (source->kind == SOL_IR_EXPR_DEFINITION)
        return replay_instruction(replay, SOL_MIR_PREDICATE_INST_FUNCTION) != NULL;
    if (source->kind == SOL_IR_EXPR_BOUND_OPERATION) {
        return replay_predicate_expression(replay, source->as.operation.receiver,
                depth + 1)
            && replay_instruction(replay,
                SOL_MIR_PREDICATE_INST_BOUND_OPERATION) != NULL;
    }
    if (source->kind == SOL_IR_EXPR_CALL
        && source->as.call.kind <= SOL_IR_CALL_METHOD) {
        if ((source->as.call.kind == SOL_IR_CALL_CALLBACK
                || source->as.call.kind == SOL_IR_CALL_CAPABILITY)
            && !replay_predicate_expression(replay, source->as.call.callee,
                depth + 1)) return false;
        if (source->as.call.kind == SOL_IR_CALL_METHOD
            && !replay_predicate_expression(replay, source->as.call.receiver,
                depth + 1)) return false;
        for (size_t i = 0; i < source->as.call.operands.count; ++i)
            if (!replay_predicate_expression(replay, replay->ir->operands[
                    source->as.call.operands.offset + i].value, depth + 1))
                return false;
        size_t source_block = replay->block;
        size_t normal = replay_new_block(replay), failure = replay_new_block(replay);
        if (normal == SOL_MIR_OPERATION_NONE || failure == SOL_MIR_OPERATION_NONE
            || !replay_failure_block(replay, failure,
                SOL_MIR_PREDICATE_FAILURE_CALL)
            || !replay_end_block(replay, SOL_MIR_PREDICATE_TERM_INVOKE))
            return false;
        const SolMirPredicateTerminator *term
            = &replay->operations->predicate_blocks[source_block].terminator;
        if (term->binding >= replay->out->concrete->materialization.binding_count
            || replay->out->concrete->materialization.bindings[
                term->binding].source.expression != expression
            || !replay_edge_target(replay, term->normal_edge, normal)
            || !replay_edge_target(replay, term->failure_edge, failure))
            return false;
        return replay_start_block(replay, normal);
    }
    if (source->kind == SOL_IR_EXPR_PROPAGATE) {
        if (!replay_predicate_expression(replay, source->as.propagate.operand,
                depth + 1)) return false;
        size_t normal = replay_new_block(replay), failure = replay_new_block(replay);
        if (normal == SOL_MIR_OPERATION_NONE || failure == SOL_MIR_OPERATION_NONE
            || !replay_failure_block(replay, failure,
                SOL_MIR_PREDICATE_FAILURE_PROPAGATION)
            || !replay_end_block(replay, SOL_MIR_PREDICATE_TERM_PROPAGATE))
            return false;
        const SolMirPredicateTerminator *term
            = &replay->operations->predicate_blocks[replay->block].terminator;
        return replay_edge_target(replay, term->normal_edge, normal)
            && replay_edge_target(replay, term->failure_edge, failure)
            && replay_start_block(replay, normal);
    }
    SolMirPredicateInstructionKind kind;
    if (source->kind == SOL_IR_EXPR_INTEGER) kind = SOL_MIR_PREDICATE_INST_I64;
    else if (source->kind == SOL_IR_EXPR_BOOL) kind = SOL_MIR_PREDICATE_INST_BOOL;
    else if (source->kind == SOL_IR_EXPR_UNIT) kind = SOL_MIR_PREDICATE_INST_UNIT;
    else if (source->kind == SOL_IR_EXPR_STRING) kind = SOL_MIR_PREDICATE_INST_TEXT;
    else if (source->kind == SOL_IR_EXPR_UNARY) {
        if (!replay_predicate_expression(replay, source->as.unary.operand,
                depth + 1)) return false;
        kind = SOL_MIR_PREDICATE_INST_UNARY;
    } else if (source->kind == SOL_IR_EXPR_BINARY) {
        if (!replay_predicate_expression(replay, source->as.binary.left,
                depth + 1)
            || !replay_predicate_expression(replay, source->as.binary.right,
                depth + 1)) return false;
        kind = SOL_MIR_PREDICATE_INST_BINARY;
    } else if (source->kind == SOL_IR_EXPR_RECORD
        || source->kind == SOL_IR_EXPR_TUPLE
        || source->kind == SOL_IR_EXPR_VARIANT
        || (source->kind == SOL_IR_EXPR_CALL
            && source->as.call.kind >= SOL_IR_CALL_BUILTIN_OK)) {
        SolIrSlice operands = source->kind == SOL_IR_EXPR_RECORD
            ? source->as.record.fields : source->kind == SOL_IR_EXPR_TUPLE
            ? source->as.tuple.operands : source->kind == SOL_IR_EXPR_VARIANT
            ? (SolIrSlice){0, 0} : source->as.call.operands;
        if (source->kind == SOL_IR_EXPR_CALL
            && source->as.call.kind == SOL_IR_CALL_DISTINCT_CONSTRUCTOR
            && source->as.call.definition < replay->ir->definition_count
            && replay->ir->definitions[source->as.call.definition].kind
                == SOL_IR_DEFINITION_REFINED) {
            if (operands.count != 1
                || !replay_predicate_expression(replay,
                    replay->ir->operands[operands.offset].value, depth + 1))
                return false;
            size_t normal = replay_new_block(replay);
            size_t failure = replay_new_block(replay);
            if (normal == SOL_MIR_OPERATION_NONE
                || failure == SOL_MIR_OPERATION_NONE
                || !replay_failure_block(replay, failure,
                    SOL_MIR_PREDICATE_FAILURE_REFINEMENT)
                || !replay_end_block(replay,
                    SOL_MIR_PREDICATE_TERM_CHECK_REFINED)) return false;
            const SolMirPredicateTerminator *term
                = &replay->operations->predicate_blocks[replay->block].terminator;
            return replay_edge_target(replay, term->normal_edge, normal)
                && replay_edge_target(replay, term->failure_edge, failure)
                && replay_start_block(replay, normal);
        }
        for (size_t i = 0; i < operands.count; ++i)
            if (!replay_predicate_expression(replay,
                    replay->ir->operands[operands.offset + i].value, depth + 1))
                return false;
        kind = SOL_MIR_PREDICATE_INST_CONSTRUCT;
    } else return false;
    const SolMirPredicateInstruction *instruction
        = replay_instruction(replay, kind);
    if (instruction == NULL) return false;
    if (kind == SOL_MIR_PREDICATE_INST_UNARY
        || kind == SOL_MIR_PREDICATE_INST_BINARY) {
        unsigned failures;
        SolTokenKind token = kind == SOL_MIR_PREDICATE_INST_UNARY
            ? source->as.unary.operator_kind : source->as.binary.operator_kind;
        SolMirOperationOpcode opcode = predicate_source_opcode(token,
            kind == SOL_MIR_PREDICATE_INST_UNARY, &failures);
        size_t instruction_id = (size_t)(instruction
            - replay->operations->predicate_instructions);
        uint32_t mask = arithmetic_failure_mask(failures);
        if (instruction->opcode != opcode || instruction->failures != failures)
            return false;
        if (mask != 0 && !append_failure_site(replay->out,
                SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC,
                replay->body_id, replay->block, instruction_id,
                source->span, mask)) return false;
    }
    return true;
}

static bool populate_failure_sites(SolMirRuntimeConventions *out) {
    const SolMirConcreteProgram *c = out->concrete;
    const SolMirMaterialization *m = &c->materialization;
    const SolMirOperations *o = &c->operations;
    const SolIr *ir = c->program.ir;
    for (size_t i = 0; i < out->call_count; ++i) {
        if (!build_event(1)) return false;
        const SolMirRuntimeCall *call = &out->calls[i];
        SolSpan span;
        SolMirRuntimeFailureOriginKind origin;
        if (call->owner_kind == SOL_MIR_RUNTIME_CALL_OWNER_IMAGE) {
            span = m->blocks[call->block].terminator.span;
            origin = SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_CALL;
        } else {
            const SolMirPredicateTerminator *term
                = &o->predicate_blocks[call->block].terminator;
            if (term->binding >= m->binding_count
                || m->bindings[term->binding].source.expression
                    >= ir->expression_count) return false;
            span = ir->expressions[m->bindings[term->binding]
                .source.expression].span;
            origin = SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_CALL;
        }
        uint32_t mask;
        if (!build_call_failure_mask(call, &c->linkage, &mask)) return false;
        if (call->failure_site != i
            || !append_failure_site(out, origin,
                call->owner_kind == SOL_MIR_RUNTIME_CALL_OWNER_IMAGE
                    ? call->image : call->predicate,
                call->block, SOL_MIR_RUNTIME_NONE, span, mask)) return false;
    }
    for (size_t i = 0; i < o->arithmetic_count; ++i) {
        if (!build_event(1)) return false;
        const SolMirOperationArithmeticPlan *plan = &o->arithmetic[i];
        uint32_t mask = arithmetic_failure_mask(plan->failures);
        if (mask == 0) continue;
        const SolMirMaterializedInstruction *instruction
            = &m->instructions[plan->instruction];
        if (!append_failure_site(out,
                SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_ARITHMETIC,
                plan->image, instruction->block, plan->instruction,
                instruction->span, mask)) return false;
    }
    for (size_t image = 0; image < m->image_count; ++image) {
        if (!build_event(1)) return false;
        SolMirPlanSlice blocks = m->images[image].blocks;
        for (size_t q = 0; q < blocks.count; ++q) {
            if (!build_event(1)) return false;
            size_t block = blocks.offset + q;
            const SolMirMaterializedTerminator *term = &m->blocks[block].terminator;
            SolMirRuntimeFailureOriginKind origin;
            SolMirRuntimeFailureCode code;
            if (term->kind == SOL_MIR_TERM_PANIC) {
                origin = SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_PANIC;
                code = SOL_MIR_RUNTIME_FAILURE_PANIC;
            } else if (term->kind == SOL_MIR_TERM_MATCH_FAILURE) {
                origin = SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_NO_MATCH;
                code = SOL_MIR_RUNTIME_FAILURE_NO_MATCH;
            } else if (term->kind == SOL_MIR_TERM_UNREACHABLE) {
                origin = SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_UNREACHABLE;
                code = SOL_MIR_RUNTIME_FAILURE_REACHED_UNREACHABLE;
            } else continue;
            if (!append_failure_site(out, origin, image, block,
                    SOL_MIR_RUNTIME_NONE, term->span, failure_code_bit(code)))
                return false;
        }
    }
    for (size_t body_id = 0; body_id < o->predicate_body_count; ++body_id) {
        if (!build_event(1) || !build_event(ir->expression_count)) return false;
        const SolMirPredicateBody *body = &o->predicate_bodies[body_id];
        PredicateSiteReplay replay = {out, o, ir, body, body_id, 0,
            SOL_MIR_OPERATION_NONE, 0};
        size_t entry = replay_new_block(&replay);
        const SolMirPlanContext *context = &m->contexts[body->context];
        if (entry != body->entry || context->obligation >= ir->obligation_count
            || !replay_start_block(&replay, entry)) return false;
        SolIrExpressionId predicate
            = ir->obligations[context->obligation].predicate;
        if (!replay_predicate_expression(&replay, predicate, 0)
            || !replay_end_block(&replay, SOL_MIR_PREDICATE_TERM_RETURN)
            || replay.allocated_blocks != body->blocks.count) return false;
        SolMirRuntimeFailureCode code = context->kind
                == SOL_MIR_PLAN_CONTEXT_REFINEMENT
            ? SOL_MIR_RUNTIME_FAILURE_REFINEMENT_VIOLATION
            : body->phase == SOL_CONTRACT_REQUIRES
                ? SOL_MIR_RUNTIME_FAILURE_REQUIRE_VIOLATION
                : SOL_MIR_RUNTIME_FAILURE_ENSURE_VIOLATION;
        if (!append_failure_site(out,
                SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT,
                body_id, replay.block, SOL_MIR_RUNTIME_NONE,
                ir->expressions[predicate].span, failure_code_bit(code)))
            return false;
    }
    return true;
}

static bool populate_entries(Builder *builder) {
    SolMirRuntimeConventions *out = builder->out;
    const SolMirConcreteProgram *c = out->concrete;
    const SolMirMaterialization *m = &c->materialization;
    for (size_t i = 0; i < c->linkage.entry_export_count; ++i) {
        if (!build_event(1)) return false;
        const SolMirLinkageEntryExport *export_ = &c->linkage.entry_exports[i];
        SolMirRuntimeSignatureId signature = export_->callable;
        if (signature >= out->signature_count) return false;
        const SolMirRuntimeSignature *sig = &out->signatures[signature];
        if (sig->slots.count != 0) {
            for (size_t q = 0; q < sig->slots.count; ++q) {
                if (!build_event(1)) return false;
                const SolMirRuntimeSignatureSlot *slot
                    = &out->signature_slots[sig->slots.offset + q];
                if (slot->role == SOL_MIR_RUNTIME_SLOT_RECEIVER
                    || slot->access != SOL_ACCESS_OWNED
                    || slot->recipe >= c->representation.recipe_count
                    || c->representation.recipes[slot->recipe].kind
                        != SOL_MIR_RECIPE_CAPABILITY)
                    return fail(builder,
                        SOL_MIR_RUNTIME_CONVENTIONS_BUILD_UNSUPPORTED_CALLABLE,
                        "entry parameters must be owned root capabilities");
            }
        }
        if (sig->result_class == SOL_MIR_RUNTIME_RESULT_VALUE
            && c->representation.recipes[sig->result].kind
                != SOL_MIR_RECIPE_INT64)
            return fail(builder,
                SOL_MIR_RUNTIME_CONVENTIONS_BUILD_UNSUPPORTED_CALLABLE,
                "entry result must be Unit or Int64");
        if (sig->result_class == SOL_MIR_RUNTIME_RESULT_NEVER)
            return fail(builder,
                SOL_MIR_RUNTIME_CONVENTIONS_BUILD_UNSUPPORTED_CALLABLE,
                "entry result must be Unit or Int64");
        if (export_->root_binding >= m->binding_count) return false;
        const SolMirMaterializedBinding *binding
            = &m->bindings[export_->root_binding];
        if (!build_event(1)) return false;
        out->entries[out->entry_count++] = (SolMirRuntimeEntry){
            i, export_->root_binding, export_->callable, signature,
            export_->symbol, binding->source, sig->result_class};
    }
    return true;
}

static bool hash_host(const SolMirLinkageHostRequirement *host,
    SolMirLinkageDigest *digest) {
    static const char domain[] = "sol.mir.runtime-host-import/1";
    SolMirLinkageSha256 sha;
    if (!build_category_event(SOL_MIR_RUNTIME_BUILD_TEST_HASH,
            sizeof(domain) + 16 + SOL_MIR_LINKAGE_DIGEST_BYTES)) return false;
    sol_mir_linkage_internal_sha256_init(&sha);
    sol_mir_linkage_internal_sha256_write(&sha, domain, sizeof(domain));
    uint8_t semantic[16];
    for (size_t i = 0; i < 8; ++i) {
        semantic[i] = (uint8_t)(host->semantic_id.high >> (56 - i * 8));
        semantic[8 + i] = (uint8_t)(host->semantic_id.low >> (56 - i * 8));
    }
    sol_mir_linkage_internal_sha256_write(&sha, semantic, sizeof(semantic));
    sol_mir_linkage_internal_sha256_write(&sha, host->requirement_key.bytes,
        SOL_MIR_LINKAGE_DIGEST_BYTES);
    return sol_mir_linkage_internal_sha256_finish(&sha, digest);
}

static bool hash_recipe(uint8_t tag, const SolMirLinkageDigest *recipe_key,
    SolMirLinkageDigest *digest) {
    static const char domain[] = "sol.mir.runtime-recipe-import/1";
    SolMirLinkageSha256 sha;
    if (!build_category_event(SOL_MIR_RUNTIME_BUILD_TEST_HASH,
            sizeof(domain) + 1 + SOL_MIR_LINKAGE_DIGEST_BYTES)) return false;
    sol_mir_linkage_internal_sha256_init(&sha);
    sol_mir_linkage_internal_sha256_write(&sha, domain, sizeof(domain));
    sol_mir_linkage_internal_sha256_write(&sha, &tag, 1);
    sol_mir_linkage_internal_sha256_write(&sha, recipe_key->bytes,
        SOL_MIR_LINKAGE_DIGEST_BYTES);
    return sol_mir_linkage_internal_sha256_finish(&sha, digest);
}

static bool digest_hex(const SolMirLinkageDigest *digest, char *at) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < SOL_MIR_LINKAGE_DIGEST_BYTES; ++i) {
        if (!build_category_event(SOL_MIR_RUNTIME_BUILD_TEST_DIGEST, 1))
            return false;
        at[i * 2] = hex[digest->bytes[i] >> 4];
        at[i * 2 + 1] = hex[digest->bytes[i] & 15];
    }
    return true;
}

static const char *import_operation_name(SolMirRuntimeImportKind kind) {
    static const char *const names[] = {"host", "create", "copy", "drop",
        "equal", "boundenv"};
    return (size_t)kind < sizeof(names) / sizeof(names[0])
        ? names[kind] : "invalid";
}

static int compare_imports(const SolMirRuntimeImport *a,
    const SolMirRuntimeImport *b) {
    return memcmp(a->symbol.bytes, b->symbol.bytes,
        SOL_MIR_RUNTIME_IMPORT_SYMBOL_CAPACITY);
}

static bool same_import_descriptor(const SolMirRuntimeImport *a,
    const SolMirRuntimeImport *b) {
    return a->kind == b->kind && a->host == b->host
        && a->recipe == b->recipe
        && a->recipe_operation == b->recipe_operation;
}

static bool canonical_import_symbol(const SolMirLinkageSymbol *symbol) {
    return memchr(symbol->bytes, '\0', sizeof(symbol->bytes)) != NULL;
}

static bool sort_imports(SolMirRuntimeImport *imports, size_t count) {
    for (size_t i = 1; i < count; ++i) {
        if (!build_category_event(SOL_MIR_RUNTIME_BUILD_TEST_SORT, 1))
            return false;
        SolMirRuntimeImport value = imports[i];
        size_t at = i;
        while (at != 0) {
            if (!build_category_event(SOL_MIR_RUNTIME_BUILD_TEST_SORT, 1))
                return false;
            if (compare_imports(&value, &imports[at - 1]) >= 0) break;
            imports[at] = imports[at - 1];
            --at;
        }
        imports[at] = value;
    }
    return true;
}

static bool populate_imports(Builder *builder) {
    SolMirRuntimeConventions *out = builder->out;
    const SolMirLinkage *l = &out->concrete->linkage;
    for (size_t i = 0; i < l->host_requirement_count; ++i) {
        if (!build_event(1)) return false;
        const SolMirLinkageHostRequirement *host = &l->host_requirements[i];
        SolMirRuntimeImport *item = &out->imports[out->import_count++];
        item->kind = SOL_MIR_RUNTIME_IMPORT_HOST;
        item->host = i; item->recipe = SOL_MIR_RECIPE_NONE;
        if (!hash_host(host, &item->identity)) return false;
        SolMirLinkageDigest symbol_identity = item->identity;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
            if (sol_mir_runtime_force_host_collision) memset(&item->identity, 0,
            sizeof(item->identity));
#endif
        (void)snprintf(item->symbol.bytes, sizeof(item->symbol.bytes),
            "sol.h1.%016" PRIx64 "%016" PRIx64 ".", host->semantic_id.high,
            host->semantic_id.low);
        if (!digest_hex(&symbol_identity, item->symbol.bytes + 40)) return false;
        item->symbol.bytes[104] = '\0';
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        if (sol_mir_runtime_force_host_symbol_collision) {
            memset(&item->symbol, 0, sizeof(item->symbol));
            memcpy(item->symbol.bytes, "sol.test.host-collision", 23);
        }
#endif
    }
    static const struct { uint32_t flag; SolMirRuntimeImportKind kind; uint8_t tag; }
        operations[] = {
            {SOL_MIR_LINKAGE_RUNTIME_CREATE,
                SOL_MIR_RUNTIME_IMPORT_RECIPE_CREATE, 1},
            {SOL_MIR_LINKAGE_RUNTIME_COPY,
                SOL_MIR_RUNTIME_IMPORT_RECIPE_COPY, 2},
            {SOL_MIR_LINKAGE_RUNTIME_DROP,
                SOL_MIR_RUNTIME_IMPORT_RECIPE_DROP, 3},
            {SOL_MIR_LINKAGE_RUNTIME_EQUAL,
                SOL_MIR_RUNTIME_IMPORT_RECIPE_EQUAL, 4},
            {SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT,
                SOL_MIR_RUNTIME_IMPORT_RECIPE_BOUND_ENVIRONMENT, 5},
        };
    for (size_t i = 0; i < l->runtime_requirement_count; ++i) {
        if (!build_event(1)) return false;
        const SolMirLinkageRuntimeRequirement *requirement
            = &l->runtime_requirements[i];
        for (size_t q = 0; q < sizeof(operations) / sizeof(operations[0]); ++q) {
            if (!build_event(1)) return false;
            if ((requirement->operations & operations[q].flag) == 0) continue;
            SolMirRuntimeImport *item = &out->imports[out->import_count++];
            item->kind = operations[q].kind;
            item->host = SOL_MIR_RUNTIME_NONE;
            item->recipe = requirement->recipe;
            item->recipe_operation = operations[q].flag;
            if (!hash_recipe(operations[q].tag, &requirement->recipe_key,
                    &item->identity)) return false;
            SolMirLinkageDigest symbol_identity = item->identity;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
            if (sol_mir_runtime_force_recipe_collision)
                memset(&item->identity, 0, sizeof(item->identity));
#endif
            int prefix = snprintf(item->symbol.bytes, sizeof(item->symbol.bytes),
                "sol.r1.%s.", import_operation_name(item->kind));
            if (prefix < 0 || (size_t)prefix + 64
                    >= sizeof(item->symbol.bytes)) return false;
            if (!digest_hex(&symbol_identity,
                    item->symbol.bytes + (size_t)prefix)) return false;
            item->symbol.bytes[(size_t)prefix + 64] = '\0';
#ifdef SOL_MIR_PLAN_TEST_HOOKS
            if (sol_mir_runtime_force_recipe_symbol_collision) {
                memset(&item->symbol, 0, sizeof(item->symbol));
                memcpy(item->symbol.bytes, "sol.test.recipe-collision", 25);
            }
#endif
        }
    }
    if (!sort_imports(out->imports, out->import_count)) return false;
    for (size_t i = 0; i < out->import_count; ++i) {
        if (!build_event(1)) return false;
        if (!canonical_import_symbol(&out->imports[i].symbol)) return false;
        for (size_t q = i + 1; q < out->import_count; ++q) {
            if (!build_event(1)) return false;
            const SolMirRuntimeImport *a = &out->imports[i];
            const SolMirRuntimeImport *b = &out->imports[q];
            bool identity_collision = memcmp(&a->identity, &b->identity,
                sizeof(a->identity)) == 0;
            bool symbol_collision = memcmp(&a->symbol, &b->symbol,
                sizeof(a->symbol)) == 0;
            if (!same_import_descriptor(a, b)
                && (identity_collision || symbol_collision))
                return fail(builder,
                    SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SYMBOL_COLLISION,
                    "distinct runtime import descriptors collided");
        }
    }
    return true;
}

static bool expected_usage(const SolMirRuntimeConventions *out,
    SolMirRuntimeConventionsUsage *usage) {
    const SolMirConcreteProgram *c = out->concrete;
    SolMirRuntimeConventionsUsage result = {
        .signatures = out->signature_count,
        .signature_slots = out->signature_slot_count,
        .calls = out->call_count, .operands = out->operand_count,
        .writebacks = out->writeback_count, .entries = out->entry_count,
        .imports = out->import_count, .failure_sites = out->failure_site_count,
        .build_scratch_bytes = c->representation.recipe_count,
    };
#define OWNED(member, type, singular) do { \
    size_t bytes; \
    if (!mul_size(out->singular##_count, sizeof(*out->member), &bytes) \
        || !add_resource_size(&result.owned_bytes, bytes)) return false; \
} while (0);
    SOL_MIR_RUNTIME_CONVENTIONS_ARENAS(OWNED)
#undef OWNED
    result.build_work = metered_build_work;
    *usage = result;
    return true;
}

SolMirRuntimeConventionsBuildOutcome sol_mir_runtime_conventions_build(
    const SolMirRuntimeConventionsBuildRequest *request,
    SolMirRuntimeConventions *output, SolDiagnostics *diagnostics) {
    if (request == NULL || request->concrete == NULL || !owner_empty(output)
        || (request->limits != NULL && !limits_zero(*request->limits)
            && !limits_complete(*request->limits))) {
        report(diagnostics, "invalid runtime conventions build request or destination");
        return SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INVALID_ARGUMENT;
    }
    SolMirRuntimeConventions scratch;
    sol_mir_runtime_conventions_init(&scratch);
    scratch.concrete = request->concrete;
    scratch.limits = request->limits == NULL || limits_zero(*request->limits)
        ? sol_mir_runtime_conventions_default_limits() : *request->limits;
    Builder builder = {&scratch, diagnostics,
        SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INTERNAL_FAILED};
    metered_build_work = 0;
    metered_build_limit = scratch.limits.max_build_work;
    build_work_exhausted = false;
    SolMirRuntimeConventionsUsage preflight;
    SolMirRuntimeConventionsBuildOutcome preflight_outcome
        = sol_mir_runtime_conventions_internal_preflight(request->concrete,
            &scratch.limits, &preflight, diagnostics, false);
    if (preflight_outcome != SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED) {
        fail(&builder, preflight_outcome,
            preflight_outcome == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SYMBOL_COLLISION
                ? "distinct runtime import descriptors collided during preflight"
                : "runtime conventions preflight failed");
        goto failed;
    }
    if (preflight.signatures > scratch.limits.max_signatures
        || preflight.signature_slots > scratch.limits.max_signature_slots
        || preflight.calls > scratch.limits.max_calls
        || preflight.operands > scratch.limits.max_operands
        || preflight.writebacks > scratch.limits.max_writebacks
        || preflight.entries > scratch.limits.max_entries
        || preflight.imports > scratch.limits.max_imports
        || preflight.failure_sites > scratch.limits.max_failure_sites
        || preflight.owned_bytes > scratch.limits.max_owned_bytes
        || preflight.build_scratch_bytes
            > scratch.limits.max_build_scratch_bytes
        || preflight.build_work > scratch.limits.max_build_work
        || preflight.validation_scratch_bytes
            > scratch.limits.max_validation_scratch_bytes
        || preflight.validation_work > scratch.limits.max_validation_work) {
        fail(&builder, SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED,
            "runtime conventions preflight resource limit exceeded");
        goto failed;
    }
    size_t recipe_count = request->concrete->representation.recipe_count;
    if (recipe_count != 0 && !build_event(1)) {
        fail(&builder, SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED,
            "runtime conventions build work limit exceeded");
        goto failed;
    }
    unsigned char *indirect = recipe_count == 0 ? NULL : calloc(recipe_count, 1);
    if (recipe_count != 0 && indirect == NULL) {
        if (diagnostics != NULL) diagnostics->allocation_failed = true;
        fail(&builder, SOL_MIR_RUNTIME_CONVENTIONS_BUILD_ALLOCATION_FAILED,
            "runtime conventions build scratch allocation failed");
        goto failed;
    }
    Counts counts;
    SolMirRuntimeConventionsBuildOutcome count_outcome
        = SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INTERNAL_FAILED;
    if (!count_records(request->concrete, indirect, &counts, &count_outcome)) {
        if (count_outcome != SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INTERNAL_FAILED)
            fail(&builder, count_outcome, "unsupported runtime callable form");
        free(indirect); goto failed;
    }
    if (counts.signatures > scratch.limits.max_signatures
        || counts.slots > scratch.limits.max_signature_slots
        || counts.calls > scratch.limits.max_calls
        || counts.operands > scratch.limits.max_operands
        || counts.writebacks > scratch.limits.max_writebacks
        || counts.entries > scratch.limits.max_entries
        || counts.imports > scratch.limits.max_imports
        || counts.failure_sites > scratch.limits.max_failure_sites) {
        fail(&builder, SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED,
            "runtime conventions arena limit exceeded");
        free(indirect); goto failed;
    }
#define ALLOC(member, type, singular, count) do { \
    scratch.member = allocate(&builder, (count), sizeof(*scratch.member)); \
    if ((count) != 0 && scratch.member == NULL) { free(indirect); goto failed; } \
    scratch.singular##_capacity = (count); \
} while (0)
    ALLOC(signatures, SolMirRuntimeSignature, signature, counts.signatures);
    ALLOC(signature_slots, SolMirRuntimeSignatureSlot, signature_slot, counts.slots);
    ALLOC(calls, SolMirRuntimeCall, call, counts.calls);
    ALLOC(operands, SolMirRuntimeOperand, operand, counts.operands);
    ALLOC(writebacks, SolMirRuntimeWriteback, writeback, counts.writebacks);
    ALLOC(entries, SolMirRuntimeEntry, entry, counts.entries);
    ALLOC(imports, SolMirRuntimeImport, import, counts.imports);
    ALLOC(failure_sites, SolMirRuntimeFailureSite, failure_site,
        counts.failure_sites);
#undef ALLOC
    if (!populate_signatures(&scratch, indirect)
        || !populate_image_calls(&scratch)
        || !populate_predicate_calls(&scratch)
        || !populate_failure_sites(&scratch)
        || !populate_entries(&builder)
        || !populate_imports(&builder)) {
        free(indirect); goto failed;
    }
    free(indirect);
    if (scratch.signature_count != counts.signatures
        || scratch.signature_slot_count != counts.slots
        || scratch.call_count != counts.calls
        || scratch.operand_count != counts.operands
        || scratch.writeback_count != counts.writebacks
        || scratch.entry_count != counts.entries
        || scratch.import_count != counts.imports
        || scratch.failure_site_count != counts.failure_sites
        || !expected_usage(&scratch, &scratch.usage)) goto failed;
    scratch.usage.validation_work = preflight.validation_work;
    scratch.usage.validation_scratch_bytes = preflight.validation_scratch_bytes;
    if (memcmp(&scratch.usage, &preflight, sizeof(preflight)) != 0) goto failed;
    if (!sol_mir_runtime_conventions_validate(&scratch, diagnostics)) {
        SolMirRuntimeConventionsBuildOutcome outcome = diagnostics != NULL
                && diagnostics->allocation_failed
            ? SOL_MIR_RUNTIME_CONVENTIONS_BUILD_ALLOCATION_FAILED
            : SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INTERNAL_FAILED;
        sol_mir_runtime_conventions_free(&scratch);
        return outcome;
    }
    *output = scratch;
    return SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED;
failed:
    if (build_work_exhausted
        && builder.outcome == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INTERNAL_FAILED)
        builder.outcome = SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED;
    if (builder.outcome == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INTERNAL_FAILED)
        report(diagnostics, "runtime conventions construction invariant failed");
    sol_mir_runtime_conventions_free(&scratch);
    return builder.outcome;
}

static const char *const failure_names[] = {
    NULL,
    "SOL-RUNTIME-PANIC",
    "SOL-RUNTIME-INTEGER-OVERFLOW",
    "SOL-RUNTIME-DIVISION-BY-ZERO",
    "SOL-RUNTIME-ALLOCATION-FAILED",
    "SOL-RUNTIME-ALLOCATION-LIMIT",
    "SOL-RUNTIME-STEP-LIMIT",
    "SOL-RUNTIME-CALL-DEPTH-LIMIT",
    "SOL-RUNTIME-VALUE-LIMIT",
    "SOL-RUNTIME-TEXT-LIMIT",
    "SOL-RUNTIME-HOST-CALL-LIMIT",
    "SOL-RUNTIME-NO-MATCH",
    "SOL-RUNTIME-REACHED-UNREACHABLE",
    "SOL-RUNTIME-REQUIRE-VIOLATION",
    "SOL-RUNTIME-ENSURE-VIOLATION",
    "SOL-RUNTIME-REFINEMENT-VIOLATION",
    "SOL-RUNTIME-HOST-ERROR",
};

const char *sol_mir_runtime_failure_name(SolMirRuntimeFailureCode code) {
    return code > SOL_MIR_RUNTIME_FAILURE_NONE
            && code <= SOL_MIR_RUNTIME_FAILURE_HOST_ERROR
        ? failure_names[code] : NULL;
}

static bool runtime_source_equal(SolMirRuntimeSource left,
    SolMirRuntimeSource right) {
    return left.file == right.file && left.start == right.start
        && left.end == right.end;
}

static bool failure_owner_header_valid(const SolMirRuntimeConventions *owner) {
    return owner != NULL && owner->failure_site_count == owner->failure_site_capacity
        && owner->failure_site_count == owner->usage.failure_sites
        && owner->limits.max_failure_sites != 0
        && owner->failure_site_count <= owner->limits.max_failure_sites
        && (owner->failure_site_count == 0
            ? owner->failure_sites == NULL
            : owner->failure_sites != NULL
                && owner->failure_site_count
                    <= SIZE_MAX / sizeof(*owner->failure_sites)
                && (uintptr_t)owner->failure_sites
                    <= UINTPTR_MAX - owner->failure_site_count
                        * sizeof(*owner->failure_sites));
}

static bool failure_record_shape_valid(const SolMirRuntimeConventions *owner,
    const SolMirRuntimeFailureRecord *record) {
    if (record == NULL || record->code <= SOL_MIR_RUNTIME_FAILURE_NONE
        || record->code > SOL_MIR_RUNTIME_FAILURE_HOST_ERROR
        || !failure_owner_header_valid(owner)) return false;
    uint32_t code = failure_code_bit(record->code);
    bool matched = false;
    for (size_t i = 0; i < owner->failure_site_count; ++i)
        matched |= runtime_source_equal(owner->failure_sites[i].source,
                record->source)
            && (owner->failure_sites[i].allowed_codes & code) != 0;
    if (!matched) return false;
    if (record->code == SOL_MIR_RUNTIME_FAILURE_PANIC)
        return record->detail_kind == SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT
            && (record->bytes == NULL) == (record->length == 0)
            && record->length <= SOL_MIR_RUNTIME_HOST_DETAIL_MAX
            && (record->length == 0
                || memchr(record->bytes, '\0', record->length) == NULL);
    if (record->code == SOL_MIR_RUNTIME_FAILURE_HOST_ERROR)
        return record->detail_kind == SOL_MIR_RUNTIME_FAILURE_DETAIL_HOST_BYTES
            && (record->bytes == NULL) == (record->length == 0)
            && record->length <= SOL_MIR_RUNTIME_HOST_DETAIL_MAX
            && (record->length == 0
                || memchr(record->bytes, '\0', record->length) == NULL);
    return record->detail_kind == SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE
        && record->length == 0 && record->bytes == NULL;
}

bool sol_mir_runtime_failure_record_validate(
    const SolMirRuntimeConventions *owner,
    const SolMirRuntimeFailureRecord *record) {
    return failure_record_shape_valid(owner, record);
}

bool sol_mir_runtime_exit_map(const SolMirRuntimeConventions *owner,
    SolMirRuntimeEntryId entry, int64_t result,
    const SolMirRuntimeFailureRecord *failure, SolMirRuntimeExit *exit) {
    if (exit == NULL || owner == NULL || entry >= owner->entry_count
        || owner->entry_count != owner->entry_capacity
        || owner->entry_count > owner->limits.max_entries
        || owner->signature_count != owner->signature_capacity
        || owner->signature_count > owner->limits.max_signatures
        || (owner->entry_count != 0 && owner->entries == NULL)
        || (owner->signature_count != 0 && owner->signatures == NULL)) return false;
    const SolMirRuntimeEntry *runtime_entry = &owner->entries[entry];
    if (runtime_entry->signature >= owner->signature_count) return false;
    SolMirRuntimeResultClass class_
        = owner->signatures[runtime_entry->signature].result_class;
    if (class_ != SOL_MIR_RUNTIME_RESULT_UNIT
        && class_ != SOL_MIR_RUNTIME_RESULT_VALUE) return false;
    SolMirRuntimeExit mapped = {.driver_status = 1,
        .failure_code = SOL_MIR_RUNTIME_FAILURE_NONE};
    if (failure != NULL) {
        if (!failure_record_shape_valid(owner, failure))
            return false;
        mapped.kind = SOL_MIR_RUNTIME_EXIT_RUNTIME_FAILURE;
        mapped.failure_code = failure->code;
        *exit = mapped;
        return true;
    }
    if (class_ == SOL_MIR_RUNTIME_RESULT_UNIT || (result >= 0 && result <= 255)) {
        mapped.kind = SOL_MIR_RUNTIME_EXIT_APPLICATION;
        mapped.driver_status = class_ == SOL_MIR_RUNTIME_RESULT_UNIT
            ? 0 : (int)result;
        mapped.application_status = class_ == SOL_MIR_RUNTIME_RESULT_UNIT
            ? 0 : (uint8_t)result;
        *exit = mapped;
        return true;
    }
    mapped.kind = SOL_MIR_RUNTIME_EXIT_BOUNDARY_ERROR;
    mapped.boundary_code = "SOL-RUN-002";
    *exit = mapped;
    return true;
}

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

static void render_digest(Buffer *out, const SolMirLinkageDigest *digest) {
    for (size_t i = 0; i < SOL_MIR_LINKAGE_DIGEST_BYTES; ++i)
        format(out, "%02x", digest->bytes[i]);
}

static void render_semantic(Buffer *out, SolSemanticId semantic) {
    format(out, "%016" PRIx64 "%016" PRIx64, semantic.high, semantic.low);
}

static const char *runtime_access_name(SolAccessMode access) {
    static const char *const names[] = {"owned", "shared", "exclusive"};
    return (size_t)access < sizeof(names) / sizeof(names[0])
        ? names[access] : "invalid";
}

static const char *runtime_result_name(SolMirRuntimeResultClass class_) {
    static const char *const names[] = {"value", "unit", "never"};
    return (size_t)class_ < sizeof(names) / sizeof(names[0])
        ? names[class_] : "invalid";
}

static const char *runtime_call_name(SolIrCallKind kind) {
    static const char *const names[] = {"function", "callback", "capability",
        "method", "ok", "err", "some", "none", "enum", "distinct"};
    return (size_t)kind < sizeof(names) / sizeof(names[0])
        ? names[kind] : "invalid";
}

static const char *runtime_value_name(SolMirRuntimeValueKind kind) {
    static const char *const names[] = {"none", "temporary", "place", "value",
        "predicate-value", "bound-receiver"};
    return (size_t)kind < sizeof(names) / sizeof(names[0])
        ? names[kind] : "invalid";
}

static const char *runtime_phase_name(SolContractClauseKind phase) {
    return phase == SOL_CONTRACT_REQUIRES ? "requires"
        : phase == SOL_CONTRACT_ENSURES ? "ensures" : "invalid";
}

static const char *runtime_outcome_name(SolContractOutcomeKind outcome) {
    static const char *const names[] = {"always", "success", "failure"};
    return (size_t)outcome < sizeof(names) / sizeof(names[0])
        ? names[outcome] : "invalid";
}

static const char *runtime_import_name(SolMirRuntimeImportKind kind) {
    static const char *const names[] = {"host", "create", "copy", "drop",
        "equal", "bound-environment"};
    return (size_t)kind < sizeof(names) / sizeof(names[0])
        ? names[kind] : "invalid";
}

static const char *runtime_failure_origin_name(
    SolMirRuntimeFailureOriginKind kind) {
    static const char *const names[] = {"image-arithmetic", "image-call",
        "image-panic", "image-no-match", "image-unreachable",
        "predicate-arithmetic", "predicate-call", "predicate-no-match",
        "predicate-result"};
    return (size_t)kind < sizeof(names) / sizeof(names[0])
        ? names[kind] : "invalid";
}

static void render_recipe(Buffer *out, const SolMirLinkage *linkage,
    SolMirRecipeId recipe) {
    SolMirLinkageDigest key;
    if (!sol_mir_linkage_internal_recipe_key(linkage, recipe, &key, NULL)) {
        out->failed = true; return;
    }
    render_digest(out, &key);
}

static void render_host(Buffer *out, const SolMirLinkageHostRequirement *host) {
    format(out, "host:"); render_semantic(out, host->semantic_id);
    format(out, ":"); render_digest(out, &host->requirement_key);
}

static void render_signature_reference(Buffer *out,
    const SolMirRuntimeConventions *owner,
    const SolMirRuntimeSignature *signature) {
    const SolMirLinkage *linkage = &owner->concrete->linkage;
    if (signature->origin == SOL_MIR_RUNTIME_SIGNATURE_INTERNAL) {
        format(out, "internal:%s",
            linkage->callables[signature->internal].symbol.bytes);
    } else if (signature->origin == SOL_MIR_RUNTIME_SIGNATURE_HOST) {
        render_host(out, &linkage->host_requirements[signature->host]);
    } else {
        format(out, "function-recipe:");
        render_recipe(out, linkage, signature->function_recipe);
    }
}

static void render_call_target(Buffer *out,
    const SolMirRuntimeConventions *owner, const SolMirRuntimeCall *call) {
    const SolMirLinkage *linkage = &owner->concrete->linkage;
    if (call->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL) {
        format(out, "internal:%s", linkage->callables[call->internal].symbol.bytes);
    } else if (call->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_HOST) {
        render_host(out, &linkage->host_requirements[call->host]);
    } else {
        format(out, "table:");
        render_digest(out, &linkage->table_entries[call->table].identity);
    }
}

static const SolMirLinkageCallable *linkage_callable_for_image(
    const SolMirRuntimeConventions *owner, size_t image) {
    const SolMirMaterialization *m = &owner->concrete->materialization;
    const SolMirLinkage *l = &owner->concrete->linkage;
    SolMirPlanInstanceId instance = m->images[image].instance;
    for (size_t i = 0; i < l->callable_count; ++i)
        if (l->callables[i].instance == instance) return &l->callables[i];
    return NULL;
}

static const SolMirLinkageHostRequirement *linkage_host_for_import(
    const SolMirRuntimeConventions *owner, size_t import) {
    const SolMirLinkage *l = &owner->concrete->linkage;
    for (size_t i = 0; i < l->host_requirement_count; ++i)
        if (l->host_requirements[i].import == import)
            return &l->host_requirements[i];
    return NULL;
}

static void render_call_owner(Buffer *out,
    const SolMirRuntimeConventions *owner, const SolMirRuntimeCall *call) {
    if (call->owner_kind == SOL_MIR_RUNTIME_CALL_OWNER_IMAGE) {
        const SolMirLinkageCallable *item
            = linkage_callable_for_image(owner, call->image);
        if (item == NULL) { out->failed = true; return; }
        format(out, "image:%s", item->symbol.bytes);
        return;
    }
    const SolMirPredicateBody *body
        = &owner->concrete->operations.predicate_bodies[call->predicate];
    format(out, "predicate:");
    if (body->owner_kind == SOL_MIR_PREDICATE_OWNER_INSTANCE) {
        const SolMirLinkageCallable *item
            = linkage_callable_for_image(owner, body->instance);
        if (item == NULL) { out->failed = true; return; }
        format(out, "internal:%s", item->symbol.bytes);
    } else {
        const SolMirLinkageHostRequirement *host
            = linkage_host_for_import(owner, body->import);
        if (host == NULL) { out->failed = true; return; }
        render_host(out, host);
    }
    format(out, ":%s:%s", runtime_phase_name(body->phase),
        runtime_outcome_name(body->outcome));
}

static void render_failure_site_owner(Buffer *out,
    const SolMirRuntimeConventions *owner,
    const SolMirRuntimeFailureSite *site) {
    if (site->origin_kind <= SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_UNREACHABLE) {
        const SolMirLinkageCallable *item
            = linkage_callable_for_image(owner, site->owner);
        if (item == NULL) { out->failed = true; return; }
        format(out, "internal:%s", item->symbol.bytes);
        return;
    }
    const SolMirPredicateBody *body
        = &owner->concrete->operations.predicate_bodies[site->owner];
    if (body->owner_kind == SOL_MIR_PREDICATE_OWNER_INSTANCE) {
        const SolMirLinkageCallable *item
            = linkage_callable_for_image(owner, body->instance);
        if (item == NULL) { out->failed = true; return; }
        format(out, "internal:%s", item->symbol.bytes);
    } else {
        const SolMirLinkageHostRequirement *host
            = linkage_host_for_import(owner, body->import);
        if (host == NULL) { out->failed = true; return; }
        render_host(out, host);
    }
    format(out, ":%s:%s", runtime_phase_name(body->phase),
        runtime_outcome_name(body->outcome));
}

typedef struct { Buffer text; } RenderLine;

static int compare_render_lines(const void *left, const void *right) {
    const RenderLine *a = left, *b = right;
    return strcmp(a->text.data, b->text.data);
}

static void free_render_lines(RenderLine *lines, size_t count) {
    for (size_t i = 0; i < count; ++i) free(lines[i].text.data);
    free(lines);
}

bool sol_mir_runtime_conventions_render(FILE *stream,
    const SolMirRuntimeConventions *owner) {
    if (stream == NULL || !sol_mir_runtime_conventions_validate(owner, NULL))
        return false;
    Buffer out = {0};
    format(&out, "mir_runtime_conventions\n");
    format(&out, "declaration.call target=direct-internal|direct-host|indirect-table operands=receiver-first failure-edge=exactly-one\n");
    format(&out, "declaration.outcome owned=consumed-both shared=borrow-ends-both exclusive=copy-in-normal-writeback failure=no-writeback\n");
    format(&out, "declaration.result value=normal-payload unit=normal-no-payload never=no-normal\n");
    for (size_t i = 1; i <= SOL_MIR_RUNTIME_FAILURE_HOST_ERROR; ++i)
        format(&out, "failure %zu name=%s\n", i,
            sol_mir_runtime_failure_name((SolMirRuntimeFailureCode)i));
    format(&out, "exit unit=application:0 int64=application:0..255 boundary=driver:1:SOL-RUN-002 runtime=driver:1:failure-record\n");
    size_t line_count = 0, line_capacity = owner->signature_count;
    if (!add_size(&line_capacity, owner->call_count)
        || !add_size(&line_capacity, owner->entry_count)
        || !add_size(&line_capacity, owner->import_count)
        || !add_size(&line_capacity, owner->failure_site_count)) {
        free(out.data); return false;
    }
    RenderLine *lines = line_capacity == 0 ? NULL
        : calloc(line_capacity, sizeof(*lines));
    if (line_capacity != 0 && lines == NULL) {
        free(out.data); return false;
    }
    const SolMirLinkage *linkage = &owner->concrete->linkage;
    for (size_t i = 0; i < owner->signature_count; ++i) {
        const SolMirRuntimeSignature *s = &owner->signatures[i];
        Buffer *line = &lines[line_count++].text;
        format(line, "signature target=");
        render_signature_reference(line, owner, s);
        format(line, " slots=[");
        for (size_t q = 0; q < s->slots.count; ++q) {
            const SolMirRuntimeSignatureSlot *slot
                = &owner->signature_slots[s->slots.offset + q];
            format(line, "%s%s", q == 0 ? "" : ",",
                slot->role == SOL_MIR_RUNTIME_SLOT_RECEIVER
                    ? "receiver" : "parameter");
            if (slot->role == SOL_MIR_RUNTIME_SLOT_PARAMETER)
                format(line, "[%zu]", slot->formal);
            format(line, ":%s:", runtime_access_name(slot->access));
            render_recipe(line, linkage, slot->recipe);
        }
        format(line, "] result="); render_recipe(line, linkage, s->result);
        format(line, " class=%s\n", runtime_result_name(s->result_class));
    }
    for (size_t i = 0; i < owner->call_count; ++i) {
        const SolMirRuntimeCall *c = &owner->calls[i];
        const SolMirRuntimeSignature *s = &owner->signatures[c->signature];
        Buffer *line = &lines[line_count++].text;
        format(line, "call owner="); render_call_owner(line, owner, c);
        format(line, " kind=%s signature=", runtime_call_name(c->call_kind));
        render_signature_reference(line, owner, s);
        format(line, " target="); render_call_target(line, owner, c);
        format(line, " callee=%s operands=[", runtime_value_name(c->callee.kind));
        for (size_t q = 0; q < c->operands.count; ++q) {
            const SolMirRuntimeOperand *operand
                = &owner->operands[c->operands.offset + q];
            const SolMirRuntimeSignatureSlot *slot
                = &owner->signature_slots[operand->signature_slot];
            format(line, "%s%s", q == 0 ? "" : ",",
                slot->role == SOL_MIR_RUNTIME_SLOT_RECEIVER
                    ? "receiver" : "parameter");
            if (slot->role == SOL_MIR_RUNTIME_SLOT_PARAMETER)
                format(line, "[%zu]", slot->formal);
            format(line, ":%s:%s", runtime_access_name(slot->access),
                runtime_value_name(operand->value.kind));
        }
        format(line, "] result=%s normal=%s failure=present writebacks=[",
            runtime_value_name(c->result.kind),
            s->result_class == SOL_MIR_RUNTIME_RESULT_NEVER ? "absent" : "present");
        for (size_t q = 0; q < c->writebacks.count; ++q) {
            const SolMirRuntimeWriteback *w
                = &owner->writebacks[c->writebacks.offset + q];
            format(line, "%s%s", q == 0 ? "" : ",",
                w->receiver ? "receiver" : "parameter");
            if (!w->receiver) format(line, "[%zu]", w->formal);
            format(line, ":"); render_recipe(line, linkage, w->recipe);
        }
        format(line, "]\n");
    }
    for (size_t i = 0; i < owner->entry_count; ++i) {
        const SolMirRuntimeEntry *e = &owner->entries[i];
        Buffer *line = &lines[line_count++].text;
        format(line, "entry symbol=%s target=%s result=%s\n", e->symbol.bytes,
            linkage->callables[e->callable].symbol.bytes,
            runtime_result_name(e->result_class));
    }
    for (size_t i = 0; i < owner->import_count; ++i) {
        const SolMirRuntimeImport *x = &owner->imports[i];
        Buffer *line = &lines[line_count++].text;
        format(line, "import operation=%s identity=", runtime_import_name(x->kind));
        render_digest(line, &x->identity);
        format(line, " symbol=%s\n", x->symbol.bytes);
    }
    for (size_t i = 0; i < owner->failure_site_count; ++i) {
        const SolMirRuntimeFailureSite *site = &owner->failure_sites[i];
        Buffer *line = &lines[line_count++].text;
        format(line, "failure-site origin=%s owner=",
            runtime_failure_origin_name(site->origin_kind));
        render_failure_site_owner(line, owner, site);
        format(line, " codes=");
        bool first = true;
        for (size_t code = 1; code <= SOL_MIR_RUNTIME_FAILURE_HOST_ERROR;
            ++code) {
            if ((site->allowed_codes
                    & failure_code_bit((SolMirRuntimeFailureCode)code)) == 0)
                continue;
            format(line, "%s%s", first ? "" : "|",
                sol_mir_runtime_failure_name((SolMirRuntimeFailureCode)code));
            first = false;
        }
        format(line, "\n");
    }
    for (size_t i = 0; i < line_count; ++i)
        if (lines[i].text.failed) out.failed = true;
    if (!out.failed && line_count > 1)
        qsort(lines, line_count, sizeof(*lines), compare_render_lines);
    for (size_t i = 0; !out.failed && i < line_count; ++i)
        format(&out, "%s", lines[i].text.data);
    free_render_lines(lines, line_count);
    bool ok = !out.failed && (out.length == 0
        || fwrite(out.data, out.length, 1, stream) == 1);
    free(out.data);
    return ok;
}
