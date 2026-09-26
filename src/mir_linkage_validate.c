#include "sol/mir_linkage.h"
#include "mir_linkage_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uintptr_t start, end; } Range;

static _Thread_local size_t metered_work;
static _Thread_local size_t metered_limit;

static bool v_add_size(size_t *value, size_t amount) {
    if (amount > SIZE_MAX - *value) return false;
    *value += amount; return true;
}

static bool v_tick(size_t amount) {
    return v_add_size(&metered_work, amount) && metered_work <= metered_limit;
}

static bool v_text_size(const char *text, size_t *size) {
    if (text == NULL || size == NULL) return false;
    size_t length = 0;
    while (text[length] != '\0') {
        if (!v_tick(1) || length == SIZE_MAX) return false;
        ++length;
    }
    if (!v_tick(1)) return false;
    *size = length + 1;
    return true;
}

static bool invalid(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) sol_diagnostics_add(diagnostics,
        "SOL-MIR-LINKAGE-001", SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    return false;
}

static bool limits_complete(SolMirLinkageLimits limits) {
#define REQUIRED(name) limits.name != 0
    return REQUIRED(max_callables) && REQUIRED(max_bindings)
        && REQUIRED(max_entry_exports) && REQUIRED(max_table_entries)
        && REQUIRED(max_callable_values) && REQUIRED(max_host_requirements)
        && REQUIRED(max_runtime_requirements) && REQUIRED(max_owned_bytes)
        && REQUIRED(max_build_scratch_bytes) && REQUIRED(max_build_work)
        && REQUIRED(max_validation_scratch_bytes)
        && REQUIRED(max_validation_work);
#undef REQUIRED
}

static bool canonical(size_t count, size_t capacity, const void *pointer) {
    return count == capacity && ((count == 0) == (pointer == NULL));
}

static bool add_range(Range *ranges, size_t *count, const void *pointer,
    size_t items, size_t item_size) {
    if (items == 0) return pointer == NULL;
    if (pointer == NULL || items > SIZE_MAX / item_size) return false;
    size_t bytes = items * item_size;
    uintptr_t start = (uintptr_t)pointer;
    if (bytes > UINTPTR_MAX - start) return false;
    Range next = {start, start + bytes};
    for (size_t i = 0; i < *count; ++i) {
        if (!v_tick(1)) return false;
        if (next.start < ranges[i].end && ranges[i].start < next.end) return false;
    }
    ranges[(*count)++] = next; return true;
}

static bool overlaps(const Range *ranges, size_t count, const void *pointer,
    size_t items, size_t item_size) {
    if (items == 0) return pointer != NULL;
    if (pointer == NULL || items > SIZE_MAX / item_size) return true;
    size_t bytes = items * item_size;
    uintptr_t start = (uintptr_t)pointer;
    if (bytes > UINTPTR_MAX - start) return true;
    uintptr_t end = start + bytes;
    for (size_t i = 0; i < count; ++i) {
        if (!v_tick(1)) return true;
        if (start < ranges[i].end && ranges[i].start < end) return true;
    }
    return false;
}

static bool digest_equal(const SolMirLinkageDigest *left,
    const SolMirLinkageDigest *right) {
    return memcmp(left->bytes, right->bytes, SOL_MIR_LINKAGE_DIGEST_BYTES) == 0;
}

static bool semantic_equal(SolSemanticId left, SolSemanticId right) {
    return left.high == right.high && left.low == right.low;
}

enum {
    V_TOKEN_U64 = 1, V_TOKEN_BYTES, V_TOKEN_SEMANTIC_ID, V_TOKEN_SEQUENCE,
    V_TOKEN_ABSENT, V_TOKEN_PRESENT, V_TOKEN_TYPE, V_TOKEN_EFFECT,
    V_TOKEN_CALLABLE, V_TOKEN_DICTIONARY, V_TOKEN_INSTANCE,
    V_TOKEN_SIGNATURE, V_TOKEN_TABLE,
};

static void v_token(SolMirLinkageSha256 *sha, uint8_t value) {
    if (!v_tick(1)) { sha->valid = false; return; }
    sol_mir_linkage_internal_sha256_write(sha, &value, 1);
}

static void v_u64(SolMirLinkageSha256 *sha, uint64_t value) {
    uint8_t bytes[8];
    v_token(sha, V_TOKEN_U64);
    if (!v_tick(sizeof(bytes))) { sha->valid = false; return; }
    for (size_t i = 0; i < 8; ++i)
        bytes[7 - i] = (uint8_t)(value >> (i * 8));
    sol_mir_linkage_internal_sha256_write(sha, bytes, sizeof(bytes));
}

static void v_bytes(SolMirLinkageSha256 *sha, const void *value,
    size_t length) {
    if (!v_tick(length)) { sha->valid = false; return; }
    v_token(sha, V_TOKEN_BYTES); v_u64(sha, (uint64_t)length);
    sol_mir_linkage_internal_sha256_write(sha, value, length);
}

static void v_label(SolMirLinkageSha256 *sha, const char *value) {
    v_bytes(sha, value, strlen(value));
}

static void v_sequence(SolMirLinkageSha256 *sha, size_t count) {
    v_token(sha, V_TOKEN_SEQUENCE); v_u64(sha, (uint64_t)count);
}

static void v_semantic(SolMirLinkageSha256 *sha, SolSemanticId semantic) {
    v_token(sha, V_TOKEN_SEMANTIC_ID);
    v_u64(sha, semantic.high); v_u64(sha, semantic.low);
}

static const char *v_type_label(SolIrTypeKind kind) {
    static const char *const labels[] = {
        "int64", "bool", "text", "unit", "never", "nominal", "option",
        "result", "tuple", "function", "parameter", "self",
    };
    return (size_t)kind < sizeof(labels) / sizeof(labels[0])
        ? labels[kind] : NULL;
}

static const char *v_access_label(SolAccessMode access) {
    static const char *const labels[] = {"owned", "shared", "exclusive"};
    return (size_t)access < sizeof(labels) / sizeof(labels[0])
        ? labels[access] : NULL;
}

static const char *v_callable_label(SolIrCallableKind kind) {
    static const char *const labels[] = {"function", "capability",
        "trait-requirement", "trait-implementation", "test"};
    return (size_t)kind < sizeof(labels) / sizeof(labels[0])
        ? labels[kind] : NULL;
}

static const char *v_authority_label(SolMirPlanEffectAuthority authority) {
    static const char *const labels[] = {"none", "receiver", "parameter", "tail"};
    return (size_t)authority < sizeof(labels) / sizeof(labels[0])
        ? labels[authority] : NULL;
}

static int v_compare_size(size_t left, size_t right) {
    return left < right ? -1 : left > right;
}

static int v_compare_u64(uint64_t left, uint64_t right) {
    return left < right ? -1 : left > right;
}

static int v_compare_semantic(SolSemanticId left, SolSemanticId right) {
    int order = v_compare_u64(left.high, right.high);
    return order != 0 ? order : v_compare_u64(left.low, right.low);
}

static int v_compare_text(const char *left, size_t left_length,
    const char *right, size_t right_length, SolMirLinkageWorkMeter *work) {
    size_t shared = left_length < right_length ? left_length : right_length;
    if (!v_tick(shared)
        || !sol_mir_linkage_internal_work_tick(work, shared)) return 0;
    int order = memcmp(left, right, shared);
    return order != 0 ? order : v_compare_size(left_length, right_length);
}

static int v_compare_callable(const SolIr *ir, SolIrCallableId left_id,
    SolIrCallableId right_id, bool *valid, SolMirLinkageWorkMeter *work) {
    if (!v_tick(1) || !sol_mir_linkage_internal_work_tick(work, 1)) {
        *valid = false; return 0;
    }
    if (left_id >= ir->callable_count || right_id >= ir->callable_count) {
        *valid = false; return 0;
    }
    const SolIrCallable *left = &ir->callables[left_id];
    const SolIrCallable *right = &ir->callables[right_id];
    const char *left_kind = v_callable_label(left->kind);
    const char *right_kind = v_callable_label(right->kind);
    if (left->owner >= ir->definition_count || right->owner >= ir->definition_count
        || left->name == NULL || right->name == NULL
        || left_kind == NULL || right_kind == NULL) {
        *valid = false; return 0;
    }
    int order = v_compare_semantic(ir->definitions[left->owner].semantic_id,
        ir->definitions[right->owner].semantic_id);
    if (order != 0) return order;
    order = strcmp(left_kind, right_kind);
    if (order != 0) return order;
    bool left_owner = ir->definitions[left->owner].callable == left_id;
    bool right_owner = ir->definitions[right->owner].callable == right_id;
    if (left_owner != right_owner) return left_owner ? -1 : 1;
    size_t left_length = strlen(left->name), right_length = strlen(right->name);
    if (work != NULL) {
        if (!sol_mir_linkage_internal_work_tick(work, left_length + 1)
            || !sol_mir_linkage_internal_work_tick(work, right_length + 1)) {
            *valid = false; return 0;
        }
    }
    return left_owner ? 0 : v_compare_text(left->name, left_length,
        right->name, right_length, work);
}

static int v_compare_effect(const SolMirPlan *plan,
    SolMirPlanEffectRowId left_id, SolMirPlanEffectRowId right_id, bool *valid,
    SolMirLinkageWorkMeter *work) {
    if (!v_tick(1) || !sol_mir_linkage_internal_work_tick(work, 1)) {
        *valid = false; return 0;
    }
    if (left_id >= plan->effect_row_count || right_id >= plan->effect_row_count) {
        *valid = false; return 0;
    }
    const SolMirPlanEffectRow *left = &plan->effect_rows[left_id];
    const SolMirPlanEffectRow *right = &plan->effect_rows[right_id];
    if (left->atom_offset > plan->effect_row_atom_count
        || left->atom_count > plan->effect_row_atom_count - left->atom_offset
        || right->atom_offset > plan->effect_row_atom_count
        || right->atom_count > plan->effect_row_atom_count - right->atom_offset) {
        *valid = false; return 0;
    }
    int order = v_compare_size(left->atom_count, right->atom_count);
    if (order != 0) return order;
    if (!v_tick(left->atom_count)) { *valid = false; return 0; }
    for (size_t i = 0; i < left->atom_count; ++i) {
        size_t left_atom = plan->effect_row_atoms[left->atom_offset + i];
        size_t right_atom = plan->effect_row_atoms[right->atom_offset + i];
        if (left_atom >= plan->effect_atom_count
            || right_atom >= plan->effect_atom_count) {
            *valid = false; return 0;
        }
        const SolMirPlanEffectAtom *a = &plan->effect_atoms[left_atom];
        const SolMirPlanEffectAtom *b = &plan->effect_atoms[right_atom];
        const char *a_authority = v_authority_label(a->authority);
        const char *b_authority = v_authority_label(b->authority);
        if (a->name == NULL || b->name == NULL
            || a_authority == NULL || b_authority == NULL) {
            *valid = false; return 0;
        }
        order = v_compare_text(a->name, a->length, b->name, b->length, work);
        if (order != 0) return order;
        order = strcmp(a_authority, b_authority);
        if (order != 0) return order;
        if (a->authority == SOL_MIR_PLAN_EFFECT_AUTHORITY_PARAMETER) {
            order = v_compare_size(a->ordinal, b->ordinal);
            if (order != 0) return order;
        }
    }
    return 0;
}

static int v_compare_type(const SolMirPlan *plan, SolMirPlanTypeId left_id,
    SolMirPlanTypeId right_id, size_t depth, bool *valid,
    SolMirLinkageWorkMeter *work) {
    if (!v_tick(1) || !sol_mir_linkage_internal_work_tick(work, 1)) {
        *valid = false; return 0;
    }
    if (left_id >= plan->type_count || right_id >= plan->type_count
        || depth > plan->type_count) {
        *valid = false; return 0;
    }
    const SolMirPlanType *left = &plan->types[left_id];
    const SolMirPlanType *right = &plan->types[right_id];
    const SolIr *ir = plan->program->ir;
    const char *left_kind = v_type_label(left->kind);
    const char *right_kind = v_type_label(right->kind);
    if (left_kind == NULL || right_kind == NULL
        || left->kind == SOL_IR_TYPE_PARAMETER || left->kind == SOL_IR_TYPE_SELF
        || right->kind == SOL_IR_TYPE_PARAMETER || right->kind == SOL_IR_TYPE_SELF) {
        *valid = false; return 0;
    }
    int order = strcmp(left_kind, right_kind);
    if (order != 0) return order;
    bool left_definition = left->definition != SOL_IR_NONE;
    bool right_definition = right->definition != SOL_IR_NONE;
    if (left_definition != right_definition) return left_definition ? 1 : -1;
    if (left_definition) {
        if (left->definition >= ir->definition_count
            || right->definition >= ir->definition_count) {
            *valid = false; return 0;
        }
        order = v_compare_semantic(ir->definitions[left->definition].semantic_id,
            ir->definitions[right->definition].semantic_id);
        if (order != 0) return order;
    }
    if (left->argument_offset > plan->type_component_count
        || left->argument_count > plan->type_component_count - left->argument_offset
        || right->argument_offset > plan->type_component_count
        || right->argument_count > plan->type_component_count - right->argument_offset) {
        *valid = false; return 0;
    }
    order = v_compare_size(left->argument_count, right->argument_count);
    if (order != 0) return order;
    for (size_t i = 0; i < left->argument_count; ++i) {
        order = v_compare_type(plan,
            plan->type_components[left->argument_offset + i],
            plan->type_components[right->argument_offset + i], depth + 1,
            valid, work);
        if (!*valid || order != 0) return order;
    }
    if (left->kind == SOL_IR_TYPE_NOMINAL) return 0;
    if (left->parameter_offset > plan->type_component_count
        || left->parameter_count > plan->type_component_count - left->parameter_offset
        || right->parameter_offset > plan->type_component_count
        || right->parameter_count > plan->type_component_count - right->parameter_offset
        || left->parameter_access_offset > plan->type_parameter_access_count
        || left->parameter_count > plan->type_parameter_access_count
            - left->parameter_access_offset
        || right->parameter_access_offset > plan->type_parameter_access_count
        || right->parameter_count > plan->type_parameter_access_count
            - right->parameter_access_offset) {
        *valid = false; return 0;
    }
    order = v_compare_size(left->parameter_count, right->parameter_count);
    if (order != 0) return order;
    for (size_t i = 0; i < left->parameter_count; ++i) {
        const char *left_access = v_access_label(plan->type_parameter_accesses[
            left->parameter_access_offset + i]);
        const char *right_access = v_access_label(plan->type_parameter_accesses[
            right->parameter_access_offset + i]);
        if (left_access == NULL || right_access == NULL) {
            *valid = false; return 0;
        }
        order = strcmp(left_access, right_access);
        if (order != 0) return order;
        order = v_compare_type(plan,
            plan->type_components[left->parameter_offset + i],
            plan->type_components[right->parameter_offset + i], depth + 1,
            valid, work);
        if (!*valid || order != 0) return order;
    }
    bool left_result = left->result != SOL_MIR_PLAN_NONE;
    bool right_result = right->result != SOL_MIR_PLAN_NONE;
    if (left_result != right_result) return left_result ? 1 : -1;
    if (left_result) {
        order = v_compare_type(plan, left->result, right->result,
            depth + 1, valid, work);
        if (!*valid || order != 0) return order;
    }
    bool left_effects = left->effects != SOL_MIR_PLAN_NONE;
    bool right_effects = right->effects != SOL_MIR_PLAN_NONE;
    if (left_effects != right_effects) return left_effects ? 1 : -1;
    return left_effects ? v_compare_effect(plan, left->effects,
        right->effects, valid, work) : 0;
}

static int v_compare_dictionary_entry(const SolMirPlan *plan,
    const SolMirPlanDictionaryEntry *left,
    const SolMirPlanDictionaryEntry *right, bool *valid,
    SolMirLinkageWorkMeter *work) {
    if (!v_tick(1) || !sol_mir_linkage_internal_work_tick(work, 1)) {
        *valid = false; return 0;
    }
    const SolIr *ir = plan->program->ir;
    if (left->trait >= ir->definition_count || right->trait >= ir->definition_count
        || left->implementation >= ir->definition_count
        || right->implementation >= ir->definition_count) {
        *valid = false; return 0;
    }
    int order = v_compare_size(left->generic_ordinal, right->generic_ordinal);
    if (order != 0) return order;
    order = v_compare_semantic(ir->definitions[left->trait].semantic_id,
        ir->definitions[right->trait].semantic_id);
    if (order != 0) return order;
    order = v_compare_callable(ir, left->requirement, right->requirement, valid,
        work);
    if (!*valid || order != 0) return order;
    order = v_compare_type(plan, left->type, right->type, 0, valid, work);
    if (!*valid || order != 0) return order;
    order = v_compare_semantic(ir->definitions[left->implementation].semantic_id,
        ir->definitions[right->implementation].semantic_id);
    return order != 0 ? order
        : v_compare_callable(ir, left->method, right->method, valid, work);
}

static bool v_hash_callable(SolMirLinkageSha256 *sha, const SolIr *ir,
    SolIrCallableId callable_id) {
    if (!v_tick(1)) return false;
    if (callable_id >= ir->callable_count) return false;
    const SolIrCallable *callable = &ir->callables[callable_id];
    const char *kind = v_callable_label(callable->kind);
    if (callable->owner >= ir->definition_count || callable->name == NULL
        || kind == NULL) return false;
    const SolIrDefinition *owner = &ir->definitions[callable->owner];
    v_token(sha, V_TOKEN_CALLABLE); v_semantic(sha, owner->semantic_id);
    v_label(sha, kind);
    if (owner->callable == callable_id) v_label(sha, "owner-callable");
    else {
        size_t length = strlen(callable->name);
        if (sha->work != NULL
            && !sol_mir_linkage_internal_work_tick(sha->work, length + 1))
            return false;
        v_label(sha, "member"); v_bytes(sha, callable->name, length);
    }
    return true;
}

static bool v_hash_effect(SolMirLinkageSha256 *sha, const SolMirPlan *plan,
    SolMirPlanEffectRowId row_id) {
    if (!v_tick(1)) return false;
    if (row_id >= plan->effect_row_count) return false;
    const SolMirPlanEffectRow *row = &plan->effect_rows[row_id];
    if (row->atom_offset > plan->effect_row_atom_count
        || row->atom_count > plan->effect_row_atom_count - row->atom_offset)
        return false;
    v_token(sha, V_TOKEN_EFFECT); v_sequence(sha, row->atom_count);
    for (size_t i = 0; i < row->atom_count; ++i) {
        size_t atom_id = plan->effect_row_atoms[row->atom_offset + i];
        if (atom_id >= plan->effect_atom_count) return false;
        const SolMirPlanEffectAtom *atom = &plan->effect_atoms[atom_id];
        const char *authority = v_authority_label(atom->authority);
        if (atom->name == NULL || authority == NULL) return false;
        v_bytes(sha, atom->name, atom->length); v_label(sha, authority);
        if (atom->authority == SOL_MIR_PLAN_EFFECT_AUTHORITY_PARAMETER)
            v_u64(sha, (uint64_t)atom->ordinal);
    }
    return true;
}

static bool v_hash_type(SolMirLinkageSha256 *sha, const SolMirPlan *plan,
    SolMirPlanTypeId type_id, size_t depth) {
    if (!v_tick(1)) return false;
    if (type_id >= plan->type_count || depth > plan->type_count) return false;
    const SolMirPlanType *type = &plan->types[type_id];
    const SolIr *ir = plan->program->ir;
    const char *kind = v_type_label(type->kind);
    if (kind == NULL || type->kind == SOL_IR_TYPE_PARAMETER
        || type->kind == SOL_IR_TYPE_SELF) return false;
    v_token(sha, V_TOKEN_TYPE); v_label(sha, kind);
    if (type->definition != SOL_IR_NONE) {
        if (type->definition >= ir->definition_count) return false;
        v_token(sha, V_TOKEN_PRESENT);
        v_semantic(sha, ir->definitions[type->definition].semantic_id);
    } else v_token(sha, V_TOKEN_ABSENT);
    if (type->argument_offset > plan->type_component_count
        || type->argument_count > plan->type_component_count - type->argument_offset)
        return false;
    v_sequence(sha, type->argument_count);
    for (size_t i = 0; i < type->argument_count; ++i)
        if (!v_hash_type(sha, plan,
                plan->type_components[type->argument_offset + i], depth + 1))
            return false;
    if (type->kind == SOL_IR_TYPE_NOMINAL) return true;
    if (type->parameter_offset > plan->type_component_count
        || type->parameter_count > plan->type_component_count - type->parameter_offset
        || type->parameter_access_offset > plan->type_parameter_access_count
        || type->parameter_count > plan->type_parameter_access_count
            - type->parameter_access_offset) return false;
    v_sequence(sha, type->parameter_count);
    for (size_t i = 0; i < type->parameter_count; ++i) {
        const char *access = v_access_label(plan->type_parameter_accesses[
            type->parameter_access_offset + i]);
        if (access == NULL) return false;
        v_label(sha, access);
        if (!v_hash_type(sha, plan,
                plan->type_components[type->parameter_offset + i], depth + 1))
            return false;
    }
    if (type->result == SOL_MIR_PLAN_NONE) v_token(sha, V_TOKEN_ABSENT);
    else {
        v_token(sha, V_TOKEN_PRESENT);
        if (!v_hash_type(sha, plan, type->result, depth + 1)) return false;
    }
    if (type->effects == SOL_MIR_PLAN_NONE) v_token(sha, V_TOKEN_ABSENT);
    else {
        v_token(sha, V_TOKEN_PRESENT);
        if (!v_hash_effect(sha, plan, type->effects)) return false;
    }
    return true;
}

static bool v_hash_dictionary(SolMirLinkageSha256 *sha,
    const SolMirPlan *plan, SolMirPlanSlice dictionary) {
    if (!v_tick(1)) return false;
    if (dictionary.offset > plan->dictionary_entry_count
        || dictionary.count > plan->dictionary_entry_count - dictionary.offset)
        return false;
    v_token(sha, V_TOKEN_DICTIONARY); v_sequence(sha, dictionary.count);
    const SolIr *ir = plan->program->ir;
    size_t previous = SOL_MIR_PLAN_NONE;
    for (size_t i = 0; i < dictionary.count; ++i) {
        size_t selected = SOL_MIR_PLAN_NONE;
        for (size_t candidate = 0; candidate < dictionary.count; ++candidate) {
            bool valid = true;
            const SolMirPlanDictionaryEntry *entry
                = &plan->dictionary_entries[dictionary.offset + candidate];
            if (previous != SOL_MIR_PLAN_NONE
                && v_compare_dictionary_entry(plan,
                    &plan->dictionary_entries[dictionary.offset + previous],
                    entry, &valid, sha->work) >= 0) {
                if (!valid) return false;
                continue;
            }
            if (selected == SOL_MIR_PLAN_NONE
                || v_compare_dictionary_entry(plan, entry,
                    &plan->dictionary_entries[dictionary.offset + selected],
                    &valid, sha->work) < 0) selected = candidate;
            if (!valid) return false;
        }
        if (selected == SOL_MIR_PLAN_NONE) return false;
        const SolMirPlanDictionaryEntry *entry
            = &plan->dictionary_entries[dictionary.offset + selected];
        if (entry->trait >= ir->definition_count
            || entry->implementation >= ir->definition_count) return false;
        v_u64(sha, (uint64_t)entry->generic_ordinal);
        v_semantic(sha, ir->definitions[entry->trait].semantic_id);
        if (!v_hash_callable(sha, ir, entry->requirement)
            || !v_hash_type(sha, plan, entry->type, 0)) return false;
        v_semantic(sha, ir->definitions[entry->implementation].semantic_id);
        if (!v_hash_callable(sha, ir, entry->method)) return false;
        previous = selected;
    }
    return true;
}

static void v_start(SolMirLinkageSha256 *sha, const char *domain,
    SolMirLinkageWorkMeter *work) {
    sol_mir_linkage_internal_sha256_init(sha);
    sha->work = work;
    v_bytes(sha, domain, strlen(domain));
}

static bool v_finish(SolMirLinkageSha256 *sha,
    SolMirLinkageDigest *digest) {
    return v_tick(128)
        && sol_mir_linkage_internal_sha256_finish(sha, digest);
}

static bool v_instance_key(const SolMirLinkage *linkage,
    SolMirPlanInstanceId instance_id, SolMirLinkageDigest *digest,
    SolMirLinkageWorkMeter *work) {
    const SolMirPlan *plan = linkage->operations->layout->representation
        ->materialization->plan;
    if (instance_id >= plan->instance_count) return false;
    const SolMirPlanInstance *instance = &plan->instances[instance_id];
    SolMirLinkageSha256 sha;
    v_start(&sha, "sol.mir.instance-key/1", work);
    v_token(&sha, V_TOKEN_INSTANCE);
    if (!v_hash_callable(&sha, plan->program->ir, instance->callable)) return false;
    if (instance->receiver == SOL_MIR_PLAN_NONE) v_token(&sha, V_TOKEN_ABSENT);
    else {
        v_token(&sha, V_TOKEN_PRESENT);
        if (!v_hash_type(&sha, plan, instance->receiver, 0)) return false;
    }
    if (instance->type_arguments.offset > plan->instance_type_id_count
        || instance->type_arguments.count > plan->instance_type_id_count
            - instance->type_arguments.offset) return false;
    v_sequence(&sha, instance->type_arguments.count);
    for (size_t i = 0; i < instance->type_arguments.count; ++i)
        if (!v_hash_type(&sha, plan, plan->instance_type_ids[
                instance->type_arguments.offset + i], 0)) return false;
    return v_hash_dictionary(&sha, plan, instance->dictionary)
        && v_hash_effect(&sha, plan, instance->effect_tail)
        && v_hash_effect(&sha, plan, instance->effects)
        && v_finish(&sha, digest);
}

static bool v_hash_signature(SolMirLinkageSha256 *sha, const SolMirPlan *plan,
    SolIrCallableId callable, SolMirPlanTypeId receiver,
    SolAccessMode receiver_access, SolMirPlanSlice parameters,
    SolMirPlanSlice accesses, SolMirPlanTypeId result,
    SolMirPlanEffectRowId effects) {
    v_token(sha, V_TOKEN_SIGNATURE);
    if (!v_hash_callable(sha, plan->program->ir, callable)) return false;
    if (receiver == SOL_MIR_PLAN_NONE) v_token(sha, V_TOKEN_ABSENT);
    else {
        const char *access = v_access_label(receiver_access);
        if (access == NULL) return false;
        v_token(sha, V_TOKEN_PRESENT); v_label(sha, access);
        if (!v_hash_type(sha, plan, receiver, 0)) return false;
    }
    if (parameters.offset > plan->instance_type_id_count
        || parameters.count > plan->instance_type_id_count - parameters.offset
        || accesses.offset > plan->instance_access_count
        || accesses.count != parameters.count
        || accesses.count > plan->instance_access_count - accesses.offset)
        return false;
    v_sequence(sha, parameters.count);
    for (size_t i = 0; i < parameters.count; ++i) {
        const char *access = v_access_label(plan->instance_accesses[
            accesses.offset + i]);
        if (access == NULL) return false;
        v_label(sha, access);
        if (!v_hash_type(sha, plan,
                plan->instance_type_ids[parameters.offset + i], 0)) return false;
    }
    return v_hash_type(sha, plan, result, 0)
        && v_hash_effect(sha, plan, effects);
}

static bool v_host_key(const SolMirLinkage *linkage,
    SolMirMaterializedImportId import_id, SolMirLinkageDigest *digest,
    SolMirLinkageWorkMeter *work) {
    const SolMirMaterialization *materialization = linkage->operations->layout
        ->representation->materialization;
    const SolMirPlan *plan = materialization->plan;
    if (import_id >= materialization->import_count) return false;
    const SolMirMaterializedImport *materialized = &materialization->imports[
        import_id];
    if (materialized->source_import >= plan->import_count) return false;
    const SolMirPlanImport *import = &plan->imports[materialized->source_import];
    SolMirLinkageSha256 sha;
    v_start(&sha, "sol.mir.host-requirement-key/1", work);
    return v_hash_signature(&sha, plan, import->callable, import->receiver,
            materialized->receiver_access, import->parameter_types,
            import->parameter_accesses, import->result, import->effects)
        && v_finish(&sha, digest);
}

static bool v_recipe_key(const SolMirLinkage *linkage, SolMirRecipeId recipe,
    SolMirLinkageDigest *digest, SolMirLinkageWorkMeter *work) {
    const SolMirPlan *plan = linkage->operations->layout->representation
        ->materialization->plan;
    SolMirLinkageSha256 sha;
    v_start(&sha, "sol.mir.runtime-recipe-key/1", work);
    return v_hash_type(&sha, plan, recipe, 0)
        && v_finish(&sha, digest);
}

static bool v_table_key(const SolMirLinkage *linkage,
    SolMirLinkageTargetKind kind, SolMirLinkageCallableId internal,
    SolMirLinkageHostRequirementId host, SolMirLinkageDigest *digest,
    SolMirLinkageWorkMeter *work) {
    SolMirLinkageSha256 sha;
    v_start(&sha, "sol.mir.function-table-key/1", work);
    v_token(&sha, V_TOKEN_TABLE);
    if (kind == SOL_MIR_LINKAGE_TARGET_INTERNAL) {
        if (internal >= linkage->callable_count || host != SOL_MIR_LINKAGE_NONE)
            return false;
        v_label(&sha, "internal");
        v_bytes(&sha, linkage->callables[internal].symbol.bytes,
            SOL_MIR_LINKAGE_SYMBOL_LENGTH);
    } else if (kind == SOL_MIR_LINKAGE_TARGET_HOST) {
        if (host >= linkage->host_requirement_count
            || internal != SOL_MIR_LINKAGE_NONE) return false;
        v_label(&sha, "host");
        v_semantic(&sha, linkage->host_requirements[host].semantic_id);
        v_bytes(&sha, linkage->host_requirements[host].requirement_key.bytes,
            SOL_MIR_LINKAGE_DIGEST_BYTES);
    } else return false;
    return v_finish(&sha, digest);
}

static bool valid_symbol(const SolMirLinkageSymbol *symbol, char namespace_kind) {
    if (memcmp(symbol->bytes, "sol.", 4) != 0
        || symbol->bytes[4] != namespace_kind || symbol->bytes[5] != '1'
        || symbol->bytes[6] != '.'
        || symbol->bytes[39] != '.'
        || symbol->bytes[SOL_MIR_LINKAGE_SYMBOL_LENGTH] != '\0') return false;
    if (!v_tick(SOL_MIR_LINKAGE_SYMBOL_LENGTH - 7)) return false;
    for (size_t i = 7; i < SOL_MIR_LINKAGE_SYMBOL_LENGTH; ++i) {
        if (i == 39) continue;
        char byte = symbol->bytes[i];
        if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
            return false;
    }
    return true;
}

static bool v_symbol(char namespace_kind, SolSemanticId semantic,
    const SolMirLinkageDigest *digest, SolMirLinkageSymbol *symbol) {
    static const char hex[] = "0123456789abcdef";
    if (!v_tick(SOL_MIR_LINKAGE_SYMBOL_LENGTH)) return false;
    memcpy(symbol->bytes, "sol.x1.", 7);
    symbol->bytes[4] = namespace_kind;
    size_t at = 7;
    uint64_t words[2] = {semantic.high, semantic.low};
    for (size_t word = 0; word < 2; ++word)
        for (size_t nibble = 0; nibble < 16; ++nibble)
            symbol->bytes[at++] = hex[(words[word]
                >> ((15 - nibble) * 4)) & 15u];
    symbol->bytes[at++] = '.';
    for (size_t i = 0; i < SOL_MIR_LINKAGE_DIGEST_BYTES; ++i) {
        symbol->bytes[at++] = hex[digest->bytes[i] >> 4];
        symbol->bytes[at++] = hex[digest->bytes[i] & 15u];
    }
    symbol->bytes[at] = '\0';
    return at == SOL_MIR_LINKAGE_SYMBOL_LENGTH;
}

static bool validate_callables(const SolMirLinkage *linkage,
    const SolMirMaterialization *materialization, const SolIr *ir,
    size_t *callable_map) {
    if (!v_tick(linkage->callable_count)) return false;
    for (size_t i = 0; i < linkage->callable_count; ++i) {
        const SolMirLinkageCallable *item = &linkage->callables[i];
        if (item->instance >= materialization->image_count
            || callable_map[item->instance] != SOL_MIR_LINKAGE_NONE)
            return false;
        const SolMirMaterializedImage *image
            = &materialization->images[item->instance];
        callable_map[item->instance] = i;
        if (!v_tick(i)) return false;
        for (size_t previous = 0; previous < i; ++previous)
            if (digest_equal(&linkage->callables[previous].instance_key,
                    &item->instance_key)) return false;
        if (image->instance != item->instance
            || image->source_callable >= ir->callable_count
            || ir->callables[image->source_callable].owner >= ir->definition_count)
            return false;
        SolSemanticId expected_semantic = ir->definitions[
            ir->callables[image->source_callable].owner].semantic_id;
        SolMirLinkageDigest expected_digest; SolMirLinkageSymbol expected_symbol;
        if (!semantic_equal(item->semantic_id, expected_semantic)
            || !v_instance_key(linkage, item->instance, &expected_digest, NULL)
            || !digest_equal(&item->instance_key, &expected_digest)) return false;
        if (!v_symbol('i', expected_semantic, &expected_digest,
                &expected_symbol)
            || !valid_symbol(&item->symbol, 'i')
            || memcmp(&item->symbol, &expected_symbol, sizeof(expected_symbol)) != 0
            || (i != 0 && memcmp(linkage->callables[i - 1].symbol.bytes,
                item->symbol.bytes, SOL_MIR_LINKAGE_SYMBOL_LENGTH) >= 0))
            return false;
    }
    if (!v_tick(materialization->image_count)) return false;
    for (size_t i = 0; i < materialization->image_count; ++i)
        if (materialization->images[i].instance >= materialization->image_count
            || callable_map[materialization->images[i].instance]
                == SOL_MIR_LINKAGE_NONE)
            return false;
    return true;
}

static bool approved(const SolMirProgram *program, SolIrCallableId callable) {
    if (!v_tick(program->approved_import_count)) return false;
    for (size_t i = 0; i < program->approved_import_count; ++i)
        if (program->approved_imports[i] == callable) return true;
    return false;
}

static bool validate_hosts(const SolMirLinkage *linkage,
    const SolMirMaterialization *materialization, const SolMirProgram *program,
    const SolIr *ir, size_t *host_map, const unsigned char *referenced) {
    if (!v_tick(linkage->host_requirement_count)) return false;
    bool valid = linkage->operations->import_envelope_count
        == materialization->import_count;
    for (size_t i = 0; valid && i < linkage->host_requirement_count; ++i) {
        const SolMirLinkageHostRequirement *item = &linkage->host_requirements[i];
        if (item->import >= materialization->import_count
            || host_map[item->import] != SOL_MIR_LINKAGE_NONE) {
            valid = false; break;
        }
        host_map[item->import] = i;
        const SolMirMaterializedImport *source
            = &materialization->imports[item->import];
        if (source->source_callable >= ir->callable_count) { valid = false; break; }
        const SolIrCallable *callable = &ir->callables[source->source_callable];
        if (callable->owner >= ir->definition_count
            || callable->kind != SOL_IR_CALLABLE_CAPABILITY
            || callable->body != SOL_IR_NONE
            || !approved(program, source->source_callable)) { valid = false; break; }
        const SolMirImportContractEnvelope *envelope
            = &linkage->operations->import_envelopes[item->import];
        SolMirLinkageDigest expected;
        if (envelope->import != item->import || !envelope->host_invoke
            || envelope->receiver != source->receiver
            || envelope->receiver_access != source->receiver_access
            || memcmp(&envelope->parameters, &source->parameter_types,
                sizeof(envelope->parameters)) != 0
            || memcmp(&envelope->parameter_accesses,
                &source->parameter_accesses,
                sizeof(envelope->parameter_accesses)) != 0
            || envelope->result != source->result
            || envelope->effects != source->effects
            || !semantic_equal(item->semantic_id,
                ir->definitions[callable->owner].semantic_id)
            || item->receiver != source->receiver
            || item->receiver_access != source->receiver_access
            || memcmp(&item->parameters, &source->parameter_types,
                sizeof(item->parameters)) != 0
            || memcmp(&item->parameter_accesses, &source->parameter_accesses,
                sizeof(item->parameter_accesses)) != 0
            || item->result != source->result || item->effects != source->effects
            || !v_host_key(linkage, item->import, &expected, NULL)
            || !digest_equal(&item->requirement_key, &expected)) {
            valid = false; break;
        }
        if ((i != 0 && memcmp(linkage->host_requirements[i - 1]
                    .requirement_key.bytes, item->requirement_key.bytes,
                SOL_MIR_LINKAGE_DIGEST_BYTES) >= 0)
            || !referenced[item->import]) valid = false;
    }
    if (valid && !v_tick(materialization->import_count)) return false;
    if (valid) for (size_t i = 0; i < materialization->import_count; ++i)
        if (host_map[i] == SOL_MIR_LINKAGE_NONE) {
            valid = false; break;
        }
    return valid;
}

static bool validate_bindings(const SolMirLinkage *linkage,
    const SolMirMaterialization *materialization, const size_t *callable_map,
    const size_t *host_map) {
    if (!v_tick(linkage->binding_count)) return false;
    for (size_t i = 0; i < linkage->binding_count; ++i) {
        const SolMirLinkageBinding *actual = &linkage->bindings[i];
        const SolMirMaterializedBinding *source = &materialization->bindings[i];
        if (actual->binding != i) return false;
        if (source->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE) {
            if (actual->target_kind != SOL_MIR_LINKAGE_TARGET_INTERNAL
                || actual->host != SOL_MIR_LINKAGE_NONE
                || source->instance >= materialization->image_count
                || actual->internal != callable_map[source->instance])
                return false;
        } else if (source->target_kind == SOL_MIR_MATERIALIZED_TARGET_IMPORT) {
            if (source->kind == SOL_MIR_PLAN_DEMAND_ROOT
                || actual->target_kind != SOL_MIR_LINKAGE_TARGET_HOST
                || actual->internal != SOL_MIR_LINKAGE_NONE
                || source->import >= materialization->import_count
                || actual->host != host_map[source->import])
                return false;
        } else return false;
    }
    return true;
}

static bool validate_exports(const SolMirLinkage *linkage,
    const SolMirMaterialization *materialization, const SolMirProgram *program) {
    if (!v_tick(program->root_count)) return false;
    size_t expected_count = 0;
    for (size_t i = 0; i < program->root_count; ++i)
        expected_count += program->roots[i].kind == SOL_MIR_PROGRAM_ROOT_ENTRY;
    if (expected_count != linkage->entry_export_count) return false;
    if (!v_tick(linkage->entry_export_count)) return false;
    for (size_t i = 0; i < linkage->entry_export_count; ++i) {
        const SolMirLinkageEntryExport *item = &linkage->entry_exports[i];
        if (item->root_binding >= materialization->binding_count
            || materialization->bindings[item->root_binding].kind
                != SOL_MIR_PLAN_DEMAND_ROOT
            || item->callable >= linkage->callable_count
            || linkage->bindings[item->root_binding].target_kind
                != SOL_MIR_LINKAGE_TARGET_INTERNAL
            || linkage->bindings[item->root_binding].internal != item->callable)
            return false;
        bool entry = false;
        if (!v_tick(program->root_count)) return false;
        for (size_t root = 0; root < program->root_count; ++root)
            if (program->roots[root].kind == SOL_MIR_PROGRAM_ROOT_ENTRY
                && program->roots[root].callable
                    == materialization->bindings[item->root_binding]
                        .symbolic_callable) entry = true;
        SolMirLinkageSymbol expected;
        if (!v_symbol('e', linkage->callables[item->callable].semantic_id,
                &linkage->callables[item->callable].instance_key, &expected)
            || !entry || !valid_symbol(&item->symbol, 'e')
            || memcmp(&item->symbol, &expected, sizeof(expected)) != 0
            || (i != 0 && memcmp(linkage->entry_exports[i - 1].symbol.bytes,
                item->symbol.bytes, SOL_MIR_LINKAGE_SYMBOL_LENGTH) >= 0))
            return false;
        if (!v_tick(i)) return false;
        for (size_t previous = 0; previous < i; ++previous)
            if (linkage->entry_exports[previous].root_binding
                    == item->root_binding)
                return false;
    }
    if (!v_tick(program->root_count)) return false;
    for (size_t root = 0; root < program->root_count; ++root) {
        if (program->roots[root].kind != SOL_MIR_PROGRAM_ROOT_ENTRY) continue;
        size_t matches = 0;
        if (!v_tick(linkage->entry_export_count)) return false;
        for (size_t i = 0; i < linkage->entry_export_count; ++i)
            if (materialization->bindings[
                    linkage->entry_exports[i].root_binding].symbolic_callable
                == program->roots[root].callable) ++matches;
        if (matches != 1) return false;
    }
    return true;
}

static bool expected_table_target(const SolMirLinkage *linkage,
    const SolMirOperationCallablePlan *plan, SolMirLinkageTargetKind *kind,
    size_t *target, const size_t *callable_map, const size_t *host_map) {
    const SolMirMaterialization *materialization = linkage->operations->layout
        ->representation->materialization;
    if (plan->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE) {
        if (plan->target_instance >= materialization->image_count) return false;
        *kind = SOL_MIR_LINKAGE_TARGET_INTERNAL;
        *target = callable_map[plan->target_instance];
    } else if (plan->target_kind == SOL_MIR_MATERIALIZED_TARGET_IMPORT) {
        if (plan->target_import >= materialization->import_count) return false;
        *kind = SOL_MIR_LINKAGE_TARGET_HOST;
        *target = host_map[plan->target_import];
    } else return false;
    return *target != SOL_MIR_LINKAGE_NONE;
}

static bool validate_tables(const SolMirLinkage *linkage,
    const size_t *callable_map, const size_t *host_map,
    unsigned char *used) {
    if (!v_tick(linkage->table_entry_count)) return false;
    for (size_t i = 0; i < linkage->table_entry_count; ++i) {
        const SolMirLinkageTableEntry *item = &linkage->table_entries[i];
        SolMirLinkageDigest expected;
        if (!v_table_key(linkage, item->target_kind, item->internal,
                item->host, &expected, NULL)
            || !digest_equal(&item->identity, &expected)
            || (i != 0 && memcmp(linkage->table_entries[i - 1].identity.bytes,
                item->identity.bytes, SOL_MIR_LINKAGE_DIGEST_BYTES) >= 0))
            return false;
    }
    if (!v_tick(linkage->callable_value_count)) return false;
    bool valid = true;
    for (size_t i = 0; valid && i < linkage->callable_value_count; ++i) {
        const SolMirLinkageCallableValue *value = &linkage->callable_values[i];
        if (value->callable_plan != i || value->table >= linkage->table_entry_count) {
            valid = false; break;
        }
        SolMirLinkageTargetKind kind; size_t target;
        if (!expected_table_target(linkage, &linkage->operations->callables[i],
                &kind, &target, callable_map, host_map)) {
            valid = false; break;
        }
        const SolMirLinkageTableEntry *entry = &linkage->table_entries[value->table];
        if (entry->target_kind != kind
            || (kind == SOL_MIR_LINKAGE_TARGET_INTERNAL
                && entry->internal != target)
            || (kind == SOL_MIR_LINKAGE_TARGET_HOST && entry->host != target)) {
            valid = false; break;
        }
        used[value->table] = 1;
    }
    if (valid && !v_tick(linkage->table_entry_count)) return false;
    if (valid) for (size_t i = 0; i < linkage->table_entry_count; ++i)
        if (!used[i]) { valid = false; break; }
    return valid;
}

static uint32_t runtime_flags(const SolMirLinkage *linkage, size_t recipe_id) {
    const SolMirRepresentation *representation = linkage->operations->layout
        ->representation;
    const SolMirRecipe *recipe = &representation->recipes[recipe_id];
    uint32_t flags = 0;
    if (recipe->inhabited && !recipe->zero_sized
        && recipe->storage != SOL_MIR_STORAGE_NONE
        && recipe->storage != SOL_MIR_STORAGE_SCALAR)
        flags |= SOL_MIR_LINKAGE_RUNTIME_CREATE;
    if (recipe->copy_kind == SOL_MIR_COPY_TEXT
        || recipe->copy_kind == SOL_MIR_COPY_AGGREGATE
        || recipe->copy_kind == SOL_MIR_COPY_WRAPPER)
        flags |= SOL_MIR_LINKAGE_RUNTIME_COPY;
    if (recipe->drop_kind != SOL_MIR_DROP_NONE)
        flags |= SOL_MIR_LINKAGE_RUNTIME_DROP;
    if (!v_tick(linkage->operations->equality_node_count)) return UINT32_MAX;
    for (size_t i = 0; i < linkage->operations->equality_node_count; ++i)
        if (linkage->operations->equality_nodes[i].recipe == recipe_id
            && linkage->operations->equality_nodes[i].kind
                != SOL_MIR_OPERATION_EQUAL_SCALAR) {
            flags |= SOL_MIR_LINKAGE_RUNTIME_EQUAL; break;
        }
    if (!v_tick(linkage->operations->callable_count)) return UINT32_MAX;
    for (size_t i = 0; i < linkage->operations->callable_count; ++i)
        if (linkage->operations->callables[i].kind
                == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION
            && linkage->operations->callables[i].function_recipe == recipe_id) {
            flags |= SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT; break;
        }
    return flags;
}

static bool validate_runtime(const SolMirLinkage *linkage,
    unsigned char *seen) {
    const SolMirRepresentation *representation = linkage->operations->layout
        ->representation;
    if (!v_tick(linkage->runtime_requirement_count)) return false;
    bool valid = true;
    for (size_t i = 0; valid && i < linkage->runtime_requirement_count; ++i) {
        const SolMirLinkageRuntimeRequirement *item
            = &linkage->runtime_requirements[i];
        if (item->recipe >= representation->recipe_count
            || seen[item->recipe]) {
            valid = false; break;
        }
        seen[item->recipe] = 1;
        const SolMirRecipe *recipe = &representation->recipes[item->recipe];
        SolMirLinkageDigest expected;
        if (item->operations != runtime_flags(linkage, item->recipe)
            || item->operations == 0 || item->storage != recipe->storage
            || item->copy_kind != recipe->copy_kind
            || item->drop_kind != recipe->drop_kind
            || !v_recipe_key(linkage, item->recipe, &expected, NULL)
            || !digest_equal(&item->recipe_key, &expected)
            || (i != 0 && memcmp(linkage->runtime_requirements[i - 1]
                    .recipe_key.bytes, item->recipe_key.bytes,
                SOL_MIR_LINKAGE_DIGEST_BYTES) >= 0)) valid = false;
    }
    if (valid && !v_tick(representation->recipe_count)) return false;
    if (valid) for (size_t i = 0; i < representation->recipe_count; ++i) {
        if ((runtime_flags(linkage, i) != 0) != (seen[i] != 0)) {
            valid = false; break;
        }
    }
    return valid;
}

typedef int (*VReplayCompare)(const void *, const void *);
typedef struct { SolMirLinkageTableEntry entry; } VReplayTableCandidate;

static bool replay_tick(SolMirLinkageWorkMeter *work, size_t amount) {
    return v_tick(amount) && sol_mir_linkage_internal_work_tick(work, amount);
}

static bool replay_mul(size_t left, size_t right, size_t *result) {
    if (left != 0 && right > SIZE_MAX / left) return false;
    *result = left * right; return true;
}

static int replay_digest_compare(const SolMirLinkageDigest *left,
    const SolMirLinkageDigest *right) {
    return memcmp(left->bytes, right->bytes, SOL_MIR_LINKAGE_DIGEST_BYTES);
}

static int replay_compare_callable_item(const void *left, const void *right) {
    const SolMirLinkageCallable *a = left, *b = right;
    return memcmp(a->symbol.bytes, b->symbol.bytes,
        SOL_MIR_LINKAGE_SYMBOL_LENGTH);
}

static int replay_compare_host_item(const void *left, const void *right) {
    const SolMirLinkageHostRequirement *a = left, *b = right;
    int order = replay_digest_compare(&a->requirement_key, &b->requirement_key);
    if (order != 0) return order;
    order = v_compare_u64(a->semantic_id.high, b->semantic_id.high);
    return order != 0 ? order
        : v_compare_u64(a->semantic_id.low, b->semantic_id.low);
}

static int replay_compare_export_item(const void *left, const void *right) {
    const SolMirLinkageEntryExport *a = left, *b = right;
    return memcmp(a->symbol.bytes, b->symbol.bytes,
        SOL_MIR_LINKAGE_SYMBOL_LENGTH);
}

static int replay_compare_table_item(const void *left, const void *right) {
    const VReplayTableCandidate *a = left, *b = right;
    return replay_digest_compare(&a->entry.identity, &b->entry.identity);
}

static int replay_compare_runtime_item(const void *left, const void *right) {
    const SolMirLinkageRuntimeRequirement *a = left, *b = right;
    return replay_digest_compare(&a->recipe_key, &b->recipe_key);
}

static bool replay_compare_items(SolMirLinkageWorkMeter *work,
    const unsigned char *left, const unsigned char *right, size_t size,
    VReplayCompare compare, int *order) {
    if (!replay_tick(work, size)) return false;
    *order = compare(left, right); return true;
}

static bool replay_swap_items(SolMirLinkageWorkMeter *work,
    unsigned char *left, unsigned char *right, size_t size) {
    if (left == right) return true;
    size_t amount;
    if (!replay_mul(size, 3, &amount) || !replay_tick(work, amount))
        return false;
    for (size_t i = 0; i < size; ++i) {
        unsigned char byte = left[i]; left[i] = right[i]; right[i] = byte;
    }
    return true;
}

static bool replay_sift_down(SolMirLinkageWorkMeter *work,
    unsigned char *base, size_t start, size_t count, size_t size,
    VReplayCompare compare) {
    size_t root = start;
    while (root < count / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count) {
            int order;
            if (!replay_compare_items(work, base + child * size,
                    base + (child + 1) * size, size, compare, &order))
                return false;
            if (order < 0) ++child;
        }
        int order;
        if (!replay_compare_items(work, base + root * size,
                base + child * size, size, compare, &order)) return false;
        if (order >= 0) return true;
        if (!replay_swap_items(work, base + root * size,
                base + child * size, size)) return false;
        root = child;
    }
    return true;
}

static bool replay_sort(SolMirLinkageWorkMeter *work, void *items,
    size_t count, size_t size, VReplayCompare compare) {
    if (count < 2) return true;
    unsigned char *base = items;
    for (size_t start = count / 2; start != 0; --start)
        if (!replay_sift_down(work, base, start - 1, count, size, compare))
            return false;
    for (size_t end = count; end > 1; --end)
        if (!replay_swap_items(work, base, base + (end - 1) * size, size)
            || !replay_sift_down(work, base, 0, end - 1, size, compare))
            return false;
    return true;
}

static SolMirLinkageCallableId replay_callable_for_instance(
    const SolMirLinkage *linkage, SolMirPlanInstanceId instance,
    SolMirLinkageWorkMeter *work) {
    for (size_t i = 0; i < linkage->callable_count; ++i) {
        if (!replay_tick(work, 1)) return SOL_MIR_LINKAGE_NONE;
        if (linkage->callables[i].instance == instance) return i;
    }
    return SOL_MIR_LINKAGE_NONE;
}

static SolMirLinkageHostRequirementId replay_host_for_import(
    const SolMirLinkage *linkage, SolMirMaterializedImportId import,
    SolMirLinkageWorkMeter *work) {
    for (size_t i = 0; i < linkage->host_requirement_count; ++i) {
        if (!replay_tick(work, 1)) return SOL_MIR_LINKAGE_NONE;
        if (linkage->host_requirements[i].import == import) return i;
    }
    return SOL_MIR_LINKAGE_NONE;
}

static bool replay_approved(const SolMirProgram *program,
    SolIrCallableId callable, SolMirLinkageWorkMeter *work) {
    for (size_t i = 0; i < program->approved_import_count; ++i) {
        if (!replay_tick(work, 1)) return false;
        if (program->approved_imports[i] == callable) return true;
    }
    return false;
}

static bool replay_dictionary_equal(const SolMirPlan *plan,
    SolMirPlanSlice left, SolMirPlanSlice right, bool *valid,
    SolMirLinkageWorkMeter *work) {
    if (left.count != right.count) return false;
    for (size_t i = 0; i < left.count; ++i) {
        bool matched = false;
        for (size_t q = 0; q < right.count; ++q) {
            int order = v_compare_dictionary_entry(plan,
                &plan->dictionary_entries[left.offset + i],
                &plan->dictionary_entries[right.offset + q], valid, work);
            if (!*valid) return false;
            if (order == 0) { matched = true; break; }
        }
        if (!matched) return false;
    }
    return true;
}

static bool replay_instance_equal(const SolMirLinkage *linkage,
    SolMirPlanInstanceId left_id, SolMirPlanInstanceId right_id, bool *equal,
    SolMirLinkageWorkMeter *work) {
    const SolMirPlan *plan = linkage->operations->layout->representation
        ->materialization->plan;
    const SolMirPlanInstance *left = &plan->instances[left_id];
    const SolMirPlanInstance *right = &plan->instances[right_id];
    bool valid = true;
    if (v_compare_callable(plan->program->ir, left->callable, right->callable,
            &valid, work) != 0) { *equal = false; return valid; }
    bool left_receiver = left->receiver != SOL_MIR_PLAN_NONE;
    bool right_receiver = right->receiver != SOL_MIR_PLAN_NONE;
    if (left_receiver != right_receiver) { *equal = false; return true; }
    if (left_receiver && v_compare_type(plan, left->receiver, right->receiver,
            0, &valid, work) != 0) { *equal = false; return valid; }
    if (left->type_arguments.count != right->type_arguments.count) {
        *equal = false; return true;
    }
    for (size_t i = 0; i < left->type_arguments.count; ++i)
        if (v_compare_type(plan, plan->instance_type_ids[
                left->type_arguments.offset + i], plan->instance_type_ids[
                right->type_arguments.offset + i], 0, &valid, work) != 0) {
            *equal = false; return valid;
        }
    if (!replay_dictionary_equal(plan, left->dictionary, right->dictionary,
            &valid, work)) { *equal = false; return valid; }
    if (v_compare_effect(plan, left->effect_tail, right->effect_tail, &valid,
            work) != 0
        || v_compare_effect(plan, left->effects, right->effects, &valid,
            work) != 0) { *equal = false; return valid; }
    *equal = true; return valid;
}

static bool replay_host_equal(const SolMirLinkage *linkage,
    SolMirMaterializedImportId left_id, SolMirMaterializedImportId right_id,
    bool *equal, SolMirLinkageWorkMeter *work) {
    const SolMirMaterialization *materialization = linkage->operations->layout
        ->representation->materialization;
    const SolMirPlan *plan = materialization->plan;
    const SolMirMaterializedImport *left_m = &materialization->imports[left_id];
    const SolMirMaterializedImport *right_m = &materialization->imports[right_id];
    const SolMirPlanImport *left = &plan->imports[left_m->source_import];
    const SolMirPlanImport *right = &plan->imports[right_m->source_import];
    bool valid = true;
    if (v_compare_callable(plan->program->ir, left->callable, right->callable,
            &valid, work) != 0) { *equal = false; return valid; }
    bool left_receiver = left->receiver != SOL_MIR_PLAN_NONE;
    bool right_receiver = right->receiver != SOL_MIR_PLAN_NONE;
    if (left_receiver != right_receiver
        || (left_receiver && left_m->receiver_access
            != right_m->receiver_access)) { *equal = false; return true; }
    if (left_receiver && v_compare_type(plan, left->receiver, right->receiver,
            0, &valid, work) != 0) { *equal = false; return valid; }
    if (left->parameter_types.count != right->parameter_types.count) {
        *equal = false; return true;
    }
    for (size_t i = 0; i < left->parameter_types.count; ++i)
        if (plan->instance_accesses[left->parameter_accesses.offset + i]
                != plan->instance_accesses[right->parameter_accesses.offset + i]
            || v_compare_type(plan, plan->instance_type_ids[
                left->parameter_types.offset + i], plan->instance_type_ids[
                right->parameter_types.offset + i], 0, &valid, work) != 0) {
            *equal = false; return valid;
        }
    if (v_compare_type(plan, left->result, right->result, 0, &valid, work) != 0
        || v_compare_effect(plan, left->effects, right->effects, &valid,
            work) != 0) { *equal = false; return valid; }
    *equal = true; return valid;
}

static bool replay_table_equal(const SolMirLinkage *linkage,
    const SolMirLinkageTableEntry *left, const SolMirLinkageTableEntry *right,
    bool *equal, SolMirLinkageWorkMeter *work) {
    if (left->target_kind != right->target_kind) {
        *equal = false; return true;
    }
    if (left->target_kind == SOL_MIR_LINKAGE_TARGET_INTERNAL)
        return replay_instance_equal(linkage,
            linkage->callables[left->internal].instance,
            linkage->callables[right->internal].instance, equal, work);
    return replay_host_equal(linkage,
        linkage->host_requirements[left->host].import,
        linkage->host_requirements[right->host].import, equal, work);
}

static bool replay_table_target(const SolMirLinkage *linkage,
    const SolMirOperationCallablePlan *plan, SolMirLinkageTableEntry *entry,
    SolMirLinkageWorkMeter *work) {
    *entry = (SolMirLinkageTableEntry){.internal = SOL_MIR_LINKAGE_NONE,
        .host = SOL_MIR_LINKAGE_NONE};
    if (plan->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE) {
        entry->target_kind = SOL_MIR_LINKAGE_TARGET_INTERNAL;
        entry->internal = replay_callable_for_instance(linkage,
            plan->target_instance, work);
        if (entry->internal == SOL_MIR_LINKAGE_NONE) return false;
    } else if (plan->target_kind == SOL_MIR_MATERIALIZED_TARGET_IMPORT) {
        entry->target_kind = SOL_MIR_LINKAGE_TARGET_HOST;
        entry->host = replay_host_for_import(linkage, plan->target_import, work);
        if (entry->host == SOL_MIR_LINKAGE_NONE) return false;
    } else return false;
    return v_table_key(linkage, entry->target_kind, entry->internal,
        entry->host, &entry->identity, work);
}

static bool replay_runtime_flags(const SolMirLinkage *linkage,
    SolMirRecipeId recipe_id, uint32_t *result, SolMirLinkageWorkMeter *work) {
    const SolMirRecipe *recipe = &linkage->operations->layout->representation
        ->recipes[recipe_id];
    uint32_t flags = 0;
    if (recipe->inhabited && !recipe->zero_sized
        && recipe->storage != SOL_MIR_STORAGE_NONE
        && recipe->storage != SOL_MIR_STORAGE_SCALAR)
        flags |= SOL_MIR_LINKAGE_RUNTIME_CREATE;
    if (recipe->copy_kind == SOL_MIR_COPY_TEXT
        || recipe->copy_kind == SOL_MIR_COPY_AGGREGATE
        || recipe->copy_kind == SOL_MIR_COPY_WRAPPER)
        flags |= SOL_MIR_LINKAGE_RUNTIME_COPY;
    if (recipe->drop_kind != SOL_MIR_DROP_NONE)
        flags |= SOL_MIR_LINKAGE_RUNTIME_DROP;
    for (size_t i = 0; i < linkage->operations->equality_node_count; ++i) {
        if (!replay_tick(work, 1)) return false;
        if (linkage->operations->equality_nodes[i].recipe == recipe_id
            && linkage->operations->equality_nodes[i].kind
                != SOL_MIR_OPERATION_EQUAL_SCALAR) {
            flags |= SOL_MIR_LINKAGE_RUNTIME_EQUAL; break;
        }
    }
    for (size_t i = 0; i < linkage->operations->callable_count; ++i) {
        if (!replay_tick(work, 1)) return false;
        if (linkage->operations->callables[i].kind
                == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION
            && linkage->operations->callables[i].function_recipe == recipe_id) {
            flags |= SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT; break;
        }
    }
    *result = flags; return true;
}

static bool replay_build_work(const SolMirLinkage *linkage, void *scratch,
    size_t *result) {
    const SolMirMaterialization *m = linkage->operations->layout
        ->representation->materialization;
    const SolMirRepresentation *representation = linkage->operations->layout
        ->representation;
    const SolMirProgram *program = m->plan->program;
    const SolIr *ir = program->ir;
    SolMirLinkageWorkMeter work = {.limit = SIZE_MAX};
    size_t entries = 0, runtime = 0;
    for (size_t i = 0; i < program->root_count; ++i) {
        if (!replay_tick(&work, 1)) return false;
        entries += program->roots[i].kind == SOL_MIR_PROGRAM_ROOT_ENTRY;
    }
    for (size_t i = 0; i < representation->recipe_count; ++i) {
        uint32_t flags;
        if (!replay_tick(&work, 1)
            || !replay_runtime_flags(linkage, i, &flags, &work)) return false;
        runtime += flags != 0;
    }

    SolMirLinkageCallable *callables = scratch;
    for (size_t i = 0; i < m->image_count; ++i) {
        if (!replay_tick(&work, 1)) return false;
        const SolMirMaterializedImage *image = &m->images[i];
        const SolIrCallable *source = &ir->callables[image->source_callable];
        SolMirLinkageCallable *target = &callables[i];
        target->instance = image->instance;
        target->semantic_id = ir->definitions[source->owner].semantic_id;
        if (!v_instance_key(linkage, image->instance, &target->instance_key,
                &work)
            || !replay_tick(&work, SOL_MIR_LINKAGE_SYMBOL_LENGTH)
            || !v_symbol('i', target->semantic_id, &target->instance_key,
                &target->symbol)) return false;
    }
    if (!replay_sort(&work, callables, m->image_count, sizeof(*callables),
            replay_compare_callable_item)) return false;
    for (size_t i = 0; i < m->image_count; ++i)
        for (size_t previous = 0; previous < i; ++previous) {
            if (!replay_tick(&work, 1)) return false;
            if (digest_equal(&callables[previous].instance_key,
                    &callables[i].instance_key)) {
                bool equal;
                if (!replay_instance_equal(linkage, callables[previous].instance,
                        callables[i].instance, &equal, &work) || equal)
                    return false;
            }
        }

    SolMirLinkageHostRequirement *hosts = scratch;
    for (size_t i = 0; i < m->import_count; ++i) {
        if (!replay_tick(&work, 1)) return false;
        const SolMirMaterializedImport *source = &m->imports[i];
        const SolIrCallable *callable = &ir->callables[source->source_callable];
        if (!replay_approved(program, source->source_callable, &work))
            return false;
        SolMirLinkageHostRequirement *target = &hosts[i];
        *target = (SolMirLinkageHostRequirement){.import = i,
            .semantic_id = ir->definitions[callable->owner].semantic_id,
            .receiver = source->receiver,
            .receiver_access = source->receiver_access,
            .parameters = source->parameter_types,
            .parameter_accesses = source->parameter_accesses,
            .result = source->result, .effects = source->effects};
        if (!v_host_key(linkage, i, &target->requirement_key, &work))
            return false;
        bool referenced = false;
        for (size_t binding = 0; binding < m->binding_count; ++binding) {
            if (!replay_tick(&work, 1)) return false;
            if (m->bindings[binding].target_kind
                    == SOL_MIR_MATERIALIZED_TARGET_IMPORT
                && m->bindings[binding].import == i) referenced = true;
        }
        for (size_t plan_id = 0; plan_id < linkage->operations->callable_count;
                ++plan_id) {
            if (!replay_tick(&work, 1)) return false;
            if (linkage->operations->callables[plan_id].target_kind
                    == SOL_MIR_MATERIALIZED_TARGET_IMPORT
                && linkage->operations->callables[plan_id].target_import == i)
                referenced = true;
        }
        if (!referenced) return false;
    }
    if (!replay_sort(&work, hosts, m->import_count, sizeof(*hosts),
            replay_compare_host_item)) return false;
    for (size_t i = 1; i < m->import_count; ++i) {
        if (!replay_tick(&work, 1)) return false;
        if (digest_equal(&hosts[i - 1].requirement_key,
                &hosts[i].requirement_key)) {
            bool equal;
            if (!replay_host_equal(linkage, hosts[i - 1].import,
                    hosts[i].import, &equal, &work) || equal) return false;
        }
    }

    for (size_t i = 0; i < m->binding_count; ++i) {
        if (!replay_tick(&work, 1)) return false;
        const SolMirMaterializedBinding *source = &m->bindings[i];
        if (source->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE) {
            if (replay_callable_for_instance(linkage, source->instance, &work)
                    == SOL_MIR_LINKAGE_NONE) return false;
        } else if (source->target_kind == SOL_MIR_MATERIALIZED_TARGET_IMPORT) {
            if (replay_host_for_import(linkage, source->import, &work)
                    == SOL_MIR_LINKAGE_NONE) return false;
        } else return false;
    }

    SolMirLinkageEntryExport *exports = scratch;
    size_t export_at = 0;
    for (size_t root = 0; root < program->root_count; ++root) {
        if (!replay_tick(&work, 1)) return false;
        if (program->roots[root].kind != SOL_MIR_PROGRAM_ROOT_ENTRY) continue;
        size_t binding = SOL_MIR_LINKAGE_NONE;
        for (size_t i = 0; i < m->binding_count; ++i) {
            if (!replay_tick(&work, 1)) return false;
            if (m->bindings[i].kind == SOL_MIR_PLAN_DEMAND_ROOT
                && m->bindings[i].symbolic_callable
                    == program->roots[root].callable) {
                binding = i; break;
            }
        }
        if (binding == SOL_MIR_LINKAGE_NONE) return false;
        SolMirLinkageCallableId callable_id = linkage->bindings[binding].internal;
        exports[export_at] = (SolMirLinkageEntryExport){.root_binding = binding,
            .callable = callable_id};
        if (!replay_tick(&work, SOL_MIR_LINKAGE_SYMBOL_LENGTH)
            || !v_symbol('e', linkage->callables[callable_id].semantic_id,
                &linkage->callables[callable_id].instance_key,
                &exports[export_at].symbol)) return false;
        ++export_at;
    }
    if (export_at != entries
        || !replay_sort(&work, exports, entries, sizeof(*exports),
            replay_compare_export_item)) return false;
    for (size_t i = 1; i < entries; ++i)
        if (!replay_tick(&work, 1)) return false;

    VReplayTableCandidate *candidates = scratch;
    for (size_t i = 0; i < linkage->operations->callable_count; ++i)
        if (!replay_tick(&work, 1)
            || !replay_table_target(linkage, &linkage->operations->callables[i],
                &candidates[i].entry, &work)) return false;
    if (!replay_sort(&work, candidates, linkage->operations->callable_count,
            sizeof(*candidates), replay_compare_table_item)) return false;
    size_t unique = 0;
    for (size_t i = 0; i < linkage->operations->callable_count; ++i) {
        if (!replay_tick(&work, 1)) return false;
        if (i != 0 && digest_equal(&candidates[i - 1].entry.identity,
                &candidates[i].entry.identity)) {
            bool equal;
            if (!replay_table_equal(linkage, &candidates[i - 1].entry,
                    &candidates[i].entry, &equal, &work) || !equal)
                return false;
            continue;
        }
        ++unique;
    }
    for (size_t i = 0; i < linkage->operations->callable_count; ++i)
        if (!replay_tick(&work, 1)) return false;
    for (size_t i = 0; i < linkage->operations->callable_count; ++i) {
        SolMirLinkageTableEntry target;
        if (!replay_tick(&work, 1)
            || !replay_table_target(linkage, &linkage->operations->callables[i],
                &target, &work)) return false;
        bool found = false;
        for (size_t q = 0; q < linkage->table_entry_count; ++q) {
            if (!replay_tick(&work, 1)) return false;
            if (digest_equal(&target.identity,
                    &linkage->table_entries[q].identity)) {
                bool equal;
                if (!replay_table_equal(linkage, &target,
                        &linkage->table_entries[q], &equal, &work) || !equal)
                    return false;
                found = true; break;
            }
        }
        if (!found) return false;
    }
    if (unique != linkage->table_entry_count) return false;

    SolMirLinkageRuntimeRequirement *requirements = scratch;
    size_t runtime_at = 0;
    for (size_t i = 0; i < representation->recipe_count; ++i) {
        if (!replay_tick(&work, 1)) return false;
        uint32_t flags;
        if (!replay_runtime_flags(linkage, i, &flags, &work)) return false;
        if (flags == 0) continue;
        const SolMirRecipe *recipe = &representation->recipes[i];
        SolMirLinkageRuntimeRequirement *target = &requirements[runtime_at++];
        *target = (SolMirLinkageRuntimeRequirement){.recipe = i,
            .operations = flags, .storage = recipe->storage,
            .copy_kind = recipe->copy_kind, .drop_kind = recipe->drop_kind};
        if (!v_recipe_key(linkage, i, &target->recipe_key, &work)) return false;
    }
    if (runtime_at != runtime
        || !replay_sort(&work, requirements, runtime,
            sizeof(*requirements), replay_compare_runtime_item)) return false;
    for (size_t i = 1; i < runtime; ++i) {
        if (!replay_tick(&work, 1)) return false;
        if (digest_equal(&requirements[i - 1].recipe_key,
                &requirements[i].recipe_key)) {
            bool valid = true;
            int order = v_compare_type(m->plan, requirements[i - 1].recipe,
                requirements[i].recipe, 0, &valid, &work);
            if (!valid || order == 0) return false;
        }
    }
    *result = work.used; return !work.exhausted;
}

static bool replay_scratch_bytes(const SolMirLinkage *linkage, size_t *bytes) {
    const SolMirMaterialization *m = linkage->operations->layout
        ->representation->materialization;
    size_t result = 0, part;
#define REPLAY_PART(count, type) \
    if (!replay_mul((count), sizeof(type), &part)) return false; \
    if (part > result) result = part
    REPLAY_PART(m->image_count, SolMirLinkageCallable);
    REPLAY_PART(m->import_count, SolMirLinkageHostRequirement);
    REPLAY_PART(linkage->entry_export_count, SolMirLinkageEntryExport);
    REPLAY_PART(linkage->operations->callable_count, VReplayTableCandidate);
    REPLAY_PART(linkage->runtime_requirement_count,
        SolMirLinkageRuntimeRequirement);
#undef REPLAY_PART
    *bytes = result; return true;
}

static bool usage_equal(SolMirLinkageUsage left, SolMirLinkageUsage right) {
    return left.callables == right.callables && left.bindings == right.bindings
        && left.entry_exports == right.entry_exports
        && left.table_entries == right.table_entries
        && left.callable_values == right.callable_values
        && left.host_requirements == right.host_requirements
        && left.runtime_requirements == right.runtime_requirements
        && left.owned_bytes == right.owned_bytes
        && left.build_scratch_bytes == right.build_scratch_bytes
        && left.validation_scratch_bytes == right.validation_scratch_bytes
        && left.validation_work == right.validation_work;
}

static bool validate(const SolMirLinkage *linkage, SolDiagnostics *diagnostics,
    bool authenticate_resources, size_t *measured_work,
    size_t *measured_scratch) {
    metered_work = 0;
    metered_limit = linkage == NULL ? 0 : linkage->limits.max_validation_work;
    if (linkage == NULL || linkage->operations == NULL
        || !limits_complete(linkage->limits))
        return invalid(diagnostics, "malformed linkage owner header");
    if (!v_tick(linkage->operations->usage.validation_work))
        return invalid(diagnostics,
            "linkage prerequisite validation work limit exceeded");
    if (!sol_mir_operations_validate(linkage->operations, diagnostics))
        return invalid(diagnostics, "invalid borrowed operations owner");
#define CANON(member, type, singular) \
    if (!v_tick(1) \
        || !canonical(linkage->singular##_count, linkage->singular##_capacity, \
            linkage->member)) return invalid(diagnostics, \
                "noncanonical linkage arena header");
    SOL_MIR_LINKAGE_ARENAS(CANON)
#undef CANON
    const SolMirMaterialization *materialization = linkage->operations->layout
        ->representation->materialization;
    const SolMirProgram *program = materialization->plan->program;
    const SolIr *ir = program->ir;
    if (!v_tick(program->root_count))
        return invalid(diagnostics, "linkage validation work limit exceeded");
    size_t entries = 0;
    for (size_t i = 0; i < program->root_count; ++i)
        entries += program->roots[i].kind == SOL_MIR_PROGRAM_ROOT_ENTRY;
    if (linkage->callable_count != materialization->image_count
        || linkage->binding_count != materialization->binding_count
        || linkage->entry_export_count != entries
        || linkage->callable_value_count != linkage->operations->callable_count
        || linkage->host_requirement_count != materialization->import_count
        || linkage->callable_count > linkage->limits.max_callables
        || linkage->binding_count > linkage->limits.max_bindings
        || linkage->entry_export_count > linkage->limits.max_entry_exports
        || linkage->table_entry_count > linkage->limits.max_table_entries
        || linkage->callable_value_count > linkage->limits.max_callable_values
        || linkage->host_requirement_count > linkage->limits.max_host_requirements
        || linkage->runtime_requirement_count
            > linkage->limits.max_runtime_requirements)
        return invalid(diagnostics, "linkage census or arena limit mismatch");
    Range ranges[7]; size_t range_count = 0;
#define RANGE(member, type, singular) \
    if (!v_tick(1) \
        || !add_range(ranges, &range_count, linkage->member, \
            linkage->singular##_count, sizeof(*linkage->member))) \
        return invalid(diagnostics, "overlapping linkage owner arenas");
    SOL_MIR_LINKAGE_ARENAS(RANGE)
#undef RANGE
    if (overlaps(ranges, range_count, linkage->operations, 1,
            sizeof(*linkage->operations)))
        return invalid(diagnostics, "linkage arenas alias the borrowed owner");
#define BORROW_OPERATIONS(member, type, singular) \
    if (!v_tick(1) \
        || overlaps(ranges, range_count, linkage->operations->member, \
            linkage->operations->singular##_capacity, \
            sizeof(*linkage->operations->member))) \
        return invalid(diagnostics, "linkage arenas alias operations storage");
    SOL_MIR_OPERATIONS_ARENAS(BORROW_OPERATIONS)
#undef BORROW_OPERATIONS
    const SolMirLayout *layout = linkage->operations->layout;
    const SolMirRepresentation *representation = layout->representation;
    const SolMirPlan *plan = materialization->plan;
    if (overlaps(ranges, range_count, layout, 1, sizeof(*layout))
        || overlaps(ranges, range_count, representation, 1,
            sizeof(*representation))
        || overlaps(ranges, range_count, materialization, 1,
            sizeof(*materialization))
        || overlaps(ranges, range_count, plan, 1, sizeof(*plan))
        || overlaps(ranges, range_count, program, 1, sizeof(*program))
        || overlaps(ranges, range_count, ir, 1, sizeof(*ir)))
        return invalid(diagnostics, "linkage arenas alias the borrowed chain");
#define BORROW(pointer, capacity) \
    if (!v_tick(1) \
        || overlaps(ranges, range_count, (pointer), (capacity), \
            sizeof(*(pointer)))) \
        return invalid(diagnostics, "linkage arenas alias borrowed storage")
    BORROW(layout->types, layout->type_capacity);
    BORROW(layout->fields, layout->field_capacity);
    BORROW(layout->variants, layout->variant_capacity);
    BORROW(layout->projections, layout->projection_capacity);
    BORROW(representation->recipes, representation->recipe_capacity);
    BORROW(representation->fields, representation->field_capacity);
    BORROW(representation->variants, representation->variant_capacity);
    BORROW(representation->recipe_ids, representation->recipe_id_capacity);
    BORROW(representation->accesses, representation->access_capacity);
    BORROW(representation->receiver_roots,
        representation->receiver_root_capacity);
    BORROW(representation->callable_producers,
        representation->callable_producer_capacity);
    BORROW(materialization->images, materialization->image_capacity);
    BORROW(materialization->types, materialization->type_capacity);
    BORROW(materialization->shape_fields,
        materialization->shape_field_capacity);
    BORROW(materialization->shape_variants,
        materialization->shape_variant_capacity);
    BORROW(materialization->type_ids, materialization->type_id_capacity);
    BORROW(materialization->accesses, materialization->access_capacity);
    BORROW(materialization->overlays, materialization->overlay_capacity);
    BORROW(materialization->contexts, materialization->context_capacity);
    BORROW(materialization->locals, materialization->local_capacity);
    BORROW(materialization->places, materialization->place_capacity);
    BORROW(materialization->projections, materialization->projection_capacity);
    BORROW(materialization->values, materialization->value_capacity);
    BORROW(materialization->instructions,
        materialization->instruction_capacity);
    BORROW(materialization->temporaries,
        materialization->temporary_capacity);
    BORROW(materialization->construct_operands,
        materialization->construct_operand_capacity);
    BORROW(materialization->call_arguments,
        materialization->call_argument_capacity);
    BORROW(materialization->blocks, materialization->block_capacity);
    BORROW(materialization->edges, materialization->edge_capacity);
    BORROW(materialization->edge_values,
        materialization->edge_value_capacity);
    BORROW(materialization->parameter_values,
        materialization->parameter_value_capacity);
    BORROW(materialization->loops, materialization->loop_capacity);
    BORROW(materialization->bindings, materialization->binding_capacity);
    BORROW(materialization->semantic_sites,
        materialization->semantic_site_capacity);
    BORROW(materialization->receiver_roots,
        materialization->receiver_root_capacity);
    BORROW(materialization->imports, materialization->import_capacity);
    BORROW(materialization->handlers, materialization->handler_capacity);
    BORROW(materialization->writebacks, materialization->writeback_capacity);
    BORROW(materialization->effect_rows, materialization->effect_row_capacity);
    BORROW(materialization->effect_atoms, materialization->effect_atom_capacity);
    BORROW(materialization->effect_row_atoms,
        materialization->effect_row_atom_capacity);
    BORROW(materialization->effect_names,
        materialization->effect_name_capacity);
    BORROW(materialization->literal_bytes,
        materialization->literal_byte_capacity);
    BORROW(plan->types, plan->type_capacity);
    BORROW(plan->type_components, plan->type_component_capacity);
    BORROW(plan->type_parameter_accesses, plan->type_parameter_access_capacity);
    BORROW(plan->effect_atoms, plan->effect_atom_capacity);
    BORROW(plan->effect_rows, plan->effect_row_capacity);
    BORROW(plan->effect_row_atoms, plan->effect_row_atom_capacity);
    BORROW(plan->instances, plan->instance_capacity);
    BORROW(plan->instance_type_ids, plan->instance_type_id_capacity);
    BORROW(plan->instance_accesses, plan->instance_access_capacity);
    BORROW(plan->dictionary_entries, plan->dictionary_entry_capacity);
    BORROW(plan->imports, plan->import_capacity);
    BORROW(plan->typed_uses, plan->typed_use_capacity);
    BORROW(plan->contexts, plan->context_capacity);
    BORROW(plan->demands, plan->demand_capacity);
    BORROW(program->roots, program->root_count);
    BORROW(program->approved_imports, program->approved_import_count);
    BORROW(program->templates, program->template_count);
    BORROW(program->imports, program->import_count);
    BORROW(program->specializations, program->specialization_count);
    BORROW(program->references, program->reference_count);
#define MIR_BORROW(mir, member, capacity) \
    BORROW((mir)->member, (mir)->capacity)
    if (!v_tick(program->template_count))
        return invalid(diagnostics, "linkage validation work limit exceeded");
    for (size_t i = 0; i < program->template_count; ++i) {
        const SolMir *mir = &program->templates[i].mir;
        if (overlaps(ranges, range_count, mir, 1, sizeof(*mir)))
            return invalid(diagnostics, "linkage arenas alias borrowed MIR");
        MIR_BORROW(mir, blocks, block_capacity);
        MIR_BORROW(mir, instructions, instruction_capacity);
        MIR_BORROW(mir, values, value_capacity);
        MIR_BORROW(mir, parameter_values, parameter_value_capacity);
        MIR_BORROW(mir, edge_values, edge_value_capacity);
        MIR_BORROW(mir, call_arguments, call_argument_capacity);
        MIR_BORROW(mir, loops, loop_capacity);
        MIR_BORROW(mir, construct_operands, construct_operand_capacity);
        MIR_BORROW(mir, temporaries, temporary_capacity);
    }
    if (!v_tick(materialization->image_count))
        return invalid(diagnostics, "linkage validation work limit exceeded");
    for (size_t i = 0; i < materialization->image_count; ++i) {
        const SolMir *mir = &materialization->images[i].topology;
        if (overlaps(ranges, range_count, mir, 1, sizeof(*mir)))
            return invalid(diagnostics, "linkage arenas alias borrowed image MIR");
        MIR_BORROW(mir, blocks, block_capacity);
        MIR_BORROW(mir, instructions, instruction_capacity);
        MIR_BORROW(mir, values, value_capacity);
        MIR_BORROW(mir, parameter_values, parameter_value_capacity);
        MIR_BORROW(mir, edge_values, edge_value_capacity);
        MIR_BORROW(mir, call_arguments, call_argument_capacity);
        MIR_BORROW(mir, loops, loop_capacity);
        MIR_BORROW(mir, construct_operands, construct_operand_capacity);
        MIR_BORROW(mir, temporaries, temporary_capacity);
    }
#undef MIR_BORROW
    BORROW(ir->definitions, ir->definition_count);
    BORROW(ir->callables, ir->callable_count);
    BORROW(ir->types, ir->type_count);
    BORROW(ir->type_ids, ir->type_id_count);
    BORROW(ir->accesses, ir->access_count);
    BORROW(ir->members, ir->member_count);
    BORROW(ir->evidence, ir->evidence_count);
    BORROW(ir->locals, ir->local_count);
    BORROW(ir->fields, ir->field_count);
    BORROW(ir->variants, ir->variant_count);
    BORROW(ir->expressions, ir->expression_count);
    BORROW(ir->places, ir->place_count);
    BORROW(ir->projections, ir->projection_count);
    BORROW(ir->statements, ir->statement_count);
    BORROW(ir->statement_ids, ir->statement_id_count);
    BORROW(ir->arms, ir->arm_count);
    BORROW(ir->arm_ids, ir->arm_id_count);
    BORROW(ir->patterns, ir->pattern_count);
    BORROW(ir->pattern_children, ir->pattern_child_count);
    BORROW(ir->operands, ir->operand_count);
    BORROW(ir->roots, ir->root_count);
    BORROW(ir->obligations, ir->obligation_count);
    BORROW(ir->snapshots, ir->snapshot_count);
    BORROW(ir->cleanup_locals, ir->cleanup_local_count);
    BORROW(ir->effects, ir->effect_count);
    BORROW(ir->generic_parameters, ir->generic_parameter_count);
    BORROW(ir->effect_parameters, ir->effect_parameter_count);
    BORROW(ir->loop_obligations, ir->loop_obligation_count);
    BORROW(ir->unreachable_obligations, ir->unreachable_obligation_count);
    BORROW(ir->files, ir->file_count);
    size_t text_size;
    if (!v_text_size(ir->source_path, &text_size))
        return invalid(diagnostics, "linkage validation work limit exceeded");
    BORROW(ir->source_path, text_size);
    BORROW(ir->source_bytes, ir->source_length + 1);
    if (!v_tick(plan->effect_atom_count))
        return invalid(diagnostics, "linkage validation work limit exceeded");
    for (size_t i = 0; i < plan->effect_atom_count; ++i) {
        if (!v_tick(plan->effect_atoms[i].length + 1))
            return invalid(diagnostics, "linkage validation work limit exceeded");
        BORROW(plan->effect_atoms[i].name, plan->effect_atoms[i].length + 1);
    }
    if (!v_tick(ir->definition_count) || !v_tick(ir->callable_count)
        || !v_tick(ir->local_count) || !v_tick(ir->field_count)
        || !v_tick(ir->variant_count) || !v_tick(ir->effect_count)
        || !v_tick(ir->expression_count) || !v_tick(ir->statement_count)
        || !v_tick(ir->generic_parameter_count)
        || !v_tick(ir->effect_parameter_count) || !v_tick(ir->file_count))
        return invalid(diagnostics, "linkage validation work limit exceeded");
    for (size_t i = 0; i < ir->definition_count; ++i)
        if (ir->definitions[i].name != NULL) {
            if (!v_text_size(ir->definitions[i].name, &text_size)) return false;
            BORROW(ir->definitions[i].name, text_size);
        }
    for (size_t i = 0; i < ir->callable_count; ++i)
        if (ir->callables[i].name != NULL) {
            if (!v_text_size(ir->callables[i].name, &text_size)) return false;
            BORROW(ir->callables[i].name, text_size);
        }
    for (size_t i = 0; i < ir->local_count; ++i)
        if (ir->locals[i].name != NULL) {
            if (!v_text_size(ir->locals[i].name, &text_size)) return false;
            BORROW(ir->locals[i].name, text_size);
        }
    for (size_t i = 0; i < ir->field_count; ++i)
        if (ir->fields[i].name != NULL) {
            if (!v_text_size(ir->fields[i].name, &text_size)) return false;
            BORROW(ir->fields[i].name, text_size);
        }
    for (size_t i = 0; i < ir->variant_count; ++i)
        if (ir->variants[i].name != NULL) {
            if (!v_text_size(ir->variants[i].name, &text_size)) return false;
            BORROW(ir->variants[i].name, text_size);
        }
    for (size_t i = 0; i < ir->effect_count; ++i) {
        if (!v_text_size(ir->effects[i].name, &text_size)) return false;
        BORROW(ir->effects[i].name, text_size);
    }
    for (size_t i = 0; i < ir->expression_count; ++i) {
        const SolIrExpression *expression = &ir->expressions[i];
        if (expression->kind == SOL_IR_EXPR_STRING) {
            if (!v_text_size(expression->as.string, &text_size)) return false;
            BORROW(expression->as.string, text_size);
        } else if (expression->kind == SOL_IR_EXPR_HANDLE) {
            if (!v_text_size(expression->as.handler.effect_name, &text_size))
                return false;
            BORROW(expression->as.handler.effect_name, text_size);
        }
    }
    for (size_t i = 0; i < ir->statement_count; ++i)
        if (ir->statements[i].region_label != NULL)
            {
                if (!v_text_size(ir->statements[i].region_label, &text_size))
                    return false;
                BORROW(ir->statements[i].region_label, text_size);
            }
    for (size_t i = 0; i < ir->generic_parameter_count; ++i) {
        if (!v_text_size(ir->generic_parameters[i].name, &text_size)) return false;
        BORROW(ir->generic_parameters[i].name, text_size);
    }
    for (size_t i = 0; i < ir->effect_parameter_count; ++i) {
        if (!v_text_size(ir->effect_parameters[i].name, &text_size)) return false;
        BORROW(ir->effect_parameters[i].name, text_size);
    }
    for (size_t i = 0; i < ir->file_count; ++i) {
        if (!v_text_size(ir->files[i].path, &text_size)) return false;
        BORROW(ir->files[i].path, text_size);
    }
#undef BORROW
    SolMirLinkageUsage expected;
    if (!sol_mir_linkage_internal_expected_usage(linkage, &expected)
        || !usage_equal(expected, linkage->usage)
        || expected.owned_bytes > linkage->limits.max_owned_bytes
        || expected.build_scratch_bytes > linkage->limits.max_build_scratch_bytes
        || expected.validation_scratch_bytes
            > linkage->limits.max_validation_scratch_bytes
        || expected.validation_work > linkage->limits.max_validation_work)
        return invalid(diagnostics, "linkage resource accounting mismatch");
    size_t local_scratch;
    if (!sol_mir_linkage_internal_validation_scratch(linkage, &local_scratch))
        return invalid(diagnostics, "linkage validation scratch overflows");
    size_t required_scratch = linkage->operations->usage.validation_scratch_bytes
            > local_scratch
        ? linkage->operations->usage.validation_scratch_bytes : local_scratch;
    if (authenticate_resources
        && required_scratch != linkage->usage.validation_scratch_bytes)
        return invalid(diagnostics, "linkage validation scratch is malformed");
    unsigned char *scratch = local_scratch == 0 ? NULL : malloc(local_scratch);
    if (local_scratch != 0 && scratch == NULL) {
        if (diagnostics != NULL) diagnostics->allocation_failed = true;
        return invalid(diagnostics, "linkage validation scratch allocation failed");
    }
    unsigned char empty_scratch = 0;
    unsigned char *base = scratch == NULL ? &empty_scratch : scratch;
    size_t *callable_map = (size_t *)(void *)base;
    size_t callable_bytes = materialization->image_count * sizeof(size_t);
    size_t *host_map = (size_t *)(void *)(base + callable_bytes);
    size_t host_bytes = materialization->import_count * sizeof(size_t);
    unsigned char *host_referenced = scratch + callable_bytes + host_bytes;
    unsigned char *table_used
        = host_referenced + materialization->import_count;
    unsigned char *runtime_seen = table_used + linkage->table_entry_count;
    if (!v_tick(callable_bytes) || !v_tick(host_bytes)
        || !v_tick(materialization->import_count)
        || !v_tick(linkage->table_entry_count)
        || !v_tick(representation->recipe_count)) {
        free(scratch);
        return invalid(diagnostics, "linkage validation work limit exceeded");
    }
    memset(callable_map, 0xff, callable_bytes);
    memset(host_map, 0xff, host_bytes);
    memset(host_referenced, 0, materialization->import_count);
    memset(table_used, 0, linkage->table_entry_count);
    memset(runtime_seen, 0, representation->recipe_count);
    bool references_valid = true;
    if (!v_tick(materialization->binding_count)) references_valid = false;
    for (size_t i = 0; references_valid
        && i < materialization->binding_count; ++i)
        if (materialization->bindings[i].target_kind
                == SOL_MIR_MATERIALIZED_TARGET_IMPORT) {
            if (materialization->bindings[i].import
                    >= materialization->import_count) {
                references_valid = false; break;
            }
            host_referenced[materialization->bindings[i].import] = 1;
        }
    if (references_valid && !v_tick(linkage->operations->callable_count))
        references_valid = false;
    for (size_t i = 0; references_valid
        && i < linkage->operations->callable_count; ++i)
        if (linkage->operations->callables[i].target_kind
                == SOL_MIR_MATERIALIZED_TARGET_IMPORT) {
            if (linkage->operations->callables[i].target_import
                    >= materialization->import_count) {
                references_valid = false; break;
            }
            host_referenced[linkage->operations->callables[i].target_import] = 1;
        }
    bool valid = references_valid
        && validate_callables(linkage, materialization, ir, callable_map)
        && validate_hosts(linkage, materialization, program, ir, host_map,
            host_referenced)
        && validate_bindings(linkage, materialization, callable_map, host_map)
        && validate_exports(linkage, materialization, program)
        && validate_tables(linkage, callable_map, host_map, table_used)
        && validate_runtime(linkage, runtime_seen);
    size_t replay_work = 0, replay_bytes = 0;
    if (valid && (!replay_scratch_bytes(linkage, &replay_bytes)
            || replay_bytes > local_scratch
            || !replay_build_work(linkage, base, &replay_work)
            || replay_work != linkage->usage.build_work
            || replay_work > linkage->limits.max_build_work))
        valid = false;
    free(scratch);
    if (!valid)
        return invalid(diagnostics, "linkage identity or closure invariant failed");
    if (authenticate_resources
        && linkage->usage.validation_work != metered_work)
        return invalid(diagnostics, "linkage validation work is malformed");
    if (measured_work != NULL) *measured_work = metered_work;
    if (measured_scratch != NULL) *measured_scratch = required_scratch;
    return true;
}

bool sol_mir_linkage_internal_validation_requirements(
    const SolMirLinkage *linkage, size_t *work, size_t *scratch) {
    return work != NULL && scratch != NULL
        && validate(linkage, NULL, false, work, scratch);
}

bool sol_mir_linkage_validate(const SolMirLinkage *linkage,
    SolDiagnostics *diagnostics) {
    return validate(linkage, diagnostics, true, NULL, NULL);
}
