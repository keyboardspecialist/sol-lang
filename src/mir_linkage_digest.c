#include "mir_linkage_internal.h"

#include <string.h>

typedef SolMirLinkageSha256 Sha256;

bool sol_mir_linkage_internal_work_tick(SolMirLinkageWorkMeter *meter,
    size_t amount) {
    if (meter == NULL) return true;
    if (amount > SIZE_MAX - meter->used
        || meter->used + amount > meter->limit) {
        meter->exhausted = true;
        return false;
    }
    meter->used += amount;
    return true;
}

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotate_right(uint32_t value, unsigned amount) {
    return (value >> amount) | (value << (32u - amount));
}

static void sha256_transform(Sha256 *sha, const uint8_t block[64]) {
    if (!sol_mir_linkage_internal_work_tick(sha->work, 128)) {
        sha->valid = false; return;
    }
    uint32_t words[64];
    for (size_t i = 0; i < 16; ++i) {
        words[i] = (uint32_t)block[i * 4] << 24
            | (uint32_t)block[i * 4 + 1] << 16
            | (uint32_t)block[i * 4 + 2] << 8
            | (uint32_t)block[i * 4 + 3];
    }
    for (size_t i = 16; i < 64; ++i) {
        uint32_t a = words[i - 15], b = words[i - 2];
        uint32_t s0 = rotate_right(a, 7) ^ rotate_right(a, 18) ^ (a >> 3);
        uint32_t s1 = rotate_right(b, 17) ^ rotate_right(b, 19) ^ (b >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    uint32_t a = sha->state[0], b = sha->state[1], c = sha->state[2];
    uint32_t d = sha->state[3], e = sha->state[4], f = sha->state[5];
    uint32_t g = sha->state[6], h = sha->state[7];
    for (size_t i = 0; i < 64; ++i) {
        uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11)
            ^ rotate_right(e, 25);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + choice + sha256_k[i] + words[i];
        uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13)
            ^ rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + majority;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    sha->state[0] += a; sha->state[1] += b; sha->state[2] += c;
    sha->state[3] += d; sha->state[4] += e; sha->state[5] += f;
    sha->state[6] += g; sha->state[7] += h;
}

static void sha256_init(Sha256 *sha) {
    *sha = (Sha256){
        .state = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u},
        .valid = true,
    };
}

static void sha256_write(Sha256 *sha, const void *data, size_t length) {
    if (!sha->valid || length > (UINT64_MAX - sha->bits) / 8u
        || !sol_mir_linkage_internal_work_tick(sha->work, length)) {
        sha->valid = false; return;
    }
    sha->bits += (uint64_t)length * 8u;
    const uint8_t *bytes = data;
    while (length != 0) {
        size_t available = sizeof(sha->block) - sha->used;
        size_t take = length < available ? length : available;
        memcpy(sha->block + sha->used, bytes, take);
        sha->used += take; bytes += take; length -= take;
        if (sha->used == sizeof(sha->block)) {
            sha256_transform(sha, sha->block);
            sha->used = 0;
            if (!sha->valid) return;
        }
    }
}

static bool sha256_finish(Sha256 *sha, SolMirLinkageDigest *digest) {
    if (!sha->valid || !sol_mir_linkage_internal_work_tick(sha->work,
            SOL_MIR_LINKAGE_DIGEST_BYTES))
        return false;
    uint64_t bits = sha->bits;
    uint8_t marker = 0x80; sha256_write(sha, &marker, 1);
    if (!sha->valid) return false;
    uint8_t zero = 0;
    while (sha->used != 56) {
        sha256_write(sha, &zero, 1);
        if (!sha->valid) return false;
    }
    uint8_t length[8];
    for (size_t i = 0; i < 8; ++i)
        length[7 - i] = (uint8_t)(bits >> (i * 8));
    sha256_write(sha, length, sizeof(length));
    if (!sha->valid || sha->used != 0) return false;
    for (size_t i = 0; i < 8; ++i) {
        digest->bytes[i * 4] = (uint8_t)(sha->state[i] >> 24);
        digest->bytes[i * 4 + 1] = (uint8_t)(sha->state[i] >> 16);
        digest->bytes[i * 4 + 2] = (uint8_t)(sha->state[i] >> 8);
        digest->bytes[i * 4 + 3] = (uint8_t)sha->state[i];
    }
    return true;
}

void sol_mir_linkage_internal_sha256_init(SolMirLinkageSha256 *sha) {
    sha256_init(sha);
}

void sol_mir_linkage_internal_sha256_write(SolMirLinkageSha256 *sha,
    const void *bytes_value, size_t length) {
    sha256_write(sha, bytes_value, length);
}

bool sol_mir_linkage_internal_sha256_finish(SolMirLinkageSha256 *sha,
    SolMirLinkageDigest *digest) {
    return sha256_finish(sha, digest);
}

enum {
    TOKEN_U64 = 1, TOKEN_BYTES, TOKEN_SEMANTIC_ID, TOKEN_SEQUENCE,
    TOKEN_ABSENT, TOKEN_PRESENT, TOKEN_TYPE, TOKEN_EFFECT, TOKEN_CALLABLE,
    TOKEN_DICTIONARY, TOKEN_INSTANCE, TOKEN_SIGNATURE, TOKEN_TABLE,
};

static void token(Sha256 *sha, uint8_t value) { sha256_write(sha, &value, 1); }

static void u64(Sha256 *sha, uint64_t value) {
    uint8_t bytes[8]; token(sha, TOKEN_U64);
    for (size_t i = 0; i < 8; ++i) bytes[7 - i] = (uint8_t)(value >> (i * 8));
    sha256_write(sha, bytes, sizeof(bytes));
}

static void bytes(Sha256 *sha, const char *value, size_t length) {
    token(sha, TOKEN_BYTES); u64(sha, (uint64_t)length);
    sha256_write(sha, value, length);
}

static void label(Sha256 *sha, const char *value) {
    bytes(sha, value, strlen(value));
}

static void sequence(Sha256 *sha, size_t count) {
    token(sha, TOKEN_SEQUENCE); u64(sha, (uint64_t)count);
}

static void semantic_id(Sha256 *sha, SolSemanticId id) {
    token(sha, TOKEN_SEMANTIC_ID); u64(sha, id.high); u64(sha, id.low);
}

static bool text_length(const char *text, SolMirLinkageWorkMeter *work,
    size_t *length) {
    size_t result = 0;
    for (;;) {
        if (!sol_mir_linkage_internal_work_tick(work, 1)) return false;
        if (text[result] == '\0') break;
        if (result == SIZE_MAX) return false;
        ++result;
    }
    *length = result; return true;
}

static const char *type_label(SolIrTypeKind kind) {
    static const char *const labels[] = {
        "int64", "bool", "text", "unit", "never", "nominal", "option",
        "result", "tuple", "function", "parameter", "self",
    };
    return (size_t)kind < sizeof(labels) / sizeof(labels[0])
        ? labels[kind] : NULL;
}

static const char *access_label(SolAccessMode access) {
    static const char *const labels[] = {"owned", "shared", "exclusive"};
    return (size_t)access < sizeof(labels) / sizeof(labels[0])
        ? labels[access] : NULL;
}

static const char *callable_label(SolIrCallableKind kind) {
    static const char *const labels[] = {"function", "capability",
        "trait-requirement", "trait-implementation", "test"};
    return (size_t)kind < sizeof(labels) / sizeof(labels[0])
        ? labels[kind] : NULL;
}

static const char *authority_label(SolMirPlanEffectAuthority authority) {
    static const char *const labels[] = {"none", "receiver", "parameter", "tail"};
    return (size_t)authority < sizeof(labels) / sizeof(labels[0])
        ? labels[authority] : NULL;
}

static bool hash_callable(Sha256 *sha, const SolIr *ir,
    SolIrCallableId callable_id) {
    if (callable_id >= ir->callable_count) return false;
    const SolIrCallable *callable = &ir->callables[callable_id];
    if (callable->owner >= ir->definition_count || callable->name == NULL)
        return false;
    const char *kind = callable_label(callable->kind);
    if (kind == NULL) return false;
    const SolIrDefinition *owner = &ir->definitions[callable->owner];
    token(sha, TOKEN_CALLABLE); semantic_id(sha, owner->semantic_id); label(sha, kind);
    if (owner->callable == callable_id) label(sha, "owner-callable");
    else {
        size_t length;
        if (!text_length(callable->name, sha->work, &length)) return false;
        label(sha, "member"); bytes(sha, callable->name, length);
    }
    return true;
}

static bool hash_effect(Sha256 *sha, const SolMirPlan *plan,
    SolMirPlanEffectRowId row_id) {
    if (row_id >= plan->effect_row_count) return false;
    const SolMirPlanEffectRow *row = &plan->effect_rows[row_id];
    if (row->atom_offset > plan->effect_row_atom_count
        || row->atom_count > plan->effect_row_atom_count - row->atom_offset)
        return false;
    token(sha, TOKEN_EFFECT); sequence(sha, row->atom_count);
    for (size_t i = 0; i < row->atom_count; ++i) {
        size_t atom_id = plan->effect_row_atoms[row->atom_offset + i];
        if (atom_id >= plan->effect_atom_count) return false;
        const SolMirPlanEffectAtom *atom = &plan->effect_atoms[atom_id];
        const char *authority = authority_label(atom->authority);
        if (atom->name == NULL || authority == NULL) return false;
        bytes(sha, atom->name, atom->length); label(sha, authority);
        if (atom->authority == SOL_MIR_PLAN_EFFECT_AUTHORITY_PARAMETER)
            u64(sha, (uint64_t)atom->ordinal);
    }
    return true;
}

static bool hash_type(Sha256 *sha, const SolMirPlan *plan,
    SolMirPlanTypeId type_id, size_t depth) {
    if (type_id >= plan->type_count || depth > plan->type_count) return false;
    const SolMirPlanType *type = &plan->types[type_id];
    const SolIr *ir = plan->program->ir;
    const char *kind = type_label(type->kind);
    if (kind == NULL || type->kind == SOL_IR_TYPE_PARAMETER
        || type->kind == SOL_IR_TYPE_SELF) return false;
    token(sha, TOKEN_TYPE); label(sha, kind);
    if (type->definition != SOL_IR_NONE) {
        if (type->definition >= ir->definition_count) return false;
        token(sha, TOKEN_PRESENT);
        semantic_id(sha, ir->definitions[type->definition].semantic_id);
    } else token(sha, TOKEN_ABSENT);
    if (type->argument_offset > plan->type_component_count
        || type->argument_count > plan->type_component_count - type->argument_offset)
        return false;
    sequence(sha, type->argument_count);
    for (size_t i = 0; i < type->argument_count; ++i)
        if (!hash_type(sha, plan,
                plan->type_components[type->argument_offset + i], depth + 1))
            return false;
    if (type->kind == SOL_IR_TYPE_NOMINAL) return true;
    if (type->parameter_offset > plan->type_component_count
        || type->parameter_count > plan->type_component_count - type->parameter_offset
        || type->parameter_access_offset > plan->type_parameter_access_count
        || type->parameter_count > plan->type_parameter_access_count
            - type->parameter_access_offset) return false;
    sequence(sha, type->parameter_count);
    for (size_t i = 0; i < type->parameter_count; ++i) {
        const char *access = access_label(plan->type_parameter_accesses[
            type->parameter_access_offset + i]);
        if (access == NULL) return false;
        label(sha, access);
        if (!hash_type(sha, plan,
                plan->type_components[type->parameter_offset + i], depth + 1))
            return false;
    }
    if (type->result == SOL_MIR_PLAN_NONE) token(sha, TOKEN_ABSENT);
    else {
        token(sha, TOKEN_PRESENT);
        if (!hash_type(sha, plan, type->result, depth + 1)) return false;
    }
    if (type->effects == SOL_MIR_PLAN_NONE) token(sha, TOKEN_ABSENT);
    else {
        token(sha, TOKEN_PRESENT);
        if (!hash_effect(sha, plan, type->effects)) return false;
    }
    return true;
}

static int compare_size(size_t left, size_t right) {
    return left < right ? -1 : left > right;
}

static int compare_u64(uint64_t left, uint64_t right) {
    return left < right ? -1 : left > right;
}

static int compare_semantic(SolSemanticId left, SolSemanticId right) {
    int high = compare_u64(left.high, right.high);
    return high != 0 ? high : compare_u64(left.low, right.low);
}

static int compare_text(const char *left, size_t left_length,
    const char *right, size_t right_length, SolMirLinkageWorkMeter *work) {
    size_t shared = left_length < right_length ? left_length : right_length;
    if (!sol_mir_linkage_internal_work_tick(work, shared)) return 0;
    int order = memcmp(left, right, shared);
    return order != 0 ? order : compare_size(left_length, right_length);
}

static int compare_callable_descriptor(const SolIr *ir,
    SolIrCallableId left_id, SolIrCallableId right_id, bool *valid,
    SolMirLinkageWorkMeter *work) {
    if (!sol_mir_linkage_internal_work_tick(work, 1)) {
        *valid = false; return 0;
    }
    if (left_id >= ir->callable_count || right_id >= ir->callable_count) {
        *valid = false; return 0;
    }
    const SolIrCallable *left = &ir->callables[left_id];
    const SolIrCallable *right = &ir->callables[right_id];
    if (left->owner >= ir->definition_count || right->owner >= ir->definition_count
        || left->name == NULL || right->name == NULL
        || callable_label(left->kind) == NULL
        || callable_label(right->kind) == NULL) {
        *valid = false; return 0;
    }
    int order = compare_semantic(ir->definitions[left->owner].semantic_id,
        ir->definitions[right->owner].semantic_id);
    if (order != 0) return order;
    const char *left_kind = callable_label(left->kind);
    const char *right_kind = callable_label(right->kind);
    order = strcmp(left_kind, right_kind);
    if (order != 0) return order;
    bool left_owner = ir->definitions[left->owner].callable == left_id;
    bool right_owner = ir->definitions[right->owner].callable == right_id;
    if (left_owner != right_owner) return left_owner ? -1 : 1;
    size_t left_length, right_length;
    if (!text_length(left->name, work, &left_length)
        || !text_length(right->name, work, &right_length)) {
        *valid = false; return 0;
    }
    return left_owner ? 0 : compare_text(left->name, left_length,
        right->name, right_length, work);
}

static int compare_effect_descriptor(const SolMirPlan *plan,
    SolMirPlanEffectRowId left_id, SolMirPlanEffectRowId right_id, bool *valid,
    SolMirLinkageWorkMeter *work) {
    if (!sol_mir_linkage_internal_work_tick(work, 1)) {
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
    int order = compare_size(left->atom_count, right->atom_count);
    if (order != 0) return order;
    for (size_t i = 0; i < left->atom_count; ++i) {
        size_t left_atom = plan->effect_row_atoms[left->atom_offset + i];
        size_t right_atom = plan->effect_row_atoms[right->atom_offset + i];
        if (left_atom >= plan->effect_atom_count
            || right_atom >= plan->effect_atom_count) {
            *valid = false; return 0;
        }
        const SolMirPlanEffectAtom *a = &plan->effect_atoms[left_atom];
        const SolMirPlanEffectAtom *b = &plan->effect_atoms[right_atom];
        if (a->name == NULL || b->name == NULL
            || authority_label(a->authority) == NULL
            || authority_label(b->authority) == NULL) {
            *valid = false; return 0;
        }
        order = compare_text(a->name, a->length, b->name, b->length, work);
        if (work != NULL && work->exhausted) { *valid = false; return 0; }
        if (order != 0) return order;
        order = strcmp(authority_label(a->authority),
            authority_label(b->authority));
        if (order != 0) return order;
        if (a->authority == SOL_MIR_PLAN_EFFECT_AUTHORITY_PARAMETER) {
            order = compare_size(a->ordinal, b->ordinal);
            if (order != 0) return order;
        }
    }
    return 0;
}

static int compare_type_descriptor(const SolMirPlan *plan,
    SolMirPlanTypeId left_id, SolMirPlanTypeId right_id, size_t depth,
    bool *valid, SolMirLinkageWorkMeter *work) {
    if (!sol_mir_linkage_internal_work_tick(work, 1)) {
        *valid = false; return 0;
    }
    if (left_id >= plan->type_count || right_id >= plan->type_count
        || depth > plan->type_count) {
        *valid = false; return 0;
    }
    const SolMirPlanType *left = &plan->types[left_id];
    const SolMirPlanType *right = &plan->types[right_id];
    const SolIr *ir = plan->program->ir;
    const char *left_kind = type_label(left->kind);
    const char *right_kind = type_label(right->kind);
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
        order = compare_semantic(ir->definitions[left->definition].semantic_id,
            ir->definitions[right->definition].semantic_id);
        if (order != 0) return order;
    }
    if (left->argument_offset > plan->type_component_count
        || left->argument_count > plan->type_component_count - left->argument_offset
        || right->argument_offset > plan->type_component_count
        || right->argument_count > plan->type_component_count - right->argument_offset) {
        *valid = false; return 0;
    }
    order = compare_size(left->argument_count, right->argument_count);
    if (order != 0) return order;
    for (size_t i = 0; i < left->argument_count; ++i) {
        order = compare_type_descriptor(plan,
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
    order = compare_size(left->parameter_count, right->parameter_count);
    if (order != 0) return order;
    for (size_t i = 0; i < left->parameter_count; ++i) {
        const char *left_access = access_label(plan->type_parameter_accesses[
            left->parameter_access_offset + i]);
        const char *right_access = access_label(plan->type_parameter_accesses[
            right->parameter_access_offset + i]);
        if (left_access == NULL || right_access == NULL) {
            *valid = false; return 0;
        }
        order = strcmp(left_access, right_access);
        if (order != 0) return order;
        order = compare_type_descriptor(plan,
            plan->type_components[left->parameter_offset + i],
            plan->type_components[right->parameter_offset + i], depth + 1,
            valid, work);
        if (!*valid || order != 0) return order;
    }
    bool left_result = left->result != SOL_MIR_PLAN_NONE;
    bool right_result = right->result != SOL_MIR_PLAN_NONE;
    if (left_result != right_result) return left_result ? 1 : -1;
    if (left_result) {
        order = compare_type_descriptor(plan, left->result, right->result,
            depth + 1, valid, work);
        if (!*valid || order != 0) return order;
    }
    bool left_effects = left->effects != SOL_MIR_PLAN_NONE;
    bool right_effects = right->effects != SOL_MIR_PLAN_NONE;
    if (left_effects != right_effects) return left_effects ? 1 : -1;
    return left_effects ? compare_effect_descriptor(plan, left->effects,
        right->effects, valid, work) : 0;
}

static int compare_dictionary_entry(const SolMirPlan *plan,
    const SolMirPlanDictionaryEntry *left,
    const SolMirPlanDictionaryEntry *right, bool *valid,
    SolMirLinkageWorkMeter *work) {
    if (!sol_mir_linkage_internal_work_tick(work, 1)) {
        *valid = false; return 0;
    }
    const SolIr *ir = plan->program->ir;
    if (left->trait >= ir->definition_count
        || right->trait >= ir->definition_count
        || left->implementation >= ir->definition_count
        || right->implementation >= ir->definition_count) {
        *valid = false; return 0;
    }
    int order = compare_size(left->generic_ordinal, right->generic_ordinal);
    if (order != 0) return order;
    order = compare_semantic(ir->definitions[left->trait].semantic_id,
        ir->definitions[right->trait].semantic_id);
    if (order != 0) return order;
    order = compare_callable_descriptor(ir, left->requirement,
        right->requirement, valid, work);
    if (!*valid || order != 0) return order;
    order = compare_type_descriptor(plan, left->type, right->type, 0, valid,
        work);
    if (!*valid || order != 0) return order;
    order = compare_semantic(ir->definitions[left->implementation].semantic_id,
        ir->definitions[right->implementation].semantic_id);
    if (order != 0) return order;
    return compare_callable_descriptor(ir, left->method, right->method, valid,
        work);
}

static bool hash_dictionary(Sha256 *sha, const SolMirPlan *plan,
    SolMirPlanSlice dictionary) {
    if (dictionary.offset > plan->dictionary_entry_count
        || dictionary.count > plan->dictionary_entry_count - dictionary.offset)
        return false;
    token(sha, TOKEN_DICTIONARY); sequence(sha, dictionary.count);
    const SolIr *ir = plan->program->ir;
    size_t previous = SOL_MIR_PLAN_NONE;
    for (size_t i = 0; i < dictionary.count; ++i) {
        size_t selected = SOL_MIR_PLAN_NONE;
        for (size_t candidate = 0; candidate < dictionary.count; ++candidate) {
            bool valid = true;
            const SolMirPlanDictionaryEntry *entry
                = &plan->dictionary_entries[dictionary.offset + candidate];
            if (previous != SOL_MIR_PLAN_NONE
                && compare_dictionary_entry(plan,
                    &plan->dictionary_entries[dictionary.offset + previous],
                    entry, &valid, sha->work) >= 0) {
                if (!valid) return false;
                continue;
            }
            if (selected == SOL_MIR_PLAN_NONE
                || compare_dictionary_entry(plan, entry,
                    &plan->dictionary_entries[dictionary.offset + selected],
                    &valid, sha->work) < 0) selected = candidate;
            if (!valid) return false;
        }
        if (selected == SOL_MIR_PLAN_NONE) return false;
        const SolMirPlanDictionaryEntry *entry
            = &plan->dictionary_entries[dictionary.offset + selected];
        if (entry->trait >= ir->definition_count
            || entry->implementation >= ir->definition_count) return false;
        u64(sha, (uint64_t)entry->generic_ordinal);
        semantic_id(sha, ir->definitions[entry->trait].semantic_id);
        if (!hash_callable(sha, ir, entry->requirement)
            || !hash_type(sha, plan, entry->type, 0)) return false;
        semantic_id(sha, ir->definitions[entry->implementation].semantic_id);
        if (!hash_callable(sha, ir, entry->method)) return false;
        previous = selected;
    }
    return true;
}

static bool start_domain(Sha256 *sha, const char *domain,
    SolMirLinkageWorkMeter *work) {
    sha256_init(sha); sha->work = work;
    bytes(sha, domain, strlen(domain)); return sha->valid;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
static _Thread_local bool force_instance_collision;
static _Thread_local bool force_host_collision;
static _Thread_local bool force_runtime_collision;
static _Thread_local bool force_table_collision;
void sol_mir_linkage_test_force_instance_digest_collision(bool force) {
    force_instance_collision = force;
}

void sol_mir_linkage_test_force_host_digest_collision(bool force) {
    force_host_collision = force;
}

void sol_mir_linkage_test_force_runtime_digest_collision(bool force) {
    force_runtime_collision = force;
}

void sol_mir_linkage_test_force_table_digest_collision(bool force) {
    force_table_collision = force;
}

bool sol_mir_linkage_test_sha256(const void *value, size_t length,
    SolMirLinkageDigest *digest) {
    if ((value == NULL && length != 0) || digest == NULL) return false;
    Sha256 sha;
    sha256_init(&sha);
    sha256_write(&sha, value, length);
    return sha256_finish(&sha, digest);
}

bool sol_mir_linkage_test_sha256_with_limit(const void *value, size_t length,
    size_t limit, SolMirLinkageDigest *digest, size_t *used) {
    if ((value == NULL && length != 0) || digest == NULL || used == NULL)
        return false;
    SolMirLinkageWorkMeter work = {.limit = limit};
    Sha256 sha;
    sha256_init(&sha); sha.work = &work;
    sha256_write(&sha, value, length);
    bool result = sha256_finish(&sha, digest);
    *used = work.used;
    return result;
}
#endif

bool sol_mir_linkage_internal_instance_key(const SolMirLinkage *linkage,
    SolMirPlanInstanceId instance_id, SolMirLinkageDigest *digest,
    SolMirLinkageWorkMeter *work) {
    if (linkage == NULL || linkage->operations == NULL || digest == NULL) return false;
    const SolMirPlan *plan = linkage->operations->layout->representation
        ->materialization->plan;
    if (instance_id >= plan->instance_count) return false;
    const SolMirPlanInstance *instance = &plan->instances[instance_id];
    Sha256 sha; start_domain(&sha, "sol.mir.instance-key/1", work);
    token(&sha, TOKEN_INSTANCE);
    if (!hash_callable(&sha, plan->program->ir, instance->callable)) return false;
    if (instance->receiver == SOL_MIR_PLAN_NONE) token(&sha, TOKEN_ABSENT);
    else {
        token(&sha, TOKEN_PRESENT);
        if (!hash_type(&sha, plan, instance->receiver, 0)) return false;
    }
    if (instance->type_arguments.offset > plan->instance_type_id_count
        || instance->type_arguments.count > plan->instance_type_id_count
            - instance->type_arguments.offset) return false;
    sequence(&sha, instance->type_arguments.count);
    for (size_t i = 0; i < instance->type_arguments.count; ++i)
        if (!hash_type(&sha, plan, plan->instance_type_ids[
                instance->type_arguments.offset + i], 0)) return false;
    if (!hash_dictionary(&sha, plan, instance->dictionary)
        || !hash_effect(&sha, plan, instance->effect_tail)
        || !hash_effect(&sha, plan, instance->effects)
        || !sha256_finish(&sha, digest)) return false;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (force_instance_collision) memset(digest, 0, sizeof(*digest));
#endif
    return true;
}

static bool hash_signature(Sha256 *sha, const SolMirPlan *plan,
    SolIrCallableId callable, SolMirPlanTypeId receiver,
    SolAccessMode receiver_access, SolMirPlanSlice parameters,
    SolMirPlanSlice accesses, SolMirPlanTypeId result,
    SolMirPlanEffectRowId effects) {
    token(sha, TOKEN_SIGNATURE);
    if (!hash_callable(sha, plan->program->ir, callable)) return false;
    if (receiver == SOL_MIR_PLAN_NONE) token(sha, TOKEN_ABSENT);
    else {
        const char *access = access_label(receiver_access);
        if (access == NULL) return false;
        token(sha, TOKEN_PRESENT); label(sha, access);
        if (!hash_type(sha, plan, receiver, 0)) return false;
    }
    if (parameters.offset > plan->instance_type_id_count
        || parameters.count > plan->instance_type_id_count - parameters.offset
        || accesses.offset > plan->instance_access_count
        || accesses.count != parameters.count
        || accesses.count > plan->instance_access_count - accesses.offset)
        return false;
    sequence(sha, parameters.count);
    for (size_t i = 0; i < parameters.count; ++i) {
        const char *access = access_label(plan->instance_accesses[accesses.offset + i]);
        if (access == NULL) return false;
        label(sha, access);
        if (!hash_type(sha, plan,
                plan->instance_type_ids[parameters.offset + i], 0)) return false;
    }
    return hash_type(sha, plan, result, 0) && hash_effect(sha, plan, effects);
}

bool sol_mir_linkage_internal_host_key(const SolMirLinkage *linkage,
    SolMirMaterializedImportId import_id, SolMirLinkageDigest *digest,
    SolMirLinkageWorkMeter *work) {
    if (linkage == NULL || linkage->operations == NULL || digest == NULL) return false;
    const SolMirMaterialization *materialization = linkage->operations->layout
        ->representation->materialization;
    const SolMirPlan *plan = materialization->plan;
    if (import_id >= materialization->import_count) return false;
    const SolMirMaterializedImport *materialized = &materialization->imports[import_id];
    if (materialized->source_import >= plan->import_count) return false;
    const SolMirPlanImport *import = &plan->imports[materialized->source_import];
    Sha256 sha; start_domain(&sha, "sol.mir.host-requirement-key/1", work);
    if (!hash_signature(&sha, plan, import->callable, import->receiver,
            materialized->receiver_access, import->parameter_types,
            import->parameter_accesses, import->result, import->effects)) return false;
    if (!sha256_finish(&sha, digest)) return false;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (force_host_collision) memset(digest, 0, sizeof(*digest));
#endif
    return true;
}

bool sol_mir_linkage_internal_recipe_key(const SolMirLinkage *linkage,
    SolMirRecipeId recipe, SolMirLinkageDigest *digest,
    SolMirLinkageWorkMeter *work) {
    if (linkage == NULL || linkage->operations == NULL || digest == NULL) return false;
    const SolMirPlan *plan = linkage->operations->layout->representation
        ->materialization->plan;
    if (recipe >= plan->type_count) return false;
    Sha256 sha; start_domain(&sha, "sol.mir.runtime-recipe-key/1", work);
    if (!hash_type(&sha, plan, recipe, 0)) return false;
    if (!sha256_finish(&sha, digest)) return false;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (force_runtime_collision) memset(digest, 0, sizeof(*digest));
#endif
    return true;
}

static bool dictionary_equal(const SolMirPlan *plan, SolMirPlanSlice left,
    SolMirPlanSlice right, bool *valid, SolMirLinkageWorkMeter *work) {
    if (left.offset > plan->dictionary_entry_count
        || left.count > plan->dictionary_entry_count - left.offset
        || right.offset > plan->dictionary_entry_count
        || right.count > plan->dictionary_entry_count - right.offset) {
        *valid = false; return false;
    }
    if (left.count != right.count) return false;
    for (size_t i = 0; i < left.count; ++i) {
        bool matched = false;
        for (size_t q = 0; q < right.count; ++q) {
            int order = compare_dictionary_entry(plan,
                &plan->dictionary_entries[left.offset + i],
                &plan->dictionary_entries[right.offset + q], valid, work);
            if (!*valid) return false;
            if (order == 0) { matched = true; break; }
        }
        if (!matched) return false;
    }
    return true;
}

bool sol_mir_linkage_internal_instance_descriptor_equal(
    const SolMirLinkage *linkage, SolMirPlanInstanceId left_id,
    SolMirPlanInstanceId right_id, bool *equal,
    SolMirLinkageWorkMeter *work) {
    if (linkage == NULL || linkage->operations == NULL || equal == NULL)
        return false;
    const SolMirPlan *plan = linkage->operations->layout->representation
        ->materialization->plan;
    if (left_id >= plan->instance_count || right_id >= plan->instance_count)
        return false;
    const SolMirPlanInstance *left = &plan->instances[left_id];
    const SolMirPlanInstance *right = &plan->instances[right_id];
    bool valid = true;
    if (compare_callable_descriptor(plan->program->ir, left->callable,
            right->callable, &valid, work) != 0) {
        *equal = false; return valid;
    }
    bool left_receiver = left->receiver != SOL_MIR_PLAN_NONE;
    bool right_receiver = right->receiver != SOL_MIR_PLAN_NONE;
    if (left_receiver != right_receiver) { *equal = false; return true; }
    if (left_receiver && compare_type_descriptor(plan, left->receiver,
            right->receiver, 0, &valid, work) != 0) {
        *equal = false; return valid;
    }
    if (left->type_arguments.offset > plan->instance_type_id_count
        || left->type_arguments.count > plan->instance_type_id_count
            - left->type_arguments.offset
        || right->type_arguments.offset > plan->instance_type_id_count
        || right->type_arguments.count > plan->instance_type_id_count
            - right->type_arguments.offset) return false;
    if (left->type_arguments.count != right->type_arguments.count) {
        *equal = false; return true;
    }
    for (size_t i = 0; i < left->type_arguments.count; ++i)
        if (compare_type_descriptor(plan, plan->instance_type_ids[
                left->type_arguments.offset + i], plan->instance_type_ids[
                right->type_arguments.offset + i], 0, &valid, work) != 0) {
            *equal = false; return valid;
        }
    if (!dictionary_equal(plan, left->dictionary, right->dictionary, &valid,
            work)) {
        *equal = false; return valid;
    }
    if (compare_effect_descriptor(plan, left->effect_tail, right->effect_tail,
            &valid, work) != 0
        || compare_effect_descriptor(plan, left->effects, right->effects,
            &valid, work) != 0) {
        *equal = false; return valid;
    }
    *equal = true; return valid;
}

static bool signature_equal(const SolMirPlan *plan,
    const SolMirMaterializedImport *left_materialized,
    const SolMirPlanImport *left, const SolMirMaterializedImport *right_materialized,
    const SolMirPlanImport *right, bool *equal, SolMirLinkageWorkMeter *work) {
    bool valid = true;
    if (compare_callable_descriptor(plan->program->ir, left->callable,
            right->callable, &valid, work) != 0) {
        *equal = false; return valid;
    }
    bool left_receiver = left->receiver != SOL_MIR_PLAN_NONE;
    bool right_receiver = right->receiver != SOL_MIR_PLAN_NONE;
    if (left_receiver != right_receiver
        || (left_receiver && left_materialized->receiver_access
            != right_materialized->receiver_access)) {
        *equal = false; return true;
    }
    if (left_receiver && compare_type_descriptor(plan, left->receiver,
            right->receiver, 0, &valid, work) != 0) {
        *equal = false; return valid;
    }
    if (left->parameter_types.offset > plan->instance_type_id_count
        || left->parameter_types.count > plan->instance_type_id_count
            - left->parameter_types.offset
        || right->parameter_types.offset > plan->instance_type_id_count
        || right->parameter_types.count > plan->instance_type_id_count
            - right->parameter_types.offset
        || left->parameter_accesses.offset > plan->instance_access_count
        || left->parameter_accesses.count != left->parameter_types.count
        || left->parameter_accesses.count > plan->instance_access_count
            - left->parameter_accesses.offset
        || right->parameter_accesses.offset > plan->instance_access_count
        || right->parameter_accesses.count != right->parameter_types.count
        || right->parameter_accesses.count > plan->instance_access_count
            - right->parameter_accesses.offset) return false;
    if (left->parameter_types.count != right->parameter_types.count) {
        *equal = false; return true;
    }
    for (size_t i = 0; i < left->parameter_types.count; ++i) {
        if (plan->instance_accesses[left->parameter_accesses.offset + i]
                != plan->instance_accesses[right->parameter_accesses.offset + i]
            || compare_type_descriptor(plan, plan->instance_type_ids[
                left->parameter_types.offset + i], plan->instance_type_ids[
                right->parameter_types.offset + i], 0, &valid, work) != 0) {
            *equal = false; return valid;
        }
    }
    if (compare_type_descriptor(plan, left->result, right->result, 0,
            &valid, work) != 0
        || compare_effect_descriptor(plan, left->effects, right->effects,
            &valid, work) != 0) {
        *equal = false; return valid;
    }
    *equal = true; return valid;
}

bool sol_mir_linkage_internal_host_descriptor_equal(
    const SolMirLinkage *linkage, SolMirMaterializedImportId left_id,
    SolMirMaterializedImportId right_id, bool *equal,
    SolMirLinkageWorkMeter *work) {
    if (linkage == NULL || linkage->operations == NULL || equal == NULL)
        return false;
    const SolMirMaterialization *materialization = linkage->operations->layout
        ->representation->materialization;
    const SolMirPlan *plan = materialization->plan;
    if (left_id >= materialization->import_count
        || right_id >= materialization->import_count) return false;
    const SolMirMaterializedImport *left = &materialization->imports[left_id];
    const SolMirMaterializedImport *right = &materialization->imports[right_id];
    if (left->source_import >= plan->import_count
        || right->source_import >= plan->import_count) return false;
    return signature_equal(plan, left, &plan->imports[left->source_import],
        right, &plan->imports[right->source_import], equal, work);
}

bool sol_mir_linkage_internal_recipe_descriptor_equal(
    const SolMirLinkage *linkage, SolMirRecipeId left, SolMirRecipeId right,
    bool *equal, SolMirLinkageWorkMeter *work) {
    if (linkage == NULL || linkage->operations == NULL || equal == NULL)
        return false;
    const SolMirPlan *plan = linkage->operations->layout->representation
        ->materialization->plan;
    bool valid = true;
    int order = compare_type_descriptor(plan, left, right, 0, &valid, work);
    *equal = valid && order == 0;
    return valid;
}

bool sol_mir_linkage_internal_table_descriptor_equal(
    const SolMirLinkage *linkage, SolMirLinkageTargetKind left_kind,
    SolMirLinkageCallableId left_internal,
    SolMirLinkageHostRequirementId left_host,
    SolMirLinkageTargetKind right_kind,
    SolMirLinkageCallableId right_internal,
    SolMirLinkageHostRequirementId right_host, bool *equal,
    SolMirLinkageWorkMeter *work) {
    if (linkage == NULL || equal == NULL) return false;
    if (left_kind != right_kind) { *equal = false; return true; }
    if (left_kind == SOL_MIR_LINKAGE_TARGET_INTERNAL) {
        if (left_internal >= linkage->callable_count
            || right_internal >= linkage->callable_count
            || left_host != SOL_MIR_LINKAGE_NONE
            || right_host != SOL_MIR_LINKAGE_NONE) return false;
        return sol_mir_linkage_internal_instance_descriptor_equal(linkage,
            linkage->callables[left_internal].instance,
            linkage->callables[right_internal].instance, equal, work);
    }
    if (left_kind == SOL_MIR_LINKAGE_TARGET_HOST) {
        if (left_host >= linkage->host_requirement_count
            || right_host >= linkage->host_requirement_count
            || left_internal != SOL_MIR_LINKAGE_NONE
            || right_internal != SOL_MIR_LINKAGE_NONE) return false;
        return sol_mir_linkage_internal_host_descriptor_equal(linkage,
            linkage->host_requirements[left_host].import,
            linkage->host_requirements[right_host].import, equal, work);
    }
    return false;
}

bool sol_mir_linkage_internal_table_key(const SolMirLinkage *linkage,
    SolMirLinkageTargetKind kind, SolMirLinkageCallableId internal,
    SolMirLinkageHostRequirementId host, SolMirLinkageDigest *digest,
    SolMirLinkageWorkMeter *work) {
    if (linkage == NULL || digest == NULL) return false;
    Sha256 sha; start_domain(&sha, "sol.mir.function-table-key/1", work);
    token(&sha, TOKEN_TABLE);
    if (kind == SOL_MIR_LINKAGE_TARGET_INTERNAL) {
        if (internal >= linkage->callable_count || host != SOL_MIR_LINKAGE_NONE)
            return false;
        label(&sha, "internal");
        bytes(&sha, linkage->callables[internal].symbol.bytes,
            SOL_MIR_LINKAGE_SYMBOL_LENGTH);
    } else if (kind == SOL_MIR_LINKAGE_TARGET_HOST) {
        if (host >= linkage->host_requirement_count
            || internal != SOL_MIR_LINKAGE_NONE) return false;
        label(&sha, "host");
        semantic_id(&sha, linkage->host_requirements[host].semantic_id);
        bytes(&sha, (const char *)linkage->host_requirements[host]
            .requirement_key.bytes, SOL_MIR_LINKAGE_DIGEST_BYTES);
    } else return false;
    if (!sha256_finish(&sha, digest)) return false;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (force_table_collision) memset(digest, 0, sizeof(*digest));
#endif
    return true;
}

void sol_mir_linkage_internal_symbol(char namespace_kind,
    SolSemanticId semantic, const SolMirLinkageDigest *digest,
    SolMirLinkageSymbol *symbol) {
    static const char hex[] = "0123456789abcdef";
    memcpy(symbol->bytes, "sol.x1.", 7); symbol->bytes[4] = namespace_kind;
    size_t at = 7;
    uint64_t words[2] = {semantic.high, semantic.low};
    for (size_t word = 0; word < 2; ++word)
        for (size_t i = 0; i < 16; ++i)
            symbol->bytes[at++] = hex[(words[word] >> ((15 - i) * 4)) & 15u];
    symbol->bytes[at++] = '.';
    for (size_t i = 0; i < SOL_MIR_LINKAGE_DIGEST_BYTES; ++i) {
        symbol->bytes[at++] = hex[digest->bytes[i] >> 4];
        symbol->bytes[at++] = hex[digest->bytes[i] & 15u];
    }
    symbol->bytes[at] = '\0';
}
