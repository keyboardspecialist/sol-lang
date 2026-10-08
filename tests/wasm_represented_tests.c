#include "wasm_represented.h"
#include "sol/package.h"
#include "sol/interpreter.h"

#include <stdio.h>
#include <wasm.h>
#include <binaryen-c.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

static uint32_t rotate_right(uint32_t value, unsigned amount) {
    return value >> amount | value << (32u - amount);
}

static void sha256(const uint8_t *input, size_t count, uint8_t digest[32]) {
    static const uint32_t constants[64] = {0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
        0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,
        0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
        0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,0x983e5152u,
        0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,
        0x92722c85u,0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,
        0xf40e3585u,0x106aa070u,0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,
        0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
        0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
    uint32_t state[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    uint8_t block[64];
    size_t blocks = count / 64;
    uint64_t bits = (uint64_t)count * 8u;
    for (size_t block_index = 0; block_index <= blocks; ++block_index) {
        size_t offset = block_index * 64, used = count - offset;
        if (used > 64) used = 64;
        memset(block, 0, sizeof block);
        if (used != 0) memcpy(block, input + offset, used);
        if (used < 64) block[used] = UINT8_C(0x80);
        if (block_index == blocks && used >= 56) { /* Final length needs a dedicated block. */
                uint32_t work[64];
                for (size_t i = 0; i < 16; ++i) work[i] = (uint32_t)block[i * 4] << 24
                    | (uint32_t)block[i * 4 + 1] << 16 | (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
                for (size_t i = 16; i < 64; ++i) work[i] = work[i - 16] + (rotate_right(work[i - 15], 7) ^ rotate_right(work[i - 15], 18) ^ work[i - 15] >> 3) + work[i - 7] + (rotate_right(work[i - 2], 17) ^ rotate_right(work[i - 2], 19) ^ work[i - 2] >> 10);
                uint32_t a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
                for (size_t i = 0; i < 64; ++i) { uint32_t t1=h+(rotate_right(e,6)^rotate_right(e,11)^rotate_right(e,25))+((e&f)^((~e)&g))+constants[i]+work[i]; uint32_t t2=(rotate_right(a,2)^rotate_right(a,13)^rotate_right(a,22))+((a&b)^(a&c)^(b&c)); h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2; }
                state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
            memset(block, 0, sizeof block);
        }
        if (block_index == blocks) for (size_t i = 0; i < 8; ++i) block[63 - i] = (uint8_t)(bits >> (i * 8));
        uint32_t work[64];
        for (size_t i = 0; i < 16; ++i) work[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 | (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
        for (size_t i = 16; i < 64; ++i) work[i] = work[i - 16] + (rotate_right(work[i - 15], 7) ^ rotate_right(work[i - 15], 18) ^ work[i - 15] >> 3) + work[i - 7] + (rotate_right(work[i - 2], 17) ^ rotate_right(work[i - 2], 19) ^ work[i - 2] >> 10);
        uint32_t a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
        for (size_t i = 0; i < 64; ++i) { uint32_t t1=h+(rotate_right(e,6)^rotate_right(e,11)^rotate_right(e,25))+((e&f)^((~e)&g))+constants[i]+work[i]; uint32_t t2=(rotate_right(a,2)^rotate_right(a,13)^rotate_right(a,22))+((a&b)^(a&c)^(b&c)); h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2; }
        state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
    }
    for (size_t i = 0; i < 8; ++i) for (size_t j = 0; j < 4; ++j) digest[i * 4 + j] = (uint8_t)(state[i] >> (24 - j * 8));
}

static bool name_equal(const wasm_name_t *name, const char *text) { return name != NULL && name->size == strlen(text) && memcmp(name->data, text, name->size) == 0; }
typedef struct {
    wasm_engine_t *engine;
    wasm_store_t *store;
    wasm_module_t *module;
    wasm_instance_t *instance;
    wasm_extern_vec_t exports;
    wasm_func_t *entry;
    wasm_global_t *code, *site, *panic_detail_offset, *panic_detail_length, *writebacks;
    wasm_global_t *cleanup_counters[4];
    wasm_global_t *trace_offset, *trace_count, *trace_overflow;
    wasm_memory_t *memory;
} WasmInstance;

typedef struct {
    uint32_t action, disposition, record;
} P44TraceSlot;

static void wasm_instance_close(WasmInstance *instance) {
    wasm_extern_vec_delete(&instance->exports);
    if (instance->instance != NULL) wasm_instance_delete(instance->instance);
    if (instance->module != NULL) wasm_module_delete(instance->module);
    if (instance->store != NULL) wasm_store_delete(instance->store);
    if (instance->engine != NULL) wasm_engine_delete(instance->engine);
    memset(instance, 0, sizeof *instance);
}

static bool wasm_instance_open(const SolWasmBackendBytes *bytes, const char *entry,
    WasmInstance *instance) {
    memset(instance, 0, sizeof *instance);
    instance->exports = (wasm_extern_vec_t)WASM_EMPTY_VEC;
    instance->engine = wasm_engine_new();
    instance->store = instance->engine == NULL ? NULL : wasm_store_new(instance->engine);
    wasm_byte_vec_t module_bytes = {bytes->count, (wasm_byte_t *)bytes->bytes};
    instance->module = instance->store == NULL ? NULL : wasm_module_new(instance->store, &module_bytes);
    wasm_importtype_vec_t imports;
    wasm_exporttype_vec_t types;
    size_t function = SIZE_MAX, code = SIZE_MAX, site = SIZE_MAX, panic_detail_offset = SIZE_MAX,
        panic_detail_length = SIZE_MAX, writebacks = SIZE_MAX, trace_offset = SIZE_MAX,
        trace_count = SIZE_MAX, trace_overflow = SIZE_MAX, memory = SIZE_MAX;
    size_t cleanup_counters[4] = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
    bool ok = instance->module != NULL;
    if (ok) {
        wasm_module_imports(instance->module, &imports);
        ok = imports.size == 0;
        wasm_importtype_vec_delete(&imports);
    }
    if (ok) {
        wasm_module_exports(instance->module, &types);
        for (size_t i = 0; i < types.size; ++i) {
            const wasm_name_t *name = wasm_exporttype_name(types.data[i]);
            wasm_externkind_t kind = wasm_externtype_kind(wasm_exporttype_type(types.data[i]));
            if (name_equal(name, entry)) { if (kind != WASM_EXTERN_FUNC || function != SIZE_MAX) ok = false; function = i; }
            if (name_equal(name, SOL_WASM_REPRESENTED_FAILURE_CODE_EXPORT)) { if (kind != WASM_EXTERN_GLOBAL || code != SIZE_MAX) ok = false; code = i; }
            if (name_equal(name, SOL_WASM_REPRESENTED_FAILURE_SITE_EXPORT)) { if (kind != WASM_EXTERN_GLOBAL || site != SIZE_MAX) ok = false; site = i; }
            if (name_equal(name, SOL_WASM_REPRESENTED_PANIC_DETAIL_OFFSET_EXPORT)) {
                if (kind != WASM_EXTERN_GLOBAL || panic_detail_offset != SIZE_MAX) ok = false;
                panic_detail_offset = i;
            }
            if (name_equal(name, SOL_WASM_REPRESENTED_PANIC_DETAIL_LENGTH_EXPORT)) {
                if (kind != WASM_EXTERN_GLOBAL || panic_detail_length != SIZE_MAX) ok = false;
                panic_detail_length = i;
            }
            if (name_equal(name, SOL_WASM_REPRESENTED_TEST_WRITEBACK_EXPORT)) {
                if (kind != WASM_EXTERN_GLOBAL || writebacks != SIZE_MAX) ok = false;
                writebacks = i;
            }
            if (name_equal(name, SOL_WASM_REPRESENTED_TEST_P44_TRACE_OFFSET_EXPORT)) {
                if (kind != WASM_EXTERN_GLOBAL || trace_offset != SIZE_MAX) ok = false;
                trace_offset = i;
            }
            if (name_equal(name, SOL_WASM_REPRESENTED_TEST_P44_TRACE_COUNT_EXPORT)) {
                if (kind != WASM_EXTERN_GLOBAL || trace_count != SIZE_MAX) ok = false;
                trace_count = i;
            }
            if (name_equal(name, SOL_WASM_REPRESENTED_TEST_P44_TRACE_OVERFLOW_EXPORT)) {
                if (kind != WASM_EXTERN_GLOBAL || trace_overflow != SIZE_MAX) ok = false;
                trace_overflow = i;
            }
            const char *const cleanup_exports[] = {
                SOL_WASM_REPRESENTED_TEST_CLEANUP_OLD_CALLABLE_EXPORT,
                SOL_WASM_REPRESENTED_TEST_CLEANUP_MOVED_CALLABLE_EXPORT,
                SOL_WASM_REPRESENTED_TEST_CLEANUP_TEXT_SIBLING_EXPORT,
                SOL_WASM_REPRESENTED_TEST_CLEANUP_ROOT_EXPORT,
            };
            for (size_t q = 0; q < sizeof cleanup_exports / sizeof *cleanup_exports; ++q)
                if (name_equal(name, cleanup_exports[q])) {
                    if (kind != WASM_EXTERN_GLOBAL || cleanup_counters[q] != SIZE_MAX) ok = false;
                    cleanup_counters[q] = i;
                }
            if (name_equal(name, SOL_WASM_BACKEND_MEMORY_EXPORT)) { if (kind != WASM_EXTERN_MEMORY || memory != SIZE_MAX) ok = false; memory = i; }
        }
        ok = ok && function != SIZE_MAX && code != SIZE_MAX && site != SIZE_MAX && memory != SIZE_MAX;
        wasm_exporttype_vec_delete(&types);
    }
    wasm_extern_vec_t none = WASM_EMPTY_VEC;
    wasm_trap_t *trap = NULL;
    if (ok) instance->instance = wasm_instance_new(instance->store, instance->module, &none, &trap);
    if (trap != NULL) { wasm_trap_delete(trap); ok = false; }
    if (ok && instance->instance != NULL) {
        wasm_instance_exports(instance->instance, &instance->exports);
        instance->entry = function < instance->exports.size ? wasm_extern_as_func(instance->exports.data[function]) : NULL;
        instance->code = code < instance->exports.size ? wasm_extern_as_global(instance->exports.data[code]) : NULL;
        instance->site = site < instance->exports.size ? wasm_extern_as_global(instance->exports.data[site]) : NULL;
        instance->panic_detail_offset = panic_detail_offset < instance->exports.size
            ? wasm_extern_as_global(instance->exports.data[panic_detail_offset]) : NULL;
        instance->panic_detail_length = panic_detail_length < instance->exports.size
            ? wasm_extern_as_global(instance->exports.data[panic_detail_length]) : NULL;
        instance->writebacks = writebacks < instance->exports.size
            ? wasm_extern_as_global(instance->exports.data[writebacks]) : NULL;
        instance->trace_offset = trace_offset < instance->exports.size
            ? wasm_extern_as_global(instance->exports.data[trace_offset]) : NULL;
        instance->trace_count = trace_count < instance->exports.size
            ? wasm_extern_as_global(instance->exports.data[trace_count]) : NULL;
        instance->trace_overflow = trace_overflow < instance->exports.size
            ? wasm_extern_as_global(instance->exports.data[trace_overflow]) : NULL;
        for (size_t i = 0; i < sizeof cleanup_counters / sizeof *cleanup_counters; ++i)
            instance->cleanup_counters[i] = cleanup_counters[i] < instance->exports.size
                ? wasm_extern_as_global(instance->exports.data[cleanup_counters[i]]) : NULL;
        instance->memory = memory < instance->exports.size ? wasm_extern_as_memory(instance->exports.data[memory]) : NULL;
        wasm_functype_t *type = instance->entry == NULL ? NULL : wasm_func_type(instance->entry);
        const wasm_valtype_vec_t *parameters = type == NULL ? NULL : wasm_functype_params(type);
        const wasm_valtype_vec_t *results = type == NULL ? NULL : wasm_functype_results(type);
        ok = instance->entry != NULL && instance->code != NULL && instance->site != NULL
            && instance->memory != NULL
            && parameters->size == 0 && results->size == 1
            && wasm_valtype_kind(results->data[0]) == WASM_I64;
        if (type != NULL) wasm_functype_delete(type);
    } else ok = false;
    if (!ok) wasm_instance_close(instance);
    return ok;
}

static bool wasm_instance_call(WasmInstance *instance, int64_t value, int32_t code,
    int32_t site) {
    wasm_val_t result[1], got_code, got_site;
    wasm_val_vec_t arguments = WASM_EMPTY_VEC, results = WASM_ARRAY_VEC(result);
    wasm_trap_t *trap = wasm_func_call(instance->entry, &arguments, &results);
    if (trap != NULL) { wasm_trap_delete(trap); return false; }
    wasm_global_get(instance->code, &got_code); wasm_global_get(instance->site, &got_site);
    return result[0].kind == WASM_I64 && result[0].of.i64 == value
        && got_code.kind == WASM_I32 && got_code.of.i32 == code
        && got_site.kind == WASM_I32 && (site < 0 ? got_site.of.i32 > 0 : got_site.of.i32 == site);
}

static bool wasm_instance_call_named(WasmInstance *instance, const char *entry, int64_t value,
    int32_t code, int32_t site) {
    wasm_exporttype_vec_t types; size_t index = SIZE_MAX;
    if (instance == NULL || instance->module == NULL || entry == NULL) return false;
    wasm_module_exports(instance->module, &types);
    for (size_t i = 0; i < types.size; ++i)
        if (name_equal(wasm_exporttype_name(types.data[i]), entry)
            && wasm_externtype_kind(wasm_exporttype_type(types.data[i])) == WASM_EXTERN_FUNC) {
            if (index != SIZE_MAX) { index = SIZE_MAX; break; }
            index = i;
        }
    wasm_exporttype_vec_delete(&types);
    if (index == SIZE_MAX || index >= instance->exports.size) return false;
    wasm_func_t *function = wasm_extern_as_func(instance->exports.data[index]);
    wasm_val_t result[1], got_code, got_site;
    wasm_val_vec_t arguments = WASM_EMPTY_VEC, results = WASM_ARRAY_VEC(result);
    wasm_trap_t *trap = function == NULL ? NULL : wasm_func_call(function, &arguments, &results);
    if (function == NULL || trap != NULL) { if (trap != NULL) wasm_trap_delete(trap); return false; }
    wasm_global_get(instance->code, &got_code); wasm_global_get(instance->site, &got_site);
    return result[0].kind == WASM_I64 && result[0].of.i64 == value
        && got_code.kind == WASM_I32 && got_code.of.i32 == code
        && got_site.kind == WASM_I32 && (site < 0 ? got_site.of.i32 > 0 : got_site.of.i32 == site);
}

static bool wasm_instance_observe(WasmInstance *instance, int64_t *value, int32_t *code,
    int32_t *site) {
    wasm_val_t result[1], got_code, got_site;
    wasm_val_vec_t arguments = WASM_EMPTY_VEC, results = WASM_ARRAY_VEC(result);
    wasm_trap_t *trap = wasm_func_call(instance->entry, &arguments, &results);
    if (trap != NULL) { wasm_trap_delete(trap); return false; }
    wasm_global_get(instance->code, &got_code); wasm_global_get(instance->site, &got_site);
    if (result[0].kind != WASM_I64 || got_code.kind != WASM_I32 || got_site.kind != WASM_I32)
        return false;
    *value = result[0].of.i64; *code = got_code.of.i32; *site = got_site.of.i32;
    return true;
}

static bool wasm_instance_writebacks(WasmInstance *instance, int32_t expected) {
    wasm_val_t value;
    if (instance == NULL || instance->writebacks == NULL) return false;
    wasm_global_get(instance->writebacks, &value);
    return value.kind == WASM_I32 && value.of.i32 == expected;
}

static bool wasm_instance_cleanup_counters(WasmInstance *instance, const int32_t expected[4]) {
    if (instance == NULL || expected == NULL) return false;
    for (size_t i = 0; i < 4; ++i) {
        wasm_val_t value;
        if (instance->cleanup_counters[i] == NULL) return false;
        wasm_global_get(instance->cleanup_counters[i], &value);
        if (value.kind != WASM_I32 || value.of.i32 != expected[i]) return false;
    }
    return true;
}

/* Read the hook ledger as raw Wasm memory: every slot is three little-endian
 * i32 words (action index, disposition, failure provenance record). */
static bool wasm_instance_trace(WasmInstance *instance, P44TraceSlot *slots, size_t capacity,
    size_t *count, bool *overflow) {
    wasm_val_t offset, length, lost;
    if (instance == NULL || slots == NULL || count == NULL || overflow == NULL
        || instance->trace_offset == NULL || instance->trace_count == NULL
        || instance->trace_overflow == NULL || instance->memory == NULL) return false;
    wasm_global_get(instance->trace_offset, &offset); wasm_global_get(instance->trace_count, &length);
    wasm_global_get(instance->trace_overflow, &lost);
    if (offset.kind != WASM_I32 || length.kind != WASM_I32 || lost.kind != WASM_I32
        || offset.of.i32 < 0 || length.of.i32 < 0 || length.of.i32 > 64
        || (lost.of.i32 != 0 && lost.of.i32 != 1) || (size_t)length.of.i32 > capacity
        || (size_t)offset.of.i32 > wasm_memory_data_size(instance->memory)
        || (size_t)length.of.i32 > (wasm_memory_data_size(instance->memory)
            - (size_t)offset.of.i32) / 12) return false;
    const uint8_t *bytes = (const uint8_t *)wasm_memory_data(instance->memory) + offset.of.i32;
    for (size_t i = 0; i < (size_t)length.of.i32; ++i) {
        const uint8_t *slot = bytes + i * 12;
        slots[i] = (P44TraceSlot){(uint32_t)slot[0] | (uint32_t)slot[1] << 8
            | (uint32_t)slot[2] << 16 | (uint32_t)slot[3] << 24,
            (uint32_t)slot[4] | (uint32_t)slot[5] << 8 | (uint32_t)slot[6] << 16
                | (uint32_t)slot[7] << 24,
            (uint32_t)slot[8] | (uint32_t)slot[9] << 8 | (uint32_t)slot[10] << 16
                | (uint32_t)slot[11] << 24};
    }
    *count = (size_t)length.of.i32; *overflow = lost.of.i32 != 0;
    return true;
}

static bool wasm_instance_panic_detail(WasmInstance *instance, const uint8_t *expected,
    size_t expected_count) {
    wasm_val_t offset, length;
    if (instance == NULL || instance->panic_detail_offset == NULL || instance->panic_detail_length == NULL
        || instance->memory == NULL || expected_count > 191) return false;
    wasm_global_get(instance->panic_detail_offset, &offset);
    wasm_global_get(instance->panic_detail_length, &length);
    if (offset.kind != WASM_I32 || length.kind != WASM_I32 || length.of.i32 < 0
        || (size_t)length.of.i32 != expected_count || offset.of.i32 < 0
        || (size_t)offset.of.i32 > wasm_memory_data_size(instance->memory)
        || expected_count + 1 > wasm_memory_data_size(instance->memory) - (size_t)offset.of.i32) return false;
    uint8_t *bytes = (uint8_t *)wasm_memory_data(instance->memory) + offset.of.i32;
    return memcmp(bytes, expected, expected_count) == 0 && bytes[expected_count] == 0;
}

static bool invoke_named(const SolWasmBackendBytes *bytes, const char *entry, int64_t value,
    int32_t code, int32_t site) {
    WasmInstance instance;
    bool ok = wasm_instance_open(bytes, entry, &instance)
        && wasm_instance_call(&instance, value, code, site);
    wasm_instance_close(&instance);
    return ok;
}

typedef struct {
    uint8_t tag, kind;
    const uint8_t *path, *symbol;
    uint32_t path_count, start, end, symbol_count, ordinal;
} ProvenanceRecord;

static bool read_u32le(const uint8_t **cursor, const uint8_t *end, uint32_t *value) {
    if ((size_t)(end - *cursor) < 4) return false;
    const uint8_t *bytes = *cursor;
    *value = (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16
        | (uint32_t)bytes[3] << 24;
    *cursor += 4;
    return true;
}

static bool read_uleb32(const uint8_t **cursor, const uint8_t *end, uint32_t *value) {
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 35; shift += 7) {
        if (*cursor == end || (shift == 28 && (**cursor & UINT8_C(0xf0)) != 0)) return false;
        uint8_t byte = *(*cursor)++;
        result |= (uint32_t)(byte & UINT8_C(0x7f)) << shift;
        if ((byte & UINT8_C(0x80)) == 0) { *value = result; return true; }
    }
    return false;
}

static bool bytes_equal(const uint8_t *bytes, uint32_t count, const char *text) {
    return count == strlen(text) && memcmp(bytes, text, count) == 0;
}

static bool usage_zero(const SolWasmRepresentedUsage *usage) {
    static const SolWasmRepresentedUsage zero = {0};
    return memcmp(usage, &zero, sizeof zero) == 0;
}

/* Keep propagation's frozen census field-by-field.  In particular, output
 * bytes alone do not authenticate the build-side resource vector. */
static bool usage_equal(const SolWasmRepresentedUsage *actual,
    const SolWasmRepresentedUsage *expected) {
    return actual->functions == expected->functions
        && actual->blocks == expected->blocks
        && actual->edges == expected->edges
        && actual->values == expected->values
        && actual->locals == expected->locals
        && actual->generated_nodes == expected->generated_nodes
        && actual->table_elements == expected->table_elements
        && actual->static_data_bytes == expected->static_data_bytes
        && actual->allocation_requests == expected->allocation_requests
        && actual->allocation_bytes == expected->allocation_bytes
        && actual->provenance_records == expected->provenance_records
        && actual->work_bytes == expected->work_bytes
        && actual->scratch_bytes == expected->scratch_bytes
        && actual->owned_bytes == expected->owned_bytes
        && actual->output_bytes == expected->output_bytes;
}

static bool provenance_record(const SolWasmBackendBytes *module, uint32_t wanted,
    ProvenanceRecord *record) {
    const uint8_t *cursor = module->bytes, *end = module->bytes + module->count;
    static const uint8_t wasm_header[] = {0, 'a', 's', 'm', 1, 0, 0, 0};
    if (wanted == 0 || module->count < sizeof wasm_header
        || memcmp(cursor, wasm_header, sizeof wasm_header) != 0) return false;
    cursor += sizeof wasm_header;
    while (cursor != end) {
        uint32_t bytes = 0, name_count = 0;
        uint8_t id = *cursor++;
        if (!read_uleb32(&cursor, end, &bytes) || bytes > (size_t)(end - cursor)) return false;
        if (id != 0) {
            cursor += bytes;
            continue;
        }
        const uint8_t *section_end = cursor + bytes;
        if (!read_uleb32(&cursor, section_end, &name_count) || name_count > (size_t)(section_end - cursor))
            return false;
        const uint8_t *name = cursor; cursor += name_count;
        if (!bytes_equal(name, name_count, SOL_WASM_REPRESENTED_PROVENANCE_SECTION)) {
            cursor = section_end;
            continue;
        }
        if ((size_t)(section_end - cursor) < 12 || memcmp(cursor, "P43P", 4) != 0) return false;
        cursor += 4;
        uint32_t version = 0, count = 0;
        if (!read_u32le(&cursor, section_end, &version) || !read_u32le(&cursor, section_end, &count)
            || version != 1 || wanted > count) return false;
        for (uint32_t index = 1; index <= count; ++index) {
            ProvenanceRecord candidate;
            uint32_t reserved = 0;
            if ((size_t)(section_end - cursor) < 4) return false;
            candidate.tag = *cursor++; candidate.kind = *cursor++;
            reserved = (uint32_t)cursor[0] | (uint32_t)cursor[1] << 8;
            cursor += 2;
            if (reserved != 0 || !read_u32le(&cursor, section_end, &candidate.path_count)
                || candidate.path_count > (size_t)(section_end - cursor)) return false;
            candidate.path = cursor; cursor += candidate.path_count;
            if (!read_u32le(&cursor, section_end, &candidate.start)
                || !read_u32le(&cursor, section_end, &candidate.end)
                || !read_u32le(&cursor, section_end, &candidate.symbol_count)
                || candidate.symbol_count > (size_t)(section_end - cursor)) return false;
            candidate.symbol = cursor; cursor += candidate.symbol_count;
            if (!read_u32le(&cursor, section_end, &candidate.ordinal)) return false;
            if (index == wanted) { *record = candidate; return true; }
        }
        return false;
    }
    return false;
}

typedef struct {
    size_t section_start, section_end, payload, payload_count;
    size_t record_start[4096], record_end[4096];
    uint32_t count;
} ProvenanceLayout;

typedef struct {
    size_t section_start, section_end, size_offset, size_width, payload;
    size_t first_name_length, first_name, first_kind, first_index;
} ExportLayout;

/* Test-side parser deliberately does not share the backend parser. */
static bool provenance_layout(const SolWasmBackendBytes *module, ProvenanceLayout *layout) {
    const uint8_t *cursor = module->bytes + 8, *end = module->bytes + module->count;
    memset(layout, 0, sizeof *layout);
    while (cursor < end) {
        const uint8_t *section = cursor;
        uint32_t size = 0, name_size = 0;
        if (++cursor > end || !read_uleb32(&cursor, end, &size) || size > (size_t)(end - cursor)) return false;
        const uint8_t *section_end = cursor + size;
        if (section[0] != 0) { cursor = section_end; continue; }
        if (!read_uleb32(&cursor, section_end, &name_size) || name_size > (size_t)(section_end - cursor)) return false;
        const uint8_t *name = cursor; cursor += name_size;
        if (!bytes_equal(name, name_size, SOL_WASM_REPRESENTED_PROVENANCE_SECTION)) { cursor = section_end; continue; }
        if ((size_t)(section_end - cursor) < 12 || memcmp(cursor, "P43P", 4) != 0) return false;
        layout->section_start = (size_t)(section - module->bytes);
        layout->section_end = (size_t)(section_end - module->bytes);
        layout->payload = (size_t)(cursor - module->bytes);
        cursor += 8;
        if (!read_u32le(&cursor, section_end, &layout->count) || layout->count > 4096) return false;
        for (uint32_t i = 0; i < layout->count; ++i) {
            uint32_t path = 0, start = 0, finish = 0, symbol = 0, ordinal = 0;
            layout->record_start[i] = (size_t)(cursor - module->bytes);
            if ((size_t)(section_end - cursor) < 4) return false;
            cursor += 4;
            if (!read_u32le(&cursor, section_end, &path) || path > (size_t)(section_end - cursor)) return false;
            cursor += path;
            if (!read_u32le(&cursor, section_end, &start)
                || !read_u32le(&cursor, section_end, &finish)
                || !read_u32le(&cursor, section_end, &symbol)
                || symbol > (size_t)(section_end - cursor)) return false;
            cursor += symbol;
            if (!read_u32le(&cursor, section_end, &ordinal)) return false;
            layout->record_end[i] = (size_t)(cursor - module->bytes);
        }
        layout->payload_count = (size_t)(section_end - (module->bytes + layout->payload));
        return true;
    }
    return false;
}

static bool export_layout(const SolWasmBackendBytes *module, ExportLayout *layout) {
    const uint8_t *cursor = module->bytes + 8, *end = module->bytes + module->count;
    memset(layout, 0, sizeof *layout);
    while (cursor < end) {
        const uint8_t *section = cursor++;
        const uint8_t *size = cursor;
        uint32_t bytes = 0;
        if (!read_uleb32(&cursor, end, &bytes) || bytes > (size_t)(end - cursor)) return false;
        const uint8_t *section_end = cursor + bytes;
        if (*section != 7) { cursor = section_end; continue; }
        uint32_t count = 0, name_count = 0, ignored = 0;
        const uint8_t *count_start = cursor;
        if (!read_uleb32(&cursor, section_end, &count) || count == 0) return false;
        layout->section_start = (size_t)(section - module->bytes);
        layout->section_end = (size_t)(section_end - module->bytes);
        layout->size_offset = (size_t)(size - module->bytes);
        layout->size_width = (size_t)(count_start - size);
        layout->payload = (size_t)(count_start - module->bytes);
        layout->first_name_length = (size_t)(cursor - module->bytes);
        if (!read_uleb32(&cursor, section_end, &name_count)
            || name_count > (size_t)(section_end - cursor)) return false;
        layout->first_name = (size_t)(cursor - module->bytes);
        cursor += name_count;
        if (cursor == section_end) return false;
        layout->first_kind = (size_t)(cursor - module->bytes);
        ++cursor;
        layout->first_index = (size_t)(cursor - module->bytes);
        return read_uleb32(&cursor, section_end, &ignored);
    }
    return false;
}

typedef struct {
    size_t name, kind, index, index_width;
    uint32_t name_count, global;
} NamedExportLayout;

/* Locate one export independently of the backend envelope reader. */
static bool named_global_export_layout(const SolWasmBackendBytes *module, const char *wanted,
    NamedExportLayout *layout) {
    const uint8_t *cursor = module->bytes + 8, *end = module->bytes + module->count;
    bool found = false;
    memset(layout, 0, sizeof *layout);
    while (cursor < end) {
        uint8_t id = *cursor++; uint32_t bytes = 0;
        if (!read_uleb32(&cursor, end, &bytes) || bytes > (size_t)(end - cursor)) return false;
        const uint8_t *section_end = cursor + bytes;
        if (id == 7) {
            uint32_t count = 0;
            if (!read_uleb32(&cursor, section_end, &count)) return false;
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t name_count = 0, global = 0;
                if (!read_uleb32(&cursor, section_end, &name_count)
                    || name_count > (size_t)(section_end - cursor)) return false;
                const uint8_t *name = cursor; cursor += name_count;
                if (cursor == section_end) return false;
                size_t kind = (size_t)(cursor - module->bytes); uint8_t export_kind = *cursor++;
                size_t index = (size_t)(cursor - module->bytes); const uint8_t *before = cursor;
                if (!read_uleb32(&cursor, section_end, &global)) return false;
                if (bytes_equal(name, name_count, wanted)) {
                    if (found || export_kind != 3) return false;
                    *layout = (NamedExportLayout){(size_t)(name - module->bytes), kind, index,
                        (size_t)(cursor - before), name_count, global};
                    found = true;
                }
            }
            if (cursor != section_end) return false;
        }
        cursor = section_end;
    }
    return found;
}

static bool write_uleb_same_width(uint8_t *bytes, size_t width, uint32_t value) {
    for (size_t i = 0; i < width; ++i) {
        uint8_t byte = (uint8_t)(value & UINT32_C(0x7f));
        value >>= 7;
        if (i + 1 != width) byte |= UINT8_C(0x80);
        bytes[i] = byte;
    }
    return value == 0;
}

typedef struct {
    size_t type[32], mutability[32], initial[32], initial_width[32];
    uint32_t initial_value[32], count;
    size_t offset_export_index, length_export_index, code_export_index;
    uint32_t offset_global, length_global;
} P44WireLayout;

/* Independent minimal parser for the private P4.4 global/export envelope.
 * It intentionally records byte locations so raw tests mutate a single
 * semantic field while keeping the surrounding Wasm structurally valid. */
static bool p44_wire_layout(const SolWasmBackendBytes *module, P44WireLayout *layout) {
    const uint8_t *cursor = module->bytes + 8, *end = module->bytes + module->count;
    bool globals = false, exports = false;
    memset(layout, 0, sizeof *layout);
    while (cursor < end) {
        uint8_t id = *cursor++; uint32_t size = 0;
        if (!read_uleb32(&cursor, end, &size) || size > (size_t)(end - cursor)) return false;
        const uint8_t *section_end = cursor + size;
        if (id == 6) {
            if (globals || !read_uleb32(&cursor, section_end, &layout->count)
                || layout->count > 32) return false;
            globals = true;
            for (uint32_t i = 0; i < layout->count; ++i) {
                if ((size_t)(section_end - cursor) < 3) return false;
                layout->type[i] = (size_t)(cursor - module->bytes);
                uint8_t type = *cursor++;
                layout->mutability[i] = (size_t)(cursor - module->bytes); ++cursor;
                if (*cursor++ != (type == UINT8_C(0x7f) ? UINT8_C(0x41) : UINT8_C(0x42))) return false;
                layout->initial[i] = (size_t)(cursor - module->bytes);
                const uint8_t *value = cursor;
                if (type == UINT8_C(0x7f)) {
                    if (!read_uleb32(&cursor, section_end, &layout->initial_value[i])) return false;
                } else {
                    do { if (cursor == section_end) return false; } while (*cursor++ & UINT8_C(0x80));
                }
                if (cursor == section_end || *cursor++ != UINT8_C(0x0b)) return false;
                layout->initial_width[i] = (size_t)(cursor - 1 - value);
            }
        } else if (id == 7) {
            uint32_t count = 0;
            if (exports || !read_uleb32(&cursor, section_end, &count)) return false;
            exports = true;
            for (uint32_t i = 0; i < count; ++i) {
                const uint8_t *name = NULL; uint32_t name_count = 0, index = 0;
                if (!read_uleb32(&cursor, section_end, &name_count)
                    || name_count > (size_t)(section_end - cursor)) return false;
                name = cursor; cursor += name_count;
                if (cursor == section_end || *cursor++ != 3) {
                    if (cursor == section_end) return false;
                    if (!read_uleb32(&cursor, section_end, &index)) return false;
                    continue;
                }
                size_t index_at = (size_t)(cursor - module->bytes);
                if (!read_uleb32(&cursor, section_end, &index)) return false;
                if (bytes_equal(name, name_count, SOL_WASM_REPRESENTED_FAILURE_CODE_EXPORT))
                    layout->code_export_index = index_at;
                if (bytes_equal(name, name_count, SOL_WASM_REPRESENTED_PANIC_DETAIL_OFFSET_EXPORT)) {
                    layout->offset_export_index = index_at; layout->offset_global = index;
                }
                if (bytes_equal(name, name_count, SOL_WASM_REPRESENTED_PANIC_DETAIL_LENGTH_EXPORT)) {
                    layout->length_export_index = index_at; layout->length_global = index;
                }
            }
        }
        cursor = section_end;
    }
    return globals && exports && layout->code_export_index != 0
        && ((layout->offset_export_index == 0 && layout->length_export_index == 0)
            || (layout->offset_export_index != 0 && layout->length_export_index != 0
                && layout->offset_global < layout->count && layout->length_global < layout->count));
}

/* Keep the raw table-absence case independent of Binaryen: removing an active
 * callback table makes the module Wasmtime-invalid, but private validation
 * must still reject it safely before handing bytes to the engine. */
static bool without_standard_section(const SolWasmBackendBytes *source, uint8_t wanted,
    SolWasmBackendBytes *result) {
    static const uint8_t header[] = {0, 'a', 's', 'm', 1, 0, 0, 0};
    if (source == NULL || result == NULL || source->bytes == NULL || source->count < sizeof header
        || memcmp(source->bytes, header, sizeof header) != 0) return false;
    const uint8_t *cursor = source->bytes + sizeof header, *end = source->bytes + source->count;
    while (cursor < end) {
        const uint8_t *section = cursor++;
        uint32_t count = 0;
        if (!read_uleb32(&cursor, end, &count) || count > (size_t)(end - cursor)) return false;
        const uint8_t *section_end = cursor + count;
        if (*section == wanted) {
            size_t before = (size_t)(section - source->bytes);
            size_t after = (size_t)(end - section_end);
            uint8_t *bytes = malloc(before + after);
            if (bytes == NULL) return false;
            memcpy(bytes, source->bytes, before); memcpy(bytes + before, section_end, after);
            *result = (SolWasmBackendBytes){bytes, before + after};
            return true;
        }
        cursor = section_end;
    }
    return false;
}

static bool module_has_no_sections(const SolWasmBackendBytes *module, uint8_t first,
    uint8_t second, uint8_t third) {
    static const uint8_t header[] = {0, 'a', 's', 'm', 1, 0, 0, 0};
    if (module == NULL || module->bytes == NULL || module->count < sizeof header
        || memcmp(module->bytes, header, sizeof header) != 0) return false;
    const uint8_t *cursor = module->bytes + sizeof header, *end = module->bytes + module->count;
    while (cursor < end) {
        uint8_t id = *cursor++;
        uint32_t size = 0;
        if (!read_uleb32(&cursor, end, &size) || size > (size_t)(end - cursor)
            || id == first || id == second || id == third) return false;
        cursor += size;
    }
    return cursor == end;
}

static bool entry_symbol(const SolWasmBackendBytes *bytes, char *name, size_t capacity) {
    if (bytes == NULL || name == NULL || capacity == 0) return false;
    for (uint32_t record = 1; record <= 64; ++record) {
        ProvenanceRecord candidate;
        if (!provenance_record(bytes, record, &candidate)) break;
        if (candidate.symbol_count < 7 || candidate.symbol_count >= capacity
            || memcmp(candidate.symbol, "sol.e1.", 7) != 0) continue;
        memcpy(name, candidate.symbol, candidate.symbol_count);
        name[candidate.symbol_count] = '\0';
        return true;
    }
    return false;
}

static int failures;

/* Coverage bookkeeping never resets a production test hook. */
typedef enum {
    FAULT_C32_REPAIR, FAULT_PROPAGATION_RESULT, FAULT_B1_FIRST_ALLOCATION,
    FAULT_REQUIRES_FORWARD, FAULT_QUALIFIED_FROZEN, FAULT_QUALIFIED_RESULT,
    FAULT_STRESS, FAULT_OLD_SNAPSHOT, FAULT_ENSURES_ROOTS, FAULT_REFINED,
    FAULT_CALLABLE_HOLE, FAULT_CALLBACK_INOUT, FAULT_METHOD, FAULT_CALLBACK,
    FAULT_PATTERN, FAULT_PANIC, FAULT_PRODUCT_TREE, FAULT_SUM_TEXT,
    FAULT_PARSER_MAP, FAULT_TEXT_BUILD, FAULT_DEPTH, FAULT_SWEEP_COUNT
} RepresentedFault;
static void represented_control(const char *id);
static void represented_fault(RepresentedFault sweep, size_t ordinal, size_t hook_after);

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #value); \
    ++failures; \
} } while (0)

static bool build_multiroot(const char *directory, bool reverse, SolWasmRepresentedOutput *output,
    size_t ids[2], const SolWasmRepresentedLimits *limits, SolWasmRepresentedResult expected) {
    SolDiagnostics diagnostics; SolHirModule hir; SolTypeTable types; SolEffectTable effects;
    SolContractTable contracts; SolIr ir; SolPackage package;
    SolMirConcreteProgram concrete; SolMirRuntimeConventions conventions; SolMirRuntimeValues values;
    SolMirRuntimeCleanup cleanup; SolMirRuntimeHostAbi host; SolMirRuntimeHandlerAbi handler;
    SolMirRuntimeLoweredProgram lowered;
    sol_diagnostics_init(&diagnostics); sol_hir_module_init(&hir); sol_type_table_init(&types);
    sol_effect_table_init(&effects); sol_contract_table_init(&contracts); sol_ir_init(&ir);
    sol_package_init(&package); sol_mir_concrete_program_init(&concrete);
    sol_mir_runtime_conventions_init(&conventions); sol_mir_runtime_values_init(&values);
    sol_mir_runtime_cleanup_init(&cleanup); sol_mir_runtime_host_abi_init(&host);
    sol_mir_runtime_handler_abi_init(&handler); sol_mir_runtime_lowered_program_init(&lowered);
    ids[0] = SIZE_MAX; ids[1] = SIZE_MAX;
    char error[256]; bool ok = sol_package_load_directory(&package, directory, &diagnostics, error,
        sizeof error);
    SolHirFileScope scope = {0};
    if (ok) scope = (SolHirFileScope){package.files[0].module_name, package.files[0].import_start,
        package.files[0].import_count, package.files[0].item_start, package.files[0].item_count};
    if (ok) ok = sol_hir_lower_scoped(&package.source, &package.syntax, &scope, 1, &hir, &diagnostics)
        && sol_type_check(&package.source, &package.syntax, &hir, &types, &diagnostics)
        && sol_effect_check(&package.source, &package.syntax, &hir, &types, &effects, &diagnostics)
        && sol_contract_lower(&package.source, &package.syntax, &hir, &types, &effects, &contracts,
            &diagnostics)
        && sol_ir_lower_scoped(&package.source, &package.syntax, &hir, &types, &effects, &contracts,
            package.files, 1, &ir, &diagnostics);
    SolMirProgramRoot roots[2]; size_t root_count = 0;
    bool callback_roots = strstr(directory, "p43_callback_prereq") != NULL;
    bool callback_inout_roots = strstr(directory, "p43_callback_inout") != NULL;
    bool method_failure_entry_roots = strstr(directory, "p43_method_failure_entry") != NULL;
    bool method_roots = strstr(directory, "p43_method_prereq") != NULL || method_failure_entry_roots;
    bool callable_hole_roots = strstr(directory, "p43_callable_hole_prereq") != NULL
        || strstr(directory, "p43_callable_hole_c32_repair") != NULL;
    bool p44_terminal_roots = strstr(directory, "p44_nested_panic") != NULL;
    bool c1_parameter_forward_roots = strstr(directory, "p44c_requires_parameter_forward") != NULL;
    if (ok && (callback_roots || callback_inout_roots || method_roots || callable_hole_roots
            || p44_terminal_roots || c1_parameter_forward_roots)) {
        for (size_t i = 0; i < ir.callable_count; ++i) {
            if (ir.callables[i].kind != SOL_IR_CALLABLE_FUNCTION) continue;
            if (!strcmp(ir.callables[i].name, "launch")) ids[0] = i;
            else if (!strcmp(ir.callables[i].name,
                (callback_inout_roots || method_roots || c1_parameter_forward_roots) ? "fail"
                    : callable_hole_roots ? "answer" : p44_terminal_roots ? "inner" : "increment")) ids[1] = i;
        }
        if (ids[0] != SIZE_MAX && ids[1] != SIZE_MAX && ids[0] != ids[1]) {
            roots[root_count++] = (SolMirProgramRoot){ids[0], method_failure_entry_roots
                ? SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE : SOL_MIR_PROGRAM_ROOT_ENTRY};
            roots[root_count++] = (SolMirProgramRoot){ids[1], method_failure_entry_roots
                ? SOL_MIR_PROGRAM_ROOT_ENTRY : SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
        }
    }
    if (ok && !callback_roots && !callback_inout_roots && !method_roots && !callable_hole_roots
        && !p44_terminal_roots && !c1_parameter_forward_roots)
        for (size_t i = 0; i < ir.callable_count; ++i) {
        if (ir.callables[i].kind != SOL_IR_CALLABLE_FUNCTION) continue;
        if (!strcmp(ir.callables[i].name, "first")) ids[0] = i;
        else if (!strcmp(ir.callables[i].name, "second")) ids[1] = i;
        else if (!strcmp(ir.callables[i].name, "launch") && root_count == 0) {
            ids[0] = i;
            roots[root_count++] = (SolMirProgramRoot){i, SOL_MIR_PROGRAM_ROOT_ENTRY};
            continue;
        } else continue;
        roots[root_count++] = (SolMirProgramRoot){i, !strcmp(ir.callables[i].name, "first")
            ? SOL_MIR_PROGRAM_ROOT_ENTRY : SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
        }
    if (ok && reverse && root_count == 2) { SolMirProgramRoot swap = roots[0]; roots[0] = roots[1]; roots[1] = swap; }
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    if (ok) ok = ((root_count == 2 && ids[0] != SIZE_MAX && ids[1] != SIZE_MAX && ids[0] != ids[1])
            || (root_count == 1 && ids[0] != SIZE_MAX))
        && sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&ir, roots, root_count, NULL,
            0, &target, NULL}, &concrete, &diagnostics) == SOL_MIR_CONCRETE_BUILD_SUCCEEDED
        && sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&concrete, NULL},
            &conventions, &diagnostics) == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        && sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&conventions, NULL},
            &values, &diagnostics) == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED
        && sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&conventions, &values, NULL},
            &cleanup, &diagnostics) == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED
        && sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&conventions, &values,
            &cleanup, NULL}, &host, &diagnostics) == SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED
        && sol_mir_runtime_handler_abi_build(&(SolMirRuntimeHandlerAbiBuildRequest){&conventions,
            &values, &cleanup, &host, NULL}, &handler, &diagnostics)
            == SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_SUCCEEDED
        && sol_mir_runtime_lowered_program_build(&(SolMirRuntimeLoweredProgramBuildRequest){&conventions,
            &values, &cleanup, &host, &handler, NULL}, &lowered, &diagnostics)
            == SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory, limits},
            output, &diagnostics) == expected;
    if (!ok) sol_diagnostics_render_human(stderr, &package.source, &diagnostics);
    sol_mir_runtime_lowered_program_free(&lowered); sol_mir_runtime_handler_abi_free(&handler);
    sol_mir_runtime_host_abi_free(&host); sol_mir_runtime_cleanup_free(&cleanup);
    sol_mir_runtime_values_free(&values); sol_mir_runtime_conventions_free(&conventions);
    sol_mir_concrete_program_free(&concrete); sol_package_free(&package); sol_ir_free(&ir);
    sol_contract_table_free(&contracts); sol_effect_table_free(&effects); sol_type_table_free(&types);
    sol_hir_module_free(&hir); sol_diagnostics_free(&diagnostics);
    return ok;
}

/* The propagation fixture needs one root at a time: its five entries isolate
 * the success/residual paths while still deriving the complete P2/P3.6 owner
 * from source.  Keep this separate from the two-root order probe above. */
static bool build_named_root(const char *directory, const char *entry,
    SolWasmRepresentedOutput *output, const SolWasmRepresentedLimits *limits,
    SolWasmRepresentedResult expected) {
    SolDiagnostics diagnostics; SolHirModule hir; SolTypeTable types; SolEffectTable effects;
    SolContractTable contracts; SolIr ir; SolPackage package;
    SolMirConcreteProgram concrete; SolMirRuntimeConventions conventions; SolMirRuntimeValues values;
    SolMirRuntimeCleanup cleanup; SolMirRuntimeHostAbi host; SolMirRuntimeHandlerAbi handler;
    SolMirRuntimeLoweredProgram lowered;
    sol_diagnostics_init(&diagnostics); sol_hir_module_init(&hir); sol_type_table_init(&types);
    sol_effect_table_init(&effects); sol_contract_table_init(&contracts); sol_ir_init(&ir);
    sol_package_init(&package); sol_mir_concrete_program_init(&concrete);
    sol_mir_runtime_conventions_init(&conventions); sol_mir_runtime_values_init(&values);
    sol_mir_runtime_cleanup_init(&cleanup); sol_mir_runtime_host_abi_init(&host);
    sol_mir_runtime_handler_abi_init(&handler); sol_mir_runtime_lowered_program_init(&lowered);
    char error[256]; bool ok = entry != NULL
        && sol_package_load_directory(&package, directory, &diagnostics, error, sizeof error);
    SolHirFileScope scope = {0};
    if (ok) scope = (SolHirFileScope){package.files[0].module_name, package.files[0].import_start,
        package.files[0].import_count, package.files[0].item_start, package.files[0].item_count};
    if (ok) ok = sol_hir_lower_scoped(&package.source, &package.syntax, &scope, 1, &hir, &diagnostics)
        && sol_type_check(&package.source, &package.syntax, &hir, &types, &diagnostics)
        && sol_effect_check(&package.source, &package.syntax, &hir, &types, &effects, &diagnostics)
        && sol_contract_lower(&package.source, &package.syntax, &hir, &types, &effects, &contracts,
            &diagnostics)
        && sol_ir_lower_scoped(&package.source, &package.syntax, &hir, &types, &effects, &contracts,
            package.files, 1, &ir, &diagnostics);
    SolIrCallableId callable = SOL_IR_NONE, launch = SOL_IR_NONE;
    if (ok) for (size_t i = 0; i < ir.callable_count; ++i)
        if (ir.callables[i].kind == SOL_IR_CALLABLE_FUNCTION) {
            if (!strcmp(ir.callables[i].name, entry)) callable = i;
            if (!strcmp(ir.callables[i].name, "launch")) launch = i;
        }
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    bool internal_fail = callable != SOL_IR_NONE && !strcmp(entry, "fail")
        && !ir.definitions[ir.callables[callable].owner].is_entrypoint;
    SolMirProgramRoot roots[2] = {{callable, SOL_MIR_PROGRAM_ROOT_ENTRY},
        {SOL_IR_NONE, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE}};
    size_t root_count = 1;
    if (internal_fail) {
        roots[0] = (SolMirProgramRoot){launch, SOL_MIR_PROGRAM_ROOT_ENTRY};
        roots[1] = (SolMirProgramRoot){callable, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
        root_count = 2;
    }
    if (ok) ok = callable != SOL_IR_NONE && (!internal_fail || launch != SOL_IR_NONE)
        && sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&ir, roots, root_count, NULL,
            0, &target, NULL}, &concrete, &diagnostics)
            == SOL_MIR_CONCRETE_BUILD_SUCCEEDED
        && sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&concrete, NULL},
            &conventions, &diagnostics) == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        && sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&conventions, NULL},
            &values, &diagnostics) == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED
        && sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&conventions, &values, NULL},
            &cleanup, &diagnostics) == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED
        && sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&conventions, &values,
            &cleanup, NULL}, &host, &diagnostics) == SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED
        && sol_mir_runtime_handler_abi_build(&(SolMirRuntimeHandlerAbiBuildRequest){&conventions,
            &values, &cleanup, &host, NULL}, &handler, &diagnostics)
            == SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_SUCCEEDED
        && sol_mir_runtime_lowered_program_build(&(SolMirRuntimeLoweredProgramBuildRequest){&conventions,
            &values, &cleanup, &host, &handler, NULL}, &lowered, &diagnostics)
            == SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory, limits},
            output, &diagnostics) == expected;
    if (!ok) sol_diagnostics_render_human(stderr, &package.source, &diagnostics);
    sol_mir_runtime_lowered_program_free(&lowered); sol_mir_runtime_handler_abi_free(&handler);
    sol_mir_runtime_host_abi_free(&host); sol_mir_runtime_cleanup_free(&cleanup);
    sol_mir_runtime_values_free(&values); sol_mir_runtime_conventions_free(&conventions);
    sol_mir_concrete_program_free(&concrete); sol_package_free(&package); sol_ir_free(&ir);
    sol_contract_table_free(&contracts); sol_effect_table_free(&effects); sol_type_table_free(&types);
    sol_hir_module_free(&hir); sol_diagnostics_free(&diagnostics);
    return ok;
}

/* Keep a complete source-derived P2/P3.6 owner live for propagation-specific
 * backend preflight tests.  Unlike build_named_root, this deliberately exposes
 * no synthetic layout or hand-written cleanup packet. */
typedef struct {
    SolDiagnostics diagnostics; SolHirModule hir; SolTypeTable types; SolEffectTable effects;
    SolContractTable contracts; SolIr ir; SolPackage package; SolMirConcreteProgram concrete;
    SolMirRuntimeConventions conventions; SolMirRuntimeValues values; SolMirRuntimeCleanup cleanup;
    SolMirRuntimeHostAbi host; SolMirRuntimeHandlerAbi handler; SolMirRuntimeLoweredProgram lowered;
} PropagationPipeline;

static void propagation_pipeline_init(PropagationPipeline *pipeline) {
    sol_diagnostics_init(&pipeline->diagnostics); sol_hir_module_init(&pipeline->hir);
    sol_type_table_init(&pipeline->types); sol_effect_table_init(&pipeline->effects);
    sol_contract_table_init(&pipeline->contracts); sol_ir_init(&pipeline->ir);
    sol_package_init(&pipeline->package); sol_mir_concrete_program_init(&pipeline->concrete);
    sol_mir_runtime_conventions_init(&pipeline->conventions);
    sol_mir_runtime_values_init(&pipeline->values); sol_mir_runtime_cleanup_init(&pipeline->cleanup);
    sol_mir_runtime_host_abi_init(&pipeline->host);
    sol_mir_runtime_handler_abi_init(&pipeline->handler);
    sol_mir_runtime_lowered_program_init(&pipeline->lowered);
}

static void propagation_pipeline_free(PropagationPipeline *pipeline) {
    sol_mir_runtime_lowered_program_free(&pipeline->lowered);
    sol_mir_runtime_handler_abi_free(&pipeline->handler); sol_mir_runtime_host_abi_free(&pipeline->host);
    sol_mir_runtime_cleanup_free(&pipeline->cleanup); sol_mir_runtime_values_free(&pipeline->values);
    sol_mir_runtime_conventions_free(&pipeline->conventions);
    sol_mir_concrete_program_free(&pipeline->concrete); sol_package_free(&pipeline->package);
    sol_ir_free(&pipeline->ir); sol_contract_table_free(&pipeline->contracts);
    sol_effect_table_free(&pipeline->effects); sol_type_table_free(&pipeline->types);
    sol_hir_module_free(&pipeline->hir); sol_diagnostics_free(&pipeline->diagnostics);
}

static bool propagation_pipeline_build_named_with_import(PropagationPipeline *pipeline,
    const char *directory, const char *entry, bool pair_method_failure, const char *import_name,
    SolMirProgramRootKind root_kind) {
    char error[256];
    bool ok = sol_package_load_directory(&pipeline->package, directory, &pipeline->diagnostics, error,
        sizeof error);
    SolHirFileScope scope = {0};
    if (ok) scope = (SolHirFileScope){pipeline->package.files[0].module_name,
        pipeline->package.files[0].import_start, pipeline->package.files[0].import_count,
        pipeline->package.files[0].item_start, pipeline->package.files[0].item_count};
    if (ok) ok = sol_hir_lower_scoped(&pipeline->package.source, &pipeline->package.syntax, &scope,
        1, &pipeline->hir, &pipeline->diagnostics)
        && sol_type_check(&pipeline->package.source, &pipeline->package.syntax, &pipeline->hir,
            &pipeline->types, &pipeline->diagnostics)
        && sol_effect_check(&pipeline->package.source, &pipeline->package.syntax, &pipeline->hir,
            &pipeline->types, &pipeline->effects, &pipeline->diagnostics)
        && sol_contract_lower(&pipeline->package.source, &pipeline->package.syntax, &pipeline->hir,
            &pipeline->types, &pipeline->effects, &pipeline->contracts, &pipeline->diagnostics)
        && sol_ir_lower_scoped(&pipeline->package.source, &pipeline->package.syntax, &pipeline->hir,
            &pipeline->types, &pipeline->effects, &pipeline->contracts, pipeline->package.files, 1,
            &pipeline->ir, &pipeline->diagnostics);
    SolIrCallableId callable = SOL_IR_NONE, companion = SOL_IR_NONE, imported = SOL_IR_NONE;
    if (ok && entry != NULL) for (size_t i = 0; i < pipeline->ir.callable_count; ++i) {
        if (import_name != NULL && !strcmp(pipeline->ir.callables[i].name, import_name)
            && pipeline->ir.callables[i].kind == SOL_IR_CALLABLE_CAPABILITY) imported = i;
        if (pipeline->ir.callables[i].kind != SOL_IR_CALLABLE_FUNCTION) continue;
        if (!strcmp(pipeline->ir.callables[i].name, entry)) callable = i;
        else if ((!strcmp(entry, "fail") && !strcmp(pipeline->ir.callables[i].name, "launch"))
            || (!strcmp(entry, "launch") && !strcmp(pipeline->ir.callables[i].name, "fail")))
            companion = i;
    }
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    bool paired_method_failure = pair_method_failure && ((entry != NULL && !strcmp(entry, "fail")
            && strstr(directory, "p43_method_failure_entry") != NULL)
        || (entry != NULL && !strcmp(entry, "launch")
            && (strstr(directory, "p43_method_prereq") != NULL
                || strstr(directory, "p44e_depth_method_inout") != NULL
                || strstr(directory, "p44e_depth_callback_inout") != NULL
                || strstr(directory, "p44e_depth_pending") != NULL
                || strstr(directory, "p44c_requires_") != NULL)));
    SolMirProgramRoot roots[2] = {{callable, root_kind},
        {companion, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE}};
    size_t root_count = paired_method_failure ? 2 : 1;
    if (paired_method_failure && !strcmp(entry, "fail")) {
        roots[0] = (SolMirProgramRoot){companion, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
        roots[1] = (SolMirProgramRoot){callable, SOL_MIR_PROGRAM_ROOT_ENTRY};
    } else if (paired_method_failure) {
        roots[1] = (SolMirProgramRoot){companion, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    }
    size_t import_count = import_name == NULL ? 0 : 1;
    return ok && callable != SOL_IR_NONE && (!paired_method_failure || companion != SOL_IR_NONE)
        && (import_name == NULL || imported != SOL_IR_NONE)
        && sol_mir_concrete_program_build(
        &(SolMirConcreteBuildRequest){&pipeline->ir, roots, root_count,
            import_count == 0 ? NULL : &imported, import_count, &target, NULL},
        &pipeline->concrete, &pipeline->diagnostics) == SOL_MIR_CONCRETE_BUILD_SUCCEEDED
        && sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){
            &pipeline->concrete, NULL}, &pipeline->conventions, &pipeline->diagnostics)
            == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        && sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&pipeline->conventions,
            NULL}, &pipeline->values, &pipeline->diagnostics) == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED
        && sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&pipeline->conventions,
            &pipeline->values, NULL}, &pipeline->cleanup, &pipeline->diagnostics)
            == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED
        && sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){
            &pipeline->conventions, &pipeline->values, &pipeline->cleanup, NULL}, &pipeline->host,
            &pipeline->diagnostics) == SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED
        && sol_mir_runtime_handler_abi_build(&(SolMirRuntimeHandlerAbiBuildRequest){
            &pipeline->conventions, &pipeline->values, &pipeline->cleanup, &pipeline->host, NULL},
            &pipeline->handler, &pipeline->diagnostics) == SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_SUCCEEDED
        && sol_mir_runtime_lowered_program_build(&(SolMirRuntimeLoweredProgramBuildRequest){
            &pipeline->conventions, &pipeline->values, &pipeline->cleanup, &pipeline->host,
            &pipeline->handler, NULL}, &pipeline->lowered, &pipeline->diagnostics)
              == SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED;
}

static bool propagation_pipeline_build_named(PropagationPipeline *pipeline, const char *directory,
    const char *entry, bool pair_method_failure) {
    return propagation_pipeline_build_named_with_import(pipeline, directory, entry,
        pair_method_failure, NULL, SOL_MIR_PROGRAM_ROOT_ENTRY);
}

static bool propagation_pipeline_build_explicit_internal_root(PropagationPipeline *pipeline,
    const char *directory, const char *entry) {
    return propagation_pipeline_build_named_with_import(pipeline, directory, entry, false, NULL,
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE);
}

static bool propagation_pipeline_build(PropagationPipeline *pipeline, const char *directory) {
    return propagation_pipeline_build_named(pipeline, directory, "launch", false);
}

typedef struct {
    SolMirTerminatorKind kind;
    SolMirRuntimeFailureOriginKind origin;
    size_t row, image, block, event, site, transition, action;
    uint32_t record, start, end, mask;
    int32_t action_target;
    bool source_reachable;
} P44TerminalOwner;

static const char *p44_symbol_for_image(const PropagationPipeline *pipeline, size_t image) {
    const SolMirLinkage *linkage = &pipeline->concrete.linkage;
    const char *symbol = NULL;
    for (size_t i = 0; i < linkage->callable_count; ++i)
        if (linkage->callables[i].instance == image) {
            if (symbol != NULL) return NULL;
            symbol = linkage->callables[i].symbol.bytes;
        }
    return symbol;
}

static bool p44_terminal_owner_controls(const char *directory, const P44TerminalOwner *want) {
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    SolWasmRepresentedOutput baseline, output;
    sol_wasm_represented_output_init(&baseline); sol_wasm_represented_output_init(&output);
    bool ok = want != NULL && propagation_pipeline_build(&pipeline, directory)
        && sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL)
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory,
            NULL}, &baseline, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
    SolMirRuntimeLoweredImageTerminator *row = ok && want->row < pipeline.lowered.image_terminator_count
        ? &pipeline.lowered.image_terminators[want->row] : NULL;
    SolMirRuntimeCleanupEvent *event = row != NULL && row->cleanup_event < pipeline.cleanup.event_count
        ? &pipeline.cleanup.events[row->cleanup_event] : NULL;
    SolMirRuntimeCleanupTransition *transition = event != NULL && want->transition < pipeline.cleanup.transition_count
        ? &pipeline.cleanup.transitions[want->transition] : NULL;
    SolMirRuntimeCleanupAction *action = transition != NULL && want->action < pipeline.cleanup.action_count
        ? &pipeline.cleanup.actions[want->action] : NULL;
    SolMirRuntimeFailureSite *site = row != NULL && row->failure_site < pipeline.conventions.failure_site_count
        ? &pipeline.conventions.failure_sites[row->failure_site] : NULL;
    const char *symbol = row == NULL ? NULL : p44_symbol_for_image(&pipeline, row->image);
    ProvenanceRecord record = {0};
    if (ok) ok = row != NULL && event != NULL && transition != NULL && action != NULL && site != NULL
        && row->state == SOL_MIR_RUNTIME_LOWERED_PRESENT && row->kind == want->kind
        && row->image == want->image && row->block == want->block && row->cleanup_event == want->event
        && row->failure_site == want->site && site->origin_kind == want->origin
        && site->owner == want->image && site->block == want->block
        && site->instruction == SOL_MIR_RUNTIME_NONE && site->source.file == 0
        && site->source.start == want->start && site->source.end == want->end
        && site->allowed_codes == want->mask && event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
        && event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
        && event->origin == SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT && event->owner == want->image
        && event->block == want->block && event->operation == SOL_MIR_RUNTIME_NONE
        && event->semantic_site == SOL_MIR_RUNTIME_NONE && event->inherited_failure_site == want->site
        && event->supplemental_site == SOL_MIR_RUNTIME_NONE && event->transitions.offset == want->transition
        && event->transitions.count == 1 && event->actions.offset == want->action && event->actions.count == 1
        && transition->event == want->event && transition->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
        && transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE
        && transition->source_edge == SOL_MIR_RUNTIME_NONE && transition->destination == SOL_MIR_RUNTIME_NONE
        && transition->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31
        && transition->failure_site == want->site && transition->failure_mask == want->mask
        && transition->actions.offset == want->action && transition->actions.count == 1
        && action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE
        && action->flags == SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY
        && action->target == (size_t)want->action_target && action->recipe == SOL_MIR_RECIPE_NONE
        && action->drop_path == SOL_MIR_RUNTIME_NONE && provenance_record(&baseline.bytes, want->record, &record)
        && symbol != NULL && record.tag == 3 && record.kind == want->origin
        && bytes_equal(record.symbol, record.symbol_count, symbol)
        && bytes_equal(record.path, record.path_count, "main.sol")
        && record.start == want->start && record.end == want->end
        && record.ordinal == want->site;
    char entry[256];
    if (ok) ok = entry_symbol(&baseline.bytes, entry, sizeof entry)
        && (want->source_reachable
            ? invoke_named(&baseline.bytes, entry, 0, (int32_t)(__builtin_ctz(want->mask) + 1),
                (int32_t)want->record)
            : invoke_named(&baseline.bytes, entry, 7, 0, 0));
#define CHECK_P44_OWNER_REJECT(cleanup_valid, edit, restore) do { \
    edit; pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
    sol_wasm_represented_output_init(&output); \
    ok = ok && (sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL) == (cleanup_valid)) \
        && !sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL) \
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, \
            &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE \
        && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage); \
    sol_wasm_represented_output_free(&output); restore; \
    pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
    ok = ok && sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL) \
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL); \
} while (0)
    if (ok) {
        size_t saved = event->inherited_failure_site;
        CHECK_P44_OWNER_REJECT(false, event->inherited_failure_site = SOL_MIR_RUNTIME_NONE,
            event->inherited_failure_site = saved);
        uint32_t mask = transition->failure_mask;
        CHECK_P44_OWNER_REJECT(false, transition->failure_mask = 0, transition->failure_mask = mask);
        saved = row->cleanup_event;
        CHECK_P44_OWNER_REJECT(true, row->cleanup_event = SOL_MIR_RUNTIME_NONE, row->cleanup_event = saved);
        SolMirRuntimeCleanupOutcome outcome = transition->outcome;
        CHECK_P44_OWNER_REJECT(false, transition->outcome = SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL,
            transition->outcome = outcome);
        SolMirRuntimeCleanupEdgeRole role = transition->edge_role;
        CHECK_P44_OWNER_REJECT(false, transition->edge_role = SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO,
            transition->edge_role = role);
        SolMirRuntimeCleanupFailureSource source = transition->failure_source;
        CHECK_P44_OWNER_REJECT(false, transition->failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE,
            transition->failure_source = source);
        saved = transition->failure_site;
        CHECK_P44_OWNER_REJECT(false, transition->failure_site = SOL_MIR_RUNTIME_NONE,
            transition->failure_site = saved);
        SolMirRuntimeSlice actions = transition->actions;
        CHECK_P44_OWNER_REJECT(false, transition->actions.count = 0, transition->actions = actions);
        SolMirRuntimeCleanupAction saved_action = *action;
        CHECK_P44_OWNER_REJECT(false, action->kind = SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY,
            *action = saved_action);
        CHECK_P44_OWNER_REJECT(false, action->target = SOL_MIR_RUNTIME_NONE, *action = saved_action);
    }
#undef CHECK_P44_OWNER_REJECT
    if (ok) {
        sol_wasm_represented_output_init(&output);
        ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory,
            NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && output.bytes.count == baseline.bytes.count
            && memcmp(output.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
            && usage_equal(&output.usage, &baseline.usage);
        sol_wasm_represented_output_free(&output);
    }
    sol_wasm_represented_output_free(&baseline); propagation_pipeline_free(&pipeline);
    return ok;
}

/* Test-only module shape: one source entry is instantiated once. The private
 * probes share the entry reset builder but never run source code themselves. */
static bool p44_same_instance_packet_reset(const char *directory, int32_t panic_site,
    const uint8_t *detail, size_t detail_count) {
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
    WasmInstance instance = {0}; char entry[256];
    sol_wasm_represented_test_p44_packet_reset_probe(true);
    bool ok = propagation_pipeline_build(&pipeline, directory)
        && pipeline.conventions.entry_count == 1
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory,
            NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && sol_wasm_represented_validate(&output.bytes) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry)
        && wasm_instance_open(&output.bytes, entry, &instance)
        && wasm_instance_call(&instance, 0, 1, panic_site)
        && wasm_instance_panic_detail(&instance, detail, detail_count)
        && wasm_instance_call_named(&instance,
            SOL_WASM_REPRESENTED_TEST_P44_PACKET_RESET_SUCCESS_EXPORT, 0, 0, 0)
        && wasm_instance_panic_detail(&instance, (const uint8_t *)"", 0)
        && wasm_instance_call(&instance, 0, 1, panic_site)
        && wasm_instance_panic_detail(&instance, detail, detail_count)
        && wasm_instance_call_named(&instance,
            SOL_WASM_REPRESENTED_TEST_P44_PACKET_RESET_NONPANIC_EXPORT, 0, 2, 0)
        && wasm_instance_panic_detail(&instance, (const uint8_t *)"", 0);
    wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_p44_packet_reset_probe(false);
    propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44_bytes_contain(const SolWasmBackendBytes *bytes, const char *needle) {
    size_t count = bytes == NULL || needle == NULL ? 0 : strlen(needle);
    if (bytes == NULL || bytes->bytes == NULL || count == 0 || count > bytes->count) return false;
    for (size_t i = 0; i <= bytes->count - count; ++i)
        if (memcmp(bytes->bytes + i, needle, count) == 0) return true;
    return false;
}

/* The sole source Text allocation has two logical requests and 34 logical
 * bytes. Its supplemental P3.3 record, not the panic terminal record, owns
 * both one-below runtime-quota failures. */
static bool p44_panic_runtime_quota_controls(const char *directory) {
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    SolWasmRepresentedOutput exact, limited;
    sol_wasm_represented_output_init(&exact); sol_wasm_represented_output_init(&limited);
    bool ok = propagation_pipeline_build(&pipeline, directory) && pipeline.conventions.entry_count == 1;
    size_t instruction = SOL_MIR_RUNTIME_LOWERED_NONE;
    if (ok) for (size_t i = 0; i < pipeline.concrete.materialization.instruction_count; ++i)
        if (pipeline.concrete.materialization.instructions[i].kind == SOL_MIR_INST_CONST_TEXT) {
            if (instruction != SOL_MIR_RUNTIME_LOWERED_NONE) ok = false;
            instruction = i;
        }
    SolMirRuntimeLoweredImageInstruction *row = instruction < pipeline.lowered.image_instruction_count
        ? &pipeline.lowered.image_instructions[instruction] : NULL;
    SolMirRuntimeCleanupEvent *event = row != NULL && row->cleanup_event < pipeline.cleanup.event_count
        ? &pipeline.cleanup.events[row->cleanup_event] : NULL;
    SolMirRuntimeCleanupSupplementalSite *site = event != NULL
        && event->supplemental_site < pipeline.cleanup.supplemental_site_count
        ? &pipeline.cleanup.supplemental_sites[event->supplemental_site] : NULL;
    SolWasmRepresentedLimits limits = sol_wasm_represented_default_limits();
    limits.max_allocation_requests = 2; limits.max_allocation_bytes = 34;
    if (ok) ok = instruction != SOL_MIR_RUNTIME_LOWERED_NONE && row != NULL && event != NULL
        && site != NULL && row->state == SOL_MIR_RUNTIME_LOWERED_PRESENT
        && event->supplemental_site < pipeline.cleanup.supplemental_site_count
        && site->event == row->cleanup_event && site->allowed_codes
            == ((UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED - 1))
                | (UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_ALLOCATION_LIMIT - 1)))
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory,
            &limits}, &exact, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
    uint32_t allocation_record = 0; size_t allocation_records = 0; ProvenanceRecord record = {0};
    const char *symbol = event == NULL ? NULL : p44_symbol_for_image(&pipeline, event->owner);
    if (ok) {
        for (uint32_t i = 1; i <= exact.usage.provenance_records; ++i) {
            ProvenanceRecord candidate;
            if (!provenance_record(&exact.bytes, i, &candidate)) { ok = false; break; }
            if (candidate.tag == 4 && candidate.start == site->source.start
                && candidate.end == site->source.end) {
                ++allocation_records; allocation_record = i; record = candidate;
            }
        }
        ok = ok && allocation_records == 1 && allocation_record == 4 && record.kind == 0
            && bytes_equal(record.path, record.path_count, "main.sol")
            && record.start == site->source.start && record.end == site->source.end
            && record.ordinal == 0 && symbol != NULL
            && bytes_equal(record.symbol, record.symbol_count, symbol);
    }
    char entry[256];
    if (ok) ok = entry_symbol(&exact.bytes, entry, sizeof entry)
        && invoke_named(&exact.bytes, entry, 0, 1, 3);
    const struct { uint64_t requests, bytes; } one_below[] = {{1, 34}, {2, 33}};
    for (size_t i = 0; ok && i < sizeof one_below / sizeof *one_below; ++i) {
        SolWasmRepresentedLimits capped = sol_wasm_represented_default_limits();
        capped.max_allocation_requests = one_below[i].requests;
        capped.max_allocation_bytes = one_below[i].bytes;
        sol_wasm_represented_output_init(&limited);
        ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory,
            &capped}, &limited, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
        WasmInstance instance = {0};
        if (ok) ok = wasm_instance_open(&limited.bytes, entry, &instance)
            && wasm_instance_call(&instance, 0, 5, 4)
            && wasm_instance_panic_detail(&instance, (const uint8_t *)"", 0)
            /* A second invocation is a same-instance retry after the wrapper reset. */
            && wasm_instance_call(&instance, 0, 5, 4)
            && wasm_instance_panic_detail(&instance, (const uint8_t *)"", 0);
        wasm_instance_close(&instance); sol_wasm_represented_output_free(&limited);
    }
    sol_wasm_represented_output_free(&exact); propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44_packet_reset_probe_authorization(const char *directory) {
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    SolWasmRepresentedOutput production, probe;
    sol_wasm_represented_output_init(&production); sol_wasm_represented_output_init(&probe);
    sol_wasm_represented_test_p44_packet_reset_probe(false);
    bool ok = propagation_pipeline_build(&pipeline, directory)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory,
            NULL}, &production, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && !p44_bytes_contain(&production.bytes,
            SOL_WASM_REPRESENTED_TEST_P44_PACKET_RESET_SUCCESS_EXPORT)
        && !p44_bytes_contain(&production.bytes,
            SOL_WASM_REPRESENTED_TEST_P44_PACKET_RESET_NONPANIC_EXPORT)
        && sol_wasm_represented_validate(&production.bytes) == SOL_WASM_REPRESENTED_OK;
    sol_wasm_represented_test_p44_packet_reset_probe(true);
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
        directory, NULL}, &probe, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && p44_bytes_contain(&probe.bytes, SOL_WASM_REPRESENTED_TEST_P44_PACKET_RESET_SUCCESS_EXPORT)
        && p44_bytes_contain(&probe.bytes, SOL_WASM_REPRESENTED_TEST_P44_PACKET_RESET_NONPANIC_EXPORT)
        && sol_wasm_represented_validate(&probe.bytes) == SOL_WASM_REPRESENTED_OK;
    char entry[256]; WasmInstance instance = {0};
    if (ok) ok = entry_symbol(&probe.bytes, entry, sizeof entry)
        && wasm_instance_open(&probe.bytes, entry, &instance)
        && wasm_instance_call_named(&instance,
            SOL_WASM_REPRESENTED_TEST_P44_PACKET_RESET_SUCCESS_EXPORT, 0, 0, 0);
    wasm_instance_close(&instance);
    sol_wasm_represented_test_p44_packet_reset_probe(false);
    if (ok) ok = sol_wasm_represented_validate(&probe.bytes) == SOL_WASM_REPRESENTED_INVALID_INPUT;
    sol_wasm_represented_output_free(&probe); sol_wasm_represented_output_free(&production);
    propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44_terminal_multiroot(const char *directory) {
    SolWasmRepresentedOutput forward, reverse, repeat, relocated;
    size_t forward_ids[2], reverse_ids[2], repeat_ids[2], relocated_ids[2];
    sol_wasm_represented_output_init(&forward); sol_wasm_represented_output_init(&reverse);
    sol_wasm_represented_output_init(&repeat); sol_wasm_represented_output_init(&relocated);
    bool ok = build_multiroot(directory, false, &forward, forward_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && build_multiroot(directory, true, &reverse, reverse_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && build_multiroot(directory, false, &repeat, repeat_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && forward_ids[0] != forward_ids[1]
        && forward_ids[0] == reverse_ids[0] && forward_ids[1] == reverse_ids[1]
        && forward_ids[0] == repeat_ids[0] && forward_ids[1] == repeat_ids[1]
        && forward.bytes.count == reverse.bytes.count && forward.bytes.count == repeat.bytes.count
        && memcmp(forward.bytes.bytes, reverse.bytes.bytes, forward.bytes.count) == 0
        && memcmp(forward.bytes.bytes, repeat.bytes.bytes, forward.bytes.count) == 0
        && usage_equal(&forward.usage, &reverse.usage) && usage_equal(&forward.usage, &repeat.usage);
    uint8_t forward_hash[32], reverse_hash[32], repeat_hash[32];
    if (ok) { sha256(forward.bytes.bytes, forward.bytes.count, forward_hash);
        sha256(reverse.bytes.bytes, reverse.bytes.count, reverse_hash);
        sha256(repeat.bytes.bytes, repeat.bytes.count, repeat_hash);
        ok = memcmp(forward_hash, reverse_hash, sizeof forward_hash) == 0
            && memcmp(forward_hash, repeat_hash, sizeof forward_hash) == 0; }
    char entry[256];
    if (ok) ok = entry_symbol(&forward.bytes, entry, sizeof entry)
        && invoke_named(&forward.bytes, entry, 0, 1, 4)
        && invoke_named(&reverse.bytes, entry, 0, 1, 4);
    char relocation[512], source[768], destination[768];
    (void)mkdir(SOL_TEST_BINARY_DIR, 0700);
    (void)snprintf(relocation, sizeof relocation, "%s/p44_nested_panic_relocated", SOL_TEST_BINARY_DIR);
    (void)mkdir(relocation, 0700);
    (void)snprintf(source, sizeof source, "%s/main.sol", directory);
    (void)snprintf(destination, sizeof destination, "%s/main.sol", relocation);
    FILE *input = fopen(source, "rb"), *copied = fopen(destination, "wb");
    if (input == NULL || copied == NULL) ok = false;
    if (input != NULL && copied != NULL) {
        uint8_t buffer[256]; size_t count;
        while ((count = fread(buffer, 1, sizeof buffer, input)) != 0)
            if (fwrite(buffer, 1, count, copied) != count) ok = false;
    }
    if (input != NULL) fclose(input); if (copied != NULL) fclose(copied);
    if (ok) ok = build_multiroot(relocation, false, &relocated, relocated_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && relocated_ids[0] == forward_ids[0]
        && relocated_ids[1] == forward_ids[1] && relocated.bytes.count == forward.bytes.count
        && memcmp(relocated.bytes.bytes, forward.bytes.bytes, forward.bytes.count) == 0
        && usage_equal(&relocated.usage, &forward.usage) && invoke_named(&relocated.bytes, entry, 0, 1, 4);
    sol_wasm_represented_output_free(&relocated); sol_wasm_represented_output_free(&repeat);
    sol_wasm_represented_output_free(&reverse); sol_wasm_represented_output_free(&forward);
    return ok;
}

static bool callable_hole_supplemental_record(const PropagationPipeline *pipeline,
    const SolWasmRepresentedOutput *output, size_t instruction, int32_t *record_out,
    SolMirRuntimeSource *source_out) {
    const SolMirRuntimeLoweredProgram *lowered = &pipeline->lowered;
    const SolMirRuntimeCleanup *cleanup = &pipeline->cleanup;
    if (instruction >= lowered->image_instruction_count || instruction >= pipeline->concrete.materialization.instruction_count
        || lowered->image_instructions[instruction].cleanup_event >= cleanup->event_count) return false;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[
        lowered->image_instructions[instruction].cleanup_event];
    if (event->supplemental_site >= cleanup->supplemental_site_count
        || cleanup->supplemental_sites[event->supplemental_site].event
            != lowered->image_instructions[instruction].cleanup_event) return false;
    const SolMirRuntimeCleanupSupplementalSite *site = &cleanup->supplemental_sites[
        event->supplemental_site];
    size_t found = 0; uint32_t record = 0;
    for (uint32_t i = 1; i <= output->usage.provenance_records; ++i) {
        ProvenanceRecord candidate;
        if (!provenance_record(&output->bytes, i, &candidate)) return false;
        if (candidate.tag == 4 && candidate.start == site->source.start
            && candidate.end == site->source.end) { ++found; record = i; }
    }
    if (found != 1 || record > INT32_MAX) return false;
    *record_out = (int32_t)record;
    *source_out = site->source;
    return true;
}

/* The three runtime allocations in the moved callable fixture are independently
 * present in P3: closure header, Text copy header/payload, then Pair object.
 * Derive their demand and canonical packet records from that owner before
 * constraining the emitted module. */
static bool callable_hole_runtime_controls(const char *directory) {
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    bool ok = propagation_pipeline_build(&pipeline, directory);
    const SolMirMaterialization *m = &pipeline.concrete.materialization;
    size_t function_value = SOL_MIR_RUNTIME_LOWERED_NONE;
    size_t text = SOL_MIR_RUNTIME_LOWERED_NONE, construct = SOL_MIR_RUNTIME_LOWERED_NONE;
    const SolMirOperationConstructPlan *pair = NULL;
    if (ok) for (size_t i = 0; i < m->instruction_count; ++i) {
        const SolMirMaterializedInstruction *instruction = &m->instructions[i];
        if (instruction->kind == SOL_MIR_INST_FUNCTION_VALUE) {
            if (function_value != SOL_MIR_RUNTIME_LOWERED_NONE) ok = false;
            function_value = i;
        } else if (instruction->kind == SOL_MIR_INST_CONST_TEXT) {
            if (text != SOL_MIR_RUNTIME_LOWERED_NONE) ok = false;
            text = i;
        } else if (instruction->kind == SOL_MIR_INST_CONSTRUCT) {
            for (size_t p = 0; p < pipeline.concrete.operations.constructor_count; ++p)
                if (pipeline.concrete.operations.constructors[p].instruction == i) {
                    if (pair != NULL) ok = false;
                    pair = &pipeline.concrete.operations.constructors[p]; construct = i;
                }
        }
    }
    SolMirRecipeId callable_recipe = function_value < m->instruction_count
        && m->instructions[function_value].type < pipeline.concrete.layout.type_count
        ? pipeline.concrete.layout.types[m->instructions[function_value].type].recipe : SOL_MIR_RECIPE_NONE;
    SolMirRecipeId text_recipe = text < m->instruction_count
        && m->instructions[text].type < pipeline.concrete.layout.type_count
        ? pipeline.concrete.layout.types[m->instructions[text].type].recipe : SOL_MIR_RECIPE_NONE;
    const SolMirRuntimeAllocationPlan *text_plan = text_recipe < pipeline.values.allocation_plan_count
        ? &pipeline.values.allocation_plans[text_recipe] : NULL;
    const SolMirRuntimeAllocationPlan *pair_plan = pair != NULL
        && pair->result_recipe < pipeline.values.allocation_plan_count
        ? &pipeline.values.allocation_plans[pair->result_recipe] : NULL;
    SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
    uint64_t callable_bytes = function_value < m->instruction_count
        && m->instructions[function_value].type < pipeline.concrete.layout.type_count
        ? pipeline.concrete.layout.types[m->instructions[function_value].type].object_size : 0;
    if (ok) ok = function_value != SOL_MIR_RUNTIME_LOWERED_NONE
        && text != SOL_MIR_RUNTIME_LOWERED_NONE && construct != SOL_MIR_RUNTIME_LOWERED_NONE
        && callable_recipe != SOL_MIR_RECIPE_NONE && text_plan != NULL && pair_plan != NULL
        && callable_bytes == 4
        && text_plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_TEXT
        && pair_plan->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_FIXED_OBJECT
        && pair_plan->object_size == 8
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory,
            NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
    int32_t callable_site = 0, text_site = 0, pair_site = 0;
    SolMirRuntimeSource callable_source = {0}, text_source = {0}, pair_source = {0};
    if (ok) ok = callable_hole_supplemental_record(&pipeline, &output, function_value,
        &callable_site, &callable_source) && callable_hole_supplemental_record(&pipeline, &output,
        text, &text_site, &text_source) && callable_hole_supplemental_record(&pipeline, &output,
        construct, &pair_site, &pair_source);
    size_t literal_bytes = 0;
    if (ok && text_source.end >= text_source.start + 2 && text_source.end <= pipeline.package.source.length
        && pipeline.package.source.text[text_source.start] == '"'
        && pipeline.package.source.text[text_source.end - 1] == '"')
        literal_bytes = text_source.end - text_source.start - 2;
    else ok = false;
    uint64_t requests = UINT64_C(1) + (literal_bytes == 0 ? UINT64_C(1) : UINT64_C(2)) + UINT64_C(1);
    uint64_t bytes = pair_plan == NULL ? 0
        : callable_bytes + UINT64_C(8) + literal_bytes + pair_plan->object_size;
    if (ok) ok = literal_bytes == 7 && requests == 4 && bytes == 27
        && (strstr(directory, "p43_callable_hole_c32_repair") != NULL
            || (callable_site == 5 && pair_site == 6 && text_site == 7))
        && callable_source.start < pair_source.start && pair_source.start < text_source.start;
    SolWasmRepresentedLimits runtime = sol_wasm_represented_default_limits();
    runtime.max_allocation_requests = requests; runtime.max_allocation_bytes = bytes;
    sol_wasm_represented_test_allocator_quota(UINT64_MAX, UINT64_MAX);
    const char *entry = pipeline.conventions.entry_count == 1
        ? pipeline.conventions.entries[0].symbol.bytes : NULL;
    if (ok) ok = entry != NULL && invoke_named(&output.bytes, entry, 42, 0, 0);
    /* These are real, serialized limits—not the test override.  Zero remains
     * an invalid partial limits record, so the late Pair checks stop at one. */
    if (ok) {
        SolWasmRepresentedOutput capped;
        sol_wasm_represented_output_init(&capped);
        ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, &runtime}, &capped, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && invoke_named(&capped.bytes, entry, 42, 0, 0);
        sol_wasm_represented_output_free(&capped);
        SolWasmRepresentedLimits request_below = runtime;
        request_below.max_allocation_requests = requests - 1;
        sol_wasm_represented_output_init(&capped);
        ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, &request_below}, &capped, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && invoke_named(&capped.bytes, entry, 0, 5, pair_site);
        sol_wasm_represented_output_free(&capped);
        SolWasmRepresentedLimits bytes_below = runtime;
        bytes_below.max_allocation_bytes = bytes - 1;
        sol_wasm_represented_output_init(&capped);
        ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, &bytes_below}, &capped, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && invoke_named(&capped.bytes, entry, 0, 5, pair_site);
        sol_wasm_represented_output_free(&capped);
    }
#define CHECK_CALLABLE_RUNTIME_OVERRIDE(request_cap, limit, site) do { \
    SolWasmRepresentedOutput capped; sol_wasm_represented_output_init(&capped); \
    sol_wasm_represented_test_allocator_quota((request_cap) ? (limit) : UINT64_MAX, \
        (request_cap) ? UINT64_MAX : (limit)); \
    bool runtime_ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, \
        directory, &runtime}, &capped, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK \
        && invoke_named(&capped.bytes, entry, 0, 5, (site)); \
    ok = ok && runtime_ok; \
    sol_wasm_represented_output_free(&capped); sol_wasm_represented_test_allocator_quota(UINT64_MAX, UINT64_MAX); \
} while (0)
    if (ok) {
        CHECK_CALLABLE_RUNTIME_OVERRIDE(true, 0, callable_site);
        CHECK_CALLABLE_RUNTIME_OVERRIDE(false, 3, callable_site);
        CHECK_CALLABLE_RUNTIME_OVERRIDE(true, 1, text_site);
        CHECK_CALLABLE_RUNTIME_OVERRIDE(false, 18, text_site);
        CHECK_CALLABLE_RUNTIME_OVERRIDE(true, 3, pair_site);
        CHECK_CALLABLE_RUNTIME_OVERRIDE(false, 26, pair_site);
    }
#undef CHECK_CALLABLE_RUNTIME_OVERRIDE
    if (ok) {
        SolWasmRepresentedOutput capped; WasmInstance instance = {0};
        sol_wasm_represented_test_callable_hole_cleanup_probe(true);
        sol_wasm_represented_test_allocator_memory(1, UINT32_C(65512));
        sol_wasm_represented_output_init(&capped);
        ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, &runtime}, &capped, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && wasm_instance_open(&capped.bytes, entry, &instance)
            && wasm_instance_call(&instance, 0, 4, pair_site);
        if (instance.instance != NULL) {
            static const int32_t no_product_cleanup[] = {0, 0, 0, 0};
            ok = ok && wasm_instance_cleanup_counters(&instance, no_product_cleanup);
        }
        wasm_instance_close(&instance); sol_wasm_represented_output_free(&capped);
        sol_wasm_represented_test_allocator_memory(0, 0);
        sol_wasm_represented_test_callable_hole_cleanup_probe(false);
        sol_wasm_represented_output_init(&capped);
        ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, &runtime}, &capped, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && wasm_instance_open(&capped.bytes, entry, &instance)
            && wasm_instance_call(&instance, 42, 0, 0)
            && wasm_instance_call(&instance, 42, 0, 0);
        wasm_instance_close(&instance); sol_wasm_represented_output_free(&capped);
    }
    sol_wasm_represented_test_allocator_quota(UINT64_MAX, UINT64_MAX);
    sol_wasm_represented_output_free(&output); propagation_pipeline_free(&pipeline);
    return ok;
}

/* C3.1's backend consumes no inferred cleanup relation.  These are source-built
 * owner mutations: each corrupts the exact instruction/event/transition/action
 * chain (or the root's authenticated physical path), reseals the outer owner,
 * and must still fail closed at the represented boundary. */
static bool callable_hole_owner_mutations(const char *directory, size_t holes) {
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    bool ok = propagation_pipeline_build(&pipeline, directory);
    SolMirMaterialization *m = &pipeline.concrete.materialization;
    SolMirRuntimeCleanupAction *action = NULL;
    SolMirRuntimeCleanupDropPath *path = NULL, *moved = NULL;
    SolMirRuntimeCleanupEvent *event = NULL;
    SolMirRuntimeCleanupTransition *transition = NULL;
    SolMirRuntimeLoweredImageInstruction *row = NULL;
    SolMirRuntimeCleanupDropPath saved_path = {0};
    size_t action_id = SOL_MIR_RUNTIME_LOWERED_NONE;
    if (ok) for (size_t i = 0; i < pipeline.lowered.image_instruction_count; ++i) {
        SolMirRuntimeLoweredImageInstruction *candidate = &pipeline.lowered.image_instructions[i];
        if (candidate->kind != SOL_MIR_INST_DROP_IF_INITIALIZED
            || candidate->cleanup_event >= pipeline.cleanup.event_count) continue;
        SolMirRuntimeCleanupEvent *candidate_event = &pipeline.cleanup.events[candidate->cleanup_event];
        if (candidate_event->transitions.count != 1
            || candidate_event->transitions.offset >= pipeline.cleanup.transition_count) continue;
        SolMirRuntimeCleanupTransition *candidate_transition = &pipeline.cleanup.transitions[
            candidate_event->transitions.offset];
        if (candidate_transition->actions.count != 1
            || candidate_transition->actions.offset >= pipeline.cleanup.action_count) continue;
        SolMirRuntimeCleanupAction *candidate_action = &pipeline.cleanup.actions[
            candidate_transition->actions.offset];
        if (candidate_action->kind != SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE
            || candidate_action->target >= m->place_count
            || m->places[candidate_action->target].projections.count != 0
            || candidate_action->drop_path >= pipeline.cleanup.drop_path_count) continue;
        SolMirRuntimeCleanupDropPath *candidate_path = &pipeline.cleanup.drop_paths[
            candidate_action->drop_path];
        if (candidate_path->root != candidate_action->target
            || candidate_path->place != candidate_action->target
            || candidate_path->recipe != candidate_action->recipe
            || candidate_path->holes.count != holes) continue;
        if (action != NULL) continue;
        action = candidate_action; path = candidate_path; event = candidate_event;
        transition = candidate_transition; row = candidate; action_id = candidate_transition->actions.offset;
    }
    if (action == NULL || path == NULL || event == NULL || transition == NULL || row == NULL
        || action_id == SOL_MIR_RUNTIME_LOWERED_NONE) ok = false;
    if (ok) {
        size_t selected = SOL_MIR_RUNTIME_LOWERED_NONE;
        ok = sol_wasm_represented_test_cleanup_marker(
            &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL},
            (size_t)(row - pipeline.lowered.image_instructions), &selected)
                == SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_ACTION && selected == action_id;
    }
    if (ok && holes == 1) {
        if (path->holes.offset >= pipeline.cleanup.drop_path_count) ok = false;
        else moved = &pipeline.cleanup.drop_paths[path->holes.offset];
    }
    if (ok && holes == 0) ok = path->liveness == SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE;
    if (ok && holes == 1) ok = moved != NULL && moved->root == path->root
        && moved->liveness == SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE
        && moved->place < m->place_count && m->places[moved->place].projections.count == 1;
#define CHECK_CALLABLE_HOLE_REJECT(edit, restore) do { \
    SolWasmRepresentedOutput rejected; sol_wasm_represented_output_init(&rejected); \
    edit; pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
    SolWasmRepresentedResult mutation_result = sol_wasm_represented_build( \
        &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, &rejected, \
        &pipeline.diagnostics); \
    bool mutation_ok = mutation_result == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE \
        && rejected.bytes.bytes == NULL && rejected.bytes.count == 0 && usage_zero(&rejected.usage); \
    ok = ok && mutation_ok; \
    sol_wasm_represented_output_free(&rejected); restore; \
    pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
    ok = ok && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL); \
} while (0)
    if (ok) {
        size_t saved_event = row->cleanup_event;
        CHECK_CALLABLE_HOLE_REJECT(row->cleanup_event = SOL_MIR_RUNTIME_LOWERED_NONE,
            row->cleanup_event = saved_event);
        size_t saved_operation = event->operation;
        CHECK_CALLABLE_HOLE_REJECT(event->operation = SOL_MIR_RUNTIME_LOWERED_NONE,
            event->operation = saved_operation);
        SolMirRuntimeSlice saved_transition_actions = transition->actions;
        CHECK_CALLABLE_HOLE_REJECT(transition->actions.count = 0,
            transition->actions = saved_transition_actions);
        SolMirRuntimeSlice saved_event_actions = event->actions;
        CHECK_CALLABLE_HOLE_REJECT(event->actions.offset = action_id + 1,
            event->actions = saved_event_actions);
        SolMirRuntimeCleanupAction saved_action = *action;
        CHECK_CALLABLE_HOLE_REJECT(action->target = moved == NULL ? m->place_count : moved->place,
            *action = saved_action);
        CHECK_CALLABLE_HOLE_REJECT(action->recipe = SOL_MIR_RECIPE_NONE, *action = saved_action);
        CHECK_CALLABLE_HOLE_REJECT(action->drop_path = SOL_MIR_RUNTIME_NONE, *action = saved_action);
        saved_path = *path;
        CHECK_CALLABLE_HOLE_REJECT(path->root = SOL_MIR_RUNTIME_NONE, *path = saved_path);
        CHECK_CALLABLE_HOLE_REJECT(path->place = SOL_MIR_RUNTIME_NONE, *path = saved_path);
        CHECK_CALLABLE_HOLE_REJECT(path->recipe = SOL_MIR_RECIPE_NONE, *path = saved_path);
        CHECK_CALLABLE_HOLE_REJECT(path->liveness = path->liveness
            == SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE ? SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL
            : SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE, *path = saved_path);
        CHECK_CALLABLE_HOLE_REJECT(path->holes.count = 2, *path = saved_path);
        CHECK_CALLABLE_HOLE_REJECT(path->holes.offset = action->drop_path,
            *path = saved_path);
    }
    if (ok && moved != NULL) {
        SolMirRuntimeCleanupDropPath saved_moved = *moved;
        CHECK_CALLABLE_HOLE_REJECT(*moved = *path, *moved = saved_moved);
        CHECK_CALLABLE_HOLE_REJECT({ SolMirRuntimeCleanupDropPath swap = *path; *path = *moved;
            *moved = swap; }, { *path = saved_path; *moved = saved_moved; });
        size_t projection = m->places[moved->place].projections.offset;
        if (projection >= pipeline.concrete.layout.projection_count) ok = false;
        else {
            SolMirProjectionMap *map = &pipeline.concrete.layout.projections[projection];
            SolMirProjectionMap saved_map = *map;
            CHECK_CALLABLE_HOLE_REJECT(map->place = path->place, *map = saved_map);
            CHECK_CALLABLE_HOLE_REJECT(++map->object_offset, *map = saved_map);
            if (map->field_layout >= pipeline.concrete.layout.field_count) ok = false;
            else {
                SolMirFieldLayout *field = &pipeline.concrete.layout.fields[map->field_layout];
                SolMirFieldLayout saved_field = *field;
                CHECK_CALLABLE_HOLE_REJECT(++field->offset, *field = saved_field);
                SolMirRecipeField *recipe_field = &pipeline.concrete.representation.fields[map->field_layout];
                SolMirRecipeField saved_recipe_field = *recipe_field;
                CHECK_CALLABLE_HOLE_REJECT(recipe_field->type = SOL_MIR_RECIPE_NONE,
                    *recipe_field = saved_recipe_field);
            }
        }
    }
#undef CHECK_CALLABLE_HOLE_REJECT
    /* A present event whose owned action no longer joins exactly must be
     * invalid, not the eventless route.  Exercise the selector before the
     * whole-owner build check, then prove restoration re-enables both. */
#define CHECK_CALLABLE_HOLE_SELECTOR_REJECT(edit, restore) do { \
    SolWasmRepresentedOutput rejected, restored; \
    sol_wasm_represented_output_init(&rejected); sol_wasm_represented_output_init(&restored); \
    edit; pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
    size_t selected = SOL_MIR_RUNTIME_LOWERED_NONE; \
    ok = ok && sol_wasm_represented_test_cleanup_marker( \
        &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, \
        (size_t)(row - pipeline.lowered.image_instructions), &selected) \
            == SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_INVALID \
        && !sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL) \
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, \
            &rejected, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE \
        && rejected.bytes.bytes == NULL && rejected.bytes.count == 0 && usage_zero(&rejected.usage); \
    sol_wasm_represented_output_free(&rejected); restore; \
    pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
    selected = SOL_MIR_RUNTIME_LOWERED_NONE; \
    ok = ok && sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL) \
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL) \
        && sol_wasm_represented_test_cleanup_marker( \
            &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, \
            (size_t)(row - pipeline.lowered.image_instructions), &selected) \
                == SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_ACTION && selected == action_id \
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, \
            &restored, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK; \
    sol_wasm_represented_output_free(&restored); \
} while (0)
    if (ok) {
        SolMirRuntimeCleanupEventKind event_kind = event->kind;
        CHECK_CALLABLE_HOLE_SELECTOR_REJECT(event->kind = SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION,
            event->kind = event_kind);
        SolMirRuntimeSlice event_actions = event->actions;
        CHECK_CALLABLE_HOLE_SELECTOR_REJECT(event->actions.count = 0,
            event->actions = event_actions);
        if (event_actions.offset + 1 < pipeline.cleanup.action_count)
            CHECK_CALLABLE_HOLE_SELECTOR_REJECT(event->actions.count = 2,
                event->actions = event_actions);
        CHECK_CALLABLE_HOLE_SELECTOR_REJECT(event->actions.count = pipeline.cleanup.action_count + 1,
            event->actions = event_actions);
        SolMirRuntimeCleanupAction saved_action = *action;
        CHECK_CALLABLE_HOLE_SELECTOR_REJECT(action->kind = SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY,
            *action = saved_action);
        CHECK_CALLABLE_HOLE_SELECTOR_REJECT(action->target = m->place_count, *action = saved_action);
        CHECK_CALLABLE_HOLE_SELECTOR_REJECT(action->recipe = SOL_MIR_RECIPE_NONE, *action = saved_action);
        size_t row_block = row->block;
        CHECK_CALLABLE_HOLE_SELECTOR_REJECT(row->block = SOL_MIR_RUNTIME_LOWERED_NONE,
            row->block = row_block);
    }
#undef CHECK_CALLABLE_HOLE_SELECTOR_REJECT
    propagation_pipeline_free(&pipeline);
    return ok;
}

/* Each C3.2 shape has one legal entry so package entry policy cannot hide an
 * otherwise valid physical route.  The wrapper resets the hook globals before
 * every invocation; a second call therefore proves no moved/init bit leaked
 * across entry reset. */
static bool callable_hole_c32_runtime(const char *directory, const int32_t expected[4]) {
    SolWasmRepresentedOutput output;
    sol_wasm_represented_output_init(&output);
    sol_wasm_represented_test_callable_hole_cleanup_probe(true);
    bool ok = build_named_root(directory, "launch", &output, NULL, SOL_WASM_REPRESENTED_OK)
        && sol_wasm_represented_validate(&output.bytes) == SOL_WASM_REPRESENTED_OK;
    char entry[256]; WasmInstance instance = {0};
    if (ok) ok = entry_symbol(&output.bytes, entry, sizeof entry)
        && wasm_instance_open(&output.bytes, entry, &instance)
        && wasm_instance_call(&instance, 42, 0, 0)
        && wasm_instance_cleanup_counters(&instance, expected)
        && wasm_instance_call(&instance, 42, 0, 0)
        && wasm_instance_cleanup_counters(&instance, expected);
    wasm_instance_close(&instance);
    sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_callable_hole_cleanup_probe(false);
    return ok;
}

/* Repair is the representative C3.2 entry: it traverses the projected marker
 * and store route while retaining the same allocation envelope as every Pair
 * shape.  Keep its complete physical census and retry/fault behavior frozen. */
static bool callable_hole_c32_repair_controls(const char *directory) {
    static const SolWasmRepresentedUsage expected = {7,2,0,8,56,1027,2,15,0,0,40,26623,10240,17973,7733};
    static const uint8_t expected_hash[32] = {0x85,0xaf,0x13,0xd2,0xf0,0xcb,0x7a,0xa4,0xcf,0xba,0x47,0xa7,0x8d,0x0c,0x18,0xf1,0x33,0x0f,0x33,0xbe,0x79,0xc6,0x13,0x9c,0xdf,0xcc,0x87,0x2d,0xd0,0x5e,0x61,0x9c};
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    SolWasmRepresentedOutput baseline, output; sol_wasm_represented_output_init(&baseline);
    sol_wasm_represented_output_init(&output);
    bool ok = propagation_pipeline_build(&pipeline, directory)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory,
            NULL}, &baseline, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
    uint8_t hash[32];
    if (ok) { sha256(baseline.bytes.bytes, baseline.bytes.count, hash); ok = usage_equal(&baseline.usage,
        &expected) && memcmp(hash, expected_hash, sizeof hash) == 0
        && sol_wasm_represented_validate(&baseline.bytes) == SOL_WASM_REPRESENTED_OK; }
    SolWasmRepresentedLimits defaults = {0};
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
        directory, &defaults}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && output.bytes.count == baseline.bytes.count && memcmp(output.bytes.bytes, baseline.bytes.bytes,
            baseline.bytes.count) == 0 && usage_equal(&output.usage, &baseline.usage);
    sol_wasm_represented_output_free(&output);
#define CHECK_C32_CAP(field, cap) do { \
    SolWasmRepresentedLimits limits = sol_wasm_represented_default_limits(); \
    limits.field = (cap); sol_wasm_represented_output_init(&output); \
    ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, \
        directory, &limits}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK \
        && output.bytes.count == baseline.bytes.count && usage_equal(&output.usage, &baseline.usage); \
    sol_wasm_represented_output_free(&output); limits.field = (cap) - 1; \
    sol_wasm_represented_output_init(&output); \
    ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, \
        directory, &limits}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED \
        && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage); \
    sol_wasm_represented_output_free(&output); \
} while (0)
    CHECK_C32_CAP(max_functions, expected.functions); CHECK_C32_CAP(max_blocks, expected.blocks);
    CHECK_C32_CAP(max_values, expected.values); CHECK_C32_CAP(max_locals, expected.locals);
    CHECK_C32_CAP(max_generated_nodes, expected.generated_nodes);
    CHECK_C32_CAP(max_table_elements, expected.table_elements);
    CHECK_C32_CAP(max_static_data_bytes, expected.static_data_bytes);
    CHECK_C32_CAP(max_provenance_records, expected.provenance_records);
    CHECK_C32_CAP(max_work_bytes, expected.work_bytes); CHECK_C32_CAP(max_scratch_bytes, expected.scratch_bytes);
    CHECK_C32_CAP(max_owned_bytes, expected.owned_bytes); CHECK_C32_CAP(max_output_bytes, expected.output_bytes);
#undef CHECK_C32_CAP
#define CHECK_C32_PARTIAL(field) do { \
    SolWasmRepresentedLimits limits = sol_wasm_represented_default_limits(); limits.field = 0; \
    sol_wasm_represented_output_init(&output); \
    ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, \
        directory, &limits}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_INVALID_ARGUMENT \
        && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage); \
    sol_wasm_represented_output_free(&output); \
} while (0)
    CHECK_C32_PARTIAL(max_functions); CHECK_C32_PARTIAL(max_blocks); CHECK_C32_PARTIAL(max_edges);
    CHECK_C32_PARTIAL(max_values); CHECK_C32_PARTIAL(max_locals); CHECK_C32_PARTIAL(max_generated_nodes);
    CHECK_C32_PARTIAL(max_table_elements); CHECK_C32_PARTIAL(max_static_data_bytes);
    CHECK_C32_PARTIAL(max_allocation_requests); CHECK_C32_PARTIAL(max_allocation_bytes);
    CHECK_C32_PARTIAL(max_provenance_records); CHECK_C32_PARTIAL(max_work_bytes);
    CHECK_C32_PARTIAL(max_scratch_bytes); CHECK_C32_PARTIAL(max_owned_bytes);
    CHECK_C32_PARTIAL(max_output_bytes);
#undef CHECK_C32_PARTIAL
    if (ok) {
        sol_wasm_represented_test_fail_allocation_after(0);
        sol_wasm_represented_output_init(&output);
        ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory,
            NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && sol_wasm_represented_test_allocation_attempts() == 66;
        sol_wasm_represented_output_free(&output);
        for (size_t attempt = 1; ok && attempt <= 66; ++attempt) {
            represented_fault(FAULT_C32_REPAIR, attempt, attempt);
            sol_wasm_represented_output_init(&output);
            ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_ALLOCATION_FAILED
                && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage);
            sol_wasm_represented_output_free(&output);
        }
        sol_wasm_represented_test_fail_allocation_after(0);
    }
    sol_wasm_represented_output_free(&baseline); propagation_pipeline_free(&pipeline);
    return ok;
}

static bool callable_hole_c32_repair_multiroot(const char *directory) {
    SolWasmRepresentedOutput forward, reverse, relocated;
    size_t forward_ids[2], reverse_ids[2], relocated_ids[2];
    sol_wasm_represented_output_init(&forward); sol_wasm_represented_output_init(&reverse);
    sol_wasm_represented_output_init(&relocated);
    bool ok = build_multiroot(directory, false, &forward, forward_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && build_multiroot(directory, true, &reverse, reverse_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && forward_ids[0] != forward_ids[1]
        && forward_ids[0] == reverse_ids[0] && forward_ids[1] == reverse_ids[1]
        && forward.bytes.count == reverse.bytes.count && memcmp(forward.bytes.bytes, reverse.bytes.bytes,
            forward.bytes.count) == 0 && usage_equal(&forward.usage, &reverse.usage);
    uint8_t forward_hash[32], reverse_hash[32];
    if (ok) { sha256(forward.bytes.bytes, forward.bytes.count, forward_hash);
        sha256(reverse.bytes.bytes, reverse.bytes.count, reverse_hash);
        ok = memcmp(forward_hash, reverse_hash, sizeof forward_hash) == 0; }
    char entry[256];
    if (ok) ok = entry_symbol(&forward.bytes, entry, sizeof entry)
        && invoke_named(&forward.bytes, entry, 42, 0, 0)
        && invoke_named(&reverse.bytes, entry, 42, 0, 0);
    char relocation[512], source[768], destination[768];
    (void)mkdir(SOL_TEST_BINARY_DIR, 0700);
    (void)snprintf(relocation, sizeof relocation, "%s/p43_callable_hole_c32_repair_relocated",
        SOL_TEST_BINARY_DIR);
    (void)mkdir(relocation, 0700);
    (void)snprintf(source, sizeof source, "%s/main.sol", directory);
    (void)snprintf(destination, sizeof destination, "%s/main.sol", relocation);
    FILE *input = fopen(source, "rb"), *output = fopen(destination, "wb");
    if (input == NULL || output == NULL) ok = false;
    if (input != NULL && output != NULL) {
        uint8_t buffer[256]; size_t count = 0;
        while ((count = fread(buffer, 1, sizeof buffer, input)) != 0)
            if (fwrite(buffer, 1, count, output) != count) ok = false;
    }
    if (input != NULL) fclose(input);
    if (output != NULL) fclose(output);
    if (ok) ok = build_multiroot(relocation, false, &relocated, relocated_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && relocated_ids[0] == forward_ids[0]
        && relocated_ids[1] == forward_ids[1] && relocated.bytes.count == forward.bytes.count
        && memcmp(relocated.bytes.bytes, forward.bytes.bytes, forward.bytes.count) == 0
        && usage_equal(&relocated.usage, &forward.usage) && invoke_named(&relocated.bytes, entry, 42, 0, 0);
    sol_wasm_represented_output_free(&relocated); sol_wasm_represented_output_free(&reverse);
    sol_wasm_represented_output_free(&forward);
    return ok;
}

static bool propagation_owner_rejected(PropagationPipeline *pipeline, const char *directory) {
    SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
    SolWasmRepresentedResult result = sol_wasm_represented_build(
        &(SolWasmRepresentedBuildRequest){&pipeline->lowered, directory, NULL}, &output,
        &pipeline->diagnostics);
    bool rejected = result == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE && output.bytes.bytes == NULL
        && output.bytes.count == 0 && usage_zero(&output.usage);
    return rejected;
}

/* C3.2 is anchored at a materialized local-root marker rather than an inferred
 * temporary cleanup.  The only STORE into that body local must trace back to
 * the projected callable move which made the local live. */
typedef struct {
    size_t marker, event, transition, action, path;
    size_t root, local, store, move;
    SolMirRecipeId recipe;
} C32MovedMarker;

typedef struct { size_t store, move, repair_store; } C32RepairChain;

static bool c32_projected_move(const SolMirMaterialization *m,
    SolMirMaterializedValueId value, size_t depth, size_t *move_out) {
    size_t definition = SOL_MIR_MATERIALIZED_NONE, definitions = 0;
    if (m == NULL || move_out == NULL || value >= m->value_count || depth > m->instruction_count)
        return false;
    for (size_t i = 0; i < m->instruction_count; ++i)
        if (m->instructions[i].result == value) { definition = i; ++definitions; }
    if (definitions != 1 || definition >= m->instruction_count
        || m->instructions[definition].kind != SOL_MIR_INST_LOAD_MOVE
        || m->instructions[definition].place >= m->place_count) return false;
    const SolMirMaterializedPlace *place = &m->places[m->instructions[definition].place];
    if (place->projections.count == 1) { *move_out = definition; return true; }
    if (place->projections.count != 0) return false;
    size_t store = SOL_MIR_MATERIALIZED_NONE, stores = 0;
    for (size_t i = 0; i < m->instruction_count; ++i)
        if (m->instructions[i].kind == SOL_MIR_INST_STORE && m->instructions[i].place < m->place_count
            && m->places[m->instructions[i].place].projections.count == 0
            && m->places[m->instructions[i].place].local == place->local) {
            store = i; ++stores;
        }
    return stores == 1 && m->instructions[store].left != SOL_MIR_MATERIALIZED_NONE
        && c32_projected_move(m, m->instructions[store].left, depth + 1, move_out);
}

static bool c32_same_projection(const SolMirMaterialization *m, size_t left, size_t right) {
    if (left >= m->place_count || right >= m->place_count) return false;
    const SolMirMaterializedPlace *a = &m->places[left], *b = &m->places[right];
    if (a->local != b->local || a->projections.count != b->projections.count) return false;
    for (size_t i = 0; i < a->projections.count; ++i) {
        const SolMirMaterializedProjection *x = &m->projections[a->projections.offset + i];
        const SolMirMaterializedProjection *y = &m->projections[b->projections.offset + i];
        if (x->kind != y->kind || x->source_field != y->source_field
            || x->tuple_ordinal != y->tuple_ordinal) return false;
    }
    return true;
}

static bool c32_reopen_after_repair(const SolMirMaterialization *m,
    const C32MovedMarker *marker) {
    size_t moves[2] = {SOL_MIR_MATERIALIZED_NONE, SOL_MIR_MATERIALIZED_NONE};
    size_t count = 0, repairs = 0;
    if (m == NULL || marker == NULL || marker->move >= m->instruction_count
        || m->instructions[marker->move].place >= m->place_count) return false;
    for (size_t i = 0; i < m->instruction_count; ++i) {
        const SolMirMaterializedInstruction *item = &m->instructions[i];
        if (item->kind == SOL_MIR_INST_LOAD_MOVE && item->place < m->place_count
            && m->places[item->place].projections.count == 1
            && c32_same_projection(m, item->place, m->instructions[marker->move].place)) {
            if (count < sizeof moves / sizeof *moves) moves[count] = i;
            ++count;
        }
    }
    if (count != 2 || moves[1] != marker->move) return false;
    for (size_t i = moves[0] + 1; i < marker->move; ++i) {
        const SolMirMaterializedInstruction *store = &m->instructions[i];
        size_t source = SOL_MIR_MATERIALIZED_NONE;
        if (store->kind != SOL_MIR_INST_STORE || store->place >= m->place_count
            || !c32_same_projection(m, store->place, m->instructions[moves[0]].place)
            || !c32_projected_move(m, store->left, 0, &source) || source != moves[0]) continue;
        ++repairs;
    }
    return repairs == 1;
}

static bool c32_unique_moved_marker(const PropagationPipeline *pipeline, bool reopen,
    C32MovedMarker *out) {
    const SolMirMaterialization *m = &pipeline->concrete.materialization;
    const SolMirConcreteProgram *concrete = &pipeline->concrete;
    size_t matches = 0;
    if (out == NULL) return false;
    for (size_t marker = 0; marker < m->instruction_count; ++marker) {
        const SolMirMaterializedInstruction *instruction = &m->instructions[marker];
        if (instruction->kind != SOL_MIR_INST_DROP_IF_INITIALIZED
            || marker >= pipeline->lowered.image_instruction_count) continue;
        const SolMirRuntimeLoweredImageInstruction *row = &pipeline->lowered.image_instructions[marker];
        if (row->state != SOL_MIR_RUNTIME_LOWERED_PRESENT || row->instruction != marker
            || row->kind != instruction->kind || row->cleanup_event >= pipeline->cleanup.event_count)
            continue;
        const SolMirRuntimeCleanupEvent *event = &pipeline->cleanup.events[row->cleanup_event];
        if (event->operation != marker || event->transitions.count != 1
            || event->transitions.offset >= pipeline->cleanup.transition_count
            || event->actions.count != 1 || event->actions.offset >= pipeline->cleanup.action_count) continue;
        const SolMirRuntimeCleanupTransition *transition = &pipeline->cleanup.transitions[event->transitions.offset];
        if (transition->event != row->cleanup_event
            || transition->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL
            || transition->actions.count != 1 || transition->actions.offset != event->actions.offset)
            continue;
        size_t action = SOL_MIR_RUNTIME_NONE, actions = 0;
        for (size_t i = 0; i < transition->actions.count; ++i) {
            size_t id = transition->actions.offset + i;
            const SolMirRuntimeCleanupAction *candidate = &pipeline->cleanup.actions[id];
            if (candidate->kind != SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE
                || candidate->target >= m->place_count
                || candidate->drop_path >= pipeline->cleanup.drop_path_count) continue;
            const SolMirMaterializedPlace *root = &m->places[candidate->target];
            const SolMirRuntimeCleanupDropPath *path = &pipeline->cleanup.drop_paths[candidate->drop_path];
            SolMirRecipeId recipe = root->final_type < concrete->layout.type_count
                ? concrete->layout.types[root->final_type].recipe : SOL_MIR_RECIPE_NONE;
            if (root->projections.count != 0 || root->local != instruction->local
                || root->local >= m->local_count || m->locals[root->local].kind != SOL_MIR_MATERIALIZED_LOCAL_BODY
                || path->root != candidate->target || path->place != candidate->target
                || path->recipe != candidate->recipe || path->holes.count != 0
                || path->liveness != SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE
                || candidate->recipe != recipe || recipe >= concrete->representation.recipe_count
                || concrete->representation.recipes[recipe].kind != SOL_MIR_RECIPE_FUNCTION) continue;
            action = id; ++actions;
        }
        if (actions != 1) continue;
        const SolMirRuntimeCleanupAction *drop = &pipeline->cleanup.actions[action];
        size_t store = SOL_MIR_MATERIALIZED_NONE, stores = 0, move = SOL_MIR_MATERIALIZED_NONE;
        for (size_t i = 0; i < m->instruction_count; ++i) {
            const SolMirMaterializedInstruction *candidate = &m->instructions[i];
            if (candidate->kind != SOL_MIR_INST_STORE || candidate->place >= m->place_count
                || m->places[candidate->place].projections.count != 0
                || m->places[candidate->place].local != m->places[drop->target].local) continue;
            size_t source = SOL_MIR_MATERIALIZED_NONE;
            if (!c32_projected_move(m, candidate->left, 0, &source)) { stores = 2; break; }
            store = i; move = source; ++stores;
        }
        if (stores != 1 || store >= marker || move >= store
            || m->instructions[move].place >= m->place_count
            || m->places[m->instructions[move].place].projections.count != 1) continue;
        C32MovedMarker candidate = {marker, row->cleanup_event, event->transitions.offset, action,
            drop->drop_path, drop->target, m->places[drop->target].local, store, move, drop->recipe};
        if (reopen && !c32_reopen_after_repair(m, &candidate)) continue;
        ++matches; *out = candidate;
    }
    return matches == 1;
}

/* Repair consumes the first moved local before its lexical marker, so it has
 * no live DROP_IF_INITIALIZED event to forge.  Its authenticated evidence is
 * the root-local STORE and the later projected repair STORE which both trace
 * to that first projected move. */
static bool c32_unique_repair_chain(const PropagationPipeline *pipeline, C32RepairChain *out) {
    const SolMirMaterialization *m = &pipeline->concrete.materialization;
    size_t stores = 0, store = SOL_MIR_MATERIALIZED_NONE, move = SOL_MIR_MATERIALIZED_NONE;
    if (out == NULL) return false;
    for (size_t i = 0; i < m->instruction_count; ++i) {
        const SolMirMaterializedInstruction *candidate = &m->instructions[i];
        size_t source = SOL_MIR_MATERIALIZED_NONE;
        if (candidate->kind != SOL_MIR_INST_STORE || candidate->place >= m->place_count
            || m->places[candidate->place].projections.count != 0
            || !c32_projected_move(m, candidate->left, 0, &source) || source >= i) continue;
        store = i; move = source; ++stores;
    }
    if (stores != 1 || move >= m->instruction_count || m->instructions[move].place >= m->place_count)
        return false;
    size_t repairs = 0, repair_store = SOL_MIR_MATERIALIZED_NONE;
    for (size_t i = store + 1; i < m->instruction_count; ++i) {
        const SolMirMaterializedInstruction *candidate = &m->instructions[i];
        size_t source = SOL_MIR_MATERIALIZED_NONE;
        if (candidate->kind != SOL_MIR_INST_STORE || candidate->place >= m->place_count
            || !c32_same_projection(m, candidate->place, m->instructions[move].place)
            || !c32_projected_move(m, candidate->left, 0, &source) || source != move) continue;
        repair_store = i; ++repairs;
    }
    if (repairs != 1) return false;
    *out = (C32RepairChain){store, move, repair_store};
    return true;
}

static bool c32_owner_rejected(PropagationPipeline *pipeline, const char *directory,
    bool cleanup_must_reject) {
    SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
    pipeline->lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline->lowered);
    bool cleanup_rejected = !sol_mir_runtime_cleanup_validate(&pipeline->cleanup, NULL);
    bool ok = cleanup_rejected == cleanup_must_reject
        && !sol_mir_runtime_lowered_program_validate(&pipeline->lowered, NULL)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline->lowered, directory,
            NULL}, &output, &pipeline->diagnostics) == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE
        && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage);
    sol_wasm_represented_output_free(&output);
    return ok;
}

static bool c32_owner_restored(PropagationPipeline *pipeline, const char *directory) {
    SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
    pipeline->lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline->lowered);
    bool ok = sol_mir_runtime_cleanup_validate(&pipeline->cleanup, NULL)
        && sol_mir_runtime_lowered_program_validate(&pipeline->lowered, NULL)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline->lowered, directory,
            NULL}, &output, &pipeline->diagnostics) == SOL_WASM_REPRESENTED_OK
        && output.bytes.bytes != NULL && output.bytes.count != 0
        && sol_wasm_represented_validate(&output.bytes) == SOL_WASM_REPRESENTED_OK;
    sol_wasm_represented_output_free(&output);
    return ok;
}

/* Every mutation below changes one authenticated owner field, then reseals the
 * outer P3.6 record solely to prove structural rejection rather than a stale
 * digest mismatch.  Restoration must recover the complete cleanup/lowered/
 * represented build before the next field is touched. */
static bool callable_hole_c32_owner_mutations(const char *directory, unsigned shape) {
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    SolMirMaterialization *m = &pipeline.concrete.materialization;
    bool ok = propagation_pipeline_build(&pipeline, directory)
        && sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL)
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL);
#define CHECK_C32_OWNER_MUTATION(cleanup_reject, edit, restore) do { \
    edit; ok = ok && c32_owner_rejected(&pipeline, directory, (cleanup_reject)); restore; \
    ok = ok && c32_owner_restored(&pipeline, directory); \
} while (0)
    if (ok && shape == 0) { /* conditional root plus its exact projected hole */
        SolMirRuntimeCleanupAction *action = NULL; SolMirRuntimeCleanupDropPath *root = NULL;
        SolMirRuntimeCleanupDropPath *hole = NULL; SolMirRuntimeCleanupEvent *event = NULL;
        SolMirRuntimeCleanupTransition *transition = NULL; SolMirRuntimeLoweredImageInstruction *row = NULL;
        size_t instruction = SOL_MIR_MATERIALIZED_NONE, matches = 0;
        for (size_t i = 0; i < m->instruction_count; ++i) {
            if (m->instructions[i].kind != SOL_MIR_INST_DROP_IF_INITIALIZED
                || i >= pipeline.lowered.image_instruction_count) continue;
            SolMirRuntimeLoweredImageInstruction *candidate = &pipeline.lowered.image_instructions[i];
            if (candidate->cleanup_event >= pipeline.cleanup.event_count) continue;
            SolMirRuntimeCleanupEvent *e = &pipeline.cleanup.events[candidate->cleanup_event];
            if (e->transitions.count != 1 || e->transitions.offset >= pipeline.cleanup.transition_count) continue;
            SolMirRuntimeCleanupTransition *t = &pipeline.cleanup.transitions[e->transitions.offset];
            if (t->actions.count != 1 || t->actions.offset >= pipeline.cleanup.action_count) continue;
            SolMirRuntimeCleanupAction *a = &pipeline.cleanup.actions[t->actions.offset];
            if (a->kind != SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE || a->drop_path >= pipeline.cleanup.drop_path_count)
                continue;
            SolMirRuntimeCleanupDropPath *p = &pipeline.cleanup.drop_paths[a->drop_path];
            if (p->holes.count != 1 || p->holes.offset >= pipeline.cleanup.drop_path_count) continue;
            ++matches; action = a; root = p; hole = &pipeline.cleanup.drop_paths[p->holes.offset];
            event = e; transition = t; row = candidate; instruction = i;
        }
        ok = ok && matches == 1 && action != NULL && root != NULL && hole != NULL && event != NULL
            && transition != NULL && row != NULL && instruction < m->instruction_count
            && action->flags == SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED
            && root->liveness == SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL
            && hole->liveness == SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL;
        if (ok) {
            SolMirRuntimeCleanupDropLiveness liveness = root->liveness;
            CHECK_C32_OWNER_MUTATION(true, root->liveness = SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE,
                root->liveness = liveness);
            liveness = hole->liveness;
            CHECK_C32_OWNER_MUTATION(true, hole->liveness = SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE,
                hole->liveness = liveness);
            unsigned flags = action->flags;
            CHECK_C32_OWNER_MUTATION(true, action->flags = 0, action->flags = flags);
            SolMirRuntimeSlice slice = event->actions;
            CHECK_C32_OWNER_MUTATION(true, event->actions.count = 0, event->actions = slice);
            slice = event->transitions;
            CHECK_C32_OWNER_MUTATION(true, event->transitions.count = 0, event->transitions = slice);
            slice = transition->actions;
            CHECK_C32_OWNER_MUTATION(true, transition->actions.count = 0, transition->actions = slice);
            size_t target = action->target;
            CHECK_C32_OWNER_MUTATION(true, action->target = m->place_count, action->target = target);
            size_t place = root->place;
            CHECK_C32_OWNER_MUTATION(true, root->place = m->place_count, root->place = place);
            size_t cleanup_event = row->cleanup_event;
            CHECK_C32_OWNER_MUTATION(false, row->cleanup_event = SOL_MIR_RUNTIME_LOWERED_NONE,
                row->cleanup_event = cleanup_event);
        }
    } else if (ok && shape == 1) {
        static const int32_t consumed[] = {1, 0, 1, 1};
        C32RepairChain chain = {0};
        /* The probe observes zero moved-callable cleanup only because the
         * repair store consumed that local at runtime. */
        ok = callable_hole_c32_runtime(directory, consumed)
            && c32_unique_repair_chain(&pipeline, &chain);
        if (ok) {
            SolMirMaterializedInstruction *store = &m->instructions[chain.store];
            SolMirMaterializedInstruction *move = &m->instructions[chain.move];
            SolMirMaterializedInstruction *repair = &m->instructions[chain.repair_store];
            size_t left = store->left;
            CHECK_C32_OWNER_MUTATION(true, store->left = SOL_MIR_MATERIALIZED_NONE, store->left = left);
            size_t store_place = store->place;
            CHECK_C32_OWNER_MUTATION(true, store->place = m->place_count, store->place = store_place);
            size_t result = move->result;
            CHECK_C32_OWNER_MUTATION(true, move->result = m->value_count, move->result = result);
            size_t move_place = move->place;
            CHECK_C32_OWNER_MUTATION(true, move->place = m->place_count, move->place = move_place);
            left = repair->left;
            CHECK_C32_OWNER_MUTATION(true, repair->left = SOL_MIR_MATERIALIZED_NONE,
                repair->left = left);
            store_place = repair->place;
            CHECK_C32_OWNER_MUTATION(true, repair->place = m->place_count,
                repair->place = store_place);
        }
    } else if (ok && shape == 2) {
        C32MovedMarker witness = {0};
        ok = ok && c32_unique_moved_marker(&pipeline, true, &witness);
        if (ok) {
            SolMirRuntimeLoweredImageInstruction *row = &pipeline.lowered.image_instructions[witness.marker];
            SolMirRuntimeCleanupEvent *event = &pipeline.cleanup.events[witness.event];
            SolMirRuntimeCleanupTransition *transition = &pipeline.cleanup.transitions[witness.transition];
            SolMirRuntimeCleanupAction *action = &pipeline.cleanup.actions[witness.action];
            SolMirRuntimeCleanupDropPath *path = &pipeline.cleanup.drop_paths[witness.path];
            SolMirMaterializedInstruction *store = &m->instructions[witness.store];
            SolMirMaterializedInstruction *move = &m->instructions[witness.move];
            size_t cleanup_event = row->cleanup_event;
            CHECK_C32_OWNER_MUTATION(false, row->cleanup_event = SOL_MIR_RUNTIME_LOWERED_NONE,
                row->cleanup_event = cleanup_event);
            size_t operation = event->operation;
            CHECK_C32_OWNER_MUTATION(true, event->operation = SOL_MIR_RUNTIME_NONE,
                event->operation = operation);
            SolMirRuntimeSlice slice = event->actions;
            CHECK_C32_OWNER_MUTATION(true, event->actions.count = 0, event->actions = slice);
            slice = event->transitions;
            CHECK_C32_OWNER_MUTATION(true, event->transitions.count = 0, event->transitions = slice);
            size_t transition_event = transition->event;
            CHECK_C32_OWNER_MUTATION(true, transition->event = SOL_MIR_RUNTIME_NONE,
                transition->event = transition_event);
            slice = transition->actions;
            CHECK_C32_OWNER_MUTATION(true, transition->actions.count = 0, transition->actions = slice);
            size_t target = action->target;
            CHECK_C32_OWNER_MUTATION(true, action->target = m->place_count, action->target = target);
            SolMirRecipeId recipe = action->recipe;
            CHECK_C32_OWNER_MUTATION(true, action->recipe = SOL_MIR_RECIPE_NONE, action->recipe = recipe);
            size_t drop_path = action->drop_path;
            CHECK_C32_OWNER_MUTATION(true, action->drop_path = SOL_MIR_RUNTIME_NONE,
                action->drop_path = drop_path);
            SolMirRuntimeCleanupDropPath saved_path = *path;
            CHECK_C32_OWNER_MUTATION(true, path->root = m->place_count, *path = saved_path);
            CHECK_C32_OWNER_MUTATION(true, path->place = m->place_count, *path = saved_path);
            CHECK_C32_OWNER_MUTATION(true, path->recipe = SOL_MIR_RECIPE_NONE, *path = saved_path);
            CHECK_C32_OWNER_MUTATION(true, path->liveness = SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL,
                *path = saved_path);
            CHECK_C32_OWNER_MUTATION(true, path->holes.count = 1, *path = saved_path);
            size_t left = store->left;
            CHECK_C32_OWNER_MUTATION(true, store->left = SOL_MIR_MATERIALIZED_NONE, store->left = left);
            size_t store_place = store->place;
            CHECK_C32_OWNER_MUTATION(true, store->place = m->place_count, store->place = store_place);
            size_t result = move->result;
            CHECK_C32_OWNER_MUTATION(true, move->result = m->value_count, move->result = result);
            size_t move_place = move->place;
            CHECK_C32_OWNER_MUTATION(true, move->place = m->place_count, move->place = move_place);
        }
    } else if (ok && shape == 3) { /* whole product move plus eventless old-root marker */
        size_t move_id = SOL_MIR_MATERIALIZED_NONE, store_id = SOL_MIR_MATERIALIZED_NONE;
        size_t old_marker = SOL_MIR_MATERIALIZED_NONE, destination_marker = SOL_MIR_MATERIALIZED_NONE;
        SolMirRuntimeCleanupAction *action = NULL; SolMirRuntimeCleanupDropPath *path = NULL;
        for (size_t i = 0; i < m->instruction_count; ++i) {
            if (m->instructions[i].kind != SOL_MIR_INST_LOAD_MOVE || m->instructions[i].place >= m->place_count
                || m->places[m->instructions[i].place].projections.count != 0) continue;
            for (size_t q = i + 1; q < m->instruction_count; ++q)
                if (m->instructions[q].kind == SOL_MIR_INST_STORE && m->instructions[q].left
                    == m->instructions[i].result && m->instructions[q].place < m->place_count
                    && m->places[m->instructions[q].place].projections.count == 0) {
                    move_id = i; store_id = q;
                }
        }
        if (move_id < m->instruction_count) for (size_t i = move_id + 1; i < m->instruction_count; ++i)
            if (m->instructions[i].kind == SOL_MIR_INST_DROP_IF_INITIALIZED
                && m->instructions[i].local == m->places[m->instructions[move_id].place].local
                && i < pipeline.lowered.image_instruction_count
                && pipeline.lowered.image_instructions[i].cleanup_event == SOL_MIR_RUNTIME_LOWERED_NONE)
                old_marker = i;
        if (store_id < m->instruction_count) for (size_t i = 0; i < m->instruction_count; ++i) {
            bool destination_drop = m->instructions[i].kind == SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED
                && m->instructions[i].place == m->instructions[store_id].place;
            destination_drop = destination_drop || (m->instructions[i].kind
                == SOL_MIR_INST_DROP_IF_INITIALIZED && m->instructions[i].local
                == m->places[m->instructions[store_id].place].local);
            if (!destination_drop || i >= pipeline.lowered.image_instruction_count) continue;
            SolMirRuntimeLoweredImageInstruction *row = &pipeline.lowered.image_instructions[i];
            if (row->cleanup_event >= pipeline.cleanup.event_count) continue;
            SolMirRuntimeCleanupEvent *event = &pipeline.cleanup.events[row->cleanup_event];
            if (event->transitions.count != 1 || event->transitions.offset >= pipeline.cleanup.transition_count) continue;
            SolMirRuntimeCleanupTransition *transition = &pipeline.cleanup.transitions[event->transitions.offset];
            if (transition->actions.count != 1 || transition->actions.offset >= pipeline.cleanup.action_count) continue;
            SolMirRuntimeCleanupAction *candidate = &pipeline.cleanup.actions[transition->actions.offset];
            if (candidate->drop_path >= pipeline.cleanup.drop_path_count) continue;
            SolMirRuntimeCleanupDropPath *candidate_path = &pipeline.cleanup.drop_paths[candidate->drop_path];
            if (candidate->target == m->instructions[store_id].place && candidate_path->holes.count == 0) {
                destination_marker = i; action = candidate; path = candidate_path;
            }
        }
        ok = ok && move_id < m->instruction_count && store_id < m->instruction_count
            && old_marker < m->instruction_count && destination_marker < m->instruction_count
            && action != NULL && path != NULL;
        if (ok) {
            SolMirRuntimeLoweredImageInstruction *old_row = &pipeline.lowered.image_instructions[old_marker];
            size_t selected = SOL_MIR_RUNTIME_LOWERED_NONE;
            ok = ok && sol_wasm_represented_test_cleanup_marker(
                &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, old_marker,
                &selected) == SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_EVENTLESS
                && selected == SOL_MIR_RUNTIME_NONE;
            SolMirRuntimeLoweredRuntimeClass runtime_class = old_row->runtime_class;
            old_row->runtime_class = SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE;
            selected = SOL_MIR_RUNTIME_LOWERED_NONE;
            ok = ok && sol_wasm_represented_test_cleanup_marker(
                &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, old_marker,
                &selected) == SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_INVALID
                && c32_owner_rejected(&pipeline, directory, false);
            old_row->runtime_class = runtime_class;
            selected = SOL_MIR_RUNTIME_LOWERED_NONE;
            ok = ok && c32_owner_restored(&pipeline, directory)
                && sol_wasm_represented_test_cleanup_marker(
                    &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, old_marker,
                    &selected) == SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_EVENTLESS
                && selected == SOL_MIR_RUNTIME_NONE;
            size_t old_event = old_row->cleanup_event;
            CHECK_C32_OWNER_MUTATION(false, old_row->cleanup_event = 0, old_row->cleanup_event = old_event);
            SolMirMaterializedInstruction *move = &m->instructions[move_id];
            size_t source = move->place;
            CHECK_C32_OWNER_MUTATION(true, move->place = m->place_count, move->place = source);
            size_t result = move->result;
            CHECK_C32_OWNER_MUTATION(true, move->result = m->value_count, move->result = result);
            SolMirMaterializedInstruction *store = &m->instructions[store_id];
            size_t left = store->left;
            CHECK_C32_OWNER_MUTATION(true, store->left = SOL_MIR_MATERIALIZED_NONE, store->left = left);
            SolMirRuntimeLoweredImageInstruction *destination_row = &pipeline.lowered.image_instructions[destination_marker];
            size_t destination_event = destination_row->cleanup_event;
            CHECK_C32_OWNER_MUTATION(false, destination_row->cleanup_event = SOL_MIR_RUNTIME_LOWERED_NONE,
                destination_row->cleanup_event = destination_event);
            size_t target = action->target;
            CHECK_C32_OWNER_MUTATION(true, action->target = m->place_count, action->target = target);
            SolMirRuntimeCleanupDropPath saved_path = *path;
            CHECK_C32_OWNER_MUTATION(true, path->root = m->place_count, *path = saved_path);
            /* A whole transfer carries no projected source hole.  Forging the
             * destination's zero-hole path into a maybe-holed source shape is
             * rejected before it can be treated as C3.2 cleanup. */
            CHECK_C32_OWNER_MUTATION(true, path->holes.count = 1, *path = saved_path);
        }
    } else ok = false;
#undef CHECK_C32_OWNER_MUTATION
    propagation_pipeline_free(&pipeline);
    return ok;
}

/* The source remains type/effect/IR authenticated.  This deliberately forges
 * only P3's callback signature after lowering: neither the program validator
 * nor the represented backend may reinterpret it as a compatible indirect
 * target. */
static bool callback_signature_fail_closed(const char *directory) {
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    bool ok = propagation_pipeline_build(&pipeline, directory);
    size_t call_id = SOL_MIR_RUNTIME_NONE;
    if (ok) for (size_t i = 0; i < pipeline.conventions.call_count; ++i)
        if (pipeline.conventions.calls[i].call_kind == SOL_IR_CALL_CALLBACK) {
            if (call_id != SOL_MIR_RUNTIME_NONE) ok = false;
            call_id = i;
        }
    SolMirRuntimeSignature *signature = ok && call_id != SOL_MIR_RUNTIME_NONE
        && pipeline.conventions.calls[call_id].signature < pipeline.conventions.signature_count
        ? &pipeline.conventions.signatures[pipeline.conventions.calls[call_id].signature] : NULL;
    if (signature == NULL || signature->slots.count == 0) ok = false;
    if (ok) {
        size_t slots = signature->slots.count;
        --signature->slots.count;
        SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
        ok = !sol_mir_runtime_conventions_validate(&pipeline.conventions, NULL)
            && !sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)
            && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &output, &pipeline.diagnostics)
                == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE
            && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage);
        sol_wasm_represented_output_free(&output);
        signature->slots.count = slots;
    }
    propagation_pipeline_free(&pipeline);
    return ok;
}

/* Both callback sites are source-derived and pass the complete P3.6 lowered
 * validation.  C1 deliberately stops before Wasm emission because one MVP
 * funcref table may carry only one physical indirect-call signature. */
static bool callback_mixed_signatures_rejected(const char *directory) {
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    bool ok = propagation_pipeline_build(&pipeline, directory);
    size_t callbacks = 0, unary = 0, nullary = 0;
    for (size_t i = 0; ok && i < pipeline.conventions.call_count; ++i) {
        const SolMirRuntimeCall *call = &pipeline.conventions.calls[i];
        if (call->call_kind != SOL_IR_CALL_CALLBACK) continue;
        if (call->signature >= pipeline.conventions.signature_count) { ok = false; break; }
        const SolMirRuntimeSignature *signature = &pipeline.conventions.signatures[call->signature];
        if (signature->slots.count == 1) ++unary;
        else if (signature->slots.count == 0) ++nullary;
        else ok = false;
        bool lowered = false;
        for (size_t row = 0; row < pipeline.lowered.call_count; ++row)
            lowered = lowered || (pipeline.lowered.calls[row].state == SOL_MIR_RUNTIME_LOWERED_PRESENT
                && pipeline.lowered.calls[row].call == i
                && pipeline.lowered.calls[row].call_kind == SOL_IR_CALL_CALLBACK);
        ok = ok && lowered; ++callbacks;
    }
    SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
    if (ok) ok = callbacks == 2 && unary == 1 && nullary == 1
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory,
            NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE
        && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage);
    sol_wasm_represented_output_free(&output); propagation_pipeline_free(&pipeline);
    return ok;
}

/* C2b's structural hook deliberately inspects the independently owned P3
 * records before trying forged variants at the represented boundary.  In
 * particular, the only WRITEBACK belongs to the normal transition; the
 * backend's failure body rejects that action class outright. */
static bool callback_inout_authentication(const char *directory) {
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    bool ok = propagation_pipeline_build(&pipeline, directory);
    size_t call_id = SOL_MIR_RUNTIME_NONE, normal_action = SOL_MIR_RUNTIME_NONE;
    size_t normal_transition = SOL_MIR_RUNTIME_NONE, failure_transition = SOL_MIR_RUNTIME_NONE;
    size_t failure_writebacks = 0, overflow_sites = 0;
    if (ok) for (size_t i = 0; i < pipeline.conventions.call_count; ++i)
        if (pipeline.conventions.calls[i].call_kind == SOL_IR_CALL_CALLBACK) {
            if (call_id != SOL_MIR_RUNTIME_NONE) ok = false;
            call_id = i;
        }
    SolMirRuntimeCall *call = ok && call_id != SOL_MIR_RUNTIME_NONE
        ? &pipeline.conventions.calls[call_id] : NULL;
    SolMirRuntimeSignature *signature = call != NULL && call->signature < pipeline.conventions.signature_count
        ? &pipeline.conventions.signatures[call->signature] : NULL;
    SolMirRuntimeWriteback *writeback = call != NULL && call->writebacks.count == 1
        && call->writebacks.offset < pipeline.conventions.writeback_count
        ? &pipeline.conventions.writebacks[call->writebacks.offset] : NULL;
    const SolMirMaterializedTerminator *term = call != NULL && call->block
        < pipeline.concrete.materialization.block_count
        ? &pipeline.concrete.materialization.blocks[call->block].terminator : NULL;
    SolMirMaterializedWriteback *materialized = term != NULL && term->writebacks.count == 1
        && term->writebacks.offset < pipeline.concrete.materialization.writeback_count
        ? &pipeline.concrete.materialization.writebacks[term->writebacks.offset] : NULL;
    if (call == NULL || signature == NULL || writeback == NULL || term == NULL || materialized == NULL || call->block
        >= pipeline.lowered.image_terminator_count) ok = false;
    const SolMirRuntimeLoweredImageTerminator *row = ok
        ? &pipeline.lowered.image_terminators[call->block] : NULL;
    const SolMirRuntimeCleanupEvent *event = row != NULL && row->cleanup_event < pipeline.cleanup.event_count
        ? &pipeline.cleanup.events[row->cleanup_event] : NULL;
    if (event == NULL) ok = false;
    if (ok) for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *transition = &pipeline.cleanup.transitions[
            event->transitions.offset + i];
        if (transition->actions.offset > pipeline.cleanup.action_count
            || transition->actions.count > pipeline.cleanup.action_count - transition->actions.offset) {
            ok = false; break;
        }
        for (size_t q = 0; q < transition->actions.count; ++q) {
            size_t action_id = transition->actions.offset + q;
            const SolMirRuntimeCleanupAction *action = &pipeline.cleanup.actions[action_id];
            if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL
                && action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK) normal_action = normal_action
                    == SOL_MIR_RUNTIME_NONE ? action_id : SIZE_MAX;
            if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE) {
                if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK) ++failure_writebacks;
            }
        }
        if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL)
            normal_transition = normal_transition == SOL_MIR_RUNTIME_NONE ? event->transitions.offset + i
                : SIZE_MAX;
        if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE)
            failure_transition = failure_transition == SOL_MIR_RUNTIME_NONE ? event->transitions.offset + i
                : SIZE_MAX;
    }
    size_t callback_target = call != NULL && call->table < pipeline.concrete.linkage.table_entry_count
        ? pipeline.concrete.linkage.table_entries[call->table].internal : SOL_MIR_RUNTIME_NONE;
    size_t callback_image = callback_target < pipeline.concrete.linkage.callable_count
        ? pipeline.concrete.linkage.callables[callback_target].instance : SOL_MIR_RUNTIME_NONE;
    if (ok) for (size_t i = 0; i < pipeline.lowered.image_instruction_count; ++i) {
        const SolMirRuntimeLoweredImageInstruction *instruction = &pipeline.lowered.image_instructions[i];
        if (instruction->image != callback_image || instruction->failure_site
            >= pipeline.conventions.failure_site_count) continue;
        const SolMirRuntimeFailureSite *site = &pipeline.conventions.failure_sites[instruction->failure_site];
        if (site->origin_kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_ARITHMETIC
            && site->allowed_codes == (UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW - 1)))
            ++overflow_sites;
    }
    ok = ok && signature->slots.count == 1 && signature->result_class
        == SOL_MIR_RUNTIME_RESULT_UNIT && call->result.kind == SOL_MIR_RUNTIME_VALUE_NONE
        && call->writebacks.count == 1 && normal_action != SOL_MIR_RUNTIME_NONE
        && normal_action != SIZE_MAX && normal_transition != SOL_MIR_RUNTIME_NONE
        && normal_transition != SIZE_MAX && failure_transition != SOL_MIR_RUNTIME_NONE
        && failure_transition != SIZE_MAX && failure_writebacks == 0
        && overflow_sites == 1;
#define CHECK_C2B_REJECT(mutate, restore) do { \
    SolWasmRepresentedOutput rejected; sol_wasm_represented_output_init(&rejected); \
    mutate; ok = ok && !sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL) \
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){ \
        &pipeline.lowered, directory, NULL}, &rejected, &pipeline.diagnostics) \
        == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE && rejected.bytes.bytes == NULL \
        && rejected.bytes.count == 0 && usage_zero(&rejected.usage); restore; \
    ok = ok && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL); \
    sol_wasm_represented_output_free(&rejected); \
} while (0)
    if (ok) {
        SolAccessMode access = pipeline.conventions.signature_slots[signature->slots.offset].access;
        CHECK_C2B_REJECT(pipeline.conventions.signature_slots[signature->slots.offset].access
            = SOL_ACCESS_OWNED, pipeline.conventions.signature_slots[signature->slots.offset].access = access);
        size_t operand = writeback->operand;
        CHECK_C2B_REJECT(writeback->operand = SOL_MIR_RUNTIME_NONE, writeback->operand = operand);
        size_t lowered_writebacks = pipeline.lowered.calls[call_id].writebacks.count;
        CHECK_C2B_REJECT(pipeline.lowered.calls[call_id].writebacks.count = 0,
            pipeline.lowered.calls[call_id].writebacks.count = lowered_writebacks);
        size_t lowered_call = pipeline.lowered.calls[call_id].call;
        CHECK_C2B_REJECT(pipeline.lowered.calls[call_id].call = SOL_MIR_RUNTIME_NONE,
            pipeline.lowered.calls[call_id].call = lowered_call);
        size_t place = writeback->place;
        size_t alternate_place = SOL_MIR_MATERIALIZED_NONE;
        for (size_t i = 0; i < pipeline.concrete.materialization.place_count; ++i) {
            const SolMirMaterializedPlace *candidate = &pipeline.concrete.materialization.places[i];
            if (i != place && candidate->projections.count == 0 && candidate->final_type
                == pipeline.concrete.materialization.places[place].final_type) {
                alternate_place = i; break;
            }
        }
        ok = ok && alternate_place != SOL_MIR_MATERIALIZED_NONE;
        CHECK_C2B_REJECT(writeback->place = alternate_place, writeback->place = place);
        size_t materialized_place = materialized->place;
        CHECK_C2B_REJECT(materialized->place = alternate_place, materialized->place = materialized_place);
        bool materialized_receiver = materialized->receiver;
        CHECK_C2B_REJECT(materialized->receiver = !materialized_receiver,
            materialized->receiver = materialized_receiver);
        size_t materialized_formal = materialized->formal;
        CHECK_C2B_REJECT(materialized->formal = materialized_formal + 1,
            materialized->formal = materialized_formal);
        size_t formal = writeback->formal;
        CHECK_C2B_REJECT(writeback->formal = 1, writeback->formal = formal);
        SolMirRecipeId recipe = writeback->recipe;
        CHECK_C2B_REJECT(writeback->recipe = signature->result, writeback->recipe = recipe);
        SolMirMaterializedTypeId materialized_type = materialized->type;
        CHECK_C2B_REJECT(materialized->type = pipeline.concrete.materialization.values[term->result].type,
            materialized->type = materialized_type);
        SolMirRuntimeValueRef result = call->result;
        SolMirRuntimeValueRef mismatched_result = {SOL_MIR_RUNTIME_VALUE_MATERIALIZED_VALUE, term->result};
        CHECK_C2B_REJECT(call->result = mismatched_result, call->result = result);
        unsigned flags = pipeline.cleanup.actions[normal_action].flags;
        CHECK_C2B_REJECT(pipeline.cleanup.actions[normal_action].flags = 0,
            pipeline.cleanup.actions[normal_action].flags = flags);
        SolMirRuntimeCleanupActionKind kind = pipeline.cleanup.actions[normal_action].kind;
        CHECK_C2B_REJECT(pipeline.cleanup.actions[normal_action].kind
            = SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE,
            pipeline.cleanup.actions[normal_action].kind = kind);
        size_t action_target = pipeline.cleanup.actions[normal_action].target;
        CHECK_C2B_REJECT(pipeline.cleanup.actions[normal_action].target = alternate_place,
            pipeline.cleanup.actions[normal_action].target = action_target);
        SolMirRecipeId action_recipe = pipeline.cleanup.actions[normal_action].recipe;
        CHECK_C2B_REJECT(pipeline.cleanup.actions[normal_action].recipe = signature->result,
            pipeline.cleanup.actions[normal_action].recipe = action_recipe);
        SolMirRuntimeSlice failure_slice = pipeline.cleanup.transitions[failure_transition].actions;
        CHECK_C2B_REJECT(pipeline.cleanup.transitions[failure_transition].actions
            = pipeline.cleanup.transitions[normal_transition].actions,
            pipeline.cleanup.transitions[failure_transition].actions = failure_slice);
        size_t count = call->writebacks.count;
        CHECK_C2B_REJECT(call->writebacks.count = count + 1, call->writebacks.count = count);
        SolMirRuntimeResultClass result_class = signature->result_class;
        CHECK_C2B_REJECT(signature->result_class = SOL_MIR_RUNTIME_RESULT_VALUE,
            signature->result_class = result_class);
        size_t projections = pipeline.concrete.materialization.places[place].projections.count;
        CHECK_C2B_REJECT(pipeline.concrete.materialization.places[place].projections.count = 1,
            pipeline.concrete.materialization.places[place].projections.count = projections);
        size_t slots = signature->slots.count;
        CHECK_C2B_REJECT(signature->slots.count = 0, signature->slots.count = slots);
    }
#undef CHECK_C2B_REJECT
    propagation_pipeline_free(&pipeline);
    return ok;
}

/* Direct methods have no table or closure transport to hide behind.  Exercise
 * the complete receiver/evidence/writeback chain at the represented boundary:
 * every forged row must be rejected before physical emission and restoring it
 * must restore the independently validated P3.6 owner. */
static bool method_authentication(const char *directory) {
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    bool ok = propagation_pipeline_build(&pipeline, directory);
    size_t shared_id = SOL_MIR_RUNTIME_NONE, exclusive_id = SOL_MIR_RUNTIME_NONE;
    if (ok) for (size_t i = 0; i < pipeline.conventions.call_count; ++i) {
        const SolMirRuntimeCall *call = &pipeline.conventions.calls[i];
        if (call->call_kind != SOL_IR_CALL_METHOD || call->signature >= pipeline.conventions.signature_count)
            continue;
        const SolMirRuntimeSignature *signature = &pipeline.conventions.signatures[call->signature];
        if (signature->slots.count != 1 || signature->slots.offset >= pipeline.conventions.signature_slot_count)
            { ok = false; break; }
        SolAccessMode access = pipeline.conventions.signature_slots[signature->slots.offset].access;
        if (access == SOL_ACCESS_SHARED) shared_id = shared_id == SOL_MIR_RUNTIME_NONE ? i : SIZE_MAX;
        else if (access == SOL_ACCESS_EXCLUSIVE) exclusive_id = exclusive_id == SOL_MIR_RUNTIME_NONE ? i : SIZE_MAX;
        else ok = false;
    }
    SolMirRuntimeCall *shared = shared_id < pipeline.conventions.call_count
        ? &pipeline.conventions.calls[shared_id] : NULL;
    SolMirRuntimeCall *exclusive = exclusive_id < pipeline.conventions.call_count
        ? &pipeline.conventions.calls[exclusive_id] : NULL;
    SolMirRuntimeSignature *shared_signature = shared != NULL
        && shared->signature < pipeline.conventions.signature_count
        ? &pipeline.conventions.signatures[shared->signature] : NULL;
    SolMirRuntimeSignature *exclusive_signature = exclusive != NULL
        && exclusive->signature < pipeline.conventions.signature_count
        ? &pipeline.conventions.signatures[exclusive->signature] : NULL;
    const SolMirMaterializedTerminator *shared_term_const = shared != NULL
        && shared->block < pipeline.concrete.materialization.block_count
        ? &pipeline.concrete.materialization.blocks[shared->block].terminator : NULL;
    SolMirMaterializedTerminator *shared_term = (SolMirMaterializedTerminator *)shared_term_const;
    const SolMirMaterializedTerminator *exclusive_term_const = exclusive != NULL
        && exclusive->block < pipeline.concrete.materialization.block_count
        ? &pipeline.concrete.materialization.blocks[exclusive->block].terminator : NULL;
    SolMirMaterializedTerminator *exclusive_term = (SolMirMaterializedTerminator *)exclusive_term_const;
    SolMirRuntimeWriteback *writeback = exclusive != NULL && exclusive->writebacks.count == 1
        && exclusive->writebacks.offset < pipeline.conventions.writeback_count
        ? &pipeline.conventions.writebacks[exclusive->writebacks.offset] : NULL;
    SolMirMaterializedWriteback *materialized = exclusive_term != NULL
        && exclusive_term->writebacks.count == 1
        && exclusive_term->writebacks.offset < pipeline.concrete.materialization.writeback_count
        ? &pipeline.concrete.materialization.writebacks[exclusive_term->writebacks.offset] : NULL;
    if (shared == NULL || exclusive == NULL || shared_signature == NULL || exclusive_signature == NULL
        || shared_term == NULL || exclusive_term == NULL || writeback == NULL || materialized == NULL
        || shared->operands.count != 1 || exclusive->operands.count != 1
        || shared->operands.offset >= pipeline.conventions.operand_count
        || exclusive->operands.offset >= pipeline.conventions.operand_count) ok = false;
    SolMirRuntimeCleanupEvent *event = ok && exclusive->block < pipeline.lowered.image_terminator_count
        && pipeline.lowered.image_terminators[exclusive->block].cleanup_event < pipeline.cleanup.event_count
        ? &pipeline.cleanup.events[pipeline.lowered.image_terminators[exclusive->block].cleanup_event] : NULL;
    size_t normal_action = SOL_MIR_RUNTIME_NONE, normal_transition = SOL_MIR_RUNTIME_NONE;
    size_t failure_transition = SOL_MIR_RUNTIME_NONE;
    if (event == NULL) ok = false;
    if (ok) for (size_t i = 0; i < event->transitions.count; ++i) {
        size_t transition_id = event->transitions.offset + i;
        if (transition_id >= pipeline.cleanup.transition_count) { ok = false; break; }
        SolMirRuntimeCleanupTransition *transition = &pipeline.cleanup.transitions[transition_id];
        if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL)
            normal_transition = normal_transition == SOL_MIR_RUNTIME_NONE ? transition_id : SIZE_MAX;
        if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE)
            failure_transition = failure_transition == SOL_MIR_RUNTIME_NONE ? transition_id : SIZE_MAX;
        for (size_t q = 0; q < transition->actions.count; ++q) {
            size_t action_id = transition->actions.offset + q;
            if (action_id >= pipeline.cleanup.action_count) { ok = false; break; }
            if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL
                && pipeline.cleanup.actions[action_id].kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK)
                normal_action = normal_action == SOL_MIR_RUNTIME_NONE ? action_id : SIZE_MAX;
        }
    }
    SolIrDispatchEvidence *evidence = NULL;
    if (ok && shared_term->source_expression < pipeline.ir.expression_count) {
        const SolIrExpression *source = &pipeline.ir.expressions[shared_term->source_expression];
        if (source->kind == SOL_IR_EXPR_CALL && source->as.call.evidence.count == 1
            && source->as.call.evidence.offset < pipeline.ir.evidence_count)
            evidence = &pipeline.ir.evidence[source->as.call.evidence.offset];
    }
    ok = ok && evidence != NULL && normal_action != SOL_MIR_RUNTIME_NONE
        && normal_action != SIZE_MAX && normal_transition != SOL_MIR_RUNTIME_NONE
        && normal_transition != SIZE_MAX && failure_transition != SOL_MIR_RUNTIME_NONE
        && failure_transition != SIZE_MAX && shared_signature->result_class == SOL_MIR_RUNTIME_RESULT_VALUE
        && exclusive_signature->result_class == SOL_MIR_RUNTIME_RESULT_UNIT && writeback->receiver
        && writeback->formal == 0 && materialized->receiver && materialized->formal == 0;
#define CHECK_METHOD_REJECT(mutate, restore) do { \
    SolWasmRepresentedOutput rejected; sol_wasm_represented_output_init(&rejected); \
    mutate; bool mutation_ok = !sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL) \
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, \
            &rejected, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE \
        && rejected.bytes.bytes == NULL && rejected.bytes.count == 0 && usage_zero(&rejected.usage); \
    ok = ok && mutation_ok; \
    restore; ok = ok && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL); \
    sol_wasm_represented_output_free(&rejected); \
} while (0)
    if (ok) {
        SolMirRuntimeSignatureSlot *shared_slot = &pipeline.conventions.signature_slots[
            shared_signature->slots.offset];
        SolMirRuntimeOperand *shared_operand = &pipeline.conventions.operands[shared->operands.offset];
        SolAccessMode access = shared_slot->access;
        CHECK_METHOD_REJECT(shared_slot->access = SOL_ACCESS_OWNED, shared_slot->access = access);
        SolMirRuntimeSlotRole role = shared_slot->role;
        CHECK_METHOD_REJECT(shared_slot->role = SOL_MIR_RUNTIME_SLOT_PARAMETER, shared_slot->role = role);
        SolIrCallKind kind = shared->call_kind;
        CHECK_METHOD_REJECT(shared->call_kind = SOL_IR_CALL_FUNCTION, shared->call_kind = kind);
        SolMirRuntimeCallTargetKind target = shared->target_kind;
        CHECK_METHOD_REJECT(shared->target_kind = (SolMirRuntimeCallTargetKind)99, shared->target_kind = target);
        size_t binding = shared_term->binding;
        CHECK_METHOD_REJECT(shared_term->binding = pipeline.concrete.materialization.binding_count,
            shared_term->binding = binding);
        SolIrCallableId method = evidence->method;
        CHECK_METHOD_REJECT(evidence->method = pipeline.ir.callable_count, evidence->method = method);
        size_t operand_place = shared_operand->value.id;
        CHECK_METHOD_REJECT(shared_operand->value.id = pipeline.concrete.materialization.place_count,
            shared_operand->value.id = operand_place);
        size_t receiver_place = shared_term->receiver.place;
        CHECK_METHOD_REJECT(shared_term->receiver.place = pipeline.concrete.materialization.place_count,
            shared_term->receiver.place = receiver_place);
        SolMirMaterializedTypeId receiver_type = shared_term->receiver.type;
        CHECK_METHOD_REJECT(shared_term->receiver.type = pipeline.concrete.materialization.type_count,
            shared_term->receiver.type = receiver_type);
        bool receiver = writeback->receiver;
        CHECK_METHOD_REJECT(writeback->receiver = !receiver, writeback->receiver = receiver);
        size_t formal = writeback->formal;
        CHECK_METHOD_REJECT(writeback->formal = 1, writeback->formal = formal);
        size_t writeback_operand = writeback->operand;
        CHECK_METHOD_REJECT(writeback->operand = pipeline.conventions.operand_count,
            writeback->operand = writeback_operand);
        size_t writeback_place = writeback->place;
        CHECK_METHOD_REJECT(writeback->place = pipeline.concrete.materialization.place_count,
            writeback->place = writeback_place);
        SolMirRecipeId recipe = writeback->recipe;
        CHECK_METHOD_REJECT(writeback->recipe = pipeline.concrete.representation.recipe_count,
            writeback->recipe = recipe);
        bool materialized_receiver = materialized->receiver;
        CHECK_METHOD_REJECT(materialized->receiver = !materialized_receiver,
            materialized->receiver = materialized_receiver);
        size_t materialized_formal = materialized->formal;
        CHECK_METHOD_REJECT(materialized->formal = 1, materialized->formal = materialized_formal);
        size_t materialized_place = materialized->place;
        CHECK_METHOD_REJECT(materialized->place = pipeline.concrete.materialization.place_count,
            materialized->place = materialized_place);
        SolMirRuntimeCleanupAction *action = &pipeline.cleanup.actions[normal_action];
        SolMirRuntimeCleanupActionKind action_kind = action->kind;
        CHECK_METHOD_REJECT(action->kind = SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE,
            action->kind = action_kind);
        unsigned flags = action->flags;
        CHECK_METHOD_REJECT(action->flags = 0, action->flags = flags);
        SolMirRuntimeSlice failure_actions = pipeline.cleanup.transitions[failure_transition].actions;
        CHECK_METHOD_REJECT(pipeline.cleanup.transitions[failure_transition].actions
                = pipeline.cleanup.transitions[normal_transition].actions,
            pipeline.cleanup.transitions[failure_transition].actions = failure_actions);
        SolMirRuntimeResultClass result_class = shared_signature->result_class;
        CHECK_METHOD_REJECT(shared_signature->result_class = SOL_MIR_RUNTIME_RESULT_UNIT,
            shared_signature->result_class = result_class);
        size_t normal_edge = shared->normal_edge;
        CHECK_METHOD_REJECT(shared->normal_edge = pipeline.concrete.materialization.edge_count,
            shared->normal_edge = normal_edge);
        SolMirRuntimeFailureSiteId site = shared->failure_site;
        CHECK_METHOD_REJECT(shared->failure_site = pipeline.conventions.failure_site_count,
            shared->failure_site = site);
        uint32_t mask = pipeline.cleanup.transitions[failure_transition].failure_mask;
        CHECK_METHOD_REJECT(pipeline.cleanup.transitions[failure_transition].failure_mask ^= UINT32_C(1),
            pipeline.cleanup.transitions[failure_transition].failure_mask = mask);
        size_t lowered_call = pipeline.lowered.calls[shared_id].call;
        CHECK_METHOD_REJECT(pipeline.lowered.calls[shared_id].call = SOL_MIR_RUNTIME_NONE,
            pipeline.lowered.calls[shared_id].call = lowered_call);
    }
#undef CHECK_METHOD_REJECT
    propagation_pipeline_free(&pipeline);
    return ok;
}

static bool verify_propagation_build_controls(const char *directory,
    const SolWasmRepresentedOutput *baseline, bool fault_sweep) {
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    bool ok = propagation_pipeline_build(&pipeline, directory);
    if (!ok) { propagation_pipeline_free(&pipeline); return false; }
    if (strstr(directory, "p43_propagate_option_success") != NULL) {
        uint64_t success_requests = 0, success_bytes = 0;
        size_t constructors = 0, copies = 0, propagation = SOL_MIR_RUNTIME_LOWERED_NONE;
        for (size_t i = 0; i < pipeline.concrete.operations.constructor_count; ++i) {
            const SolMirOperationConstructPlan *plan = &pipeline.concrete.operations.constructors[i];
            const SolMirRuntimeAllocationPlan *allocation = plan->result_recipe
                < pipeline.values.allocation_plan_count
                ? &pipeline.values.allocation_plans[plan->result_recipe] : NULL;
            if (allocation == NULL || allocation->kind != SOL_MIR_RUNTIME_ALLOCATION_PLAN_FIXED_OBJECT)
                continue;
            size_t owners = 0;
            for (size_t row = 0; row < pipeline.lowered.semantic_plan_count; ++row)
                owners += pipeline.lowered.semantic_plans[row].state == SOL_MIR_RUNTIME_LOWERED_PRESENT
                    && pipeline.lowered.semantic_plans[row].arena
                        == SOL_MIR_RUNTIME_LOWERED_SEMANTIC_CONSTRUCT
                    && pipeline.lowered.semantic_plans[row].plan == i
                    && pipeline.lowered.semantic_plans[row].producer == plan->instruction;
            ok = ok && owners == 1;
            ++constructors; ++success_requests; success_bytes += allocation->object_size;
        }
        for (size_t i = 0; i < pipeline.concrete.materialization.instruction_count; ++i) {
            const SolMirMaterializedInstruction *instruction =
                &pipeline.concrete.materialization.instructions[i];
            if (instruction->kind != SOL_MIR_INST_LOAD_COPY
                || instruction->type >= pipeline.values.allocation_plan_count
                || pipeline.values.allocation_plans[instruction->type].kind
                    != SOL_MIR_RUNTIME_ALLOCATION_PLAN_FIXED_OBJECT) continue;
            const SolMirRuntimeLoweredImageInstruction *row = i < pipeline.lowered.image_instruction_count
                ? &pipeline.lowered.image_instructions[i] : NULL;
            ok = ok && row != NULL && row->state == SOL_MIR_RUNTIME_LOWERED_PRESENT
                && row->instruction == i && (row->facilities & SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY);
            ++copies; ++success_requests;
            success_bytes += pipeline.values.allocation_plans[instruction->type].object_size;
        }
        for (size_t i = 0; i < pipeline.concrete.operations.propagation_count; ++i)
            if (pipeline.concrete.operations.propagations[i].source_residual_field_layout
                == SOL_MIR_OPERATION_NONE) propagation = i;
        const SolMirOperationPropagationPlan *plan = propagation
            < pipeline.concrete.operations.propagation_count
            ? &pipeline.concrete.operations.propagations[propagation] : NULL;
        const SolMirRuntimeLoweredImageTerminator *term = plan != NULL
            && plan->block < pipeline.lowered.image_terminator_count
            ? &pipeline.lowered.image_terminators[plan->block] : NULL;
        const SolMirRuntimeCleanupEvent *pre = term != NULL
            && term->pre_operation_cleanup_event < pipeline.cleanup.event_count
            ? &pipeline.cleanup.events[term->pre_operation_cleanup_event] : NULL;
        const SolMirRuntimeAllocationPlan *residual = plan != NULL
            && plan->residual_recipe < pipeline.values.allocation_plan_count
            ? &pipeline.values.allocation_plans[plan->residual_recipe] : NULL;
        const SolMirRuntimeCleanupEvent *main = term != NULL
            && term->cleanup_event < pipeline.cleanup.event_count
            ? &pipeline.cleanup.events[term->cleanup_event] : NULL;
        bool value_branch = false, residual_branch = false;
        if (main != NULL && plan != NULL
            && main->transitions.offset <= pipeline.cleanup.transition_count
            && main->transitions.count <= pipeline.cleanup.transition_count - main->transitions.offset)
            for (size_t i = 0; i < main->transitions.count; ++i) {
                const SolMirRuntimeCleanupTransition *transition =
                    &pipeline.cleanup.transitions[main->transitions.offset + i];
                value_branch = value_branch || (transition->edge_role
                    == SOL_MIR_RUNTIME_CLEANUP_EDGE_PROPAGATE_VALUE
                    && transition->continuation == plan->success_edge);
                residual_branch = residual_branch || (transition->edge_role
                    == SOL_MIR_RUNTIME_CLEANUP_EDGE_PROPAGATE_RESIDUAL
                    && transition->continuation == plan->residual_edge);
            }
        ok = ok && constructors == 3 && copies == 1 && success_requests == 4 && success_bytes == 64
            && plan != NULL && pre != NULL && pre->phase
                == SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_PROPAGATE_RESIDUAL
            && main != NULL && value_branch && residual_branch
            && residual != NULL && residual->kind == SOL_MIR_RUNTIME_ALLOCATION_PLAN_FIXED_OBJECT
            && residual->object_size == 16;
        /* The propagated residual has its own authenticated pre-event demand,
         * but the selected value branch does not execute it: the four owner-
         * checked constructors/copies consume exactly 4 requests and 64 bytes.
         * An eager pre-event request would be a fifth 16-byte allocation and
         * make this exact cap fail. */
        SolWasmRepresentedLimits exact = sol_wasm_represented_default_limits();
        SolWasmRepresentedOutput capped; exact.max_allocation_requests = 4;
        exact.max_allocation_bytes = 64; sol_wasm_represented_output_init(&capped);
        ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, &exact}, &capped, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && pipeline.conventions.entry_count == 1 && invoke_named(&capped.bytes,
                pipeline.conventions.entries[0].symbol.bytes, 101, 0, 0);
        sol_wasm_represented_output_free(&capped);
    }
    SolWasmRepresentedLimits defaults = {0}; SolWasmRepresentedOutput output;
    sol_wasm_represented_output_init(&output);
    ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory,
        &defaults}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && output.bytes.count == baseline->bytes.count
        && memcmp(output.bytes.bytes, baseline->bytes.bytes, baseline->bytes.count) == 0
        && usage_equal(&output.usage, &baseline->usage);
    sol_wasm_represented_output_free(&output);
#define CHECK_PROPAGATION_BUILD_CAP(field, exact) do { \
    SolWasmRepresentedLimits capped = sol_wasm_represented_default_limits(); \
    capped.field = (exact); sol_wasm_represented_output_init(&output); \
    ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, \
        directory, &capped}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK \
        && output.bytes.count == baseline->bytes.count \
        && memcmp(output.bytes.bytes, baseline->bytes.bytes, baseline->bytes.count) == 0 \
        && usage_equal(&output.usage, &baseline->usage); \
    sol_wasm_represented_output_free(&output); capped.field = (exact) - 1; \
    sol_wasm_represented_output_init(&output); \
    ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, \
        directory, &capped}, &output, &pipeline.diagnostics) \
            == SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED \
        && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage); \
    sol_wasm_represented_output_free(&output); \
} while (0)
    CHECK_PROPAGATION_BUILD_CAP(max_functions, baseline->usage.functions);
    CHECK_PROPAGATION_BUILD_CAP(max_blocks, baseline->usage.blocks);
    CHECK_PROPAGATION_BUILD_CAP(max_edges, baseline->usage.edges);
    CHECK_PROPAGATION_BUILD_CAP(max_values, baseline->usage.values);
    CHECK_PROPAGATION_BUILD_CAP(max_locals, baseline->usage.locals);
    CHECK_PROPAGATION_BUILD_CAP(max_generated_nodes, baseline->usage.generated_nodes);
    if (baseline->usage.static_data_bytes != 0)
        CHECK_PROPAGATION_BUILD_CAP(max_static_data_bytes, baseline->usage.static_data_bytes);
    CHECK_PROPAGATION_BUILD_CAP(max_provenance_records, baseline->usage.provenance_records);
    CHECK_PROPAGATION_BUILD_CAP(max_work_bytes, baseline->usage.work_bytes);
    CHECK_PROPAGATION_BUILD_CAP(max_scratch_bytes, baseline->usage.scratch_bytes);
    CHECK_PROPAGATION_BUILD_CAP(max_owned_bytes, baseline->usage.owned_bytes);
    CHECK_PROPAGATION_BUILD_CAP(max_output_bytes, baseline->usage.output_bytes);
#undef CHECK_PROPAGATION_BUILD_CAP
#define CHECK_PROPAGATION_PARTIAL_ZERO(field) do { \
    SolWasmRepresentedLimits partial = sol_wasm_represented_default_limits(); \
    partial.field = 0; sol_wasm_represented_output_init(&output); \
    ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, \
        directory, &partial}, &output, &pipeline.diagnostics) \
            == SOL_WASM_REPRESENTED_INVALID_ARGUMENT \
        && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage); \
    sol_wasm_represented_output_free(&output); \
} while (0)
    CHECK_PROPAGATION_PARTIAL_ZERO(max_functions); CHECK_PROPAGATION_PARTIAL_ZERO(max_blocks);
    CHECK_PROPAGATION_PARTIAL_ZERO(max_edges); CHECK_PROPAGATION_PARTIAL_ZERO(max_values);
    CHECK_PROPAGATION_PARTIAL_ZERO(max_locals); CHECK_PROPAGATION_PARTIAL_ZERO(max_generated_nodes);
    CHECK_PROPAGATION_PARTIAL_ZERO(max_table_elements);
    CHECK_PROPAGATION_PARTIAL_ZERO(max_static_data_bytes);
    CHECK_PROPAGATION_PARTIAL_ZERO(max_allocation_requests);
    CHECK_PROPAGATION_PARTIAL_ZERO(max_allocation_bytes);
    CHECK_PROPAGATION_PARTIAL_ZERO(max_provenance_records);
    CHECK_PROPAGATION_PARTIAL_ZERO(max_work_bytes);
    CHECK_PROPAGATION_PARTIAL_ZERO(max_scratch_bytes);
    CHECK_PROPAGATION_PARTIAL_ZERO(max_owned_bytes);
    CHECK_PROPAGATION_PARTIAL_ZERO(max_output_bytes);
#undef CHECK_PROPAGATION_PARTIAL_ZERO
    if (fault_sweep) {
        sol_wasm_represented_output_init(&output);
        ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
        sol_wasm_represented_output_free(&output);
        size_t attempts = sol_wasm_represented_test_allocation_attempts();
        /* Result residual's backend construction allocation sequence is part
         * of the metered B2 contract: ordinals are exactly 0 through 129. */
        ok = ok && attempts == 130;
        for (size_t ordinal = 0; ordinal < attempts; ++ordinal) {
            represented_fault(FAULT_PROPAGATION_RESULT, ordinal + 1, ordinal + 1);
            sol_wasm_represented_output_init(&output);
            ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){
                &pipeline.lowered, directory, NULL}, &output, &pipeline.diagnostics)
                    == SOL_WASM_REPRESENTED_ALLOCATION_FAILED
                && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage);
            sol_wasm_represented_output_free(&output);
            sol_wasm_represented_test_fail_allocation_after(0);
            sol_wasm_represented_output_init(&output);
            ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){
                &pipeline.lowered, directory, NULL}, &output, &pipeline.diagnostics)
                    == SOL_WASM_REPRESENTED_OK && output.bytes.count == baseline->bytes.count
                && memcmp(output.bytes.bytes, baseline->bytes.bytes, baseline->bytes.count) == 0
                && usage_equal(&output.usage, &baseline->usage);
            sol_wasm_represented_output_free(&output);
        }
        sol_wasm_represented_test_fail_allocation_after(0);
        sol_wasm_represented_output_init(&output);
        ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && output.bytes.count == baseline->bytes.count
            && memcmp(output.bytes.bytes, baseline->bytes.bytes, baseline->bytes.count) == 0
            && usage_equal(&output.usage, &baseline->usage)
            && sol_wasm_represented_test_allocation_attempts() == 130;
        sol_wasm_represented_output_free(&output);
        sol_wasm_represented_test_fail_allocation_after(0);
    }
    propagation_pipeline_free(&pipeline);
    return ok;
}

static bool verify_propagation_owner(const char *directory) {
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    bool ok = propagation_pipeline_build(&pipeline, directory)
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL);
    size_t plan_id = SOL_MIR_RUNTIME_LOWERED_NONE;
    if (ok) for (size_t i = 0; i < pipeline.concrete.operations.propagation_count; ++i) {
        const SolMirOperationPropagationPlan *candidate = &pipeline.concrete.operations.propagations[i];
        if (candidate->source_residual_field_layout != SOL_MIR_OPERATION_NONE) {
            if (plan_id != SOL_MIR_RUNTIME_LOWERED_NONE) ok = false;
            plan_id = i;
        }
    }
    if (!ok || plan_id == SOL_MIR_RUNTIME_LOWERED_NONE) {
        propagation_pipeline_free(&pipeline);
        return false;
    }
    SolMirOperationPropagationPlan *plan = &pipeline.concrete.operations.propagations[plan_id];
    SolMirRuntimeLoweredImageTerminator *term = plan->block < pipeline.lowered.image_terminator_count
        ? &pipeline.lowered.image_terminators[plan->block] : NULL;
    SolMirRuntimeCleanupEvent *pre = term != NULL
        && term->pre_operation_cleanup_event < pipeline.cleanup.event_count
        ? &pipeline.cleanup.events[term->pre_operation_cleanup_event] : NULL;
    SolMirRuntimeCleanupSupplementalSite *site = pre != NULL
        && pre->supplemental_site < pipeline.cleanup.supplemental_site_count
        ? &pipeline.cleanup.supplemental_sites[pre->supplemental_site] : NULL;
    SolMirRuntimeCleanupTransition *ready = NULL, *failure = NULL;
    if (pre != NULL && pre->transitions.offset <= pipeline.cleanup.transition_count
        && pre->transitions.count <= pipeline.cleanup.transition_count - pre->transitions.offset)
        for (size_t i = 0; i < pre->transitions.count; ++i) {
            SolMirRuntimeCleanupTransition *candidate =
                &pipeline.cleanup.transitions[pre->transitions.offset + i];
            if (candidate->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_PRE_OPERATION_READY) ready = candidate;
            if (candidate->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE) failure = candidate;
        }
    SolMirRuntimeCleanupAction *action = failure != NULL && failure->actions.count != 0
        && failure->actions.offset < pipeline.cleanup.action_count
        ? &pipeline.cleanup.actions[failure->actions.offset + failure->actions.count - 1] : NULL;
    ok = ok && term != NULL && pre != NULL && site != NULL && ready != NULL && failure != NULL
        && action != NULL && plan->source_residual_field_offset
            != plan->destination_residual_field_offset;
#define CHECK_PROPAGATION_OWNER_MUTATION(edit, restore) do { \
    edit; pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
    ok = ok && !sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL) \
        && propagation_owner_rejected(&pipeline, directory); \
    restore; pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
    ok = ok && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL); \
} while (0)
    if (ok) {
        SolMirRecipeId recipe = plan->source_recipe;
        CHECK_PROPAGATION_OWNER_MUTATION(plan->source_recipe = plan->success_recipe,
            plan->source_recipe = recipe);
        recipe = plan->success_recipe;
        CHECK_PROPAGATION_OWNER_MUTATION(plan->success_recipe = plan->source_recipe,
            plan->success_recipe = recipe);
        recipe = plan->residual_recipe;
        CHECK_PROPAGATION_OWNER_MUTATION(plan->residual_recipe = plan->source_recipe,
            plan->residual_recipe = recipe);
        uint32_t tag = plan->success_tag;
        CHECK_PROPAGATION_OWNER_MUTATION(plan->success_tag ^= 1u, plan->success_tag = tag);
        tag = plan->source_residual_tag;
        CHECK_PROPAGATION_OWNER_MUTATION(plan->source_residual_tag ^= 1u,
            plan->source_residual_tag = tag);
        tag = plan->destination_residual_tag;
        CHECK_PROPAGATION_OWNER_MUTATION(plan->destination_residual_tag ^= 1u,
            plan->destination_residual_tag = tag);
        uint64_t offset = plan->success_field_offset;
        CHECK_PROPAGATION_OWNER_MUTATION(++plan->success_field_offset,
            plan->success_field_offset = offset);
        offset = plan->source_residual_field_offset;
        CHECK_PROPAGATION_OWNER_MUTATION(++plan->source_residual_field_offset,
            plan->source_residual_field_offset = offset);
        offset = plan->destination_residual_field_offset;
        CHECK_PROPAGATION_OWNER_MUTATION(++plan->destination_residual_field_offset,
            plan->destination_residual_field_offset = offset);
        size_t edge = plan->success_edge;
        CHECK_PROPAGATION_OWNER_MUTATION(plan->success_edge = plan->residual_edge,
            plan->success_edge = edge);
        edge = plan->residual_edge;
        CHECK_PROPAGATION_OWNER_MUTATION(plan->residual_edge = plan->success_edge,
            plan->residual_edge = edge);
        size_t event = term->pre_operation_cleanup_event;
        CHECK_PROPAGATION_OWNER_MUTATION(term->pre_operation_cleanup_event = SOL_MIR_RUNTIME_LOWERED_NONE,
            term->pre_operation_cleanup_event = event);
        size_t supplemental = term->pre_operation_supplemental_site;
        CHECK_PROPAGATION_OWNER_MUTATION(term->pre_operation_supplemental_site = SOL_MIR_RUNTIME_LOWERED_NONE,
            term->pre_operation_supplemental_site = supplemental);
        SolMirRuntimeCleanupEdgeRole role = ready->edge_role;
        CHECK_PROPAGATION_OWNER_MUTATION(ready->edge_role = SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO,
            ready->edge_role = role);
        SolMirRuntimeCleanupActionKind kind = action->kind;
        CHECK_PROPAGATION_OWNER_MUTATION(action->kind = SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE,
            action->kind = kind);
    }
#undef CHECK_PROPAGATION_OWNER_MUTATION
    propagation_pipeline_free(&pipeline);
    return ok;
}

/* This deliberately rebuilds the entire authenticated P2/P3 chain rather than
 * manufacturing a layout: the Unit field contract is a boundary invariant of
 * the actual fixture closure consumed by the represented backend. */
static bool verify_unit_sum_pipeline(const char *directory) {
    SolDiagnostics diagnostics; SolHirModule hir; SolTypeTable types; SolEffectTable effects;
    SolContractTable contracts; SolIr ir; SolPackage package; SolMirConcreteProgram concrete;
    SolMirRuntimeConventions conventions; SolMirRuntimeValues values; SolMirRuntimeCleanup cleanup;
    SolMirRuntimeHostAbi host; SolMirRuntimeHandlerAbi handler; SolMirRuntimeLoweredProgram lowered;
    sol_diagnostics_init(&diagnostics); sol_hir_module_init(&hir); sol_type_table_init(&types);
    sol_effect_table_init(&effects); sol_contract_table_init(&contracts); sol_ir_init(&ir);
    sol_package_init(&package); sol_mir_concrete_program_init(&concrete);
    sol_mir_runtime_conventions_init(&conventions); sol_mir_runtime_values_init(&values);
    sol_mir_runtime_cleanup_init(&cleanup); sol_mir_runtime_host_abi_init(&host);
    sol_mir_runtime_handler_abi_init(&handler); sol_mir_runtime_lowered_program_init(&lowered);
    char error[256]; bool ok = sol_package_load_directory(&package, directory, &diagnostics, error,
        sizeof error);
    SolHirFileScope scope = {0};
    if (ok) scope = (SolHirFileScope){package.files[0].module_name, package.files[0].import_start,
        package.files[0].import_count, package.files[0].item_start, package.files[0].item_count};
    if (ok) ok = sol_hir_lower_scoped(&package.source, &package.syntax, &scope, 1, &hir, &diagnostics)
        && sol_type_check(&package.source, &package.syntax, &hir, &types, &diagnostics)
        && sol_effect_check(&package.source, &package.syntax, &hir, &types, &effects, &diagnostics)
        && sol_contract_lower(&package.source, &package.syntax, &hir, &types, &effects, &contracts,
            &diagnostics)
        && sol_ir_lower_scoped(&package.source, &package.syntax, &hir, &types, &effects, &contracts,
            package.files, 1, &ir, &diagnostics);
    SolMirProgramRoot roots[2]; size_t root_count = 0;
    if (ok) for (size_t i = 0; i < ir.callable_count; ++i) {
        if (ir.callables[i].kind != SOL_IR_CALLABLE_FUNCTION) continue;
        if (!strcmp(ir.callables[i].name, "first") || !strcmp(ir.callables[i].name, "second"))
            roots[root_count++] = (SolMirProgramRoot){i, !strcmp(ir.callables[i].name, "first")
                ? SOL_MIR_PROGRAM_ROOT_ENTRY : SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    }
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    if (ok) ok = root_count == 2 && sol_mir_concrete_program_build(
        &(SolMirConcreteBuildRequest){&ir, roots, root_count, NULL, 0, &target, NULL}, &concrete,
        &diagnostics) == SOL_MIR_CONCRETE_BUILD_SUCCEEDED
        && sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&concrete, NULL},
            &conventions, &diagnostics) == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        && sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&conventions, NULL},
            &values, &diagnostics) == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED
        && sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&conventions, &values, NULL},
            &cleanup, &diagnostics) == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED
        && sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&conventions, &values,
            &cleanup, NULL}, &host, &diagnostics) == SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED
        && sol_mir_runtime_handler_abi_build(&(SolMirRuntimeHandlerAbiBuildRequest){&conventions,
            &values, &cleanup, &host, NULL}, &handler, &diagnostics)
            == SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_SUCCEEDED
        && sol_mir_runtime_lowered_program_build(&(SolMirRuntimeLoweredProgramBuildRequest){&conventions,
            &values, &cleanup, &host, &handler, NULL}, &lowered, &diagnostics)
            == SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED;
    size_t sum = SIZE_MAX, unit_field = SIZE_MAX, constructors = 0, copies = 0, equality_children = 0;
    unsigned constructor_tags = 0;
    if (ok) for (size_t recipe = 0; recipe < concrete.representation.recipe_count; ++recipe) {
        const SolMirRecipe *item = &concrete.representation.recipes[recipe];
        if (item->kind == SOL_MIR_RECIPE_ENUM && item->variants.count == 3) sum = recipe;
    }
    if (ok && sum != SIZE_MAX) {
        const SolMirRecipe *item = &concrete.representation.recipes[sum];
        ok = item->variants.count == 3 && item->variants.offset + 3 <= concrete.representation.variant_count;
        for (size_t i = 0; ok && i < 3; ++i) {
            const SolMirRecipeVariant *variant = &concrete.representation.variants[item->variants.offset + i];
            ok = variant->semantic_tag == i;
            if (i == 0) ok = variant->fields.count == 1;
            if (i == 0 && ok) unit_field = variant->fields.offset;
        }
        if (ok) ok = unit_field < concrete.layout.field_count
            && concrete.layout.fields[unit_field].owner_recipe == sum
            && concrete.layout.fields[unit_field].has_storage
            && concrete.layout.fields[unit_field].offset == 8
            && concrete.layout.fields[unit_field].size == 0
            && concrete.layout.fields[unit_field].alignment == 1;
        for (size_t i = 0; ok && i < concrete.operations.constructor_count; ++i) {
            const SolMirOperationConstructPlan *plan = &concrete.operations.constructors[i];
            if (plan->kind == SOL_MIR_OPERATION_CONSTRUCT_SUM && plan->result_recipe == sum) {
                ++constructors;
                if (plan->semantic_tag < 3) constructor_tags |= 1u << plan->semantic_tag;
            }
        }
        for (size_t i = 0; ok && i < concrete.materialization.instruction_count; ++i)
            if (concrete.materialization.instructions[i].kind == SOL_MIR_INST_LOAD_COPY
                && concrete.materialization.instructions[i].type < concrete.layout.type_count
                && concrete.layout.types[concrete.materialization.instructions[i].type].recipe == sum)
                ++copies;
        for (size_t i = 0; ok && i < concrete.operations.equality_node_count; ++i) {
            const SolMirOperationEqualityNode *node = &concrete.operations.equality_nodes[i];
            if (node->kind == SOL_MIR_OPERATION_EQUAL_SUM && node->recipe == sum)
                equality_children += node->children.count;
        }
        ok = ok && constructors == 4 && constructor_tags == 7 && copies >= 3 && equality_children == 12;
    } else ok = false;
    sol_mir_runtime_lowered_program_free(&lowered); sol_mir_runtime_handler_abi_free(&handler);
    sol_mir_runtime_host_abi_free(&host); sol_mir_runtime_cleanup_free(&cleanup);
    sol_mir_runtime_values_free(&values); sol_mir_runtime_conventions_free(&conventions);
    sol_mir_concrete_program_free(&concrete); sol_package_free(&package); sol_ir_free(&ir);
    sol_contract_table_free(&contracts); sol_effect_table_free(&effects); sol_type_table_free(&types);
    sol_hir_module_free(&hir); sol_diagnostics_free(&diagnostics);
    return ok;
}

/* Keep the frontend rejection boundary independent of the represented
 * backend: a non-total source match never supplies a P2/P3 closure to it. */
static bool frontend_rejects_non_total(const char *directory) {
    SolDiagnostics diagnostics; SolHirModule hir; SolTypeTable types; SolPackage package;
    sol_diagnostics_init(&diagnostics); sol_hir_module_init(&hir); sol_type_table_init(&types);
    sol_package_init(&package);
    char error[256];
    bool ok = sol_package_load_directory(&package, directory, &diagnostics, error, sizeof error);
    SolHirFileScope scope = {0};
    if (ok) scope = (SolHirFileScope){package.files[0].module_name, package.files[0].import_start,
        package.files[0].import_count, package.files[0].item_start, package.files[0].item_count};
    if (ok) ok = sol_hir_lower_scoped(&package.source, &package.syntax, &scope, 1, &hir,
        &diagnostics);
    if (ok) (void)sol_type_check(&package.source, &package.syntax, &hir, &types, &diagnostics);
    bool exhaustive = false;
    for (size_t i = 0; i < diagnostics.count; ++i) {
        exhaustive = exhaustive || (!strcmp(diagnostics.items[i].code, "SOL-MATCH-001")
            && strstr(diagnostics.items[i].message, "not exhaustive") != NULL);
    }
    sol_package_free(&package); sol_type_table_free(&types); sol_hir_module_free(&hir);
    sol_diagnostics_free(&diagnostics);
    return ok && exhaustive;
}

typedef enum {
    SHAPE_VALID_TABLE, SHAPE_IMPORT, SHAPE_START, SHAPE_MEMORY_EXPORT, SHAPE_MEMORY_INITIAL,
    SHAPE_MEMORY_MAXIMUM, SHAPE_CODE_GLOBAL, SHAPE_SITE_GLOBAL, SHAPE_EXTRA_ENTRY,
    SHAPE_HELPER_EXPORT, SHAPE_WRONG_MEMORY_EXPORT, SHAPE_MISSING_ENTRY, SHAPE_RENAMED_ENTRY,
    SHAPE_TABLE_EXTRA, SHAPE_TABLE_MAXIMUM, SHAPE_TABLE_EXPORT, SHAPE_ELEMENT_MISSING,
    SHAPE_ELEMENT_EXTRA,
    SHAPE_ELEMENT_OFFSET, SHAPE_ELEMENT_DUPLICATE, SHAPE_ELEMENT_FUNCTION,
    SHAPE_ELEMENT_MANY_LATE_TYPE, SHAPE_FUNCTION_OVERLIMIT,
} ShapeVariant;

static bool make_shape_variant(const SolWasmBackendBytes *reference, const ProvenanceLayout *layout,
    ShapeVariant variant, SolWasmBackendBytes *result) {
    ProvenanceRecord entry;
    if (!provenance_record(reference, 1, &entry) || entry.tag != 1 || entry.symbol_count >= 256)
        return false;
    char name[256]; memcpy(name, entry.symbol, entry.symbol_count); name[entry.symbol_count] = '\0';
    BinaryenModuleRef module = BinaryenModuleCreate();
    if (module == NULL) return false;
    bool reference_types = variant == SHAPE_TABLE_EXTRA;
    BinaryenModuleSetFeatures(module, BinaryenFeatureMVP() | BinaryenFeatureMutableGlobals()
        | (reference_types ? BinaryenFeatureReferenceTypes() : 0));
    BinaryenIndex initial = variant == SHAPE_MEMORY_INITIAL ? 2 : 1;
    BinaryenIndex maximum = variant == SHAPE_MEMORY_MAXIMUM ? 2 : 256;
    BinaryenSetMemory(module, initial, maximum, NULL, NULL, NULL, NULL, NULL, NULL, 0, false,
        false, SOL_WASM_BACKEND_MEMORY_EXPORT);
    if (variant != SHAPE_MEMORY_EXPORT) BinaryenAddMemoryExport(module,
        SOL_WASM_BACKEND_MEMORY_EXPORT, variant == SHAPE_WRONG_MEMORY_EXPORT ? "wrong.memory"
            : SOL_WASM_BACKEND_MEMORY_EXPORT);
    const char *code = variant == SHAPE_CODE_GLOBAL ? "wrong.code"
        : SOL_WASM_REPRESENTED_FAILURE_CODE_EXPORT;
    const char *site = variant == SHAPE_SITE_GLOBAL ? "wrong.site"
        : SOL_WASM_REPRESENTED_FAILURE_SITE_EXPORT;
    bool ok = BinaryenAddGlobal(module, code, BinaryenTypeInt32(), true,
        BinaryenConst(module, BinaryenLiteralInt32(0))) != NULL
        && BinaryenAddGlobal(module, site, BinaryenTypeInt32(), true,
            BinaryenConst(module, BinaryenLiteralInt32(0))) != NULL;
    if (ok) { BinaryenAddGlobalExport(module, code, code); BinaryenAddGlobalExport(module, site, site); }
    /* The synthetic table controls still carry the mandatory private meter
     * envelope, so their intended table mutation remains the rejection cause. */
    const char *const private_globals[] = {"heap", "max-requests", "max-bytes", "requests", "bytes",
        "sol.p44.max-steps", "sol.p44.steps", "sol.p44.max-call-depth", "sol.p44.call-depth"};
    for (size_t i = 0; ok && i < sizeof private_globals / sizeof *private_globals; ++i) {
        bool mutable = i == 0 || i == 3 || i == 4 || i == 6 || i == 8;
        ok = BinaryenAddGlobal(module, private_globals[i], i == 0 || i >= 7 ? BinaryenTypeInt32()
                : BinaryenTypeInt64(), mutable, i == 0 || i >= 7
                ? BinaryenConst(module, BinaryenLiteralInt32(i == 0 ? 1024 : i == 7 ? 64 : 1))
                : BinaryenConst(module, BinaryenLiteralInt64(i == 5 ? 100000 : 0))) != NULL;
    }
    BinaryenType entry_parameter[] = {BinaryenTypeInt64()};
    BinaryenType entry_parameters = variant == SHAPE_IMPORT ? BinaryenTypeCreate(entry_parameter, 1)
        : BinaryenTypeNone();
    BinaryenFunctionRef entry_function = ok ? BinaryenAddFunction(module, name, entry_parameters,
        BinaryenTypeInt64(), NULL, 0, BinaryenConst(module, BinaryenLiteralInt64(0))) : NULL;
    if (entry_function == NULL) ok = false;
    if (ok && variant != SHAPE_MISSING_ENTRY) BinaryenAddFunctionExport(module, name,
        variant == SHAPE_RENAMED_ENTRY ? "sol.e1.renamed" : name);
    bool table = true;
    BinaryenIndex table_initial = variant == SHAPE_ELEMENT_MANY_LATE_TYPE ? 256 : 4;
    BinaryenIndex table_maximum = variant == SHAPE_TABLE_MAXIMUM ? 5 : table_initial;
    BinaryenType table_type = BinaryenTypeFuncref();
    if (ok && table) ok = BinaryenAddTable(module, "unexpected", table_initial, table_maximum,
        table_type, NULL) != NULL;
    if (ok && variant == SHAPE_TABLE_EXTRA) ok = BinaryenAddTable(module, "unexpected.extra", 1, 1,
        BinaryenTypeFuncref(), NULL) != NULL;
    if (ok && variant == SHAPE_TABLE_EXPORT) BinaryenAddTableExport(module, "unexpected", "sol.p43.table");
    if (ok && variant == SHAPE_FUNCTION_OVERLIMIT) for (size_t i = 0; i < 256; ++i) {
        char helper[32];
        (void)snprintf(helper, sizeof helper, "many.function.%zu", i);
        if (BinaryenAddFunction(module, helper, BinaryenTypeNone(), BinaryenTypeInt64(), NULL, 0,
                BinaryenConst(module, BinaryenLiteralInt64(0))) == NULL) ok = false;
    }
    if (ok && table && table_type == BinaryenTypeFuncref() && variant != SHAPE_ELEMENT_MISSING) {
        if (variant == SHAPE_ELEMENT_MANY_LATE_TYPE) {
            char names[255][32]; const char *elements[255];
            BinaryenType parameter[] = {BinaryenTypeInt64()};
            BinaryenType unary = BinaryenTypeCreate(parameter, 1);
            for (size_t i = 0; i < 254; ++i) {
                (void)snprintf(names[i], sizeof names[i], "many.table.%zu", i);
                elements[i] = names[i];
                if (BinaryenAddFunction(module, names[i], unary, BinaryenTypeInt64(), NULL, 0,
                        BinaryenLocalGet(module, 0, BinaryenTypeInt64())) == NULL) ok = false;
            }
            (void)snprintf(names[254], sizeof names[254], "many.table.wrong");
            elements[254] = names[254];
            if (BinaryenAddFunction(module, names[254], BinaryenTypeNone(), BinaryenTypeInt32(), NULL,
                    0, BinaryenConst(module, BinaryenLiteralInt32(0))) == NULL
                || BinaryenAddActiveElementSegment(module, "unexpected", "unexpected.elements",
                    elements, 255, BinaryenConst(module, BinaryenLiteralInt32(1))) == NULL) ok = false;
        } else {
        BinaryenType parameter[] = {BinaryenTypeInt64()};
        BinaryenType unary = BinaryenTypeCreate(parameter, 1);
        if (variant == SHAPE_IMPORT) BinaryenAddFunctionImport(module, "foreign", "env", "foreign",
            unary, BinaryenTypeInt64());
        BinaryenFunctionRef first = BinaryenAddFunction(module, "table.first", unary,
            BinaryenTypeInt64(), NULL, 0, BinaryenLocalGet(module, 0, BinaryenTypeInt64()));
        BinaryenFunctionRef second = BinaryenAddFunction(module, "table.second", unary,
            BinaryenTypeInt64(), NULL, 0, BinaryenLocalGet(module, 0, BinaryenTypeInt64()));
        BinaryenFunctionRef third = BinaryenAddFunction(module, "table.third", unary,
            BinaryenTypeInt64(), NULL, 0, BinaryenLocalGet(module, 0, BinaryenTypeInt64()));
        const char *element_functions[] = {variant == SHAPE_IMPORT ? name : "table.first",
            variant == SHAPE_IMPORT ? "table.first" : "table.second",
            variant == SHAPE_IMPORT ? "table.second" : "table.third"};
        BinaryenExpressionRef offset = BinaryenConst(module, BinaryenLiteralInt32(
            variant == SHAPE_ELEMENT_OFFSET ? 0 : 1));
        if (first == NULL || second == NULL || third == NULL) ok = false;
        else if (variant == SHAPE_ELEMENT_DUPLICATE) {
            const char *duplicate_functions[] = {"table.first", "table.second", "table.second"};
            if (BinaryenAddActiveElementSegment(module, "unexpected", "unexpected.elements",
                    duplicate_functions, 3, offset) == NULL) ok = false;
        } else if (variant == SHAPE_ELEMENT_FUNCTION) {
            BinaryenFunctionRef wrong = BinaryenAddFunction(module, "wrong", BinaryenTypeNone(),
                BinaryenTypeInt32(), NULL, 0, BinaryenConst(module, BinaryenLiteralInt32(0)));
            const char *wrong_functions[] = {"table.first", "table.second", "wrong"};
            if (wrong == NULL || BinaryenAddActiveElementSegment(module, "unexpected",
                    "unexpected.elements", wrong_functions, 3, offset) == NULL) ok = false;
        } else if (BinaryenAddActiveElementSegment(module, "unexpected", "unexpected.elements",
                element_functions, 3, offset) == NULL) ok = false;
        if (ok && variant == SHAPE_ELEMENT_EXTRA) {
            BinaryenExpressionRef extra_offset = BinaryenConst(module, BinaryenLiteralInt32(1));
            if (BinaryenAddActiveElementSegment(module, "unexpected", "unexpected.extra.elements",
                    element_functions, 3, extra_offset) == NULL) ok = false;
        }
        }
    }
    if (ok && variant == SHAPE_START) {
        BinaryenFunctionRef start = BinaryenAddFunction(module, "start", BinaryenTypeNone(),
            BinaryenTypeNone(), NULL, 0, BinaryenNop(module));
        if (start == NULL) ok = false; else BinaryenSetStart(module, start);
    }
    if (ok && variant == SHAPE_EXTRA_ENTRY) BinaryenAddFunctionExport(module, name, "sol.e1.extra");
    if (ok && variant == SHAPE_HELPER_EXPORT) BinaryenAddFunctionExport(module, name, "sol.i1.helper");
    if (ok) BinaryenAddCustomSection(module, SOL_WASM_REPRESENTED_PROVENANCE_SECTION,
        (const char *)reference->bytes + layout->payload, (BinaryenIndex)layout->payload_count);
    if (!ok || !BinaryenModuleValidate(module)) { BinaryenModuleDispose(module); return false; }
    BinaryenModuleAllocateAndWriteResult written = BinaryenModuleAllocateAndWrite(module, NULL);
    BinaryenModuleDispose(module);
    if (written.binary == NULL || written.binaryBytes == 0) { free(written.binary); return false; }
    *result = (SolWasmBackendBytes){(uint8_t *)written.binary, written.binaryBytes};
    return true;
}

/* Preserve every accepted-control byte except the sole table element type.
 * A later Wasmtime failure would be WASMTIME_VALIDATION_FAILED; asserting the
 * earlier INVALID_INPUT result proves private funcref enforcement. */
static bool table_type_externref(const SolWasmBackendBytes *source, SolWasmBackendBytes *result) {
    const uint8_t *cursor = source->bytes + 8, *end = source->bytes + source->count;
    uint8_t *copy = source->count < 8 ? NULL : malloc(source->count);
    if (copy == NULL) return false;
    memcpy(copy, source->bytes, source->count);
    while (cursor < end) {
        uint8_t id = *cursor++; uint32_t size = 0;
        if (!read_uleb32(&cursor, end, &size) || size > (size_t)(end - cursor)) break;
        const uint8_t *section_end = cursor + size;
        if (id == 4) {
            uint32_t count = 0;
            if (!read_uleb32(&cursor, section_end, &count) || count != 1 || cursor == section_end
                || *cursor != UINT8_C(0x70)) break;
            copy[cursor - source->bytes] = UINT8_C(0x6f);
            *result = (SolWasmBackendBytes){copy, source->count};
            return true;
        }
        cursor = section_end;
    }
    free(copy); return false;
}

static bool wasmtime_module_valid(const SolWasmBackendBytes *bytes) {
    wasm_engine_t *engine = wasm_engine_new();
    wasm_store_t *store = engine == NULL ? NULL : wasm_store_new(engine);
    wasm_byte_vec_t input = {bytes->count, (wasm_byte_t *)bytes->bytes};
    bool valid = store != NULL && wasm_module_validate(store, &input);
    if (store != NULL) wasm_store_delete(store);
    if (engine != NULL) wasm_engine_delete(engine);
    return valid;
}

/* B1's normal controls are deliberately tested from a source-built P3.6 owner:
 * GOTO includes lowered break/continue, each branch arm is independently named,
 * and the fixture contains both value and Unit return exits.  The mutations are
 * made only after the complete owner is built and are resealed so the backend's
 * hostile-input boundary, rather than an unsealed-token shortcut, is exercised. */
static bool p44_b1_control_owners(const char *directory) {
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    SolWasmRepresentedOutput baseline, output;
    sol_wasm_represented_output_init(&baseline); sol_wasm_represented_output_init(&output);
    bool ok = propagation_pipeline_build(&pipeline, directory)
        && sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL)
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory,
            NULL}, &baseline, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
    const SolMirMaterialization *m = &pipeline.concrete.materialization;
    SolMirRuntimeLoweredImageTerminator *branch_row = NULL;
    SolMirRuntimeCleanupEvent *branch_event = NULL;
    SolMirRuntimeCleanupTransition *branch_true = NULL, *branch_false = NULL;
    size_t branch_block = SOL_MIR_RUNTIME_LOWERED_NONE;
    SolMirRuntimeLoweredImageTerminator *goto_row = NULL, *return_row = NULL;
    SolMirRuntimeCleanupEvent *goto_event = NULL, *return_event = NULL;
    SolMirRuntimeCleanupTransition *goto_transition = NULL, *return_transition = NULL;
    size_t goto_block = SOL_MIR_RUNTIME_LOWERED_NONE, return_block = SOL_MIR_RUNTIME_LOWERED_NONE;
    size_t gotos = 0, breaks = 0, continues = 0, branches = 0, returns = 0, unit_returns = 0;
    for (size_t block = 0; ok && block < m->block_count; ++block) {
        const SolMirMaterializedTerminator *term = &m->blocks[block].terminator;
        SolMirRuntimeCleanupEdgeRole role;
        SolMirRuntimeCleanupOutcome outcome;
        size_t expected = 0;
        if (term->kind == SOL_MIR_TERM_GOTO || term->kind == SOL_MIR_TERM_BREAK
            || term->kind == SOL_MIR_TERM_CONTINUE) {
            role = SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO;
            outcome = SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL; expected = 1;
            ++gotos;
            if (term->kind == SOL_MIR_TERM_BREAK) ++breaks;
            if (term->kind == SOL_MIR_TERM_CONTINUE) ++continues;
        } else if (term->kind == SOL_MIR_TERM_BRANCH) {
            role = SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_TRUE;
            outcome = SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL; expected = 2; ++branches;
        } else if (term->kind == SOL_MIR_TERM_RETURN) {
            role = SOL_MIR_RUNTIME_CLEANUP_EDGE_RETURN;
            outcome = SOL_MIR_RUNTIME_CLEANUP_OUTCOME_EXIT; expected = 1; ++returns;
            if (term->value < m->value_count
                && m->values[term->value].type < pipeline.concrete.layout.type_count
                && pipeline.concrete.representation.recipes[pipeline.concrete.layout.types[
                    m->values[term->value].type].recipe].kind == SOL_MIR_RECIPE_UNIT) ++unit_returns;
        } else continue;
        SolMirRuntimeLoweredImageTerminator *row = block < pipeline.lowered.image_terminator_count
            ? &pipeline.lowered.image_terminators[block] : NULL;
        SolMirRuntimeCleanupEvent *event = row != NULL && row->cleanup_event < pipeline.cleanup.event_count
            ? &pipeline.cleanup.events[row->cleanup_event] : NULL;
        if (row == NULL || event == NULL || row->state != SOL_MIR_RUNTIME_LOWERED_PRESENT
            || row->image >= m->image_count || row->block != block || row->kind != term->kind
            || event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
            || event->phase != SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
            || event->owner != row->image || event->block != block
            || event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL
            || event->inherited_failure_site != SOL_MIR_RUNTIME_NONE
            || event->supplemental_site != SOL_MIR_RUNTIME_NONE || event->captures_failure_detail
            || event->transitions.count != expected) { ok = false; break; }
        size_t seen = 0;
        SolMirRuntimeCleanupTransition *local_true = NULL, *local_false = NULL;
        for (size_t i = 0; i < event->transitions.count; ++i) {
            SolMirRuntimeCleanupTransition *transition = &pipeline.cleanup.transitions[
                event->transitions.offset + i];
            if (transition->event != row->cleanup_event || transition->outcome != outcome
                || transition->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE
                || transition->failure_site != SOL_MIR_RUNTIME_NONE || transition->failure_mask != 0)
                { ok = false; break; }
            if (term->kind == SOL_MIR_TERM_BRANCH) {
                if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_TRUE
                    && transition->continuation == term->true_edge) {
                    ++seen;
                    if (local_true != NULL) ok = false;
                    local_true = transition;
                } else if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_FALSE
                    && transition->continuation == term->false_edge) {
                    ++seen;
                    if (local_false != NULL) ok = false;
                    local_false = transition;
                } else ok = false;
            } else if (transition->edge_role == role) ++seen;
            else ok = false;
            if (transition->continuation == SOL_MIR_RUNTIME_NONE) {
                if (transition->source_edge != SOL_MIR_RUNTIME_NONE
                    || transition->destination != SOL_MIR_RUNTIME_NONE) ok = false;
            } else if (transition->continuation >= m->edge_count
                || transition->source_edge != transition->continuation
                || transition->destination != m->edges[transition->continuation].block) ok = false;
        }
        if (seen != expected) ok = false;
        SolMirRuntimeSlice direct_actions = {0};
        size_t direct_transition = SOL_MIR_RUNTIME_LOWERED_NONE;
        if (term->kind == SOL_MIR_TERM_BRANCH) {
            size_t true_transition = SOL_MIR_RUNTIME_LOWERED_NONE;
            SolMirRuntimeSlice true_actions = {0};
            ok = ok && sol_wasm_represented_test_control_transition(
                &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, block,
                SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_TRUE, &true_transition, &true_actions)
                && sol_wasm_represented_test_control_transition(
                    &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, block,
                    SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_FALSE, &direct_transition, &direct_actions)
                && local_true != NULL && local_false != NULL
                && true_transition == (size_t)(local_true - pipeline.cleanup.transitions)
                && direct_transition == (size_t)(local_false - pipeline.cleanup.transitions)
                && true_actions.offset == local_true->actions.offset && true_actions.count == 0
                && direct_actions.offset == local_false->actions.offset && direct_actions.count == 0;
        } else {
            ok = ok && sol_wasm_represented_test_control_transition(
                &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, block, role,
                &direct_transition, &direct_actions)
                && direct_transition == event->transitions.offset && direct_actions.offset
                    == event->actions.offset && direct_actions.count == 0;
        }
        if (term->kind == SOL_MIR_TERM_BRANCH && branch_row == NULL) {
            branch_row = row; branch_event = event;
            branch_true = local_true; branch_false = local_false;
            branch_block = block;
        }
        if ((term->kind == SOL_MIR_TERM_GOTO || term->kind == SOL_MIR_TERM_BREAK
                || term->kind == SOL_MIR_TERM_CONTINUE) && goto_row == NULL) {
            goto_row = row; goto_event = event; goto_transition = &pipeline.cleanup.transitions[
                event->transitions.offset]; goto_block = block;
        }
        if (term->kind == SOL_MIR_TERM_RETURN && return_row == NULL) {
            return_row = row; return_event = event; return_transition = &pipeline.cleanup.transitions[
                event->transitions.offset]; return_block = block;
        }
    }
    char entry[256];
    if (ok) ok = gotos != 0 && breaks != 0 && continues != 0 && branches >= 2
        && returns >= 3 && unit_returns >= 2 && branch_row != NULL && branch_event != NULL
        && branch_true != NULL && branch_false != NULL && goto_row != NULL && goto_event != NULL
        && goto_transition != NULL && return_row != NULL && return_event != NULL
        && return_transition != NULL
        && sol_wasm_represented_test_control_transition(
            &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, branch_block,
            SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_TRUE, &(size_t){0}, &(SolMirRuntimeSlice){0})
        && entry_symbol(&baseline.bytes, entry, sizeof entry)
        && invoke_named(&baseline.bytes, entry, 43, 0, 0);
    static const uint8_t expected_hash[32] = {0xdf,0x7d,0xec,0x5e,0xaa,0x0e,0xb5,0x46,0xd5,0xeb,0x61,0xf9,0x28,0x68,0xb2,0x54,0x25,0xba,0x1b,0x4a,0x4b,0xe9,0x27,0x9a,0xd4,0x52,0x17,0x8d,0xd0,0x8b,0x47,0x43};
    uint8_t baseline_hash[32];
    if (ok) sha256(baseline.bytes.bytes, baseline.bytes.count, baseline_hash);
    if (ok) ok = usage_equal(&baseline.usage, &(SolWasmRepresentedUsage){6,23,26,56,103,2897,0,0,0,0,165,73291,35307,38340,28100})
        && memcmp(baseline_hash, expected_hash, sizeof baseline_hash) == 0;
#define CHECK_P44_B1_REJECT_ROUTE(test_block, test_role, edit, restore) do { \
    edit; pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
    sol_wasm_represented_output_init(&output); \
    ok = ok && !sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL) \
        && !sol_wasm_represented_test_control_transition( \
            &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, (test_block), \
            (test_role), &(size_t){0}, &(SolMirRuntimeSlice){0}) \
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, \
            &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE \
        && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage); \
    sol_wasm_represented_output_free(&output); restore; \
    pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
    ok = ok && sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL) \
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL) \
        && sol_wasm_represented_test_control_transition( \
            &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, (test_block), \
            (test_role), &(size_t){0}, &(SolMirRuntimeSlice){0}); \
} while (0)
#define CHECK_P44_B1_REJECT(edit, restore) \
    CHECK_P44_B1_REJECT_ROUTE(branch_block, SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_TRUE, edit, restore)
    if (ok) {
        size_t saved_event = branch_row->cleanup_event;
        CHECK_P44_B1_REJECT(branch_row->cleanup_event = SOL_MIR_RUNTIME_LOWERED_NONE,
            branch_row->cleanup_event = saved_event);
        if (m->image_count > 1) {
            size_t row_image = branch_row->image;
            CHECK_P44_B1_REJECT(branch_row->image = row_image == 0 ? 1 : 0,
                branch_row->image = row_image);
        }
        SolMirRuntimeCleanupPhase phase = branch_event->phase;
        CHECK_P44_B1_REJECT(branch_event->phase = SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_INVOKE_CALLABLE,
            branch_event->phase = phase);
        SolMirRuntimeCleanupProducerKind producer = branch_event->producer;
        CHECK_P44_B1_REJECT(branch_event->producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_INVOKE,
            branch_event->producer = producer);
        SolMirRuntimeCleanupEdgeRole true_role = branch_true->edge_role;
        SolMirRuntimeCleanupEdgeRole false_role = branch_false->edge_role;
        CHECK_P44_B1_REJECT(branch_true->edge_role = false_role; branch_false->edge_role = true_role,
            branch_true->edge_role = true_role; branch_false->edge_role = false_role);
        SolMirRuntimeCleanupOutcome outcome = branch_true->outcome;
        CHECK_P44_B1_REJECT(branch_true->outcome = SOL_MIR_RUNTIME_CLEANUP_OUTCOME_EXIT,
            branch_true->outcome = outcome);
        size_t continuation = branch_true->continuation;
        CHECK_P44_B1_REJECT(branch_true->continuation = SOL_MIR_RUNTIME_NONE,
            branch_true->continuation = continuation);
        size_t source_edge = branch_true->source_edge;
        CHECK_P44_B1_REJECT(branch_true->source_edge = SOL_MIR_RUNTIME_NONE,
            branch_true->source_edge = source_edge);
        size_t destination = branch_true->destination;
        CHECK_P44_B1_REJECT(branch_true->destination = SOL_MIR_RUNTIME_NONE,
            branch_true->destination = destination);
        CHECK_P44_B1_REJECT(branch_false->edge_role = true_role,
            branch_false->edge_role = false_role);
        size_t transition_count = branch_event->transitions.count;
        CHECK_P44_B1_REJECT(branch_event->transitions.count = 1,
            branch_event->transitions.count = transition_count);
        SolMirRuntimeSlice actions = branch_true->actions;
        CHECK_P44_B1_REJECT(branch_true->actions.count = pipeline.cleanup.action_count + 1,
            branch_true->actions = actions);
        SolMirRuntimeCleanupFailureSource failure = branch_true->failure_source;
        CHECK_P44_B1_REJECT(branch_true->failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31,
            branch_true->failure_source = failure);
        size_t failure_site = branch_true->failure_site;
        CHECK_P44_B1_REJECT(branch_true->failure_site = 0, branch_true->failure_site = failure_site);
        uint32_t failure_mask = branch_true->failure_mask;
        CHECK_P44_B1_REJECT(branch_true->failure_mask = 1, branch_true->failure_mask = failure_mask);
        SolMirRuntimeSlice false_actions = branch_false->actions;
        CHECK_P44_B1_REJECT_ROUTE(branch_block, SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_FALSE,
            branch_false->actions.offset = 0; branch_false->actions.count = 1,
            branch_false->actions = false_actions);
        CHECK_P44_B1_REJECT_ROUTE(branch_block, SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_FALSE,
            branch_false->actions.count = pipeline.cleanup.action_count + 1,
            branch_false->actions = false_actions);
        size_t false_continuation = branch_false->continuation;
        CHECK_P44_B1_REJECT_ROUTE(branch_block, SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_FALSE,
            branch_false->continuation = SOL_MIR_RUNTIME_NONE,
            branch_false->continuation = false_continuation);
        SolMirRuntimeCleanupEdgeRole goto_role = goto_transition->edge_role;
        CHECK_P44_B1_REJECT_ROUTE(goto_block, SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO,
            goto_transition->edge_role = SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_TRUE,
            goto_transition->edge_role = goto_role);
        size_t goto_destination = goto_transition->destination;
        CHECK_P44_B1_REJECT_ROUTE(goto_block, SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO,
            goto_transition->destination = SOL_MIR_RUNTIME_NONE,
            goto_transition->destination = goto_destination);
        SolMirRuntimeSlice goto_actions = goto_transition->actions;
        CHECK_P44_B1_REJECT_ROUTE(goto_block, SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO,
            goto_transition->actions.offset = 0; goto_transition->actions.count = 1,
            goto_transition->actions = goto_actions);
        SolMirRuntimeCleanupEdgeRole return_role = return_transition->edge_role;
        CHECK_P44_B1_REJECT_ROUTE(return_block, SOL_MIR_RUNTIME_CLEANUP_EDGE_RETURN,
            return_transition->edge_role = SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO,
            return_transition->edge_role = return_role);
        size_t return_destination = return_transition->destination;
        CHECK_P44_B1_REJECT_ROUTE(return_block, SOL_MIR_RUNTIME_CLEANUP_EDGE_RETURN,
            return_transition->destination = 0, return_transition->destination = return_destination);
        SolMirRuntimeSlice return_actions = return_transition->actions;
        CHECK_P44_B1_REJECT_ROUTE(return_block, SOL_MIR_RUNTIME_CLEANUP_EDGE_RETURN,
            return_transition->actions.offset = 0; return_transition->actions.count = 1,
            return_transition->actions = return_actions);
    }
#undef CHECK_P44_B1_REJECT
#undef CHECK_P44_B1_REJECT_ROUTE
    if (ok) {
        SolWasmRepresentedLimits limits = sol_wasm_represented_default_limits();
        limits.max_generated_nodes = baseline.usage.generated_nodes;
        sol_wasm_represented_output_init(&output);
        ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, &limits}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && usage_equal(&output.usage, &baseline.usage) && output.bytes.count == baseline.bytes.count
            && memcmp(output.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0;
        sol_wasm_represented_output_free(&output);
        limits.max_generated_nodes = baseline.usage.generated_nodes - 1;
        sol_wasm_represented_output_init(&output);
        ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, &limits}, &output, &pipeline.diagnostics)
                == SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED
            && output.bytes.bytes == NULL && usage_zero(&output.usage);
        sol_wasm_represented_output_free(&output);
        represented_fault(FAULT_B1_FIRST_ALLOCATION, 1, 1);
        sol_wasm_represented_output_init(&output);
        ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_ALLOCATION_FAILED
            && output.bytes.bytes == NULL && usage_zero(&output.usage);
        sol_wasm_represented_test_fail_allocation_after(0);
        sol_wasm_represented_output_free(&output);
        sol_wasm_represented_output_init(&output);
        ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && usage_equal(&output.usage, &baseline.usage) && output.bytes.count == baseline.bytes.count
            && memcmp(output.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0;
        sol_wasm_represented_output_free(&output);
    }
    sol_wasm_represented_output_free(&output); sol_wasm_represented_output_free(&baseline);
    propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44_trace_slots_authentic(const PropagationPipeline *pipeline,
    const P44TraceSlot *slots, size_t count, int32_t failure_record, bool *kinds) {
    const SolMirRuntimeCleanup *cleanup = &pipeline->cleanup;
    const unsigned known = SOL_WASM_REPRESENTED_TEST_P44_TRACE_EXECUTED
        | SOL_WASM_REPRESENTED_TEST_P44_TRACE_SKIPPED | SOL_WASM_REPRESENTED_TEST_P44_TRACE_FAILURE
        | SOL_WASM_REPRESENTED_TEST_P44_TRACE_IMPLICIT | SOL_WASM_REPRESENTED_TEST_P44_TRACE_PENDING
        | SOL_WASM_REPRESENTED_TEST_P44_TRACE_NORMAL | SOL_WASM_REPRESENTED_TEST_P44_TRACE_ACTION_FAILURE
        | SOL_WASM_REPRESENTED_TEST_P44_TRACE_GUARDED;
    if (kinds != NULL) memset(kinds, 0, 5 * sizeof *kinds);
    for (size_t i = 0; i < count; ++i) {
        if (slots[i].action >= cleanup->action_count
            || (slots[i].disposition & ~known) != 0
            || (slots[i].disposition & (SOL_WASM_REPRESENTED_TEST_P44_TRACE_EXECUTED
                | SOL_WASM_REPRESENTED_TEST_P44_TRACE_SKIPPED)) == 0
            || (slots[i].disposition & (SOL_WASM_REPRESENTED_TEST_P44_TRACE_EXECUTED
                | SOL_WASM_REPRESENTED_TEST_P44_TRACE_SKIPPED))
                == (SOL_WASM_REPRESENTED_TEST_P44_TRACE_EXECUTED
                    | SOL_WASM_REPRESENTED_TEST_P44_TRACE_SKIPPED)
            || ((slots[i].disposition & SOL_WASM_REPRESENTED_TEST_P44_TRACE_FAILURE) != 0
                ? slots[i].record != (uint32_t)failure_record : slots[i].record != 0)) return false;
        if (kinds != NULL) switch (cleanup->actions[slots[i].action].kind) {
            case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY: kinds[0] = true; break;
            case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE: kinds[1] = true; break;
            case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER: kinds[2] = true; break;
            case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE: kinds[3] = true; break;
            case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_REGION: kinds[4] = true; break;
            default: break;
        }
    }
    return true;
}

/* Each expected word is reconstructed from the P3.6 transition that owns its
 * action. The fixture's known path then supplies only the ordered selection of
 * those authenticated slices, never a backend-side action identity. */
static bool p44_trace_expected_from_selected(const PropagationPipeline *pipeline,
    const P44TraceSlot *expected, size_t count, int32_t failure_record) {
    const SolMirRuntimeCleanup *cleanup = &pipeline->cleanup;
    for (size_t slot = 0; slot < count; ++slot) {
        bool found = false;
        for (size_t event_id = 0; event_id < cleanup->event_count; ++event_id) {
            const SolMirRuntimeCleanupEvent *event = &cleanup->events[event_id];
            for (size_t item = 0; item < event->transitions.count; ++item) {
                const SolMirRuntimeCleanupTransition *transition = &cleanup->transitions[
                    event->transitions.offset + item];
                if (expected[slot].action < transition->actions.offset
                    || expected[slot].action - transition->actions.offset >= transition->actions.count)
                    continue;
                const SolMirRuntimeCleanupAction *action = &cleanup->actions[expected[slot].action];
                /* Conditional hole actions are source-owned but can be
                 * skipped after their authenticated liveness test.  The
                 * expected disposition selects that runtime branch; every
                 * remaining bit still comes solely from its P3 owner. */
                unsigned disposition = (expected[slot].disposition
                    & SOL_WASM_REPRESENTED_TEST_P44_TRACE_SKIPPED) != 0
                    ? SOL_WASM_REPRESENTED_TEST_P44_TRACE_SKIPPED
                    : SOL_WASM_REPRESENTED_TEST_P44_TRACE_EXECUTED;
                if (transition->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE)
                    disposition |= SOL_WASM_REPRESENTED_TEST_P44_TRACE_FAILURE;
                if (event->origin == SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT)
                    disposition |= SOL_WASM_REPRESENTED_TEST_P44_TRACE_IMPLICIT;
                if (transition->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING)
                    disposition |= SOL_WASM_REPRESENTED_TEST_P44_TRACE_PENDING;
                if ((action->flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY) != 0)
                    disposition |= SOL_WASM_REPRESENTED_TEST_P44_TRACE_NORMAL;
                if ((action->flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY) != 0)
                    disposition |= SOL_WASM_REPRESENTED_TEST_P44_TRACE_ACTION_FAILURE;
                if ((action->flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED) != 0)
                    disposition |= SOL_WASM_REPRESENTED_TEST_P44_TRACE_GUARDED;
                if ((disposition & SOL_WASM_REPRESENTED_TEST_P44_TRACE_SKIPPED) != 0
                    && (action->flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED) == 0) continue;
                if (expected[slot].disposition != disposition
                    || expected[slot].record != ((disposition
                        & SOL_WASM_REPRESENTED_TEST_P44_TRACE_FAILURE) != 0
                            ? (uint32_t)failure_record : 0)) continue;
                if (found) return false;
                found = true;
            }
        }
        if (!found) return false;
    }
    return true;
}

/* B2 deliberately obtains every expectation from the authenticated P3.6
 * actions.  The Wasm reader above is the only decoding of the wire ledger. */
static bool p44_cleanup_trace_case(const char *directory, int64_t value,
    int32_t code, int32_t site, const P44TraceSlot *expected, size_t expected_count,
    bool require_all_kinds) {
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    SolWasmRepresentedOutput plain, traced;
    sol_wasm_represented_output_init(&plain); sol_wasm_represented_output_init(&traced);
    P44TraceSlot first[64], second[64]; size_t first_count = 0, second_count = 0;
    bool first_overflow = false, second_overflow = false, kinds[5] = {false};
    WasmInstance instance = {0}; char entry[256];
    sol_wasm_represented_test_p44_cleanup_trace_probe(false);
    bool pipeline_ok = propagation_pipeline_build(&pipeline, directory);
    SolWasmRepresentedResult plain_result = pipeline_ok ? sol_wasm_represented_build(
        &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, &plain,
        &pipeline.diagnostics) : SOL_WASM_REPRESENTED_INVALID_INPUT;
    bool ok = pipeline_ok && plain_result == SOL_WASM_REPRESENTED_OK
        && !p44_bytes_contain(&plain.bytes, SOL_WASM_REPRESENTED_TEST_P44_TRACE_OFFSET_EXPORT)
        && !p44_bytes_contain(&plain.bytes, SOL_WASM_REPRESENTED_TEST_P44_TRACE_COUNT_EXPORT)
        && !p44_bytes_contain(&plain.bytes, SOL_WASM_REPRESENTED_TEST_P44_TRACE_OVERFLOW_EXPORT);
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    SolWasmRepresentedResult traced_result = ok ? sol_wasm_represented_build(
        &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, &traced,
        &pipeline.diagnostics) : SOL_WASM_REPRESENTED_INVALID_INPUT;
    if (ok) ok = traced_result == SOL_WASM_REPRESENTED_OK
        && sol_wasm_represented_validate(&traced.bytes) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&traced.bytes, entry, sizeof entry)
        && wasm_instance_open(&traced.bytes, entry, &instance)
        && instance.trace_offset != NULL && instance.trace_count != NULL && instance.trace_overflow != NULL
        && wasm_instance_call(&instance, value, code, site)
        && wasm_instance_trace(&instance, first, 64, &first_count, &first_overflow)
        && first_count != 0 && !first_overflow
        && p44_trace_slots_authentic(&pipeline, first, first_count, site, kinds)
        && p44_trace_expected_from_selected(&pipeline, expected, expected_count, site);
    /* The source-owned Text equality temporary is first; the owned formal is
     * released only after the nested lexical scope, explicit region, and place
     * cleanup have completed. */
    if (ok && require_all_kinds)
        for (size_t i = 0; i < sizeof kinds / sizeof *kinds; ++i) ok = ok && kinds[i];
    if (ok) ok = first_count == expected_count
        && memcmp(first, expected, expected_count * sizeof *expected) == 0
        && wasm_instance_call(&instance, value, code, site)
        && wasm_instance_trace(&instance, second, 64, &second_count, &second_overflow)
        && second_count == first_count && second_overflow == first_overflow
        && memcmp(first, second, first_count * sizeof *first) == 0;
    wasm_instance_close(&instance);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false);
    if (ok) ok = sol_wasm_represented_validate(&traced.bytes) == SOL_WASM_REPRESENTED_INVALID_INPUT;
    sol_wasm_represented_output_free(&traced); sol_wasm_represented_output_free(&plain);
    propagation_pipeline_free(&pipeline);
    return ok;
}

/* The trace exports are a hook-only envelope over the existing P4.4 packet.
 * Keep every malformed wire case raw: no Binaryen rewrite is used to make a
 * hostile export look acceptable. */
static bool p44_trace_wire_controls(const char *directory) {
    SolWasmRepresentedOutput output;
    P44WireLayout wire;
    NamedExportLayout offset, count, overflow;
    sol_wasm_represented_output_init(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    char entry[256]; WasmInstance instance = {0}; P44TraceSlot slots[64]; size_t slot_count = 0;
    bool slot_overflow = false;
    bool ok = build_named_root(directory, "launch", &output, NULL, SOL_WASM_REPRESENTED_OK)
        && sol_wasm_represented_validate(&output.bytes) == SOL_WASM_REPRESENTED_OK
        && p44_wire_layout(&output.bytes, &wire)
        && named_global_export_layout(&output.bytes, SOL_WASM_REPRESENTED_TEST_P44_TRACE_OFFSET_EXPORT,
            &offset)
        && named_global_export_layout(&output.bytes, SOL_WASM_REPRESENTED_TEST_P44_TRACE_COUNT_EXPORT,
            &count)
        && named_global_export_layout(&output.bytes, SOL_WASM_REPRESENTED_TEST_P44_TRACE_OVERFLOW_EXPORT,
            &overflow)
        && offset.global < wire.count && count.global < wire.count && overflow.global < wire.count
        && offset.global != count.global && offset.global != overflow.global
        && count.global != overflow.global && wire.offset_global < wire.count
        && wire.initial_value[offset.global] == wire.initial_value[wire.offset_global] + 192
        /* The adjacent D..D+192 panic packet and D+192..D+960 ledger remain
         * independently readable in one hook-on panic invocation. */
        && entry_symbol(&output.bytes, entry, sizeof entry)
        && wasm_instance_open(&output.bytes, entry, &instance)
        && wasm_instance_call(&instance, 0, 1, 3)
        && wasm_instance_panic_detail(&instance, (const uint8_t *)"represented terminal panic", 26)
        && wasm_instance_trace(&instance, slots, 64, &slot_count, &slot_overflow)
        && !slot_overflow;
    wasm_instance_close(&instance);
#define CHECK_P44_TRACE_WIRE(edit) do { \
    uint8_t *mutated = malloc(output.bytes.count); \
    if (mutated == NULL) ok = false; \
    else { memcpy(mutated, output.bytes.bytes, output.bytes.count); edit; \
        ok = ok && sol_wasm_represented_validate(&(SolWasmBackendBytes){mutated, output.bytes.count}) \
            == SOL_WASM_REPRESENTED_INVALID_INPUT; free(mutated); } \
} while (0)
    if (ok) {
        const NamedExportLayout exports[] = {offset, count, overflow};
        for (size_t i = 0; i < sizeof exports / sizeof *exports; ++i) {
            /* Each individual missing/unknown trace export is invalid. */
            CHECK_P44_TRACE_WIRE(mutated[exports[i].name] = 'x');
            CHECK_P44_TRACE_WIRE(mutated[exports[i].name + exports[i].name_count - 1] = 'x');
            CHECK_P44_TRACE_WIRE(mutated[exports[i].kind] = 0);
            CHECK_P44_TRACE_WIRE(write_uleb_same_width(mutated + exports[i].index,
                exports[i].index_width, i == 0 ? count.global : offset.global));
            CHECK_P44_TRACE_WIRE(mutated[wire.type[exports[i].global]] = UINT8_C(0x7e));
            CHECK_P44_TRACE_WIRE(mutated[wire.mutability[exports[i].global]] =
                i == 0 ? 1 : 0);
            CHECK_P44_TRACE_WIRE(write_uleb_same_width(mutated + wire.initial[exports[i].global],
                wire.initial_width[exports[i].global], wire.initial_value[exports[i].global] ^ 1));
        }
        uint32_t data = wire.initial_value[wire.offset_global];
        CHECK_P44_TRACE_WIRE(write_uleb_same_width(mutated + wire.initial[offset.global],
            wire.initial_width[offset.global], data + 191)); /* before D + 192 */
        CHECK_P44_TRACE_WIRE(write_uleb_same_width(mutated + wire.initial[offset.global],
            wire.initial_width[offset.global], UINT32_C(16383))); /* after heap/one-page bounds */
        CHECK_P44_TRACE_WIRE(write_uleb_same_width(mutated + wire.initial[offset.global],
            wire.initial_width[offset.global], data + 100)); /* overlaps panic detail */
        CHECK_P44_TRACE_WIRE(write_uleb_same_width(mutated + wire.initial[2],
            wire.initial_width[2], data + 959)); /* heap below D + 960 */
    }
#undef CHECK_P44_TRACE_WIRE
    sol_wasm_represented_test_p44_cleanup_trace_probe(false);
    ok = ok && sol_wasm_represented_validate(&output.bytes) == SOL_WASM_REPRESENTED_INVALID_INPUT;
    sol_wasm_represented_output_free(&output);
    return ok;
}

static bool p44_trace_stress_case(const char *directory, const P44TraceSlot *prefix,
    size_t prefix_count, const P44TraceSlot *repeat, size_t repeat_count,
    const P44TraceSlot *tail, size_t tail_count) {
    PropagationPipeline pipeline; SolWasmRepresentedOutput output;
    P44TraceSlot first[64], second[64]; size_t first_count = 0, second_count = 0;
    bool first_overflow = false, second_overflow = false; WasmInstance instance = {0}; char entry[256];
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    sol_wasm_represented_test_p44_packet_reset_probe(true);
    bool pipeline_ok = propagation_pipeline_build(&pipeline, directory);
    SolWasmRepresentedResult build_result = pipeline_ok ? sol_wasm_represented_build(
        &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, &output,
        &pipeline.diagnostics) : SOL_WASM_REPRESENTED_INVALID_INPUT;
    bool ok = prefix != NULL && prefix_count != 0 && repeat != NULL && repeat_count != 0
        && tail != NULL && tail_count != 0 && prefix_count + repeat_count * 5 + tail_count == 64 && pipeline_ok
        && build_result == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry)
        && wasm_instance_open(&output.bytes, entry, &instance)
        && wasm_instance_call(&instance, 43, 0, 0)
        && wasm_instance_trace(&instance, first, 64, &first_count, &first_overflow)
        && first_count == 64 && first_overflow && wasm_instance_panic_detail(&instance,
            (const uint8_t *)"", 0)
        && p44_trace_slots_authentic(&pipeline, first, first_count, 0, NULL);
    for (size_t i = 0; ok && i < first_count; ++i) {
        const P44TraceSlot *expected = i < prefix_count ? &prefix[i]
            : i < prefix_count + repeat_count * 5 ? &repeat[(i - prefix_count) % repeat_count]
            : &tail[i - prefix_count - repeat_count * 5];
        ok = first[i].action == expected->action && first[i].disposition == expected->disposition
            && first[i].record == expected->record;
    }
    /* This short export shares the exact entry reset prologue. Its packet and
     * trace metadata must be blank before the next overflowing source entry. */
    if (ok) ok = wasm_instance_call_named(&instance,
        SOL_WASM_REPRESENTED_TEST_P44_PACKET_RESET_SUCCESS_EXPORT, 0, 0, 0)
        && wasm_instance_panic_detail(&instance, (const uint8_t *)"", 0)
        && wasm_instance_trace(&instance, second, 64, &second_count, &second_overflow)
        && second_count == 0 && !second_overflow
        && wasm_instance_call(&instance, 43, 0, 0)
        && wasm_instance_trace(&instance, second, 64, &second_count, &second_overflow)
        && second_count == 64 && second_overflow
        && memcmp(first, second, sizeof first) == 0;
    wasm_instance_close(&instance); sol_wasm_represented_test_p44_cleanup_trace_probe(false);
    sol_wasm_represented_test_p44_packet_reset_probe(false);
    sol_wasm_represented_output_free(&output); propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44_trace_skipped_case(const char *directory) {
    PropagationPipeline pipeline; SolWasmRepresentedOutput output; WasmInstance instance = {0};
    P44TraceSlot slots[64]; size_t count = 0, action = SOL_MIR_RUNTIME_NONE, instruction = SOL_MIR_RUNTIME_NONE;
    bool overflow = false; char entry[256];
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    bool ok = propagation_pipeline_build(&pipeline, directory);
    SolWasmRepresentedBuildRequest request = {&pipeline.lowered, directory, NULL};
    if (ok) for (size_t i = 0; i < pipeline.concrete.materialization.instruction_count; ++i) {
        const SolMirMaterializedInstruction *item = &pipeline.concrete.materialization.instructions[i];
        size_t candidate = SOL_MIR_RUNTIME_NONE;
        if (item->kind == SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED
            && item->place < pipeline.concrete.materialization.place_count
            && pipeline.concrete.materialization.places[item->place].projections.count != 0
            && sol_wasm_represented_test_cleanup_marker(&request, i, &candidate)
                == SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_ACTION
            && candidate < pipeline.cleanup.action_count
            && (pipeline.cleanup.actions[candidate].flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED) != 0) {
            if (action != SOL_MIR_RUNTIME_NONE) ok = false;
            action = candidate; instruction = i;
        }
    }
    static const P44TraceSlot expected[] = {
        {14, 1026, 0}, {15, 1, 0}, {16, 1, 0}, {17, 1, 0},
    };
    const SolMirRuntimeCleanupAction *selected = action < pipeline.cleanup.action_count
        ? &pipeline.cleanup.actions[action] : NULL;
    if (ok) ok = action == 14 && instruction == 21 && selected != NULL
        && selected->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE
        && (selected->flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED) != 0
        && sol_wasm_represented_build(&request, &output, &pipeline.diagnostics)
            == SOL_WASM_REPRESENTED_OK && entry_symbol(&output.bytes, entry, sizeof entry)
        && wasm_instance_open(&output.bytes, entry, &instance) && wasm_instance_call(&instance, 42, 0, 0)
        && wasm_instance_trace(&instance, slots, 64, &count, &overflow) && !overflow
        && count == sizeof expected / sizeof *expected && memcmp(slots, expected, sizeof expected) == 0;
    wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false); propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44_trace_eventless_case(const char *directory) {
    PropagationPipeline pipeline; SolWasmRepresentedOutput output; WasmInstance instance = {0};
    P44TraceSlot slots[64]; size_t count = 0, markers = 0, eventless[2] = {SOL_MIR_RUNTIME_NONE,
        SOL_MIR_RUNTIME_NONE};
    bool overflow = false; char entry[256];
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    bool ok = propagation_pipeline_build(&pipeline, directory);
    SolWasmRepresentedBuildRequest request = {&pipeline.lowered, directory, NULL};
    static const P44TraceSlot expected[] = {{14, 1, 0}, {15, 1, 0}, {16, 1, 0}};
    if (ok) for (size_t i = 0; i < pipeline.concrete.materialization.instruction_count; ++i) {
        size_t action = SOL_MIR_RUNTIME_NONE;
        if (sol_wasm_represented_test_cleanup_marker(&request, i, &action)
            == SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_EVENTLESS) {
            if (markers < sizeof eventless / sizeof *eventless) eventless[markers] = i;
            ++markers;
            ok = ok && action == SOL_MIR_RUNTIME_NONE;
        }
    }
    if (ok) ok = markers == 2 && eventless[0] == 23 && eventless[1] == 25
        && sol_wasm_represented_build(&request, &output, &pipeline.diagnostics)
            == SOL_WASM_REPRESENTED_OK && entry_symbol(&output.bytes, entry, sizeof entry)
        && wasm_instance_open(&output.bytes, entry, &instance) && wasm_instance_call(&instance, 42, 0, 0)
        && wasm_instance_trace(&instance, slots, 64, &count, &overflow) && !overflow
        && count == sizeof expected / sizeof *expected && memcmp(slots, expected, sizeof expected) == 0;
    for (size_t i = 0; ok && i < count; ++i)
        ok = slots[i].action != eventless[0] && slots[i].action != eventless[1];
    wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false); propagation_pipeline_free(&pipeline);
    return ok;
}

/* Direct internal calls route LOCAL_OR_PENDING through an empty call-failure
 * slice: the still-live caller Text is released by the successor RESUME_FAILURE
 * terminal's PENDING slice. This fixture freezes that exact structural gate. */
static bool p44_trace_pending_case(const char *directory) {
    PropagationPipeline pipeline; SolWasmRepresentedOutput output; WasmInstance instance = {0};
    P44TraceSlot slots[64]; size_t count = 0, call_id = SOL_MIR_RUNTIME_NONE;
    bool overflow = false; char entry[256];
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    bool ok = propagation_pipeline_build(&pipeline, directory);
    if (ok) for (size_t i = 0; i < pipeline.conventions.call_count; ++i)
        if (pipeline.conventions.calls[i].target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL) {
            if (call_id != SOL_MIR_RUNTIME_NONE) ok = false;
            call_id = i;
        }
    const SolMirRuntimeCall *call = call_id < pipeline.conventions.call_count
        ? &pipeline.conventions.calls[call_id] : NULL;
    const SolMirRuntimeLoweredImageTerminator *row = call != NULL
        && call->block < pipeline.lowered.image_terminator_count
        ? &pipeline.lowered.image_terminators[call->block] : NULL;
    const SolMirRuntimeCleanupEvent *event = row != NULL && row->cleanup_event < pipeline.cleanup.event_count
        ? &pipeline.cleanup.events[row->cleanup_event] : NULL;
    const SolMirRuntimeCleanupTransition *failure = NULL;
    if (ok && event != NULL) for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *candidate = &pipeline.cleanup.transitions[
            event->transitions.offset + i];
        if (candidate->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE) failure = candidate;
    }
    size_t resume_block = call != NULL && call->failure_edge < pipeline.concrete.materialization.edge_count
        ? pipeline.concrete.materialization.edges[call->failure_edge].block : SOL_MIR_RUNTIME_NONE;
    const SolMirRuntimeLoweredImageTerminator *resume_row = resume_block
        < pipeline.lowered.image_terminator_count ? &pipeline.lowered.image_terminators[resume_block] : NULL;
    const SolMirRuntimeCleanupEvent *resume = resume_row != NULL
        && resume_row->cleanup_event < pipeline.cleanup.event_count
        ? &pipeline.cleanup.events[resume_row->cleanup_event] : NULL;
    const SolMirRuntimeCleanupTransition *pending = resume != NULL && resume->transitions.count == 1
        ? &pipeline.cleanup.transitions[resume->transitions.offset] : NULL;
    static const P44TraceSlot expected[] = {
        {3, 1, 0}, {4, 1, 0}, {5, 517, 4}, {9, 1, 0},
        {10, 1, 0}, {11, 1, 0}, {12, 533, 4},
    };
    if (ok) ok = call != NULL && event != NULL && failure != NULL
        && failure->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING
        && failure->failure_site == call->failure_site && failure->actions.count == 0
        && failure->actions.offset <= pipeline.cleanup.action_count
        && failure->actions.count <= pipeline.cleanup.action_count - failure->actions.offset
        && resume != NULL && pending != NULL && pending->failure_source
            == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING
        && pending->actions.offset == 12 && pending->actions.count == 1
        && pipeline.cleanup.actions[12].kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory,
            NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry) && wasm_instance_open(&output.bytes, entry, &instance)
        && wasm_instance_call(&instance, 0, 1, -1)
        && wasm_instance_trace(&instance, slots, 64, &count, &overflow) && !overflow
        && count == sizeof expected / sizeof *expected && memcmp(slots, expected, sizeof expected) == 0
        && (slots[6].disposition & SOL_WASM_REPRESENTED_TEST_P44_TRACE_PENDING) != 0
        && (slots[6].disposition & SOL_WASM_REPRESENTED_TEST_P44_TRACE_IMPLICIT) == 0;
    wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false); propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44_trace_freeze_case(const char *directory, const SolWasmRepresentedUsage *expected_usage,
    const uint8_t expected_hash[32]) {
    PropagationPipeline pipeline; SolWasmRepresentedOutput traced, copy;
    SolWasmRepresentedLimits zero = {0}, cap;
    uint8_t hash[32];
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&traced);
    sol_wasm_represented_output_init(&copy); sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    bool ok = expected_usage != NULL && expected_hash != NULL && propagation_pipeline_build(&pipeline, directory)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, NULL}, &traced, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
    if (ok) {
        sha256(traced.bytes.bytes, traced.bytes.count, hash);
        ok = usage_equal(&traced.usage, expected_usage)
            && memcmp(hash, expected_hash, sizeof hash) == 0;
    }
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
        directory, &zero}, &copy, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && usage_equal(&copy.usage, expected_usage) && copy.bytes.count == traced.bytes.count
        && memcmp(copy.bytes.bytes, traced.bytes.bytes, traced.bytes.count) == 0;
    sol_wasm_represented_output_free(&copy);
#define CHECK_P44_TRACE_FREEZE_CAP(field, exact) do { \
    cap = sol_wasm_represented_default_limits(); cap.field = (exact); \
    sol_wasm_represented_output_init(&copy); \
    ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, \
        directory, &cap}, &copy, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK \
        && usage_equal(&copy.usage, expected_usage) && copy.bytes.count == traced.bytes.count \
        && memcmp(copy.bytes.bytes, traced.bytes.bytes, traced.bytes.count) == 0; \
    sol_wasm_represented_output_free(&copy); cap.field = (exact) - 1; \
    sol_wasm_represented_output_init(&copy); \
    ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, \
        directory, &cap}, &copy, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED \
        && copy.bytes.bytes == NULL && usage_zero(&copy.usage); \
    sol_wasm_represented_output_free(&copy); \
} while (0)
    if (ok) {
        CHECK_P44_TRACE_FREEZE_CAP(max_generated_nodes, expected_usage->generated_nodes);
        CHECK_P44_TRACE_FREEZE_CAP(max_output_bytes, expected_usage->output_bytes);
    }
#undef CHECK_P44_TRACE_FREEZE_CAP
#define CHECK_P44_TRACE_PARTIAL_ZERO(field) do { \
    cap = sol_wasm_represented_default_limits(); cap.field = 0; \
    sol_wasm_represented_output_init(&copy); \
    ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, \
        directory, &cap}, &copy, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_INVALID_ARGUMENT \
        && copy.bytes.bytes == NULL && usage_zero(&copy.usage); \
    sol_wasm_represented_output_free(&copy); \
} while (0)
    if (ok) {
        CHECK_P44_TRACE_PARTIAL_ZERO(max_functions);
        CHECK_P44_TRACE_PARTIAL_ZERO(max_blocks);
        CHECK_P44_TRACE_PARTIAL_ZERO(max_edges);
        CHECK_P44_TRACE_PARTIAL_ZERO(max_values);
        CHECK_P44_TRACE_PARTIAL_ZERO(max_locals);
        CHECK_P44_TRACE_PARTIAL_ZERO(max_generated_nodes);
        CHECK_P44_TRACE_PARTIAL_ZERO(max_table_elements);
        CHECK_P44_TRACE_PARTIAL_ZERO(max_static_data_bytes);
        CHECK_P44_TRACE_PARTIAL_ZERO(max_allocation_requests);
        CHECK_P44_TRACE_PARTIAL_ZERO(max_allocation_bytes);
        CHECK_P44_TRACE_PARTIAL_ZERO(max_provenance_records);
        CHECK_P44_TRACE_PARTIAL_ZERO(max_work_bytes);
        CHECK_P44_TRACE_PARTIAL_ZERO(max_scratch_bytes);
        CHECK_P44_TRACE_PARTIAL_ZERO(max_owned_bytes);
        CHECK_P44_TRACE_PARTIAL_ZERO(max_output_bytes);
    }
#undef CHECK_P44_TRACE_PARTIAL_ZERO
    sol_wasm_represented_test_p44_cleanup_trace_probe(false);
    sol_wasm_represented_output_free(&traced); propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44_trace_owner_reject(const SolWasmRepresentedBuildRequest *request,
    SolDiagnostics *diagnostics, const char *directory) {
    SolWasmRepresentedOutput output;
    sol_wasm_represented_output_init(&output);
    SolMirRuntimeLoweredProgram *owner = (SolMirRuntimeLoweredProgram *)request->program;
    owner->authentication = sol_mir_runtime_lowered_program_test_seal(owner);
    SolWasmRepresentedResult result = sol_wasm_represented_build(request, &output, diagnostics);
    bool ok = !sol_mir_runtime_lowered_program_validate(owner, NULL)
        && result == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE && output.bytes.bytes == NULL
        && output.bytes.count == 0 && usage_zero(&output.usage);
    sol_wasm_represented_output_free(&output);
    (void)directory;
    return ok;
}

/* These are source-backed owner attacks, not synthetic trace records.  The
 * selector proves the marker/action pairing before the whole-owner build
 * validator rejects every hostile mutation and the restored bytes match. */
static bool p44_trace_marker_owner_mutations(const char *directory) {
    PropagationPipeline pipeline; SolWasmRepresentedOutput baseline, restored;
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&baseline);
    sol_wasm_represented_output_init(&restored);
    bool ok = propagation_pipeline_build(&pipeline, directory);
    SolWasmRepresentedBuildRequest request = {&pipeline.lowered, directory, NULL};
    size_t instruction = SOL_MIR_RUNTIME_NONE, action = SOL_MIR_RUNTIME_NONE;
    if (ok) for (size_t i = 0; i < pipeline.lowered.image_instruction_count; ++i) {
        size_t candidate = SOL_MIR_RUNTIME_NONE;
        if (sol_wasm_represented_test_cleanup_marker(&request, i, &candidate)
            == SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_ACTION) {
            instruction = i; action = candidate; break;
        }
    }
    if (ok) ok = instruction != SOL_MIR_RUNTIME_NONE && action < pipeline.cleanup.action_count
        && sol_wasm_represented_build(&request, &baseline, &pipeline.diagnostics)
            == SOL_WASM_REPRESENTED_OK;
#define CHECK_P44_TRACE_OWNER_REJECT(name, edit, restore) do { \
    edit; bool rejected = p44_trace_owner_reject(&request, &pipeline.diagnostics, directory); \
    ok = ok && rejected; restore; \
    pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
    size_t selected = SOL_MIR_RUNTIME_NONE; \
    ok = ok && sol_wasm_represented_test_cleanup_marker(&request, instruction, &selected) \
        == SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_ACTION && selected == action; \
} while (0)
    if (ok) {
        SolMirRuntimeLoweredImageInstruction *marker = &pipeline.lowered.image_instructions[instruction];
        size_t event_id = marker->cleanup_event;
        SolMirRuntimeCleanupEvent *event = &pipeline.cleanup.events[event_id];
        SolMirRuntimeCleanupTransition *transition = &pipeline.cleanup.transitions[event->transitions.offset];
        SolMirRuntimeCleanupAction *owner_action = &pipeline.cleanup.actions[action];
        size_t saved_event = marker->cleanup_event;
        marker->cleanup_event = pipeline.cleanup.event_count;
        size_t rejected_action = SOL_MIR_RUNTIME_NONE;
        ok = ok && sol_wasm_represented_test_cleanup_marker(&request, instruction, &rejected_action)
            == SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_INVALID
            && rejected_action == SOL_MIR_RUNTIME_NONE
            && p44_trace_owner_reject(&request, &pipeline.diagnostics, directory);
        marker->cleanup_event = saved_event;
        pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered);
        { size_t selected = SOL_MIR_RUNTIME_NONE;
          ok = ok && sol_wasm_represented_test_cleanup_marker(&request, instruction, &selected)
              == SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_ACTION && selected == action; }
        SolMirRuntimeSlice saved_slice = transition->actions;
        CHECK_P44_TRACE_OWNER_REJECT("slice", transition->actions.count = pipeline.cleanup.action_count + 1,
            transition->actions = saved_slice);
        SolMirRuntimeCleanupActionKind saved_kind = owner_action->kind;
        CHECK_P44_TRACE_OWNER_REJECT("kind", owner_action->kind = SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK,
            owner_action->kind = saved_kind);
        size_t saved_target = owner_action->target;
        CHECK_P44_TRACE_OWNER_REJECT("target", owner_action->target = SOL_MIR_RUNTIME_NONE,
            owner_action->target = saved_target);
        unsigned saved_flags = owner_action->flags;
        CHECK_P44_TRACE_OWNER_REJECT("flags", owner_action->flags ^= SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY,
            owner_action->flags = saved_flags);
    }
#undef CHECK_P44_TRACE_OWNER_REJECT
    if (ok) ok = sol_wasm_represented_build(&request, &restored, &pipeline.diagnostics)
        == SOL_WASM_REPRESENTED_OK && restored.bytes.count == baseline.bytes.count
        && memcmp(restored.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
        && usage_equal(&restored.usage, &baseline.usage);
    sol_wasm_represented_output_free(&restored); sol_wasm_represented_output_free(&baseline);
    propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44_trace_writeback_owner_mutations(const char *directory) {
    PropagationPipeline pipeline; SolWasmRepresentedOutput baseline, restored;
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&baseline);
    sol_wasm_represented_output_init(&restored);
    bool ok = propagation_pipeline_build(&pipeline, directory);
    SolWasmRepresentedBuildRequest request = {&pipeline.lowered, directory, NULL};
    SolMirRuntimeCleanupTransition *normal = NULL, *failure = NULL;
    SolMirRuntimeCleanupAction *writeback = NULL;
    if (ok) for (size_t event = 0; event < pipeline.cleanup.event_count; ++event) {
        SolMirRuntimeCleanupEvent *candidate = &pipeline.cleanup.events[event];
        SolMirRuntimeCleanupTransition *n = NULL, *f = NULL;
        for (size_t i = 0; i < candidate->transitions.count; ++i) {
            SolMirRuntimeCleanupTransition *transition = &pipeline.cleanup.transitions[
                candidate->transitions.offset + i];
            if (transition->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL) n = transition;
            if (transition->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE) f = transition;
        }
        if (n != NULL && f != NULL)
            for (size_t i = 0; i < n->actions.count; ++i) {
                SolMirRuntimeCleanupAction *candidate_action = &pipeline.cleanup.actions[
                    n->actions.offset + i];
                if (candidate_action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK) {
                    normal = n; failure = f; writeback = candidate_action; break;
                }
            }
        if (writeback != NULL) break;
    }
    if (ok) ok = normal != NULL && failure != NULL && writeback != NULL
        && sol_wasm_represented_build(&request, &baseline, &pipeline.diagnostics)
            == SOL_WASM_REPRESENTED_OK;
#define CHECK_P44_WRITEBACK_OWNER_REJECT(name, edit, restore) do { \
    edit; bool rejected = p44_trace_owner_reject(&request, &pipeline.diagnostics, directory); \
    ok = ok && rejected; restore; \
    pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
} while (0)
    if (ok) {
        SolMirRuntimeSlice saved_normal = normal->actions, saved_failure = failure->actions;
        CHECK_P44_WRITEBACK_OWNER_REJECT("removed", normal->actions.count = 0, normal->actions = saved_normal);
        CHECK_P44_WRITEBACK_OWNER_REJECT("moved", failure->actions = saved_normal, failure->actions = saved_failure);
        unsigned saved_flags = writeback->flags;
        CHECK_P44_WRITEBACK_OWNER_REJECT("flags", writeback->flags ^= SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY,
            writeback->flags = saved_flags);
        SolMirRuntimeCleanupFailureSource saved_source = failure->failure_source;
        CHECK_P44_WRITEBACK_OWNER_REJECT("source", failure->failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE,
            failure->failure_source = saved_source);
        size_t saved_site = failure->failure_site;
        CHECK_P44_WRITEBACK_OWNER_REJECT("site", failure->failure_site = SOL_MIR_RUNTIME_NONE,
            failure->failure_site = saved_site);
        uint32_t saved_mask = failure->failure_mask;
        CHECK_P44_WRITEBACK_OWNER_REJECT("mask", failure->failure_mask ^= UINT32_C(1),
            failure->failure_mask = saved_mask);
    }
#undef CHECK_P44_WRITEBACK_OWNER_REJECT
    if (ok) ok = sol_wasm_represented_build(&request, &restored, &pipeline.diagnostics)
        == SOL_WASM_REPRESENTED_OK && restored.bytes.count == baseline.bytes.count
        && memcmp(restored.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
        && usage_equal(&restored.usage, &baseline.usage);
    sol_wasm_represented_output_free(&restored); sol_wasm_represented_output_free(&baseline);
    propagation_pipeline_free(&pipeline);
    return ok;
}

/* The paired method fixture keeps `launch` as its one public entry and `fail`
 * as the sole internal zero-argument failure probe.  Both routes are built
 * from the same P3.6 owner, so the test can prove entry reset between an
 * overflow failure and the normal writeback route without inventing a call. */
static bool p44_method_failure_trace_case(const char *directory) {
    static const P44TraceSlot failure_expected[] = {
        {29, 13, 6}, {30, 13, 6}, {31, 13, 6}, {32, 525, 6},
        {20, 1, 0}, {21, 1, 0}, {22, 1, 0}, {23, 533, 6},
    };
    static const P44TraceSlot success_expected[] = {
        {27, 1, 0}, {28, 1, 0}, {33, 1, 0}, {34, 1, 0}, {35, 1, 0}, {4, 257, 0},
        {15, 1, 0}, {16, 1, 0}, {17, 1, 0}, {18, 1, 0},
    };
    PropagationPipeline pipeline; SolWasmRepresentedOutput output; WasmInstance instance = {0};
    P44TraceSlot first[64], normal[64]; size_t first_count = 0, normal_count = 0;
    bool first_overflow = false, normal_overflow = false;
    char entry[256];
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    sol_wasm_represented_test_callback_writeback_probe(true);
    bool pipeline_ok = propagation_pipeline_build_named(&pipeline, directory, "launch", true);
    bool failure_from_p3 = pipeline_ok && p44_trace_expected_from_selected(&pipeline, failure_expected,
        sizeof failure_expected / sizeof *failure_expected, 6);
    bool success_from_p3 = pipeline_ok && p44_trace_expected_from_selected(&pipeline, success_expected,
        sizeof success_expected / sizeof *success_expected, 0);
    bool failure_has_no_writeback = failure_from_p3;
    for (size_t i = 0; failure_has_no_writeback && i < sizeof failure_expected / sizeof *failure_expected;
         ++i)
        failure_has_no_writeback = failure_expected[i].action < pipeline.cleanup.action_count
            && pipeline.cleanup.actions[failure_expected[i].action].kind
                != SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK;
    bool ok = pipeline_ok && failure_from_p3 && success_from_p3 && failure_has_no_writeback
        && sol_wasm_represented_build(
            &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory,
                NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry)
        && wasm_instance_open(&output.bytes, entry, &instance);
    if (ok) {
        bool failure_call = wasm_instance_call_named(&instance,
            SOL_WASM_REPRESENTED_TEST_FAILURE_ENTRY_EXPORT, 0, 2, 6);
        bool failure_writeback = wasm_instance_writebacks(&instance, 0);
        bool failure_trace = wasm_instance_trace(&instance, first, 64, &first_count, &first_overflow);
        bool failure_shape = !first_overflow
            && first_count == sizeof failure_expected / sizeof *failure_expected
            && memcmp(first, failure_expected, sizeof failure_expected) == 0;
        ok = failure_call && failure_writeback && failure_trace && failure_shape;
    }
    if (ok) {
        bool normal_call = wasm_instance_call(&instance, 83, 0, 0);
        bool normal_writeback = wasm_instance_writebacks(&instance, 1);
        bool normal_trace = wasm_instance_trace(&instance, normal, 64, &normal_count, &normal_overflow);
        bool normal_shape = !normal_overflow
            && normal_count == sizeof success_expected / sizeof *success_expected
            && memcmp(normal, success_expected, sizeof success_expected) == 0;
        ok = normal_call && normal_writeback && normal_trace && normal_shape;
    }
    wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_callback_writeback_probe(false);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false); propagation_pipeline_free(&pipeline);
    return ok;
}

/* Quota and physical growth are distinct allocator causes, but P3 assigns both
 * to this one Text supplemental occurrence.  The emitted limit/fault controls
 * are immutable module configuration, so a same-instance retry proves reset
 * and identical failure; a cleared control requires a fresh module. */
static bool p44_allocation_failure_trace_case(const char *directory) {
    static const P44TraceSlot allocation_failure[] = {
        {0, 13, 4}, {1, 13, 4}, {2, 525, 4},
    };
    static const P44TraceSlot allocation_success[] = {
        {3, 1, 0}, {4, 1, 0}, {5, 517, 3},
    };
    PropagationPipeline pipeline; SolWasmRepresentedOutput output; char entry[256];
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    bool ok = propagation_pipeline_build(&pipeline, directory);
    size_t instruction = SOL_MIR_RUNTIME_LOWERED_NONE;
    if (ok) for (size_t i = 0; i < pipeline.concrete.materialization.instruction_count; ++i)
        if (pipeline.concrete.materialization.instructions[i].kind == SOL_MIR_INST_CONST_TEXT) {
            if (instruction != SOL_MIR_RUNTIME_LOWERED_NONE) ok = false;
            instruction = i;
        }
    SolMirRuntimeLoweredImageInstruction *row = instruction < pipeline.lowered.image_instruction_count
        ? &pipeline.lowered.image_instructions[instruction] : NULL;
    SolMirRuntimeCleanupEvent *event = row != NULL && row->cleanup_event < pipeline.cleanup.event_count
        ? &pipeline.cleanup.events[row->cleanup_event] : NULL;
    SolMirRuntimeCleanupSupplementalSite *site = event != NULL
        && event->supplemental_site < pipeline.cleanup.supplemental_site_count
        ? &pipeline.cleanup.supplemental_sites[event->supplemental_site] : NULL;
    SolMirRuntimeCleanupTransition *failure = NULL;
    if (event != NULL) for (size_t i = 0; i < event->transitions.count; ++i) {
        SolMirRuntimeCleanupTransition *candidate = &pipeline.cleanup.transitions[
            event->transitions.offset + i];
        if (candidate->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_SUPPLEMENTAL_P33)
            failure = candidate;
    }
    if (ok) ok = row != NULL && event != NULL && site != NULL && failure != NULL
        && site->allowed_codes == ((UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED - 1))
            | (UINT32_C(1) << (SOL_MIR_RUNTIME_FAILURE_ALLOCATION_LIMIT - 1)))
        && failure->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
        && failure->failure_site == event->supplemental_site
        && failure->continuation == SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; ok && i < failure->actions.count; ++i) {
        const SolMirRuntimeCleanupAction *action = &pipeline.cleanup.actions[failure->actions.offset + i];
        ok = action->kind != SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK;
    }
    const struct { uint64_t requests, bytes; bool grow; int32_t code; } cases[] = {
        {1, 34, false, 5}, {2, 33, false, 5}, {0, 0, true, 4},
    };
    for (size_t item = 0; ok && item < sizeof cases / sizeof *cases; ++item) {
        SolWasmRepresentedLimits limits = sol_wasm_represented_default_limits();
        P44TraceSlot first[64], second[64]; size_t first_count = 0, second_count = 0;
        bool first_overflow = false, second_overflow = false; WasmInstance instance = {0};
        if (cases[item].grow) sol_wasm_represented_test_allocator_memory(1, UINT32_C(65528));
        else { limits.max_allocation_requests = cases[item].requests;
            limits.max_allocation_bytes = cases[item].bytes; }
        sol_wasm_represented_test_p44_cleanup_trace_probe(true);
        sol_wasm_represented_output_init(&output);
        ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory,
                cases[item].grow ? NULL : &limits}, &output, &pipeline.diagnostics)
            == SOL_WASM_REPRESENTED_OK && entry_symbol(&output.bytes, entry, sizeof entry)
            && wasm_instance_open(&output.bytes, entry, &instance)
            && wasm_instance_call(&instance, 0, cases[item].code, 4)
            && wasm_instance_trace(&instance, first, 64, &first_count, &first_overflow)
            && !first_overflow && first_count == sizeof allocation_failure / sizeof *allocation_failure
            && memcmp(first, allocation_failure, sizeof allocation_failure) == 0
            && p44_trace_slots_authentic(&pipeline, first, first_count, 4, NULL)
            && p44_trace_expected_from_selected(&pipeline, allocation_failure,
                sizeof allocation_failure / sizeof *allocation_failure, 4)
            && wasm_instance_call(&instance, 0, cases[item].code, 4)
            && wasm_instance_trace(&instance, second, 64, &second_count, &second_overflow)
            && second_count == first_count && second_overflow == first_overflow
            && memcmp(first, second, first_count * sizeof *first) == 0;
        wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
        sol_wasm_represented_test_p44_cleanup_trace_probe(false);
        sol_wasm_represented_test_allocator_memory(0, 0);
    }
    /* Clearing the build-time fault/limit produces a new ordinary module.  Its
     * allocation succeeds, then the source panic's separate terminal route
     * owns the trace; it is not a fabricated allocation continuation. */
    if (ok) {
        P44TraceSlot slots[64]; size_t count = 0; bool overflow = false; WasmInstance instance = {0};
        sol_wasm_represented_test_p44_cleanup_trace_probe(true);
        sol_wasm_represented_output_init(&output);
        ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory,
                NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && entry_symbol(&output.bytes, entry, sizeof entry)
            && wasm_instance_open(&output.bytes, entry, &instance) && wasm_instance_call(&instance, 0, 1, 3)
            && wasm_instance_trace(&instance, slots, 64, &count, &overflow) && !overflow
            && count == sizeof allocation_success / sizeof *allocation_success
            && memcmp(slots, allocation_success, sizeof allocation_success) == 0
            && p44_trace_slots_authentic(&pipeline, slots, count, 3, NULL)
            && p44_trace_expected_from_selected(&pipeline, allocation_success,
                sizeof allocation_success / sizeof *allocation_success, 3);
        wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
        sol_wasm_represented_test_p44_cleanup_trace_probe(false);
    }
    propagation_pipeline_free(&pipeline);
    return ok;
}

/* C1 is deliberately source-built.  The expected ledgers below are not an
 * action-kind smoke test: they freeze the ordered P3.3 slices selected by the
 * source execution, including the provenance record carried on failure. */
typedef struct {
    const char *leaf, *callable, *symbol;
    int32_t code;
    uint32_t record, code_mask, start, end;
    SolMirRuntimeFailureOriginKind origin;
    const P44TraceSlot *success;
    size_t success_count;
    const P44TraceSlot *failure;
    size_t failure_count;
} C1TraceCase;

static bool c1_slice_valid(const SolMirRuntimeCleanup *cleanup, SolMirRuntimeSlice slice) {
    return cleanup != NULL && slice.offset <= cleanup->action_count
        && slice.count <= cleanup->action_count - slice.offset;
}

static unsigned c1_trace_disposition(const SolMirRuntimeCleanupEvent *event,
    const SolMirRuntimeCleanupTransition *transition, const SolMirRuntimeCleanupAction *action) {
    unsigned result = SOL_WASM_REPRESENTED_TEST_P44_TRACE_EXECUTED;
    if (transition->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE)
        result |= SOL_WASM_REPRESENTED_TEST_P44_TRACE_FAILURE;
    if (event->origin == SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT)
        result |= SOL_WASM_REPRESENTED_TEST_P44_TRACE_IMPLICIT;
    if (transition->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING)
        result |= SOL_WASM_REPRESENTED_TEST_P44_TRACE_PENDING;
    if ((action->flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY) != 0)
        result |= SOL_WASM_REPRESENTED_TEST_P44_TRACE_NORMAL;
    if ((action->flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY) != 0)
        result |= SOL_WASM_REPRESENTED_TEST_P44_TRACE_ACTION_FAILURE;
    if ((action->flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED) != 0)
        result |= SOL_WASM_REPRESENTED_TEST_P44_TRACE_GUARDED;
    return result;
}

/* Reconstruct a slot from the one P3.3 action slice which owns it.  C1's
 * selected transition list is represented by the literal action order below;
 * an action must have one and only one owning transition before it is accepted. */
static bool c1_trace_from_selected_slices(const PropagationPipeline *pipeline,
    const P44TraceSlot *literal, size_t literal_count, uint32_t record,
    P44TraceSlot *derived, size_t capacity) {
    const SolMirRuntimeCleanup *cleanup = &pipeline->cleanup;
    if (literal == NULL || derived == NULL || literal_count == 0 || literal_count > capacity) return false;
    for (size_t slot = 0; slot < literal_count; ++slot) {
        const SolMirRuntimeCleanupEvent *event = NULL;
        const SolMirRuntimeCleanupTransition *transition = NULL;
        if (literal[slot].action >= cleanup->action_count) return false;
        for (size_t event_id = 0; event_id < cleanup->event_count; ++event_id) {
            const SolMirRuntimeCleanupEvent *candidate_event = &cleanup->events[event_id];
            if (candidate_event->transitions.offset > cleanup->transition_count
                || candidate_event->transitions.count > cleanup->transition_count
                    - candidate_event->transitions.offset) return false;
            for (size_t item = 0; item < candidate_event->transitions.count; ++item) {
                const SolMirRuntimeCleanupTransition *candidate = &cleanup->transitions[
                    candidate_event->transitions.offset + item];
                if (!c1_slice_valid(cleanup, candidate->actions)) return false;
                if (literal[slot].action < candidate->actions.offset
                    || literal[slot].action - candidate->actions.offset >= candidate->actions.count) continue;
                if (transition != NULL) return false;
                event = candidate_event;
                transition = candidate;
            }
        }
        if (event == NULL || transition == NULL) return false;
        const SolMirRuntimeCleanupAction *action = &cleanup->actions[literal[slot].action];
        derived[slot] = (P44TraceSlot){literal[slot].action,
            c1_trace_disposition(event, transition, action),
            transition->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE ? record : 0};
    }
    return memcmp(derived, literal, literal_count * sizeof *literal) == 0;
}

static bool c1_trace_matches(const PropagationPipeline *pipeline, const P44TraceSlot *literal,
    size_t literal_count, uint32_t record, const P44TraceSlot *actual, size_t actual_count,
    bool overflow) {
    P44TraceSlot derived[64];
    return literal_count != 0 && literal_count <= sizeof derived / sizeof *derived && !overflow
        && actual_count == literal_count
        && c1_trace_from_selected_slices(pipeline, literal, literal_count, record, derived,
            sizeof derived / sizeof *derived)
        && memcmp(actual, derived, literal_count * sizeof *literal) == 0;
}

static bool c1_trace_excludes(const P44TraceSlot *evaluated, size_t evaluated_count,
    const P44TraceSlot *violation, size_t violation_count) {
    if (evaluated == NULL || violation == NULL || evaluated_count == 0 || violation_count == 0)
        return false;
    for (size_t i = 0; i < evaluated_count; ++i)
        for (size_t j = 0; j < violation_count; ++j)
            if (evaluated[i].action == violation[j].action) return false;
    return true;
}

/* The plan/body selector is intentionally independent of the emitted record.
 * It starts at the named callable's contract predicate plan, binds it to the
 * exact IR predicate expression, then accepts one P3.1 site of the requested
 * origin and complete code mask. */
static bool c1_failure_site(const PropagationPipeline *pipeline, const C1TraceCase *want,
    size_t *site_out) {
    const SolMirOperations *operations;
    const SolMirMaterialization *materialization;
    size_t body = SOL_MIR_RUNTIME_NONE, context_id = SOL_MIR_RUNTIME_NONE, plans = 0, sites = 0,
        selected = SOL_MIR_RUNTIME_NONE;
    if (pipeline == NULL || want == NULL || site_out == NULL) return false;
    operations = &pipeline->concrete.operations;
    materialization = &pipeline->concrete.materialization;
    for (size_t body_id = 0; body_id < operations->predicate_body_count; ++body_id) {
        const SolMirPredicateBody *predicate = &operations->predicate_bodies[body_id];
        if (predicate->context >= materialization->context_count) continue;
        const SolMirPlanContext *context = &materialization->contexts[predicate->context];
        if (context->obligation >= pipeline->ir.obligation_count) continue;
        const SolIrObligation *obligation = &pipeline->ir.obligations[context->obligation];
        if (obligation->predicate >= pipeline->ir.expression_count || obligation->owner >= pipeline->ir.callable_count
            || strcmp(pipeline->ir.callables[obligation->owner].name, want->callable) != 0) continue;
        body = body_id;
        context_id = predicate->context;
        ++plans;
    }
    if (plans != 1 || body == SOL_MIR_RUNTIME_NONE) return false;
    plans = 0;
    for (size_t plan_id = 0; plan_id < operations->predicate_count; ++plan_id) {
        const SolMirOperationPredicatePlan *plan = &operations->predicates[plan_id];
        if (plan->kind == SOL_MIR_OPERATION_PREDICATE_CONTRACT && plan->body == body
            && plan->context == context_id) ++plans;
    }
    if (plans != 1) return false;
    for (size_t site = 0; site < pipeline->conventions.failure_site_count; ++site) {
        const SolMirRuntimeFailureSite *candidate = &pipeline->conventions.failure_sites[site];
        const SolSpan expression = pipeline->ir.expressions[
            pipeline->ir.obligations[materialization->contexts[context_id].obligation].predicate].span;
        if (candidate->owner != body || candidate->origin_kind != want->origin
            || candidate->allowed_codes != want->code_mask
            || (candidate->allowed_codes & (UINT32_C(1) << (unsigned)(want->code - 1))) == 0) continue;
        if (candidate->source.file != 0 || candidate->source.start != want->start
            || candidate->source.end != want->end || candidate->source.start < expression.start
            || candidate->source.end > expression.end) continue;
        selected = site;
        ++sites;
    }
    if (sites != 1) return false;
    *site_out = selected;
    return true;
}

static bool c1_canonical_record(const SolWasmRepresentedOutput *output, const C1TraceCase *want,
    uint32_t observed, size_t site) {
    size_t candidates = 0;
    if (output == NULL || want == NULL || observed == 0 || want->record != observed
        || output->usage.provenance_records == 0) return false;
    for (uint32_t index = 1; index <= output->usage.provenance_records; ++index) {
        ProvenanceRecord record;
        if (!provenance_record(&output->bytes, index, &record)) return false;
        bool match = record.tag == 3 && record.kind == want->origin
            && bytes_equal(record.path, record.path_count, "main.sol")
            && record.start == want->start && record.end == want->end && record.ordinal == site
            && bytes_equal(record.symbol, record.symbol_count, want->symbol);
        if (match) { ++candidates; if (index != observed) return false; }
    }
    return candidates == 1;
}

static bool c1_run_trace_case(const C1TraceCase *want) {
    char directory[512], entry[256];
    PropagationPipeline pipeline; SolWasmRepresentedOutput output; WasmInstance instance = {0};
    P44TraceSlot slots[64]; size_t count = 0, site = SOL_MIR_RUNTIME_NONE; bool overflow = false;
    wasm_val_t observed;
    if (want == NULL || snprintf(directory, sizeof directory, "%s/tests/conformance/%s",
        SOL_TEST_SOURCE_DIR, want->leaf) < 0) return false;
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    sol_wasm_represented_test_callback_writeback_probe(true);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", true)
        && sol_mir_operations_validate(&pipeline.concrete.operations, NULL)
        && sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL)
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)
        && c1_failure_site(&pipeline, want, &site)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL},
            &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && sol_wasm_represented_validate(&output.bytes) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry)
        && wasm_instance_open(&output.bytes, entry, &instance);
    if (ok && want->success != NULL) {
        ok = wasm_instance_call(&instance, 42, 0, 0)
            && wasm_instance_trace(&instance, slots, sizeof slots / sizeof *slots, &count, &overflow)
            && c1_trace_matches(&pipeline, want->success, want->success_count, 0, slots, count, overflow);
        wasm_instance_close(&instance);
        if (ok) ok = wasm_instance_open(&output.bytes, entry, &instance);
    }
    if (ok) {
        ok = wasm_instance_call_named(&instance, SOL_WASM_REPRESENTED_TEST_FAILURE_ENTRY_EXPORT,
            0, want->code, -1)
            && wasm_instance_trace(&instance, slots, sizeof slots / sizeof *slots, &count, &overflow);
        if (ok) wasm_global_get(instance.site, &observed);
        ok = ok && observed.kind == WASM_I32 && observed.of.i32 > 0
            && (uint32_t)observed.of.i32 == want->record && c1_canonical_record(&output, want,
                (uint32_t)observed.of.i32, site)
            && c1_trace_matches(&pipeline, want->failure, want->failure_count, want->record, slots,
                count, overflow);
    }
    wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_callback_writeback_probe(false);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false); propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44c_exact_requires_traces(void) {
    static const P44TraceSlot satisfied[] = {{0,257,0}, {3,1,0}, {2,1,0}};
    static const P44TraceSlot evaluated[] = {{14,1,0}, {15,533,6}};
    static const P44TraceSlot bool_satisfied[] = {
        {0,257,0}, {3,1,0}, {2,1,0}, {11,1,0}, {12,1,0},
    };
    static const P44TraceSlot bool_violation[] = {
        {1,5,5}, {4,1,0}, {5,517,5}, {13,1,0}, {14,1,0}, {15,533,5},
    };
    static const P44TraceSlot violation_only[] = {{1,5,5}, {4,1,0}, {5,517,5}};
    static const C1TraceCase cases[] = {
        {"p44c_requires_divzero", "fail",
            "sol.i1.a398578f1ec73646b6fb03e760d48d7f.ed5dc01c0688627d86c5d6345e71dbcffd189991a0bf3cae43d2ad2287b1bb95",
            3, 6, 6, 202, 207, SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC,
            satisfied, sizeof satisfied / sizeof *satisfied, evaluated, sizeof evaluated / sizeof *evaluated},
        {"p44c_requires_neg_overflow", "fail",
            "sol.i1.a26514c31202139a1030be6938167d0d.0f8f5616c98052d439c4459800f914b9a519d229bd0c3958d4d40ab5c99b523d",
            2, 6, 2, 204, 230, SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC,
            satisfied, sizeof satisfied / sizeof *satisfied, evaluated, sizeof evaluated / sizeof *evaluated},
        {"p44c_requires_bool_forward", "forward",
            "sol.i1.4ab8ff9162ad3aeaa02f4796bca6093b.a60800343682b54f54785d2dd3df2d09fa064f40b329000f0d44def9c3995b8c",
            13, 5, UINT32_C(0x1000), 122, 139, SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT,
            bool_satisfied, sizeof bool_satisfied / sizeof *bool_satisfied,
            bool_violation, sizeof bool_violation / sizeof *bool_violation},
    };
    bool ok = c1_trace_excludes(evaluated, sizeof evaluated / sizeof *evaluated,
        violation_only, sizeof violation_only / sizeof *violation_only);
    for (size_t i = 0; ok && i < sizeof cases / sizeof *cases; ++i) ok = c1_run_trace_case(&cases[i]);
    /* The forwarding failure's first three slots are the callee's violation and
     * terminal, followed by the caller's RESUME_FAILURE/terminal slices. */
    return ok && bool_violation[0].action == 1 && bool_violation[2].action == 5
        && bool_violation[3].action == 13 && bool_violation[5].action == 15
        && bool_violation[0].record == 5 && bool_violation[2].record == 5
        && bool_violation[5].record == 5;
}

static bool p44c_false_violation_trace(void) {
    static const P44TraceSlot expected[] = {{1,5,3}, {4,1,0}, {5,517,3}};
    static const C1TraceCase want = {"p44c_requires_false", "launch",
        "sol.i1.7764055811eae60d659fb0fcabc6a840.f0a811ff804c1d8b046c21f20aef17ce9393f2a352343fae3f55cd68d919e8ed",
        13, 3, UINT32_C(0x1000), 109, 114, SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT,
        NULL, 0, expected, sizeof expected / sizeof *expected};
    char directory[512], entry[256]; PropagationPipeline pipeline;
    SolWasmRepresentedOutput output; WasmInstance instance = {0};
    P44TraceSlot actual[64]; size_t count = 0, selected = SOL_MIR_RUNTIME_NONE;
    bool overflow = false; wasm_val_t site;
    (void)snprintf(directory, sizeof directory, "%s/tests/conformance/p44c_requires_false",
        SOL_TEST_SOURCE_DIR);
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false)
        && c1_failure_site(&pipeline, &want, &selected)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL},
            &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry) && wasm_instance_open(&output.bytes, entry, &instance)
        && wasm_instance_call(&instance, 0, 13, -1)
        && wasm_instance_trace(&instance, actual, sizeof actual / sizeof *actual, &count, &overflow);
    if (ok) wasm_global_get(instance.site, &site);
    ok = ok && selected < pipeline.conventions.failure_site_count && site.kind == WASM_I32
        && site.of.i32 == 3 && c1_canonical_record(&output, &want,
            (uint32_t)site.of.i32, selected)
        && c1_trace_matches(&pipeline, expected, sizeof expected / sizeof *expected, 3, actual, count, overflow);
    wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false); propagation_pipeline_free(&pipeline);
    return ok;
}

/* C1's Bool formal is carried through a predicate edge argument.  All range
 * checks precede dereference, including mutation target discovery. */
static bool p44c_bool_forward_routes(const char *directory) {
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    bool direct = false, ok = propagation_pipeline_build_named(&pipeline, directory, "launch", true);
    const SolMirOperations *operations = &pipeline.concrete.operations;
    for (size_t body_id = 0; ok && body_id < operations->predicate_body_count; ++body_id) {
        const SolMirPredicateBody *body = &operations->predicate_bodies[body_id];
        if (body->inputs.offset > operations->predicate_input_count || body->inputs.count
            > operations->predicate_input_count - body->inputs.offset || body->values.offset
            > operations->predicate_value_count || body->values.count > operations->predicate_value_count
                - body->values.offset || body->blocks.offset > operations->predicate_block_count
            || body->blocks.count > operations->predicate_block_count - body->blocks.offset) { ok = false; break; }
        size_t input = SOL_MIR_OPERATION_NONE, inputs = 0;
        for (size_t value = body->values.offset; value < body->values.offset + body->values.count; ++value) {
            const SolMirPredicateValue *candidate = &operations->predicate_values[value];
            if (candidate->kind == SOL_MIR_PREDICATE_VALUE_INPUT && candidate->definition >= body->inputs.offset
                && candidate->definition < body->inputs.offset + body->inputs.count
                && operations->predicate_inputs[candidate->definition].kind == SOL_MIR_PREDICATE_INPUT_PARAMETER) {
                input = value; ++inputs;
            }
        }
        if (inputs > 1) { ok = false; break; }
        for (size_t edge = 0; inputs == 1 && edge < operations->predicate_edge_count; ++edge) {
            const SolMirPredicateEdge *candidate = &operations->predicate_edges[edge];
            if (candidate->source < body->blocks.offset || candidate->source >= body->blocks.offset + body->blocks.count
                || candidate->target < body->blocks.offset || candidate->target >= body->blocks.offset + body->blocks.count
                || candidate->arguments.offset > operations->predicate_edge_value_count || candidate->arguments.count
                    > operations->predicate_edge_value_count - candidate->arguments.offset) continue;
            const SolMirPredicateBlock *target = &operations->predicate_blocks[candidate->target];
            for (size_t argument = candidate->arguments.offset;
                argument < candidate->arguments.offset + candidate->arguments.count; ++argument)
                if (operations->predicate_edge_values[argument] == input && target->parameters.count != 0) direct = true;
        }
    }
    ok = ok && direct && sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL)
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL);
    propagation_pipeline_free(&pipeline); return ok;
}

static bool p44c_owner_mutations(const char *directory) {
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    SolWasmRepresentedOutput baseline; sol_wasm_represented_output_init(&baseline);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", true);
    SolMirOperations *operations = &pipeline.concrete.operations;
    SolMirMaterialization *materialization = &pipeline.concrete.materialization;
    SolMirRuntimeCleanup *cleanup = &pipeline.cleanup;
    SolMirRuntimeConventions *conventions = &pipeline.conventions;
    uint8_t baseline_hash[32] = {0};
    if (ok) ok = sol_wasm_represented_build(
        &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, &baseline,
        &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && sol_wasm_represented_validate(&baseline.bytes) == SOL_WASM_REPRESENTED_OK;
    if (ok) sha256(baseline.bytes.bytes, baseline.bytes.count, baseline_hash);
#define C1_REJECT(layer, edit, restore) do { \
    SolWasmRepresentedOutput rejected, restored; sol_wasm_represented_output_init(&rejected); \
    sol_wasm_represented_output_init(&restored); \
    edit; pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
    bool rejected_ok = !(layer) && !sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL) \
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, \
            &rejected, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE \
        && rejected.bytes.bytes == NULL && rejected.bytes.count == 0 && usage_zero(&rejected.usage); \
    restore; pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
    bool restored_ok = sol_mir_operations_validate(operations, NULL) \
        && sol_mir_materialization_validate(materialization, NULL) \
        && sol_mir_runtime_conventions_validate(conventions, NULL) \
        && sol_mir_runtime_cleanup_validate(cleanup, NULL) \
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL) \
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, \
            &restored, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK \
        && restored.bytes.count == baseline.bytes.count && usage_equal(&restored.usage, &baseline.usage) \
        && memcmp(restored.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0; \
    if (restored_ok) { uint8_t restored_hash[32]; sha256(restored.bytes.bytes, restored.bytes.count, restored_hash); \
        restored_ok = memcmp(restored_hash, baseline_hash, sizeof baseline_hash) == 0; } \
    ok = ok && rejected_ok && restored_ok; \
    sol_wasm_represented_output_free(&restored); sol_wasm_represented_output_free(&rejected); \
} while (0)
    size_t edge_id = SOL_MIR_RUNTIME_NONE, edge_matches = 0;
    for (size_t i = 0; ok && i < operations->predicate_edge_count; ++i) {
        SolMirPredicateEdge *edge = &operations->predicate_edges[i];
        if (edge->source >= operations->predicate_block_count || edge->target >= operations->predicate_block_count
            || edge->arguments.offset > operations->predicate_edge_value_count || edge->arguments.count
                > operations->predicate_edge_value_count - edge->arguments.offset) continue;
        const SolMirPredicateBlock *target = &operations->predicate_blocks[edge->target];
        bool parameter = false;
        for (size_t a = edge->arguments.offset; a < edge->arguments.offset + edge->arguments.count; ++a) {
            size_t value = operations->predicate_edge_values[a];
            if (value < operations->predicate_value_count && operations->predicate_values[value].kind
                == SOL_MIR_PREDICATE_VALUE_INPUT) parameter = true;
        }
        if (parameter && target->parameters.count != 0) { edge_id = i; ++edge_matches; }
    }
    if (ok) ok = edge_matches == 1;
    if (ok) {
        SolMirPredicateEdge *edge = &operations->predicate_edges[edge_id]; size_t source = edge->source, target = edge->target;
        size_t body_id = operations->predicate_blocks[source].body;
        if (body_id >= operations->predicate_body_count) ok = false;
        const SolMirPredicateBody *body = ok ? &operations->predicate_bodies[body_id] : NULL;
        size_t input_id = SOL_MIR_OPERATION_NONE, value_id = SOL_MIR_OPERATION_NONE, matches = 0;
        if (body == NULL || body->inputs.offset > operations->predicate_input_count || body->inputs.count
            > operations->predicate_input_count - body->inputs.offset || body->entry >= operations->predicate_block_count)
            ok = false;
        for (size_t argument = ok ? edge->arguments.offset : 0;
            ok && argument < edge->arguments.offset + edge->arguments.count; ++argument) {
            size_t candidate_value = operations->predicate_edge_values[argument];
            if (candidate_value >= operations->predicate_value_count) { ok = false; break; }
            SolMirPredicateValue *value = &operations->predicate_values[candidate_value];
            if (value->kind != SOL_MIR_PREDICATE_VALUE_INPUT || value->definition < body->inputs.offset
                || value->definition >= body->inputs.offset + body->inputs.count
                || value->definition >= operations->predicate_input_count) continue;
            if (operations->predicate_inputs[value->definition].recipe == SOL_MIR_RECIPE_NONE
                || value->recipe == SOL_MIR_RECIPE_NONE) continue;
            input_id = value->definition; value_id = candidate_value; ++matches;
        }
        if (ok) ok = matches == 1 && input_id < operations->predicate_input_count
            && value_id < operations->predicate_value_count;
        if (ok) {
            SolMirPredicateBody *mutable_body = &operations->predicate_bodies[body_id];
            size_t entry = mutable_body->entry;
            C1_REJECT(sol_mir_operations_validate(operations, NULL), mutable_body->entry = SOL_MIR_OPERATION_NONE,
                mutable_body->entry = entry);
            SolMirPredicateValue *value = &operations->predicate_values[value_id]; SolMirRecipeId recipe = value->recipe;
            C1_REJECT(sol_mir_operations_validate(operations, NULL), value->recipe = SOL_MIR_RECIPE_NONE,
                value->recipe = recipe);
            SolMirPredicateInput *input = &operations->predicate_inputs[input_id]; recipe = input->recipe;
            C1_REJECT(sol_mir_operations_validate(operations, NULL), input->recipe = SOL_MIR_RECIPE_NONE,
                input->recipe = recipe);
        }
        C1_REJECT(sol_mir_operations_validate(operations, NULL), edge->source = target, edge->source = source);
        C1_REJECT(sol_mir_operations_validate(operations, NULL), edge->target = source, edge->target = target);
    }
    size_t arithmetic = SOL_MIR_RUNTIME_NONE, arithmetic_matches = 0;
    for (size_t i = 0; ok && i < conventions->failure_site_count; ++i) {
        SolMirRuntimeFailureSite *site = &conventions->failure_sites[i];
        if (site->origin_kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC
            && (site->allowed_codes & UINT32_C(6)) != 0) { arithmetic = i; ++arithmetic_matches; }
    }
    if (ok) ok = arithmetic_matches == 0 || arithmetic_matches == 1;
    if (ok && arithmetic_matches == 1 && arithmetic < conventions->failure_site_count) {
        SolMirRuntimeFailureSite *site = &conventions->failure_sites[arithmetic]; uint32_t mask = site->allowed_codes;
        C1_REJECT(sol_mir_runtime_conventions_validate(conventions, NULL), site->allowed_codes = 0, site->allowed_codes = mask);
    }
    size_t check_row = SOL_MIR_RUNTIME_NONE, checks = 0;
    for (size_t i = 0; ok && i < pipeline.lowered.image_terminator_count; ++i) {
        SolMirRuntimeLoweredImageTerminator *row = &pipeline.lowered.image_terminators[i];
        if (row->kind == SOL_MIR_TERM_CHECK_CONTRACT && row->cleanup_event < cleanup->event_count) {
            check_row = i; ++checks;
        }
    }
    if (ok) ok = checks == 1;
    if (ok) {
        SolMirRuntimeLoweredImageTerminator *row = &pipeline.lowered.image_terminators[check_row];
        size_t event_id = row->cleanup_event, saved_event = event_id;
        C1_REJECT(sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL),
            row->cleanup_event = cleanup->event_count, row->cleanup_event = saved_event);
        if (row->block >= materialization->block_count) ok = false;
        SolMirMaterializedTerminator *check = ok ? &materialization->blocks[row->block].terminator : NULL;
        if (check == NULL || check->kind != SOL_MIR_TERM_CHECK_CONTRACT
            || check->failure_edge >= materialization->edge_count
            || check->satisfied_edge >= materialization->edge_count
            || check->failure_edge == check->satisfied_edge) ok = false;
        if (ok) {
            size_t failure_edge = check->failure_edge;
            /* Redirect the CHECK_CONTRACT evaluation-failure edge to the
             * in-range satisfied edge rather than its RESUME_FAILURE block. */
            C1_REJECT(sol_mir_materialization_validate(materialization, NULL),
                check->failure_edge = check->satisfied_edge, check->failure_edge = failure_edge);
        }
        SolMirRuntimeCleanupEvent *event = &cleanup->events[event_id];
        SolMirRuntimeCleanupTransition *satisfied = NULL, *failure = NULL, *violation = NULL;
        size_t satisfied_n = 0, failure_n = 0, violation_n = 0;
        if (event->transitions.offset > cleanup->transition_count || event->transitions.count
            > cleanup->transition_count - event->transitions.offset) ok = false;
        for (size_t i = 0; ok && i < event->transitions.count; ++i) {
            SolMirRuntimeCleanupTransition *t = &cleanup->transitions[event->transitions.offset + i];
            if (!c1_slice_valid(cleanup, t->actions)) { ok = false; break; }
            if (t->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_SATISFIED) { satisfied = t; ++satisfied_n; }
            if (t->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_FAILURE) { failure = t; ++failure_n; }
            if (t->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION) { violation = t; ++violation_n; }
        }
        if (ok) ok = satisfied_n == 1 && failure_n == 1 && violation_n == 1 && satisfied != NULL
            && failure != NULL && violation != NULL;
        if (ok) {
            size_t continuation = satisfied->continuation;
            C1_REJECT(sol_mir_runtime_cleanup_validate(cleanup, NULL), satisfied->continuation = SOL_MIR_RUNTIME_NONE,
                satisfied->continuation = continuation);
            continuation = failure->continuation;
            C1_REJECT(sol_mir_runtime_cleanup_validate(cleanup, NULL), failure->continuation = SOL_MIR_RUNTIME_NONE,
                failure->continuation = continuation);
            SolMirRuntimeCleanupEdgeRole role = violation->edge_role; SolMirRuntimeSlice actions = violation->actions;
            size_t site = violation->failure_site; uint32_t mask = violation->failure_mask;
            C1_REJECT(sol_mir_runtime_cleanup_validate(cleanup, NULL),
                violation->edge_role = SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_FAILURE, violation->edge_role = role);
            C1_REJECT(sol_mir_runtime_cleanup_validate(cleanup, NULL), violation->actions.count = 0,
                violation->actions = actions);
            C1_REJECT(sol_mir_runtime_cleanup_validate(cleanup, NULL), violation->failure_site = SOL_MIR_RUNTIME_NONE,
                violation->failure_site = site);
            C1_REJECT(sol_mir_runtime_cleanup_validate(cleanup, NULL), violation->failure_mask = 0,
                violation->failure_mask = mask);
        }
    }
    size_t resume_call = SOL_MIR_RUNTIME_NONE, wrong_failure_edge = SOL_MIR_RUNTIME_NONE;
    size_t resume_calls = 0, wrong_calls = 0;
    for (size_t i = 0; ok && i < conventions->call_count; ++i) {
        SolMirRuntimeCall *call = &conventions->calls[i];
        if (call->image >= materialization->image_count || call->failure_edge >= materialization->edge_count)
            continue;
        const SolMirMaterializedImage *image = &materialization->images[call->image];
        if (image->source_callable >= pipeline.ir.callable_count) continue;
        size_t block = materialization->edges[call->failure_edge].block;
        if (block >= materialization->block_count || materialization->blocks[block].terminator.kind
            != SOL_MIR_TERM_RESUME_FAILURE) continue;
        if (!strcmp(pipeline.ir.callables[image->source_callable].name, "fail")) {
            resume_call = i; ++resume_calls;
        } else if (!strcmp(pipeline.ir.callables[image->source_callable].name, "launch")) {
            wrong_failure_edge = call->failure_edge; ++wrong_calls;
        }
    }
    if (ok) ok = resume_calls == 1 && wrong_calls == 1 && wrong_failure_edge < materialization->edge_count;
    if (ok) {
        SolMirRuntimeCall *call = &conventions->calls[resume_call]; size_t failure_edge = call->failure_edge;
        /* Associate fail's RESUME_FAILURE with launch's distinct check/caller source. */
        C1_REJECT(sol_mir_runtime_conventions_validate(conventions, NULL), call->failure_edge = wrong_failure_edge,
            call->failure_edge = failure_edge);
    }
#undef C1_REJECT
    sol_wasm_represented_output_free(&baseline); propagation_pipeline_free(&pipeline); return ok;
}

static bool p44c_arithmetic_site_mutation(const char *directory) {
    static const C1TraceCase want = {"p44c_requires_divzero", "fail", "", 3, 6, 6, 202, 207,
        SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC, NULL, 0, NULL, 0};
    PropagationPipeline pipeline; SolWasmRepresentedOutput baseline;
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&baseline);
    size_t selected = SOL_MIR_RUNTIME_NONE, alternate = SOL_MIR_RUNTIME_NONE;
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", true)
        && c1_failure_site(&pipeline, &want, &selected)
        && selected < pipeline.conventions.failure_site_count;
    if (ok) for (size_t i = 0; i < pipeline.conventions.failure_site_count; ++i)
        if (i != selected) { alternate = i; break; }
    if (ok) ok = alternate < pipeline.conventions.failure_site_count;
    if (ok) ok = sol_wasm_represented_build(
        &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, &baseline,
        &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && sol_wasm_represented_validate(&baseline.bytes) == SOL_WASM_REPRESENTED_OK;
    uint8_t baseline_hash[32] = {0};
    if (ok) sha256(baseline.bytes.bytes, baseline.bytes.count, baseline_hash);
#define C1_ARITH_REJECT(layer, edit, restore) do { \
    SolWasmRepresentedOutput rejected, restored; sol_wasm_represented_output_init(&rejected); \
    sol_wasm_represented_output_init(&restored); \
    edit; pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
    bool rejected_ok = !(layer) && !sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL) \
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, \
            &rejected, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE \
        && rejected.bytes.bytes == NULL && rejected.bytes.count == 0 && usage_zero(&rejected.usage); \
    restore; pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
    bool restored_ok = sol_mir_operations_validate(&pipeline.concrete.operations, NULL) \
        && sol_mir_materialization_validate(&pipeline.concrete.materialization, NULL) \
        && sol_mir_runtime_conventions_validate(&pipeline.conventions, NULL) \
        && sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL) \
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL) \
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, \
            &restored, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK \
        && restored.bytes.count == baseline.bytes.count && usage_equal(&restored.usage, &baseline.usage) \
        && memcmp(restored.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0; \
    if (restored_ok) { uint8_t restored_hash[32]; sha256(restored.bytes.bytes, restored.bytes.count, restored_hash); \
        restored_ok = memcmp(restored_hash, baseline_hash, sizeof baseline_hash) == 0; } \
    ok = ok && rejected_ok && restored_ok; \
    sol_wasm_represented_output_free(&restored); sol_wasm_represented_output_free(&rejected); \
} while (0)
    size_t lowered_instruction = SOL_MIR_RUNTIME_NONE, lowered_matches = 0;
    for (size_t i = 0; ok && i < pipeline.lowered.predicate_instruction_count; ++i) {
        SolMirRuntimeLoweredPredicateInstruction *row = &pipeline.lowered.predicate_instructions[i];
        if (row->failure_site != selected || row->instruction >= pipeline.concrete.operations.predicate_instruction_count)
            continue;
        const SolMirPredicateInstruction *instruction = &pipeline.concrete.operations.predicate_instructions[
            row->instruction];
        if (instruction->kind == SOL_MIR_PREDICATE_INST_BINARY
            && (instruction->failures & UINT32_C(6)) != 0) { lowered_instruction = i; ++lowered_matches; }
    }
    if (ok) ok = lowered_matches == 1;
    if (ok) {
        SolMirRuntimeLoweredPredicateInstruction *row = &pipeline.lowered.predicate_instructions[lowered_instruction];
        size_t saved = row->failure_site;
        C1_ARITH_REJECT(sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL),
            row->failure_site = alternate, row->failure_site = saved);
    }
    size_t event_id = SOL_MIR_RUNTIME_NONE, events = 0, transition_id = SOL_MIR_RUNTIME_NONE, transitions = 0;
    for (size_t i = 0; ok && i < pipeline.cleanup.event_count; ++i) {
        SolMirRuntimeCleanupEvent *event = &pipeline.cleanup.events[i];
        if (event->inherited_failure_site != selected || event->transitions.offset > pipeline.cleanup.transition_count
            || event->transitions.count > pipeline.cleanup.transition_count - event->transitions.offset) continue;
        event_id = i; ++events;
        for (size_t j = 0; j < event->transitions.count; ++j) {
            SolMirRuntimeCleanupTransition *transition = &pipeline.cleanup.transitions[event->transitions.offset + j];
            if (transition->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31
                && transition->failure_site == selected) { transition_id = event->transitions.offset + j; ++transitions; }
        }
    }
    if (ok) ok = events == 1 && transitions == 1 && event_id < pipeline.cleanup.event_count
        && transition_id < pipeline.cleanup.transition_count;
    if (ok) {
        SolMirRuntimeCleanupEvent *event = &pipeline.cleanup.events[event_id];
        size_t saved = event->inherited_failure_site;
        C1_ARITH_REJECT(sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL),
            event->inherited_failure_site = alternate, event->inherited_failure_site = saved);
        SolMirRuntimeCleanupTransition *transition = &pipeline.cleanup.transitions[transition_id];
        saved = transition->failure_site;
        C1_ARITH_REJECT(sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL),
            transition->failure_site = alternate, transition->failure_site = saved);
    }
    if (ok) {
        SolMirRuntimeFailureSite *site = &pipeline.conventions.failure_sites[selected];
        uint32_t saved = site->allowed_codes;
        C1_ARITH_REJECT(sol_mir_runtime_conventions_validate(&pipeline.conventions, NULL),
            site->allowed_codes = 0, site->allowed_codes = saved);
    }
#undef C1_ARITH_REJECT
    sol_wasm_represented_output_free(&baseline); propagation_pipeline_free(&pipeline);
    return ok;
}

/* C1's paired parameter predicate has enough control-flow to ensure the
 * resource census counts the emitted predicate closure, rather than merely a
 * scalar entry body.  The source package supplies launch as the public root
 * and fail as a genuine internal root. */
static bool p44c_parameter_forward_resource_determinism(const char *directory) {
    static const SolWasmRepresentedUsage expected = {7,15,12,16,55,1085,0,0,0,0,57,31791,12715,20261,10021};
    static const uint8_t expected_hash[32] = {0x4d,0xfa,0xc4,0xd1,0x18,0x0e,0xb6,0xe3,0x0e,0xf4,0x2f,0x1e,0x8c,0xe6,0xbb,0x3a,0x2a,0x65,0xf6,0x3a,0xfb,0xd6,0xc0,0x8c,0xd8,0xde,0x85,0x49,0xd7,0x45,0x26,0xb1};
    PropagationPipeline pipeline; SolWasmRepresentedOutput baseline, output;
    SolWasmRepresentedOutput forward, reverse, relocated;
    uint8_t hash[32]; size_t forward_ids[2], reverse_ids[2], relocated_ids[2];
    char entry[256], relocation[512], source[768], destination[768];
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&baseline);
    sol_wasm_represented_output_init(&output); sol_wasm_represented_output_init(&forward);
    sol_wasm_represented_output_init(&reverse); sol_wasm_represented_output_init(&relocated);
    sol_wasm_represented_test_fail_allocation_after(0);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false);
    sol_wasm_represented_test_callback_writeback_probe(false);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", true)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory,
            NULL}, &baseline, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
    size_t attempts = sol_wasm_represented_test_allocation_attempts();
    if (ok) {
        sha256(baseline.bytes.bytes, baseline.bytes.count, hash);
        ok = usage_equal(&baseline.usage, &expected) && attempts == 102
            && memcmp(hash, expected_hash, sizeof hash) == 0
            && sol_wasm_represented_validate(&baseline.bytes) == SOL_WASM_REPRESENTED_OK
            && !p44_bytes_contain(&baseline.bytes,
                SOL_WASM_REPRESENTED_TEST_P44_TRACE_OFFSET_EXPORT)
            && !p44_bytes_contain(&baseline.bytes,
                SOL_WASM_REPRESENTED_TEST_WRITEBACK_EXPORT);
    }
    /* Only body ranges owned by linked instances are emitted.  The four
     * predicate edges have a one-word maximum parallel-copy requirement. */
    if (ok) {
        const SolMirConcreteProgram *concrete = &pipeline.concrete;
        const SolMirOperations *operations = &concrete->operations;
        size_t predicate_blocks = 0, predicate_values = 0, predicate_edges = 0, scratch = 0;
        for (size_t body_id = 0; ok && body_id < operations->predicate_body_count; ++body_id) {
            const SolMirPredicateBody *body = &operations->predicate_bodies[body_id];
            bool admitted = false;
            for (size_t callable = 0; callable < concrete->linkage.callable_count; ++callable)
                admitted = admitted || (body->owner_kind == SOL_MIR_PREDICATE_OWNER_INSTANCE
                    && body->instance == concrete->linkage.callables[callable].instance);
            if (!admitted) continue;
            if (body->blocks.offset > operations->predicate_block_count || body->blocks.count
                > operations->predicate_block_count - body->blocks.offset || body->values.offset
                > operations->predicate_value_count || body->values.count
                > operations->predicate_value_count - body->values.offset) { ok = false; break; }
            predicate_blocks += body->blocks.count; predicate_values += body->values.count;
            for (size_t edge = 0; edge < operations->predicate_edge_count; ++edge) {
                const SolMirPredicateEdge *item = &operations->predicate_edges[edge];
                if (item->source < body->blocks.offset || item->source - body->blocks.offset
                    >= body->blocks.count) continue;
                if (item->target < body->blocks.offset || item->target - body->blocks.offset
                    >= body->blocks.count || item->arguments.offset
                        > operations->predicate_edge_value_count || item->arguments.count
                        > operations->predicate_edge_value_count - item->arguments.offset) {
                    ok = false; break;
                }
                ++predicate_edges;
                if (item->arguments.count > scratch) scratch = item->arguments.count;
            }
        }
        ok = ok && baseline.usage.blocks == concrete->materialization.block_count + predicate_blocks
            && baseline.usage.values == concrete->materialization.value_count + predicate_values
            && baseline.usage.edges == concrete->materialization.edge_count + predicate_edges
            && predicate_edges == 4 && scratch == 1;
    }
    SolWasmRepresentedLimits exact = sol_wasm_represented_default_limits();
    exact.max_functions = expected.functions; exact.max_blocks = expected.blocks;
    exact.max_edges = expected.edges; exact.max_values = expected.values;
    exact.max_locals = expected.locals; exact.max_generated_nodes = expected.generated_nodes;
    exact.max_provenance_records = expected.provenance_records;
    exact.max_work_bytes = expected.work_bytes; exact.max_scratch_bytes = expected.scratch_bytes;
    exact.max_owned_bytes = expected.owned_bytes; exact.max_output_bytes = expected.output_bytes;
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
        directory, &exact}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && output.bytes.count == baseline.bytes.count
        && memcmp(output.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
        && usage_equal(&output.usage, &baseline.usage);
    sol_wasm_represented_output_free(&output);
    SolWasmRepresentedLimits defaults = {0};
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
        directory, &defaults}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && output.bytes.count == baseline.bytes.count
        && memcmp(output.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
        && usage_equal(&output.usage, &baseline.usage);
    sol_wasm_represented_output_free(&output);
#define CHECK_C1_RESOURCE_CAP(field, value) do { \
    SolWasmRepresentedLimits cap = sol_wasm_represented_default_limits(); \
    cap.field = (value); sol_wasm_represented_output_init(&output); \
    ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, \
        directory, &cap}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK \
        && output.bytes.count == baseline.bytes.count && usage_equal(&output.usage, &baseline.usage); \
    sol_wasm_represented_output_free(&output); cap.field = (value) - 1; \
    sol_wasm_represented_output_init(&output); \
    ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, \
        directory, &cap}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED \
        && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage); \
    sol_wasm_represented_output_free(&output); \
} while (0)
    CHECK_C1_RESOURCE_CAP(max_functions, expected.functions);
    CHECK_C1_RESOURCE_CAP(max_blocks, expected.blocks);
    CHECK_C1_RESOURCE_CAP(max_edges, expected.edges);
    CHECK_C1_RESOURCE_CAP(max_values, expected.values);
    CHECK_C1_RESOURCE_CAP(max_locals, expected.locals);
    CHECK_C1_RESOURCE_CAP(max_generated_nodes, expected.generated_nodes);
    CHECK_C1_RESOURCE_CAP(max_provenance_records, expected.provenance_records);
    CHECK_C1_RESOURCE_CAP(max_work_bytes, expected.work_bytes);
    CHECK_C1_RESOURCE_CAP(max_scratch_bytes, expected.scratch_bytes);
    CHECK_C1_RESOURCE_CAP(max_owned_bytes, expected.owned_bytes);
    CHECK_C1_RESOURCE_CAP(max_output_bytes, expected.output_bytes);
#undef CHECK_C1_RESOURCE_CAP
#define CHECK_C1_PARTIAL_LIMIT(field) do { \
    SolWasmRepresentedLimits partial = sol_wasm_represented_default_limits(); \
    partial.field = 0; sol_wasm_represented_output_init(&output); \
    ok = ok && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, \
        directory, &partial}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_INVALID_ARGUMENT \
        && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage); \
    sol_wasm_represented_output_free(&output); \
} while (0)
    CHECK_C1_PARTIAL_LIMIT(max_functions); CHECK_C1_PARTIAL_LIMIT(max_blocks);
    CHECK_C1_PARTIAL_LIMIT(max_edges); CHECK_C1_PARTIAL_LIMIT(max_values);
    CHECK_C1_PARTIAL_LIMIT(max_locals); CHECK_C1_PARTIAL_LIMIT(max_generated_nodes);
    CHECK_C1_PARTIAL_LIMIT(max_table_elements); CHECK_C1_PARTIAL_LIMIT(max_static_data_bytes);
    CHECK_C1_PARTIAL_LIMIT(max_allocation_requests); CHECK_C1_PARTIAL_LIMIT(max_allocation_bytes);
    CHECK_C1_PARTIAL_LIMIT(max_provenance_records); CHECK_C1_PARTIAL_LIMIT(max_work_bytes);
    CHECK_C1_PARTIAL_LIMIT(max_scratch_bytes); CHECK_C1_PARTIAL_LIMIT(max_owned_bytes);
    CHECK_C1_PARTIAL_LIMIT(max_output_bytes);
#undef CHECK_C1_PARTIAL_LIMIT
    SolWasmRepresentedLimits hard_cap = sol_wasm_represented_default_limits();
    hard_cap.max_functions = 256;
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
        directory, &hard_cap}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
    sol_wasm_represented_output_free(&output); hard_cap.max_functions = 257;
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
        directory, &hard_cap}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_INVALID_ARGUMENT
        && output.bytes.bytes == NULL && usage_zero(&output.usage);
    sol_wasm_represented_output_free(&output); hard_cap = sol_wasm_represented_default_limits();
    hard_cap.max_provenance_records = 32768;
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
        directory, &hard_cap}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
    sol_wasm_represented_output_free(&output); hard_cap.max_provenance_records = 32769;
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
        directory, &hard_cap}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_INVALID_ARGUMENT
        && output.bytes.bytes == NULL && usage_zero(&output.usage);
    sol_wasm_represented_output_free(&output);
    for (size_t ordinal = 1; ok && ordinal <= attempts; ++ordinal) {
        represented_fault(FAULT_REQUIRES_FORWARD, ordinal, ordinal);
        sol_wasm_represented_output_init(&output);
        ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_ALLOCATION_FAILED
            && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage);
        sol_wasm_represented_output_free(&output); sol_wasm_represented_test_fail_allocation_after(0);
        sol_wasm_represented_output_init(&output);
        if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && output.bytes.count == baseline.bytes.count
            && memcmp(output.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
            && usage_equal(&output.usage, &baseline.usage)
            && sol_wasm_represented_test_allocation_attempts() == attempts;
        sol_wasm_represented_output_free(&output);
    }
    sol_wasm_represented_test_fail_allocation_after(0);
    if (ok) ok = build_multiroot(directory, false, &forward, forward_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && build_multiroot(directory, true, &reverse, reverse_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && forward_ids[0] == 1 && forward_ids[1] == 2
        && reverse_ids[0] == forward_ids[0] && reverse_ids[1] == forward_ids[1]
        && forward.bytes.count == baseline.bytes.count
        && memcmp(forward.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
        && reverse.bytes.count == forward.bytes.count
        && memcmp(reverse.bytes.bytes, forward.bytes.bytes, forward.bytes.count) == 0
        && usage_equal(&forward.usage, &baseline.usage) && usage_equal(&reverse.usage, &baseline.usage);
    if (ok) {
        sha256(forward.bytes.bytes, forward.bytes.count, hash);
        ok = memcmp(hash, expected_hash, sizeof hash) == 0 && entry_symbol(&forward.bytes, entry,
            sizeof entry) && invoke_named(&forward.bytes, entry, 42, 0, 0)
            && invoke_named(&reverse.bytes, entry, 42, 0, 0);
    }
    (void)mkdir(SOL_TEST_BINARY_DIR, 0700);
    (void)snprintf(relocation, sizeof relocation, "%s/p44c_requires_parameter_forward_relocated",
        SOL_TEST_BINARY_DIR);
    (void)mkdir(relocation, 0700);
    (void)snprintf(source, sizeof source, "%s/main.sol", directory);
    (void)snprintf(destination, sizeof destination, "%s/main.sol", relocation);
    FILE *input = fopen(source, "rb"), *copy = fopen(destination, "wb");
    if (input == NULL || copy == NULL) ok = false;
    if (input != NULL && copy != NULL) {
        uint8_t buffer[256]; size_t count = 0;
        while ((count = fread(buffer, 1, sizeof buffer, input)) != 0)
            if (fwrite(buffer, 1, count, copy) != count) ok = false;
    }
    if (input != NULL) fclose(input);
    if (copy != NULL) fclose(copy);
    if (ok) ok = build_multiroot(relocation, false, &relocated, relocated_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && relocated_ids[0] == forward_ids[0]
        && relocated_ids[1] == forward_ids[1] && relocated.bytes.count == forward.bytes.count
        && memcmp(relocated.bytes.bytes, forward.bytes.bytes, forward.bytes.count) == 0
        && usage_equal(&relocated.usage, &forward.usage) && invoke_named(&relocated.bytes, entry, 42, 0, 0);
    sol_wasm_represented_output_free(&relocated); sol_wasm_represented_output_free(&reverse);
    sol_wasm_represented_output_free(&forward); sol_wasm_represented_output_free(&output);
    sol_wasm_represented_output_free(&baseline); propagation_pipeline_free(&pipeline);
    return ok;
}

/* Keep C1's public acceptance surface in one source-derived matrix.  A named
 * `fail` root is paired with the source entry and made independently callable,
 * so failure cases do not depend on launch retaining an unreachable companion. */
typedef struct {
    const char *leaf, *root, *provenance_callable;
    int64_t value;
    int32_t code;
    uint32_t code_mask, start, end;
    SolMirRuntimeFailureOriginKind origin;
    SolWasmRepresentedResult build_result;
} C1AcceptanceCase;

static bool c1_matrix_failure_record(const PropagationPipeline *pipeline,
    const C1AcceptanceCase *test, const SolWasmRepresentedOutput *output, uint32_t observed) {
    C1TraceCase site = {test->leaf, test->provenance_callable, "", test->code, 0,
        test->code_mask, test->start, test->end, test->origin, NULL, 0, NULL, 0};
    ProvenanceRecord record;
    size_t selected = SOL_MIR_RUNTIME_NONE;
    bool selected_ok = observed != 0 && c1_failure_site(pipeline, &site, &selected);
    bool record_ok = selected_ok && selected < pipeline->conventions.failure_site_count
        && provenance_record(&output->bytes, observed, &record)
        && record.tag == 3 && record.kind == test->origin
        && bytes_equal(record.path, record.path_count, "main.sol")
        && record.start == test->start && record.end == test->end
        && record.ordinal == selected;
    return record_ok;
}

static bool c1_run_acceptance_case(const C1AcceptanceCase *test) {
    char directory[512], entry[256];
    SolWasmRepresentedOutput output;
    WasmInstance instance = {0};
    PropagationPipeline pipeline;
    wasm_val_t observed;
    bool failure = test != NULL && test->code != 0;
    bool internal_failure = failure && test != NULL && !strcmp(test->root, "fail");
    if (test == NULL || snprintf(directory, sizeof directory, "%s/tests/conformance/%s",
        SOL_TEST_SOURCE_DIR, test->leaf) < 0) return false;
    sol_wasm_represented_output_init(&output);
    sol_wasm_represented_test_callback_writeback_probe(internal_failure);
    bool ok = build_named_root(directory, test->root, &output, NULL, test->build_result);
    if (test->build_result != SOL_WASM_REPRESENTED_OK) {
        ok = ok && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage);
        sol_wasm_represented_output_free(&output);
        sol_wasm_represented_test_callback_writeback_probe(false);
        return ok;
    }
    propagation_pipeline_init(&pipeline);
    bool module_ready = ok && sol_wasm_represented_validate(&output.bytes) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry)
        && wasm_instance_open(&output.bytes, entry, &instance);
    ok = module_ready;
    bool invoked = ok && (internal_failure
        ? wasm_instance_call_named(&instance, SOL_WASM_REPRESENTED_TEST_FAILURE_ENTRY_EXPORT,
            test->value, test->code, -1)
        : wasm_instance_call(&instance, test->value, test->code, failure ? -1 : 0));
    ok = invoked;
    if (ok && failure) {
        wasm_global_get(instance.site, &observed);
        bool built = propagation_pipeline_build_named(&pipeline, directory, "launch", internal_failure);
        bool exact = observed.kind == WASM_I32 && observed.of.i32 > 0 && built
            && c1_matrix_failure_record(&pipeline, test, &output, (uint32_t)observed.of.i32);
        ok = exact;
    }
    wasm_instance_close(&instance);
    propagation_pipeline_free(&pipeline);
    sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_callback_writeback_probe(false);
    return ok;
}

static bool p44c_acceptance_matrix(void) {
    static const C1AcceptanceCase cases[] = {
        {"p44c_requires_true", "launch", NULL, 42, 0, 0, 0, 0, 0,
            SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_false", "launch", "launch", 0, 13, UINT32_C(0x1000), 109, 114,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT, SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_short_circuit", "launch", NULL, 42, 0, 0, 0, 0, 0,
            SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_short_circuit", "fail", "fail", 0, 13, UINT32_C(0x1000), 218, 223,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT, SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_multiple", "launch", NULL, 42, 0, 0, 0, 0, 0,
            SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_multiple", "fail", "fail", 0, 13, UINT32_C(0x1000), 206, 211,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT, SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_parameter_forward", "launch", NULL, 42, 0, 0, 0, 0, 0,
            SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_parameter_forward", "fail", "forward", 0, 13, UINT32_C(0x1000),
            124, 146, SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT, SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_bool_forward", "launch", NULL, 42, 0, 0, 0, 0, 0,
            SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_bool_forward", "fail", "forward", 0, 13, UINT32_C(0x1000),
            122, 139, SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT, SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_neg_overflow", "launch", NULL, 42, 0, 0, 0, 0, 0,
            SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_neg_overflow", "fail", "fail", 0, 2, 2, 204, 230,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC, SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_add_overflow", "launch", NULL, 42, 0, 0, 0, 0, 0,
            SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_add_overflow", "fail", "fail", 0, 2, 2, 207, 230,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC, SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_sub_overflow", "launch", NULL, 42, 0, 0, 0, 0, 0,
            SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_sub_overflow", "fail", "fail", 0, 2, 2, 208, 237,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC, SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_mul_overflow", "launch", NULL, 42, 0, 0, 0, 0, 0,
            SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_mul_overflow", "fail", "fail", 0, 2, 2, 207, 230,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC, SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_div_overflow", "launch", NULL, 42, 0, 0, 0, 0, 0,
            SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_div_overflow", "fail", "fail", 0, 2, 6, 208, 238,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC, SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_rem_overflow", "launch", NULL, 42, 0, 0, 0, 0, 0,
            SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_rem_overflow", "fail", "fail", 0, 2, 6, 208, 238,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC, SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_divzero", "launch", NULL, 42, 0, 0, 0, 0, 0,
            SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_divzero", "fail", "fail", 0, 3, 6, 202, 207,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC, SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_rem_zero", "launch", NULL, 42, 0, 0, 0, 0, 0,
            SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_rem_zero", "fail", "fail", 0, 3, 6, 203, 208,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC, SOL_WASM_REPRESENTED_OK},
        {"p44c_requires_text", "launch", NULL, 0, 0, 0, 0, 0, 0,
            SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE},
        {"p44_guard_call_reject", "launch", NULL, 0, 0, 0, 0, 0, 0,
            SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i)
        if (!c1_run_acceptance_case(&cases[i])) return false;
    return true;
}

typedef enum {
    C2A_SOURCE_ORDINARY,
    C2A_SOURCE_IMPORT_OWNER,
    C2A_SOURCE_EXCLUSIVE_OWNER,
    C2A_SOURCE_QUALIFIED_RESULT,
    C2A_SOURCE_REFINEMENT_OWNER,
    C2A_SOURCE_P3_VALID_BACKEND_REJECT,
} C2aSourceShape;

typedef struct {
    const char *leaf;
    int64_t value;
    int32_t code;
    SolMirRuntimeFailureOriginKind origin;
    SolWasmRepresentedResult build_result;
    uint32_t record, code_mask, start, end, ordinal;
    const char *symbol;
    C2aSourceShape source_shape;
} C2aAcceptanceCase;

static bool c2a_source_unsupported_case(const C2aAcceptanceCase *test, const char *directory,
    SolWasmRepresentedOutput *output) {
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    bool ok = test != NULL && directory != NULL && output != NULL
        && (test->source_shape == C2A_SOURCE_IMPORT_OWNER
            ? propagation_pipeline_build_named_with_import(&pipeline, directory, "launch", false,
                "count", SOL_MIR_PROGRAM_ROOT_ENTRY)
            : propagation_pipeline_build_named(&pipeline, directory, "launch", false))
        && sol_mir_operations_validate(&pipeline.concrete.operations, NULL)
        && sol_mir_materialization_validate(&pipeline.concrete.materialization, NULL)
        && sol_mir_runtime_conventions_validate(&pipeline.conventions, NULL)
        && sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL)
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL);
    const SolMirOperations *operations = &pipeline.concrete.operations;
    size_t matching_bodies = 0, matching_rows = 0, exclusive_inputs = 0;
    size_t exclusive_slots = 0, exclusive_locals = 0;
    size_t qualified_bodies = 0, qualified_rows = 0, success_results = 0;
    size_t refinement_bodies = 0, refinement_rows = 0;
    for (size_t body_id = 0; ok && body_id < operations->predicate_body_count; ++body_id) {
        const SolMirPredicateBody *body = &operations->predicate_bodies[body_id];
        bool row_matches = false;
        if (body_id < pipeline.lowered.predicate_body_count) {
            const SolMirRuntimeLoweredPredicateBody *row = &pipeline.lowered.predicate_bodies[body_id];
            row_matches = row->state == SOL_MIR_RUNTIME_LOWERED_PRESENT && row->body == body_id
                && row->owner_kind == body->owner_kind && row->image == body->instance
                && row->import_id == body->import && row->context == body->context
                && row->phase == body->phase && row->outcome == body->outcome;
        }
        if (body->phase != SOL_CONTRACT_ENSURES
            || body->outcome != SOL_CONTRACT_OUTCOME_ALWAYS) {
            if (body->phase == SOL_CONTRACT_ENSURES
                && (body->outcome == SOL_CONTRACT_OUTCOME_SUCCESS
                    || body->outcome == SOL_CONTRACT_OUTCOME_FAILURE)) {
                ++qualified_bodies;
                qualified_rows += row_matches;
                for (size_t i = body->inputs.offset;
                    i < body->inputs.offset + body->inputs.count; ++i)
                    success_results += operations->predicate_inputs[i].kind
                        == SOL_MIR_PREDICATE_INPUT_SUCCESS_RESULT;
            }
            if (body->refinement_self_recipe != SOL_MIR_RECIPE_NONE) {
                ++refinement_bodies; refinement_rows += row_matches;
            }
            continue;
        }
        bool owner_match = test->source_shape == C2A_SOURCE_IMPORT_OWNER
            ? body->owner_kind == SOL_MIR_PREDICATE_OWNER_IMPORT
            : body->owner_kind == SOL_MIR_PREDICATE_OWNER_INSTANCE;
        if (!owner_match) continue;
        ++matching_bodies;
        matching_rows += row_matches;
        if (body->inputs.offset > operations->predicate_input_count || body->inputs.count
            > operations->predicate_input_count - body->inputs.offset) { ok = false; break; }
        for (size_t i = body->inputs.offset; i < body->inputs.offset + body->inputs.count; ++i) {
            const SolMirPredicateInput *input = &operations->predicate_inputs[i];
            if (input->kind == SOL_MIR_PREDICATE_INPUT_PARAMETER
                && input->access == SOL_ACCESS_EXCLUSIVE) ++exclusive_inputs;
        }
    }
    if (ok && test->source_shape == C2A_SOURCE_IMPORT_OWNER)
        ok = matching_bodies == 1 && matching_rows == 1 && pipeline.lowered.import_count != 0;
    if (ok && test->source_shape == C2A_SOURCE_EXCLUSIVE_OWNER) {
        size_t exclusive_calls = 0;
        for (size_t i = 0; i < pipeline.conventions.signature_slot_count; ++i)
            exclusive_slots += pipeline.conventions.signature_slots[i].access == SOL_ACCESS_EXCLUSIVE;
        for (size_t i = 0; i < pipeline.concrete.materialization.local_count; ++i)
            exclusive_locals += pipeline.concrete.materialization.locals[i].access
                == SOL_ACCESS_EXCLUSIVE;
        for (size_t i = 0; i < pipeline.conventions.call_count; ++i) {
            const SolMirRuntimeCall *call = &pipeline.conventions.calls[i];
            if (call->signature >= pipeline.conventions.signature_count
                || i >= pipeline.lowered.call_count) continue;
            const SolMirRuntimeSignature *signature = &pipeline.conventions.signatures[
                call->signature];
            const SolMirRuntimeLoweredCall *lowered = &pipeline.lowered.calls[i];
            if (signature->slots.count == 1
                && signature->slots.offset < pipeline.conventions.signature_slot_count
                && pipeline.conventions.signature_slots[signature->slots.offset].access
                    == SOL_ACCESS_EXCLUSIVE
                && call->writebacks.count == 1
                && lowered->state == SOL_MIR_RUNTIME_LOWERED_PRESENT
                && lowered->call == i && lowered->signature == call->signature
                && lowered->writebacks.offset == call->writebacks.offset
                && lowered->writebacks.count == call->writebacks.count) ++exclusive_calls;
        }
        ok = matching_bodies == 1 && matching_rows == 1 && exclusive_inputs == 0
            && exclusive_slots == 2 && exclusive_locals == 1 && exclusive_calls == 1;
    }
    if (ok && test->source_shape == C2A_SOURCE_QUALIFIED_RESULT)
        ok = qualified_bodies == 2 && qualified_rows == 2 && success_results == 1;
    if (ok && test->source_shape == C2A_SOURCE_REFINEMENT_OWNER)
        ok = matching_bodies == 1 && matching_rows == 1 && refinement_bodies == 2
            && refinement_rows == 2;
    if (ok && test->source_shape == C2A_SOURCE_P3_VALID_BACKEND_REJECT)
        ok = pipeline.diagnostics.count == 0;
    SolWasmRepresentedResult backend = SOL_WASM_REPRESENTED_INVALID_ARGUMENT;
    if (ok) backend = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, NULL}, output, &pipeline.diagnostics);
    if (ok) ok = backend == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE
        && output->bytes.bytes == NULL && output->bytes.count == 0 && usage_zero(&output->usage);
    propagation_pipeline_free(&pipeline); return ok;
}

static bool c2a_failure_record(const PropagationPipeline *pipeline,
    const SolWasmRepresentedOutput *output, uint32_t observed,
    const C2aAcceptanceCase *test) {
    ProvenanceRecord record;
    if (pipeline == NULL || output == NULL || test == NULL || observed == 0 || test->code <= 0
        || !provenance_record(&output->bytes, observed, &record)) return false;
    if (observed != test->record || record.tag != 3 || record.kind != (uint8_t)test->origin
        || !bytes_equal(record.path, record.path_count, "main.sol")
        || record.start != test->start || record.end != test->end
        || !bytes_equal(record.symbol, record.symbol_count, test->symbol)) return false;
    size_t matches = 0, selected = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < pipeline->conventions.failure_site_count; ++i) {
        const SolMirRuntimeFailureSite *site = &pipeline->conventions.failure_sites[i];
        if (site->origin_kind == test->origin && site->source.file == 0
            && site->source.start == test->start && site->source.end == test->end
            && site->allowed_codes == test->code_mask
            && (site->allowed_codes & (UINT32_C(1) << ((unsigned)test->code - 1u))) != 0) {
            if (test->origin >= SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC) {
                if (site->owner >= pipeline->concrete.operations.predicate_body_count) continue;
                const SolMirPredicateBody *body = &pipeline->concrete.operations.predicate_bodies[
                    site->owner];
                if (body->owner_kind != SOL_MIR_PREDICATE_OWNER_INSTANCE
                    || body->phase != SOL_CONTRACT_ENSURES
                    || body->outcome != SOL_CONTRACT_OUTCOME_ALWAYS) continue;
            }
            selected = i;
            ++matches;
        }
    }
    return matches == 1 && record.ordinal == selected;
}

static bool c2a_run_acceptance_case(const C2aAcceptanceCase *test) {
    char directory[512], entry[256]; SolWasmRepresentedOutput output;
    PropagationPipeline pipeline; WasmInstance instance = {0}; wasm_val_t observed;
    if (test == NULL || snprintf(directory, sizeof directory, "%s/tests/conformance/%s",
            SOL_TEST_SOURCE_DIR, test->leaf) < 0) return false;
    sol_wasm_represented_output_init(&output);
    if (test->source_shape != C2A_SOURCE_ORDINARY) {
        bool ok = c2a_source_unsupported_case(test, directory, &output);
        sol_wasm_represented_output_free(&output); return ok;
    }
    bool ok = build_named_root(directory, "launch", &output, NULL, test->build_result);
    if (test->build_result != SOL_WASM_REPRESENTED_OK) {
        ok = ok && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage);
        sol_wasm_represented_output_free(&output); return ok;
    }
    propagation_pipeline_init(&pipeline);
    int64_t actual_value = 0; int32_t actual_code = 0, actual_site = 0;
    ok = ok && propagation_pipeline_build_named(&pipeline, directory, "launch", false)
        && sol_mir_operations_validate(&pipeline.concrete.operations, NULL)
        && sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL)
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)
        && sol_wasm_represented_validate(&output.bytes) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry)
        && wasm_instance_open(&output.bytes, entry, &instance)
        && wasm_instance_observe(&instance, &actual_value, &actual_code, &actual_site);
    if (ok && (actual_value != test->value || actual_code != test->code
            || (test->code == 0 ? actual_site != 0 : actual_site <= 0))) {
        fprintf(stderr, "C2a observed %s value=%lld code=%d site=%d\n", test->leaf,
            (long long)actual_value, actual_code, actual_site);
        ok = false;
    }
    if (ok && test->code != 0) {
        wasm_global_get(instance.site, &observed);
        ok = observed.kind == WASM_I32 && observed.of.i32 == actual_site
            && c2a_failure_record(&pipeline, &output, (uint32_t)actual_site, test);
        if (!ok) fprintf(stderr, "C2a site validation %s kind=%d observed=%d actual=%d\n",
            test->leaf, (int)observed.kind, observed.of.i32, actual_site);
    }
    wasm_instance_close(&instance); propagation_pipeline_free(&pipeline);
    sol_wasm_represented_output_free(&output); return ok;
}

static bool p44c_ensures_acceptance_matrix(void) {
    static const C2aAcceptanceCase cases[] = {
        {.leaf="p44c_ensures_qualified_result", .value=42,
            .build_result=SOL_WASM_REPRESENTED_OK},
        {.leaf="p44c_qualified_old_true", .value=42, .build_result=SOL_WASM_REPRESENTED_OK},
        {.leaf="p44c_qualified_old_skipped", .value=42, .build_result=SOL_WASM_REPRESENTED_OK},
        {.leaf="p44c_qualified_old_reverse", .value=42, .build_result=SOL_WASM_REPRESENTED_OK},
        {.leaf="p44c_qualified_order", .value=42, .build_result=SOL_WASM_REPRESENTED_OK},
        {.leaf="p44c_qualified_payload_edges", .value=42, .build_result=SOL_WASM_REPRESENTED_OK},
        {.leaf="p44c_qualified_callback_reject",
            .build_result=SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE,
            .source_shape=C2A_SOURCE_P3_VALID_BACKEND_REJECT},
        {.leaf="p44c_qualified_propagate_reject",
            .build_result=SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE,
            .source_shape=C2A_SOURCE_P3_VALID_BACKEND_REJECT},
        {.leaf="p44c_qualified_mixed_call_reject",
            .build_result=SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE,
            .source_shape=C2A_SOURCE_P3_VALID_BACKEND_REJECT},
        {.leaf="p44c_qualified_shape_text",
            .build_result=SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE,
            .source_shape=C2A_SOURCE_P3_VALID_BACKEND_REJECT},
        {.leaf="p44c_qualified_shape_unit",
            .build_result=SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE,
            .source_shape=C2A_SOURCE_P3_VALID_BACKEND_REJECT},
        {.leaf="p44c_qualified_shape_aggregate",
            .build_result=SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE,
            .source_shape=C2A_SOURCE_P3_VALID_BACKEND_REJECT},
        {.leaf="p44c_qualified_shape_multi_failure",
            .build_result=SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE,
            .source_shape=C2A_SOURCE_P3_VALID_BACKEND_REJECT},
        {.leaf="p44c_qualified_shape_failure_payload",
            .build_result=SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE,
            .source_shape=C2A_SOURCE_P3_VALID_BACKEND_REJECT},
        {.leaf="p44c_qualified_shape_non_enum_error",
            .build_result=SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE,
            .source_shape=C2A_SOURCE_P3_VALID_BACKEND_REJECT},
        {.leaf="p44c_ensures_true", .value=42, .build_result=SOL_WASM_REPRESENTED_OK},
        {.leaf="p44c_ensures_false", .code=14, .origin=SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT,
            .build_result=SOL_WASM_REPRESENTED_OK, .record=3, .code_mask=UINT32_C(0x2000),
            .start=107, .end=119, .symbol="sol.i1.8cb69ceb06385880bcf8c9f3a6a07dbf.98a64193f6a64785d2be35a7b3fae7770956bb07017c1f258c0ef70bdc47a216"},
        {.leaf="p44c_ensures_bool", .value=1, .build_result=SOL_WASM_REPRESENTED_OK},
        {.leaf="p44c_ensures_computed_false", .code=14,
            .origin=SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT,
            .build_result=SOL_WASM_REPRESENTED_OK, .record=3, .code_mask=UINT32_C(0x2000),
            .start=116, .end=128, .symbol="sol.i1.82c011e724d1b7c262bf7773491809bb.f5f3f5f741df728807dbdb7b4c3c72670d283d977a482caa45bcae21153af2d8"},
        {.leaf="p44c_ensures_divzero", .code=3,
            .origin=SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC,
            .build_result=SOL_WASM_REPRESENTED_OK, .record=3, .code_mask=UINT32_C(6),
            .start=109, .end=119, .symbol="sol.i1.9d3760ad901cdd46602f94d0bfd311f1.42e76e3ade0ff6942bf33c88fea25aefdd5dbb4f0f966efdf51dc4e1571d3056"},
        {.leaf="p44c_ensures_overflow", .code=2,
            .origin=SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC,
            .build_result=SOL_WASM_REPRESENTED_OK, .record=3, .code_mask=UINT32_C(2),
            .start=114, .end=142, .symbol="sol.i1.139d634c460646bb1ccf3de4af90dfc6.7a3d611d481cb8e85dde09eb401b0e63ac8ebfcccf1837fff663a8aecebe93e4"},
        {.leaf="p44c_ensures_order", .code=14,
            .origin=SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT,
            .build_result=SOL_WASM_REPRESENTED_OK, .record=3, .code_mask=UINT32_C(0x2000),
            .start=111, .end=116, .symbol="sol.i1.cc4c76aee900658fafa72a2f8519b654.6d1fba074e24a03c7293c60dd630aaa60e70afacb2b5cedd03c8ffc19ceb7ea1"},
        {.leaf="p44c_ensures_requires", .value=42, .build_result=SOL_WASM_REPRESENTED_OK},
        {.leaf="p44c_ensures_body_failure", .code=3,
            .origin=SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_ARITHMETIC,
            .build_result=SOL_WASM_REPRESENTED_OK, .record=4, .code_mask=UINT32_C(6),
            .start=135, .end=140, .ordinal=1,
            .symbol="sol.i1.1ffad1b9b80ed0a4dfb0f6305fac887d.7fc2accb01b17e36a40d582617ce9141d896ba347451e6ddef4729c0ea3f9902"},
        {.leaf="p44c_ensures_callee_failure", .code=12,
            .origin=SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_UNREACHABLE,
            .build_result=SOL_WASM_REPRESENTED_OK, .record=4, .code_mask=UINT32_C(0x800),
            .start=96, .end=124,
            .symbol="sol.i1.9d28b8780423ec731e45f1c21a8c9ca6.1530f3a1a9b6d0b17818a66bd4bf73dc82705fce59494c2bb753832f6242d036"},
        {.leaf="p44c_ensures_constant_unit", .build_result=SOL_WASM_REPRESENTED_OK},
        {.leaf="p44c_ensures_old_reject", .value=43, .build_result=SOL_WASM_REPRESENTED_OK},
        {.leaf="p44c_ensures_old_bool", .value=1, .build_result=SOL_WASM_REPRESENTED_OK},
        {.leaf="p44c_ensures_old_two", .value=42, .build_result=SOL_WASM_REPRESENTED_OK},
        {.leaf="p44c_ensures_old_repeat", .value=42, .build_result=SOL_WASM_REPRESENTED_OK},
        {.leaf="p44c_ensures_old_requires", .value=42, .build_result=SOL_WASM_REPRESENTED_OK},
        {.leaf="p44c_ensures_old_many", .value=42, .build_result=SOL_WASM_REPRESENTED_OK},
        {.leaf="p44c_ensures_result_reject", .build_result=SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE},
        {.leaf="p44c_ensures_text_reject", .build_result=SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE},
        {.leaf="p44c_ensures_import_reject", .build_result=SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE,
            .source_shape=C2A_SOURCE_IMPORT_OWNER},
        {.leaf="p44c_ensures_exclusive_reject",
            .build_result=SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE,
            .source_shape=C2A_SOURCE_EXCLUSIVE_OWNER},
        {.leaf="p44c_ensures_qualified_reject",
            .build_result=SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE,
            .source_shape=C2A_SOURCE_QUALIFIED_RESULT},
        {.leaf="p44c_ensures_refinement_reject",
            .build_result=SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE,
            .source_shape=C2A_SOURCE_REFINEMENT_OWNER},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i)
        if (!c2a_run_acceptance_case(&cases[i])) {
            fprintf(stderr, "C2a case failed: %s\n", cases[i].leaf);
            return false;
        }
    return true;
}

static size_t c2b2_diagnostic_code_count(const SolDiagnostics *diagnostics, const char *code) {
    size_t count = 0;
    if (diagnostics != NULL && code != NULL)
        for (size_t i = 0; i < diagnostics->count; ++i)
            count += strcmp(diagnostics->items[i].code, code) == 0;
    return count;
}

static bool c2b2_source_gate(const char *leaf, const char *code, size_t expected_stage,
    size_t expected_code_stage, bool pipeline_stops) {
    char directory[512], error[256]; SolDiagnostics diagnostics; SolPackage package;
    SolHirModule hir; SolTypeTable types; SolEffectTable effects; SolContractTable contracts; SolIr ir;
    sol_diagnostics_init(&diagnostics); sol_package_init(&package); sol_hir_module_init(&hir);
    sol_type_table_init(&types); sol_effect_table_init(&effects); sol_contract_table_init(&contracts);
    sol_ir_init(&ir);
    bool ok = leaf != NULL && code != NULL && snprintf(directory, sizeof directory,
        "%s/tests/conformance/%s", SOL_TEST_SOURCE_DIR, leaf) >= 0;
    size_t stage = 0, code_stage = 0;
    bool passed = ok && sol_package_load_directory(&package, directory, &diagnostics, error,
        sizeof error);
    if (c2b2_diagnostic_code_count(&diagnostics, code) != 0) code_stage = 1;
    SolHirFileScope scope = {0};
    if (passed) { stage = 1; scope = (SolHirFileScope){package.files[0].module_name,
        package.files[0].import_start, package.files[0].import_count, package.files[0].item_start,
        package.files[0].item_count}; }
    if (passed) { passed = sol_hir_lower_scoped(&package.source, &package.syntax, &scope, 1, &hir,
        &diagnostics); if (passed) stage = 2; }
    if (code_stage == 0 && c2b2_diagnostic_code_count(&diagnostics, code) != 0) code_stage = 2;
    if (passed) { passed = sol_type_check(&package.source, &package.syntax, &hir, &types,
        &diagnostics); if (passed) stage = 3; }
    if (code_stage == 0 && c2b2_diagnostic_code_count(&diagnostics, code) != 0) code_stage = 3;
    if (passed) { passed = sol_effect_check(&package.source, &package.syntax, &hir, &types, &effects,
        &diagnostics); if (passed) stage = 4; }
    if (code_stage == 0 && c2b2_diagnostic_code_count(&diagnostics, code) != 0) code_stage = 4;
    if (passed) { passed = sol_contract_lower(&package.source, &package.syntax, &hir, &types, &effects,
        &contracts, &diagnostics); if (passed) stage = 5; }
    if (code_stage == 0 && c2b2_diagnostic_code_count(&diagnostics, code) != 0) code_stage = 5;
    if (passed) { passed = sol_ir_lower_scoped(&package.source, &package.syntax, &hir, &types, &effects,
        &contracts, package.files, 1, &ir, &diagnostics); if (passed) stage = 6; }
    if (code_stage == 0 && c2b2_diagnostic_code_count(&diagnostics, code) != 0) code_stage = 6;
    ok = ok && passed == !pipeline_stops && stage == expected_stage
        && code_stage == expected_code_stage
        && c2b2_diagnostic_code_count(&diagnostics, code) == 1;
    if (!ok) fprintf(stderr, "source gate %s passed=%d stage=%zu expected=%zu code-stage=%zu expected-code-stage=%zu diagnostics=%zu code=%zu\n",
        leaf, passed, stage, expected_stage, code_stage, expected_code_stage, diagnostics.count,
        c2b2_diagnostic_code_count(&diagnostics, code));
    sol_ir_free(&ir); sol_contract_table_free(&contracts); sol_effect_table_free(&effects);
    sol_type_table_free(&types); sol_hir_module_free(&hir); sol_package_free(&package);
    sol_diagnostics_free(&diagnostics); return ok;
}

static bool p44c_qualified_source_gates(void) {
    return c2b2_source_gate("p44c_qualified_entry_source_reject", "SOL-ENTRY-003", 5, 3, true)
        && c2b2_source_gate("p44c_qualified_method_source_reject", "SOL-PARSE-019", 6, 1,
            false);
}

static bool p44c_qualified_root_gate(void) {
    char directory[512]; PropagationPipeline pipeline; SolWasmRepresentedOutput output;
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    (void)snprintf(directory, sizeof directory,
        "%s/tests/conformance/p44c_qualified_root_reject", SOL_TEST_SOURCE_DIR);
    bool ok = propagation_pipeline_build_explicit_internal_root(&pipeline, directory, "checked")
        && pipeline.diagnostics.count == 0 && pipeline.conventions.entry_count == 0
        && pipeline.conventions.call_count == 0
        && sol_mir_operations_validate(&pipeline.concrete.operations, NULL)
        && sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL)
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &output, &pipeline.diagnostics)
            == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE
        && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage);
    sol_wasm_represented_output_free(&output); propagation_pipeline_free(&pipeline); return ok;
}

static bool p44c_qualified_mixed_route_gate(void) {
    char directory[512]; PropagationPipeline pipeline; SolWasmRepresentedOutput output;
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    (void)snprintf(directory, sizeof directory,
        "%s/tests/conformance/p44c_qualified_mixed_call_reject", SOL_TEST_SOURCE_DIR);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false)
        && pipeline.diagnostics.count == 0
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL);
    size_t direct = 0, indirect = 0;
    for (size_t i = 0; ok && i < pipeline.conventions.call_count; ++i) {
        const SolMirRuntimeCall *call = &pipeline.conventions.calls[i];
        if (call->call_kind == SOL_IR_CALL_CALLBACK
            && call->target_kind == SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE
            && call->signature < pipeline.conventions.signature_count) {
            SolMirRecipeId result = pipeline.conventions.signatures[call->signature].result;
            indirect += result < pipeline.concrete.representation.recipe_count
                && pipeline.concrete.representation.recipes[result].kind == SOL_MIR_RECIPE_RESULT;
        }
        if (call->internal >= pipeline.concrete.linkage.callable_count
            || pipeline.concrete.linkage.callables[call->internal].instance
                >= pipeline.concrete.materialization.image_count) continue;
        size_t image = pipeline.concrete.linkage.callables[call->internal].instance;
        bool qualified = false;
        for (size_t b = 0; b < pipeline.concrete.operations.predicate_body_count; ++b) {
            const SolMirPredicateBody *body = &pipeline.concrete.operations.predicate_bodies[b];
            qualified = qualified || (body->owner_kind == SOL_MIR_PREDICATE_OWNER_INSTANCE
                && body->instance == image && body->phase == SOL_CONTRACT_ENSURES
                && body->outcome == SOL_CONTRACT_OUTCOME_SUCCESS);
        }
        if (!qualified) continue;
        direct += call->call_kind == SOL_IR_CALL_FUNCTION
            && call->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL;
    }
    ok = ok && direct == 1 && indirect == 1
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &output, &pipeline.diagnostics)
            == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE
        && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage);
    if (!ok) fprintf(stderr, "mixed route direct=%zu indirect=%zu calls=%zu\n", direct, indirect,
        pipeline.conventions.call_count);
    sol_wasm_represented_output_free(&output); propagation_pipeline_free(&pipeline); return ok;
}

typedef struct {
    const char *leaf;
    int32_t code;
    SolMirRuntimeFailureOriginKind origin;
    SolContractClauseKind phase;
    SolContractOutcomeKind outcome;
    uint32_t mask, start, end, record, ordinal;
} C2b2FailureCase;

static bool c2b2_failure_case(const C2b2FailureCase *test) {
    char directory[512], entry[256]; PropagationPipeline pipeline;
    SolWasmRepresentedOutput output; WasmInstance instance = {0}; ProvenanceRecord record = {0};
    int64_t value = 0; int32_t code = 0, site = 0; size_t matches = 0;
    SolWasmRepresentedTestControlTransitionStats transition_stats = {0};
    if (test == NULL || snprintf(directory, sizeof directory, "%s/tests/conformance/%s",
            SOL_TEST_SOURCE_DIR, test->leaf) < 0) return false;
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    size_t selected = SOL_MIR_RUNTIME_NONE;
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false)
        && sol_mir_operations_validate(&pipeline.concrete.operations, NULL)
        && sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL)
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry)
        && wasm_instance_open(&output.bytes, entry, &instance)
        && wasm_instance_observe(&instance, &value, &code, &site)
        && value == 0 && code == test->code && site == (int32_t)test->record
        && provenance_record(&output.bytes, test->record, &record)
        && record.tag == 3 && record.kind == (uint8_t)test->origin
        && bytes_equal(record.path, record.path_count, "main.sol")
        && record.start == test->start && record.end == test->end;
    transition_stats = sol_wasm_represented_test_control_transition_stats();
    if (strcmp(test->leaf, "p44c_qualified_always_false") == 0)
        ok = ok && transition_stats.indexed_lookups == 2
            && transition_stats.indexed_matches == 0
            && transition_stats.legacy_fallback_scans == 2;
    for (size_t i = 0; ok && i < pipeline.conventions.failure_site_count; ++i) {
        const SolMirRuntimeFailureSite *candidate = &pipeline.conventions.failure_sites[i];
        if (candidate->origin_kind == test->origin && candidate->source.file == 0
            && candidate->source.start == test->start && candidate->source.end == test->end
            && candidate->allowed_codes == test->mask) { selected = i; ++matches; }
    }
    if (ok && matches == 1) {
        const SolMirRuntimeFailureSite *failure_site =
            &pipeline.conventions.failure_sites[selected];
        size_t image = SOL_MIR_RUNTIME_NONE, plans = 0;
        if (test->origin == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT
            || test->origin == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC) {
            if (failure_site->owner >= pipeline.concrete.operations.predicate_body_count) ok = false;
            else {
                const SolMirPredicateBody *body = &pipeline.concrete.operations.predicate_bodies[
                    failure_site->owner];
                image = body->instance;
                ok = body->owner_kind == SOL_MIR_PREDICATE_OWNER_INSTANCE
                    && body->phase == test->phase && body->outcome == test->outcome;
                for (size_t i = 0; ok && i < pipeline.concrete.operations.predicate_count; ++i) {
                    const SolMirOperationPredicatePlan *plan =
                        &pipeline.concrete.operations.predicates[i];
                    if (plan->kind == SOL_MIR_OPERATION_PREDICATE_CONTRACT
                        && plan->image == image && plan->body == failure_site->owner
                        && plan->context == body->context
                        && plan->contract_phase == test->phase
                        && plan->contract_outcome == test->outcome
                        && plan->output_recipe == body->output_recipe) ++plans;
                }
                ok = ok && plans == 1;
            }
        } else image = failure_site->owner;
        const char *symbol = ok && image < pipeline.concrete.materialization.image_count
            ? p44_symbol_for_image(&pipeline, image) : NULL;
        ok = ok && record.ordinal == selected && symbol != NULL
            && bytes_equal(record.symbol, record.symbol_count, symbol);
    }
    if (!ok) fprintf(stderr, "qualified failure %s value=%lld code=%d site=%d record=%u span=%u-%u ordinal=%u matches=%zu lookups=%zu indexed=%zu fallback=%zu\n",
        test->leaf, (long long)value, code, site, record.tag, record.start, record.end,
        record.ordinal, matches, transition_stats.indexed_lookups,
        transition_stats.indexed_matches, transition_stats.legacy_fallback_scans);
    wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    propagation_pipeline_free(&pipeline); return ok && matches == 1;
}

static bool p44c_qualified_result_failures(void) {
    static const C2b2FailureCase cases[] = {
        {"p44c_qualified_success_false", 14, SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT,
            SOL_CONTRACT_ENSURES, SOL_CONTRACT_OUTCOME_SUCCESS, UINT32_C(0x2000), 157, 168, 4, 0},
        {"p44c_qualified_failure_false", 14, SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT,
            SOL_CONTRACT_ENSURES, SOL_CONTRACT_OUTCOME_FAILURE, UINT32_C(0x2000), 157, 162, 4, 0},
        {"p44c_qualified_eval_failure", 3, SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC,
            SOL_CONTRACT_ENSURES, SOL_CONTRACT_OUTCOME_SUCCESS, UINT32_C(6), 156, 166, 4, 0},
        {"p44c_qualified_failure_eval", 3,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC,
            SOL_CONTRACT_ENSURES, SOL_CONTRACT_OUTCOME_FAILURE, UINT32_C(6), 166, 171, 4, 0},
        {"p44c_qualified_body_failure", 3, SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_ARITHMETIC,
            SOL_CONTRACT_ENSURES, SOL_CONTRACT_OUTCOME_ALWAYS, UINT32_C(6), 183, 188, 5, 1},
        {"p44c_qualified_failure_body", 3, SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_ARITHMETIC,
            SOL_CONTRACT_ENSURES, SOL_CONTRACT_OUTCOME_ALWAYS, UINT32_C(6), 183, 188, 5, 1},
        {"p44c_qualified_panic", 1, SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_PANIC,
            SOL_CONTRACT_ENSURES, SOL_CONTRACT_OUTCOME_ALWAYS, UINT32_C(1), 115, 143, 5, 0},
        {"p44c_qualified_callee_failure", 12, SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_UNREACHABLE,
            SOL_CONTRACT_ENSURES, SOL_CONTRACT_OUTCOME_ALWAYS, UINT32_C(0x800), 123, 151, 5, 0},
        {"p44c_qualified_old_false", 14, SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT,
            SOL_CONTRACT_ENSURES, SOL_CONTRACT_OUTCOME_SUCCESS, UINT32_C(0x2000), 165, 185, 4, 0},
        {"p44c_qualified_old_predicate_failure", 3,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC,
            SOL_CONTRACT_ENSURES, SOL_CONTRACT_OUTCOME_SUCCESS, UINT32_C(6), 187, 201, 5, 1},
        {"p44c_qualified_old_body_failure", 3,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_ARITHMETIC,
            SOL_CONTRACT_ENSURES, SOL_CONTRACT_OUTCOME_ALWAYS, UINT32_C(6), 214, 223, 5, 1},
        {"p44c_qualified_multihop_failure", 3,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC,
            SOL_CONTRACT_ENSURES, SOL_CONTRACT_OUTCOME_SUCCESS, UINT32_C(6), 160, 170, 5, 0},
        {"p44c_qualified_requires_false", 13, SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT,
            SOL_CONTRACT_REQUIRES, SOL_CONTRACT_OUTCOME_ALWAYS, UINT32_C(0x1000), 148, 153, 4, 0},
        {"p44c_qualified_requires_eval", 3, SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC,
            SOL_CONTRACT_REQUIRES, SOL_CONTRACT_OUTCOME_ALWAYS, UINT32_C(6), 147, 152, 4, 0},
        {"p44c_qualified_always_false", 14, SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT,
            SOL_CONTRACT_ENSURES, SOL_CONTRACT_OUTCOME_ALWAYS, UINT32_C(0x2000), 149, 154, 4, 0},
        {"p44c_qualified_always_eval", 3, SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC,
            SOL_CONTRACT_ENSURES, SOL_CONTRACT_OUTCOME_ALWAYS, UINT32_C(6), 148, 153, 4, 0},
        {"p44c_qualified_failure_old_false", 14,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT, SOL_CONTRACT_ENSURES,
            SOL_CONTRACT_OUTCOME_FAILURE, UINT32_C(0x2000), 200, 244, 4, 0},
        {"p44c_qualified_failure_old_eval", 3,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC, SOL_CONTRACT_ENSURES,
            SOL_CONTRACT_OUTCOME_FAILURE, UINT32_C(6), 198, 207, 5, 1},
        {"p44c_qualified_failure_old_body", 3,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_ARITHMETIC, SOL_CONTRACT_ENSURES,
            SOL_CONTRACT_OUTCOME_ALWAYS, UINT32_C(6), 213, 222, 5, 1},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i)
        if (!c2b2_failure_case(&cases[i])) return false;
    return true;
}

typedef struct {
    const char *leaf;
    SolWasmRepresentedUsage usage;
    uint8_t hash[32];
    size_t attempts, snapshots;
    int64_t value;
    int32_t code, site;
} C2b2FrozenClosure;

static bool c2b1_predecessors_valid(const PropagationPipeline *pipeline);

static bool c2b2_output_identity(const SolWasmRepresentedOutput *output,
    const SolWasmRepresentedOutput *baseline, const C2b2FrozenClosure *frozen) {
    uint8_t hash[32];
    if (output == NULL || baseline == NULL || frozen == NULL
        || output->bytes.count != baseline->bytes.count
        || memcmp(output->bytes.bytes, baseline->bytes.bytes, baseline->bytes.count) != 0
        || !usage_equal(&output->usage, &frozen->usage)) return false;
    sha256(output->bytes.bytes, output->bytes.count, hash);
    return memcmp(hash, frozen->hash, sizeof hash) == 0;
}

static bool c2b2_frozen_closure(const C2b2FrozenClosure *frozen, size_t first, size_t last) {
    char directory[512], relocation[512], source[768], destination[768], entry[256];
    PropagationPipeline pipeline; SolWasmRepresentedOutput baseline, output, relocated;
    SolWasmRepresentedLimits limits, zero = {0}; uint8_t hash[32];
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&baseline);
    sol_wasm_represented_output_init(&output); sol_wasm_represented_output_init(&relocated);
    if (frozen == NULL || snprintf(directory, sizeof directory, "%s/tests/conformance/%s",
            SOL_TEST_SOURCE_DIR, frozen->leaf) < 0) return false;
    sol_wasm_represented_test_fail_allocation_after(0);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false)
        && pipeline.concrete.operations.snapshot_count == frozen->snapshots
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &baseline, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
    size_t attempts = sol_wasm_represented_test_allocation_attempts();
    if (ok) {
        sha256(baseline.bytes.bytes, baseline.bytes.count, hash);
        ok = attempts == frozen->attempts && usage_equal(&baseline.usage, &frozen->usage)
            && memcmp(hash, frozen->hash, sizeof hash) == 0
            && sol_wasm_represented_validate(&baseline.bytes) == SOL_WASM_REPRESENTED_OK
            && entry_symbol(&baseline.bytes, entry, sizeof entry)
            && invoke_named(&baseline.bytes, entry, frozen->value, frozen->code, frozen->site);
        if (!ok) fprintf(stderr, "C2b2 frozen %s attempts=%zu usage={%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%llu,%llu,%zu,%zu,%zu,%zu,%zu} hash=",
            frozen->leaf, attempts, baseline.usage.functions, baseline.usage.blocks,
            baseline.usage.edges, baseline.usage.values, baseline.usage.locals,
            baseline.usage.generated_nodes, baseline.usage.table_elements,
            baseline.usage.static_data_bytes, (unsigned long long)baseline.usage.allocation_requests,
            (unsigned long long)baseline.usage.allocation_bytes, baseline.usage.provenance_records,
            baseline.usage.work_bytes, baseline.usage.scratch_bytes, baseline.usage.owned_bytes,
            baseline.usage.output_bytes);
        if (!ok) { for (size_t i = 0; i < sizeof hash; ++i) fprintf(stderr, "%02x", hash[i]);
            fputc('\n', stderr); }
    }
#define C2B2_BUILD_IDENTITY(limit_pointer) (sol_wasm_represented_build( \
    &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, (limit_pointer)}, \
    &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK \
    && c2b2_output_identity(&output, &baseline, frozen))
    if (ok) ok = C2B2_BUILD_IDENTITY(NULL);
    sol_wasm_represented_output_free(&output); sol_wasm_represented_output_init(&output);
    limits = sol_wasm_represented_default_limits();
    if (ok) ok = C2B2_BUILD_IDENTITY(&limits);
    sol_wasm_represented_output_free(&output); sol_wasm_represented_output_init(&output);
    if (ok) ok = C2B2_BUILD_IDENTITY(&zero);
    sol_wasm_represented_output_free(&output);

    limits = sol_wasm_represented_default_limits();
    limits.max_functions = frozen->usage.functions; limits.max_blocks = frozen->usage.blocks;
    limits.max_edges = frozen->usage.edges; limits.max_values = frozen->usage.values;
    limits.max_locals = frozen->usage.locals;
    limits.max_generated_nodes = frozen->usage.generated_nodes;
    limits.max_provenance_records = frozen->usage.provenance_records;
    limits.max_work_bytes = frozen->usage.work_bytes;
    limits.max_scratch_bytes = frozen->usage.scratch_bytes;
    limits.max_owned_bytes = frozen->usage.owned_bytes;
    limits.max_output_bytes = frozen->usage.output_bytes;
    sol_wasm_represented_output_init(&output);
    if (ok) ok = C2B2_BUILD_IDENTITY(&limits);
    sol_wasm_represented_output_free(&output);
#define C2B2_EXACT_AND_BELOW(field, exact) do { \
    SolWasmRepresentedLimits cap = sol_wasm_represented_default_limits(); cap.field = (exact); \
    sol_wasm_represented_output_init(&output); ok = ok && C2B2_BUILD_IDENTITY(&cap); \
    sol_wasm_represented_output_free(&output); cap.field = (exact) - 1; \
    sol_wasm_represented_output_init(&output); ok = ok && sol_wasm_represented_build( \
        &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, &cap}, &output, \
        &pipeline.diagnostics) == SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED \
        && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage); \
    sol_wasm_represented_output_free(&output); \
} while (0)
    if (ok) {
        C2B2_EXACT_AND_BELOW(max_functions, frozen->usage.functions);
        C2B2_EXACT_AND_BELOW(max_blocks, frozen->usage.blocks);
        C2B2_EXACT_AND_BELOW(max_edges, frozen->usage.edges);
        C2B2_EXACT_AND_BELOW(max_values, frozen->usage.values);
        C2B2_EXACT_AND_BELOW(max_locals, frozen->usage.locals);
        C2B2_EXACT_AND_BELOW(max_generated_nodes, frozen->usage.generated_nodes);
        if (frozen->usage.table_elements != 0)
            C2B2_EXACT_AND_BELOW(max_table_elements, frozen->usage.table_elements);
        if (frozen->usage.static_data_bytes != 0)
            C2B2_EXACT_AND_BELOW(max_static_data_bytes, frozen->usage.static_data_bytes);
        if (frozen->usage.allocation_requests != 0)
            C2B2_EXACT_AND_BELOW(max_allocation_requests, frozen->usage.allocation_requests);
        if (frozen->usage.allocation_bytes != 0)
            C2B2_EXACT_AND_BELOW(max_allocation_bytes, frozen->usage.allocation_bytes);
        C2B2_EXACT_AND_BELOW(max_provenance_records, frozen->usage.provenance_records);
        C2B2_EXACT_AND_BELOW(max_work_bytes, frozen->usage.work_bytes);
        C2B2_EXACT_AND_BELOW(max_scratch_bytes, frozen->usage.scratch_bytes);
        C2B2_EXACT_AND_BELOW(max_owned_bytes, frozen->usage.owned_bytes);
        C2B2_EXACT_AND_BELOW(max_output_bytes, frozen->usage.output_bytes);
    }
#undef C2B2_EXACT_AND_BELOW
#define C2B2_PARTIAL_ZERO(field) do { \
    SolWasmRepresentedLimits partial = sol_wasm_represented_default_limits(); partial.field = 0; \
    sol_wasm_represented_output_init(&output); ok = ok && sol_wasm_represented_build( \
        &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, &partial}, &output, \
        &pipeline.diagnostics) == SOL_WASM_REPRESENTED_INVALID_ARGUMENT \
        && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage); \
    sol_wasm_represented_output_free(&output); \
} while (0)
    if (ok) {
        C2B2_PARTIAL_ZERO(max_functions); C2B2_PARTIAL_ZERO(max_blocks);
        C2B2_PARTIAL_ZERO(max_edges); C2B2_PARTIAL_ZERO(max_values);
        C2B2_PARTIAL_ZERO(max_locals); C2B2_PARTIAL_ZERO(max_generated_nodes);
        C2B2_PARTIAL_ZERO(max_table_elements); C2B2_PARTIAL_ZERO(max_static_data_bytes);
        C2B2_PARTIAL_ZERO(max_allocation_requests); C2B2_PARTIAL_ZERO(max_allocation_bytes);
        C2B2_PARTIAL_ZERO(max_provenance_records); C2B2_PARTIAL_ZERO(max_work_bytes);
        C2B2_PARTIAL_ZERO(max_scratch_bytes); C2B2_PARTIAL_ZERO(max_owned_bytes);
        C2B2_PARTIAL_ZERO(max_output_bytes);
    }
#undef C2B2_PARTIAL_ZERO
    for (size_t ordinal = first; ok && ordinal <= last && ordinal <= attempts; ++ordinal) {
        represented_fault(frozen->attempts == 117 ? FAULT_QUALIFIED_FROZEN
            : FAULT_QUALIFIED_RESULT, ordinal, ordinal);
        sol_wasm_represented_output_init(&output);
        ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &output, &pipeline.diagnostics)
                == SOL_WASM_REPRESENTED_ALLOCATION_FAILED
            && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage);
        sol_wasm_represented_output_free(&output);
        sol_wasm_represented_test_fail_allocation_after(0);
        sol_wasm_represented_output_init(&output);
        if (ok) ok = C2B2_BUILD_IDENTITY(NULL)
            && sol_wasm_represented_test_allocation_attempts() == attempts;
        sol_wasm_represented_output_free(&output);
    }
    sol_wasm_represented_test_fail_allocation_after(0);
#undef C2B2_BUILD_IDENTITY

    (void)mkdir(SOL_TEST_BINARY_DIR, 0700);
    (void)snprintf(relocation, sizeof relocation, "%s/%s-relocated", SOL_TEST_BINARY_DIR,
        frozen->leaf);
    (void)mkdir(relocation, 0700);
    (void)snprintf(source, sizeof source, "%s/main.sol", directory);
    (void)snprintf(destination, sizeof destination, "%s/main.sol", relocation);
    FILE *input = fopen(source, "rb"), *copy = fopen(destination, "wb");
    if (input == NULL || copy == NULL) ok = false;
    if (input != NULL && copy != NULL) {
        uint8_t buffer[256]; size_t count;
        while ((count = fread(buffer, 1, sizeof buffer, input)) != 0)
            if (fwrite(buffer, 1, count, copy) != count) ok = false;
    }
    if (input != NULL) fclose(input); if (copy != NULL) fclose(copy);
    if (ok) ok = build_named_root(relocation, "launch", &relocated, NULL,
            SOL_WASM_REPRESENTED_OK) && c2b2_output_identity(&relocated, &baseline, frozen)
        && invoke_named(&relocated.bytes, entry, frozen->value, frozen->code, frozen->site);
    sol_wasm_represented_output_free(&relocated); sol_wasm_represented_output_free(&output);
    sol_wasm_represented_output_free(&baseline); propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44c_qualified_result_determinism(size_t closure, size_t first, size_t last) {
    static const C2b2FrozenClosure closures[] = {
        {"p44c_qualified_success_false", {11,12,10,18,85,1439,0,0,0,0,54,32989,12509,20547,10307},
            {0x01,0x31,0xb2,0xf0,0xa4,0x39,0x93,0x61,0xd2,0xe1,0x18,0xad,0xfe,0x78,0x0d,0xdf,0xfd,0xd6,0xb6,0x28,0x86,0x85,0xba,0xbb,0xe4,0xfe,0x23,0x40,0x9d,0xaa,0x19,0xab},
            117, 0, 0, 14, 4},
        {"p44c_ensures_qualified_result", {16,64,61,104,255,7285,0,0,0,0,350,154395,74303,74303,61589},
            {0x04,0xf8,0x7f,0xd1,0x44,0xc7,0x9b,0x6d,0xa7,0x4c,0xa1,0x67,0x95,0xae,0x60,0x50,0x60,0xd7,0xe3,0x8a,0xd5,0x29,0x13,0xde,0xf3,0x7b,0x06,0x38,0xe9,0x28,0xf2,0xbf},
            628, 1, 42, 0, 0},
    };
    return closure < sizeof closures / sizeof *closures
        && c2b2_frozen_closure(&closures[closure], first, last);
}

static bool p44c_qualified_root_order_relocation(void) {
    static const SolWasmRepresentedUsage expected = {13,24,20,38,130,2510,0,0,0,0,124,61290,27178,32074,21834};
    static const uint8_t expected_hash[32] = {0xc2,0x09,0x46,0x20,0x1b,0xa4,0xed,0x32,0x3c,0x5e,0xeb,0xe5,0x75,0x84,0x39,0x7d,0x15,0xfa,0x4d,0xee,0x7c,0xae,0x27,0x59,0x04,0x09,0x66,0x72,0x89,0xb0,0x90,0x93};
    char directory[512], relocation[512], source[768], destination[768], entry[256];
    SolWasmRepresentedOutput forward, reverse, repeat, relocated;
    size_t forward_ids[2], reverse_ids[2], repeat_ids[2], relocated_ids[2]; uint8_t hash[32];
    sol_wasm_represented_output_init(&forward); sol_wasm_represented_output_init(&reverse);
    sol_wasm_represented_output_init(&repeat); sol_wasm_represented_output_init(&relocated);
    (void)snprintf(directory, sizeof directory, "%s/tests/conformance/p44c_qualified_roots",
        SOL_TEST_SOURCE_DIR);
    sol_wasm_represented_test_fail_allocation_after(0);
    bool ok = build_multiroot(directory, false, &forward, forward_ids, NULL,
            SOL_WASM_REPRESENTED_OK)
        && sol_wasm_represented_test_allocation_attempts() == 235;
    if (ok) { sha256(forward.bytes.bytes, forward.bytes.count, hash);
        ok = usage_equal(&forward.usage, &expected)
            && memcmp(hash, expected_hash, sizeof hash) == 0; }
    if (!ok && forward.bytes.bytes != NULL) {
        sha256(forward.bytes.bytes, forward.bytes.count, hash);
        fprintf(stderr, "C2b2 roots attempts=%zu usage={%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%llu,%llu,%zu,%zu,%zu,%zu,%zu} hash=",
            sol_wasm_represented_test_allocation_attempts(), forward.usage.functions,
            forward.usage.blocks, forward.usage.edges, forward.usage.values, forward.usage.locals,
            forward.usage.generated_nodes, forward.usage.table_elements,
            forward.usage.static_data_bytes, (unsigned long long)forward.usage.allocation_requests,
            (unsigned long long)forward.usage.allocation_bytes, forward.usage.provenance_records,
            forward.usage.work_bytes, forward.usage.scratch_bytes, forward.usage.owned_bytes,
            forward.usage.output_bytes);
        for (size_t i = 0; i < sizeof hash; ++i) fprintf(stderr, "%02x", hash[i]);
        fputc('\n', stderr);
    }
    if (ok) ok = build_multiroot(directory, true, &reverse, reverse_ids, NULL,
            SOL_WASM_REPRESENTED_OK)
        && build_multiroot(directory, false, &repeat, repeat_ids, NULL,
            SOL_WASM_REPRESENTED_OK)
        && forward_ids[0] == reverse_ids[0] && forward_ids[1] == reverse_ids[1]
        && forward_ids[0] == repeat_ids[0] && forward_ids[1] == repeat_ids[1]
        && reverse.bytes.count == forward.bytes.count && repeat.bytes.count == forward.bytes.count
        && memcmp(reverse.bytes.bytes, forward.bytes.bytes, forward.bytes.count) == 0
        && memcmp(repeat.bytes.bytes, forward.bytes.bytes, forward.bytes.count) == 0
        && usage_equal(&reverse.usage, &expected) && usage_equal(&repeat.usage, &expected)
        && entry_symbol(&forward.bytes, entry, sizeof entry)
        && invoke_named(&forward.bytes, entry, 42, 0, 0)
        && invoke_named(&reverse.bytes, entry, 42, 0, 0)
        && invoke_named(&repeat.bytes, entry, 42, 0, 0);
    (void)mkdir(SOL_TEST_BINARY_DIR, 0700);
    (void)snprintf(relocation, sizeof relocation, "%s/p44c_qualified_roots_relocated",
        SOL_TEST_BINARY_DIR);
    (void)mkdir(relocation, 0700);
    (void)snprintf(source, sizeof source, "%s/main.sol", directory);
    (void)snprintf(destination, sizeof destination, "%s/main.sol", relocation);
    FILE *input = fopen(source, "rb"), *copy = fopen(destination, "wb");
    if (input == NULL || copy == NULL) ok = false;
    if (input != NULL && copy != NULL) {
        uint8_t buffer[256]; size_t count;
        while ((count = fread(buffer, 1, sizeof buffer, input)) != 0)
            if (fwrite(buffer, 1, count, copy) != count) ok = false;
    }
    if (input != NULL) fclose(input); if (copy != NULL) fclose(copy);
    if (ok) ok = build_multiroot(relocation, false, &relocated, relocated_ids, NULL,
            SOL_WASM_REPRESENTED_OK)
        && relocated_ids[0] == forward_ids[0] && relocated_ids[1] == forward_ids[1]
        && relocated.bytes.count == forward.bytes.count
        && memcmp(relocated.bytes.bytes, forward.bytes.bytes, forward.bytes.count) == 0
        && usage_equal(&relocated.usage, &expected)
        && invoke_named(&relocated.bytes, entry, 42, 0, 0);
    sol_wasm_represented_output_free(&relocated); sol_wasm_represented_output_free(&repeat);
    sol_wasm_represented_output_free(&reverse); sol_wasm_represented_output_free(&forward);
    return ok;
}

static bool c2b2_supplemental_object_size(const PropagationPipeline *pipeline, size_t site,
    uint64_t *object_size) {
    if (pipeline == NULL || object_size == NULL || site >= pipeline->cleanup.supplemental_site_count)
        return false;
    const SolMirRuntimeCleanupSupplementalSite *supplemental =
        &pipeline->cleanup.supplemental_sites[site];
    if (supplemental->event >= pipeline->cleanup.event_count) return false;
    const SolMirRuntimeCleanupEvent *event = &pipeline->cleanup.events[supplemental->event];
    if (event->operation >= pipeline->concrete.materialization.instruction_count) return false;
    size_t found = 0; uint64_t size = 0;
    for (size_t i = 0; i < pipeline->concrete.operations.constructor_count; ++i) {
        const SolMirOperationConstructPlan *plan = &pipeline->concrete.operations.constructors[i];
        if (plan->instruction != event->operation) continue;
        if (plan->result_recipe >= pipeline->concrete.layout.type_count) return false;
        size = pipeline->concrete.layout.types[plan->result_recipe].object_size;
        ++found;
    }
    if (found != 1) return false;
    *object_size = size;
    return true;
}

static bool p44c_qualified_sequential_stress(size_t first, size_t last) {
    static const SolWasmRepresentedUsage expected = {11,148,110,188,279,8535,0,0,0,0,608,244622,127330,127330,98918};
    static const uint8_t expected_hash[32] = {0xfa,0x12,0xed,0x67,0x13,0x97,0x11,0x00,0xe1,0x01,0xc9,0x13,0xb6,0x78,0x76,0x95,0xfb,0x82,0x7f,0x68,0xbc,0x60,0x0e,0x0d,0x30,0xb6,0x78,0x0d,0x74,0x4c,0x5f,0x39};
    static const P44TraceSlot expected_trace[] = {
        {5,1,0},{6,257,0},{8,257,0},{16,257,0},{24,257,0},{32,257,0},{40,257,0},
        {48,257,0},{56,257,0},{64,1,0},{65,1,0},{77,1,0},{78,257,0},{80,257,0},
        {88,257,0},{96,257,0},{104,257,0},{112,257,0},{120,257,0},{128,257,0},
        {136,1,0},{137,1,0},{149,1,0},{150,257,0},{152,257,0},{160,257,0},
        {168,257,0},{176,257,0},{184,257,0},{192,257,0},{200,257,0},{208,1,0},
        {209,1,0},{221,1,0},{222,257,0},{224,257,0},{232,257,0},{240,257,0},
        {248,257,0},{256,257,0},{264,257,0},{272,257,0},{280,1,0},{281,1,0},
        {295,1,0},{296,1,0},{297,1,0},{298,1,0},{311,1,0},{312,1,0},{313,1,0},
        {314,1,0},{315,1,0},
    };
    char directory[512], relocation[512], source[768], destination[768], entry[256];
    PropagationPipeline pipeline; SolWasmRepresentedOutput traced;
    WasmInstance instance = {0}; P44TraceSlot actual[64], repeated_trace[64];
    size_t count = 0, repeated_count = 0; bool overflow = false, repeated_overflow = false;
    int64_t value = 0; int32_t code = 0, site = 0;
    SolWasmRepresentedTestControlTransitionStats transition_stats = {0};
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&traced);
    (void)snprintf(directory, sizeof directory,
        "%s/tests/conformance/p44c_qualified_sequential_stress", SOL_TEST_SOURCE_DIR);
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "first", false)
        && pipeline.diagnostics.count == 0
        && pipeline.concrete.operations.predicate_count == 32
        && pipeline.concrete.operations.predicate_body_count == 32;
    bool images[256] = {0}; size_t image_count = 0, success = 0, failure = 0,
        arithmetic_sites = 0;
    for (size_t body_id = 0; ok && body_id < 32; ++body_id) {
        const SolMirPredicateBody *body = &pipeline.concrete.operations.predicate_bodies[body_id];
        SolContractOutcomeKind expected_outcome = body_id % 2 == 0
            ? SOL_CONTRACT_OUTCOME_SUCCESS : SOL_CONTRACT_OUTCOME_FAILURE;
        size_t plans = 0;
        for (size_t p = 0; p < pipeline.concrete.operations.predicate_count; ++p) {
            const SolMirOperationPredicatePlan *plan = &pipeline.concrete.operations.predicates[p];
            plans += plan->kind == SOL_MIR_OPERATION_PREDICATE_CONTRACT
                && plan->body == body_id && plan->image == body->instance
                && plan->contract_outcome == expected_outcome;
        }
        ok = body->owner_kind == SOL_MIR_PREDICATE_OWNER_INSTANCE
            && body->phase == SOL_CONTRACT_ENSURES && body->outcome == expected_outcome
            && body->instance < sizeof images / sizeof *images && plans == 1;
        if (ok && !images[body->instance]) { images[body->instance] = true; ++image_count; }
        success += ok && expected_outcome == SOL_CONTRACT_OUTCOME_SUCCESS;
        failure += ok && expected_outcome == SOL_CONTRACT_OUTCOME_FAILURE;
    }
    for (size_t i = 0; ok && i < pipeline.conventions.failure_site_count; ++i) {
        const SolMirRuntimeFailureSite *candidate = &pipeline.conventions.failure_sites[i];
        arithmetic_sites += candidate->origin_kind
                == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC
            && candidate->owner < 32 && candidate->allowed_codes == UINT32_C(6);
    }
    ok = ok && image_count == 4 && success == 16 && failure == 16 && arithmetic_sites == 16
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &traced, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && sol_wasm_represented_test_qualified_certifications() == 4
        && entry_symbol(&traced.bytes, entry, sizeof entry)
        && wasm_instance_open(&traced.bytes, entry, &instance)
        && wasm_instance_observe(&instance, &value, &code, &site)
        && value == 42 && code == 0 && site == 0
        && wasm_instance_trace(&instance, actual, 64, &count, &overflow)
        && c1_trace_matches(&pipeline, expected_trace,
            sizeof expected_trace / sizeof *expected_trace, 0, actual, count, overflow)
        && wasm_instance_call(&instance, 42, 0, 0)
        && wasm_instance_trace(&instance, repeated_trace, 64, &repeated_count,
            &repeated_overflow)
        && repeated_count == count && repeated_overflow == overflow
        && memcmp(repeated_trace, actual, count * sizeof *actual) == 0;
    transition_stats = sol_wasm_represented_test_control_transition_stats();
    ok = ok && transition_stats.indexed_lookups == 2
        && transition_stats.indexed_matches == 2
        && transition_stats.legacy_fallback_scans == 0;
    wasm_instance_close(&instance); sol_wasm_represented_output_free(&traced);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false);
    propagation_pipeline_free(&pipeline);

    SolWasmRepresentedOutput forward, reverse, repeat, output, relocated;
    size_t forward_ids[2], reverse_ids[2], repeat_ids[2], relocated_ids[2]; uint8_t hash[32];
    sol_wasm_represented_output_init(&forward); sol_wasm_represented_output_init(&reverse);
    sol_wasm_represented_output_init(&repeat); sol_wasm_represented_output_init(&output);
    sol_wasm_represented_output_init(&relocated);
    sol_wasm_represented_test_fail_allocation_after(0);
    if (ok) ok = build_multiroot(directory, false, &forward, forward_ids, NULL,
            SOL_WASM_REPRESENTED_OK)
        && sol_wasm_represented_test_allocation_attempts() == 994
        && sol_wasm_represented_test_qualified_certifications() == 4;
    size_t attempts = sol_wasm_represented_test_allocation_attempts();
    if (ok) {
        sha256(forward.bytes.bytes, forward.bytes.count, hash);
        ok = usage_equal(&forward.usage, &expected)
            && memcmp(hash, expected_hash, sizeof hash) == 0
            && sol_wasm_represented_validate(&forward.bytes) == SOL_WASM_REPRESENTED_OK
            && entry_symbol(&forward.bytes, entry, sizeof entry)
            && invoke_named(&forward.bytes, entry, 42, 0, 0);
    }
    if (ok) ok = build_multiroot(directory, true, &reverse, reverse_ids, NULL,
            SOL_WASM_REPRESENTED_OK)
        && sol_wasm_represented_test_allocation_attempts() == attempts
        && sol_wasm_represented_test_qualified_certifications() == 4
        && build_multiroot(directory, false, &repeat, repeat_ids, NULL,
            SOL_WASM_REPRESENTED_OK)
        && sol_wasm_represented_test_allocation_attempts() == attempts
        && sol_wasm_represented_test_qualified_certifications() == 4
        && forward_ids[0] == reverse_ids[0] && forward_ids[1] == reverse_ids[1]
        && forward_ids[0] == repeat_ids[0] && forward_ids[1] == repeat_ids[1]
        && reverse.bytes.count == forward.bytes.count && repeat.bytes.count == forward.bytes.count
        && memcmp(reverse.bytes.bytes, forward.bytes.bytes, forward.bytes.count) == 0
        && memcmp(repeat.bytes.bytes, forward.bytes.bytes, forward.bytes.count) == 0
        && usage_equal(&reverse.usage, &expected) && usage_equal(&repeat.usage, &expected);

#define C2B2_STRESS_CAP(field, exact) do { \
    SolWasmRepresentedLimits cap = sol_wasm_represented_default_limits(); cap.field = (exact); \
    sol_wasm_represented_output_free(&output); sol_wasm_represented_output_init(&output); \
    ok = ok && build_multiroot(directory, false, &output, forward_ids, &cap, \
        SOL_WASM_REPRESENTED_OK) && usage_equal(&output.usage, &expected) \
        && output.bytes.count == forward.bytes.count \
        && memcmp(output.bytes.bytes, forward.bytes.bytes, forward.bytes.count) == 0; \
    sol_wasm_represented_output_free(&output); sol_wasm_represented_output_init(&output); \
    --cap.field; ok = ok && build_multiroot(directory, false, &output, forward_ids, &cap, \
        SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED) && output.bytes.bytes == NULL \
        && output.bytes.count == 0 && usage_zero(&output.usage); \
} while (0)
    if (ok) {
        C2B2_STRESS_CAP(max_functions, expected.functions);
        C2B2_STRESS_CAP(max_blocks, expected.blocks);
        C2B2_STRESS_CAP(max_edges, expected.edges);
        C2B2_STRESS_CAP(max_values, expected.values);
        C2B2_STRESS_CAP(max_locals, expected.locals);
        C2B2_STRESS_CAP(max_generated_nodes, expected.generated_nodes);
        C2B2_STRESS_CAP(max_provenance_records, expected.provenance_records);
        C2B2_STRESS_CAP(max_work_bytes, expected.work_bytes);
        C2B2_STRESS_CAP(max_scratch_bytes, expected.scratch_bytes);
        C2B2_STRESS_CAP(max_owned_bytes, expected.owned_bytes);
        C2B2_STRESS_CAP(max_output_bytes, expected.output_bytes);
    }
#undef C2B2_STRESS_CAP
    sol_wasm_represented_output_free(&output);
    for (size_t ordinal = first; ok && ordinal <= last && ordinal <= attempts; ++ordinal) {
        represented_fault(FAULT_STRESS, ordinal, ordinal);
        sol_wasm_represented_output_init(&output);
        ok = build_multiroot(directory, false, &output, forward_ids, NULL,
                SOL_WASM_REPRESENTED_ALLOCATION_FAILED)
            && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage);
        sol_wasm_represented_output_free(&output);
    }
    sol_wasm_represented_test_fail_allocation_after(0);
    sol_wasm_represented_output_init(&output);
    if (ok) ok = build_multiroot(directory, false, &output, forward_ids, NULL,
            SOL_WASM_REPRESENTED_OK)
        && sol_wasm_represented_test_allocation_attempts() == attempts
        && sol_wasm_represented_test_qualified_certifications() == 4
        && output.bytes.count == forward.bytes.count
        && memcmp(output.bytes.bytes, forward.bytes.bytes, forward.bytes.count) == 0
        && usage_equal(&output.usage, &expected);
    sol_wasm_represented_output_free(&output);

    (void)mkdir(SOL_TEST_BINARY_DIR, 0700);
    (void)snprintf(relocation, sizeof relocation, "%s/p44c_qualified_sequential_stress_relocated",
        SOL_TEST_BINARY_DIR);
    (void)mkdir(relocation, 0700);
    (void)snprintf(source, sizeof source, "%s/main.sol", directory);
    (void)snprintf(destination, sizeof destination, "%s/main.sol", relocation);
    FILE *input = fopen(source, "rb"), *copy = fopen(destination, "wb");
    if (input == NULL || copy == NULL) ok = false;
    if (input != NULL && copy != NULL) {
        uint8_t buffer[256]; size_t bytes;
        while ((bytes = fread(buffer, 1, sizeof buffer, input)) != 0)
            if (fwrite(buffer, 1, bytes, copy) != bytes) ok = false;
    }
    if (input != NULL) fclose(input); if (copy != NULL) fclose(copy);
    if (ok) ok = build_multiroot(relocation, false, &relocated, relocated_ids, NULL,
            SOL_WASM_REPRESENTED_OK)
        && sol_wasm_represented_test_qualified_certifications() == 4
        && relocated_ids[0] == forward_ids[0] && relocated_ids[1] == forward_ids[1]
        && relocated.bytes.count == forward.bytes.count
        && memcmp(relocated.bytes.bytes, forward.bytes.bytes, forward.bytes.count) == 0
        && usage_equal(&relocated.usage, &expected)
        && invoke_named(&relocated.bytes, entry, 42, 0, 0);
    if (!ok) fprintf(stderr, "qualified sequential stress failed attempts=%zu certs=%zu lookups=%zu indexed=%zu fallback=%zu\n",
        attempts, sol_wasm_represented_test_qualified_certifications(),
        transition_stats.indexed_lookups, transition_stats.indexed_matches,
        transition_stats.legacy_fallback_scans);
    sol_wasm_represented_output_free(&relocated); sol_wasm_represented_output_free(&repeat);
    sol_wasm_represented_output_free(&reverse); sol_wasm_represented_output_free(&forward);
    return ok;
}

static bool p44c_qualified_err_two_allocations(void) {
    static const P44TraceSlot success_trace[] = {
        {17,1,0},{27,1,0},{0,257,0},{28,257,0},{40,1,0},{41,1,0},{42,1,0},
        {43,1,0},{57,1,0},{58,1,0},{59,1,0},
    };
    static const P44TraceSlot inner_failure[] = {
        {2,13,9},{3,13,9},{4,13,9},{5,13,9},{6,13,9},{7,13,9},{8,525,9},
        {54,1,0},{55,1,0},{56,533,9},
    };
    static const P44TraceSlot outer_failure[] = {
        {9,13,8},{10,13,8},{11,13,8},{12,13,8},{13,13,8},{14,13,8},{15,13,8},
        {16,525,8},{54,1,0},{55,1,0},{56,533,8},
    };
    char directory[512], entry[256]; PropagationPipeline pipeline;
    SolWasmRepresentedOutput output; WasmInstance instance = {0};
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    (void)snprintf(directory, sizeof directory,
        "%s/tests/conformance/p44c_qualified_err_two_allocations", SOL_TEST_SOURCE_DIR);
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false)
        && pipeline.diagnostics.count == 0 && pipeline.concrete.operations.snapshot_count == 1
        && pipeline.cleanup.supplemental_site_count == 3
        && pipeline.conventions.call_count == 1
        && pipeline.conventions.calls[0].writebacks.count == 0;
    uint64_t inner_size = 0, outer_size = 0, dormant_size = 0;
    const SolMirRuntimeCleanupSupplementalSite *inner = ok ? &pipeline.cleanup.supplemental_sites[0] : NULL;
    const SolMirRuntimeCleanupSupplementalSite *outer = ok ? &pipeline.cleanup.supplemental_sites[1] : NULL;
    const SolMirRuntimeCleanupSupplementalSite *dormant = ok ? &pipeline.cleanup.supplemental_sites[2] : NULL;
    ok = ok && c2b2_supplemental_object_size(&pipeline, 0, &inner_size)
        && c2b2_supplemental_object_size(&pipeline, 1, &outer_size)
        && c2b2_supplemental_object_size(&pipeline, 2, &dormant_size)
        && inner_size == 4 && outer_size == 16 && dormant_size == 16
        && inner->source.file == 0 && inner->source.start == 270 && inner->source.end == 284
        && outer->source.file == 0 && outer->source.start == 266 && outer->source.end == 285
        && dormant->source.file == 0 && dormant->source.start == 295 && dormant->source.end == 304
        && inner->allowed_codes == UINT32_C(0x18) && outer->allowed_codes == UINT32_C(0x18)
        && dormant->allowed_codes == UINT32_C(0x18);
    SolWasmRepresentedLimits exact = sol_wasm_represented_default_limits();
    exact.max_allocation_requests = 2; exact.max_allocation_bytes = 20;
    P44TraceSlot actual[64], repeated[64]; size_t count = 0, repeated_count = 0;
    bool overflow = false, repeated_overflow = false;
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, &exact}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && sol_wasm_represented_test_qualified_certifications() == 1
        && entry_symbol(&output.bytes, entry, sizeof entry);
    ProvenanceRecord inner_record = {0}, outer_record = {0};
    const char *checked_symbol = NULL;
    if (ok) {
        size_t image = pipeline.cleanup.events[inner->event].owner;
        checked_symbol = p44_symbol_for_image(&pipeline, image);
        ok = checked_symbol != NULL && provenance_record(&output.bytes, 9, &inner_record)
            && provenance_record(&output.bytes, 8, &outer_record)
            && inner_record.tag == 4 && inner_record.kind == 0
            && inner_record.start == 270 && inner_record.end == 284 && inner_record.ordinal == 0
            && outer_record.tag == 4 && outer_record.kind == 0
            && outer_record.start == 266 && outer_record.end == 285 && outer_record.ordinal == 0
            && bytes_equal(inner_record.path, inner_record.path_count, "main.sol")
            && bytes_equal(outer_record.path, outer_record.path_count, "main.sol")
            && bytes_equal(inner_record.symbol, inner_record.symbol_count, checked_symbol)
            && bytes_equal(outer_record.symbol, outer_record.symbol_count, checked_symbol)
            && wasm_instance_open(&output.bytes, entry, &instance)
            && wasm_instance_call(&instance, 42, 0, 0)
            && wasm_instance_trace(&instance, actual, 64, &count, &overflow)
            && c1_trace_matches(&pipeline, success_trace,
                sizeof success_trace / sizeof *success_trace, 0, actual, count, overflow)
            && wasm_instance_call(&instance, 42, 0, 0)
            && wasm_instance_trace(&instance, repeated, 64, &repeated_count, &repeated_overflow)
            && repeated_count == count && repeated_overflow == overflow
            && memcmp(repeated, actual, count * sizeof *actual) == 0;
    }
    wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);

    const struct {
        bool quota_override, byte_cap, grow;
        uint32_t heap_base; int32_t code, record;
        const P44TraceSlot *trace; size_t trace_count;
    } cases[] = {
        {true,false,false,0,5,9,inner_failure,sizeof inner_failure / sizeof *inner_failure},
        {false,true,false,0,5,8,outer_failure,sizeof outer_failure / sizeof *outer_failure},
        {false,false,true,UINT32_C(65534),4,9,inner_failure,
            sizeof inner_failure / sizeof *inner_failure},
        {false,false,true,UINT32_C(65520),4,8,outer_failure,
            sizeof outer_failure / sizeof *outer_failure},
    };
    for (size_t item = 0; ok && item < sizeof cases / sizeof *cases; ++item) {
        SolWasmRepresentedLimits limits = sol_wasm_represented_default_limits();
        P44TraceSlot first[64], second[64]; size_t first_count = 0, second_count = 0;
        bool first_overflow = false, second_overflow = false; int64_t first_value = 1, second_value = 1;
        int32_t first_code = 0, first_site = 0, second_code = 0, second_site = 0;
        if (cases[item].quota_override)
            sol_wasm_represented_test_allocator_quota(0, UINT64_MAX);
        if (cases[item].byte_cap) limits.max_allocation_bytes = 4;
        if (cases[item].grow)
            sol_wasm_represented_test_allocator_memory(1, cases[item].heap_base);
        sol_wasm_represented_output_init(&output);
        ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, cases[item].byte_cap ? &limits : NULL}, &output,
                &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && sol_wasm_represented_test_qualified_certifications() == 1
            && entry_symbol(&output.bytes, entry, sizeof entry)
            && provenance_record(&output.bytes, (uint32_t)cases[item].record,
                cases[item].record == 9 ? &inner_record : &outer_record)
            && wasm_instance_open(&output.bytes, entry, &instance)
            && wasm_instance_observe(&instance, &first_value, &first_code, &first_site)
            && first_value == 0 && first_code == cases[item].code
            && first_site == cases[item].record
            && wasm_instance_trace(&instance, first, 64, &first_count, &first_overflow)
            && c1_trace_matches(&pipeline, cases[item].trace, cases[item].trace_count,
                (uint32_t)cases[item].record, first, first_count, first_overflow)
            && wasm_instance_observe(&instance, &second_value, &second_code, &second_site)
            && second_value == first_value && second_code == first_code && second_site == first_site
            && wasm_instance_trace(&instance, second, 64, &second_count, &second_overflow)
            && second_count == first_count && second_overflow == first_overflow
            && memcmp(second, first, first_count * sizeof *first) == 0;
        size_t snapshot_drops = 0, pending = 0;
        for (size_t i = 0; ok && i < first_count; ++i) {
            if (first[i].action >= pipeline.cleanup.action_count) { ok = false; break; }
            const SolMirRuntimeCleanupAction *action = &pipeline.cleanup.actions[first[i].action];
            if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_CHECK_CONTRACT
                || action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK) ok = false;
            snapshot_drops += action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT;
            pending += action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE
                && (first[i].disposition & SOL_WASM_REPRESENTED_TEST_P44_TRACE_PENDING) != 0;
        }
        ok = ok && snapshot_drops == 1 && pending == 1;
        wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
        sol_wasm_represented_test_allocator_quota(UINT64_MAX, UINT64_MAX);
        sol_wasm_represented_test_allocator_memory(0, 0);
    }
    sol_wasm_represented_test_allocator_quota(UINT64_MAX, UINT64_MAX);
    sol_wasm_represented_test_allocator_memory(0, 0);
    sol_wasm_represented_output_init(&output);
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && sol_wasm_represented_test_qualified_certifications() == 1
        && entry_symbol(&output.bytes, entry, sizeof entry)
        && invoke_named(&output.bytes, entry, 42, 0, 0);
    if (!ok) fprintf(stderr, "qualified Err two-allocation regression failed\n");
    sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false);
    propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44c_qualified_constructor_allocation(void) {
    static const P44TraceSlot failure_trace[] = {
        {0,13,6},{1,13,6},{2,13,6},{3,13,6},{4,13,6},{5,525,6},
        {20,1,0},{21,1,0},{22,533,6},
    };
    static const P44TraceSlot success_trace[] = {
        {6,1,0},{7,257,0},{9,1,0},{10,1,0},{11,1,0},{23,1,0},{24,1,0},{25,1,0},
    };
    char directory[512], entry[256]; PropagationPipeline pipeline;
    SolWasmRepresentedOutput output; ProvenanceRecord record;
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    (void)snprintf(directory, sizeof directory,
        "%s/tests/conformance/p44c_qualified_constructor_allocation", SOL_TEST_SOURCE_DIR);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false)
        && pipeline.concrete.operations.snapshot_count == 1
        && pipeline.cleanup.supplemental_site_count == 1;
    size_t construct = SOL_MIR_RUNTIME_NONE, constructs = 0, body_id = SOL_MIR_RUNTIME_NONE,
        bodies = 0, plan_id = SOL_MIR_RUNTIME_NONE, plans = 0;
    for (size_t i = 0; ok && i < pipeline.concrete.operations.constructor_count; ++i) {
        const SolMirOperationConstructPlan *candidate = &pipeline.concrete.operations.constructors[i];
        if (candidate->kind == SOL_MIR_OPERATION_CONSTRUCT_SUM
            && candidate->result_recipe < pipeline.concrete.representation.recipe_count
            && pipeline.concrete.representation.recipes[candidate->result_recipe].kind
                == SOL_MIR_RECIPE_RESULT) { construct = i; ++constructs; }
    }
    for (size_t i = 0; ok && i < pipeline.concrete.operations.predicate_body_count; ++i) {
        const SolMirPredicateBody *candidate = &pipeline.concrete.operations.predicate_bodies[i];
        if (candidate->phase == SOL_CONTRACT_ENSURES
            && candidate->outcome == SOL_CONTRACT_OUTCOME_SUCCESS) { body_id = i; ++bodies; }
    }
    for (size_t i = 0; ok && i < pipeline.concrete.operations.predicate_count; ++i) {
        const SolMirOperationPredicatePlan *candidate = &pipeline.concrete.operations.predicates[i];
        if (candidate->kind == SOL_MIR_OPERATION_PREDICATE_CONTRACT
            && candidate->body == body_id) { plan_id = i; ++plans; }
    }
    const SolMirOperationConstructPlan *constructor = constructs == 1
        ? &pipeline.concrete.operations.constructors[construct] : NULL;
    const SolMirRuntimeCleanupSupplementalSite *supplemental =
        pipeline.cleanup.supplemental_site_count == 1 ? &pipeline.cleanup.supplemental_sites[0] : NULL;
    const SolMirRuntimeCleanupEvent *allocation_event = supplemental != NULL
            && supplemental->event < pipeline.cleanup.event_count
        ? &pipeline.cleanup.events[supplemental->event] : NULL;
    const SolMirRuntimeCleanupTransition *allocation_failure = NULL;
    if (allocation_event != NULL) for (size_t i = 0; i < allocation_event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *candidate = &pipeline.cleanup.transitions[
            allocation_event->transitions.offset + i];
        if (candidate->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_SUPPLEMENTAL_P33)
            allocation_failure = candidate;
    }
    const SolMirOperationPredicatePlan *plan = plans == 1
        ? &pipeline.concrete.operations.predicates[plan_id] : NULL;
    const SolMirRuntimeLoweredImageTerminator *check_row = plan != NULL
            && plan->block < pipeline.lowered.image_terminator_count
        ? &pipeline.lowered.image_terminators[plan->block] : NULL;
    ok = ok && constructs == 1 && bodies == 1 && plans == 1 && constructor != NULL
        && constructor->instruction < pipeline.concrete.materialization.instruction_count
        && constructor->result_recipe < pipeline.concrete.layout.type_count
        && pipeline.concrete.layout.types[constructor->result_recipe].object_size == 16
        && supplemental != NULL && supplemental->source.file == 0
        && supplemental->source.start == 214 && supplemental->source.end == 223
        && supplemental->allowed_codes == UINT32_C(0x18)
        && allocation_event != NULL && allocation_failure != NULL
        && allocation_failure->continuation == SOL_MIR_RUNTIME_NONE
        && allocation_failure->actions.offset == 0 && allocation_failure->actions.count == 6
        && check_row != NULL && check_row->cleanup_event < pipeline.cleanup.event_count;
    bool forbidden[64] = {0};
    if (ok) {
        const SolMirRuntimeCleanupEvent *check = &pipeline.cleanup.events[check_row->cleanup_event];
        for (size_t i = 0; i < check->actions.count; ++i) {
            size_t action = check->actions.offset + i;
            if (action >= sizeof forbidden / sizeof *forbidden) ok = false;
            else forbidden[action] = true;
        }
        for (size_t event = 0; event < pipeline.cleanup.event_count; ++event) {
            const SolMirRuntimeCleanupEvent *candidate = &pipeline.cleanup.events[event];
            if (candidate->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION
                || candidate->owner != body_id) continue;
            for (size_t i = 0; i < candidate->actions.count; ++i) {
                size_t action = candidate->actions.offset + i;
                if (action >= sizeof forbidden / sizeof *forbidden) ok = false;
                else forbidden[action] = true;
            }
        }
    }
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    SolWasmRepresentedLimits exact = sol_wasm_represented_default_limits();
    exact.max_allocation_requests = 1; exact.max_allocation_bytes = 16;
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, &exact}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry);
    if (ok) {
        WasmInstance instance = {0}; P44TraceSlot actual[64]; size_t count = 0; bool overflow = false;
        ok = wasm_instance_open(&output.bytes, entry, &instance)
            && wasm_instance_call(&instance, 42, 0, 0)
            && wasm_instance_trace(&instance, actual, 64, &count, &overflow)
            && c1_trace_matches(&pipeline, success_trace,
                sizeof success_trace / sizeof *success_trace, 0, actual, count, overflow)
            && wasm_instance_call(&instance, 42, 0, 0);
        wasm_instance_close(&instance);
    }
    sol_wasm_represented_output_free(&output);
    const struct { uint64_t requests, bytes; bool override_requests, grow; int32_t code; } cases[] = {
        {1,16,true,false,5}, {1,15,false,false,5}, {UINT64_MAX,UINT64_MAX,false,true,4},
    };
    for (size_t item = 0; ok && item < sizeof cases / sizeof *cases; ++item) {
        SolWasmRepresentedLimits limits = sol_wasm_represented_default_limits();
        limits.max_allocation_requests = cases[item].requests == UINT64_MAX
            ? limits.max_allocation_requests : cases[item].requests;
        limits.max_allocation_bytes = cases[item].bytes == UINT64_MAX
            ? limits.max_allocation_bytes : cases[item].bytes;
        if (cases[item].override_requests)
            sol_wasm_represented_test_allocator_quota(0, UINT64_MAX);
        if (cases[item].grow) sol_wasm_represented_test_allocator_memory(1, UINT32_C(65528));
        sol_wasm_represented_output_init(&output); WasmInstance instance = {0};
        P44TraceSlot first[64], second[64]; size_t first_count = 0, second_count = 0;
        bool first_overflow = false, second_overflow = false;
        ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, cases[item].grow ? NULL : &limits}, &output, &pipeline.diagnostics)
                == SOL_WASM_REPRESENTED_OK
            && entry_symbol(&output.bytes, entry, sizeof entry)
            && provenance_record(&output.bytes, 6, &record)
            && record.tag == 4 && record.kind == 0
            && bytes_equal(record.path, record.path_count, "main.sol")
            && record.start == 214 && record.end == 223 && record.ordinal == 0
            && bytes_equal(record.symbol, record.symbol_count,
                p44_symbol_for_image(&pipeline, constructor->image))
            && wasm_instance_open(&output.bytes, entry, &instance)
            && wasm_instance_call(&instance, 0, cases[item].code, 6)
            && wasm_instance_trace(&instance, first, 64, &first_count, &first_overflow)
            && c1_trace_matches(&pipeline, failure_trace,
                sizeof failure_trace / sizeof *failure_trace, 6, first, first_count,
                first_overflow)
            && wasm_instance_call(&instance, 0, cases[item].code, 6)
            && wasm_instance_trace(&instance, second, 64, &second_count, &second_overflow)
            && second_count == first_count && second_overflow == first_overflow
            && memcmp(second, first, first_count * sizeof *first) == 0;
        size_t snapshot_drops = 0, pending = 0;
        for (size_t i = 0; ok && i < first_count; ++i) {
            if (first[i].action >= pipeline.cleanup.action_count
                || first[i].action >= sizeof forbidden / sizeof *forbidden
                || forbidden[first[i].action]) { ok = false; break; }
            const SolMirRuntimeCleanupAction *action = &pipeline.cleanup.actions[first[i].action];
            snapshot_drops += action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT;
            pending += action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE
                && (first[i].disposition & SOL_WASM_REPRESENTED_TEST_P44_TRACE_PENDING) != 0;
            if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_CHECK_CONTRACT
                || action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK) ok = false;
        }
        ok = ok && snapshot_drops == 1 && pending == 1;
        wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
        sol_wasm_represented_test_allocator_quota(UINT64_MAX, UINT64_MAX);
        sol_wasm_represented_test_allocator_memory(0, 0);
    }
    /* A separately emitted unconstrained module must still take the ordinary
     * constructor and qualified-check route after every failed-instance case. */
    sol_wasm_represented_test_allocator_quota(UINT64_MAX, UINT64_MAX);
    sol_wasm_represented_test_allocator_memory(0, 0);
    sol_wasm_represented_output_init(&output);
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry)
        && invoke_named(&output.bytes, entry, 42, 0, 0);
    sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false);
    propagation_pipeline_free(&pipeline);
    return ok;
}

/* Hostile C2b2 joins are predecessor corruptions unless explicitly described
 * as a coherent rebuild. Every row below is rejected by its independent owner
 * validator after resealing P3.6, then restoration reproduces the frozen
 * baseline. The separate stale-seal and snapshot-rebuild cases distinguish
 * authentication rejection from independently valid ownership. */
static bool p44c_qualified_owner_mutations(void) {
    char directory[512]; PropagationPipeline pipeline; SolWasmRepresentedOutput baseline;
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&baseline);
    (void)snprintf(directory, sizeof directory,
        "%s/tests/conformance/p44c_qualified_constructor_allocation", SOL_TEST_SOURCE_DIR);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &baseline, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
    uint8_t baseline_hash[32] = {0}; if (ok) sha256(baseline.bytes.bytes,
        baseline.bytes.count, baseline_hash);
    SolMirRuntimeConventions *conventions = &pipeline.conventions;
    SolMirConcreteProgram *concrete = &pipeline.concrete;
    SolMirOperations *operations = &concrete->operations;
    SolMirRepresentation *representation = &concrete->representation;
    SolMirLayout *layout = &concrete->layout;
    SolMirRuntimeCleanup *cleanup = &pipeline.cleanup;
    SolMirRuntimeLoweredProgram *lowered = &pipeline.lowered;
    SolMirRuntimeCall *call = conventions->call_count == 1 ? &conventions->calls[0] : NULL;
    SolMirRuntimeSignature *signature = call != NULL && call->signature < conventions->signature_count
        ? &conventions->signatures[call->signature] : NULL;
    size_t body_id = SOL_MIR_RUNTIME_NONE, plan_id = SOL_MIR_RUNTIME_NONE,
        success_input = SOL_MIR_RUNTIME_NONE, snapshot_input = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; ok && i < operations->predicate_body_count; ++i)
        if (operations->predicate_bodies[i].phase == SOL_CONTRACT_ENSURES
            && operations->predicate_bodies[i].outcome == SOL_CONTRACT_OUTCOME_SUCCESS)
            body_id = body_id == SOL_MIR_RUNTIME_NONE ? i : operations->predicate_body_count;
    for (size_t i = 0; ok && i < operations->predicate_count; ++i)
        if (operations->predicates[i].body == body_id)
            plan_id = plan_id == SOL_MIR_RUNTIME_NONE ? i : operations->predicate_count;
    SolMirPredicateBody *body = body_id < operations->predicate_body_count
        ? &operations->predicate_bodies[body_id] : NULL;
    if (body != NULL) for (size_t i = 0; i < body->inputs.count; ++i) {
        size_t id = body->inputs.offset + i;
        if (operations->predicate_inputs[id].kind == SOL_MIR_PREDICATE_INPUT_SUCCESS_RESULT)
            success_input = id;
        if (operations->predicate_inputs[id].kind == SOL_MIR_PREDICATE_INPUT_SNAPSHOT)
            snapshot_input = id;
    }
    SolMirOperationPredicatePlan *plan = plan_id < operations->predicate_count
        ? &operations->predicates[plan_id] : NULL;
    SolMirRuntimeLoweredImageTerminator *check = plan != NULL
            && plan->block < lowered->image_terminator_count
        ? &lowered->image_terminators[plan->block] : NULL;
    SolMirRuntimeLoweredSemanticPlan *semantic = check != NULL
            && check->plan < lowered->semantic_plan_count
        ? &lowered->semantic_plans[check->plan] : NULL;
    SolMirRuntimeCleanupEvent *event = check != NULL && check->cleanup_event < cleanup->event_count
        ? &cleanup->events[check->cleanup_event] : NULL;
    SolMirRuntimeCleanupTransition *violation = NULL;
    if (event != NULL) for (size_t i = 0; i < event->transitions.count; ++i) {
        SolMirRuntimeCleanupTransition *candidate = &cleanup->transitions[
            event->transitions.offset + i];
        if (candidate->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION)
            violation = candidate;
    }
    SolMirRuntimeFailureSite *site = violation != NULL
            && violation->failure_site < conventions->failure_site_count
        ? &conventions->failure_sites[violation->failure_site] : NULL;
    SolMirRecipeId result_recipe = signature == NULL ? SOL_MIR_RECIPE_NONE : signature->result;
    SolMirRecipe *result = result_recipe < representation->recipe_count
        ? &representation->recipes[result_recipe] : NULL;
    SolMirTypeLayout *result_layout = result_recipe < layout->type_count
        ? &layout->types[result_recipe] : NULL;
    SolMirRecipeVariant *success = result != NULL && result->variants.count == 2
        ? &representation->variants[result->variants.offset] : NULL;
    SolMirRecipeVariant *failure = success == NULL ? NULL
        : &representation->variants[result->variants.offset + 1];
    if (success != NULL && success->ordinal == 1) {
        SolMirRecipeVariant *temporary = success; success = failure; failure = temporary;
    }
    SolMirRecipeField *success_field = success != NULL && success->fields.count == 1
        ? &representation->fields[success->fields.offset] : NULL;
    SolMirRecipeField *failure_field = failure != NULL && failure->fields.count == 1
        ? &representation->fields[failure->fields.offset] : NULL;
    SolMirFieldLayout *success_field_layout = success_field != NULL
        ? &layout->fields[success->fields.offset] : NULL;
    SolMirFieldLayout *failure_field_layout = failure_field != NULL
        ? &layout->fields[failure->fields.offset] : NULL;
    SolMirVariantLayout *success_variant_layout = success != NULL
        ? &layout->variants[(size_t)(success - representation->variants)] : NULL;
    SolMirRecipe *error_recipe = failure_field != NULL
            && failure_field->type < representation->recipe_count
        ? &representation->recipes[failure_field->type] : NULL;
    SolIrDefinition *error_definition = error_recipe != NULL
            && error_recipe->concrete_definition < pipeline.ir.definition_count
        ? &pipeline.ir.definitions[error_recipe->concrete_definition] : NULL;
    SolMirOperationSnapshotPlan *snapshot = operations->snapshot_count == 1
        ? &operations->snapshots[0] : NULL;
    ok = ok && call != NULL && signature != NULL && body != NULL && plan != NULL && check != NULL
        && semantic != NULL && event != NULL && violation != NULL && site != NULL && result != NULL
        && result_layout != NULL && success != NULL && failure != NULL && success_field != NULL
        && failure_field != NULL && success_field_layout != NULL && failure_field_layout != NULL
        && success_variant_layout != NULL && error_recipe != NULL && error_definition != NULL
        && snapshot != NULL && success_input < operations->predicate_input_count
        && snapshot_input < operations->predicate_input_count;
#define C2B2_INVALID(layer, edit, restore) do { \
    SolWasmRepresentedOutput rejected, restored; sol_wasm_represented_output_init(&rejected); \
    sol_wasm_represented_output_init(&restored); edit; \
    lowered->authentication = sol_mir_runtime_lowered_program_test_seal(lowered); \
    bool rejected_ok = !(layer) && !sol_mir_runtime_lowered_program_validate(lowered, NULL) \
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){lowered, directory, NULL}, \
            &rejected, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE \
        && rejected.bytes.bytes == NULL && rejected.bytes.count == 0 && usage_zero(&rejected.usage); \
    restore; lowered->authentication = sol_mir_runtime_lowered_program_test_seal(lowered); \
    bool restored_ok = c2b1_predecessors_valid(&pipeline) \
        && sol_mir_runtime_lowered_program_validate(lowered, NULL) \
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){lowered, directory, NULL}, \
            &restored, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK \
        && restored.bytes.count == baseline.bytes.count \
        && memcmp(restored.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0 \
        && usage_equal(&restored.usage, &baseline.usage); \
    if (restored_ok) { uint8_t restored_hash[32]; sha256(restored.bytes.bytes, \
        restored.bytes.count, restored_hash); restored_ok = memcmp(restored_hash, baseline_hash, \
        sizeof baseline_hash) == 0; } \
    if (!rejected_ok || !restored_ok) fprintf(stderr, \
        "C2b2 mutation line %d rejected=%d restored=%d\n", __LINE__, rejected_ok, restored_ok); \
    ok = ok && rejected_ok && restored_ok; sol_wasm_represented_output_free(&restored); \
    sol_wasm_represented_output_free(&rejected); \
} while (0)
    if (ok) {
        SolMirRuntimeResultClass result_class = signature->result_class;
        C2B2_INVALID(sol_mir_runtime_conventions_validate(conventions, NULL),
            signature->result_class = SOL_MIR_RUNTIME_RESULT_UNIT,
            signature->result_class = result_class);
        SolMirRecipeId signature_result = signature->result;
        C2B2_INVALID(sol_mir_runtime_conventions_validate(conventions, NULL),
            signature->result = success_field->type, signature->result = signature_result);
        SolIrCallKind call_kind = call->call_kind;
        C2B2_INVALID(sol_mir_runtime_conventions_validate(conventions, NULL),
            call->call_kind = SOL_IR_CALL_CALLBACK, call->call_kind = call_kind);
        SolMirRuntimeCallTargetKind target_kind = call->target_kind;
        C2B2_INVALID(sol_mir_runtime_conventions_validate(conventions, NULL),
            call->target_kind = SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE,
            call->target_kind = target_kind);
        SolMirRuntimeValueRef call_result = call->result;
        C2B2_INVALID(sol_mir_runtime_conventions_validate(conventions, NULL),
            call->result = ((SolMirRuntimeValueRef){SOL_MIR_RUNTIME_VALUE_NONE,
                SOL_MIR_RUNTIME_NONE}),
            call->result = call_result);

        SolContractClauseKind obligation_kind = pipeline.ir.obligations[
            concrete->materialization.contexts[body->context].obligation].kind;
        C2B2_INVALID(sol_mir_concrete_program_validate(concrete, NULL),
            pipeline.ir.obligations[concrete->materialization.contexts[body->context].obligation].kind
                = SOL_CONTRACT_REQUIRES,
            pipeline.ir.obligations[concrete->materialization.contexts[body->context].obligation].kind
                = obligation_kind);
        SolContractOutcomeKind obligation_outcome = pipeline.ir.obligations[
            concrete->materialization.contexts[body->context].obligation].outcome;
        C2B2_INVALID(sol_mir_concrete_program_validate(concrete, NULL),
            pipeline.ir.obligations[concrete->materialization.contexts[body->context].obligation].outcome
                = SOL_CONTRACT_OUTCOME_FAILURE,
            pipeline.ir.obligations[concrete->materialization.contexts[body->context].obligation].outcome
                = obligation_outcome);
        SolMirPlanContextId body_context = body->context;
        C2B2_INVALID(sol_mir_operations_validate(operations, NULL),
            body->context = SOL_MIR_RUNTIME_NONE, body->context = body_context);
        SolContractClauseKind body_phase = body->phase;
        C2B2_INVALID(sol_mir_operations_validate(operations, NULL),
            body->phase = SOL_CONTRACT_REQUIRES, body->phase = body_phase);
        SolContractOutcomeKind body_outcome = body->outcome;
        C2B2_INVALID(sol_mir_operations_validate(operations, NULL),
            body->outcome = SOL_CONTRACT_OUTCOME_FAILURE, body->outcome = body_outcome);
        SolMirPredicateInput *input = &operations->predicate_inputs[success_input];
        SolMirPredicateInputKind input_kind = input->kind;
        C2B2_INVALID(sol_mir_operations_validate(operations, NULL),
            input->kind = SOL_MIR_PREDICATE_INPUT_COMPLETE_RESULT, input->kind = input_kind);
        size_t input_ordinal = input->ordinal;
        C2B2_INVALID(sol_mir_operations_validate(operations, NULL),
            input->ordinal = 1, input->ordinal = input_ordinal);
        SolMirRecipeId input_recipe = input->recipe;
        C2B2_INVALID(sol_mir_operations_validate(operations, NULL),
            input->recipe = result_recipe, input->recipe = input_recipe);
        SolMirMaterializedValueId plan_result = plan->result;
        C2B2_INVALID(sol_mir_operations_validate(operations, NULL),
            plan->result = SOL_MIR_MATERIALIZED_NONE, plan->result = plan_result);
        SolMirRecipeId plan_recipe = plan->result_recipe;
        C2B2_INVALID(sol_mir_operations_validate(operations, NULL),
            plan->result_recipe = success_field->type, plan->result_recipe = plan_recipe);
        SolContractOutcomeKind plan_outcome = plan->contract_outcome;
        C2B2_INVALID(sol_mir_operations_validate(operations, NULL),
            plan->contract_outcome = SOL_CONTRACT_OUTCOME_FAILURE,
            plan->contract_outcome = plan_outcome);

        SolContractOutcomeKind lowered_outcome = lowered->predicate_bodies[body_id].outcome;
        C2B2_INVALID(sol_mir_runtime_lowered_program_validate(lowered, NULL),
            lowered->predicate_bodies[body_id].outcome = SOL_CONTRACT_OUTCOME_FAILURE,
            lowered->predicate_bodies[body_id].outcome = lowered_outcome);
        uint32_t semantic_facilities = semantic->facilities;
        C2B2_INVALID(sol_mir_runtime_lowered_program_validate(lowered, NULL),
            semantic->facilities = SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE,
            semantic->facilities = semantic_facilities);
        SolMirMaterializedProducerKind producer_kind = semantic->producer_kind;
        C2B2_INVALID(sol_mir_runtime_lowered_program_validate(lowered, NULL),
            semantic->producer_kind = SOL_MIR_MATERIALIZED_PRODUCER_TERMINATOR,
            semantic->producer_kind = producer_kind);

        SolMirPlanSlice variants = result->variants;
        C2B2_INVALID(sol_mir_representation_validate(representation, NULL),
            result->variants.count = 1, result->variants = variants);
        size_t ordinal = success->ordinal;
        C2B2_INVALID(sol_mir_representation_validate(representation, NULL),
            success->ordinal = failure->ordinal, success->ordinal = ordinal);
        size_t semantic_tag = success->semantic_tag;
        C2B2_INVALID(sol_mir_representation_validate(representation, NULL),
            success->semantic_tag = failure->semantic_tag, success->semantic_tag = semantic_tag);
        SolMirRecipeId field_recipe = success_field->type;
        C2B2_INVALID(sol_mir_representation_validate(representation, NULL),
            success_field->type = failure_field->type, success_field->type = field_recipe);
        field_recipe = failure_field->type;
        C2B2_INVALID(sol_mir_representation_validate(representation, NULL),
            failure_field->type = success_field->type, failure_field->type = field_recipe);
        uint64_t object_size = result_layout->object_size;
        C2B2_INVALID(sol_mir_layout_validate(layout, NULL),
            result_layout->object_size = result_layout->payload_offset,
            result_layout->object_size = object_size);
        uint64_t offset = success_field_layout->offset;
        C2B2_INVALID(sol_mir_layout_validate(layout, NULL),
            success_field_layout->offset = result_layout->object_size,
            success_field_layout->offset = offset);
        uint64_t field_size = success_field_layout->size;
        C2B2_INVALID(sol_mir_layout_validate(layout, NULL),
            success_field_layout->size = 4, success_field_layout->size = field_size);
        uint64_t alignment = success_field_layout->alignment;
        C2B2_INVALID(sol_mir_layout_validate(layout, NULL),
            success_field_layout->alignment = 4, success_field_layout->alignment = alignment);
        uint32_t variant_tag = success_variant_layout->tag;
        C2B2_INVALID(sol_mir_layout_validate(layout, NULL),
            success_variant_layout->tag = (uint32_t)failure->semantic_tag,
            success_variant_layout->tag = variant_tag);
        SolMirRecipeKind error_kind = error_recipe->kind;
        C2B2_INVALID(sol_mir_representation_validate(representation, NULL),
            error_recipe->kind = SOL_MIR_RECIPE_RESULT, error_recipe->kind = error_kind);
        bool error_open = error_definition->open;
        C2B2_INVALID(sol_mir_concrete_program_validate(concrete, NULL),
            error_definition->open = true, error_definition->open = error_open);

        uint32_t site_mask = site->allowed_codes;
        C2B2_INVALID(sol_mir_runtime_conventions_validate(conventions, NULL),
            site->allowed_codes = UINT32_C(0x1000), site->allowed_codes = site_mask);
        size_t site_owner = site->owner;
        C2B2_INVALID(sol_mir_runtime_conventions_validate(conventions, NULL),
            site->owner = SOL_MIR_RUNTIME_NONE, site->owner = site_owner);
        SolMirRuntimeCleanupEdgeRole role = violation->edge_role;
        C2B2_INVALID(sol_mir_runtime_cleanup_validate(cleanup, NULL),
            violation->edge_role = SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO,
            violation->edge_role = role);
        size_t continuation = violation->continuation;
        C2B2_INVALID(sol_mir_runtime_cleanup_validate(cleanup, NULL),
            violation->continuation = 0, violation->continuation = continuation);
        SolMirRuntimeSlice actions = violation->actions;
        C2B2_INVALID(sol_mir_runtime_cleanup_validate(cleanup, NULL),
            violation->actions.count = 0, violation->actions = actions);
        size_t snapshot_slot = snapshot->slot;
        C2B2_INVALID(sol_mir_operations_validate(operations, NULL),
            snapshot->slot = 1, snapshot->slot = snapshot_slot);
        size_t snapshot_provenance = snapshot->provenance;
        C2B2_INVALID(sol_mir_operations_validate(operations, NULL),
            snapshot->provenance = operations->provenance_count,
            snapshot->provenance = snapshot_provenance);
        SolMirPredicateInput *old_input = &operations->predicate_inputs[snapshot_input];
        input_kind = old_input->kind;
        C2B2_INVALID(sol_mir_operations_validate(operations, NULL),
            old_input->kind = SOL_MIR_PREDICATE_INPUT_PARAMETER, old_input->kind = input_kind);
    }
#undef C2B2_INVALID
    /* Authentication-only rejection: all predecessors remain independently
     * valid; clearing/resealing the outer owner restores the unchanged bytes. */
    if (ok) {
        SolWasmRepresentedOutput stale, resealed; sol_wasm_represented_output_init(&stale);
        sol_wasm_represented_output_init(&resealed);
        uint64_t authentication = lowered->authentication; lowered->authentication ^= UINT64_C(1);
        bool stale_ok = c2b1_predecessors_valid(&pipeline)
            && !sol_mir_runtime_lowered_program_validate(lowered, NULL)
            && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){lowered, directory, NULL},
                &stale, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE
            && stale.bytes.bytes == NULL && usage_zero(&stale.usage);
        lowered->authentication = sol_mir_runtime_lowered_program_test_seal(lowered);
        bool resealed_ok = lowered->authentication == authentication
            && sol_mir_runtime_lowered_program_validate(lowered, NULL)
            && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){lowered, directory, NULL},
                &resealed, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && resealed.bytes.count == baseline.bytes.count
            && memcmp(resealed.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
            && usage_equal(&resealed.usage, &baseline.usage);
        ok = stale_ok && resealed_ok;
        sol_wasm_represented_output_free(&resealed); sol_wasm_represented_output_free(&stale);
    }
    sol_wasm_represented_output_free(&baseline); propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44c_qualified_control_route_mutations(void) {
    char directory[512]; PropagationPipeline pipeline;
    propagation_pipeline_init(&pipeline);
    (void)snprintf(directory, sizeof directory,
        "%s/tests/conformance/p44c_qualified_failure_eval", SOL_TEST_SOURCE_DIR);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false)
        && pipeline.diagnostics.count == 0;
    SolMirOperations *operations = &pipeline.concrete.operations;
    SolMirMaterialization *m = &pipeline.concrete.materialization;
    size_t body = SOL_MIR_RUNTIME_NONE, image_id = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; ok && i < operations->predicate_count; ++i) {
        const SolMirOperationPredicatePlan *plan = &operations->predicates[i];
        if (plan->kind != SOL_MIR_OPERATION_PREDICATE_CONTRACT
            || plan->contract_phase != SOL_CONTRACT_ENSURES
            || plan->contract_outcome != SOL_CONTRACT_OUTCOME_FAILURE) continue;
        if (body != SOL_MIR_RUNTIME_NONE) { ok = false; break; }
        body = plan->body; image_id = plan->image;
    }
    size_t site = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; ok && i < pipeline.conventions.failure_site_count; ++i) {
        const SolMirRuntimeFailureSite *candidate = &pipeline.conventions.failure_sites[i];
        if (candidate->origin_kind != SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT
            || candidate->owner != body
            || candidate->allowed_codes != (UINT32_C(1)
                << (SOL_MIR_RUNTIME_FAILURE_ENSURE_VIOLATION - 1))) continue;
        if (site != SOL_MIR_RUNTIME_NONE) { ok = false; break; }
        site = i;
    }
    SolMirMaterializedImage *image = image_id < m->image_count ? &m->images[image_id] : NULL;
    size_t branch = SOL_MIR_RUNTIME_NONE, returning = SOL_MIR_RUNTIME_NONE,
        unrelated = SOL_MIR_RUNTIME_NONE;
    SolWasmRepresentedBuildRequest request = {&pipeline.lowered, directory, NULL};
    if (ok && image != NULL && site < pipeline.conventions.failure_site_count)
        for (size_t i = 0; i < image->blocks.count; ++i) {
            size_t block = image->blocks.offset + i;
            if (block >= m->block_count || block >= pipeline.lowered.image_terminator_count)
                { ok = false; break; }
            SolMirMaterializedTerminator *term = &m->blocks[block].terminator;
            SolMirRuntimeLoweredImageTerminator *row = &pipeline.lowered.image_terminators[block];
            if (row->cleanup_event >= pipeline.cleanup.event_count) { ok = false; break; }
            SolMirRuntimeCleanupEvent *event = &pipeline.cleanup.events[row->cleanup_event];
            size_t transition = 0; SolMirRuntimeSlice actions = {0};
            if ((term->kind == SOL_MIR_TERM_GOTO || term->kind == SOL_MIR_TERM_BREAK
                    || term->kind == SOL_MIR_TERM_CONTINUE)
                && term->edge < m->edge_count) {
                if (m->edges[term->edge].block != image->contract_epilogue
                    && event->inherited_failure_site == SOL_MIR_RUNTIME_NONE
                    && sol_wasm_represented_test_control_transition(&request, block,
                        SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO, &transition, &actions)) unrelated = block;
            } else if (term->kind == SOL_MIR_TERM_BRANCH
                && event->inherited_failure_site == SOL_MIR_RUNTIME_NONE
                && sol_wasm_represented_test_control_transition(&request, block,
                    SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_TRUE, &transition, &actions)) branch = block;
            else if (term->kind == SOL_MIR_TERM_RETURN
                && event->inherited_failure_site == SOL_MIR_RUNTIME_NONE
                && sol_wasm_represented_test_control_transition(&request, block,
                    SOL_MIR_RUNTIME_CLEANUP_EDGE_RETURN, &transition, &actions)) returning = block;
        }
    ok = ok && branch < m->block_count && returning < m->block_count
        && unrelated < m->block_count;
#define C2B2_ROUTE_REJECT(test_block, test_role) do { \
    SolMirRuntimeLoweredImageTerminator *test_row = &pipeline.lowered.image_terminators[test_block]; \
    SolMirRuntimeCleanupEvent *test_event = &pipeline.cleanup.events[test_row->cleanup_event]; \
    size_t saved_site = test_event->inherited_failure_site; \
    SolWasmRepresentedTestControlTransitionStats before = \
        sol_wasm_represented_test_control_transition_stats(); \
    test_event->inherited_failure_site = site; \
    bool rejected = !sol_wasm_represented_test_control_transition(&request, test_block, \
        test_role, &(size_t){0}, &(SolMirRuntimeSlice){0}); \
    SolWasmRepresentedTestControlTransitionStats after = \
        sol_wasm_represented_test_control_transition_stats(); \
    test_event->inherited_failure_site = saved_site; \
    bool restored = sol_wasm_represented_test_control_transition(&request, test_block, \
        test_role, &(size_t){0}, &(SolMirRuntimeSlice){0}); \
    ok = ok && rejected && restored && after.indexed_lookups == before.indexed_lookups \
        && after.indexed_matches == before.indexed_matches \
        && after.legacy_fallback_scans == before.legacy_fallback_scans; \
} while (0)
    if (ok) {
        C2B2_ROUTE_REJECT(branch, SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_TRUE);
        C2B2_ROUTE_REJECT(returning, SOL_MIR_RUNTIME_CLEANUP_EDGE_RETURN);
        C2B2_ROUTE_REJECT(unrelated, SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO);
    }
#undef C2B2_ROUTE_REJECT
    propagation_pipeline_free(&pipeline); propagation_pipeline_init(&pipeline);
    (void)snprintf(directory, sizeof directory,
        "%s/tests/conformance/p44c_qualified_sequential_stress", SOL_TEST_SOURCE_DIR);
    size_t ingress = SOL_MIR_RUNTIME_NONE;
    ok = ok && propagation_pipeline_build_named(&pipeline, directory, "first", false)
        && pipeline.diagnostics.count == 0;
    request = (SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL};
    m = &pipeline.concrete.materialization;
    for (size_t block = 0; ok && block < m->block_count; ++block) {
        SolMirMaterializedTerminator *term = &m->blocks[block].terminator;
        SolMirRuntimeLoweredImageTerminator *row = block < pipeline.lowered.image_terminator_count
            ? &pipeline.lowered.image_terminators[block] : NULL;
        if (row == NULL || row->image >= m->image_count
            || row->cleanup_event >= pipeline.cleanup.event_count
            || (term->kind != SOL_MIR_TERM_GOTO && term->kind != SOL_MIR_TERM_BREAK
                && term->kind != SOL_MIR_TERM_CONTINUE)
            || term->edge >= m->edge_count
            || m->edges[term->edge].block != m->images[row->image].contract_epilogue)
            continue;
        SolMirRuntimeCleanupEvent *event = &pipeline.cleanup.events[row->cleanup_event];
        if (event->inherited_failure_site >= pipeline.conventions.failure_site_count
            || pipeline.conventions.failure_sites[event->inherited_failure_site].origin_kind
                != SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT)
            continue;
        ingress = block; break;
    }
    SolWasmRepresentedTestControlTransitionStats before =
        sol_wasm_represented_test_control_transition_stats();
    ok = ok && ingress < m->block_count
        && sol_wasm_represented_test_control_transition(&request, ingress,
            SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO, &(size_t){0}, &(SolMirRuntimeSlice){0});
    SolWasmRepresentedTestControlTransitionStats after =
        sol_wasm_represented_test_control_transition_stats();
    ok = ok && after.indexed_lookups == before.indexed_lookups + 1
        && after.indexed_matches == before.indexed_matches + 1
        && after.legacy_fallback_scans == before.legacy_fallback_scans;
    if (!ok) fprintf(stderr,
        "qualified control routes failed site=%zu image=%zu ingress=%zu branch=%zu return=%zu unrelated=%zu\n",
        site, image_id, ingress, branch, returning, unrelated);
    propagation_pipeline_free(&pipeline); return ok;
}

typedef struct {
    const char *leaf;
    int64_t value;
    int32_t code, record;
    const P44TraceSlot *slots;
    size_t slot_count;
} C2aTraceCase;

static bool c2a_run_trace_case(const C2aTraceCase *test) {
    char directory[512], entry[256]; PropagationPipeline pipeline;
    SolWasmRepresentedOutput output; WasmInstance instance = {0}; P44TraceSlot actual[64];
    size_t count = 0; bool overflow = false; int64_t value = 0; int32_t code = 0, site = 0;
    if (test == NULL || snprintf(directory, sizeof directory, "%s/tests/conformance/%s",
            SOL_TEST_SOURCE_DIR, test->leaf) < 0) return false;
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry)
        && wasm_instance_open(&output.bytes, entry, &instance)
        && wasm_instance_observe(&instance, &value, &code, &site)
        && value == test->value && code == test->code && site == test->record
        && wasm_instance_trace(&instance, actual, sizeof actual / sizeof *actual, &count, &overflow)
        && c1_trace_matches(&pipeline, test->slots, test->slot_count, (uint32_t)test->record,
            actual, count, overflow);
    bool check_origin = strcmp(test->leaf, "p44c_ensures_old_false") == 0
        || strcmp(test->leaf, "p44c_ensures_old_predicate_failure") == 0
        || strcmp(test->leaf, "p44c_ensures_old_body_failure") == 0
        || strcmp(test->leaf, "p44c_ensures_old_panic") == 0;
    SolMirRuntimeFailureOriginKind origin = strcmp(test->leaf,
            "p44c_ensures_old_false") == 0 ? SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT
        : strcmp(test->leaf, "p44c_ensures_old_predicate_failure") == 0
        ? SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC
        : strcmp(test->leaf, "p44c_ensures_old_panic") == 0
        ? SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_PANIC
        : SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_ARITHMETIC;
    if (ok && check_origin) {
        ProvenanceRecord record; size_t matches = 0, matched_site = SOL_MIR_RUNTIME_NONE;
        ok = test->code > 0 && test->record > 0
            && provenance_record(&output.bytes, (uint32_t)test->record, &record)
            && record.tag == 3 && record.kind == (uint8_t)origin
            && bytes_equal(record.path, record.path_count, "main.sol");
        for (size_t i = 0; ok && i < pipeline.conventions.failure_site_count; ++i) {
            const SolMirRuntimeFailureSite *candidate = &pipeline.conventions.failure_sites[i];
            if (candidate->origin_kind == origin && candidate->source.file == 0
                && candidate->source.start == record.start && candidate->source.end == record.end
                && (candidate->allowed_codes & (UINT32_C(1) << ((unsigned)test->code - 1u))) != 0)
                { ++matches; matched_site = i; }
        }
        ok = ok && matches == 1;
        if (ok && origin == SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_PANIC) {
            size_t events = 0, drops = 0;
            for (size_t i = 0; i < pipeline.cleanup.event_count; ++i) {
                const SolMirRuntimeCleanupEvent *event = &pipeline.cleanup.events[i];
                if (event->inherited_failure_site == matched_site
                    && event->producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_PANIC
                    && event->captures_failure_detail
                    && event->capture_detail_kind == SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT)
                    ++events;
            }
            for (size_t i = 0; i < test->slot_count; ++i)
                if (test->slots[i].action < pipeline.cleanup.action_count
                    && pipeline.cleanup.actions[test->slots[i].action].kind
                        == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT) ++drops;
            ok = events == 1 && drops == 1;
        }
    }
    if (!ok) {
        fprintf(stderr, "trace detail %s value=%lld code=%d site=%d count=%zu overflow=%d\n",
            test->leaf, (long long)value, code, site, count, overflow);
        for (size_t i = 0; i < count; ++i) fprintf(stderr, " {%u,%u,%u}", actual[i].action,
            actual[i].disposition, actual[i].record);
        fputc('\n', stderr);
    }
    wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false); propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44c_exact_ensures_traces(void) {
    static const P44TraceSlot success[] = {{0,1,0}, {1,257,0}, {3,1,0}};
    static const P44TraceSlot violation[] = {{0,1,0}, {2,5,3}, {4,1,0}, {5,517,3}};
    static const P44TraceSlot bool_success[] = {
        {0,1,0}, {1,257,0}, {3,1,0}, {11,1,0}, {12,1,0}, {13,1,0},
    };
    static const P44TraceSlot computed_violation[] = {
        {4,1,0}, {5,1,0}, {7,5,3}, {9,1,0}, {10,517,3},
    };
    static const P44TraceSlot predicate_failure[] = {{0,1,0}, {6,1,0}, {7,533,3}};
    static const P44TraceSlot ordered_violation[] = {{0,1,0}, {2,5,3}, {5,1,0}, {6,517,3}};
    static const P44TraceSlot requires_success[] = {{0,257,0}, {4,1,0}, {2,257,0}, {9,1,0}};
    static const P44TraceSlot body_failure[] = {{0,13,4}, {1,13,4}, {2,525,4}};
    static const P44TraceSlot callee_failure[] = {
        {0,1,0}, {1,1,0}, {2,517,4}, {5,1,0}, {6,1,0}, {7,533,4},
    };
    static const C2aTraceCase cases[] = {
        {"p44c_ensures_true", 42, 0, 0, success, sizeof success / sizeof *success},
        {"p44c_ensures_false", 0, 14, 3, violation, sizeof violation / sizeof *violation},
        {"p44c_ensures_bool", 1, 0, 0, bool_success, sizeof bool_success / sizeof *bool_success},
        {"p44c_ensures_computed_false", 0, 14, 3, computed_violation,
            sizeof computed_violation / sizeof *computed_violation},
        {"p44c_ensures_divzero", 0, 3, 3, predicate_failure,
            sizeof predicate_failure / sizeof *predicate_failure},
        {"p44c_ensures_overflow", 0, 2, 3, predicate_failure,
            sizeof predicate_failure / sizeof *predicate_failure},
        {"p44c_ensures_order", 0, 14, 3, ordered_violation,
            sizeof ordered_violation / sizeof *ordered_violation},
        {"p44c_ensures_requires", 42, 0, 0, requires_success,
            sizeof requires_success / sizeof *requires_success},
        {"p44c_ensures_body_failure", 0, 3, 4, body_failure,
            sizeof body_failure / sizeof *body_failure},
        {"p44c_ensures_callee_failure", 0, 12, 4, callee_failure,
            sizeof callee_failure / sizeof *callee_failure},
        {"p44c_ensures_constant_unit", 0, 0, 0, success, sizeof success / sizeof *success},
    };
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof cases / sizeof *cases; ++i)
        ok = c2a_run_trace_case(&cases[i]);
    return ok && c1_trace_excludes(success, sizeof success / sizeof *success,
        violation + 1, 1) && c1_trace_excludes(ordered_violation, 2, predicate_failure + 1, 1);
}

static bool p44c_old_exact_traces(void) {
    static const P44TraceSlot unlike[] = {
        {5,1,0}, {6,257,0}, {8,1,0}, {9,1,0}, {10,1,0}, {22,1,0}, {23,1,0},
    };
    static const P44TraceSlot bool_old[] = {
        {0,1,0}, {1,257,0}, {3,1,0}, {4,1,0}, {5,1,0}, {17,1,0}, {18,1,0}, {19,1,0},
    };
    static const P44TraceSlot two[] = {
        {7,1,0}, {8,257,0}, {10,1,0}, {11,1,0}, {12,1,0}, {13,1,0}, {14,1,0},
        {30,1,0}, {31,1,0},
    };
    static const P44TraceSlot repeated[] = {
        {0,1,0}, {1,257,0}, {3,1,0}, {4,1,0}, {5,1,0}, {6,1,0}, {7,1,0},
        {23,1,0}, {24,1,0},
    };
    static const P44TraceSlot requires[] = {
        {0,257,0}, {9,1,0}, {2,257,0}, {16,1,0}, {17,1,0}, {18,1,0}, {30,1,0}, {31,1,0},
    };
    static const P44TraceSlot violation[] = {
        {5,1,0}, {7,5,4}, {11,1,0}, {12,1,0}, {13,5,4}, {14,517,4},
        {19,1,0}, {20,1,0}, {21,533,4},
    };
    static const P44TraceSlot predicate_failure[] = {
        {0,1,0}, {10,1,0}, {11,1,0}, {12,21,5}, {13,533,5}, {14,1,0}, {15,1,0}, {16,533,5},
    };
    static const P44TraceSlot body_failure[] = {
        {0,13,5}, {1,13,5}, {2,13,5}, {3,13,5}, {4,525,5}, {19,1,0}, {20,1,0}, {21,533,5},
    };
    static const P44TraceSlot panic_failure[] = {
        {3,1,0}, {4,1,0}, {5,517,5}, {8,1,0}, {9,1,0}, {10,21,5}, {11,533,5},
        {21,1,0}, {22,1,0}, {23,533,5},
    };
    static const C2aTraceCase cases[] = {
        {"p44c_ensures_old_reject", 43, 0, 0, unlike, sizeof unlike / sizeof *unlike},
        {"p44c_ensures_old_bool", 1, 0, 0, bool_old, sizeof bool_old / sizeof *bool_old},
        {"p44c_ensures_old_two", 42, 0, 0, two, sizeof two / sizeof *two},
        {"p44c_ensures_old_repeat", 42, 0, 0, repeated, sizeof repeated / sizeof *repeated},
        {"p44c_ensures_old_requires", 42, 0, 0, requires, sizeof requires / sizeof *requires},
        {"p44c_ensures_old_false", 0, 14, 4, violation, sizeof violation / sizeof *violation},
        {"p44c_ensures_old_predicate_failure", 0, 3, 5, predicate_failure,
            sizeof predicate_failure / sizeof *predicate_failure},
        {"p44c_ensures_old_body_failure", 0, 3, 5, body_failure,
            sizeof body_failure / sizeof *body_failure},
        {"p44c_ensures_old_panic", 0, 1, 5, panic_failure,
            sizeof panic_failure / sizeof *panic_failure},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i)
        if (!c2a_run_trace_case(&cases[i])) return false;
    return true;
}

static bool p44c_qualified_exact_traces(void) {
    static const P44TraceSlot matrix[] = {
        {26,1,0},{27,1,0},{0,257,0},{28,257,0},{40,1,0},{41,1,0},{42,1,0},{43,1,0},
        {17,1,0},{27,1,0},{0,257,0},{28,257,0},{40,1,0},{41,1,0},{42,1,0},{43,1,0},
        {77,1,0},{78,1,0},{54,257,0},{79,257,0},{89,1,0},{90,1,0},{91,1,0},
        {69,1,0},{78,1,0},{54,257,0},{79,257,0},{89,1,0},{90,1,0},{91,1,0},
        {107,1,0},{108,257,0},{110,1,0},{119,1,0},{120,257,0},{122,1,0},
        {301,1,0},{303,1,0},{304,1,0},{305,1,0},{306,1,0},{307,1,0},{308,1,0},
        {309,1,0},{310,1,0},
    };
    static const P44TraceSlot success_false[] = {
        {4,1,0}, {6,5,4}, {8,1,0}, {9,517,4}, {12,1,0}, {13,1,0}, {14,533,4},
    };
    static const P44TraceSlot failure_false[] = {
        {7,1,0}, {9,5,4}, {11,1,0}, {12,517,4}, {15,1,0}, {16,1,0}, {17,533,4},
    };
    static const P44TraceSlot eval_failure[] = {
        {4,1,0}, {10,1,0}, {11,533,4}, {12,1,0}, {13,1,0}, {14,533,4},
    };
    static const P44TraceSlot body_failure[] = {
        {0,13,5}, {1,13,5}, {2,525,5}, {17,1,0}, {18,1,0}, {19,533,5},
    };
    static const P44TraceSlot failure_eval[] = {
        {13,1,0},{21,1,0},{27,1,0},{28,1,0},{29,533,4},{30,1,0},{31,1,0},{32,533,4},
    };
    static const P44TraceSlot failure_body[] = {
        {0,13,5},{1,13,5},{2,525,5},{21,1,0},{22,1,0},{23,533,5},
    };
    static const P44TraceSlot panic_failure[] = {
        {3,1,0},{4,1,0},{5,517,5},{8,1,0},{9,1,0},{10,533,5},{42,1,0},{43,1,0},{44,533,5},
    };
    static const P44TraceSlot callee_failure[] = {
        {0,1,0},{1,1,0},{2,517,5},{5,1,0},{6,1,0},{7,533,5},{24,1,0},{25,1,0},{26,533,5},
    };
    static const P44TraceSlot old_true[] = {
        {11,1,0},{12,257,0},{14,1,0},{15,1,0},{16,1,0},{37,1,0},{39,1,0},{40,1,0},{41,1,0},
    };
    static const P44TraceSlot old_skipped[] = {
        {17,1,0},{27,1,0},{0,257,0},{28,1,0},{29,1,0},{30,1,0},{31,1,0},
        {58,1,0},{60,1,0},{61,1,0},{62,1,0},
    };
    static const P44TraceSlot old_reverse[] = {
        {28,1,0},{29,257,0},{31,1,0},{32,1,0},{33,1,0},{34,1,0},{35,1,0},
        {36,1,0},{37,1,0},{66,1,0},{68,1,0},{69,1,0},{70,1,0},
    };
    static const P44TraceSlot old_false[] = {
        {11,1,0},{13,5,4},{17,1,0},{18,1,0},{19,5,4},{20,517,4},{25,1,0},{26,1,0},{27,533,4},
    };
    static const P44TraceSlot old_eval[] = {
        {6,1,0},{16,1,0},{17,1,0},{18,21,5},{19,533,5},{20,1,0},{21,1,0},{22,533,5},
    };
    static const P44TraceSlot old_body[] = {
        {0,13,5},{1,13,5},{2,13,5},{3,13,5},{4,525,5},{27,1,0},{28,1,0},{29,533,5},
    };
    static const P44TraceSlot ordered[] = {
        {0,257,0},{36,1,0},{37,1,0},{2,257,0},{38,257,0},{50,257,0},{62,257,0},
        {74,257,0},{86,1,0},{87,1,0},{88,1,0},{89,1,0},{0,257,0},{27,1,0},
        {37,1,0},{2,257,0},{38,257,0},{50,257,0},{62,257,0},{74,257,0},{86,1,0},
        {87,1,0},{88,1,0},{89,1,0},{134,1,0},{136,1,0},{137,1,0},{138,1,0},{139,1,0},
    };
    static const P44TraceSlot payload_edges[] = {
        {7,1,0},{8,257,0},{10,1,0},{19,1,0},{20,257,0},{22,1,0},
        {61,1,0},{63,1,0},{64,1,0},{65,1,0},{66,1,0},
    };
    static const P44TraceSlot multihop[] = {
        {4,1,0},{10,1,0},{11,533,5},{12,1,0},{13,1,0},{14,533,5},{29,1,0},{30,1,0},{31,533,5},
    };
    static const P44TraceSlot requires_false[] = {
        {1,5,4},{9,1,0},{10,517,4},{18,1,0},{19,1,0},{20,533,4},
    };
    static const P44TraceSlot requires_eval[] = {
        {11,1,0},{12,533,4},{18,1,0},{19,1,0},{20,533,4},
    };
    static const P44TraceSlot always_false[] = {
        {4,1,0},{6,5,4},{9,1,0},{10,517,4},{18,1,0},{19,1,0},{20,533,4},
    };
    static const P44TraceSlot always_eval[] = {
        {4,1,0},{11,1,0},{12,533,4},{18,1,0},{19,1,0},{20,533,4},
    };
    static const P44TraceSlot failure_old_false[] = {
        {21,1,0},{42,1,0},{1,5,4},{49,1,0},{50,1,0},{51,1,0},{52,1,0},
        {53,5,4},{54,5,4},{55,517,4},{63,1,0},{64,1,0},{65,533,4},
    };
    static const P44TraceSlot failure_old_eval[] = {
        {17,1,0},{34,1,0},{44,1,0},{45,1,0},{46,1,0},{47,21,5},{48,533,5},
        {49,1,0},{50,1,0},{51,533,5},
    };
    static const P44TraceSlot failure_old_skipped[] = {
        {11,1,0},{12,257,0},{14,1,0},{15,1,0},{16,1,0},{37,1,0},{39,1,0},{40,1,0},{41,1,0},
    };
    static const P44TraceSlot failure_old_body[] = {
        {0,13,5},{1,13,5},{2,13,5},{3,13,5},{4,525,5},{27,1,0},{28,1,0},{29,533,5},
    };
    static const C2aTraceCase cases[] = {
        {"p44c_ensures_qualified_result", 42, 0, 0, matrix, sizeof matrix / sizeof *matrix},
        {"p44c_qualified_success_false", 0, 14, 4, success_false,
            sizeof success_false / sizeof *success_false},
        {"p44c_qualified_failure_false", 0, 14, 4, failure_false,
            sizeof failure_false / sizeof *failure_false},
        {"p44c_qualified_eval_failure", 0, 3, 4, eval_failure,
            sizeof eval_failure / sizeof *eval_failure},
        {"p44c_qualified_body_failure", 0, 3, 5, body_failure,
            sizeof body_failure / sizeof *body_failure},
        {"p44c_qualified_failure_eval", 0, 3, 4, failure_eval,
            sizeof failure_eval / sizeof *failure_eval},
        {"p44c_qualified_failure_body", 0, 3, 5, failure_body,
            sizeof failure_body / sizeof *failure_body},
        {"p44c_qualified_panic", 0, 1, 5, panic_failure,
            sizeof panic_failure / sizeof *panic_failure},
        {"p44c_qualified_callee_failure", 0, 12, 5, callee_failure,
            sizeof callee_failure / sizeof *callee_failure},
        {"p44c_qualified_old_true", 42, 0, 0, old_true, sizeof old_true / sizeof *old_true},
        {"p44c_qualified_old_skipped", 42, 0, 0, old_skipped,
            sizeof old_skipped / sizeof *old_skipped},
        {"p44c_qualified_old_reverse", 42, 0, 0, old_reverse,
            sizeof old_reverse / sizeof *old_reverse},
        {"p44c_qualified_old_false", 0, 14, 4, old_false,
            sizeof old_false / sizeof *old_false},
        {"p44c_qualified_old_predicate_failure", 0, 3, 5, old_eval,
            sizeof old_eval / sizeof *old_eval},
        {"p44c_qualified_old_body_failure", 0, 3, 5, old_body,
            sizeof old_body / sizeof *old_body},
        {"p44c_qualified_order", 42, 0, 0, ordered, sizeof ordered / sizeof *ordered},
        {"p44c_qualified_payload_edges", 42, 0, 0, payload_edges,
            sizeof payload_edges / sizeof *payload_edges},
        {"p44c_qualified_multihop_failure", 0, 3, 5, multihop,
            sizeof multihop / sizeof *multihop},
        {"p44c_qualified_requires_false", 0, 13, 4, requires_false,
            sizeof requires_false / sizeof *requires_false},
        {"p44c_qualified_requires_eval", 0, 3, 4, requires_eval,
            sizeof requires_eval / sizeof *requires_eval},
        {"p44c_qualified_always_false", 0, 14, 4, always_false,
            sizeof always_false / sizeof *always_false},
        {"p44c_qualified_always_eval", 0, 3, 4, always_eval,
            sizeof always_eval / sizeof *always_eval},
        {"p44c_qualified_failure_old_false", 0, 14, 4, failure_old_false,
            sizeof failure_old_false / sizeof *failure_old_false},
        {"p44c_qualified_failure_old_eval", 0, 3, 5, failure_old_eval,
            sizeof failure_old_eval / sizeof *failure_old_eval},
        {"p44c_qualified_failure_old_skipped", 42, 0, 0, failure_old_skipped,
            sizeof failure_old_skipped / sizeof *failure_old_skipped},
        {"p44c_qualified_failure_old_body", 0, 3, 5, failure_old_body,
            sizeof failure_old_body / sizeof *failure_old_body},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i)
        if (!c2a_run_trace_case(&cases[i])) {
            fprintf(stderr, "qualified trace failed: %s\n", cases[i].leaf);
            return false;
        }
    char directory[512]; PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    (void)snprintf(directory, sizeof directory, "%s/tests/conformance/p44c_qualified_old_reverse",
        SOL_TEST_SOURCE_DIR);
    bool reverse = propagation_pipeline_build_named(&pipeline, directory, "launch", false)
        && pipeline.cleanup.action_count > 37;
    for (size_t i = 0; reverse && i < 3; ++i) {
        const SolMirRuntimeCleanupAction *action = &pipeline.cleanup.actions[35 + i];
        reverse = action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT
            && action->target == 2 - i;
    }
    propagation_pipeline_free(&pipeline);
    return reverse && c1_trace_excludes(success_false, sizeof success_false / sizeof *success_false,
            eval_failure + 1, 1)
        && c1_trace_excludes(eval_failure, sizeof eval_failure / sizeof *eval_failure,
            success_false + 1, 1);
}

static bool p44c_qualified_direct_routes(void) {
    char directory[512]; PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    (void)snprintf(directory, sizeof directory,
        "%s/tests/conformance/p44c_ensures_qualified_result", SOL_TEST_SOURCE_DIR);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false);
    const SolMirRuntimeConventions *conventions = &pipeline.conventions;
    const SolMirConcreteProgram *concrete = &pipeline.concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    size_t qualified_calls = 0;
    for (size_t i = 0; ok && i < conventions->call_count; ++i) {
        const SolMirRuntimeCall *call = &conventions->calls[i];
        if (call->internal >= concrete->linkage.callable_count) continue;
        size_t image = concrete->linkage.callables[call->internal].instance;
        bool qualified = false;
        for (size_t b = 0; b < concrete->operations.predicate_body_count; ++b) {
            const SolMirPredicateBody *body = &concrete->operations.predicate_bodies[b];
            qualified = qualified || (body->owner_kind == SOL_MIR_PREDICATE_OWNER_INSTANCE
                && body->instance == image && body->phase == SOL_CONTRACT_ENSURES
                && (body->outcome == SOL_CONTRACT_OUTCOME_SUCCESS
                    || body->outcome == SOL_CONTRACT_OUTCOME_FAILURE));
        }
        if (!qualified) continue;
        ++qualified_calls;
        if (call->owner_kind != SOL_MIR_RUNTIME_CALL_OWNER_IMAGE
            || call->call_kind != SOL_IR_CALL_FUNCTION
            || call->target_kind != SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL
            || call->writebacks.count != 0
            || call->result.kind != SOL_MIR_RUNTIME_VALUE_MATERIALIZED_VALUE
            || call->result.id >= m->value_count || call->signature >= conventions->signature_count
            || m->values[call->result.id].type != conventions->signatures[call->signature].result
            || conventions->signatures[call->signature].result
                >= concrete->representation.recipe_count
            || concrete->representation.recipes[conventions->signatures[call->signature].result].kind
                != SOL_MIR_RECIPE_RESULT)
            ok = false;
    }
    propagation_pipeline_free(&pipeline);
    return ok && qualified_calls == 6;
}

static bool c2b2_later_qualified_absent_case(const char *leaf,
    SolContractOutcomeKind outcome) {
    char directory[512], entry[256]; PropagationPipeline pipeline; SolWasmRepresentedOutput output;
    WasmInstance instance = {0}; P44TraceSlot trace[64]; size_t count = 0; bool overflow = false;
    int64_t value = 0; int32_t code = 0, site = 0;
    size_t body_id = SOL_MIR_RUNTIME_NONE, bodies = 0, plan_block = SOL_MIR_RUNTIME_NONE, plans = 0;
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    (void)snprintf(directory, sizeof directory, "%s/tests/conformance/%s", SOL_TEST_SOURCE_DIR, leaf);
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false);
    for (size_t i = 0; ok && i < pipeline.concrete.operations.predicate_body_count; ++i) {
        const SolMirPredicateBody *body = &pipeline.concrete.operations.predicate_bodies[i];
        if (body->owner_kind == SOL_MIR_PREDICATE_OWNER_INSTANCE
            && body->phase == SOL_CONTRACT_ENSURES && body->outcome == outcome) {
            body_id = i; ++bodies;
        }
    }
    for (size_t i = 0; ok && i < pipeline.concrete.operations.predicate_count; ++i) {
        const SolMirOperationPredicatePlan *plan = &pipeline.concrete.operations.predicates[i];
        if (plan->kind == SOL_MIR_OPERATION_PREDICATE_CONTRACT && plan->body == body_id) {
            plan_block = plan->block; ++plans;
        }
    }
    size_t check_event = plans == 1 && plan_block < pipeline.lowered.image_terminator_count
        ? pipeline.lowered.image_terminators[plan_block].cleanup_event : SOL_MIR_RUNTIME_NONE;
    ok = ok && bodies == 1 && plans == 1 && check_event < pipeline.cleanup.event_count
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry)
        && wasm_instance_open(&output.bytes, entry, &instance)
        && wasm_instance_observe(&instance, &value, &code, &site)
        && wasm_instance_trace(&instance, trace, sizeof trace / sizeof *trace, &count, &overflow)
        && !overflow;
    bool forbidden[512] = {0}; size_t forbidden_count = 0;
    for (size_t e = 0; ok && e < pipeline.cleanup.event_count; ++e) {
        const SolMirRuntimeCleanupEvent *event = &pipeline.cleanup.events[e];
        bool selected = (event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION
            && event->owner == body_id) || e == check_event;
        if (!selected) continue;
        for (size_t t = 0; t < event->transitions.count; ++t) {
            const SolMirRuntimeCleanupTransition *transition = &pipeline.cleanup.transitions[
                event->transitions.offset + t];
            for (size_t a = 0; a < transition->actions.count; ++a) {
                size_t action = transition->actions.offset + a;
                if (action >= sizeof forbidden / sizeof *forbidden) ok = false;
                else if (!forbidden[action]) { forbidden[action] = true; ++forbidden_count; }
            }
        }
    }
    for (size_t i = 0; ok && i < count; ++i)
        if (trace[i].action < sizeof forbidden / sizeof *forbidden
            && forbidden[trace[i].action]) ok = false;
    ok = ok && forbidden_count != 0;
    if (!ok) fprintf(stderr, "later clause absence failed %s bodies=%zu plans=%zu event=%zu forbidden=%zu trace=%zu\n",
        leaf, bodies, plans, check_event, forbidden_count, count);
    wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false); propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44c_qualified_later_clauses_absent(void) {
    static const char *success_cases[] = {"p44c_qualified_requires_false",
        "p44c_qualified_requires_eval", "p44c_qualified_always_false",
        "p44c_qualified_always_eval"};
    for (size_t i = 0; i < sizeof success_cases / sizeof *success_cases; ++i)
        if (!c2b2_later_qualified_absent_case(success_cases[i], SOL_CONTRACT_OUTCOME_SUCCESS))
            return false;
    return true;
}

static bool c2b2_snapshot_drop_case(const char *leaf, size_t expected) {
    char directory[512], entry[256]; PropagationPipeline pipeline; SolWasmRepresentedOutput output;
    WasmInstance instance = {0}; P44TraceSlot trace[64]; size_t count = 0; bool overflow = false;
    int64_t value = 0; int32_t code = 0, site = 0, prior = INT32_MAX; size_t drops = 0;
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    (void)snprintf(directory, sizeof directory, "%s/tests/conformance/%s", SOL_TEST_SOURCE_DIR, leaf);
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false)
        && pipeline.concrete.operations.snapshot_count == expected
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry)
        && wasm_instance_open(&output.bytes, entry, &instance)
        && wasm_instance_observe(&instance, &value, &code, &site)
        && wasm_instance_trace(&instance, trace, sizeof trace / sizeof *trace, &count, &overflow)
        && !overflow;
    size_t snapshot_inputs = 0; uint64_t slots = 0;
    for (size_t b = 0; ok && b < pipeline.concrete.operations.predicate_body_count; ++b) {
        const SolMirPredicateBody *body = &pipeline.concrete.operations.predicate_bodies[b];
        if (body->phase != SOL_CONTRACT_ENSURES
            || body->outcome != SOL_CONTRACT_OUTCOME_FAILURE) continue;
        for (size_t i = 0; i < body->inputs.count; ++i) {
            const SolMirPredicateInput *input = &pipeline.concrete.operations.predicate_inputs[
                body->inputs.offset + i];
            if (input->kind != SOL_MIR_PREDICATE_INPUT_SNAPSHOT) continue;
            if (input->ordinal >= 64 || (slots & (UINT64_C(1) << input->ordinal)) != 0) ok = false;
            else slots |= UINT64_C(1) << input->ordinal;
            ++snapshot_inputs;
        }
    }
    ok = ok && snapshot_inputs == expected
        && slots == (expected == 64 ? UINT64_MAX : (UINT64_C(1) << expected) - 1);
    for (size_t i = 0; ok && i < count; ++i) {
        if (trace[i].action >= pipeline.cleanup.action_count) { ok = false; break; }
        const SolMirRuntimeCleanupAction *action = &pipeline.cleanup.actions[trace[i].action];
        if (action->kind != SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT) continue;
        if (action->target >= expected || (drops != 0 && (int32_t)action->target >= prior)) ok = false;
        prior = (int32_t)action->target; ++drops;
    }
    ok = ok && drops == expected && prior == 0;
    wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false); propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44c_qualified_failure_old_snapshot_routes(void) {
    return c2b2_snapshot_drop_case("p44c_qualified_failure_old_false", 2)
        && c2b2_snapshot_drop_case("p44c_qualified_failure_old_eval", 1)
        && c2b2_snapshot_drop_case("p44c_qualified_failure_old_skipped", 1)
        && c2b2_snapshot_drop_case("p44c_qualified_failure_old_body", 1);
}

static bool p44c_qualified_panic_detail(void) {
    static const uint8_t detail[] = "qualified body panic";
    char directory[512], entry[256]; PropagationPipeline pipeline; SolWasmRepresentedOutput output;
    WasmInstance instance = {0}; P44TraceSlot trace[16]; size_t trace_count = 0;
    bool overflow = false; int64_t value = 0; int32_t code = 0, observed_site = 0;
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    (void)snprintf(directory, sizeof directory, "%s/tests/conformance/p44c_qualified_panic",
        SOL_TEST_SOURCE_DIR);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false);
    size_t sites = 0, events = 0, direct = 0;
    for (size_t i = 0; ok && i < pipeline.conventions.failure_site_count; ++i) {
        const SolMirRuntimeFailureSite *site = &pipeline.conventions.failure_sites[i];
        sites += site->origin_kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_PANIC
            && site->source.file == 0 && site->source.start == 115 && site->source.end == 143
            && site->allowed_codes == UINT32_C(1);
    }
    for (size_t i = 0; ok && i < pipeline.cleanup.event_count; ++i) {
        const SolMirRuntimeCleanupEvent *event = &pipeline.cleanup.events[i];
        events += event->producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_PANIC
            && event->captures_failure_detail
            && event->capture_detail_kind == SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT;
    }
    for (size_t i = 0; ok && i < pipeline.conventions.call_count; ++i) {
        const SolMirRuntimeCall *call = &pipeline.conventions.calls[i];
        direct += call->call_kind == SOL_IR_CALL_FUNCTION
            && call->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL;
    }
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    ok = ok && sizeof detail - 1 == 20 && sites == 1 && events == 1 && direct == 2
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry)
        && wasm_instance_open(&output.bytes, entry, &instance)
        && wasm_instance_observe(&instance, &value, &code, &observed_site)
        && value == 0 && code == 1 && observed_site == 5
        && wasm_instance_trace(&instance, trace, sizeof trace / sizeof *trace, &trace_count,
            &overflow) && trace_count == 9 && !overflow
        /* The source Text has already traversed callee/caller cleanup; the
         * packet owns an independent bounded copy with an exact trailing NUL. */
        && wasm_instance_panic_detail(&instance, detail, sizeof detail - 1);
    wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false);
    propagation_pipeline_free(&pipeline);
    return ok && p44_same_instance_packet_reset(directory, 5, detail, sizeof detail - 1);
}

typedef enum {
    C2B2_TRANSPORT_OK,
    C2B2_TRANSPORT_ERR,
    C2B2_TRANSPORT_PREDICATE_FAILURE,
    C2B2_TRANSPORT_BODY_FAILURE,
} C2b2TransportRoute;

typedef struct {
    const char *leaf;
    C2b2TransportRoute route;
    int64_t value;
    int32_t code, record;
    SolMirRuntimeFailureOriginKind origin;
    uint32_t mask, start, end, ordinal;
    const P44TraceSlot *trace;
    size_t trace_count;
} C2b2TransportCase;

typedef struct {
    size_t body;
    size_t satisfied_action;
    size_t violation_action;
} C2b2TransportClause;

static const char *c2b2_transport_image_name(const PropagationPipeline *pipeline, size_t image) {
    if (pipeline == NULL || image >= pipeline->concrete.materialization.image_count) return NULL;
    size_t source = pipeline->concrete.materialization.images[image].source_callable;
    return source < pipeline->ir.callable_count ? pipeline->ir.callables[source].name : NULL;
}

static size_t c2b2_transport_trace_position(const P44TraceSlot *trace, size_t count,
    size_t action, size_t *matches) {
    size_t position = SIZE_MAX, found = 0;
    for (size_t i = 0; trace != NULL && i < count; ++i)
        if (trace[i].action == action) { position = i; ++found; }
    if (matches != NULL) *matches = found;
    return position;
}

static bool c2b2_transport_calls(const PropagationPipeline *pipeline) {
    if (pipeline == NULL) return false;
    const SolMirRuntimeConventions *conventions = &pipeline->conventions;
    const SolMirConcreteProgram *concrete = &pipeline->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    size_t inner = 0, outer = 0; SolMirRecipeId shared_result = SOL_MIR_RECIPE_NONE;
    for (size_t i = 0; i < conventions->call_count; ++i) {
        const SolMirRuntimeCall *call = &conventions->calls[i];
        if (call->image >= m->image_count || call->internal >= concrete->linkage.callable_count
            || call->signature >= conventions->signature_count || call->block >= m->block_count)
            return false;
        size_t callee_image = concrete->linkage.callables[call->internal].instance;
        const char *caller = c2b2_transport_image_name(pipeline, call->image);
        const char *callee = c2b2_transport_image_name(pipeline, callee_image);
        const SolMirRuntimeSignature *signature = &conventions->signatures[call->signature];
        if (caller == NULL || callee == NULL || signature->result >= concrete->representation.recipe_count
            || signature->result >= concrete->layout.type_count) return false;
        bool selected = (!strcmp(caller, "outer") && !strcmp(callee, "inner"))
            || (!strcmp(caller, "launch") && !strcmp(callee, "outer"));
        if (!selected) return false;
        const SolMirRecipe *recipe = &concrete->representation.recipes[signature->result];
        const SolMirTypeLayout *layout = &concrete->layout.types[signature->result];
        const SolMirMaterializedTerminator *term = &m->blocks[call->block].terminator;
        if (call->call_kind != SOL_IR_CALL_FUNCTION
            || call->target_kind != SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL
            || call->writebacks.count != 0
            || call->result.kind != SOL_MIR_RUNTIME_VALUE_MATERIALIZED_VALUE
            || call->result.id >= m->value_count
            || m->values[call->result.id].type != signature->result
            || signature->result_class != SOL_MIR_RUNTIME_RESULT_VALUE
            || recipe->kind != SOL_MIR_RECIPE_RESULT
            || recipe->storage != SOL_MIR_STORAGE_AGGREGATE_VALUE
            || layout->object_kind != SOL_MIR_LAYOUT_OBJECT_SUM || !layout->has_object
            || layout->value_size != 4 || term->kind != SOL_MIR_TERM_INVOKE
            || term->call_kind != SOL_IR_CALL_FUNCTION) return false;
        if (shared_result == SOL_MIR_RECIPE_NONE) shared_result = signature->result;
        else if (shared_result != signature->result) return false;
        if (!strcmp(callee, "inner")) ++inner; else ++outer;
    }
    return inner == 1 && outer == 1 && conventions->call_count == 2;
}

static bool c2b2_transport_clauses(const PropagationPipeline *pipeline,
    C2b2TransportClause clauses[4]) {
    if (pipeline == NULL || clauses == NULL) return false;
    const SolMirOperations *operations = &pipeline->concrete.operations;
    for (size_t i = 0; i < 4; ++i) clauses[i] = (C2b2TransportClause){SOL_MIR_RUNTIME_NONE,
        SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE};
    size_t found = 0;
    for (size_t body_id = 0; body_id < operations->predicate_body_count; ++body_id) {
        const SolMirPredicateBody *body = &operations->predicate_bodies[body_id];
        const char *owner = c2b2_transport_image_name(pipeline, body->instance);
        if (body->owner_kind != SOL_MIR_PREDICATE_OWNER_INSTANCE || owner == NULL
            || body->phase != SOL_CONTRACT_ENSURES
            || (body->outcome != SOL_CONTRACT_OUTCOME_SUCCESS
                && body->outcome != SOL_CONTRACT_OUTCOME_FAILURE)) continue;
        size_t owner_index = !strcmp(owner, "inner") ? 0 : !strcmp(owner, "outer") ? 1 : SIZE_MAX;
        if (owner_index == SIZE_MAX) continue;
        size_t outcome_index = body->outcome == SOL_CONTRACT_OUTCOME_SUCCESS ? 0 : 1;
        size_t slot = owner_index * 2 + outcome_index;
        if (clauses[slot].body != SOL_MIR_RUNTIME_NONE) return false;
        size_t snapshots = 0, plans = 0, plan_block = SOL_MIR_RUNTIME_NONE;
        for (size_t i = 0; i < body->inputs.count; ++i)
            snapshots += operations->predicate_inputs[body->inputs.offset + i].kind
                == SOL_MIR_PREDICATE_INPUT_SNAPSHOT;
        if (snapshots != (outcome_index == 0 ? 2 : 1)) return false;
        for (size_t p = 0; p < operations->predicate_count; ++p) {
            const SolMirOperationPredicatePlan *plan = &operations->predicates[p];
            if (plan->kind == SOL_MIR_OPERATION_PREDICATE_CONTRACT && plan->body == body_id) {
                plan_block = plan->block; ++plans;
            }
        }
        if (plans != 1 || plan_block >= pipeline->lowered.image_terminator_count) return false;
        size_t event_id = pipeline->lowered.image_terminators[plan_block].cleanup_event;
        if (event_id >= pipeline->cleanup.event_count) return false;
        const SolMirRuntimeCleanupEvent *event = &pipeline->cleanup.events[event_id];
        const SolMirRuntimeCleanupTransition *satisfied = NULL, *violation = NULL;
        for (size_t t = 0; t < event->transitions.count; ++t) {
            const SolMirRuntimeCleanupTransition *transition = &pipeline->cleanup.transitions[
                event->transitions.offset + t];
            if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_SATISFIED)
                satisfied = transition;
            else if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION)
                violation = transition;
        }
        if (satisfied == NULL || violation == NULL || satisfied->actions.count != 1
            || violation->actions.count != 1) return false;
        size_t arithmetic_sites = 0;
        for (size_t i = 0; i < pipeline->conventions.failure_site_count; ++i) {
            const SolMirRuntimeFailureSite *site = &pipeline->conventions.failure_sites[i];
            arithmetic_sites += site->origin_kind
                    == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC
                && site->owner == body_id
                && (site->allowed_codes & (UINT32_C(1)
                    << (SOL_MIR_RUNTIME_FAILURE_DIVISION_BY_ZERO - 1))) != 0;
        }
        if (arithmetic_sites == 0) return false;
        clauses[slot] = (C2b2TransportClause){body_id, satisfied->actions.offset,
            violation->actions.offset};
        ++found;
    }
    return found == 4;
}

static bool c2b2_transport_failure_owner(const C2b2TransportCase *test,
    const PropagationPipeline *pipeline, const SolWasmRepresentedOutput *output) {
    ProvenanceRecord record; size_t matches = 0, image = SOL_MIR_RUNTIME_NONE;
    size_t selected = SOL_MIR_RUNTIME_NONE;
    if (test == NULL || pipeline == NULL || output == NULL || test->code == 0
        || !provenance_record(&output->bytes, (uint32_t)test->record, &record)
        || record.tag != 3 || record.kind != (uint8_t)test->origin
        || !bytes_equal(record.path, record.path_count, "main.sol")
        || record.start != test->start || record.end != test->end) return false;
    for (size_t i = 0; i < pipeline->conventions.failure_site_count; ++i) {
        const SolMirRuntimeFailureSite *site = &pipeline->conventions.failure_sites[i];
        if (site->origin_kind != test->origin || site->source.file != 0
            || site->source.start != test->start || site->source.end != test->end
            || site->allowed_codes != test->mask) continue;
        if (test->origin == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT) {
            if (site->owner >= pipeline->concrete.operations.predicate_body_count) continue;
            const SolMirPredicateBody *body = &pipeline->concrete.operations.predicate_bodies[
                site->owner];
            if (body->outcome != SOL_CONTRACT_OUTCOME_SUCCESS) continue;
            image = body->instance;
        } else image = site->owner;
        selected = i;
        ++matches;
    }
    const char *owner = c2b2_transport_image_name(pipeline, image);
    const char *symbol = image < pipeline->concrete.materialization.image_count
        ? p44_symbol_for_image(pipeline, image) : NULL;
    return matches == 1 && record.ordinal == selected
        && owner != NULL && !strcmp(owner, "inner") && symbol != NULL
        && bytes_equal(record.symbol, record.symbol_count, symbol);
}

static bool c2b2_transport_case(const C2b2TransportCase *test) {
    char directory[512], entry[256]; PropagationPipeline pipeline; SolWasmRepresentedOutput output;
    WasmInstance instance = {0}; P44TraceSlot actual[64], repeated[64];
    size_t count = 0, repeated_count = 0; bool overflow = false, repeated_overflow = false;
    int64_t value = 0; int32_t code = 0, site = 0; C2b2TransportClause clauses[4];
    if (test == NULL || snprintf(directory, sizeof directory, "%s/tests/conformance/%s",
            SOL_TEST_SOURCE_DIR, test->leaf) < 0) return false;
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false)
        && pipeline.diagnostics.count == 0
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)
        && c2b2_transport_calls(&pipeline)
        && c2b2_transport_clauses(&pipeline, clauses)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry)
        && wasm_instance_open(&output.bytes, entry, &instance)
        && wasm_instance_observe(&instance, &value, &code, &site)
        && value == test->value && code == test->code && site == test->record
        && wasm_instance_trace(&instance, actual, sizeof actual / sizeof *actual, &count, &overflow)
        && c1_trace_matches(&pipeline, test->trace, test->trace_count, (uint32_t)test->record,
            actual, count, overflow);
    if (ok && test->code != 0)
        ok = c2b2_transport_failure_owner(test, &pipeline, &output);
    size_t positions[4] = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
    for (size_t i = 0; ok && i < 4; ++i) {
        size_t matches = 0;
        positions[i] = c2b2_transport_trace_position(actual, count,
            clauses[i].satisfied_action, &matches);
        if (test->route == C2B2_TRANSPORT_OK || test->route == C2B2_TRANSPORT_ERR)
            ok = matches == 1;
        else ok = matches == 0;
    }
    if (ok && (test->route == C2B2_TRANSPORT_OK || test->route == C2B2_TRANSPORT_ERR))
        ok = positions[0] < positions[1] && positions[1] < positions[2]
            && positions[2] < positions[3];
    if (ok && test->route == C2B2_TRANSPORT_PREDICATE_FAILURE) {
        size_t matches = 0;
        (void)c2b2_transport_trace_position(actual, count, clauses[0].violation_action, &matches);
        ok = matches == 1;
        for (size_t i = 1; ok && i < 4; ++i) {
            (void)c2b2_transport_trace_position(actual, count, clauses[i].violation_action,
                &matches);
            ok = matches == 0;
        }
    }
    if (ok && test->route == C2B2_TRANSPORT_BODY_FAILURE)
        for (size_t i = 0; ok && i < 4; ++i) {
            size_t matches = 0;
            (void)c2b2_transport_trace_position(actual, count, clauses[i].violation_action,
                &matches);
            ok = matches == 0;
        }
    for (size_t i = 0; ok && i < count; ++i)
        if (actual[i].record != 0 && actual[i].record != (uint32_t)test->record) ok = false;
    if (ok) ok = wasm_instance_call(&instance, test->value, test->code, test->record)
        && wasm_instance_trace(&instance, repeated, sizeof repeated / sizeof *repeated,
            &repeated_count, &repeated_overflow)
        && repeated_count == count && repeated_overflow == overflow
        && memcmp(repeated, actual, count * sizeof *actual) == 0;
    if (!ok) fprintf(stderr, "qualified direct transport failed: %s value=%lld code=%d site=%d trace=%zu\n",
        test->leaf, (long long)value, code, site, count);
    wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false); propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44c_qualified_direct_transport(void) {
    static const P44TraceSlot ok_trace[] = {
        {94,1,0},{95,1,0},{96,1,0},{97,1,0},{0,257,0},{98,257,0},{114,1,0},
        {115,1,0},{116,1,0},{117,1,0},{118,1,0},{119,1,0},{153,1,0},{154,1,0},
        {134,257,0},{155,257,0},{171,1,0},{172,1,0},{173,1,0},{174,1,0},
        {175,1,0},{176,1,0},{203,1,0},{205,1,0},{206,1,0},{207,1,0},
    };
    static const P44TraceSlot err_trace[] = {
        {46,1,0},{96,1,0},{97,1,0},{0,257,0},{98,257,0},{114,1,0},{115,1,0},
        {116,1,0},{117,1,0},{118,1,0},{119,1,0},{153,1,0},{154,1,0},{134,257,0},
        {155,257,0},{171,1,0},{172,1,0},{173,1,0},{174,1,0},{175,1,0},{176,1,0},
        {207,1,0},{209,1,0},{210,1,0},{211,1,0},
    };
    static const P44TraceSlot predicate_failure[] = {
        {70,1,0},{95,1,0},{96,1,0},{97,1,0},{1,5,5},{100,1,0},{101,1,0},
        {102,1,0},{103,5,5},{104,5,5},{105,5,5},{106,517,5},{136,1,0},
        {137,1,0},{138,1,0},{139,1,0},{140,21,5},{141,21,5},{142,21,5},
        {143,533,5},{191,1,0},{192,1,0},{193,533,5},
    };
    static const P44TraceSlot body_failure[] = {
        {2,13,11},{3,13,11},{4,13,11},{5,13,11},{6,13,11},{7,13,11},
        {8,13,11},{9,13,11},{10,525,11},{136,1,0},{137,1,0},{138,1,0},
        {139,1,0},{140,21,11},{141,21,11},{142,21,11},{143,533,11},
        {191,1,0},{192,1,0},{193,533,11},
    };
    static const C2b2TransportCase cases[] = {
        {"p44c_qualified_direct_transport", C2B2_TRANSPORT_OK, 42, 0, 0, 0, 0, 0, 0, 0,
            ok_trace, sizeof ok_trace / sizeof *ok_trace},
        {"p44c_qualified_direct_transport_err", C2B2_TRANSPORT_ERR, 42, 0, 0, 0, 0, 0, 0, 0,
            err_trace, sizeof err_trace / sizeof *err_trace},
        {"p44c_qualified_direct_transport_predicate_failure",
            C2B2_TRANSPORT_PREDICATE_FAILURE, 0, 14, 5,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT, UINT32_C(0x2000), 188, 244, 0,
            predicate_failure, sizeof predicate_failure / sizeof *predicate_failure},
        {"p44c_qualified_direct_transport_body_failure", C2B2_TRANSPORT_BODY_FAILURE, 0, 3, 11,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_ARITHMETIC, UINT32_C(6), 328, 337, 4,
            body_failure, sizeof body_failure / sizeof *body_failure},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i)
        if (!c2b2_transport_case(&cases[i])) return false;
    return true;
}

static bool c2a_rebuilt_identity(const char *directory,
    const SolWasmRepresentedOutput *baseline, const uint8_t expected_hash[32]);

typedef struct {
    size_t source_snapshot[2], snapshot_expression[2];
} C2b1SnapshotIdentityPair;

/* Locate the equal-recipe source pair. The mutation itself is confined to the
 * owning IR; public builders reconstruct every downstream authoritative row. */
static bool c2b1_equal_snapshot_pair(PropagationPipeline *pipeline,
    C2b1SnapshotIdentityPair *pair) {
    SolMirMaterialization *m = &pipeline->concrete.materialization;
    SolMirOperations *operations = &pipeline->concrete.operations;
    if (pair == NULL || operations->snapshot_count != 2
        || operations->snapshots[0].image != operations->snapshots[1].image
        || operations->snapshots[0].context != operations->snapshots[1].context
        || operations->snapshots[0].recipe != operations->snapshots[1].recipe
        || operations->snapshots[0].slot != 0 || operations->snapshots[1].slot != 1)
        return false;
    size_t instruction[2] = {operations->snapshots[0].instruction,
        operations->snapshots[1].instruction};
    *pair = (C2b1SnapshotIdentityPair){
        .source_snapshot = {SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE},
        .snapshot_expression = {SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE},
    };
    if (instruction[0] >= m->instruction_count || instruction[1] >= m->instruction_count) return false;
    for (size_t side = 0; side < 2; ++side) {
        size_t source = m->instructions[instruction[side]].source_snapshot;
        size_t expression_matches = 0;
        pair->source_snapshot[side] = source;
        for (size_t i = 0; i < pipeline->ir.expression_count; ++i)
            if (pipeline->ir.expressions[i].kind == SOL_IR_EXPR_SNAPSHOT_READ
                && pipeline->ir.expressions[i].as.snapshot == source) {
                pair->snapshot_expression[side] = i; ++expression_matches;
            }
        if (source >= pipeline->ir.snapshot_count || expression_matches != 1) return false;
    }
    return true;
}

#define C2B1_SWAP(type, left, right) do { type temporary = (left); (left) = (right); \
    (right) = temporary; } while (0)

/* Involution: exchange the operands and read expressions of two same-type
 * snapshots, and exchange the predicate references so source semantics remain
 * unchanged. A rebuild then maps the logical identities to opposite slots. */
static void c2b1_swap_equal_snapshot_identities(PropagationPipeline *pipeline,
    const C2b1SnapshotIdentityPair *pair) {
    SolIr *ir = &pipeline->ir;
    SolIrSnapshot *left_snapshot = &ir->snapshots[pair->source_snapshot[0]];
    SolIrSnapshot *right_snapshot = &ir->snapshots[pair->source_snapshot[1]];
    C2B1_SWAP(SolIrExpressionId, left_snapshot->read, right_snapshot->read);
    C2B1_SWAP(SolIrExpressionId, left_snapshot->operand, right_snapshot->operand);
    C2B1_SWAP(SolIrSnapshotId,
        ir->expressions[pair->snapshot_expression[0]].as.snapshot,
        ir->expressions[pair->snapshot_expression[1]].as.snapshot);
}

#undef C2B1_SWAP

static bool c2b1_predecessors_valid(const PropagationPipeline *pipeline) {
    return sol_ir_validate(&pipeline->ir, NULL)
        && sol_mir_concrete_program_validate(&pipeline->concrete, NULL)
        && sol_mir_runtime_conventions_validate(&pipeline->conventions, NULL)
        && sol_mir_runtime_values_validate(&pipeline->values, NULL)
        && sol_mir_runtime_cleanup_validate(&pipeline->cleanup, NULL)
        && sol_mir_runtime_host_abi_validate(&pipeline->host, NULL)
        && sol_mir_runtime_handler_abi_validate(&pipeline->handler, NULL);
}

/* Rebuild P1b through P3.6 from the currently owned IR. This is deliberately
 * the public builder path, not hand-sealing malformed predecessor rows. */
static bool c2b1_rebuild_snapshot_downstream(PropagationPipeline *pipeline) {
    sol_mir_runtime_lowered_program_free(&pipeline->lowered);
    sol_mir_runtime_handler_abi_free(&pipeline->handler);
    sol_mir_runtime_host_abi_free(&pipeline->host);
    sol_mir_runtime_cleanup_free(&pipeline->cleanup);
    sol_mir_runtime_values_free(&pipeline->values);
    sol_mir_runtime_conventions_free(&pipeline->conventions);
    sol_mir_concrete_program_free(&pipeline->concrete);
    sol_mir_concrete_program_init(&pipeline->concrete);
    sol_mir_runtime_conventions_init(&pipeline->conventions);
    sol_mir_runtime_values_init(&pipeline->values);
    sol_mir_runtime_cleanup_init(&pipeline->cleanup);
    sol_mir_runtime_host_abi_init(&pipeline->host);
    sol_mir_runtime_handler_abi_init(&pipeline->handler);
    sol_mir_runtime_lowered_program_init(&pipeline->lowered);
    SolIrCallableId callable = SOL_IR_NONE;
    for (size_t i = 0; i < pipeline->ir.callable_count; ++i)
        if (pipeline->ir.callables[i].kind == SOL_IR_CALLABLE_FUNCTION
            && strcmp(pipeline->ir.callables[i].name, "launch") == 0) callable = i;
    SolMirProgramRoot root = {callable, SOL_MIR_PROGRAM_ROOT_ENTRY};
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    return callable != SOL_IR_NONE && sol_ir_validate(&pipeline->ir, NULL)
        && sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&pipeline->ir,
                &root, 1, NULL, 0, &target, NULL}, &pipeline->concrete, &pipeline->diagnostics)
            == SOL_MIR_CONCRETE_BUILD_SUCCEEDED
        && sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){
                &pipeline->concrete, NULL}, &pipeline->conventions, &pipeline->diagnostics)
            == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED
        && sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&pipeline->conventions,
                NULL}, &pipeline->values, &pipeline->diagnostics)
            == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED
        && sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&pipeline->conventions,
                &pipeline->values, NULL}, &pipeline->cleanup, &pipeline->diagnostics)
            == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED
        && sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){
                &pipeline->conventions, &pipeline->values, &pipeline->cleanup, NULL},
                &pipeline->host, &pipeline->diagnostics) == SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED
        && sol_mir_runtime_handler_abi_build(&(SolMirRuntimeHandlerAbiBuildRequest){
                &pipeline->conventions, &pipeline->values, &pipeline->cleanup, &pipeline->host,
                NULL}, &pipeline->handler, &pipeline->diagnostics)
            == SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_SUCCEEDED
        && sol_mir_runtime_lowered_program_build(&(SolMirRuntimeLoweredProgramBuildRequest){
                &pipeline->conventions, &pipeline->values, &pipeline->cleanup, &pipeline->host,
                &pipeline->handler, NULL}, &pipeline->lowered, &pipeline->diagnostics)
            == SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED;
}

static bool p44c_old_predecessor_mutations(const char *directory) {
    PropagationPipeline pipeline; SolWasmRepresentedOutput baseline;
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&baseline);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false);
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, NULL}, &baseline, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
    uint8_t baseline_hash[32] = {0};
    if (ok) sha256(baseline.bytes.bytes, baseline.bytes.count, baseline_hash);
    C2b1SnapshotIdentityPair pair;
    if (ok) ok = c2b1_equal_snapshot_pair(&pipeline, &pair);
    uint64_t baseline_authentication = pipeline.lowered.authentication;
    SolMirMaterializedLocalId baseline_local[2] = {SOL_MIR_MATERIALIZED_NONE,
        SOL_MIR_MATERIALIZED_NONE};
    SolIrExpressionId baseline_provenance_expression[2] = {SOL_IR_NONE, SOL_IR_NONE};
    if (ok) for (size_t side = 0; side < 2; ++side) {
        const SolMirOperationSnapshotPlan *snapshot = &pipeline.concrete.operations.snapshots[side];
        if (snapshot->provenance >= pipeline.concrete.operations.provenance_count) { ok = false; break; }
        baseline_local[side] = snapshot->local;
        baseline_provenance_expression[side] = pipeline.concrete.operations.provenance[
            snapshot->provenance].source_expression;
    }

    /* A/B paired authentication case:
     *
     * mutation                     predecessor   stale P3.6   resealed P3.6   backend
     * equal-recipe identity/slot   VALID         reject       VALID           accept
     *
     * Public builders reconstruct P1b through P3.6 from the valid mutated IR.
     * The old authentication is then installed to test A. `test_seal` only
     * refreshes that otherwise-valid owner for B; it repairs no row. */
    if (ok) {
        SolWasmRepresentedOutput stale, resealed, restored;
        sol_wasm_represented_output_init(&stale); sol_wasm_represented_output_init(&resealed);
        sol_wasm_represented_output_init(&restored);
        c2b1_swap_equal_snapshot_identities(&pipeline, &pair);
        bool rebuilt = sol_ir_validate(&pipeline.ir, NULL)
            && c2b1_rebuild_snapshot_downstream(&pipeline);
        SolMirOperations *mutated = &pipeline.concrete.operations;
        uint64_t mutated_authentication = pipeline.lowered.authentication;
        bool reassociated = rebuilt && mutated->snapshot_count == 2
            && mutated->snapshots[0].slot == 0 && mutated->snapshots[1].slot == 1
            && mutated->snapshots[0].local == baseline_local[1]
            && mutated->snapshots[1].local == baseline_local[0]
            && mutated->snapshots[0].provenance < mutated->provenance_count
            && mutated->snapshots[1].provenance < mutated->provenance_count
            && mutated->provenance[mutated->snapshots[0].provenance].source_expression
                == baseline_provenance_expression[1]
            && mutated->provenance[mutated->snapshots[1].provenance].source_expression
                == baseline_provenance_expression[0];
        pipeline.lowered.authentication = baseline_authentication;
        bool predecessor_valid = reassociated && c2b1_predecessors_valid(&pipeline);
        bool stale_rejected = predecessor_valid && baseline_authentication != mutated_authentication
            && !sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)
            && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                    directory, NULL}, &stale, &pipeline.diagnostics)
                == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE
            && stale.bytes.bytes == NULL && stale.bytes.count == 0 && usage_zero(&stale.usage);
        pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(
            &pipeline.lowered);
        char entry[256];
        bool resealed_accepted = c2b1_predecessors_valid(&pipeline)
            && pipeline.lowered.authentication == mutated_authentication
            && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)
            && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                    directory, NULL}, &resealed, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && usage_equal(&resealed.usage, &baseline.usage)
            && entry_symbol(&resealed.bytes, entry, sizeof entry)
            && invoke_named(&resealed.bytes, entry, 42, 0, 0);
        c2b1_swap_equal_snapshot_identities(&pipeline, &pair);
        bool restored_identical = sol_ir_validate(&pipeline.ir, NULL)
            && c2b1_rebuild_snapshot_downstream(&pipeline)
            && c2b1_predecessors_valid(&pipeline)
            && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)
            && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                    directory, NULL}, &restored, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && restored.bytes.count == baseline.bytes.count
            && memcmp(restored.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
            && usage_equal(&restored.usage, &baseline.usage);
        if (restored_identical) { uint8_t hash[32];
            sha256(restored.bytes.bytes, restored.bytes.count, hash);
            restored_identical = memcmp(hash, baseline_hash, sizeof hash) == 0
                && c2a_rebuilt_identity(directory, &baseline, baseline_hash); }
        ok = stale_rejected && resealed_accepted && restored_identical;
        sol_wasm_represented_output_free(&restored); sol_wasm_represented_output_free(&resealed);
        sol_wasm_represented_output_free(&stale);
    }

    /* No policy-reject B case is fabricated here.  The current public P1a
     * builders reject projected, exclusive, and non-scalar `old` sources before
     * an owner exists, while cross-context/image and nontrivial access/path/copy
     * edits fail the independent plan/operation validators.  Those forms can
     * therefore appear only in C below, not as backend-preflight evidence. */

    SolMirPlan *plan_owner = &pipeline.concrete.plan;
    SolMirMaterialization *materialization = &pipeline.concrete.materialization;
    SolMirOperations *operations = &pipeline.concrete.operations;
    SolMirRuntimeCleanup *cleanup = &pipeline.cleanup;
    SolMirRuntimeLoweredProgram *lowered = &pipeline.lowered;
    size_t snapshot = 0, instruction = SOL_MIR_RUNTIME_NONE, typed_use = SOL_MIR_RUNTIME_NONE;
    size_t input = SOL_MIR_RUNTIME_NONE, action0 = SOL_MIR_RUNTIME_NONE;
    size_t action1 = SOL_MIR_RUNTIME_NONE, transition_id = SOL_MIR_RUNTIME_NONE;
    if (ok) ok = operations->snapshot_count == 2;
    if (ok) instruction = operations->snapshots[snapshot].instruction;
    size_t source_snapshot = instruction < materialization->instruction_count
        ? materialization->instructions[instruction].source_snapshot : SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; ok && i < plan_owner->typed_use_count; ++i)
        if (plan_owner->typed_uses[i].kind == SOL_MIR_PLAN_USE_SNAPSHOT
            && plan_owner->typed_uses[i].source == source_snapshot) {
            if (typed_use != SOL_MIR_RUNTIME_NONE) ok = false;
            typed_use = i;
        }
    for (size_t i = 0; ok && i < operations->predicate_input_count; ++i)
        if (operations->predicate_inputs[i].kind == SOL_MIR_PREDICATE_INPUT_SNAPSHOT
            && operations->predicate_inputs[i].ordinal == 0) {
            if (input != SOL_MIR_RUNTIME_NONE) ok = false;
            input = i;
        }
    for (size_t i = 0; ok && i < cleanup->transition_count; ++i) {
        SolMirRuntimeCleanupTransition *candidate = &cleanup->transitions[i];
        if (candidate->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_RETURN
            || candidate->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_EXIT
            || candidate->actions.count != 2) continue;
        if (transition_id != SOL_MIR_RUNTIME_NONE) { ok = false; break; }
        transition_id = i; action0 = candidate->actions.offset; action1 = action0 + 1;
    }
    ok = ok && instruction < materialization->instruction_count
        && typed_use < plan_owner->typed_use_count && input < operations->predicate_input_count
        && action1 < cleanup->action_count && transition_id < cleanup->transition_count
        && cleanup->actions[action0].kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT
        && cleanup->actions[action1].kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT
        && cleanup->actions[action0].target == 1 && cleanup->actions[action1].target == 0
        && lowered->image_instructions[instruction].plan < lowered->semantic_plan_count;
    size_t semantic_id = ok ? lowered->image_instructions[instruction].plan : SOL_MIR_RUNTIME_NONE;

    /* C cases below are deliberately predecessor-invalid corruption tests.
     * Their own named validator must fail even after the outer owner is
     * resealed.  They are not counted as represented-backend policy coverage:
     *
     * layer                 corruptions
     * P2/operations         access, copy kind, slot, context, provenance, input
     * materialization       source snapshot; plan typed-use context
     * P3.6                  instruction/semantic facilities
     * P3.3 cleanup          target, recipe, flags, action slice, RETURN role */
#define C2B1_CORRUPTION_REJECT(predecessor, edit, restore) do { \
    SolWasmRepresentedOutput rejected, restored; sol_wasm_represented_output_init(&rejected); \
    sol_wasm_represented_output_init(&restored); edit; \
    lowered->authentication = sol_mir_runtime_lowered_program_test_seal(lowered); \
    bool rejected_ok = !(predecessor) && !sol_mir_runtime_lowered_program_validate(lowered, NULL) \
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){lowered, directory, NULL}, \
            &rejected, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE \
        && rejected.bytes.bytes == NULL && rejected.bytes.count == 0 && usage_zero(&rejected.usage); \
    restore; lowered->authentication = sol_mir_runtime_lowered_program_test_seal(lowered); \
    bool restored_ok = sol_mir_plan_validate(plan_owner, NULL) \
        && sol_mir_materialization_validate(materialization, NULL) \
        && sol_mir_operations_validate(operations, NULL) \
        && sol_mir_runtime_cleanup_validate(cleanup, NULL) \
        && sol_mir_runtime_lowered_program_validate(lowered, NULL) \
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){lowered, directory, NULL}, \
            &restored, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK \
        && restored.bytes.count == baseline.bytes.count \
        && memcmp(restored.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0 \
        && usage_equal(&restored.usage, &baseline.usage); \
    if (restored_ok) { uint8_t hash[32]; sha256(restored.bytes.bytes, restored.bytes.count, hash); \
        restored_ok = memcmp(hash, baseline_hash, sizeof hash) == 0; } \
    ok = ok && rejected_ok && restored_ok; sol_wasm_represented_output_free(&restored); \
    sol_wasm_represented_output_free(&rejected); \
} while (0)
    if (ok) {
        SolMirOperationSnapshotPlan *item = &operations->snapshots[snapshot];
        SolMirOperationAccessId access = item->access;
        C2B1_CORRUPTION_REJECT(sol_mir_operations_validate(operations, NULL),
            item->access = 0, item->access = access);
        SolMirCopyKind copy = item->copy_kind;
        C2B1_CORRUPTION_REJECT(sol_mir_operations_validate(operations, NULL),
            item->copy_kind = SOL_MIR_COPY_TEXT, item->copy_kind = copy);
        size_t slot = item->slot;
        C2B1_CORRUPTION_REJECT(sol_mir_operations_validate(operations, NULL),
            item->slot = 1, item->slot = slot);
        SolMirPlanContextId context = item->context;
        C2B1_CORRUPTION_REJECT(sol_mir_operations_validate(operations, NULL),
            item->context = 0, item->context = context);
        size_t provenance = item->provenance;
        C2B1_CORRUPTION_REJECT(sol_mir_operations_validate(operations, NULL),
            item->provenance = operations->provenance_count, item->provenance = provenance);
        SolIrSnapshotId source = materialization->instructions[instruction].source_snapshot;
        C2B1_CORRUPTION_REJECT(sol_mir_materialization_validate(materialization, NULL),
            materialization->instructions[instruction].source_snapshot = 1,
            materialization->instructions[instruction].source_snapshot = source);
        SolMirPlanContextId use_context = plan_owner->typed_uses[typed_use].context;
        C2B1_CORRUPTION_REJECT(sol_mir_plan_validate(plan_owner, NULL),
            plan_owner->typed_uses[typed_use].context = 0,
            plan_owner->typed_uses[typed_use].context = use_context);
        size_t ordinal = operations->predicate_inputs[input].ordinal;
        C2B1_CORRUPTION_REJECT(sol_mir_operations_validate(operations, NULL),
            operations->predicate_inputs[input].ordinal = ordinal + 2,
            operations->predicate_inputs[input].ordinal = ordinal);
        uint32_t facilities = lowered->image_instructions[instruction].facilities;
        C2B1_CORRUPTION_REJECT(sol_mir_runtime_lowered_program_validate(lowered, NULL),
            lowered->image_instructions[instruction].facilities = 0,
            lowered->image_instructions[instruction].facilities = facilities);
        facilities = lowered->semantic_plans[semantic_id].facilities;
        C2B1_CORRUPTION_REJECT(sol_mir_runtime_lowered_program_validate(lowered, NULL),
            lowered->semantic_plans[semantic_id].facilities = SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE,
            lowered->semantic_plans[semantic_id].facilities = facilities);
        size_t target = cleanup->actions[action0].target;
        C2B1_CORRUPTION_REJECT(sol_mir_runtime_cleanup_validate(cleanup, NULL),
            cleanup->actions[action0].target = cleanup->actions[action1].target,
            cleanup->actions[action0].target = target);
        SolMirRecipeId action_recipe = cleanup->actions[action0].recipe;
        C2B1_CORRUPTION_REJECT(sol_mir_runtime_cleanup_validate(cleanup, NULL),
            cleanup->actions[action0].recipe = SOL_MIR_RECIPE_NONE,
            cleanup->actions[action0].recipe = action_recipe);
        unsigned action_flags = cleanup->actions[action0].flags;
        C2B1_CORRUPTION_REJECT(sol_mir_runtime_cleanup_validate(cleanup, NULL),
            cleanup->actions[action0].flags = SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED,
            cleanup->actions[action0].flags = action_flags);
        size_t action_count = cleanup->transitions[transition_id].actions.count;
        C2B1_CORRUPTION_REJECT(sol_mir_runtime_cleanup_validate(cleanup, NULL),
            cleanup->transitions[transition_id].actions.count = action_count - 1,
            cleanup->transitions[transition_id].actions.count = action_count);
        SolMirRuntimeCleanupEdgeRole role = cleanup->transitions[transition_id].edge_role;
        C2B1_CORRUPTION_REJECT(sol_mir_runtime_cleanup_validate(cleanup, NULL),
            cleanup->transitions[transition_id].edge_role = SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO,
            cleanup->transitions[transition_id].edge_role = role);
    }
#undef C2B1_CORRUPTION_REJECT
    sol_wasm_represented_output_free(&baseline); propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44c_qualified_snapshot_owner_permutation(const char *directory) {
    PropagationPipeline pipeline; SolWasmRepresentedOutput baseline;
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&baseline);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &baseline, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
    uint8_t baseline_hash[32] = {0}; if (ok) sha256(baseline.bytes.bytes,
        baseline.bytes.count, baseline_hash);
    C2b1SnapshotIdentityPair pair; if (ok) ok = c2b1_equal_snapshot_pair(&pipeline, &pair);
    uint64_t baseline_authentication = pipeline.lowered.authentication;
    SolMirMaterializedLocalId baseline_local[2];
    SolIrExpressionId baseline_expression[2];
    for (size_t side = 0; ok && side < 2; ++side) {
        const SolMirOperationSnapshotPlan *snapshot = &pipeline.concrete.operations.snapshots[side];
        if (snapshot->provenance >= pipeline.concrete.operations.provenance_count) { ok = false; break; }
        baseline_local[side] = snapshot->local;
        baseline_expression[side] = pipeline.concrete.operations.provenance[
            snapshot->provenance].source_expression;
    }
    if (ok) {
        SolWasmRepresentedOutput stale, resealed, restored;
        sol_wasm_represented_output_init(&stale); sol_wasm_represented_output_init(&resealed);
        sol_wasm_represented_output_init(&restored);
        c2b1_swap_equal_snapshot_identities(&pipeline, &pair);
        bool rebuilt = sol_ir_validate(&pipeline.ir, NULL)
            && c2b1_rebuild_snapshot_downstream(&pipeline);
        SolMirOperations *operations = &pipeline.concrete.operations;
        uint64_t rebuilt_authentication = pipeline.lowered.authentication;
        bool permuted = rebuilt && operations->snapshot_count == 2
            && operations->snapshots[0].slot == 0 && operations->snapshots[1].slot == 1
            && operations->snapshots[0].local == baseline_local[1]
            && operations->snapshots[1].local == baseline_local[0]
            && operations->snapshots[0].provenance < operations->provenance_count
            && operations->snapshots[1].provenance < operations->provenance_count
            && operations->provenance[operations->snapshots[0].provenance].source_expression
                == baseline_expression[1]
            && operations->provenance[operations->snapshots[1].provenance].source_expression
                == baseline_expression[0];
        pipeline.lowered.authentication = baseline_authentication;
        bool stale_ok = permuted && baseline_authentication != rebuilt_authentication
            && c2b1_predecessors_valid(&pipeline)
            && !sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)
            && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                    directory, NULL}, &stale, &pipeline.diagnostics)
                == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE
            && stale.bytes.bytes == NULL && usage_zero(&stale.usage);
        pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(
            &pipeline.lowered);
        char entry[256];
        bool resealed_ok = pipeline.lowered.authentication == rebuilt_authentication
            && c2b1_predecessors_valid(&pipeline)
            && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)
            && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                    directory, NULL}, &resealed, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && entry_symbol(&resealed.bytes, entry, sizeof entry)
            && invoke_named(&resealed.bytes, entry, 0, 14, 4);
        c2b1_swap_equal_snapshot_identities(&pipeline, &pair);
        bool restored_ok = sol_ir_validate(&pipeline.ir, NULL)
            && c2b1_rebuild_snapshot_downstream(&pipeline)
            && c2b1_predecessors_valid(&pipeline)
            && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)
            && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                    directory, NULL}, &restored, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && restored.bytes.count == baseline.bytes.count
            && memcmp(restored.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
            && usage_equal(&restored.usage, &baseline.usage);
        if (restored_ok) { uint8_t hash[32]; sha256(restored.bytes.bytes, restored.bytes.count, hash);
            restored_ok = memcmp(hash, baseline_hash, sizeof hash) == 0; }
        ok = stale_ok && resealed_ok && restored_ok;
        sol_wasm_represented_output_free(&restored); sol_wasm_represented_output_free(&resealed);
        sol_wasm_represented_output_free(&stale);
    }
    sol_wasm_represented_output_free(&baseline); propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44c_old_many_shape(const char *directory) {
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false);
    const SolMirOperations *operations = &pipeline.concrete.operations;
    unsigned char images[10] = {0};
    if (ok) ok = operations->snapshot_count == 10;
    for (size_t i = 0; ok && i < operations->snapshot_count; ++i) {
        const SolMirOperationSnapshotPlan *plan = &operations->snapshots[i];
        if (plan->image >= sizeof images || plan->slot != 0 || plan->access != SOL_MIR_OPERATION_NONE
            || plan->path.count != 0 || plan->copy_kind != SOL_MIR_COPY_TRIVIAL
            || images[plan->image] != 0) ok = false;
        else images[plan->image] = 1;
    }
    for (size_t i = 0; ok && i < sizeof images; ++i) ok = images[i] == 1;
    propagation_pipeline_free(&pipeline); return ok;
}

/* Snapshot-owning images reserve one i64 value and one i32 initialized flag
 * per plan plus one shared staged-return i64.  Paired source shapes isolate
 * that exact 2n+1 delta for one, two, and three captures. */
static bool p44c_old_local_delta(const char *old_directory, const char *current_directory,
    size_t expected_delta, int64_t value) {
    SolWasmRepresentedOutput old, current; char entry[256];
    sol_wasm_represented_output_init(&old); sol_wasm_represented_output_init(&current);
    bool ok = build_named_root(old_directory, "launch", &old, NULL, SOL_WASM_REPRESENTED_OK)
        && build_named_root(current_directory, "launch", &current, NULL, SOL_WASM_REPRESENTED_OK)
        && old.usage.locals == current.usage.locals + expected_delta
        && entry_symbol(&old.bytes, entry, sizeof entry)
        && invoke_named(&old.bytes, entry, value, 0, 0);
    sol_wasm_represented_output_free(&current); sol_wasm_represented_output_free(&old); return ok;
}

static bool p44c_old_resource_determinism(const char *directory, const char *current_directory) {
    static const SolWasmRepresentedUsage expected = {6,9,6,13,51,1084,0,0,0,0,44,26868,10240,18584,8344};
    static const uint8_t expected_hash[32] = {0xd9,0xc1,0x3d,0x0d,0x4f,0xc6,0x91,0xbd,0x27,0xb0,0x97,0xdf,0x9d,0xb9,0x18,0x7f,0x09,0xa1,0xb3,0x2d,0x65,0x4b,0xd0,0x1b,0x9f,0x16,0xbb,0x09,0xa8,0x58,0x51,0x0b};
    SolWasmRepresentedOutput baseline, repeat, output, current, relocated;
    sol_wasm_represented_output_init(&baseline); sol_wasm_represented_output_init(&repeat);
    sol_wasm_represented_output_init(&output); sol_wasm_represented_output_init(&current);
    sol_wasm_represented_output_init(&relocated);
    sol_wasm_represented_test_fail_allocation_after(0);
    bool ok = build_named_root(directory, "launch", &baseline, NULL, SOL_WASM_REPRESENTED_OK);
    size_t attempts = sol_wasm_represented_test_allocation_attempts(); uint8_t hash[32]; char entry[256];
    if (ok) { sha256(baseline.bytes.bytes, baseline.bytes.count, hash);
        ok = usage_equal(&baseline.usage, &expected) && attempts == 79
            && memcmp(hash, expected_hash, sizeof hash) == 0
            && sol_wasm_represented_validate(&baseline.bytes) == SOL_WASM_REPRESENTED_OK
            && entry_symbol(&baseline.bytes, entry, sizeof entry)
            && invoke_named(&baseline.bytes, entry, 43, 0, 0); }
    if (ok) ok = build_named_root(directory, "launch", &repeat, NULL, SOL_WASM_REPRESENTED_OK)
        && repeat.bytes.count == baseline.bytes.count
        && memcmp(repeat.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
        && usage_equal(&repeat.usage, &expected);
    if (ok) ok = build_named_root(current_directory, "launch", &current, NULL,
            SOL_WASM_REPRESENTED_OK) && baseline.usage.locals == current.usage.locals + 3;
    SolWasmRepresentedLimits exact = sol_wasm_represented_default_limits();
    exact.max_functions = expected.functions; exact.max_blocks = expected.blocks;
    exact.max_edges = expected.edges; exact.max_values = expected.values;
    exact.max_locals = expected.locals; exact.max_generated_nodes = expected.generated_nodes;
    exact.max_provenance_records = expected.provenance_records;
    exact.max_work_bytes = expected.work_bytes; exact.max_scratch_bytes = expected.scratch_bytes;
    exact.max_owned_bytes = expected.owned_bytes; exact.max_output_bytes = expected.output_bytes;
    if (ok) ok = build_named_root(directory, "launch", &output, &exact, SOL_WASM_REPRESENTED_OK)
        && output.bytes.count == baseline.bytes.count
        && memcmp(output.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
        && usage_equal(&output.usage, &expected);
    sol_wasm_represented_output_free(&output);
#define CHECK_C2B1_RESOURCE_CAP(field, value) do { \
    SolWasmRepresentedLimits cap = sol_wasm_represented_default_limits(); cap.field = (value) - 1; \
    sol_wasm_represented_output_init(&output); \
    ok = ok && build_named_root(directory, "launch", &output, &cap, \
        SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED) && output.bytes.bytes == NULL \
        && output.bytes.count == 0 && usage_zero(&output.usage); \
    sol_wasm_represented_output_free(&output); \
} while (0)
    CHECK_C2B1_RESOURCE_CAP(max_functions, expected.functions);
    CHECK_C2B1_RESOURCE_CAP(max_blocks, expected.blocks);
    CHECK_C2B1_RESOURCE_CAP(max_edges, expected.edges);
    CHECK_C2B1_RESOURCE_CAP(max_values, expected.values);
    CHECK_C2B1_RESOURCE_CAP(max_locals, expected.locals);
    CHECK_C2B1_RESOURCE_CAP(max_generated_nodes, expected.generated_nodes);
    CHECK_C2B1_RESOURCE_CAP(max_provenance_records, expected.provenance_records);
    CHECK_C2B1_RESOURCE_CAP(max_work_bytes, expected.work_bytes);
    CHECK_C2B1_RESOURCE_CAP(max_scratch_bytes, expected.scratch_bytes);
    CHECK_C2B1_RESOURCE_CAP(max_owned_bytes, expected.owned_bytes);
    CHECK_C2B1_RESOURCE_CAP(max_output_bytes, expected.output_bytes);
#undef CHECK_C2B1_RESOURCE_CAP
    for (size_t ordinal = 1; ok && ordinal <= attempts; ++ordinal) {
        represented_fault(FAULT_OLD_SNAPSHOT, ordinal, ordinal);
        sol_wasm_represented_output_init(&output);
        ok = build_named_root(directory, "launch", &output, NULL,
                SOL_WASM_REPRESENTED_ALLOCATION_FAILED)
            && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage);
        sol_wasm_represented_output_free(&output);
        sol_wasm_represented_test_fail_allocation_after(0);
        sol_wasm_represented_output_init(&output);
        if (ok) ok = build_named_root(directory, "launch", &output, NULL,
                SOL_WASM_REPRESENTED_OK) && output.bytes.count == baseline.bytes.count
            && memcmp(output.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
            && usage_equal(&output.usage, &expected)
            && sol_wasm_represented_test_allocation_attempts() == attempts;
        sol_wasm_represented_output_free(&output);
    }
    sol_wasm_represented_test_fail_allocation_after(0);
    sol_wasm_represented_output_init(&output);
    if (ok) ok = build_named_root(directory, "launch", &output, NULL, SOL_WASM_REPRESENTED_OK)
        && output.bytes.count == baseline.bytes.count
        && memcmp(output.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
        && usage_equal(&output.usage, &expected)
        && sol_wasm_represented_test_allocation_attempts() == attempts;
    sol_wasm_represented_output_free(&output);
    char relocation[512], source[768], destination[768];
    (void)mkdir(SOL_TEST_BINARY_DIR, 0700);
    (void)snprintf(relocation, sizeof relocation, "%s/p44c_ensures_old_relocated", SOL_TEST_BINARY_DIR);
    (void)mkdir(relocation, 0700);
    (void)snprintf(source, sizeof source, "%s/main.sol", directory);
    (void)snprintf(destination, sizeof destination, "%s/main.sol", relocation);
    FILE *input_file = fopen(source, "rb"), *copy = fopen(destination, "wb");
    if (input_file == NULL || copy == NULL) ok = false;
    if (input_file != NULL && copy != NULL) {
        uint8_t buffer[256]; size_t count = 0;
        while ((count = fread(buffer, 1, sizeof buffer, input_file)) != 0)
            if (fwrite(buffer, 1, count, copy) != count) ok = false;
    }
    if (input_file != NULL) fclose(input_file); if (copy != NULL) fclose(copy);
    if (ok) ok = build_named_root(relocation, "launch", &relocated, NULL, SOL_WASM_REPRESENTED_OK)
        && relocated.bytes.count == baseline.bytes.count
        && memcmp(relocated.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
        && usage_equal(&relocated.usage, &expected) && invoke_named(&relocated.bytes, entry, 43, 0, 0);
    sol_wasm_represented_output_free(&relocated); sol_wasm_represented_output_free(&current);
    sol_wasm_represented_output_free(&output); sol_wasm_represented_output_free(&repeat);
    sol_wasm_represented_output_free(&baseline); return ok;
}

static bool p44c_old_root_order_determinism(const char *directory) {
    SolWasmRepresentedOutput forward, reverse, repeat; size_t a[2], b[2], c[2];
    sol_wasm_represented_output_init(&forward); sol_wasm_represented_output_init(&reverse);
    sol_wasm_represented_output_init(&repeat);
    bool ok = build_multiroot(directory, false, &forward, a, NULL, SOL_WASM_REPRESENTED_OK)
        && build_multiroot(directory, true, &reverse, b, NULL, SOL_WASM_REPRESENTED_OK)
        && build_multiroot(directory, false, &repeat, c, NULL, SOL_WASM_REPRESENTED_OK)
        && a[0] == b[0] && a[1] == b[1] && a[0] == c[0] && a[1] == c[1]
        && forward.bytes.count == reverse.bytes.count && forward.bytes.count == repeat.bytes.count
        && memcmp(forward.bytes.bytes, reverse.bytes.bytes, forward.bytes.count) == 0
        && memcmp(forward.bytes.bytes, repeat.bytes.bytes, forward.bytes.count) == 0
        && usage_equal(&forward.usage, &reverse.usage) && usage_equal(&forward.usage, &repeat.usage);
    char entry[256];
    if (ok) ok = entry_symbol(&forward.bytes, entry, sizeof entry)
        && invoke_named(&forward.bytes, entry, 42, 0, 0);
    sol_wasm_represented_output_free(&repeat); sol_wasm_represented_output_free(&reverse);
    sol_wasm_represented_output_free(&forward); return ok;
}

static bool c2a_rebuilt_identity(const char *directory,
    const SolWasmRepresentedOutput *baseline, const uint8_t expected_hash[32]) {
    PropagationPipeline rebuilt; SolWasmRepresentedOutput output; uint8_t hash[32];
    propagation_pipeline_init(&rebuilt); sol_wasm_represented_output_init(&output);
    bool ok = propagation_pipeline_build_named(&rebuilt, directory, "launch", false)
        && sol_mir_operations_validate(&rebuilt.concrete.operations, NULL)
        && sol_mir_materialization_validate(&rebuilt.concrete.materialization, NULL)
        && sol_mir_runtime_conventions_validate(&rebuilt.conventions, NULL)
        && sol_mir_runtime_cleanup_validate(&rebuilt.cleanup, NULL)
        && sol_mir_runtime_lowered_program_validate(&rebuilt.lowered, NULL)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&rebuilt.lowered,
                directory, NULL}, &output, &rebuilt.diagnostics) == SOL_WASM_REPRESENTED_OK
        && output.bytes.count == baseline->bytes.count
        && memcmp(output.bytes.bytes, baseline->bytes.bytes, baseline->bytes.count) == 0
        && usage_equal(&output.usage, &baseline->usage);
    if (ok) { sha256(output.bytes.bytes, output.bytes.count, hash);
        ok = memcmp(hash, expected_hash, sizeof hash) == 0; }
    sol_wasm_represented_output_free(&output); propagation_pipeline_free(&rebuilt); return ok;
}

static bool p44c_ensures_predecessor_mutations(const char *directory) {
    PropagationPipeline pipeline; SolWasmRepresentedOutput baseline;
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&baseline);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false);
    SolMirOperations *operations = &pipeline.concrete.operations;
    SolMirMaterialization *materialization = &pipeline.concrete.materialization;
    SolMirRuntimeCleanup *cleanup = &pipeline.cleanup;
    SolMirRuntimeConventions *conventions = &pipeline.conventions;
    size_t body_id = SOL_MIR_RUNTIME_NONE, plan_id = SOL_MIR_RUNTIME_NONE, input_id = SOL_MIR_RUNTIME_NONE;
    size_t bodies = 0, plans = 0, inputs = 0;
    for (size_t i = 0; ok && i < operations->predicate_body_count; ++i)
        if (operations->predicate_bodies[i].phase == SOL_CONTRACT_ENSURES) {
            body_id = i; ++bodies;
        }
    for (size_t i = 0; ok && i < operations->predicate_count; ++i)
        if (operations->predicates[i].kind == SOL_MIR_OPERATION_PREDICATE_CONTRACT
            && operations->predicates[i].body == body_id) { plan_id = i; ++plans; }
    if (ok && body_id < operations->predicate_body_count) {
        const SolMirPredicateBody *body = &operations->predicate_bodies[body_id];
        if (body->inputs.offset > operations->predicate_input_count || body->inputs.count
            > operations->predicate_input_count - body->inputs.offset) ok = false;
        for (size_t i = ok ? body->inputs.offset : 0;
            ok && i < body->inputs.offset + body->inputs.count; ++i)
            if (operations->predicate_inputs[i].kind == SOL_MIR_PREDICATE_INPUT_COMPLETE_RESULT) {
                input_id = i; ++inputs;
            }
    }
    ok = ok && bodies == 1 && plans == 1 && inputs == 1 && plan_id < operations->predicate_count;
    SolMirOperationPredicatePlan *plan = ok ? &operations->predicates[plan_id] : NULL;
    SolMirMaterializedTerminator *check = plan != NULL && plan->block < materialization->block_count
        ? &materialization->blocks[plan->block].terminator : NULL;
    SolMirRuntimeLoweredImageTerminator *row = plan != NULL && plan->block
            < pipeline.lowered.image_terminator_count
        ? &pipeline.lowered.image_terminators[plan->block] : NULL;
    SolMirRuntimeLoweredSemanticPlan *semantic = row != NULL && row->plan
            < pipeline.lowered.semantic_plan_count
        ? &pipeline.lowered.semantic_plans[row->plan] : NULL;
    SolMirRuntimeCleanupTransition *violation = NULL; size_t violations = 0;
    SolMirRuntimeCleanupEvent *event = row != NULL && row->cleanup_event < cleanup->event_count
        ? &cleanup->events[row->cleanup_event] : NULL;
    if (event != NULL && event->transitions.offset <= cleanup->transition_count
        && event->transitions.count <= cleanup->transition_count - event->transitions.offset)
        for (size_t i = 0; i < event->transitions.count; ++i) {
            SolMirRuntimeCleanupTransition *candidate = &cleanup->transitions[
                event->transitions.offset + i];
            if (candidate->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION) {
                violation = candidate; ++violations;
            }
        }
    SolMirRuntimeFailureSite *site = violation != NULL
            && violation->failure_site < conventions->failure_site_count
        ? &conventions->failure_sites[violation->failure_site] : NULL;
    ok = ok && check != NULL && row != NULL && semantic != NULL && event != NULL
        && violations == 1 && site != NULL;
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, NULL}, &baseline, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
    uint8_t baseline_hash[32] = {0};
    if (ok) sha256(baseline.bytes.bytes, baseline.bytes.count, baseline_hash);
/* These are deliberately predecessor mutations, not represented-policy cases.
 * The named independent validator must reject first; resealing P3.6 proves the
 * backend result is not caused by stale authentication.  A fresh public-builder
 * pipeline after every case must reproduce the baseline bytes, usage, and hash. */
#define C2A_PREDECESSOR_REJECT(predecessor, edit, restore) do { \
    SolWasmRepresentedOutput rejected, restored; sol_wasm_represented_output_init(&rejected); \
    sol_wasm_represented_output_init(&restored); edit; \
    pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
    bool rejected_ok = !(predecessor) && !sol_mir_runtime_lowered_program_validate( \
            &pipeline.lowered, NULL) \
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){ \
            &pipeline.lowered, directory, NULL}, &rejected, &pipeline.diagnostics) \
            == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE \
        && rejected.bytes.bytes == NULL && rejected.bytes.count == 0 && usage_zero(&rejected.usage); \
    restore; pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
    bool restored_ok = sol_mir_operations_validate(operations, NULL) \
        && sol_mir_materialization_validate(materialization, NULL) \
        && sol_mir_runtime_conventions_validate(conventions, NULL) \
        && sol_mir_runtime_cleanup_validate(cleanup, NULL) \
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL) \
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, \
            &restored, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK \
        && restored.bytes.count == baseline.bytes.count \
        && memcmp(restored.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0 \
        && usage_equal(&restored.usage, &baseline.usage); \
    if (restored_ok) { uint8_t restored_hash[32]; sha256(restored.bytes.bytes, \
        restored.bytes.count, restored_hash); restored_ok = memcmp(restored_hash, baseline_hash, \
            sizeof baseline_hash) == 0 && c2a_rebuilt_identity(directory, &baseline, baseline_hash); } \
    ok = ok && rejected_ok && restored_ok; sol_wasm_represented_output_free(&restored); \
    sol_wasm_represented_output_free(&rejected); \
} while (0)
    if (ok) {
        SolMirPredicateInput *input = &operations->predicate_inputs[input_id];
        SolAccessMode access = input->access;
        C2A_PREDECESSOR_REJECT(sol_mir_operations_validate(operations, NULL),
            input->access = SOL_ACCESS_SHARED, input->access = access);
        SolMirPredicateInputKind kind = input->kind;
        C2A_PREDECESSOR_REJECT(sol_mir_operations_validate(operations, NULL),
            input->kind = SOL_MIR_PREDICATE_INPUT_SUCCESS_RESULT, input->kind = kind);
        size_t ordinal = input->ordinal;
        C2A_PREDECESSOR_REJECT(sol_mir_operations_validate(operations, NULL),
            input->ordinal = 1, input->ordinal = ordinal);
        SolMirPredicateBody *body = &operations->predicate_bodies[body_id];
        SolMirPredicateOwnerKind owner_kind = body->owner_kind;
        C2A_PREDECESSOR_REJECT(sol_mir_operations_validate(operations, NULL),
            body->owner_kind = SOL_MIR_PREDICATE_OWNER_IMPORT, body->owner_kind = owner_kind);
        SolMirMaterializedImportId import_id = body->import;
        C2A_PREDECESSOR_REJECT(sol_mir_operations_validate(operations, NULL),
            body->import = 0, body->import = import_id);
        SolMirPlanInstanceId instance = body->instance;
        C2A_PREDECESSOR_REJECT(sol_mir_operations_validate(operations, NULL),
            body->instance = SOL_MIR_RUNTIME_NONE, body->instance = instance);
        SolMirRuntimeLoweredPredicateBody *body_row = &pipeline.lowered.predicate_bodies[body_id];
        owner_kind = body_row->owner_kind;
        C2A_PREDECESSOR_REJECT(sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL),
            body_row->owner_kind = SOL_MIR_PREDICATE_OWNER_IMPORT,
            body_row->owner_kind = owner_kind);
        SolMirPlanContextId context = body_row->context;
        C2A_PREDECESSOR_REJECT(sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL),
            body_row->context = SOL_MIR_RUNTIME_NONE, body_row->context = context);
        SolMirMaterializedValueId result = plan->result;
        C2A_PREDECESSOR_REJECT(sol_mir_operations_validate(operations, NULL),
            plan->result = SOL_MIR_MATERIALIZED_NONE, plan->result = result);
        SolMirRecipeId recipe = plan->result_recipe;
        C2A_PREDECESSOR_REJECT(sol_mir_operations_validate(operations, NULL),
            plan->result_recipe = SOL_MIR_RECIPE_NONE, plan->result_recipe = recipe);
        SolContractClauseKind phase = plan->contract_phase;
        C2A_PREDECESSOR_REJECT(sol_mir_operations_validate(operations, NULL),
            plan->contract_phase = SOL_CONTRACT_REQUIRES, plan->contract_phase = phase);
        SolContractOutcomeKind outcome = plan->contract_outcome;
        C2A_PREDECESSOR_REJECT(sol_mir_operations_validate(operations, NULL),
            plan->contract_outcome = SOL_CONTRACT_OUTCOME_SUCCESS,
            plan->contract_outcome = outcome);
        size_t block = plan->block;
        size_t alternate_block = materialization->block_count == 0 ? SOL_MIR_MATERIALIZED_NONE
            : (block + 1) % materialization->block_count;
        C2A_PREDECESSOR_REJECT(sol_mir_operations_validate(operations, NULL),
            plan->block = alternate_block, plan->block = block);
        result = check->result;
        C2A_PREDECESSOR_REJECT(sol_mir_materialization_validate(materialization, NULL),
            check->result = SOL_MIR_MATERIALIZED_NONE, check->result = result);
        SolMirMaterializedProducerKind producer_kind = semantic->producer_kind;
        C2A_PREDECESSOR_REJECT(sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL),
            semantic->producer_kind = SOL_MIR_MATERIALIZED_PRODUCER_TERMINATOR,
            semantic->producer_kind = producer_kind);
        uint32_t mask = violation->failure_mask;
        C2A_PREDECESSOR_REJECT(sol_mir_runtime_cleanup_validate(cleanup, NULL),
            violation->failure_mask = UINT32_C(0x1000), violation->failure_mask = mask);
        size_t owner = site->owner;
        C2A_PREDECESSOR_REJECT(sol_mir_runtime_conventions_validate(conventions, NULL),
            site->owner = body_id + 1, site->owner = owner);
    }
#undef C2A_PREDECESSOR_REJECT
    sol_wasm_represented_output_free(&baseline); propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44c_ensures_resource_determinism(const char *directory) {
    static const SolWasmRepresentedUsage expected = {6,12,8,10,42,788,0,0,0,0,33,23356,10240,16459,6219};
    static const uint8_t expected_hash[32] = {0x6a,0x2d,0x0d,0xd0,0xb2,0x75,0xf6,0x9a,0x87,0x31,0x20,0x61,0x38,0xe3,0x99,0xe7,0x8f,0x68,0xa5,0x0d,0x9f,0x65,0xd4,0xc4,0x0b,0x58,0x84,0x2f,0xc5,0x34,0xb4,0x01};
    SolWasmRepresentedOutput baseline, reverse, repeat, relocated, output;
    size_t baseline_ids[2], reverse_ids[2], repeat_ids[2], relocated_ids[2];
    uint8_t hash[32]; char entry[256], relocation[512], source[768], destination[768];
    sol_wasm_represented_output_init(&baseline); sol_wasm_represented_output_init(&reverse);
    sol_wasm_represented_output_init(&repeat); sol_wasm_represented_output_init(&relocated);
    sol_wasm_represented_output_init(&output);
    sol_wasm_represented_test_fail_allocation_after(0);
    bool ok = build_multiroot(directory, false, &baseline, baseline_ids, NULL,
            SOL_WASM_REPRESENTED_OK);
    size_t attempts = sol_wasm_represented_test_allocation_attempts();
    if (ok) {
        sha256(baseline.bytes.bytes, baseline.bytes.count, hash);
        ok = baseline_ids[0] == 0 && baseline_ids[1] == 1
            && usage_equal(&baseline.usage, &expected) && attempts == 67
            && memcmp(hash, expected_hash, sizeof hash) == 0
            && sol_wasm_represented_validate(&baseline.bytes) == SOL_WASM_REPRESENTED_OK
            && entry_symbol(&baseline.bytes, entry, sizeof entry)
            && invoke_named(&baseline.bytes, entry, 42, 0, 0);
    }
    if (ok) ok = build_multiroot(directory, true, &reverse, reverse_ids, NULL,
            SOL_WASM_REPRESENTED_OK)
        && build_multiroot(directory, false, &repeat, repeat_ids, NULL,
            SOL_WASM_REPRESENTED_OK)
        && reverse_ids[0] == baseline_ids[0] && reverse_ids[1] == baseline_ids[1]
        && repeat_ids[0] == baseline_ids[0] && repeat_ids[1] == baseline_ids[1]
        && reverse.bytes.count == baseline.bytes.count
        && repeat.bytes.count == baseline.bytes.count
        && memcmp(reverse.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
        && memcmp(repeat.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
        && usage_equal(&reverse.usage, &expected) && usage_equal(&repeat.usage, &expected);
    SolWasmRepresentedLimits exact = sol_wasm_represented_default_limits();
    exact.max_functions = expected.functions; exact.max_blocks = expected.blocks;
    exact.max_edges = expected.edges; exact.max_values = expected.values;
    exact.max_locals = expected.locals; exact.max_generated_nodes = expected.generated_nodes;
    exact.max_provenance_records = expected.provenance_records;
    exact.max_work_bytes = expected.work_bytes; exact.max_scratch_bytes = expected.scratch_bytes;
    exact.max_owned_bytes = expected.owned_bytes; exact.max_output_bytes = expected.output_bytes;
    if (ok) ok = build_multiroot(directory, false, &output, repeat_ids, &exact,
            SOL_WASM_REPRESENTED_OK) && output.bytes.count == baseline.bytes.count
        && memcmp(output.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
        && usage_equal(&output.usage, &expected);
    sol_wasm_represented_output_free(&output);
#define CHECK_C2A_RESOURCE_CAP(field, value) do { \
    SolWasmRepresentedLimits cap = sol_wasm_represented_default_limits(); cap.field = (value); \
    sol_wasm_represented_output_init(&output); \
    ok = ok && build_multiroot(directory, false, &output, repeat_ids, &cap, \
        SOL_WASM_REPRESENTED_OK) && output.bytes.count == baseline.bytes.count \
        && memcmp(output.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0 \
        && usage_equal(&output.usage, &expected); sol_wasm_represented_output_free(&output); \
    cap.field = (value) - 1; sol_wasm_represented_output_init(&output); \
    ok = ok && build_multiroot(directory, false, &output, repeat_ids, &cap, \
        SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED) && output.bytes.bytes == NULL \
        && output.bytes.count == 0 && usage_zero(&output.usage); \
    sol_wasm_represented_output_free(&output); \
} while (0)
    CHECK_C2A_RESOURCE_CAP(max_functions, expected.functions);
    CHECK_C2A_RESOURCE_CAP(max_blocks, expected.blocks);
    CHECK_C2A_RESOURCE_CAP(max_edges, expected.edges);
    CHECK_C2A_RESOURCE_CAP(max_values, expected.values);
    CHECK_C2A_RESOURCE_CAP(max_locals, expected.locals);
    CHECK_C2A_RESOURCE_CAP(max_generated_nodes, expected.generated_nodes);
    CHECK_C2A_RESOURCE_CAP(max_provenance_records, expected.provenance_records);
    CHECK_C2A_RESOURCE_CAP(max_work_bytes, expected.work_bytes);
    CHECK_C2A_RESOURCE_CAP(max_scratch_bytes, expected.scratch_bytes);
    CHECK_C2A_RESOURCE_CAP(max_owned_bytes, expected.owned_bytes);
    CHECK_C2A_RESOURCE_CAP(max_output_bytes, expected.output_bytes);
#undef CHECK_C2A_RESOURCE_CAP
    for (size_t ordinal = 1; ok && ordinal <= attempts; ++ordinal) {
        represented_fault(FAULT_ENSURES_ROOTS, ordinal, ordinal);
        sol_wasm_represented_output_init(&output);
        ok = build_multiroot(directory, false, &output, repeat_ids, NULL,
                SOL_WASM_REPRESENTED_ALLOCATION_FAILED)
            && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage);
        sol_wasm_represented_output_free(&output);
        sol_wasm_represented_test_fail_allocation_after(0);
        sol_wasm_represented_output_init(&output);
        if (ok) ok = build_multiroot(directory, false, &output, repeat_ids, NULL,
                SOL_WASM_REPRESENTED_OK) && output.bytes.count == baseline.bytes.count
            && memcmp(output.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
            && usage_equal(&output.usage, &expected)
            && sol_wasm_represented_test_allocation_attempts() == attempts;
        sol_wasm_represented_output_free(&output);
    }
    sol_wasm_represented_test_fail_allocation_after(0);
    (void)mkdir(SOL_TEST_BINARY_DIR, 0700);
    (void)snprintf(relocation, sizeof relocation, "%s/p44c_ensures_roots_relocated",
        SOL_TEST_BINARY_DIR);
    (void)mkdir(relocation, 0700);
    (void)snprintf(source, sizeof source, "%s/main.sol", directory);
    (void)snprintf(destination, sizeof destination, "%s/main.sol", relocation);
    FILE *input = fopen(source, "rb"), *copy = fopen(destination, "wb");
    if (input == NULL || copy == NULL) ok = false;
    if (input != NULL && copy != NULL) {
        uint8_t buffer[256]; size_t count = 0;
        while ((count = fread(buffer, 1, sizeof buffer, input)) != 0)
            if (fwrite(buffer, 1, count, copy) != count) ok = false;
    }
    if (input != NULL) fclose(input);
    if (copy != NULL) fclose(copy);
    if (ok) ok = build_multiroot(relocation, false, &relocated, relocated_ids, NULL,
            SOL_WASM_REPRESENTED_OK)
        && relocated_ids[0] == baseline_ids[0] && relocated_ids[1] == baseline_ids[1]
        && relocated.bytes.count == baseline.bytes.count
        && memcmp(relocated.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
        && usage_equal(&relocated.usage, &expected) && invoke_named(&relocated.bytes, entry, 42, 0, 0);
    sol_wasm_represented_output_free(&output); sol_wasm_represented_output_free(&relocated);
    sol_wasm_represented_output_free(&repeat); sol_wasm_represented_output_free(&reverse);
    sol_wasm_represented_output_free(&baseline); return ok;
}

typedef struct {
    size_t block, body, recipe, backing, event, violation_site;
    SolMirRuntimeCleanupTransition *satisfied, *violation, *failure;
} C3aRefinedOwner;

static bool c3a_refined_owner(PropagationPipeline *pipeline, C3aRefinedOwner *owner) {
    if (pipeline == NULL || owner == NULL) return false;
    const SolMirMaterialization *m = &pipeline->concrete.materialization;
    const SolMirOperations *operations = &pipeline->concrete.operations;
    size_t matches = 0;
    memset(owner, 0, sizeof *owner);
    owner->block = owner->body = owner->recipe = owner->backing = owner->event
        = owner->violation_site = SOL_MIR_RUNTIME_NONE;
    for (size_t block = 0; block < m->block_count; ++block) {
        const SolMirMaterializedTerminator *term = &m->blocks[block].terminator;
        if (term->kind != SOL_MIR_TERM_CHECK_REFINED) continue;
        if (++matches != 1 || block >= pipeline->lowered.image_terminator_count
            || term->result >= m->value_count || term->representation >= m->temporary_count)
            return false;
        owner->block = block; owner->recipe = m->values[term->result].type;
        if (owner->recipe >= pipeline->concrete.representation.recipe_count) return false;
        owner->backing = pipeline->concrete.representation.recipes[owner->recipe].backing;
        const SolMirRuntimeLoweredImageTerminator *row = &pipeline->lowered.image_terminators[block];
        if (row->cleanup_event >= pipeline->cleanup.event_count) return false;
        owner->event = row->cleanup_event;
        for (size_t plan = 0; plan < operations->predicate_count; ++plan) {
            const SolMirOperationPredicatePlan *candidate = &operations->predicates[plan];
            if (candidate->kind == SOL_MIR_OPERATION_PREDICATE_REFINEMENT
                && candidate->block == block) {
                if (owner->body != SOL_MIR_RUNTIME_NONE) return false;
                owner->body = candidate->body;
            }
        }
    }
    if (matches != 1 || owner->body >= operations->predicate_body_count) return false;
    SolMirRuntimeCleanupEvent *event = &pipeline->cleanup.events[owner->event];
    if (event->transitions.offset > pipeline->cleanup.transition_count
        || event->transitions.count > pipeline->cleanup.transition_count - event->transitions.offset)
        return false;
    size_t satisfied = 0, violation = 0, failure = 0;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        SolMirRuntimeCleanupTransition *transition = &pipeline->cleanup.transitions[
            event->transitions.offset + i];
        if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_SATISFIED) {
            owner->satisfied = transition; ++satisfied;
        } else if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_VIOLATION) {
            owner->violation = transition; owner->violation_site = transition->failure_site; ++violation;
        } else if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_FAILURE) {
            owner->failure = transition; ++failure;
        }
    }
    return satisfied == 1 && violation == 1 && failure == 1;
}

static bool c3a_refined_acceptance(void) {
    static const struct {
        const char *leaf, *entry;
        int64_t value;
        int32_t code;
        SolWasmRepresentedUsage usage;
        uint8_t hash[32];
    } cases[] = {
        {"p44c_refined_leaf", "launch_true", 42, 0,
            {8,15,10,22,82,1678,0,0,0,0,104,49318,22518,27734,17494},
            {0xaf,0x63,0xff,0x05,0xce,0xdc,0xab,0xc3,0x53,0x89,0x7d,0xd8,0xd4,0x7e,0xd2,0x11,0xdc,0xa2,0x2b,0xac,0x6a,0x92,0x69,0x1c,0xf9,0xa9,0x4e,0xa9,0xdf,0x85,0xa0,0xff}},
        {"p44c_refined_false", "launch", 0, 15,
            {6,9,6,14,53,1025,0,0,0,0,54,30328,11960,19729,9489},
            {0x5b,0xb5,0x83,0x64,0x27,0x28,0xe3,0xd7,0x19,0x43,0x2b,0x49,0x37,0xe1,0x6f,0x5e,0x8b,0x1d,0x12,0x1f,0xc0,0x45,0xd8,0x83,0xab,0xd0,0x19,0x90,0xf8,0xcf,0x26,0x1e}},
        {"p44c_refined_bool", "launch", 1, 0,
            {6,11,8,15,58,1235,0,0,0,0,67,35466,14782,21873,11633},
            {0x50,0x26,0x7e,0x6e,0x45,0xd1,0x2a,0xdc,0x97,0x2c,0x3a,0x8d,0x59,0x95,0x86,0x05,0xb4,0xe2,0x1f,0xba,0x70,0x30,0x8e,0x79,0x44,0xbd,0xd9,0x7b,0x29,0x8b,0xe8,0x0f}},
        {"p44c_refined_constant", "launch", 7, 0,
            {6,9,6,13,52,1037,0,0,0,0,55,30780,12164,19894,9654},
            {0x4a,0x04,0xef,0x23,0xcb,0x54,0xc5,0xf0,0x16,0x19,0xca,0xb6,0x6c,0x4a,0xd6,0x03,0xcb,0xbb,0x47,0x90,0x28,0x9a,0x6f,0x57,0x7f,0xc9,0xa4,0x4e,0xd7,0x2f,0xf5,0xef}},
        {"p44c_refined_arithmetic", "launch", 0, 3,
            {6,12,10,23,62,1155,0,0,0,0,64,33360,14072,21342,11102},
            {0x41,0x43,0x54,0x98,0x3c,0xc1,0xfe,0xc5,0x3f,0xbf,0x6e,0xf8,0x93,0xf9,0x38,0xef,0xb2,0xae,0xee,0x2d,0xda,0x42,0xc9,0xce,0x71,0x72,0x23,0x7c,0x91,0x00,0x96,0x9c}},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        char directory[512], entry[256]; uint8_t hash[32]; SolWasmRepresentedOutput output;
        sol_wasm_represented_output_init(&output);
        (void)snprintf(directory, sizeof directory, "%s/tests/conformance/%s",
            SOL_TEST_SOURCE_DIR, cases[i].leaf);
        bool ok = build_named_root(directory, cases[i].entry, &output, NULL,
                SOL_WASM_REPRESENTED_OK)
            && sol_wasm_represented_validate(&output.bytes) == SOL_WASM_REPRESENTED_OK
            && output.usage.allocation_requests == 0 && output.usage.allocation_bytes == 0
            && usage_equal(&output.usage, &cases[i].usage)
            && entry_symbol(&output.bytes, entry, sizeof entry)
            && invoke_named(&output.bytes, entry, cases[i].value, cases[i].code,
                cases[i].code == 0 ? 0 : -1);
        if (output.bytes.bytes != NULL) sha256(output.bytes.bytes, output.bytes.count, hash);
        ok = ok && output.bytes.bytes != NULL && memcmp(hash, cases[i].hash, sizeof hash) == 0;
        sol_wasm_represented_output_free(&output);
        if (!ok) return false;
    }
    return true;
}

static bool c3a_p36_backend_gate(const char *leaf) {
    char directory[512]; PropagationPipeline pipeline; SolWasmRepresentedOutput output;
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    (void)snprintf(directory, sizeof directory, "%s/tests/conformance/%s",
        SOL_TEST_SOURCE_DIR, leaf);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false)
        && c2b1_predecessors_valid(&pipeline)
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &output, &pipeline.diagnostics)
            == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE
        && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage);
    sol_wasm_represented_output_free(&output); propagation_pipeline_free(&pipeline);
    return ok;
}

static bool c3a_refined_gates(void) {
    static const char *backend[] = {"p44c_refined_nested_reject", "p44c_refined_text_reject",
        "p44c_refined_aggregate_reject", "p44c_refined_borrow_reject",
        "p44c_refined_inout_reject", "p44c_refined_method_reject"};
    bool ok = c2b2_source_gate("p44c_refined_entry_reject", "SOL-ENTRY-003", 5, 3, true)
        && c2b2_source_gate("p44c_refined_cross_equality_reject", "SOL-TYPE-002", 5, 3, true);
    for (size_t i = 0; ok && i < sizeof backend / sizeof *backend; ++i) {
        char directory[512]; SolWasmRepresentedOutput output;
        sol_wasm_represented_output_init(&output);
        (void)snprintf(directory, sizeof directory, "%s/tests/conformance/%s",
            SOL_TEST_SOURCE_DIR, backend[i]);
        ok = build_named_root(directory, "launch", &output, NULL,
                SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE)
            && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage);
        sol_wasm_represented_output_free(&output);
    }
    /* These source-valid aggregate nests reach a complete authenticated P3.6
     * owner. They are backend policy gates, not predecessor corruptions. */
    static const char *nested[] = {"p44c_refined_option_reject",
        "p44c_refined_result_reject", "p44c_refined_enum_reject"};
    for (size_t i = 0; ok && i < sizeof nested / sizeof *nested; ++i)
        ok = c3a_p36_backend_gate(nested[i]);
    if (ok) {
        char directory[512]; PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
        (void)snprintf(directory, sizeof directory,
            "%s/tests/conformance/p44c_refined_indirect_reject", SOL_TEST_SOURCE_DIR);
        ok = !propagation_pipeline_build_named(&pipeline, directory, "launch", false)
            && pipeline.diagnostics.count != 0;
        propagation_pipeline_free(&pipeline);
    }
    if (ok) {
        char directory[512]; PropagationPipeline pipeline; SolWasmRepresentedOutput output;
        propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
        (void)snprintf(directory, sizeof directory,
            "%s/tests/conformance/p44c_refined_import_reject", SOL_TEST_SOURCE_DIR);
        ok = !propagation_pipeline_build_named_with_import(&pipeline, directory, "launch", false,
                "count", SOL_MIR_PROGRAM_ROOT_ENTRY)
            && pipeline.diagnostics.count != 0 && output.bytes.bytes == NULL
            && output.bytes.count == 0 && usage_zero(&output.usage);
        sol_wasm_represented_output_free(&output); propagation_pipeline_free(&pipeline);
    }
    return ok;
}

static bool c3a_refinement_index(void) {
    char directory[512], entry[256]; PropagationPipeline pipeline;
    SolWasmRepresentedOutput output; propagation_pipeline_init(&pipeline);
    sol_wasm_represented_output_init(&output);
    (void)snprintf(directory, sizeof directory,
        "%s/tests/conformance/p44c_refined_many_self", SOL_TEST_SOURCE_DIR);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false);
    size_t plans = 0, self_inputs = 0;
    for (size_t i = 0; ok && i < pipeline.concrete.operations.predicate_count; ++i)
        plans += pipeline.concrete.operations.predicates[i].kind
            == SOL_MIR_OPERATION_PREDICATE_REFINEMENT;
    for (size_t i = 0; ok && i < pipeline.concrete.operations.predicate_input_count; ++i)
        self_inputs += pipeline.concrete.operations.predicate_inputs[i].kind
            == SOL_MIR_PREDICATE_INPUT_REFINEMENT_SELF;
    SolWasmRepresentedResult index_build = ok ? sol_wasm_represented_build(
        &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, &output,
        &pipeline.diagnostics) : SOL_WASM_REPRESENTED_INVALID_INPUT;
    ok = ok && plans == 2 && self_inputs >= 4
        && index_build == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry)
        && invoke_named(&output.bytes, entry, 42, 0, 0);
    SolWasmRepresentedTestRefinementIndexStats indexed =
        sol_wasm_represented_test_refinement_index_stats();
    ok = ok && indexed.body_table_allocations == 1
        && indexed.body_table_bytes == pipeline.concrete.operations.predicate_body_count
            * sizeof(size_t)
        && indexed.indexed_lookups == 12 && indexed.indexed_matches == 12
        && indexed.linear_fallback_scans == 0;
    sol_wasm_represented_output_free(&output); propagation_pipeline_free(&pipeline);

    PropagationPipeline plain; propagation_pipeline_init(&plain);
    sol_wasm_represented_output_init(&output);
    (void)snprintf(directory, sizeof directory,
        "%s/tests/conformance/p43_multiroot", SOL_TEST_SOURCE_DIR);
    ok = ok && propagation_pipeline_build_named(&plain, directory, "first", false)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&plain.lowered,
                directory, NULL}, &output, &plain.diagnostics) == SOL_WASM_REPRESENTED_OK;
    SolWasmRepresentedTestRefinementIndexStats empty =
        sol_wasm_represented_test_refinement_index_stats();
    ok = ok && empty.body_table_allocations == 0 && empty.body_table_bytes == 0
        && empty.indexed_lookups == 0 && empty.indexed_matches == 0
        && empty.linear_fallback_scans == 0;
    sol_wasm_represented_output_free(&output); propagation_pipeline_free(&plain);
    return ok;
}

static bool c3a_refined_certificate_mutations(void) {
    char directory[512]; PropagationPipeline pipeline; C3aRefinedOwner owner;
    SolWasmRepresentedOutput baseline; uint8_t baseline_hash[32];
    (void)snprintf(directory, sizeof directory,
        "%s/tests/conformance/p44c_refined_leaf", SOL_TEST_SOURCE_DIR);
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&baseline);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch_true", false)
        && c3a_refined_owner(&pipeline, &owner)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &baseline, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
    if (ok) sha256(baseline.bytes.bytes, baseline.bytes.count, baseline_hash);
    SolMirRecipe *recipe = ok ? &pipeline.concrete.representation.recipes[owner.recipe] : NULL;
    SolMirTypeLayout *layout = ok ? &pipeline.concrete.layout.types[owner.recipe] : NULL;
    SolMirMaterializedTerminator *term = ok
        ? &pipeline.concrete.materialization.blocks[owner.block].terminator : NULL;
    SolMirPredicateBody *body = ok ? &pipeline.concrete.operations.predicate_bodies[owner.body] : NULL;
    SolMirPredicateInput *input = body != NULL && body->inputs.count != 0
        ? &pipeline.concrete.operations.predicate_inputs[body->inputs.offset] : NULL;
    SolMirOperationArithmeticPlan *equality = NULL;
    for (size_t i = 0; ok && i < pipeline.concrete.operations.arithmetic_count; ++i) {
        SolMirOperationArithmeticPlan *candidate = &pipeline.concrete.operations.arithmetic[i];
        if ((candidate->opcode == SOL_MIR_OPERATION_VALUE_EQ
                || candidate->opcode == SOL_MIR_OPERATION_VALUE_NE)
            && candidate->operand_recipe == owner.recipe) { equality = candidate; break; }
    }
    if (ok) ok = recipe != NULL && layout != NULL && term != NULL && input != NULL
        && equality != NULL && recipe->kind == SOL_MIR_RECIPE_REFINED
        && recipe->storage == SOL_MIR_STORAGE_SCALAR && recipe->copy_kind == SOL_MIR_COPY_WRAPPER
        && recipe->drop_kind == SOL_MIR_DROP_WRAPPER && owner.backing < pipeline.concrete.layout.type_count
        && layout->value_size == pipeline.concrete.layout.types[owner.backing].value_size
        && layout->value_alignment == pipeline.concrete.layout.types[owner.backing].value_alignment
        && !layout->has_object && layout->object_kind == SOL_MIR_LAYOUT_OBJECT_NONE
        && input->kind == SOL_MIR_PREDICATE_INPUT_REFINEMENT_SELF
        && input->ordinal == 0 && input->access == SOL_ACCESS_OWNED
        && input->recipe == owner.backing && owner.satisfied->failure_source
            == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE
        && owner.violation->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31
        && owner.violation->failure_mask == UINT32_C(0x4000)
        && owner.failure->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING
        && owner.violation_site < pipeline.conventions.failure_site_count
        && pipeline.conventions.failure_sites[owner.violation_site].allowed_codes == UINT32_C(0x4000)
        && pipeline.conventions.failure_sites[owner.violation_site].origin_kind
            == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT;
    /* The mutations below are deliberately predecessor-invalid. Their named
     * owner validator rejects before the resealed P3.6/backend checks; they do
     * not count as represented-policy gates. */
#define C3A_REJECT(layer, edit, restore) do { \
    SolWasmRepresentedOutput rejected, rebuilt; sol_wasm_represented_output_init(&rejected); \
    sol_wasm_represented_output_init(&rebuilt); edit; \
    pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
    bool rejected_ok = !(layer) && sol_wasm_represented_build( \
        &(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, &rejected, \
        &pipeline.diagnostics) == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE \
        && rejected.bytes.bytes == NULL && rejected.bytes.count == 0 && usage_zero(&rejected.usage); \
    restore; pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal(&pipeline.lowered); \
    bool rebuilt_ok = sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL) \
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory, NULL}, \
            &rebuilt, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK \
        && rebuilt.bytes.count == baseline.bytes.count && usage_equal(&rebuilt.usage, &baseline.usage) \
        && memcmp(rebuilt.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0; \
    if (rebuilt_ok) { uint8_t hash[32]; sha256(rebuilt.bytes.bytes, rebuilt.bytes.count, hash); \
        rebuilt_ok = memcmp(hash, baseline_hash, sizeof hash) == 0; } \
    ok = ok && rejected_ok && rebuilt_ok; sol_wasm_represented_output_free(&rebuilt); \
    sol_wasm_represented_output_free(&rejected); \
} while (0)
    if (ok) {
        SolMirCopyKind copy = recipe->copy_kind;
        C3A_REJECT(sol_mir_representation_validate(&pipeline.concrete.representation, NULL),
            recipe->copy_kind = SOL_MIR_COPY_TRIVIAL, recipe->copy_kind = copy);
        size_t value_size = layout->value_size;
        C3A_REJECT(sol_mir_layout_validate(&pipeline.concrete.layout, NULL),
            ++layout->value_size, layout->value_size = value_size);
        SolAccessMode access = input->access;
        C3A_REJECT(sol_mir_operations_validate(&pipeline.concrete.operations, NULL),
            input->access = SOL_ACCESS_SHARED, input->access = access);
        SolObligationId obligation = term->source_obligation;
        C3A_REJECT(sol_mir_materialization_validate(&pipeline.concrete.materialization, NULL),
            term->source_obligation = SOL_IR_NONE, term->source_obligation = obligation);
        SolMirPlanSlice normal_arguments = pipeline.concrete.materialization.edges[
            term->normal_edge].arguments;
        C3A_REJECT(sol_mir_materialization_validate(&pipeline.concrete.materialization, NULL),
            pipeline.concrete.materialization.edges[term->normal_edge].arguments.count = 0,
            pipeline.concrete.materialization.edges[term->normal_edge].arguments = normal_arguments);
        SolMirRuntimeCleanupEdgeRole role = owner.violation->edge_role;
        C3A_REJECT(sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL),
            owner.violation->edge_role = SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_FAILURE,
            owner.violation->edge_role = role);
        SolMirRuntimeCleanupFailureSource source = owner.failure->failure_source;
        C3A_REJECT(sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL),
            owner.failure->failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE,
            owner.failure->failure_source = source);
        uint32_t mask = owner.violation->failure_mask;
        C3A_REJECT(sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL),
            owner.violation->failure_mask = 0, owner.violation->failure_mask = mask);
        SolMirPlanSlice equality_slice = equality->equality;
        C3A_REJECT(sol_mir_operations_validate(&pipeline.concrete.operations, NULL),
            equality->equality.count = 0, equality->equality = equality_slice);
        SolMirOperationEqualityNode *root = &pipeline.concrete.operations.equality_nodes[
            equality->equality.offset];
        SolMirRecipeId equality_recipe = root->recipe;
        C3A_REJECT(sol_mir_operations_validate(&pipeline.concrete.operations, NULL),
            root->recipe = owner.backing, root->recipe = equality_recipe);
    }
#undef C3A_REJECT
    sol_wasm_represented_output_free(&baseline); propagation_pipeline_free(&pipeline); return ok;
}

static bool c3a_refined_trace_case(const char *leaf, const char *entry_name, int64_t value,
    int32_t code, SolMirRuntimeFailureOriginKind origin, const P44TraceSlot *expected,
    size_t expected_count) {
    char directory[512], entry[256]; PropagationPipeline pipeline; SolWasmRepresentedOutput output;
    WasmInstance instance = {0}; P44TraceSlot actual[64]; size_t count = 0; bool overflow = false;
    (void)snprintf(directory, sizeof directory, "%s/tests/conformance/%s", SOL_TEST_SOURCE_DIR, leaf);
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, entry_name, false)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry)
        && wasm_instance_open(&output.bytes, entry, &instance)
        && wasm_instance_call(&instance, value, code, code == 0 ? 0 : -1)
        && wasm_instance_trace(&instance, actual, sizeof actual / sizeof *actual, &count, &overflow)
        && !overflow && count == expected_count
        && memcmp(actual, expected, expected_count * sizeof *expected) == 0;
    if (ok && code != 0) {
        wasm_val_t observed; wasm_global_get(instance.site, &observed);
        ProvenanceRecord record;
        ok = observed.kind == WASM_I32 && observed.of.i32 > 0
            && provenance_record(&output.bytes, (uint32_t)observed.of.i32, &record)
            && record.tag == 3 && record.kind == origin
            && bytes_equal(record.path, record.path_count, "main.sol");
    }
    wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false); propagation_pipeline_free(&pipeline);
    return ok;
}

static bool c3a_refined_exact_traces(void) {
    static const P44TraceSlot success[] = {{0,1,0},{5,1,0},{6,1,0},{7,1,0},{8,1,0},
        {9,1,0},{10,1,0},{11,1,0},{12,1,0},{13,1,0},{14,1,0},{27,1,0},{28,1,0},
        {29,1,0},{30,1,0},{31,1,0}};
    static const P44TraceSlot violation[] = {
        {1,1,0},{2,1,0},{3,1,0},{4,533,4},{8,1,0},{9,1,0},{10,533,4}};
    static const P44TraceSlot arithmetic[] = {
        {1,1,0},{2,1,0},{3,1,0},{4,533,5},{8,1,0},{9,1,0},{10,533,5}};
    static const P44TraceSlot overflow[] = {
        {1,1,0},{2,1,0},{3,1,0},{4,533,4},{8,1,0},{9,1,0},{10,533,4}};
    return c3a_refined_trace_case("p44c_refined_leaf", "launch_true", 42, 0,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT, success,
            sizeof success / sizeof *success)
        && c3a_refined_trace_case("p44c_refined_false", "launch", 0, 15,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT, violation,
            sizeof violation / sizeof *violation)
        && c3a_refined_trace_case("p44c_refined_arithmetic", "launch", 0, 3,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC, arithmetic,
            sizeof arithmetic / sizeof *arithmetic)
        && c3a_refined_trace_case("p44c_refined_overflow", "launch", 0, 2,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC, overflow,
            sizeof overflow / sizeof *overflow);
}

static bool c3a_refined_route_case(const char *leaf, const char *entry_name, int64_t value,
    int32_t code, SolMirRuntimeCleanupEdgeRole expected_role) {
    char directory[512], entry[256]; PropagationPipeline pipeline; C3aRefinedOwner owner;
    SolWasmRepresentedOutput output; WasmInstance instance = {0}; P44TraceSlot actual[64];
    size_t count = 0;
    bool overflow = false;
    (void)snprintf(directory, sizeof directory, "%s/tests/conformance/%s", SOL_TEST_SOURCE_DIR, leaf);
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&output);
    sol_wasm_represented_test_p44_refinement_trace_probe(true);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, entry_name, false)
        && c3a_refined_owner(&pipeline, &owner)
        && owner.event <= UINT16_MAX
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry)
        && wasm_instance_open(&output.bytes, entry, &instance)
        && wasm_instance_call(&instance, value, code, code == 0 ? 0 : -1)
        && wasm_instance_trace(&instance, actual, sizeof actual / sizeof *actual, &count, &overflow)
        && !overflow && count == 1
        && actual[0].action == SOL_WASM_REPRESENTED_TEST_P44_REFINEMENT_ROUTE
        && SOL_WASM_REPRESENTED_TEST_P44_REFINEMENT_EVENT(actual[0].disposition)
            == owner.event
        && SOL_WASM_REPRESENTED_TEST_P44_REFINEMENT_ROLE(actual[0].disposition)
            == (uint32_t)expected_role;
    if (ok) {
        wasm_val_t observed; wasm_global_get(instance.site, &observed);
        ok = observed.kind == WASM_I32
            && actual[0].record == (code == 0 ? 0 : (uint32_t)observed.of.i32);
    }
    wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_p44_refinement_trace_probe(false);
    propagation_pipeline_free(&pipeline); return ok;
}

static bool c3a_refined_route_markers(void) {
    return c3a_refined_route_case("p44c_refined_leaf", "launch_true", 42, 0,
            SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_SATISFIED)
        && c3a_refined_route_case("p44c_refined_false", "launch", 0, 15,
            SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_VIOLATION)
        && c3a_refined_route_case("p44c_refined_arithmetic", "launch", 0, 3,
            SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_FAILURE)
        && c3a_refined_route_case("p44c_refined_overflow", "launch", 0, 2,
            SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_FAILURE);
}

static bool c3a_refined_resources(void) {
    static const SolWasmRepresentedUsage expected =
        {8,15,10,22,82,1678,0,0,0,0,104,49318,22518,27734,17494};
    static const uint8_t expected_hash[32] = {0xaf,0x63,0xff,0x05,0xce,0xdc,0xab,0xc3,0x53,0x89,0x7d,0xd8,0xd4,0x7e,0xd2,0x11,0xdc,0xa2,0x2b,0xac,0x6a,0x92,0x69,0x1c,0xf9,0xa9,0x4e,0xa9,0xdf,0x85,0xa0,0xff};
    char directory[512]; SolWasmRepresentedOutput baseline, output;
    sol_wasm_represented_output_init(&baseline); sol_wasm_represented_output_init(&output);
    (void)snprintf(directory, sizeof directory,
        "%s/tests/conformance/p44c_refined_leaf", SOL_TEST_SOURCE_DIR);
    bool ok = build_named_root(directory, "launch_true", &baseline, NULL, SOL_WASM_REPRESENTED_OK);
    uint8_t hash[32]; if (ok) sha256(baseline.bytes.bytes, baseline.bytes.count, hash);
    size_t attempts = sol_wasm_represented_test_allocation_attempts();
    ok = ok && attempts == 162 && usage_equal(&baseline.usage, &expected)
        && memcmp(hash, expected_hash, sizeof hash) == 0;
    SolWasmRepresentedLimits exact = sol_wasm_represented_default_limits();
    exact.max_functions=expected.functions; exact.max_blocks=expected.blocks; exact.max_edges=expected.edges;
    exact.max_values=expected.values; exact.max_locals=expected.locals;
    exact.max_generated_nodes=expected.generated_nodes;
    exact.max_provenance_records=expected.provenance_records; exact.max_work_bytes=expected.work_bytes;
    exact.max_scratch_bytes=expected.scratch_bytes; exact.max_owned_bytes=expected.owned_bytes;
    exact.max_output_bytes=expected.output_bytes;
    if (ok) ok = build_named_root(directory, "launch_true", &output, &exact,
            SOL_WASM_REPRESENTED_OK) && usage_equal(&output.usage, &expected)
        && output.bytes.count == baseline.bytes.count
        && memcmp(output.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0;
    sol_wasm_represented_output_free(&output);
#define C3A_CAP(field, exact_value) do { \
    SolWasmRepresentedLimits cap = sol_wasm_represented_default_limits(); cap.field=(exact_value)-1; \
    sol_wasm_represented_output_init(&output); ok = ok && build_named_root(directory, "launch_true", \
        &output, &cap, SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED) && output.bytes.bytes == NULL \
        && output.bytes.count == 0 && usage_zero(&output.usage); sol_wasm_represented_output_free(&output); \
} while (0)
    C3A_CAP(max_functions, expected.functions); C3A_CAP(max_blocks, expected.blocks);
    C3A_CAP(max_edges, expected.edges); C3A_CAP(max_values, expected.values);
    C3A_CAP(max_locals, expected.locals); C3A_CAP(max_generated_nodes, expected.generated_nodes);
    C3A_CAP(max_provenance_records, expected.provenance_records);
    C3A_CAP(max_work_bytes, expected.work_bytes); C3A_CAP(max_scratch_bytes, expected.scratch_bytes);
    C3A_CAP(max_owned_bytes, expected.owned_bytes); C3A_CAP(max_output_bytes, expected.output_bytes);
#undef C3A_CAP
    for (size_t ordinal = 1; ok && ordinal <= attempts; ++ordinal) {
        represented_fault(FAULT_REFINED, ordinal, ordinal);
        sol_wasm_represented_output_init(&output);
        ok = build_named_root(directory, "launch_true", &output, NULL,
                SOL_WASM_REPRESENTED_ALLOCATION_FAILED)
            && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage);
        sol_wasm_represented_output_free(&output);
        sol_wasm_represented_test_fail_allocation_after(0);
    }
    sol_wasm_represented_output_init(&output);
    if (ok) ok = build_named_root(directory, "launch_true", &output, NULL,
            SOL_WASM_REPRESENTED_OK) && sol_wasm_represented_test_allocation_attempts() == attempts
        && usage_equal(&output.usage, &expected) && output.bytes.count == baseline.bytes.count
        && memcmp(output.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0;
    sol_wasm_represented_output_free(&output); sol_wasm_represented_output_free(&baseline);
    sol_wasm_represented_test_fail_allocation_after(0); return ok;
}

static bool c3a_refined_root_order_relocation(void) {
    static const SolWasmRepresentedUsage expected =
        {7,14,10,22,67,1391,0,0,0,0,86,42397,18625,24732,14492};
    static const uint8_t expected_hash[32] = {0xb4,0xb3,0xe3,0x07,0x53,0x63,0x04,0xe9,0x54,0x15,0x9a,0x7f,0x08,0xbf,0x87,0x72,0x01,0x96,0x31,0x4f,0xa7,0x16,0x6c,0x8d,0xae,0xbe,0xa7,0x46,0x65,0x66,0xa5,0x40};
    char directory[512], relocation[512], source[512], destination[512], entry[256];
    size_t ids[2], reverse_ids[2], relocated_ids[2]; uint8_t hash[32];
    SolWasmRepresentedOutput baseline, reverse, relocated;
    sol_wasm_represented_output_init(&baseline); sol_wasm_represented_output_init(&reverse);
    sol_wasm_represented_output_init(&relocated);
    (void)snprintf(directory, sizeof directory,
        "%s/tests/conformance/p44c_refined_roots", SOL_TEST_SOURCE_DIR);
    bool ok = build_multiroot(directory, false, &baseline, ids, NULL, SOL_WASM_REPRESENTED_OK);
    if (ok) sha256(baseline.bytes.bytes, baseline.bytes.count, hash);
    ok = ok && ids[0] != ids[1] && usage_equal(&baseline.usage, &expected)
        && memcmp(hash, expected_hash, sizeof hash) == 0
        && build_multiroot(directory, true, &reverse, reverse_ids, NULL, SOL_WASM_REPRESENTED_OK)
        && reverse_ids[0] == ids[0] && reverse_ids[1] == ids[1]
        && usage_equal(&reverse.usage, &expected) && reverse.bytes.count == baseline.bytes.count
        && memcmp(reverse.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0;
    (void)mkdir(SOL_TEST_BINARY_DIR, 0700);
    (void)snprintf(relocation, sizeof relocation, "%s/p44c_refined_roots_relocated",
        SOL_TEST_BINARY_DIR); (void)mkdir(relocation, 0700);
    (void)snprintf(source, sizeof source, "%s/main.sol", directory);
    (void)snprintf(destination, sizeof destination, "%s/main.sol", relocation);
    FILE *input = fopen(source, "rb"), *copy = fopen(destination, "wb");
    if (input == NULL || copy == NULL) ok = false;
    if (input != NULL && copy != NULL) {
        uint8_t buffer[256]; size_t count;
        while ((count = fread(buffer, 1, sizeof buffer, input)) != 0)
            if (fwrite(buffer, 1, count, copy) != count) ok = false;
    }
    if (input != NULL) fclose(input); if (copy != NULL) fclose(copy);
    if (ok) ok = build_multiroot(relocation, false, &relocated, relocated_ids, NULL,
            SOL_WASM_REPRESENTED_OK) && relocated_ids[0] == ids[0] && relocated_ids[1] == ids[1]
        && usage_equal(&relocated.usage, &expected) && relocated.bytes.count == baseline.bytes.count
        && memcmp(relocated.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
        && entry_symbol(&relocated.bytes, entry, sizeof entry)
        && invoke_named(&relocated.bytes, entry, 42, 0, 0);
    sol_wasm_represented_output_free(&relocated); sol_wasm_represented_output_free(&reverse);
    sol_wasm_represented_output_free(&baseline); return ok;
}

static bool c3a_write_seal_source(const char *source, const char *destination, char comparison) {
    uint8_t bytes[2048]; FILE *input = fopen(source, "rb");
    if (input == NULL || (comparison != '>' && comparison != '<')) {
        if (input != NULL) fclose(input);
        return false;
    }
    size_t count = fread(bytes, 1, sizeof bytes, input);
    bool ok = !ferror(input) && feof(input) && count < sizeof bytes;
    fclose(input);
    static const char predicate[] = "self > 0";
    size_t matches = 0;
    for (size_t i = 0; ok && i + sizeof predicate - 1 <= count; ++i)
        if (memcmp(bytes + i, predicate, sizeof predicate - 1) == 0
            || (memcmp(bytes + i, "self < 0", sizeof predicate - 1) == 0)) {
            bytes[i + 5] = (uint8_t)comparison; ++matches;
        }
    FILE *output = ok && matches == 1 ? fopen(destination, "wb") : NULL;
    if (output == NULL) return false;
    bool written = fwrite(bytes, 1, count, output) == count;
    bool closed = fclose(output) == 0;
    return written && closed;
}

static bool c3a_refined_source_rebuild_seal(void) {
    char source[512], directory[512], destination[512], entry[256];
    (void)snprintf(source, sizeof source,
        "%s/tests/conformance/p44c_refined_seal/main.sol", SOL_TEST_SOURCE_DIR);
    (void)mkdir(SOL_TEST_BINARY_DIR, 0700);
    (void)snprintf(directory, sizeof directory, "%s/p44c_refined_seal_rebuild",
        SOL_TEST_BINARY_DIR);
    (void)mkdir(directory, 0700);
    (void)snprintf(destination, sizeof destination, "%s/main.sol", directory);
    bool ok = c3a_write_seal_source(source, destination, '>');

    PropagationPipeline baseline_pipeline; propagation_pipeline_init(&baseline_pipeline);
    SolWasmRepresentedOutput baseline; sol_wasm_represented_output_init(&baseline);
    ok = ok && propagation_pipeline_build_named(&baseline_pipeline, directory, "launch", false)
        && c2b1_predecessors_valid(&baseline_pipeline)
        && sol_mir_runtime_lowered_program_validate(&baseline_pipeline.lowered, NULL)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){
                &baseline_pipeline.lowered, directory, NULL}, &baseline,
            &baseline_pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&baseline.bytes, entry, sizeof entry)
        && invoke_named(&baseline.bytes, entry, 42, 0, 0);
    uint64_t old_seal = baseline_pipeline.lowered.authentication;
    uint8_t baseline_hash[32] = {0};
    if (ok) sha256(baseline.bytes.bytes, baseline.bytes.count, baseline_hash);
    propagation_pipeline_free(&baseline_pipeline);

    PropagationPipeline rebuilt; propagation_pipeline_init(&rebuilt);
    SolWasmRepresentedOutput stale, changed; sol_wasm_represented_output_init(&stale);
    sol_wasm_represented_output_init(&changed);
    ok = ok && c3a_write_seal_source(source, destination, '<')
        && propagation_pipeline_build_named(&rebuilt, directory, "launch", false)
        && c2b1_predecessors_valid(&rebuilt)
        && sol_mir_runtime_lowered_program_validate(&rebuilt.lowered, NULL);
    uint64_t new_seal = rebuilt.lowered.authentication;
    rebuilt.lowered.authentication = old_seal;
    bool stale_rejected = ok && old_seal != new_seal
        && !sol_mir_runtime_lowered_program_validate(&rebuilt.lowered, NULL)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&rebuilt.lowered,
                directory, NULL}, &stale, &rebuilt.diagnostics)
            == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE
        && stale.bytes.bytes == NULL && stale.bytes.count == 0 && usage_zero(&stale.usage);
    rebuilt.lowered.authentication = new_seal;
    bool changed_accepted = stale_rejected && c2b1_predecessors_valid(&rebuilt)
        && sol_mir_runtime_lowered_program_validate(&rebuilt.lowered, NULL)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&rebuilt.lowered,
                directory, NULL}, &changed, &rebuilt.diagnostics) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&changed.bytes, entry, sizeof entry)
        && invoke_named(&changed.bytes, entry, 0, 15, -1);
    sol_wasm_represented_output_free(&changed); sol_wasm_represented_output_free(&stale);
    propagation_pipeline_free(&rebuilt);

    PropagationPipeline restored_pipeline; propagation_pipeline_init(&restored_pipeline);
    SolWasmRepresentedOutput restored; sol_wasm_represented_output_init(&restored);
    bool restored_ok = changed_accepted && c3a_write_seal_source(source, destination, '>')
        && propagation_pipeline_build_named(&restored_pipeline, directory, "launch", false)
        && c2b1_predecessors_valid(&restored_pipeline)
        && sol_mir_runtime_lowered_program_validate(&restored_pipeline.lowered, NULL)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){
                &restored_pipeline.lowered, directory, NULL}, &restored,
            &restored_pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && usage_equal(&restored.usage, &baseline.usage)
        && restored.bytes.count == baseline.bytes.count
        && memcmp(restored.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0;
    if (restored_ok) { uint8_t hash[32]; sha256(restored.bytes.bytes, restored.bytes.count, hash);
        restored_ok = memcmp(hash, baseline_hash, sizeof hash) == 0; }
    ok = restored_ok;
    sol_wasm_represented_output_free(&restored);
    propagation_pipeline_free(&restored_pipeline);
    sol_wasm_represented_output_free(&baseline);
    return ok;
}

static char *p44d_lowered_render(const SolMirRuntimeLoweredProgram *lowered,size_t *length){FILE*stream=tmpfile();long end;if(!stream||!sol_mir_runtime_lowered_program_render(stream,lowered)||fflush(stream)!=0||fseek(stream,0,SEEK_END)!=0||(end=ftell(stream))<0||fseek(stream,0,SEEK_SET)!=0){if(stream)fclose(stream);return NULL;}char*bytes=malloc((size_t)end+1);if(!bytes||fread(bytes,1,(size_t)end,stream)!=(size_t)end){free(bytes);fclose(stream);return NULL;}bytes[end]='\0';fclose(stream);if(length)*length=(size_t)end;return bytes;}
static bool p44d_step_authentication(void) {
    char directory[512], entry[256]; PropagationPipeline pipeline;
    SolWasmRepresentedOutput baseline;
    propagation_pipeline_init(&pipeline); sol_wasm_represented_output_init(&baseline);
    (void)snprintf(directory, sizeof directory,
        "%s/tests/conformance/p44c_refined_leaf", SOL_TEST_SOURCE_DIR);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch_true", false)
        && sol_mir_runtime_conventions_validate(&pipeline.conventions, NULL)
        && sol_mir_runtime_cleanup_validate(&pipeline.cleanup, NULL)
        && sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &baseline, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&baseline.bytes, entry, sizeof entry)
        && invoke_named(&baseline.bytes, entry, 42, 0, 0);

    char relocation[512],source[512],destination[512];
    (void)mkdir(SOL_TEST_BINARY_DIR,0700);
    (void)snprintf(relocation,sizeof relocation,"%s/p44d_step_relocated",SOL_TEST_BINARY_DIR);
    (void)mkdir(relocation,0700);(void)snprintf(source,sizeof source,"%s/main.sol",directory);
    (void)snprintf(destination,sizeof destination,"%s/main.sol",relocation);
    FILE*input=fopen(source,"rb"),*copy=fopen(destination,"wb");
    if(input==NULL||copy==NULL)ok=false;
    if(input!=NULL&&copy!=NULL){uint8_t buffer[256];size_t count;while((count=fread(buffer,1,sizeof buffer,input))!=0)if(fwrite(buffer,1,count,copy)!=count)ok=false;}
    if(input!=NULL)fclose(input);if(copy!=NULL)fclose(copy);
    PropagationPipeline relocated_pipeline;propagation_pipeline_init(&relocated_pipeline);
    size_t baseline_render_length=0,relocated_render_length=0;char*baseline_render=NULL,*relocated_render=NULL;
    if(ok)ok=propagation_pipeline_build_named(&relocated_pipeline,relocation,"launch_true",false)
        &&sol_mir_runtime_lowered_program_validate(&relocated_pipeline.lowered,NULL)
        &&(baseline_render=p44d_lowered_render(&pipeline.lowered,&baseline_render_length))!=NULL
        &&(relocated_render=p44d_lowered_render(&relocated_pipeline.lowered,&relocated_render_length))!=NULL
        &&baseline_render_length==relocated_render_length
        &&!memcmp(baseline_render,relocated_render,baseline_render_length);
    free(relocated_render);free(baseline_render);propagation_pipeline_free(&relocated_pipeline);

    SolMirRuntimeLoweredImageInstruction *image_instruction = NULL;
    SolMirRuntimeLoweredImageTerminator *image_terminator = NULL;
    SolMirRuntimeLoweredPredicateInstruction *predicate_instruction = NULL;
    SolMirRuntimeLoweredPredicateTerminator *predicate_terminator = NULL;
    for (size_t i = 0; ok && i < pipeline.lowered.image_instruction_count; ++i)
        if (pipeline.lowered.image_instructions[i].state == SOL_MIR_RUNTIME_LOWERED_PRESENT) {
            image_instruction = &pipeline.lowered.image_instructions[i]; break;
        }
    for (size_t i = 0; ok && i < pipeline.lowered.image_terminator_count; ++i)
        if (pipeline.lowered.image_terminators[i].state == SOL_MIR_RUNTIME_LOWERED_PRESENT) {
            image_terminator = &pipeline.lowered.image_terminators[i]; break;
        }
    for (size_t i = 0; ok && i < pipeline.lowered.predicate_instruction_count; ++i)
        if (pipeline.lowered.predicate_instructions[i].state == SOL_MIR_RUNTIME_LOWERED_PRESENT) {
            predicate_instruction = &pipeline.lowered.predicate_instructions[i]; break;
        }
    for (size_t i = 0; ok && i < pipeline.lowered.predicate_terminator_count; ++i)
        if (pipeline.lowered.predicate_terminators[i].state == SOL_MIR_RUNTIME_LOWERED_PRESENT) {
            predicate_terminator = &pipeline.lowered.predicate_terminators[i]; break;
        }
    ok = ok && image_instruction != NULL && image_terminator != NULL
        && predicate_instruction != NULL && predicate_terminator != NULL;

    size_t step_sites[2] = {SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE};
    size_t step_site_count = 0;
    for (size_t site = 0; ok && site < pipeline.conventions.failure_site_count; ++site) {
        const SolMirRuntimeFailureSite *candidate = &pipeline.conventions.failure_sites[site];
        if (candidate->origin_kind != SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_STEP
            && candidate->origin_kind != SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_STEP) continue;
        if (step_site_count < 2) step_sites[step_site_count] = site;
        ++step_site_count;
        size_t records = 0;
        for (uint32_t record_id = 1; record_id <= baseline.usage.provenance_records; ++record_id) {
            ProvenanceRecord record;
            if (!provenance_record(&baseline.bytes, record_id, &record)) { ok = false; break; }
            records += record.tag == 3 && record.kind == (uint8_t)candidate->origin_kind
                && record.start == candidate->source.start && record.end == candidate->source.end
                && record.ordinal == site;
        }
        ok = ok && candidate->occurrence == site && candidate->allowed_codes == UINT32_C(0x20)
            && records == 1;
    }
    ok = ok && step_site_count != 0 && step_sites[1] != SOL_MIR_RUNTIME_NONE;

    SolMirRuntimeCleanupEvent *image_event = ok
        ? &pipeline.cleanup.events[image_instruction->step_cleanup_event] : NULL;
    SolMirRuntimeCleanupEvent *predicate_event = ok
        ? &pipeline.cleanup.events[predicate_instruction->step_cleanup_event] : NULL;
    SolMirRuntimeCleanupTransition *image_ready = NULL, *image_failure = NULL;
    SolMirRuntimeCleanupTransition *predicate_ready = NULL, *predicate_failure = NULL;
    SolMirRuntimeCleanupEvent *events[2] = {image_event, predicate_event};
    SolMirRuntimeCleanupTransition **ready[2] = {&image_ready, &predicate_ready};
    SolMirRuntimeCleanupTransition **failure[2] = {&image_failure, &predicate_failure};
    for (size_t kind = 0; ok && kind < 2; ++kind) {
        SolMirRuntimeCleanupEvent *event = events[kind];
        ok = event != NULL && event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_STEP
            && event->producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_STEP_METER
            && event->transitions.offset <= pipeline.cleanup.transition_count
            && event->transitions.count == 2
            && event->transitions.count <= pipeline.cleanup.transition_count
                - event->transitions.offset;
        for (size_t i = 0; ok && i < event->transitions.count; ++i) {
            SolMirRuntimeCleanupTransition *transition =
                &pipeline.cleanup.transitions[event->transitions.offset + i];
            if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_PRE_OPERATION_READY)
                *ready[kind] = transition;
            else if (transition->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_STEP_FAILURE)
                *failure[kind] = transition;
            else ok = false;
        }
    }
    SolMirRuntimeCleanupAction *image_action = NULL;
    if (ok) {
        ok = image_ready != NULL && image_failure != NULL && predicate_ready != NULL
            && predicate_failure != NULL && image_failure->actions.count != 0
            && predicate_failure->actions.count == 0;
        if (ok) image_action = &pipeline.cleanup.actions[image_failure->actions.offset];
    }

#define P44D_REJECT(edit, restore) do { \
    SolWasmRepresentedOutput rejected, rebuilt; \
    sol_wasm_represented_output_init(&rejected); sol_wasm_represented_output_init(&rebuilt); \
    edit; pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal( \
        &pipeline.lowered); \
    bool rejected_ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){ \
            &pipeline.lowered, directory, NULL}, &rejected, &pipeline.diagnostics) \
            == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE \
        && rejected.bytes.bytes == NULL && rejected.bytes.count == 0 \
        && usage_zero(&rejected.usage); \
    restore; pipeline.lowered.authentication = sol_mir_runtime_lowered_program_test_seal( \
        &pipeline.lowered); \
    bool rebuilt_ok = sol_mir_runtime_lowered_program_validate(&pipeline.lowered, NULL) \
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, \
            directory, NULL}, &rebuilt, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK \
        && usage_equal(&rebuilt.usage, &baseline.usage) \
        && rebuilt.bytes.count == baseline.bytes.count \
        && memcmp(rebuilt.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0; \
    ok = ok && rejected_ok && rebuilt_ok; \
    sol_wasm_represented_output_free(&rebuilt); sol_wasm_represented_output_free(&rejected); \
} while (0)
    if (ok) {
        size_t saved = image_instruction->step_cleanup_event;
        P44D_REJECT(image_instruction->step_cleanup_event = image_terminator->step_cleanup_event,
            image_instruction->step_cleanup_event = saved);
        saved = image_instruction->step_failure_site;
        P44D_REJECT(image_instruction->step_failure_site = image_terminator->step_failure_site,
            image_instruction->step_failure_site = saved);
        saved = image_terminator->step_cleanup_event;
        P44D_REJECT(image_terminator->step_cleanup_event = image_instruction->step_cleanup_event,
            image_terminator->step_cleanup_event = saved);
        saved = image_terminator->step_failure_site;
        P44D_REJECT(image_terminator->step_failure_site = image_instruction->step_failure_site,
            image_terminator->step_failure_site = saved);
        saved = predicate_instruction->step_cleanup_event;
        P44D_REJECT(predicate_instruction->step_cleanup_event
                = predicate_terminator->step_cleanup_event,
            predicate_instruction->step_cleanup_event = saved);
        saved = predicate_instruction->step_failure_site;
        P44D_REJECT(predicate_instruction->step_failure_site
                = predicate_terminator->step_failure_site,
            predicate_instruction->step_failure_site = saved);
        saved = predicate_terminator->step_cleanup_event;
        P44D_REJECT(predicate_terminator->step_cleanup_event
                = predicate_instruction->step_cleanup_event,
            predicate_terminator->step_cleanup_event = saved);
        saved = predicate_terminator->step_failure_site;
        P44D_REJECT(predicate_terminator->step_failure_site
                = predicate_instruction->step_failure_site,
            predicate_terminator->step_failure_site = saved);

        SolMirRuntimeFailureSite *site = &pipeline.conventions.failure_sites[step_sites[0]];
        SolMirRuntimeFailureOriginKind origin = site->origin_kind;
        P44D_REJECT(site->origin_kind = origin == SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_STEP
                ? SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_STEP
                : SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_STEP,
            site->origin_kind = origin);
        saved = site->owner;
        P44D_REJECT(site->owner = SOL_MIR_RUNTIME_NONE, site->owner = saved);
        saved = site->block;
        P44D_REJECT(site->block = SOL_MIR_RUNTIME_NONE, site->block = saved);
        saved = site->instruction;
        P44D_REJECT(site->instruction = SOL_MIR_RUNTIME_NONE, site->instruction = saved);
        saved = site->source.file;
        P44D_REJECT(site->source.file = SOL_MIR_RUNTIME_NONE, site->source.file = saved);
        saved = site->source.start;
        P44D_REJECT(site->source.start ^= 1, site->source.start = saved);
        saved = site->source.end;
        P44D_REJECT(site->source.end ^= 1, site->source.end = saved);
        size_t occurrence = pipeline.conventions.failure_sites[step_sites[1]].occurrence;
        P44D_REJECT(pipeline.conventions.failure_sites[step_sites[1]].occurrence
                = pipeline.conventions.failure_sites[step_sites[0]].occurrence,
            pipeline.conventions.failure_sites[step_sites[1]].occurrence = occurrence);
        SolMirRuntimeFailureSite *other = &pipeline.conventions.failure_sites[step_sites[1]];
        SolMirRuntimeFailureSite collision_saved = *other;
        P44D_REJECT(*other = *site, *other = collision_saved);
        uint32_t allowed_codes = site->allowed_codes;
        P44D_REJECT(site->allowed_codes = 0, site->allowed_codes = allowed_codes);

        SolMirRuntimeCleanupEventKind event_kind = image_event->kind;
        P44D_REJECT(image_event->kind = SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION,
            image_event->kind = event_kind);
        SolMirRuntimeCleanupPhase phase = image_event->phase;
        P44D_REJECT(image_event->phase = SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION,
            image_event->phase = phase);
        SolMirRuntimeCleanupOrigin event_origin = image_event->origin;
        P44D_REJECT(image_event->origin = SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT,
            image_event->origin = event_origin);
        saved = image_event->owner;
        P44D_REJECT(image_event->owner = SOL_MIR_RUNTIME_NONE, image_event->owner = saved);
        saved = image_event->block;
        P44D_REJECT(image_event->block = SOL_MIR_RUNTIME_NONE, image_event->block = saved);
        saved = image_event->operation;
        P44D_REJECT(image_event->operation = SOL_MIR_RUNTIME_NONE,
            image_event->operation = saved);
        saved = image_event->semantic_site;
        P44D_REJECT(image_event->semantic_site = 0, image_event->semantic_site = saved);
        saved = image_event->source.file;
        P44D_REJECT(image_event->source.file = SOL_MIR_RUNTIME_NONE,
            image_event->source.file = saved);
        saved = image_event->source.start;
        P44D_REJECT(image_event->source.start ^= 1, image_event->source.start = saved);
        saved = image_event->source.end;
        P44D_REJECT(image_event->source.end ^= 1, image_event->source.end = saved);
        saved = image_event->inherited_failure_site;
        P44D_REJECT(image_event->inherited_failure_site
                = predicate_instruction->step_failure_site,
            image_event->inherited_failure_site = saved);
        saved = image_event->supplemental_site;
        P44D_REJECT(image_event->supplemental_site = 0,
            image_event->supplemental_site = saved);
        SolMirRuntimeSlice event_actions = image_event->actions;
        P44D_REJECT(++image_event->actions.offset, image_event->actions = event_actions);
        P44D_REJECT(--image_event->actions.count, image_event->actions = event_actions);
        SolMirRuntimeSlice event_transitions = image_event->transitions;
        P44D_REJECT(++image_event->transitions.offset,
            image_event->transitions = event_transitions);
        P44D_REJECT(--image_event->transitions.count,
            image_event->transitions = event_transitions);
        SolMirRuntimeCleanupProducerKind producer = image_event->producer;
        P44D_REJECT(image_event->producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL,
            image_event->producer = producer);
        bool captures = image_event->captures_failure_detail;
        P44D_REJECT(image_event->captures_failure_detail = false,
            image_event->captures_failure_detail = captures);
        SolMirRuntimeFailureDetailKind detail_kind = image_event->capture_detail_kind;
        P44D_REJECT(image_event->capture_detail_kind
                = SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT,
            image_event->capture_detail_kind = detail_kind);

        saved = image_ready->event;
        P44D_REJECT(image_ready->event = predicate_instruction->step_cleanup_event,
            image_ready->event = saved);
        SolMirRuntimeCleanupOutcome transition_outcome = image_ready->outcome;
        P44D_REJECT(image_ready->outcome = SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE,
            image_ready->outcome = transition_outcome);
        saved = image_ready->continuation;
        P44D_REJECT(image_ready->continuation = 0, image_ready->continuation = saved);
        SolMirRuntimeSlice transition_actions = image_ready->actions;
        P44D_REJECT(++image_ready->actions.offset, image_ready->actions = transition_actions);
        P44D_REJECT(++image_ready->actions.count, image_ready->actions = transition_actions);
        bool primary = image_ready->primary_failure_wins;
        P44D_REJECT(image_ready->primary_failure_wins = false,
            image_ready->primary_failure_wins = primary);
        SolMirRuntimeCleanupEdgeRole role = image_ready->edge_role;
        P44D_REJECT(image_ready->edge_role = SOL_MIR_RUNTIME_CLEANUP_EDGE_STEP_FAILURE,
            image_ready->edge_role = role);
        saved = image_ready->source_edge;
        P44D_REJECT(image_ready->source_edge = 0, image_ready->source_edge = saved);
        saved = image_ready->destination;
        P44D_REJECT(image_ready->destination = 0, image_ready->destination = saved);
        SolMirRuntimeCleanupFailureSource ready_source = image_ready->failure_source;
        P44D_REJECT(image_ready->failure_source
                = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31,
            image_ready->failure_source = ready_source);
        saved = image_ready->failure_site;
        P44D_REJECT(image_ready->failure_site = image_instruction->step_failure_site,
            image_ready->failure_site = saved);
        uint32_t ready_mask = image_ready->failure_mask;
        P44D_REJECT(image_ready->failure_mask = UINT32_C(0x20),
            image_ready->failure_mask = ready_mask);
        SolContractClauseKind contract_phase = image_ready->contract_phase;
        P44D_REJECT(image_ready->contract_phase = contract_phase == SOL_CONTRACT_REQUIRES
                ? SOL_CONTRACT_ENSURES : SOL_CONTRACT_REQUIRES,
            image_ready->contract_phase = contract_phase);
        SolContractOutcomeKind contract_outcome = image_ready->contract_outcome;
        P44D_REJECT(image_ready->contract_outcome
                = contract_outcome == SOL_CONTRACT_OUTCOME_SUCCESS
                    ? SOL_CONTRACT_OUTCOME_FAILURE : SOL_CONTRACT_OUTCOME_SUCCESS,
            image_ready->contract_outcome = contract_outcome);

        SolMirRuntimeCleanupFailureSource failure_source = image_failure->failure_source;
        P44D_REJECT(image_failure->failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE,
            image_failure->failure_source = failure_source);
        saved = image_failure->failure_site;
        P44D_REJECT(image_failure->failure_site = predicate_instruction->step_failure_site,
            image_failure->failure_site = saved);
        uint32_t failure_mask = image_failure->failure_mask;
        P44D_REJECT(image_failure->failure_mask = 0,
            image_failure->failure_mask = failure_mask);
        transition_actions = image_failure->actions;
        P44D_REJECT(++image_failure->actions.offset,
            image_failure->actions = transition_actions);
        P44D_REJECT(--image_failure->actions.count,
            image_failure->actions = transition_actions);
        SolMirRuntimeSlice predicate_actions = predicate_failure->actions;
        P44D_REJECT(predicate_failure->actions = image_failure->actions,
            predicate_failure->actions = predicate_actions);
        SolMirRuntimeCleanupActionKind action_kind = image_action->kind;
        P44D_REJECT(image_action->kind = SOL_MIR_RUNTIME_CLEANUP_ACTION_CHECK_CONTRACT,
            image_action->kind = action_kind);
        unsigned action_flags = image_action->flags;
        P44D_REJECT(image_action->flags ^= SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED,
            image_action->flags = action_flags);
        saved = image_action->target;
        P44D_REJECT(image_action->target ^= 1, image_action->target = saved);
        SolMirRecipeId action_recipe = image_action->recipe;
        P44D_REJECT(image_action->recipe = action_recipe == SOL_MIR_RECIPE_NONE
                ? 0 : SOL_MIR_RECIPE_NONE,
            image_action->recipe = action_recipe);
        saved = image_action->drop_path;
        P44D_REJECT(image_action->drop_path = saved == SOL_MIR_RUNTIME_NONE
                ? 0 : SOL_MIR_RUNTIME_NONE,
            image_action->drop_path = saved);
        SolMirRuntimeCleanupAction *propagate = &pipeline.cleanup.actions[
            image_failure->actions.offset + image_failure->actions.count - 1];
        saved = propagate->target;
        P44D_REJECT(propagate->target = predicate_instruction->step_failure_site,
            propagate->target = saved);
    }
#undef P44D_REJECT
    if (ok) ok = invoke_named(&baseline.bytes, entry, 42, 0, 0);
    sol_wasm_represented_output_free(&baseline); propagation_pipeline_free(&pipeline);
    return ok;
}

/* Independent source-path witnesses: (P3 occurrence, number of ticks). A Text
 * operation owns its base tick and its three byte ticks. Branches below follow
 * the source's true arms, not the runtime packet. Panic's terminal range owns
 * one base tick plus the bounded capture; old_two enters the callee between
 * the caller's call and normal-return blocks. The SHA-256 witness freezes every
 * selected owner's origin/owner/block/instruction/occurrence/span/code mask. */
typedef struct { uint16_t occurrence, ticks; } P44eOwnerRange;
static const P44eOwnerRange p44e_rows_path[] = {
    {4,1},{20,1},{28,1},{29,1},{30,1},{31,1},{32,1},{33,1},{34,1},{35,1},{38,1},
    {7,1},{8,1},{9,1},{10,1},{11,1},{12,1},{13,1},{14,1},{15,1},{16,1},{22,1},
    {21,1},{39,1},{40,1},{41,1},{19,1},{25,1},
};
static const P44eOwnerRange p44e_text_path[] = {
    {0,1},{1,1},{2,4},{3,1},{4,1},{5,4},{6,1},{7,1},{8,4},{9,1},{10,1},{11,1},
    {12,1},{13,4},{14,1},{15,1},{16,4},{17,1},{18,1},{19,1},{20,1},{21,4},
    {22,1},{23,1},{24,4},{25,4},{26,4},{55,1},{27,4},{28,4},{29,4},{56,1},
    {57,1},{30,4},{31,4},{32,4},{58,1},{59,1},{33,1},{34,1},{35,1},{36,1},
    {60,1},{41,1},{42,1},{43,1},{44,1},{45,1},{46,1},{47,1},{48,1},{49,1},
    {50,1},{51,1},{52,1},{53,1},{54,1},{62,1},
};
static const P44eOwnerRange p44e_nested_path[] = {
    {0,1},{1,1},{2,4},{3,1},{4,1},{5,1},{6,1},{7,1},{8,1},{9,4},{10,1},
    {11,1},{12,4},{13,1},{14,1},{15,1},{16,1},{17,1},{18,1},{19,4},{20,1},
    {21,1},{22,4},{23,4},{24,4},{46,1},{25,4},{26,4},{27,4},{47,1},{48,1},
    {28,1},{29,1},{30,1},{31,1},{49,1},{36,1},{37,1},{38,1},{39,1},{40,1},
    {41,1},{42,1},{43,1},{44,1},{45,1},{51,1},
};
static const P44eOwnerRange p44e_cleanup_path[] = {
    {6,1},{7,1},{8,1},{9,1},{10,1},{11,1},{12,1},{13,4},{14,1},{15,1},
    {16,1},{17,1},{18,1},{19,1},{20,1},{21,1},{22,1},{23,1},{24,1},{25,1},
    {26,1},{27,1},{28,1},{29,1},{30,1},{31,1},{32,1},{33,1},
};
static const P44eOwnerRange p44e_panic_path[] = {
    {1,1},{2,1},{3,27},{4,1},{5,1},{6,27},
};
static const P44eOwnerRange p44e_panic_empty_path[] = {
    {1,1},{2,1},{3,1},{4,1},{5,1},{6,1},
};
static const P44eOwnerRange p44e_panic_191_path[] = {
    {1,1},{2,1},{3,192},{4,1},{5,1},{6,192},
};
static const P44eOwnerRange p44e_panic_192_path[] = {
    {1,1},{2,1},{3,193},{4,1},{5,1},{6,192},
};
static const P44eOwnerRange p44e_old_two_path[] = {
    {34,1},{35,1},{36,1},{37,1},{38,1},{39,1},{44,1},
    {4,1},{5,1},{6,1},{7,1},{8,1},{9,1},{10,1},{11,1},{12,1},{13,1},
    {29,1},{30,1},{47,1},{48,1},{49,1},{24,1},{25,1},{26,1},{27,1},{28,1},
    {31,1},{42,1},{43,1},{46,1},
};

typedef struct {
    const char *leaf, *root;
    const P44eOwnerRange *path;
    size_t count;
    const char *witness;
    size_t ledger_boundaries;
} P44eOwnerPath;
#define P44E_PATH(leaf, root, path, witness, ledgers) \
    {leaf, root, path, sizeof path / sizeof *path, witness, ledgers}
static const P44eOwnerPath p44e_owner_paths[] = {
    P44E_PATH("p44e_step_rows", "launch", p44e_rows_path,
        "fc507c4af20d3b9cce767816bdbf29a476fa0e2f01afa56ee18b0eb4f13572ee", 15),
    P44E_PATH("p44e_step_text", "launch", p44e_text_path,
        "c796ee6ef6776fa05dff71249f012df7db7a345487894dbd754e513f10e88d25", 28),
    P44E_PATH("p44e_step_text", "nested", p44e_nested_path,
        "8a0ac78d1dfc0a2aabc464010a7d1b49fd892db1aa76e9cacfa0a46e2e87eedb", 16),
    P44E_PATH("p44e_step_cleanup", "launch", p44e_cleanup_path,
        "df7c1b064afba18c7f67fe8a4a2d37e2fff673c399ffe343cf06bcf72b9bb7c6", 7),
    P44E_PATH("p44_panic", "launch", p44e_panic_path,
        "bb8749c407ff7a5b84d872192cb562d254b3ba6b9e776b8a80fbbedba3f08d7a", 54),
    P44E_PATH("p44_panic_empty", "launch", p44e_panic_empty_path,
        "243387dd358732263538093844809e5b327a23a00497e0ea280cc0b82cc77c3e", 2),
    P44E_PATH("p44_panic_191", "launch", p44e_panic_191_path,
        "bd78064f0ee6ff352e95ea59dd4b9c645abad8f98cf708287f998d05eda6f217", 384),
    P44E_PATH("p44_panic_192", "launch", p44e_panic_192_path,
        "e2dbe8e52f5711b2ffa99b267d57d70558752831dddf4a160f6fa172818bc65a", 385),
    P44E_PATH("p44c_ensures_old_two", "launch", p44e_old_two_path,
        "e948961f78c558b6b030e594d4bc3d4de436c30d831f133d55ea6562a2eb3e33", 5),
};
#undef P44E_PATH

static const P44eOwnerPath *p44e_owner_path(const char *leaf, const char *root) {
    for (size_t i = 0; i < sizeof p44e_owner_paths / sizeof *p44e_owner_paths; ++i)
        if (!strcmp(leaf, p44e_owner_paths[i].leaf) && !strcmp(root, p44e_owner_paths[i].root))
            return &p44e_owner_paths[i];
    return NULL;
}

static bool p44e_owner_witness(const PropagationPipeline *pipeline, const P44eOwnerPath *path) {
    char text[16384], digest[65]; size_t used = 0;
    for (size_t i = 0; i < path->count; ++i) {
        size_t ordinal = path->path[i].occurrence;
        if (ordinal >= pipeline->conventions.failure_site_count) return false;
        const SolMirRuntimeFailureSite *s = &pipeline->conventions.failure_sites[ordinal];
        int length = snprintf(text + used, sizeof text - used, "%zu:%u:%zu:%zu:%zu:%zu:%zu:%zu:%zu:%u;",
            ordinal, (unsigned)s->origin_kind, s->owner, s->block, s->instruction, s->occurrence,
            s->source.file, s->source.start, s->source.end, s->allowed_codes);
        if (length < 0 || (size_t)length >= sizeof text - used) return false;
        used += (size_t)length;
    }
    uint8_t hash[32]; sha256((const uint8_t *)text, used, hash);
    for (size_t i = 0; i < sizeof hash; ++i) (void)snprintf(digest + i * 2, 3, "%02x", hash[i]);
    if (strcmp(path->witness, digest)) {
        fprintf(stderr, "P44e owner witness %s/%s %s\n", path->leaf, path->root, digest);
        return false;
    }
    return true;
}

static bool p44e_owner_matches(const PropagationPipeline *pipeline,
    const SolWasmRepresentedOutput *output, const P44eOwnerPath *path, uint64_t cap, int32_t observed) {
    uint64_t position = 0; size_t ordinal = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < path->count; ++i) {
        position += path->path[i].ticks;
        if (cap < position) { ordinal = path->path[i].occurrence; break; }
    }
    if (ordinal >= pipeline->conventions.failure_site_count || observed <= 0) return false;
    const SolMirRuntimeFailureSite *s = &pipeline->conventions.failure_sites[ordinal];
    uint32_t selected = 0; size_t matches = 0;
    /* The static table picks the owner BEFORE provenance records are inspected. */
    for (uint32_t i = 1; i <= output->usage.provenance_records; ++i) {
        ProvenanceRecord r;
        if (!provenance_record(&output->bytes, i, &r)) return false;
        if (r.tag == 3 && r.kind == s->origin_kind && r.ordinal == ordinal
            && r.start == s->source.start && r.end == s->source.end
            && bytes_equal(r.path, r.path_count, "main.sol")) { selected = i; ++matches; }
    }
    return matches == 1 && selected == (uint32_t)observed && s->occurrence == ordinal
        && s->allowed_codes == UINT32_C(0x20);
}

typedef struct {
    uint16_t event, transition, first, count;
    uint64_t skipped;
} P44eLedgerSlice;
typedef struct {
    const char *leaf, *root;
    uint16_t first_cap, last_cap;
    size_t count;
    P44eLedgerSlice slices[12];
} P44eLedgerCase;
#define E1S(e,t,o,n,k) {e,t,o,n,k}
/* These complete transition sequences come from the source execution path and
 * P3 selection, not a search for actions found in the observed ledger. A zero
 * action predicate STEP_FAILURE is deliberately included. Hole repair skips
 * the old callable field (bit zero); the initialized Pair remains executed.
 * old_two drops snapshots 1 then 0, both on PRE_STEP and enclosing transport. */
static const P44eLedgerCase p44e_ledgers[] = {
    {"p44e_step_rows","launch",1,1,1,{E1S(31,1,20,2,0)}},
    {"p44e_step_rows","launch",2,10,3,
        {E1S(0,2,2,0,0),E1S(8,0,12,1,0),E1S(9,0,13,1,0)}},
    {"p44e_step_rows","launch",11,11,2,{E1S(0,0,0,1,0),E1S(33,1,24,2,0)}},
    {"p44e_step_rows","launch",21,21,4,
        {E1S(0,0,0,1,0),E1S(3,0,8,1,0),E1S(4,0,9,1,0),E1S(43,1,57,2,0)}},
    {"p44e_step_rows","launch",23,25,6,
        {E1S(0,0,0,1,0),E1S(3,0,8,1,0),E1S(4,0,9,1,0),E1S(1,2,4,0,0),
         E1S(14,0,17,1,0),E1S(15,0,18,1,0)}},
    {"p44e_step_text","launch",2,5,1,{E1S(34,1,114,3,0)}},
    {"p44e_step_text","launch",8,11,1,{E1S(37,1,123,4,0)}},
    {"p44e_step_text","launch",22,25,1,{E1S(45,1,161,6,0)}},
    {"p44e_step_text","launch",36,39,1,{E1S(53,1,215,8,0)}},
    {"p44e_step_text","launch",50,53,1,{E1S(58,1,257,9,0)}},
    {"p44e_step_text","launch",63,66,1,{E1S(62,1,293,9,0)}},
    {"p44e_step_text","launch",77,80,1,{E1S(67,1,338,9,0)}},
    {"p44e_step_text","nested",12,15,1,{E1S(35,1,100,4,0)}},
    {"p44e_step_text","nested",28,31,1,{E1S(45,1,149,6,0)}},
    {"p44e_step_text","nested",42,45,1,{E1S(50,1,181,7,0)}},
    {"p44e_step_text","nested",55,58,1,{E1S(54,1,209,7,0)}},
    {"p44e_step_cleanup","launch",7,10,1,{E1S(24,1,49,4,0)}},
    {"p44e_step_cleanup","launch",18,18,1,{E1S(32,1,80,5,0)}},
    {"p44e_step_cleanup","launch",19,19,1,{E1S(33,1,85,4,0)}},
    {"p44e_step_cleanup","launch",30,30,5,
        {E1S(6,0,14,1,1),E1S(7,0,15,1,0),E1S(8,0,16,1,0),E1S(9,0,17,1,0),
         E1S(44,1,123,1,0)}},
    {"p44_panic","launch",2,28,1,{E1S(6,1,9,3,0)}},
    {"p44_panic","launch",31,57,3,
        {E1S(1,0,3,1,0),E1S(2,0,4,1,0),E1S(9,1,17,1,0)}},
    {"p44_panic_empty","launch",2,2,1,{E1S(6,1,9,3,0)}},
    {"p44_panic_empty","launch",5,5,3,
        {E1S(1,0,3,1,0),E1S(2,0,4,1,0),E1S(9,1,17,1,0)}},
    {"p44_panic_191","launch",2,193,1,{E1S(6,1,9,3,0)}},
    {"p44_panic_191","launch",196,387,3,
        {E1S(1,0,3,1,0),E1S(2,0,4,1,0),E1S(9,1,17,1,0)}},
    {"p44_panic_192","launch",2,194,1,{E1S(6,1,9,3,0)}},
    {"p44_panic_192","launch",197,388,3,
        {E1S(1,0,3,1,0),E1S(2,0,4,1,0),E1S(9,1,17,1,0)}},
    {"p44c_ensures_old_two","launch",15,15,5,
        {E1S(34,1,67,7,0),E1S(16,1,27,0,0),E1S(17,0,27,1,0),E1S(18,0,28,1,0),
         E1S(19,0,29,1,0)}},
    {"p44c_ensures_old_two","launch",19,21,10,
        {E1S(1,0,7,1,0),E1S(3,2,10,0,0),E1S(12,0,21,1,0),E1S(13,0,22,1,0),
         E1S(14,0,23,1,0),E1S(15,0,24,3,0),E1S(16,1,27,0,0),E1S(17,0,27,1,0),
         E1S(18,0,28,1,0),E1S(19,0,29,1,0)}},
    {"p44c_ensures_old_two","launch",27,27,9,
        {E1S(1,0,7,1,0),E1S(3,0,8,1,0),E1S(4,0,10,1,0),E1S(5,0,11,1,0),
         E1S(6,0,12,1,0),E1S(43,1,117,3,0),E1S(17,0,27,1,0),E1S(18,0,28,1,0),
         E1S(19,0,29,1,0)}},
};
#undef E1S

static bool p44e_ledger_matches(const PropagationPipeline *pipeline, const char *leaf,
    const char *root, uint64_t cap, uint32_t record, const P44TraceSlot *actual, size_t count,
    size_t *checked) {
    for (size_t i = 0; i < sizeof p44e_ledgers / sizeof *p44e_ledgers; ++i) {
        const P44eLedgerCase *want = &p44e_ledgers[i];
        if (strcmp(leaf, want->leaf) || strcmp(root, want->root)
            || cap < want->first_cap || cap > want->last_cap) continue;
        P44TraceSlot expected[64]; size_t used = 0;
        for (size_t j = 0; j < want->count; ++j) {
            const P44eLedgerSlice *slice = &want->slices[j];
            if (slice->event >= pipeline->cleanup.event_count) return false;
            const SolMirRuntimeCleanupEvent *e = &pipeline->cleanup.events[slice->event];
            if (slice->transition >= e->transitions.count) return false;
            const SolMirRuntimeCleanupTransition *t =
                &pipeline->cleanup.transitions[e->transitions.offset + slice->transition];
            if (t->actions.offset != slice->first || t->actions.count != slice->count
                || slice->count > 64 - used) return false;
            for (size_t k = 0; k < slice->count; ++k) {
                const SolMirRuntimeCleanupAction *a = &pipeline->cleanup.actions[slice->first + k];
                unsigned disposition = c1_trace_disposition(e, t, a);
                if ((slice->skipped & (UINT64_C(1) << k)) != 0) {
                    if (!(a->flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED)) return false;
                    disposition ^= SOL_WASM_REPRESENTED_TEST_P44_TRACE_EXECUTED
                        | SOL_WASM_REPRESENTED_TEST_P44_TRACE_SKIPPED;
                }
                expected[used++] = (P44TraceSlot){(uint32_t)(slice->first + k), disposition,
                    t->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE ? record : 0};
            }
        }
        if (used != count || memcmp(expected, actual, used * sizeof *expected)) {
            fprintf(stderr, "P44e exact ledger %s/%s cap=%llu expected=%zu actual=%zu\n",
                leaf, root, (unsigned long long)cap, used, count);
            for (size_t k = 0; k < count; ++k) fprintf(stderr, " actual {%u,%u,%u}\n",
                actual[k].action, actual[k].disposition, actual[k].record);
            return false;
        }
        ++*checked;
        return true;
    }
    return true;
}

/* Exhaust every runtime boundary, not a test-only quota hook. Each code-six
 * record is matched to its authenticated occurrence and exact PRE_STEP slice;
 * predicates own no local cleanup and retain the enclosing packet route. */
static bool p44e_step_sweep(const char *leaf, const char *root, int64_t expected_value,
    int32_t expected_code, uint64_t expected_steps, unsigned *classes) {
    char directory[512], entry[256]; PropagationPipeline pipeline;
    propagation_pipeline_init(&pipeline);
    (void)snprintf(directory, sizeof directory, "%s/tests/conformance/%s", SOL_TEST_SOURCE_DIR, leaf);
    /* Re-root from source, never by mutating an authenticated IR/owner. */
    if (!strcmp(leaf, "p44e_step_text") && strcmp(root, "launch")) {
        char source[768], destination[768], line[1024], marker[256];
        (void)snprintf(source, sizeof source, "%s/main.sol", directory);
        (void)snprintf(directory, sizeof directory, "%s/p44e_text_%s", SOL_TEST_BINARY_DIR, root);
        (void)mkdir(directory, 0700);
        (void)snprintf(destination, sizeof destination, "%s/main.sol", directory);
        (void)snprintf(marker, sizeof marker, "public function %s(", root);
        FILE *input = fopen(source, "r"), *copy = fopen(destination, "w");
        if (input == NULL || copy == NULL) {
            if (input != NULL) fclose(input);
            if (copy != NULL) fclose(copy);
            return false;
        }
        while (fgets(line, sizeof line, input) != NULL) {
            if (!strcmp(line, "@entry\n")) continue;
            if (strstr(line, marker) != NULL) fputs("@entry\n", copy);
            fputs(line, copy);
        }
        bool copied = !ferror(input) && !ferror(copy);
        fclose(input); if (fclose(copy) != 0) copied = false;
        if (!copied) return false;
    }
    bool ok = propagation_pipeline_build_named(&pipeline, directory, root, false);
    const P44eOwnerPath *owner_path = p44e_owner_path(leaf, root);
    if (ok && owner_path != NULL) ok = p44e_owner_witness(&pipeline, owner_path);
    if (!ok) for (size_t i = 0; i < pipeline.diagnostics.count; ++i)
        fprintf(stderr, "P44e source %s: %s\n", leaf, pipeline.diagnostics.items[i].message);
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    uint64_t completed = 0; size_t ledger_boundaries = 0;
    for (uint64_t cap = 1; ok && cap <= 512; ++cap) {
        SolWasmRepresentedLimits limits = sol_wasm_represented_default_limits();
        limits.max_steps = cap;
        SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
        WasmInstance instance = {0}; int64_t value = -1; int32_t code = 0, site = 0;
        P44TraceSlot slots[64]; size_t count = 0; bool overflow = false;
        ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, &limits}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && entry_symbol(&output.bytes, entry, sizeof entry)
            && wasm_instance_open(&output.bytes, entry, &instance)
            && wasm_instance_observe(&instance, &value, &code, &site)
            && wasm_instance_trace(&instance, slots, 64, &count, &overflow) && !overflow;
        P44TraceSlot repeated[64]; size_t repeated_count = 0; bool repeated_overflow = false;
        ok = ok && wasm_instance_call(&instance, value, code, site)
            && wasm_instance_trace(&instance, repeated, 64, &repeated_count, &repeated_overflow)
            && repeated_count == count && repeated_overflow == overflow
            && memcmp(repeated, slots, count * sizeof *slots) == 0;
        if (ok && code != SOL_MIR_RUNTIME_FAILURE_STEP_LIMIT) {
            ok = code == expected_code && value == expected_value;
            completed = cap;
        } else if (ok) {
            if (owner_path != NULL) ok = p44e_owner_matches(&pipeline, &output, owner_path, cap, site);
            ok = ok && p44e_ledger_matches(&pipeline, leaf, root, cap, (uint32_t)site, slots, count,
                &ledger_boundaries);
            ProvenanceRecord record;
            ok = ok && value == 0 && site > 0 && provenance_record(&output.bytes, (uint32_t)site, &record)
                && record.tag == 3 && bytes_equal(record.path, record.path_count, "main.sol")
                && record.ordinal < pipeline.conventions.failure_site_count;
            const SolMirRuntimeFailureSite *source = ok
                ? &pipeline.conventions.failure_sites[record.ordinal] : NULL;
            ok = ok && source->occurrence == record.ordinal && source->source.start == record.start
                && source->source.end == record.end && (uint8_t)source->origin_kind == record.kind
                && source->allowed_codes == (UINT32_C(1) << 5);
            const SolMirRuntimeCleanupEvent *event = NULL;
            for (size_t i = 0; ok && i < pipeline.cleanup.event_count; ++i) {
                const SolMirRuntimeCleanupEvent *candidate = &pipeline.cleanup.events[i];
                if (candidate->producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_STEP_METER
                    && candidate->inherited_failure_site == record.ordinal) {
                    if (event != NULL) ok = false;
                    event = candidate;
                }
            }
            ok = ok && event != NULL && event->transitions.count == 2;
            if (ok) {
                unsigned row_class = event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION ? 1u
                    : event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR ? 2u
                    : event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION ? 4u : 8u;
                *classes |= row_class;
                const SolMirRuntimeCleanupTransition *failure =
                    &pipeline.cleanup.transitions[event->transitions.offset + 1];
                ok = failure->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_STEP_FAILURE;
                size_t local_start = count;
                for (size_t i = 0; i < count; ++i)
                    if (slots[i].action == failure->actions.offset) { local_start = i; break; }
                if (row_class <= 2u) {
                    ok = ok && failure->actions.count != 0 && local_start < count
                        && failure->actions.count <= count - local_start;
                    for (size_t i = 0; ok && i < failure->actions.count; ++i) {
                        const P44TraceSlot *slot = &slots[local_start + i];
                        const SolMirRuntimeCleanupAction *action =
                            &pipeline.cleanup.actions[failure->actions.offset + i];
                        uint32_t disposition = 12u
                            | ((action->flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY) ? 512u : 0u)
                            | ((action->flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED) ? 1024u : 0u);
                        ok = slot->action == failure->actions.offset + i && slot->record == (uint32_t)site
                            && (slot->disposition == (disposition | 1u)
                                || ((action->flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED)
                                    && slot->disposition == (disposition | 2u)));
                        size_t matches = 0;
                        for (size_t j = 0; j < count; ++j) if (slots[j].action == slot->action) ++matches;
                        ok = ok && matches == 1;
                    }
                } else {
                    ok = ok && failure->actions.count == 0;
                    for (size_t i = 0; ok && i < count; ++i)
                        if ((slots[i].disposition & 4u) != 0) ok = slots[i].record == (uint32_t)site;
                }
            }
            if (ok && instance.panic_detail_length != NULL) {
                /* Unpublished capture bytes may be dirty; only the completed
                 * prefix length is observable as panic detail. */
                wasm_val_t length;
                wasm_global_get(instance.panic_detail_length, &length);
                ok = length.kind == WASM_I32 && length.of.i32 == 0;
            }
        }
        if (!ok) {
            fprintf(stderr, "P44e sweep %s/%s cap=%llu value=%lld code=%d site=%d trace=%zu\n",
                leaf, root, (unsigned long long)cap, (long long)value, code, site, count);
            for (size_t i = 0; i < pipeline.diagnostics.count; ++i)
                fprintf(stderr, "P44e build: %s\n", pipeline.diagnostics.items[i].message);
        }
        wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
        if (completed != 0) break;
    }
    if (completed != expected_steps) fprintf(stderr, "P44e boundary %s/%s steps=%llu expected=%llu\n",
        leaf, root, (unsigned long long)completed, (unsigned long long)expected_steps);
    ok = ok && expected_steps != 0 && completed == expected_steps;
    if (owner_path != NULL) ok = ok && ledger_boundaries == owner_path->ledger_boundaries;
    sol_wasm_represented_test_p44_cleanup_trace_probe(false); propagation_pipeline_free(&pipeline);
    return ok;
}

static bool p44e_step_limits_wire(void) {
    const char *directory = SOL_TEST_SOURCE_DIR "/tests/conformance/p44e_step_rows";
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    SolWasmRepresentedOutput baseline, output;
    sol_wasm_represented_output_init(&baseline); sol_wasm_represented_output_init(&output);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered, directory,
            NULL}, &baseline, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
    SolWasmRepresentedLimits zero = {0};
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, &zero}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && output.bytes.count == baseline.bytes.count
        && memcmp(output.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0;
    sol_wasm_represented_output_free(&output);
    SolWasmRepresentedLimits limits = sol_wasm_represented_default_limits(); limits.max_steps = 0;
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
        directory, &limits}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_INVALID_ARGUMENT
        && output.bytes.bytes == NULL && usage_zero(&output.usage);
    SolWasmRepresentedLimits steps_only = {.max_steps = 1};
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
        directory, &steps_only}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_INVALID_ARGUMENT
        && output.bytes.bytes == NULL && usage_zero(&output.usage);
    const uint64_t caps[] = {1, UINT64_C(100000), (UINT64_C(1) << 63) - 1,
        UINT64_C(1) << 63, UINT64_MAX - 1, UINT64_MAX};
    for (size_t i = 0; ok && i < sizeof caps / sizeof *caps; ++i) {
        char entry[256]; limits.max_steps = caps[i];
        ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, &limits}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
            && sol_wasm_represented_validate(&output.bytes) == SOL_WASM_REPRESENTED_OK
            && entry_symbol(&output.bytes, entry, sizeof entry)
            && invoke_named(&output.bytes, entry, i == 0 ? 0 : 42, i == 0 ? 6 : 0, i == 0 ? -1 : 0);
        if (ok && caps[i] == UINT64_C(100000)) ok = output.bytes.count == baseline.bytes.count
            && memcmp(output.bytes.bytes, baseline.bytes.bytes, baseline.bytes.count) == 0
            && usage_equal(&output.usage, &baseline.usage);
        if (ok && caps[i] == (UINT64_C(1) << 63)) {
            P44WireLayout high; ok = p44_wire_layout(&output.bytes, &high) && high.initial_width[7] == 10;
            const uint8_t invalid_tenth[] = {1, UINT8_C(0x7e), UINT8_C(0xff)};
            for (size_t j = 0; ok && j < sizeof invalid_tenth; ++j) {
                uint8_t *copy = malloc(output.bytes.count);
                if (copy == NULL) { ok = false; break; }
                memcpy(copy, output.bytes.bytes, output.bytes.count);
                copy[high.initial[7] + 9] = invalid_tenth[j];
                ok = sol_wasm_represented_validate(&(SolWasmBackendBytes){copy, output.bytes.count})
                    == SOL_WASM_REPRESENTED_INVALID_INPUT;
                free(copy);
            }
        }
        sol_wasm_represented_output_free(&output);
    }
    P44WireLayout wire;
    if (ok) ok = p44_wire_layout(&baseline.bytes, &wire) && wire.count >= 9;
#define P44E_WIRE_REJECT(edit) do { \
    uint8_t *copy = malloc(baseline.bytes.count); \
    if (copy == NULL) { ok = false; break; } \
    memcpy(copy, baseline.bytes.bytes, baseline.bytes.count); edit; \
    ok = ok && sol_wasm_represented_validate(&(SolWasmBackendBytes){copy, baseline.bytes.count}) \
        == SOL_WASM_REPRESENTED_INVALID_INPUT; free(copy); \
} while (0)
    if (ok) {
        P44E_WIRE_REJECT(copy[wire.type[7]] = UINT8_C(0x7f));
        P44E_WIRE_REJECT(copy[wire.mutability[7]] = 1);
        P44E_WIRE_REJECT(write_uleb_same_width(copy + wire.initial[7], wire.initial_width[7], 0));
        P44E_WIRE_REJECT(copy[wire.type[8]] = UINT8_C(0x7f));
        P44E_WIRE_REJECT(copy[wire.mutability[8]] = 0);
        P44E_WIRE_REJECT(copy[wire.initial[8]] = 1);
        P44E_WIRE_REJECT(copy[wire.initial[8]] = UINT8_C(0x80));
        /* An otherwise authorized packet alias may not expose the meter. */
        P44E_WIRE_REJECT(copy[wire.code_export_index] = 7);
        P44E_WIRE_REJECT(copy[wire.code_export_index] = 8);
    }
#undef P44E_WIRE_REJECT
    sol_wasm_represented_output_free(&output); sol_wasm_represented_output_free(&baseline);
    propagation_pipeline_free(&pipeline); return ok;
}

static bool p44e_depth_gate_probe(void) {
    const char *directory = SOL_TEST_SOURCE_DIR "/tests/conformance/p43_method_prereq";
    SolWasmRepresentedLimits limits = sol_wasm_represented_default_limits();
    SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
    char entry[256]; limits.max_call_depth = 1;
    bool ok = build_named_root(directory, "launch", &output, &limits, SOL_WASM_REPRESENTED_OK)
        && entry_symbol(&output.bytes, entry, sizeof entry)
        && invoke_named(&output.bytes, entry, 0, 7, -1);
    sol_wasm_represented_output_free(&output);
    return ok;
}

static bool p44e_depth_source_probe(void) {
    const char *leaves[] = {"p44e_depth_self", "p44e_depth_method", "p44e_depth_method_inout",
        "p44e_depth_callback", "p44e_depth_callback_inout"};
    for (size_t i = 0; i < sizeof leaves / sizeof *leaves; ++i) {
        const char *roots[] = {"launch"};
        for (size_t j = 0; j < 1; ++j) {
            char directory[512], entry[256];
            (void)snprintf(directory, sizeof directory, "%s/tests/conformance/%s", SOL_TEST_SOURCE_DIR,
                leaves[i]);
            PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
            SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
            bool ok = propagation_pipeline_build_named(&pipeline, directory, roots[j], false);
            if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                directory, NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
                && entry_symbol(&output.bytes, entry, sizeof entry)
                && invoke_named(&output.bytes, entry, 42, 0, 0);
            if (!ok) {
                fprintf(stderr, "DEPTH_SOURCE_BLOCKER %s/%s\n", leaves[i], roots[j]);
                for (size_t d = 0; d < pipeline.diagnostics.count; ++d)
                    fprintf(stderr, "%s\n", pipeline.diagnostics.items[d].message);
            }
            sol_wasm_represented_output_free(&output); propagation_pipeline_free(&pipeline);
            if (!ok) return false;
        }
    }
    return true;
}

typedef struct {
    const char *leaf, *failure_expression;
    uint32_t limit;
    int32_t code;
    bool cycle;
} P44eDepthCase;

typedef struct {
    P44TraceSlot slot;
    SolMirRuntimeCleanupActionKind kind;
    size_t target, recipe, path;
    unsigned flags;
} P44eDepthAction;
typedef struct {
    const char *leaf;
    uint32_t record, start, end, occurrence;
    size_t count;
    P44eDepthAction actions[12];
} P44eDepthLedger;
#define DEPTH_SCOPE(id, target) {{id,1,0},SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE,target,SIZE_MAX,SIZE_MAX,0}
#define DEPTH_PROP(id, record) {{id,533,record},SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE,SIZE_MAX,SIZE_MAX,SIZE_MAX,2}
#define DEPTH_PLACE(id, target, recipe, path, flags, disposition) {{id,disposition,0},SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE,target,recipe,path,flags}
/* Independent source-path witnesses, including complete semantic actions. The
 * originating LOCAL_OR_PENDING call slices in this source ingress are empty;
 * the pending RESUME_FAILURE slices below are the observable cleanup, even
 * when the packet was just produced by the same frame's local depth check. */
static const P44eDepthLedger p44e_depth_ledgers[] = {
    {"p44e_depth_self",6,232,243,1,3,{DEPTH_SCOPE(18,32),DEPTH_SCOPE(19,33),DEPTH_PROP(20,6)}},
    {"p44e_depth_method",6,650,662,0,4,{DEPTH_PLACE(0,0,0,0,0,1),DEPTH_SCOPE(1,7),
        DEPTH_SCOPE(2,8),DEPTH_PROP(3,6)}},
    {"p44e_depth_method_inout",6,434,446,0,4,{DEPTH_PLACE(1,0,0,0,0,1),DEPTH_SCOPE(2,7),
        DEPTH_SCOPE(3,8),DEPTH_PROP(4,6)}},
    {"p44e_depth_callback",6,491,503,1,3,{DEPTH_SCOPE(27,48),DEPTH_SCOPE(28,49),DEPTH_PROP(29,6)}},
    {"p44e_depth_callback_inout",6,368,383,1,4,{DEPTH_PLACE(26,9,0,5,0,1),DEPTH_SCOPE(27,59),
        DEPTH_SCOPE(28,60),DEPTH_PROP(29,6)}},
    {"p44e_depth_hole",4,352,359,0,4,{DEPTH_PLACE(14,4,3,0,4,1025),DEPTH_SCOPE(15,28),
        DEPTH_SCOPE(16,29),DEPTH_PROP(17,4)}},
    {"p44e_depth_packet",5,91,99,0,9,{DEPTH_SCOPE(0,2),DEPTH_SCOPE(1,3),DEPTH_PROP(2,5),
        DEPTH_SCOPE(5,8),DEPTH_SCOPE(6,9),DEPTH_PROP(7,5),DEPTH_SCOPE(10,14),DEPTH_SCOPE(11,15),
        DEPTH_PROP(12,5)}},
    {"p44e_depth_snapshot",7,219,233,1,8,{DEPTH_SCOPE(7,19),
        {{8,1,0},SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER,1,0,0,0},DEPTH_SCOPE(9,22),
        {{10,21,7},SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT,0,0,SIZE_MAX,0},DEPTH_PROP(11,7),
        DEPTH_SCOPE(24,37),DEPTH_SCOPE(25,38),DEPTH_PROP(26,7)}},
};
#undef DEPTH_SCOPE
#undef DEPTH_PROP
#undef DEPTH_PLACE

static bool p44e_depth_ledger_matches(const PropagationPipeline *pipeline, const char *leaf,
    const SolWasmRepresentedOutput *output, const P44TraceSlot *slots, size_t count) {
    const P44eDepthLedger *want = NULL;
    for (size_t i = 0; i < sizeof p44e_depth_ledgers / sizeof *p44e_depth_ledgers; ++i)
        if (!strcmp(leaf, p44e_depth_ledgers[i].leaf)) want = &p44e_depth_ledgers[i];
    if (want == NULL || count != want->count) return false;
    ProvenanceRecord record;
    if (!provenance_record(&output->bytes, want->record, &record) || record.tag != 3
        || record.kind != SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_CALL
        || record.start != want->start || record.end != want->end
        || record.ordinal != want->occurrence || !bytes_equal(record.path, record.path_count,"main.sol"))
        return false;
    const SolMirRuntimeCleanup *cleanup = &pipeline->cleanup;
    for (size_t i = 0; i < count; ++i) {
        const P44eDepthAction *a = &want->actions[i];
        if (memcmp(&slots[i], &a->slot, sizeof a->slot) || a->slot.action >= cleanup->action_count)
            return false;
        const SolMirRuntimeCleanupAction *actual = &cleanup->actions[a->slot.action];
        if (actual->kind != a->kind || actual->target != a->target || actual->recipe != a->recipe
            || actual->drop_path != a->path || actual->flags != a->flags) return false;
    }
    /* Authenticate every call's exact local/pending branch and resume. Never
     * manufacture a nonempty local slice merely to make tracing observable. */
    const SolMirMaterialization *m = &pipeline->concrete.materialization;
    for (size_t i = 0; i < pipeline->conventions.call_count; ++i) {
        const SolMirRuntimeCall *call = &pipeline->conventions.calls[i];
        if (call->owner_kind != SOL_MIR_RUNTIME_CALL_OWNER_IMAGE) continue;
        const SolMirRuntimeCleanupEvent *event = &cleanup->events[
            pipeline->lowered.image_terminators[call->block].cleanup_event];
        const SolMirRuntimeCleanupTransition *failure = NULL;
        for (size_t j = 0; j < event->transitions.count; ++j) {
            const SolMirRuntimeCleanupTransition *t = &cleanup->transitions[event->transitions.offset+j];
            if (t->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE) failure = t;
        }
        if (failure == NULL || failure->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING
            || failure->failure_site != call->failure_site || failure->failure_mask != 64
            || failure->actions.count != 0 || failure->source_edge != call->failure_edge
            || failure->continuation != call->failure_edge || !failure->primary_failure_wins) return false;
        const SolMirMaterializedTerminator *resume = &m->blocks[m->edges[call->failure_edge].block].terminator;
        if (resume->kind != SOL_MIR_TERM_RESUME_FAILURE) return false;
    }
    if (!strcmp(leaf,"p44e_depth_snapshot")) {
        const SolMirOperations *o = &pipeline->concrete.operations;
        /* DROP_SNAPSHOT targets the global PLAN, not its image-local slot. */
        if (o->snapshot_count != 1 || o->snapshots[0].image != 1 || o->snapshots[0].slot != 0
            || cleanup->actions[10].target != 0 || cleanup->actions[11].kind
                != SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE) return false;
        const SolMirRuntimeCleanupTransition *t = &cleanup->transitions[16];
        const SolMirRuntimeCleanupTransition *root = &cleanup->transitions[4];
        if (t->event != 12 || t->actions.offset != 10 || t->actions.count != 2
            || t->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING
            || root->actions.offset != 2 || root->actions.count != 1
            || root->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING) return false;
    }
    return true;
}

static bool p44e_depth_case(const P44eDepthCase *want, bool trace) {
    char directory[512], entry[256];
    (void)snprintf(directory, sizeof directory, "%s/tests/conformance/%s", SOL_TEST_SOURCE_DIR,
        want->leaf);
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
    SolWasmRepresentedLimits limits = sol_wasm_represented_default_limits();
    limits.max_call_depth = want->limit;
    sol_wasm_represented_test_p44_cleanup_trace_probe(trace);
    bool ok = propagation_pipeline_build(&pipeline, directory);
    SolWasmRepresentedTestCallCatalog catalog = {0};
    if (ok) ok = sol_wasm_represented_test_call_catalog(&(SolWasmRepresentedBuildRequest){
        &pipeline.lowered, directory, &limits}, NULL, 0, &catalog) == SOL_WASM_REPRESENTED_OK
        && catalog.has_cycle == want->cycle
        && (!want->cycle || catalog.longest_chain == 0)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, &limits}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && sol_wasm_represented_validate(&output.bytes) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry);
    if (ok && strstr(want->leaf,"p44e_depth_chain") != NULL) {
        size_t edges = (size_t)strtoul(want->leaf + strlen("p44e_depth_chain"), NULL, 10);
        ok = catalog.calls == edges && catalog.edges == edges && catalog.longest_chain == edges;
    }
    uint32_t expected_record = 0;
    if (ok && want->code != 0) {
        const char *source = pipeline.package.source.text;
        const char *expression = strstr(source, want->failure_expression);
        /* Match a preselected SOURCE occurrence, never an observed PacketID. */
        size_t expression_length = strlen(want->failure_expression);
        if (expression == NULL) ok = false;
        if (ok && !strncmp(want->failure_expression, "return ", 7)) {
            expression += 7; expression_length -= 7;
        }
        for (uint32_t id = 1; ok && id <= output.usage.provenance_records; ++id) {
            ProvenanceRecord p;
            ok = provenance_record(&output.bytes, id, &p);
            if (ok && p.tag == 3 && p.kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_CALL
                && p.start == (size_t)(expression - source)
                && p.end == p.start + expression_length) {
                if (expected_record != 0) ok = false;
                expected_record = id;
            }
        }
        ok = ok && expected_record != 0;
    }
    WasmInstance instance = {0};
    if (ok) ok = wasm_instance_open(&output.bytes, entry, &instance);
    for (size_t repeat = 0; ok && repeat < 2; ++repeat) {
        ok = wasm_instance_call(&instance, want->code == 0 ? 42 : 0, want->code,
            (int32_t)expected_record);
        if (ok && trace) {
            P44TraceSlot slots[64]; size_t count = 0; bool overflow = false;
            ok = wasm_instance_trace(&instance, slots, 64, &count, &overflow) && !overflow
                && p44e_depth_ledger_matches(&pipeline, want->leaf, &output, slots, count);
        }
    }
    if (ok) {
        SolIrCallableId root=SOL_IR_NONE;
        for (size_t i=0; i<pipeline.ir.callable_count; ++i)
            if (pipeline.ir.callables[i].kind==SOL_IR_CALLABLE_FUNCTION
                && !strcmp(pipeline.ir.callables[i].name,"launch")) root=i;
        SolInterpreterResult interpreted; sol_interpreter_result_init(&interpreted);
        bool success=sol_interpret(&(SolInterpreterRequest){.ir=&pipeline.ir,.callable=root,
            .definition=SOL_IR_NONE,.contracts=SOL_INTERPRETER_CONTRACTS_CHECK,
            .limits={100000,want->limit,100000,1048576,10000}},&interpreted);
        if (want->code==0) ok=success && interpreted.value.kind==SOL_INTERPRETER_VALUE_INT64
            && interpreted.value.as.integer==42;
        else {
            ProvenanceRecord p;
            ok=!success && interpreted.diagnostic.code==SOL_INTERPRETER_CALL_DEPTH_LIMIT
                && provenance_record(&output.bytes,expected_record,&p)
                && interpreted.diagnostic.span.start==p.start && interpreted.diagnostic.span.end==p.end;
        }
        sol_interpreter_result_free(&interpreted);
    }
    if (!ok) {
        fprintf(stderr, "DEPTH_CASE_FAILED %s limit=%u record=%u\n", want->leaf, want->limit,
            expected_record);
        for (size_t d = 0; d < pipeline.diagnostics.count; ++d)
            fprintf(stderr, "%s\n", pipeline.diagnostics.items[d].message);
    }
    wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    propagation_pipeline_free(&pipeline); sol_wasm_represented_test_p44_cleanup_trace_probe(false);
    return ok;
}

/* Splice an independently specified signed immediate, updating the global
 * section payload length (which remains ULEB). Do not reuse the producer's
 * encoder or the shape validator's signed decoder in this raw regression. */
static bool p44e_replace_global_initial(const SolWasmBackendBytes *source, size_t global,
    const uint8_t *encoding, size_t width, SolWasmBackendBytes *result) {
    P44WireLayout wire;
    if (!p44_wire_layout(source,&wire) || global>=wire.count) return false;
    const uint8_t *cursor=source->bytes+8,*end=source->bytes+source->count;
    while (cursor<end) {
        uint8_t id=*cursor++;
        size_t size_at=(size_t)(cursor-source->bytes);
        uint32_t size=0;
        if (!read_uleb32(&cursor,end,&size) || size>(size_t)(end-cursor)) return false;
        const uint8_t *section_end=cursor+size;
        if (id==6) {
            size_t old_width=wire.initial_width[global],at=wire.initial[global];
            if (old_width>size || width>UINT32_MAX-size || at<(size_t)(cursor-source->bytes)
                || at+old_width>(size_t)(section_end-source->bytes)) return false;
            result->count=source->count-old_width+width;
            result->bytes=malloc(result->count);
            if (result->bytes==NULL) { result->count=0; return false; }
            memcpy(result->bytes,source->bytes,at);
            memcpy(result->bytes+at,encoding,width);
            memcpy(result->bytes+at+width,source->bytes+at+old_width,source->count-at-old_width);
            bool ok=write_uleb_same_width(result->bytes+size_at,
                (size_t)(cursor-source->bytes)-size_at,size-(uint32_t)old_width+(uint32_t)width);
            if (!ok) { free(result->bytes); *result=(SolWasmBackendBytes){0}; }
            return ok;
        }
        cursor=section_end;
    }
    return false;
}

static bool p44e_depth_signed_initializers(void) {
    SolWasmRepresentedOutput baseline; sol_wasm_represented_output_init(&baseline);
    char entry[256]; P44WireLayout wire;
    bool ok=build_named_root(SOL_TEST_SOURCE_DIR "/tests/conformance/p44e_depth_self_65","launch",
        &baseline,NULL,SOL_WASM_REPRESENTED_OK) && p44_wire_layout(&baseline.bytes,&wire)
        && wire.initial_width[9]==2 && baseline.bytes.bytes[wire.initial[9]-1]==UINT8_C(0x41)
        && baseline.bytes.bytes[wire.initial[9]]==UINT8_C(0xc0)
        && baseline.bytes.bytes[wire.initial[9]+1]==0
        && baseline.bytes.bytes[wire.initial[9]+2]==UINT8_C(0x0b)
        && entry_symbol(&baseline.bytes,entry,sizeof entry)
        && invoke_named(&baseline.bytes,entry,0,7,4);
    static const struct {
        const char *name;
        size_t width;
        uint8_t bytes[6];
        bool well_formed, admitted;
        int32_t value;
    } cases[]={
        {"minus64-short",1,{0x40},true,false,-64},
        {"minus1",1,{0x7f},true,false,-1},
        {"minus32",1,{0x60},true,false,-32},
        {"minus63",1,{0x41},true,false,-63},
        {"minus65",2,{0xbf,0x7f},true,false,-65},
        {"minus128",2,{0x80,0x7f},true,false,-128},
        {"int32-min",5,{0x80,0x80,0x80,0x80,0x78},true,false,INT32_MIN},
        {"int32-max",5,{0xff,0xff,0xff,0xff,0x07},true,false,INT32_MAX},
        {"zero",1,{0x00},true,false,0},
        {"one",1,{0x01},true,true,1},
        {"sixty-three",1,{0x3f},true,true,63},
        {"sixty-four",2,{0xc0,0x00},true,true,64},
        {"sixty-five",2,{0xc1,0x00},true,false,65},
        {"one-padded",2,{0x81,0x00},true,true,1},
        {"one-five",5,{0x81,0x80,0x80,0x80,0x00},true,true,1},
        {"sixty-four-five",5,{0xc0,0x80,0x80,0x80,0x00},true,true,64},
        {"minus64-five",5,{0xc0,0xff,0xff,0xff,0x7f},true,false,-64},
        {"minus1-five",5,{0xff,0xff,0xff,0xff,0x7f},true,false,-1},
        {"fifth-positive-unused",5,{0x81,0x80,0x80,0x80,0x10},false,false,0},
        {"fifth-missing-sign-extension",5,{0x80,0x80,0x80,0x80,0x08},false,false,0},
        {"fifth-negative-unused",5,{0x80,0x80,0x80,0x80,0x77},false,false,0},
        {"six-byte",6,{0x81,0x80,0x80,0x80,0x80,0x00},false,false,0},
        {"unterminated",5,{0x81,0x80,0x80,0x80,0x80},false,false,0},
        {"empty",0,{0},false,false,0},
    };
    for (size_t i=0; ok && i<sizeof cases/sizeof *cases; ++i) {
        SolWasmBackendBytes mutated={0};
        ok=p44e_replace_global_initial(&baseline.bytes,9,cases[i].bytes,cases[i].width,&mutated);
        if (ok) {
            SolWasmRepresentedResult expected=cases[i].admitted?SOL_WASM_REPRESENTED_OK
                :SOL_WASM_REPRESENTED_INVALID_INPUT;
            ok=sol_wasm_represented_validate(&mutated)==expected;
            wasm_engine_t *engine=wasm_engine_new();
            wasm_store_t *store=engine==NULL?NULL:wasm_store_new(engine);
            wasm_byte_vec_t bytes={mutated.count,(wasm_byte_t *)mutated.bytes};
            ok=ok && store!=NULL && wasm_module_validate(store,&bytes)==cases[i].well_formed;
            if (store!=NULL) wasm_store_delete(store);
            if (engine!=NULL) wasm_engine_delete(engine);
        }
        if (ok && i==0) {
            /* The exact P1 reproduction removes one byte: 41 c0 00 0b ->
             * 41 40 0b. Wasmtime runs 65 frames without code 7 because -64
             * becomes a huge unsigned threshold in the runtime guard. The
             * represented validator must reject it BEFORE engine execution. */
            ok=mutated.count+1==baseline.bytes.count && invoke_named(&mutated,entry,42,0,0);
        }
        if (ok && cases[i].well_formed) {
            /* Engine-only alias observes the actual signed initializer. This
             * deliberately invalid envelope is never a production export. */
            size_t delta=wire.initial_width[9];
            size_t export_at=wire.code_export_index-delta+cases[i].width;
            ok=mutated.bytes[export_at]==0;
            mutated.bytes[export_at]=9;
            WasmInstance instance={0};
            if (ok) ok=wasm_instance_open(&mutated,entry,&instance);
            if (ok) {
                wasm_val_t actual; wasm_global_get(instance.code,&actual);
                ok=actual.kind==WASM_I32 && actual.of.i32==cases[i].value;
            }
            wasm_instance_close(&instance);
        }
        if (!ok) fprintf(stderr,"DEPTH_SIGNED_INITIALIZER_FAILED %s\n",cases[i].name);
        free(mutated.bytes);
    }
    /* Current depth has the same signed grammar but only value 1 is legal. */
    const uint8_t current_negative[]={0x7f},current_padded[]={0x81,0x80,0x80,0x80,0x00};
    SolWasmBackendBytes mutated={0};
    if (ok) ok=p44e_replace_global_initial(&baseline.bytes,10,current_negative,sizeof current_negative,&mutated)
        && sol_wasm_represented_validate(&mutated)==SOL_WASM_REPRESENTED_INVALID_INPUT;
    free(mutated.bytes); mutated=(SolWasmBackendBytes){0};
    if (ok) ok=p44e_replace_global_initial(&baseline.bytes,10,current_padded,sizeof current_padded,&mutated)
        && sol_wasm_represented_validate(&mutated)==SOL_WASM_REPRESENTED_OK;
    free(mutated.bytes); sol_wasm_represented_output_free(&baseline); return ok;
}

static bool p44e_depth_limits_wire(void) {
    const char *directory = SOL_TEST_SOURCE_DIR "/tests/conformance/p44e_depth_self";
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    SolWasmRepresentedOutput baseline, output;
    sol_wasm_represented_output_init(&baseline); sol_wasm_represented_output_init(&output);
    bool ok = propagation_pipeline_build(&pipeline,directory)
        && sol_wasm_represented_default_limits().max_call_depth == 64
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,directory,
            NULL},&baseline,&pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
    SolWasmRepresentedLimits zero = {0};
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
        directory,&zero},&output,&pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && usage_equal(&output.usage,&baseline.usage) && output.bytes.count == baseline.bytes.count
        && memcmp(output.bytes.bytes,baseline.bytes.bytes,baseline.bytes.count) == 0;
    sol_wasm_represented_output_free(&output);
    const uint32_t invalid[] = {0,65,UINT32_MAX};
    for (size_t i = 0; ok && i < sizeof invalid / sizeof *invalid; ++i) {
        SolWasmRepresentedLimits limits = sol_wasm_represented_default_limits();
        limits.max_call_depth = invalid[i];
        ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,directory,
            &limits},&output,&pipeline.diagnostics) == SOL_WASM_REPRESENTED_INVALID_ARGUMENT
            && output.bytes.bytes == NULL && output.bytes.count == 0 && usage_zero(&output.usage);
    }
    SolWasmRepresentedLimits depth_only = {.max_call_depth=1};
    if (ok) ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,directory,
        &depth_only},&output,&pipeline.diagnostics) == SOL_WASM_REPRESENTED_INVALID_ARGUMENT
        && output.bytes.bytes == NULL && usage_zero(&output.usage);
    P44WireLayout wire;
    if (ok) ok = p44_wire_layout(&baseline.bytes,&wire) && wire.count >= 11
        && baseline.bytes.bytes[wire.type[9]] == UINT8_C(0x7f)
        && baseline.bytes.bytes[wire.mutability[9]] == 0 && wire.initial_value[9] == 64
        && baseline.bytes.bytes[wire.type[10]] == UINT8_C(0x7f)
        && baseline.bytes.bytes[wire.mutability[10]] == 1 && wire.initial_value[10] == 1;
#define DEPTH_WIRE_REJECT(edit) do { \
    uint8_t *copy = malloc(baseline.bytes.count); \
    if (copy == NULL) { ok = false; break; } \
    memcpy(copy,baseline.bytes.bytes,baseline.bytes.count); edit; \
    ok = ok && sol_wasm_represented_validate(&(SolWasmBackendBytes){copy,baseline.bytes.count}) \
        == SOL_WASM_REPRESENTED_INVALID_INPUT; free(copy); \
} while (0)
    if (ok) {
        DEPTH_WIRE_REJECT(copy[wire.type[9]]=UINT8_C(0x7e));
        DEPTH_WIRE_REJECT(copy[wire.mutability[9]]=1);
        DEPTH_WIRE_REJECT(write_uleb_same_width(copy+wire.initial[9],wire.initial_width[9],0));
        DEPTH_WIRE_REJECT(write_uleb_same_width(copy+wire.initial[9],wire.initial_width[9],65));
        DEPTH_WIRE_REJECT(copy[wire.type[10]]=UINT8_C(0x7e));
        DEPTH_WIRE_REJECT(copy[wire.mutability[10]]=0);
        DEPTH_WIRE_REJECT(copy[wire.initial[10]]=0);
        DEPTH_WIRE_REJECT(copy[wire.initial[10]]=2);
        DEPTH_WIRE_REJECT(copy[wire.initial[9]]=UINT8_C(0xff); copy[wire.initial[9]+1]=UINT8_C(0xff));
        DEPTH_WIRE_REJECT(copy[wire.initial[10]]=UINT8_C(0x80));
        for (uint8_t index=7; index<=10; ++index)
            DEPTH_WIRE_REJECT(copy[wire.code_export_index]=index);
    }
#undef DEPTH_WIRE_REJECT
    sol_wasm_represented_output_free(&output); sol_wasm_represented_output_free(&baseline);
    propagation_pipeline_free(&pipeline); return ok;
}

static bool p44e_depth_owner_mutations(void) {
    const char *directory = SOL_TEST_SOURCE_DIR "/tests/conformance/p44e_depth_snapshot";
    PropagationPipeline p; propagation_pipeline_init(&p);
    SolWasmRepresentedOutput baseline; sol_wasm_represented_output_init(&baseline);
    bool ok = propagation_pipeline_build(&p,directory)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&p.lowered,directory,NULL},
            &baseline,&p.diagnostics) == SOL_WASM_REPRESENTED_OK;
    if (!ok) { propagation_pipeline_free(&p); return false; }
    /* The source witness is checked->forever(value), never the observed packet. */
    SolMirRuntimeCall *call = &p.conventions.calls[1];
    SolMirRuntimeLoweredCall *lowered = &p.lowered.calls[1];
    SolMirRuntimeFailureSite *site = &p.conventions.failure_sites[call->failure_site];
    SolMirRuntimeCleanupEvent *event = &p.cleanup.events[7];
    SolMirRuntimeCleanupTransition *failure = &p.cleanup.transitions[9];
    SolMirRuntimeCleanupTransition *resume = &p.cleanup.transitions[16];
    SolMirMaterializedTerminator *term = &p.concrete.materialization.blocks[5].terminator;
    SolMirRuntimeFailureSite saved_site = *site;
    SolMirRuntimeCleanupEvent saved_event = *event;
    SolMirRuntimeCleanupTransition saved_failure = *failure, saved_resume = *resume;
    SolMirRuntimeCall saved_call = *call;
    SolMirRuntimeLoweredCall saved_lowered = *lowered;
    SolMirMaterializedTerminator saved_term = *term;
#define DEPTH_OWNER_REJECT(edit,restore) do { \
    SolWasmRepresentedOutput rejected,retry; \
    sol_wasm_represented_output_init(&rejected); sol_wasm_represented_output_init(&retry); \
    edit; \
    /* A stale seal and a resealed hostile owner must both fail closed. */ \
    bool stale = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&p.lowered,directory, \
        NULL},&rejected,&p.diagnostics) == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE \
        && rejected.bytes.bytes == NULL && usage_zero(&rejected.usage); \
    p.lowered.authentication=sol_mir_runtime_lowered_program_test_seal(&p.lowered); \
    bool sealed = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&p.lowered,directory, \
        NULL},&rejected,&p.diagnostics) == SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE \
        && rejected.bytes.bytes == NULL && rejected.bytes.count == 0 && usage_zero(&rejected.usage); \
    restore; p.lowered.authentication=sol_mir_runtime_lowered_program_test_seal(&p.lowered); \
    bool restored = sol_mir_runtime_lowered_program_validate(&p.lowered,NULL) \
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&p.lowered,directory,NULL}, \
            &retry,&p.diagnostics) == SOL_WASM_REPRESENTED_OK \
        && usage_equal(&retry.usage,&baseline.usage) && retry.bytes.count == baseline.bytes.count \
        && memcmp(retry.bytes.bytes,baseline.bytes.bytes,baseline.bytes.count) == 0; \
    if (!stale || !sealed || !restored) fprintf(stderr,"DEPTH_OWNER_REJECT failed: %s\n",#edit); \
    ok = ok && stale && sealed && restored; \
    sol_wasm_represented_output_free(&retry); sol_wasm_represented_output_free(&rejected); \
} while (0)
    DEPTH_OWNER_REJECT(site->origin_kind=SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_STEP,*site=saved_site);
    DEPTH_OWNER_REJECT(site->owner^=1,*site=saved_site);
    DEPTH_OWNER_REJECT(site->block^=1,*site=saved_site);
    DEPTH_OWNER_REJECT(site->allowed_codes=32,*site=saved_site);
    DEPTH_OWNER_REJECT(event->origin=SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT,*event=saved_event);
    DEPTH_OWNER_REJECT(event->owner^=1,*event=saved_event);
    DEPTH_OWNER_REJECT(event->block^=1,*event=saved_event);
    DEPTH_OWNER_REJECT(failure->failure_source=SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING,
        *failure=saved_failure);
    DEPTH_OWNER_REJECT(failure->failure_site=SOL_MIR_RUNTIME_NONE,*failure=saved_failure);
    DEPTH_OWNER_REJECT(failure->failure_mask=32,*failure=saved_failure);
    DEPTH_OWNER_REJECT(failure->source_edge^=1,*failure=saved_failure);
    DEPTH_OWNER_REJECT(failure->continuation^=1,*failure=saved_failure);
    DEPTH_OWNER_REJECT(resume->failure_source=SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING,
        *resume=saved_resume);
    DEPTH_OWNER_REJECT(term->kind=SOL_MIR_TERM_RETURN,*term=saved_term);
    DEPTH_OWNER_REJECT(call->failure_site=SOL_MIR_RUNTIME_NONE,*call=saved_call);
    DEPTH_OWNER_REJECT(lowered->failure_site=SOL_MIR_RUNTIME_NONE,*lowered=saved_lowered);
#undef DEPTH_OWNER_REJECT
    sol_wasm_represented_output_free(&baseline); propagation_pipeline_free(&p); return ok;
}

static bool p44e_depth_resources(size_t first, size_t last) {
    const char *directory=SOL_TEST_SOURCE_DIR "/tests/conformance/p44e_depth_reset";
    static const SolWasmRepresentedUsage expected={7,13,10,20,61,1502,0,24,0,0,88,44757,19225,25250,15010};
    static const uint8_t expected_hash[32]={0x73,0x43,0xcc,0x8e,0xc5,0x52,0x1f,0x35,0x75,0xb5,0x0f,0x51,0x28,0x0c,0xab,0x22,0xc9,0x0e,0xdc,0x3f,0x6c,0x52,0x31,0xad,0x4c,0x47,0x0f,0x92,0x1c,0x6d,0x37,0x70};
    SolWasmRepresentedOutput baseline,output; size_t ids[2]; uint8_t hash[32];
    sol_wasm_represented_output_init(&baseline); sol_wasm_represented_output_init(&output);
    bool ok=build_multiroot(directory,false,&baseline,ids,NULL,SOL_WASM_REPRESENTED_OK)
        && sol_wasm_represented_test_allocation_attempts()==151;
    const size_t baseline_ids[2]={ids[0],ids[1]};
    if (ok) {
        sha256(baseline.bytes.bytes,baseline.bytes.count,hash);
        ok=usage_equal(&baseline.usage,&expected) && memcmp(hash,expected_hash,sizeof hash)==0;
    }
#define DEPTH_CAP(field,value) do { \
    SolWasmRepresentedLimits limits=sol_wasm_represented_default_limits(); limits.field=(value); \
    bool exact=build_multiroot(directory,false,&output,ids,&limits,SOL_WASM_REPRESENTED_OK) \
        && usage_equal(&output.usage,&expected) && output.bytes.count==baseline.bytes.count \
        && memcmp(output.bytes.bytes,baseline.bytes.bytes,baseline.bytes.count)==0; \
    sol_wasm_represented_output_free(&output); --limits.field; \
    bool below=build_multiroot(directory,false,&output,ids,&limits,SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED) \
        && output.bytes.bytes==NULL && output.bytes.count==0 && usage_zero(&output.usage); \
    sol_wasm_represented_output_free(&output); ok=ok && exact && below; \
} while (0)
    if (ok) {
        DEPTH_CAP(max_functions,expected.functions); DEPTH_CAP(max_blocks,expected.blocks);
        DEPTH_CAP(max_edges,expected.edges); DEPTH_CAP(max_values,expected.values);
        DEPTH_CAP(max_locals,expected.locals); DEPTH_CAP(max_generated_nodes,expected.generated_nodes);
        DEPTH_CAP(max_static_data_bytes,expected.static_data_bytes);
        DEPTH_CAP(max_provenance_records,expected.provenance_records);
        DEPTH_CAP(max_work_bytes,expected.work_bytes); DEPTH_CAP(max_scratch_bytes,expected.scratch_bytes);
        DEPTH_CAP(max_owned_bytes,expected.owned_bytes); DEPTH_CAP(max_output_bytes,expected.output_bytes);
    }
#undef DEPTH_CAP
    SolWasmRepresentedLimits zero={0};
    if (ok) ok=build_multiroot(directory,false,&output,ids,&zero,SOL_WASM_REPRESENTED_OK)
        && output.bytes.count==baseline.bytes.count
        && memcmp(output.bytes.bytes,baseline.bytes.bytes,baseline.bytes.count)==0;
    sol_wasm_represented_output_free(&output);
    if (first==0) { first=1; last=151; }
    for (size_t ordinal=first; ok && ordinal<=last; ++ordinal) {
        represented_fault(FAULT_DEPTH,ordinal,ordinal);
        ok=build_multiroot(directory,false,&output,ids,NULL,SOL_WASM_REPRESENTED_ALLOCATION_FAILED)
            && output.bytes.bytes==NULL && output.bytes.count==0 && usage_zero(&output.usage);
        sol_wasm_represented_output_free(&output); sol_wasm_represented_test_fail_allocation_after(0);
        if (ok) ok=build_multiroot(directory,false,&output,ids,NULL,SOL_WASM_REPRESENTED_OK)
            && sol_wasm_represented_test_allocation_attempts()==151
            && usage_equal(&output.usage,&expected) && output.bytes.count==baseline.bytes.count
            && memcmp(output.bytes.bytes,baseline.bytes.bytes,baseline.bytes.count)==0;
        sol_wasm_represented_output_free(&output);
    }
    if (ok) ok=build_multiroot(directory,true,&output,ids,NULL,SOL_WASM_REPRESENTED_OK)
        && ids[0]==baseline_ids[0] && ids[1]==baseline_ids[1]
        && output.bytes.count==baseline.bytes.count && usage_equal(&output.usage,&expected)
        && memcmp(output.bytes.bytes,baseline.bytes.bytes,baseline.bytes.count)==0;
    sol_wasm_represented_output_free(&output);
    char relocation[512],source[768],destination[768];
    (void)snprintf(relocation,sizeof relocation,"%s/p44e_depth_relocated",SOL_TEST_BINARY_DIR);
    (void)mkdir(relocation,0700);
    (void)snprintf(source,sizeof source,"%s/main.sol",directory);
    (void)snprintf(destination,sizeof destination,"%s/main.sol",relocation);
    FILE *input=fopen(source,"rb"),*copy=fopen(destination,"wb");
    ok=ok && input!=NULL && copy!=NULL;
    if (input!=NULL && copy!=NULL) {
        uint8_t bytes[256]; size_t count;
        while ((count=fread(bytes,1,sizeof bytes,input))!=0)
            if (fwrite(bytes,1,count,copy)!=count) ok=false;
    }
    if (input!=NULL) fclose(input); if (copy!=NULL) fclose(copy);
    if (ok) ok=build_multiroot(relocation,false,&output,ids,NULL,SOL_WASM_REPRESENTED_OK)
        && ids[0]==baseline_ids[0] && ids[1]==baseline_ids[1]
        && usage_equal(&output.usage,&expected) && output.bytes.count==baseline.bytes.count
        && memcmp(output.bytes.bytes,baseline.bytes.bytes,baseline.bytes.count)==0;
    sol_wasm_represented_output_free(&output);
    /* Flip only the first immutable literal's payload in exported memory. This
     * chooses the source's success arm on the SAME instance; the comparison's
     * distinct literal remains "fail". No depth hook or exported counter exists. */
    char entry[256]; WasmInstance instance={0};
    ProvenanceRecord failure;
    if (ok) ok=provenance_record(&baseline.bytes,5,&failure)
        && failure.tag==3 && failure.kind==SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_CALL
        && failure.start==142 && failure.end==160 && failure.ordinal==0
        && bytes_equal(failure.path,failure.path_count,"main.sol");
    if (ok) ok=entry_symbol(&baseline.bytes,entry,sizeof entry)
        && wasm_instance_open(&baseline.bytes,entry,&instance)
        && wasm_instance_call(&instance,0,7,5);
    if (ok) {
        uint8_t *memory=(uint8_t *)wasm_memory_data(instance.memory);
        size_t at=0;
        while (at+4<=64 && memcmp(memory+at,"fail",4)!=0) ++at;
        ok=at+4<=64;
        for (size_t repeat=0; ok && repeat<3; ++repeat) {
            memcpy(memory+at,"pass",4);
            ok=wasm_instance_call(&instance,43,0,0) && wasm_instance_call(&instance,43,0,0);
            memcpy(memory+at,"fail",4);
            ok=ok && wasm_instance_call(&instance,0,7,5);
        }
    }
    wasm_instance_close(&instance); sol_wasm_represented_output_free(&baseline); return ok;
}

static bool p44e_depth_step_precedence(void) {
    const char *directory=SOL_TEST_SOURCE_DIR "/tests/conformance/p44e_depth_packet";
    PropagationPipeline p; propagation_pipeline_init(&p);
    bool ok=propagation_pipeline_build(&p,directory);
    static const P44TraceSlot step_trace[]={{18,13,15},{19,13,15},{20,525,15},
        {5,1,0},{6,1,0},{7,533,15},{10,1,0},{11,1,0},{12,533,15}};
    static const P44TraceSlot depth_trace[]={{0,1,0},{1,1,0},{2,533,5},
        {5,1,0},{6,1,0},{7,533,5},{10,1,0},{11,1,0},{12,533,5}};
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    for (uint64_t cap=8; ok && cap<=10; ++cap) {
        SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
        SolWasmRepresentedLimits limits=sol_wasm_represented_default_limits();
        limits.max_call_depth=3; limits.max_steps=cap;
        char entry[256]; WasmInstance instance={0};
        ok=sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&p.lowered,directory,&limits},
            &output,&p.diagnostics)==SOL_WASM_REPRESENTED_OK
            && entry_symbol(&output.bytes,entry,sizeof entry)
            && wasm_instance_open(&output.bytes,entry,&instance);
        for (size_t repeat=0; ok && repeat<2; ++repeat) {
            P44TraceSlot slots[64]; size_t count=0; bool overflow=false;
            ok=wasm_instance_call(&instance,0,cap==8?6:7,cap==8?15:5)
                && wasm_instance_trace(&instance,slots,64,&count,&overflow) && !overflow && count==9
                && memcmp(slots,cap==8?step_trace:depth_trace,sizeof step_trace)==0;
        }
        ProvenanceRecord site;
        if (ok) ok=provenance_record(&output.bytes,cap==8?15u:5u,&site)
            && site.tag==3 && site.kind==(cap==8?SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_STEP
                :SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_CALL);
        wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    }
    sol_wasm_represented_test_p44_cleanup_trace_probe(false); propagation_pipeline_free(&p); return ok;
}

static bool p44e_depth_pending_packets(void) {
    const char *directory=SOL_TEST_SOURCE_DIR "/tests/conformance/p44e_depth_pending";
    PropagationPipeline p; propagation_pipeline_init(&p);
    bool ok=propagation_pipeline_build_named(&p,directory,"launch",true);
    if (ok) {
        const SolMirRuntimeCleanupAction *a=&p.cleanup.actions[23];
        ok=a[0].kind==SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE && a[0].target==40
            && a[1].kind==SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE && a[1].target==41
            && a[2].kind==SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE && a[2].target==SIZE_MAX;
        for (size_t i=0; ok && i<3; ++i)
            ok=a[i].recipe==SIZE_MAX && a[i].drop_path==SIZE_MAX && a[i].flags==(i==2?2u:0u);
        /* The fail root has an empty originating LOCAL_OR_PENDING slice and a
         * distinct terminal pending slice; an old packet never becomes local. */
        const SolMirRuntimeCall *call=&p.conventions.calls[2];
        const SolMirRuntimeCleanupEvent *event=&p.cleanup.events[
            p.lowered.image_terminators[call->block].cleanup_event];
        const SolMirRuntimeCleanupTransition *failure=&p.cleanup.transitions[event->transitions.offset+1];
        ok=ok && failure->failure_source==SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING
            && failure->failure_site==call->failure_site && failure->failure_mask==64
            && failure->actions.count==0 && failure->source_edge==call->failure_edge;
    }
    sol_wasm_represented_test_callback_writeback_probe(true);
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    for (size_t kind=0; ok && kind<2; ++kind) {
        SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
        SolWasmRepresentedLimits limits=sol_wasm_represented_default_limits();
        limits.max_call_depth=1; limits.max_steps=kind==0?100000:1;
        char entry[256]; WasmInstance instance={0};
        int32_t code=kind==0?7:6,site=kind==0?7:43;
        ok=sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&p.lowered,directory,&limits},
            &output,&p.diagnostics)==SOL_WASM_REPRESENTED_OK
            && sol_wasm_represented_validate(&output.bytes)==SOL_WASM_REPRESENTED_OK
            && entry_symbol(&output.bytes,entry,sizeof entry)
            && wasm_instance_open(&output.bytes,entry,&instance);
        for (size_t repeat=0; ok && repeat<3; ++repeat) {
            ok=wasm_instance_call(&instance,0,code,site) && wasm_instance_writebacks(&instance,0);
            /* The preexisting direct test export is intentionally not an entry:
             * clear ONLY the observable ledger, not the packet or depth. */
            wasm_val_t zero=WASM_I32_VAL(0);
            if (ok) {
                wasm_global_set(instance.trace_count,&zero); wasm_global_set(instance.trace_overflow,&zero);
                ok=wasm_instance_call_named(&instance,SOL_WASM_REPRESENTED_TEST_FAILURE_ENTRY_EXPORT,
                    0,code,site) && wasm_instance_writebacks(&instance,0);
            }
            P44TraceSlot actual[64]; size_t count=0; bool overflow=false;
            const P44TraceSlot expected[]={{23,1,0},{24,1,0},{25,533,(uint32_t)site}};
            if (ok) ok=wasm_instance_trace(&instance,actual,64,&count,&overflow) && !overflow && count==3
                && memcmp(actual,expected,sizeof expected)==0;
            /* The failed call has its OWN canonical record 8. Keeping 7/43,
             * with no callee trace, proves both no invocation and no overwrite. */
            ProvenanceRecord own;
            if (ok) ok=provenance_record(&output.bytes,8,&own)
                && own.kind==SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_CALL && own.start==307 && own.end==317;
            if (ok && kind==0) {
                /* No entry reset and no depth hook: after clearing ONLY the
                 * packet, depth 1 must still deny this distinct call. Neither
                 * a local denial nor a preexisting-packet route may decrement. */
                wasm_global_set(instance.code,&zero); wasm_global_set(instance.site,&zero);
                ok=wasm_instance_call_named(&instance,SOL_WASM_REPRESENTED_TEST_FAILURE_ENTRY_EXPORT,
                    0,7,8);
            }
        }
        wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    }
    if (ok) {
        SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
        char entry[256]; WasmInstance instance={0};
        ok=sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&p.lowered,directory,NULL},
            &output,&p.diagnostics)==SOL_WASM_REPRESENTED_OK && entry_symbol(&output.bytes,entry,sizeof entry)
            && wasm_instance_open(&output.bytes,entry,&instance) && wasm_instance_call(&instance,0,7,5);
        if (ok) {
            /* All 63 successfully entered callees have returned through their
             * failure paths. Clearing the packet but NOT depth/steps permits
             * the raw fixture's one-frame success only if those calls balanced. */
            wasm_val_t zero=WASM_I32_VAL(0);
            wasm_global_set(instance.code,&zero); wasm_global_set(instance.site,&zero);
            ok=wasm_instance_call_named(&instance,SOL_WASM_REPRESENTED_TEST_FAILURE_ENTRY_EXPORT,42,0,0);
        }
        wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    }
    sol_wasm_represented_test_callback_writeback_probe(false);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false); propagation_pipeline_free(&p); return ok;
}

static bool p44e_depth_writebacks_holes(void) {
    const char *leaves[]={"p44e_depth_method_inout","p44e_depth_callback_inout"};
    const uint32_t depths[]={1,63,64}; bool ok=true;
    sol_wasm_represented_test_callback_writeback_probe(true);
    for (size_t leaf=0; ok && leaf<2; ++leaf) {
        char directory[512];
        (void)snprintf(directory,sizeof directory,"%s/tests/conformance/%s",SOL_TEST_SOURCE_DIR,leaves[leaf]);
        PropagationPipeline p; propagation_pipeline_init(&p);
        ok=propagation_pipeline_build_named(&p,directory,"launch",true);
        for (size_t cap=0; ok && cap<3; ++cap) {
            SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
            SolWasmRepresentedLimits limits=sol_wasm_represented_default_limits();
            limits.max_call_depth=depths[cap]; char entry[256]; WasmInstance instance={0};
            ok=sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&p.lowered,directory,&limits},
                &output,&p.diagnostics)==SOL_WASM_REPRESENTED_OK
                && entry_symbol(&output.bytes,entry,sizeof entry)
                && wasm_instance_open(&output.bytes,entry,&instance);
            for (size_t repeat=0; ok && repeat<3; ++repeat)
                ok=wasm_instance_call(&instance,cap==2?42:0,cap==2?0:7,cap==2?0:cap==0?7:6)
                    && wasm_instance_writebacks(&instance,cap==2?63:0);
            wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
        }
        propagation_pipeline_free(&p);
    }
    sol_wasm_represented_test_callback_writeback_probe(false);
    sol_wasm_represented_test_callable_hole_cleanup_probe(true);
    for (size_t cap=0; ok && cap<2; ++cap) {
        SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
        SolWasmRepresentedLimits limits=sol_wasm_represented_default_limits(); limits.max_call_depth=cap==0?1:64;
        char entry[256]; WasmInstance instance={0};
        ok=build_named_root(SOL_TEST_SOURCE_DIR "/tests/conformance/p44e_depth_hole","launch",
            &output,&limits,SOL_WASM_REPRESENTED_OK) && entry_symbol(&output.bytes,entry,sizeof entry)
            && wasm_instance_open(&output.bytes,entry,&instance);
        const int32_t counters[]={0,1,1,1};
        for (size_t repeat=0; ok && repeat<3; ++repeat)
            ok=wasm_instance_call(&instance,cap==0?0:42,cap==0?7:0,cap==0?4:0)
                && wasm_instance_cleanup_counters(&instance,counters);
        wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    }
    sol_wasm_represented_test_callable_hole_cleanup_probe(false); return ok;
}

static void run_depth(size_t first, size_t last) {
    represented_control("depth.recursion-routes-boundaries-packets-repeat");
    static const P44eDepthCase cases[] = {
        {"p44e_depth_self_63",NULL,63,0,true},
        {"p44e_depth_self",NULL,64,0,true},
        {"p44e_depth_self_65","descend(value - 1)",64,7,true},
        {"p44e_depth_forever","return forever()",64,7,true},
        {"p44e_depth_mutual",NULL,64,0,true},
        {"p44e_depth_mutual_forever","return right()",64,7,true},
        {"p44e_depth_method",NULL,64,0,true},
        {"p44e_depth_method_inout",NULL,64,0,true},
        {"p44e_depth_callback",NULL,64,0,true},
        {"p44e_depth_callback_inout",NULL,64,0,true},
        {"p44e_depth_self","descend(62)",1,7,true},
        {"p44e_depth_method","value.read()",1,7,true},
        {"p44e_depth_method_inout","value.bump()",1,7,true},
        {"p44e_depth_callback","callback(62)",1,7,true},
        {"p44e_depth_callback_inout","callback(value)",1,7,true},
        {"p44e_depth_hole","moved()",1,7,false},
        {"p44e_depth_packet","return bottom()",3,7,true},
        {"p44e_depth_snapshot","return forever(value)",2,7,true},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i)
        CHECK(p44e_depth_case(&cases[i], i >= 10));
    represented_control("depth.static-catalog-chains-api-wire-owner-mutations");
    const P44eDepthCase chains[] = {
        {"p44e_depth_chain63",NULL,64,0,false},
        {"p44e_depth_chain64","return f63()",64,7,false},
        {"p44e_depth_chain65","return f63()",64,7,false},
    };
    for (size_t i=0; i<sizeof chains/sizeof *chains; ++i) CHECK(p44e_depth_case(&chains[i],false));
    CHECK(p44e_depth_limits_wire());
    CHECK(p44e_depth_signed_initializers());
    CHECK(p44e_depth_owner_mutations());
    represented_control("depth.recursive-multiroot-census-caps-faults-reset-relocation");
    CHECK(p44e_depth_resources(first,last));
    represented_control("depth.step-precedence-authoritative-pending-writebacks-holes");
    CHECK(p44e_depth_step_precedence()); CHECK(p44e_depth_pending_packets());
    CHECK(p44e_depth_writebacks_holes());
}

static bool p44e_step_allocation_precedence(void) {
    const char *directory = SOL_TEST_SOURCE_DIR "/tests/conformance/p44e_step_cleanup";
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false);
    for (unsigned dimension = 0; ok && dimension < 2; ++dimension) {
        ProvenanceRecord previous = {0}; bool allocated = false;
        for (uint64_t cap = 1; ok && cap <= 64; ++cap) {
            SolWasmRepresentedLimits limits = sol_wasm_represented_default_limits();
            limits.max_steps = cap;
            if (dimension == 0) limits.max_allocation_requests = 1;
            else limits.max_allocation_bytes = 4;
            SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
            char entry[256]; WasmInstance instance = {0};
            int64_t value = -1; int32_t code = 0, site = 0; ProvenanceRecord record;
            ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
                    directory, &limits}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
                && entry_symbol(&output.bytes, entry, sizeof entry)
                && wasm_instance_open(&output.bytes, entry, &instance)
                && wasm_instance_observe(&instance, &value, &code, &site)
                && value == 0 && site > 0
                && provenance_record(&output.bytes, (uint32_t)site, &record)
                && wasm_instance_call(&instance, value, code, site);
            if (ok && code == SOL_MIR_RUNTIME_FAILURE_STEP_LIMIT) {
                ok = record.tag == 3;
                /* Keep only scalar fields: the module bytes are freed below. */
                previous = (ProvenanceRecord){.tag = record.tag, .start = record.start,
                    .end = record.end};
            } else if (ok) {
                /* The base tick wins when unavailable. Once admitted, the
                 * allocation check wins before the first Text byte tick. */
                ok = code == SOL_MIR_RUNTIME_FAILURE_ALLOCATION_LIMIT && record.tag == 4
                    && previous.tag == 3 && record.start == previous.start && record.end == previous.end;
                allocated = ok;
            }
            wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
            if (allocated) break;
        }
        ok = ok && allocated;
    }
    propagation_pipeline_free(&pipeline); return ok;
}

/* A real memory.grow failure, not an allocator-limit override. At cap 2 the
 * CONST_TEXT base tick is unavailable. At cap 3 it succeeds and physical
 * allocation fails before byte zero. Clearing only the memory fault makes that
 * same cap fail at byte zero after allocation. The two P3 routes are disjoint. */
static bool p44e_step_physical_allocation(void) {
    const char *directory = SOL_TEST_SOURCE_DIR "/tests/conformance/p44_panic";
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false);
    const struct { uint64_t cap; bool grow; int32_t code; uint8_t tag; size_t ordinal; } cases[] = {
        {2,true,6,3,3}, {3,true,4,4,0}, {3,false,6,3,3},
    };
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    for (size_t i = 0; ok && i < sizeof cases / sizeof *cases; ++i) {
        SolWasmRepresentedLimits limits = sol_wasm_represented_default_limits();
        limits.max_steps = cases[i].cap;
        sol_wasm_represented_test_allocator_memory(cases[i].grow ? 1 : 0,
            cases[i].grow ? UINT32_C(65528) : 0);
        SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
        WasmInstance instance = {0}; char entry[256]; uint32_t selected = 0; size_t matches = 0;
        ok = sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, &limits}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK;
        for (uint32_t r = 1; ok && r <= output.usage.provenance_records; ++r) {
            ProvenanceRecord record;
            ok = provenance_record(&output.bytes, r, &record);
            if (ok && record.tag == cases[i].tag && record.ordinal == cases[i].ordinal
                && record.start == 101 && record.end == 129
                && bytes_equal(record.path, record.path_count, "main.sol")) {
                selected = r; ++matches;
            }
        }
        P44TraceSlot expected[] = {{cases[i].grow && cases[i].cap == 3 ? 0u : 9u,13,selected},
            {cases[i].grow && cases[i].cap == 3 ? 1u : 10u,13,selected},
            {cases[i].grow && cases[i].cap == 3 ? 2u : 11u,525,selected}};
        P44TraceSlot slots[64]; size_t count = 0; bool overflow = false;
        ok = ok && matches == 1 && entry_symbol(&output.bytes, entry, sizeof entry)
            && wasm_instance_open(&output.bytes, entry, &instance);
        for (size_t retry = 0; ok && retry < 2; ++retry)
            ok = wasm_instance_call(&instance, 0, cases[i].code, (int32_t)selected)
                && wasm_instance_trace(&instance, slots, 64, &count, &overflow)
                && c1_trace_matches(&pipeline, expected, 3, selected, slots, count, overflow);
        if (!ok) fprintf(stderr, "P44e physical allocation case=%zu failed\n", i);
        wasm_instance_close(&instance); sol_wasm_represented_output_free(&output);
    }
    sol_wasm_represented_test_allocator_memory(0, 0);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false);
    propagation_pipeline_free(&pipeline); return ok;
}

/* Inject into the existing fold image, then invoke the ORIGINAL generated entry
 * and caller. Neither the copy/equality call sites nor their result/packet
 * branches are replaced. Private test-side call witnesses prove that both real
 * emitter bodies run; complete normal callee cleanup is followed by the real
 * caller's pending transport. There are no new source shapes or meter exports. */
static bool p44e_step_pending_emitter(void) {
    const char *directory = SOL_TEST_SOURCE_DIR "/tests/conformance/p44_b2_trace";
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
    char entry[256]; uint32_t record = 0; size_t matches = 0;
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, NULL}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry);
    for (uint32_t r = 1; ok && r <= output.usage.provenance_records; ++r) {
        ProvenanceRecord site;
        ok = provenance_record(&output.bytes, r, &site);
        if (ok && site.tag == 3 && site.kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_STEP
            && site.ordinal == 5 && site.start == 108 && site.end == 117
            && bytes_equal(site.path, site.path_count, "main.sol")) { record = r; ++matches; }
    }
    const SolMirRuntimeFailureSite *source = pipeline.conventions.failure_site_count > 5
        ? &pipeline.conventions.failure_sites[5] : NULL;
    ok = ok && matches == 1 && source != NULL && source->owner == 0 && source->block == 0
        && source->instruction == 3 && source->occurrence == 5
        && source->source.start == 108 && source->source.end == 117
        && source->allowed_codes == UINT32_C(0x20);
    BinaryenModuleRef module = ok ? BinaryenModuleRead((char *)output.bytes.bytes, output.bytes.count) : NULL;
    if (module == NULL) ok = false;
    if (ok) {
        BinaryenModuleSetFeatures(module, BinaryenFeatureMVP() | BinaryenFeatureMutableGlobals());
        const char *code = BinaryenGlobalGetName(BinaryenGetGlobalByIndex(module, 0));
        const char *site = BinaryenGlobalGetName(BinaryenGetGlobalByIndex(module, 1));
        const char *steps = BinaryenGlobalGetName(BinaryenGetGlobalByIndex(module, 8));
        const char *counters[] = {"p44e.called.copy", "p44e.called.equal"};
        for (BinaryenIndex i = 0; ok && i < 2; ++i) {
            ok = BinaryenAddGlobal(module, counters[i], BinaryenTypeInt32(), true,
                BinaryenConst(module, BinaryenLiteralInt32(0))) != NULL;
            BinaryenFunctionRef helper = BinaryenGetFunctionByIndex(module, i + 1);
            BinaryenExpressionRef body[] = {BinaryenGlobalSet(module, counters[i],
                BinaryenBinary(module, BinaryenAddInt32(),
                    BinaryenGlobalGet(module, counters[i], BinaryenTypeInt32()),
                    BinaryenConst(module, BinaryenLiteralInt32(1)))), BinaryenFunctionGetBody(helper)};
            BinaryenFunctionSetBody(helper, BinaryenBlock(module, NULL, body, 2, BinaryenTypeInt64()));
        }
        /* The source has exactly one represented function with a Text parameter
         * and Int64 result. Root emission order is not materialization order. */
        BinaryenFunctionRef fold = NULL; size_t candidates = 0;
        for (BinaryenIndex i = 3; i < BinaryenGetNumFunctions(module); ++i) {
            BinaryenFunctionRef candidate = BinaryenGetFunctionByIndex(module, i);
            if (BinaryenFunctionGetParams(candidate) == BinaryenTypeInt64()
                && BinaryenFunctionGetResults(candidate) == BinaryenTypeInt64()) {
                fold = candidate; ++candidates;
            }
        }
        ok = ok && candidates == 1 && fold != NULL;
        if (ok) {
            BinaryenExpressionRef body[] = {
                BinaryenGlobalSet(module, code, BinaryenConst(module, BinaryenLiteralInt32(6))),
                BinaryenGlobalSet(module, site, BinaryenConst(module, BinaryenLiteralInt32((int32_t)record))),
                BinaryenGlobalSet(module, steps, BinaryenConst(module, BinaryenLiteralInt64(100000))),
                BinaryenFunctionGetBody(fold),
            };
            BinaryenFunctionSetBody(fold, BinaryenBlock(module, NULL, body, 4, BinaryenTypeInt64()));
            const char *original = BinaryenExportGetValue(BinaryenGetExport(module, entry));
            BinaryenExpressionRef condition = BinaryenBinary(module, BinaryenAndInt32(),
                BinaryenBinary(module, BinaryenEqInt32(),
                    BinaryenGlobalGet(module, counters[0], BinaryenTypeInt32()),
                    /* Caller literal, parameter LOAD_COPY, comparison literal,
                     * match temporary, and owned-region literal. */
                    BinaryenConst(module, BinaryenLiteralInt32(5))),
                BinaryenBinary(module, BinaryenEqInt32(),
                    BinaryenGlobalGet(module, counters[1], BinaryenTypeInt32()),
                    BinaryenConst(module, BinaryenLiteralInt32(1))));
            BinaryenExpressionRef wrapper[] = {
                BinaryenGlobalSet(module, counters[0], BinaryenConst(module, BinaryenLiteralInt32(0))),
                BinaryenGlobalSet(module, counters[1], BinaryenConst(module, BinaryenLiteralInt32(0))),
                BinaryenLocalSet(module, 0, BinaryenCall(module, original, NULL, 0, BinaryenTypeInt64())),
                BinaryenIf(module, condition, BinaryenLocalGet(module, 0, BinaryenTypeInt64()),
                    BinaryenConst(module, BinaryenLiteralInt64(-1))),
            };
            BinaryenType local = BinaryenTypeInt64();
            ok = BinaryenAddFunction(module, "p44e.pending.entry", BinaryenTypeNone(),
                BinaryenTypeInt64(), &local, 1,
                BinaryenBlock(module, NULL, wrapper, 4, BinaryenTypeInt64())) != NULL;
            if (ok) {
                BinaryenRemoveExport(module, entry);
                ok = BinaryenAddFunctionExport(module, "p44e.pending.entry", entry) != NULL
                    && BinaryenModuleValidate(module);
            }
        }
    }
    BinaryenModuleAllocateAndWriteResult written = {0};
    if (ok) written = BinaryenModuleAllocateAndWrite(module, NULL);
    SolWasmBackendBytes bytes = {(uint8_t *)written.binary, written.binaryBytes};
    WasmInstance instance = {0}; P44TraceSlot slots[64]; size_t count = 0; bool overflow = false;
    P44TraceSlot expected[] = {{13,1,0},{14,1,0},{31,1,0},{32,1,0},{33,1,0},{34,1,0},
        {35,1,0},{36,1,0},{37,1,0},{38,1,0},{39,1,0},{40,1,0},
        {44,1,0},{45,1,0},{46,533,record}};
    ok = ok && written.binary != NULL && wasm_instance_open(&bytes, entry, &instance);
    for (size_t retry = 0; ok && retry < 2; ++retry) {
        int64_t value = -1; int32_t observed_code = 0, observed_site = 0;
        ok = wasm_instance_observe(&instance, &value, &observed_code, &observed_site)
            && wasm_instance_trace(&instance, slots, 64, &count, &overflow);
        if (ok && (value != 0 || observed_code != 6 || observed_site != (int32_t)record))
            fprintf(stderr, "P44e pending result value=%lld code=%d site=%d\n",
                (long long)value, observed_code, observed_site);
        ok = ok && value == 0 && observed_code == 6 && observed_site == (int32_t)record
            && c1_trace_matches(&pipeline, expected, sizeof expected / sizeof *expected,
                record, slots, count, overflow);
    }
    if (!ok) {
        fprintf(stderr, "P44e pending real emitter failed: record=%u trace=%zu\n", record, count);
        for (size_t i = 0; i < count; ++i) fprintf(stderr, " pending {%u,%u,%u}\n",
            slots[i].action, slots[i].disposition, slots[i].record);
    }
    wasm_instance_close(&instance); free(written.binary); free(written.sourceMap);
    if (module != NULL) BinaryenModuleDispose(module);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false);
    sol_wasm_represented_output_free(&output); propagation_pipeline_free(&pipeline); return ok;
}

/* Isolate otherwise unreachable helper states without adding a production
 * hook or exporting quota/counter globals. Source LOAD_COPY makes distinct
 * handles even in `text == text`, so an envelope-only wrapper is needed for
 * the same-handle fast path and exact transactional allocator assertions. */
static bool p44e_step_helper_boundaries(void) {
    const char *directory = SOL_TEST_SOURCE_DIR "/tests/conformance/p44e_step_text";
    PropagationPipeline pipeline; propagation_pipeline_init(&pipeline);
    SolWasmRepresentedOutput output; sol_wasm_represented_output_init(&output);
    SolWasmRepresentedLimits limits = sol_wasm_represented_default_limits();
    limits.max_steps = UINT64_MAX;
    char entry[256]; uint32_t step_record = 0;
    bool ok = propagation_pipeline_build_named(&pipeline, directory, "launch", false)
        && sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&pipeline.lowered,
            directory, &limits}, &output, &pipeline.diagnostics) == SOL_WASM_REPRESENTED_OK
        && entry_symbol(&output.bytes, entry, sizeof entry);
    for (uint32_t i = 1; ok && i <= output.usage.provenance_records; ++i) {
        ProvenanceRecord record;
        ok = provenance_record(&output.bytes, i, &record);
        if (ok && record.tag == 3 && record.kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_STEP) {
            step_record = i; break;
        }
    }
    ok = ok && step_record != 0;
    for (unsigned mode = 0; ok && mode < 6; ++mode) {
        BinaryenModuleRef module = BinaryenModuleRead((char *)output.bytes.bytes, output.bytes.count);
        if (module == NULL) { ok = false; break; }
        BinaryenModuleSetFeatures(module, BinaryenFeatureMVP() | BinaryenFeatureMutableGlobals());
        const char *globals[9];
        for (BinaryenIndex i = 0; i < 9; ++i)
            globals[i] = BinaryenGlobalGetName(BinaryenGetGlobalByIndex(module, i));
        const char *memory = BinaryenExportGetValue(BinaryenGetExport(module,
            SOL_WASM_BACKEND_MEMORY_EXPORT));
        /* Emission order is meter, Text copy, Text equality. */
        bool copy = mode == 1 || mode == 4 || mode == 5;
        bool pending = mode == 2 || mode == 5;
        const char *helper = BinaryenFunctionGetName(BinaryenGetFunctionByIndex(module,
            copy ? 1 : mode == 3 ? 0 : 2));
        BinaryenExpressionRef items[32]; BinaryenIndex count = 0;
#define P44E_I32(value) BinaryenConst(module, BinaryenLiteralInt32((int32_t)(value)))
#define P44E_I64(value) BinaryenConst(module, BinaryenLiteralInt64((int64_t)(value)))
#define P44E_SET32(index, value) BinaryenGlobalSet(module, globals[index], P44E_I32(value))
#define P44E_SET64(index, value) BinaryenGlobalSet(module, globals[index], P44E_I64(value))
#define P44E_EQ64(index, value) BinaryenBinary(module, BinaryenEqInt64(), \
    BinaryenGlobalGet(module, globals[index], BinaryenTypeInt64()), P44E_I64(value))
#define P44E_STORE(address, value) BinaryenStore(module, 4, 0, 4, P44E_I32(address), \
    P44E_I32(value), BinaryenTypeInt32(), memory)
        items[count++] = P44E_SET32(0, pending ? 6 : 0);
        items[count++] = P44E_SET32(1, pending ? step_record : 0);
        items[count++] = P44E_SET32(2, 8192);
        items[count++] = P44E_SET64(5, 0);
        items[count++] = P44E_SET64(6, 0);
        items[count++] = P44E_SET64(8, mode == 1 || mode == 3 ? UINT64_MAX - 1 : UINT64_MAX);
        items[count++] = P44E_STORE(8, 48); items[count++] = P44E_STORE(12, 3);
        items[count++] = P44E_STORE(24, 52); items[count++] = P44E_STORE(28, 3);
        items[count++] = P44E_STORE(48, UINT32_C(0x00636261));
        items[count++] = P44E_STORE(52, UINT32_C(0x00636261));
        items[count++] = P44E_STORE(8192, UINT32_C(0x55));
        items[count++] = P44E_STORE(8200, UINT32_C(0x44444444));
        BinaryenExpressionRef arguments[] = {P44E_I64(mode == 0 ? UINT64_MAX : 8),
            P44E_I64(mode == 0 ? UINT64_MAX : copy ? step_record : 24),
            P44E_I64(step_record)};
        BinaryenExpressionRef call = BinaryenCall(module, helper,
            mode == 3 ? &arguments[2] : arguments, mode == 3 ? 1 : 3,
            mode == 3 ? BinaryenTypeInt32() : BinaryenTypeInt64());
        BinaryenExpressionRef matched = BinaryenBinary(module,
            mode == 3 ? BinaryenEqInt32() : BinaryenEqInt64(), call,
            mode == 3 ? P44E_I32(1) : P44E_I64(copy ? mode == 5 ? 8192 : -1 : 1));
        items[count++] = BinaryenLocalSet(module, 0, matched);
        if (mode == 3) {
            BinaryenExpressionRef second = BinaryenCall(module, helper, &arguments[2], 1,
                BinaryenTypeInt32());
            items[count++] = BinaryenLocalSet(module, 0, BinaryenBinary(module, BinaryenAndInt32(),
                BinaryenLocalGet(module, 0, BinaryenTypeInt32()), BinaryenUnary(module,
                    BinaryenEqZInt32(), second)));
        }
        BinaryenExpressionRef condition = BinaryenLocalGet(module, 0, BinaryenTypeInt32());
        const unsigned checked[] = {5, 6, 8};
        const uint64_t wanted[] = {mode == 5 ? 2u : 0u, mode == 5 ? 11u : 0u, UINT64_MAX};
        for (size_t i = 0; i < 3; ++i) condition = BinaryenBinary(module, BinaryenAndInt32(),
            condition, P44E_EQ64(checked[i], wanted[i]));
        condition = BinaryenBinary(module, BinaryenAndInt32(), condition, BinaryenBinary(module,
            BinaryenEqInt32(), BinaryenGlobalGet(module, globals[2], BinaryenTypeInt32()),
            P44E_I32(mode == 5 ? 8208 : 8192)));
        condition = BinaryenBinary(module, BinaryenAndInt32(), condition, BinaryenBinary(module,
            BinaryenEqInt32(), BinaryenLoad(module, 4, false, 0, 4, BinaryenTypeInt32(),
                P44E_I32(8192), memory), P44E_I32(mode == 5 ? 8200 : UINT32_C(0x55))));
        condition = BinaryenBinary(module, BinaryenAndInt32(), condition, BinaryenBinary(module,
            BinaryenEqInt32(), BinaryenLoad(module, 4, false, 0, 4, BinaryenTypeInt32(),
                P44E_I32(8200), memory), P44E_I32(mode == 1
                    ? UINT32_C(0x44444461) : mode == 5 ? UINT32_C(0x44636261) : UINT32_C(0x44444444))));
        items[count++] = BinaryenIf(module, condition, P44E_I64(42), P44E_I64(0));
        BinaryenType local = BinaryenTypeInt32();
        ok = BinaryenAddFunction(module, "p44e.helper.boundary", BinaryenTypeNone(), BinaryenTypeInt64(),
            &local, 1, BinaryenBlock(module, NULL, items, count, BinaryenTypeInt64())) != NULL;
        BinaryenExportRef exported = BinaryenGetExport(module, entry);
        if (ok && exported != NULL) {
            BinaryenFunctionRef wrapper = BinaryenGetFunction(module, BinaryenExportGetValue(exported));
            BinaryenFunctionSetBody(wrapper, BinaryenCall(module, "p44e.helper.boundary", NULL, 0,
                BinaryenTypeInt64()));
            ok = BinaryenModuleValidate(module);
        } else ok = false;
        BinaryenModuleAllocateAndWriteResult written = {0};
        if (ok) written = BinaryenModuleAllocateAndWrite(module, NULL);
        SolWasmBackendBytes bytes = {(uint8_t *)written.binary, written.binaryBytes};
        ok = ok && written.binary != NULL && invoke_named(&bytes, entry, 42,
            mode == 0 ? 0 : 6, mode == 0 ? 0 : (int32_t)step_record);
        free(written.binary); free(written.sourceMap); BinaryenModuleDispose(module);
#undef P44E_I32
#undef P44E_I64
#undef P44E_SET32
#undef P44E_SET64
#undef P44E_EQ64
#undef P44E_STORE
        if (!ok) fprintf(stderr, "P44e helper boundary mode=%u failed\n", mode);
    }
    sol_wasm_represented_output_free(&output); propagation_pipeline_free(&pipeline); return ok;
}

static bool p44e_step_execution(void) {
    unsigned classes = 0;
    const struct { const char *leaf, *root; int64_t value; int32_t code; uint64_t steps; } cases[] = {
        {"p44e_step_rows", "launch", 42, 0, 28},
        {"p44e_step_text", "launch", 42, 0, 103},
        {"p44e_step_text", "empty", 42, 0, 24},
        {"p44e_step_text", "mismatch", 42, 0, 22},
        {"p44e_step_text", "length_mismatch", 42, 0, 18},
        {"p44e_step_text", "identity", 42, 0, 31},
        {"p44e_step_text", "late_mismatch", 42, 0, 23},
        {"p44e_step_text", "product_mismatch", 42, 0, 44},
        {"p44e_step_text", "sum_tag_mismatch", 42, 0, 32},
        {"p44e_step_text", "nested", 42, 0, 77},
        {"p44e_step_cleanup", "launch", 42, 0, 31},
        {"p44_panic_empty", "launch", 0, 1, 6},
        {"p44_panic", "launch", 0, 1, 58},
        {"p44_panic_191", "launch", 0, 1, 388},
        {"p44_panic_192", "launch", 0, 1, 389},
        {"p42_scalar_add_overflow", "launch", 0, 2, 5},
        {"p44c_ensures_old_body_failure", "launch", 0, 3, 12},
        {"p44c_ensures_old_predicate_failure", "launch", 0, 3, 15},
        {"p44c_ensures_old_two", "launch", 42, 0, 31},
        {"p44c_ensures_old_repeat", "launch", 42, 0, 27},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i)
        if (!p44e_step_sweep(cases[i].leaf, cases[i].root, cases[i].value, cases[i].code,
                cases[i].steps, &classes)) return false;
    return classes == 15u && p44e_step_limits_wire() && p44e_step_allocation_precedence()
        && p44e_step_physical_allocation() && p44e_step_pending_emitter()
        && p44e_step_helper_boundaries();
}

static void run_api(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("api.all-controls-mutations-quotas-sequences");
    SolWasmRepresentedLimits limits = sol_wasm_represented_default_limits();
    CHECK(limits.max_functions != 0 && limits.max_blocks != 0
        && limits.max_values != 0 && limits.max_locals != 0
        && limits.max_generated_nodes != 0 && limits.max_table_elements != 0
        && limits.max_static_data_bytes != 0 && limits.max_scratch_bytes != 0
        && limits.max_work_bytes != 0 && limits.max_owned_bytes != 0
        && limits.max_output_bytes != 0 && limits.max_steps == UINT64_C(100000));
    SolWasmRepresentedOutput output;
    sol_wasm_represented_output_init(&output);
    CHECK(output.bytes.bytes == NULL && output.bytes.count == 0
        && output.usage.functions == 0 && output.usage.static_data_bytes == 0
        && output.usage.output_bytes == 0);
    CHECK(sol_wasm_represented_validate(NULL) == SOL_WASM_REPRESENTED_INVALID_ARGUMENT);
    /* 3 setup nodes + 124 arms + 1 terminal node exactly fills the private
     * helper array; the next arm is rejected before a Binaryen node is built. */
    CHECK(sol_wasm_represented_test_sum_helper_items(124));
    CHECK(!sol_wasm_represented_test_sum_helper_items(125));
    CHECK(!sol_wasm_represented_test_rejects_capture_snapshot());
    sol_wasm_represented_output_free(&output);
}

static void run_e1(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("e1.all-controls-mutations-quotas-sequences");
    CHECK(p44e_step_execution());
}

static void run_refined(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("refined.all-controls-mutations-quotas-sequences");
    CHECK(c3a_refined_acceptance());
    represented_control("refined.gates-index-certificate-mutations-traces-markers");
    CHECK(c3a_refined_gates());
    CHECK(c3a_refinement_index());
    CHECK(c3a_refined_certificate_mutations());
    CHECK(c3a_refined_exact_traces());
    CHECK(c3a_refined_route_markers());
    represented_control("refined.resources-faults-rootorder-relocation-rebuild-seal");
    CHECK(c3a_refined_resources());
    CHECK(c3a_refined_root_order_relocation());
    CHECK(c3a_refined_source_rebuild_seal());
}

static void run_step_owner(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("step-owner.all-controls-mutations-quotas-sequences");
    CHECK(p44d_step_authentication());
}

static void run_contracts(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("contracts.all-controls-mutations-quotas-sequences");
    CHECK(p44c_acceptance_matrix());
    represented_control("contracts.requires-ensures-qualified-gates-routes");
    CHECK(p44c_qualified_source_gates());
    CHECK(p44c_ensures_acceptance_matrix());
    CHECK(p44c_qualified_root_gate());
    CHECK(p44c_qualified_mixed_route_gate());
    CHECK(p44c_qualified_result_failures());
    CHECK(p44c_qualified_root_order_relocation());
    represented_control("contracts.allocations-owners-mutations-exact-traces-packets");
    CHECK(p44c_qualified_err_two_allocations());
    CHECK(p44c_qualified_constructor_allocation());
    CHECK(p44c_qualified_owner_mutations());
    CHECK(p44c_qualified_control_route_mutations());
    CHECK(p44c_exact_ensures_traces());
    CHECK(p44c_old_exact_traces());
    CHECK(p44c_qualified_exact_traces());
    CHECK(p44c_qualified_direct_routes());
    CHECK(p44c_qualified_direct_transport());
    CHECK(p44c_qualified_later_clauses_absent());
    CHECK(p44c_qualified_failure_old_snapshot_routes());
    CHECK(p44c_qualified_panic_detail());
    represented_control("contracts.old-snapshot-mutations-resources-deltas-rootorder");
    {
        char directory[512];
        (void)snprintf(directory, sizeof directory,
            "%s/tests/conformance/p44c_ensures_old_two", SOL_TEST_SOURCE_DIR);
        CHECK(p44c_old_predecessor_mutations(directory));
    }
    {
        char directory[512];
        (void)snprintf(directory, sizeof directory,
            "%s/tests/conformance/p44c_qualified_failure_old_false", SOL_TEST_SOURCE_DIR);
        CHECK(p44c_qualified_snapshot_owner_permutation(directory));
    }
    {
        char directory[512];
        (void)snprintf(directory, sizeof directory,
            "%s/tests/conformance/p44c_ensures_old_many", SOL_TEST_SOURCE_DIR);
        CHECK(p44c_old_many_shape(directory));
    }
    {
        char directory[512], current[512];
        (void)snprintf(directory, sizeof directory,
            "%s/tests/conformance/p44c_ensures_old_reject", SOL_TEST_SOURCE_DIR);
        (void)snprintf(current, sizeof current,
            "%s/tests/conformance/p44c_ensures_current", SOL_TEST_SOURCE_DIR);
        CHECK(p44c_old_resource_determinism(directory, current));
    }
    {
        char directory[512], current[512];
        (void)snprintf(directory, sizeof directory,
            "%s/tests/conformance/p44c_ensures_old_two", SOL_TEST_SOURCE_DIR);
        (void)snprintf(current, sizeof current,
            "%s/tests/conformance/p44c_ensures_current_two", SOL_TEST_SOURCE_DIR);
        CHECK(p44c_old_local_delta(directory, current, 5, 42));
    }
    {
        char directory[512], current[512];
        (void)snprintf(directory, sizeof directory,
            "%s/tests/conformance/p44c_ensures_old_repeat", SOL_TEST_SOURCE_DIR);
        (void)snprintf(current, sizeof current,
            "%s/tests/conformance/p44c_ensures_current_repeat", SOL_TEST_SOURCE_DIR);
        CHECK(p44c_old_local_delta(directory, current, 7, 42));
    }
    {
        char directory[512];
        (void)snprintf(directory, sizeof directory,
            "%s/tests/conformance/p44c_ensures_old_roots", SOL_TEST_SOURCE_DIR);
        CHECK(p44c_old_root_order_determinism(directory));
    }
    {
        char directory[512];
        (void)snprintf(directory, sizeof directory,
            "%s/tests/conformance/p44c_ensures_true", SOL_TEST_SOURCE_DIR);
        CHECK(p44c_ensures_predecessor_mutations(directory));
    }
    {
        char directory[512];
        (void)snprintf(directory, sizeof directory,
            "%s/tests/conformance/p44c_ensures_roots", SOL_TEST_SOURCE_DIR);
        CHECK(p44c_ensures_resource_determinism(directory));
    }
    CHECK(p44c_false_violation_trace());
    represented_control("contracts.requires-forward-bool-owner-arithmetic-mutations");
    CHECK(p44c_exact_requires_traces());
    {
        char directory[512];
        (void)snprintf(directory, sizeof directory,
            "%s/tests/conformance/p44c_requires_parameter_forward", SOL_TEST_SOURCE_DIR);
        CHECK(p44c_parameter_forward_resource_determinism(directory));
    }
    {
        char directory[512];
        (void)snprintf(directory, sizeof directory,
            "%s/tests/conformance/p44c_requires_bool_forward", SOL_TEST_SOURCE_DIR);
        CHECK(p44c_bool_forward_routes(directory));
        CHECK(p44c_owner_mutations(directory));
    }
    {
        char directory[512];
        (void)snprintf(directory, sizeof directory,
            "%s/tests/conformance/p44c_requires_divzero", SOL_TEST_SOURCE_DIR);
        CHECK(p44c_arithmetic_site_mutation(directory));
    }
}

static void run_qualified_frozen(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("qualified-frozen.all-controls-mutations-quotas-sequences");
    CHECK(p44c_qualified_result_determinism(0, 1, 117));
}

static void run_qualified_result(size_t first, size_t last) {
    represented_control("qualified-result.all-controls-mutations-quotas-sequences");
    CHECK(p44c_qualified_result_determinism(1, first, last));
    /* Each heavy chunk also retains the independent genuine-root order and
     * relocation prerequisite, rather than relying on the contracts shard. */
    represented_control("qualified-result.rootorder-relocation");
    CHECK(p44c_qualified_root_order_relocation());
}

static void run_stress(size_t first, size_t last) {
    represented_control("stress.all-controls-mutations-quotas-sequences");
    CHECK(p44c_qualified_sequential_stress(first, last));
}

static void run_callable_holes(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("callable-holes.all-controls-mutations-quotas-sequences");
    SolWasmRepresentedOutput output;
    char callable_hole_directory[512];
    (void)snprintf(callable_hole_directory, sizeof callable_hole_directory,
        "%s/tests/conformance/p43_callable_hole_prereq", SOL_TEST_SOURCE_DIR);
    sol_wasm_represented_output_init(&output);
    ProvenanceRecord callable_hole_entry;
    char callable_hole_entry_name[256];
    static const SolWasmRepresentedUsage expected_callable_hole_usage = {7,4,2,9,59,1216,2,15,0,0,49,29707,11311,19574,9334};
    static const uint8_t expected_callable_hole_hash[32] = {0x49,0x6f,0xd9,0xe9,0xf7,0xc4,0xae,0x10,0x78,0xd3,0xd8,0xf6,0x39,0xbb,0x0f,0xee,0xce,0xe8,0xa4,0x66,0x11,0xdd,0x27,0xf4,0xfc,0x3e,0xcf,0x8f,0xb7,0xad,0xda,0xfe};
    uint8_t callable_hole_hash[32];
    CHECK(build_named_root(callable_hole_directory, "launch", &output, NULL,
        SOL_WASM_REPRESENTED_OK) && sol_wasm_represented_validate(&output.bytes)
            == SOL_WASM_REPRESENTED_OK && output.bytes.bytes != NULL
        && output.usage.allocation_requests == 0 && output.usage.allocation_bytes == 0
        && provenance_record(&output.bytes, 1, &callable_hole_entry)
        && callable_hole_entry.symbol_count < sizeof callable_hole_entry_name);
    if (output.bytes.bytes != NULL && provenance_record(&output.bytes, 1, &callable_hole_entry)
        && callable_hole_entry.symbol_count < sizeof callable_hole_entry_name) {
        memcpy(callable_hole_entry_name, callable_hole_entry.symbol,
            callable_hole_entry.symbol_count);
        callable_hole_entry_name[callable_hole_entry.symbol_count] = '\0';
        CHECK(invoke_named(&output.bytes, callable_hole_entry_name, 42, 0, 0));
        sha256(output.bytes.bytes, output.bytes.count, callable_hole_hash);
        CHECK(usage_equal(&output.usage, &expected_callable_hole_usage)
            && memcmp(callable_hole_hash, expected_callable_hole_hash,
                sizeof callable_hole_hash) == 0);
        /* Capture this before another represented build resets the hook.  The
         * source product contains one Text literal, reflected by nonempty
         * static literal data, and every backend allocation ordinal is faulted. */
        /* `validate` above has its own four parser-map allocations.  Rebuild
         * before freezing the build-only sweep so each injected ordinal names
         * exactly one represented-build allocation. */
        SolWasmRepresentedOutput callable_hole_allocation_probe;
        sol_wasm_represented_output_init(&callable_hole_allocation_probe);
        CHECK(build_named_root(callable_hole_directory, "launch", &callable_hole_allocation_probe,
            NULL, SOL_WASM_REPRESENTED_OK));
        sol_wasm_represented_output_free(&callable_hole_allocation_probe);
        size_t callable_hole_attempts = sol_wasm_represented_test_allocation_attempts();
        CHECK(output.usage.static_data_bytes > 0 && callable_hole_attempts == 87);
        for (size_t attempt = 1; attempt <= callable_hole_attempts; ++attempt) {
            SolWasmRepresentedOutput failed;
            sol_wasm_represented_output_init(&failed);
            represented_fault(FAULT_CALLABLE_HOLE, attempt, attempt);
            CHECK(build_named_root(callable_hole_directory, "launch", &failed, NULL,
                SOL_WASM_REPRESENTED_ALLOCATION_FAILED) && failed.bytes.bytes == NULL
                && failed.bytes.count == 0 && usage_zero(&failed.usage));
            sol_wasm_represented_output_free(&failed);
            sol_wasm_represented_test_fail_allocation_after(0);
        }
        SolWasmRepresentedOutput callable_hole_retry;
        sol_wasm_represented_output_init(&callable_hole_retry);
        CHECK(build_named_root(callable_hole_directory, "launch", &callable_hole_retry, NULL,
            SOL_WASM_REPRESENTED_OK) && callable_hole_retry.bytes.count == output.bytes.count
            && memcmp(callable_hole_retry.bytes.bytes, output.bytes.bytes, output.bytes.count) == 0
            && usage_equal(&callable_hole_retry.usage, &output.usage)
            && sol_wasm_represented_test_allocation_attempts() == callable_hole_attempts);
        sol_wasm_represented_output_free(&callable_hole_retry);
        SolWasmRepresentedLimits callable_hole_zero = {0};
        sol_wasm_represented_output_init(&callable_hole_retry);
        CHECK(build_named_root(callable_hole_directory, "launch", &callable_hole_retry,
            &callable_hole_zero, SOL_WASM_REPRESENTED_OK)
            && callable_hole_retry.bytes.count == output.bytes.count
            && memcmp(callable_hole_retry.bytes.bytes, output.bytes.bytes, output.bytes.count) == 0
            && usage_equal(&callable_hole_retry.usage, &output.usage));
        sol_wasm_represented_output_free(&callable_hole_retry);
#define CHECK_CALLABLE_HOLE_CAP(field, exact) do { \
    SolWasmRepresentedLimits capped = sol_wasm_represented_default_limits(); \
    capped.field = (exact); sol_wasm_represented_output_init(&callable_hole_retry); \
    CHECK(build_named_root(callable_hole_directory, "launch", &callable_hole_retry, &capped, \
        SOL_WASM_REPRESENTED_OK) && usage_equal(&callable_hole_retry.usage, &output.usage)); \
    sol_wasm_represented_output_free(&callable_hole_retry); capped.field = (exact) - 1; \
    sol_wasm_represented_output_init(&callable_hole_retry); \
    CHECK(build_named_root(callable_hole_directory, "launch", &callable_hole_retry, &capped, \
        SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED) && callable_hole_retry.bytes.bytes == NULL \
        && callable_hole_retry.bytes.count == 0 && usage_zero(&callable_hole_retry.usage)); \
    sol_wasm_represented_output_free(&callable_hole_retry); \
} while (0)
        CHECK_CALLABLE_HOLE_CAP(max_functions, output.usage.functions);
        CHECK_CALLABLE_HOLE_CAP(max_blocks, output.usage.blocks);
        CHECK_CALLABLE_HOLE_CAP(max_edges, output.usage.edges);
        CHECK_CALLABLE_HOLE_CAP(max_values, output.usage.values);
        CHECK_CALLABLE_HOLE_CAP(max_locals, output.usage.locals);
        CHECK_CALLABLE_HOLE_CAP(max_generated_nodes, output.usage.generated_nodes);
        CHECK_CALLABLE_HOLE_CAP(max_table_elements, output.usage.table_elements);
        CHECK_CALLABLE_HOLE_CAP(max_static_data_bytes, output.usage.static_data_bytes);
        CHECK_CALLABLE_HOLE_CAP(max_provenance_records, output.usage.provenance_records);
        CHECK_CALLABLE_HOLE_CAP(max_work_bytes, output.usage.work_bytes);
        CHECK_CALLABLE_HOLE_CAP(max_scratch_bytes, output.usage.scratch_bytes);
        CHECK_CALLABLE_HOLE_CAP(max_owned_bytes, output.usage.owned_bytes);
        CHECK_CALLABLE_HOLE_CAP(max_output_bytes, output.usage.output_bytes);
#undef CHECK_CALLABLE_HOLE_CAP
#define CHECK_CALLABLE_HOLE_PARTIAL(field) do { \
    SolWasmRepresentedLimits partial = sol_wasm_represented_default_limits(); \
    partial.field = 0; sol_wasm_represented_output_init(&callable_hole_retry); \
    CHECK(build_named_root(callable_hole_directory, "launch", &callable_hole_retry, &partial, \
        SOL_WASM_REPRESENTED_INVALID_ARGUMENT) && callable_hole_retry.bytes.bytes == NULL \
        && callable_hole_retry.bytes.count == 0 && usage_zero(&callable_hole_retry.usage)); \
    sol_wasm_represented_output_free(&callable_hole_retry); \
} while (0)
        CHECK_CALLABLE_HOLE_PARTIAL(max_functions); CHECK_CALLABLE_HOLE_PARTIAL(max_blocks);
        CHECK_CALLABLE_HOLE_PARTIAL(max_edges); CHECK_CALLABLE_HOLE_PARTIAL(max_values);
        CHECK_CALLABLE_HOLE_PARTIAL(max_locals); CHECK_CALLABLE_HOLE_PARTIAL(max_generated_nodes);
        CHECK_CALLABLE_HOLE_PARTIAL(max_table_elements); CHECK_CALLABLE_HOLE_PARTIAL(max_static_data_bytes);
        CHECK_CALLABLE_HOLE_PARTIAL(max_allocation_requests);
        CHECK_CALLABLE_HOLE_PARTIAL(max_allocation_bytes);
        CHECK_CALLABLE_HOLE_PARTIAL(max_provenance_records);
        CHECK_CALLABLE_HOLE_PARTIAL(max_work_bytes); CHECK_CALLABLE_HOLE_PARTIAL(max_scratch_bytes);
        CHECK_CALLABLE_HOLE_PARTIAL(max_owned_bytes); CHECK_CALLABLE_HOLE_PARTIAL(max_output_bytes);
#undef CHECK_CALLABLE_HOLE_PARTIAL
        /* `answer` is already reachable through the callable value, so adding
         * it as a second genuine internal root changes only root input order,
         * not the represented closure or its public export shape. */
        SolWasmRepresentedOutput callable_hole_forward, callable_hole_reverse;
        size_t callable_hole_forward_ids[2], callable_hole_reverse_ids[2];
        sol_wasm_represented_output_init(&callable_hole_forward);
        sol_wasm_represented_output_init(&callable_hole_reverse);
        CHECK(build_multiroot(callable_hole_directory, false, &callable_hole_forward,
            callable_hole_forward_ids, NULL, SOL_WASM_REPRESENTED_OK)
            && build_multiroot(callable_hole_directory, true, &callable_hole_reverse,
                callable_hole_reverse_ids, NULL, SOL_WASM_REPRESENTED_OK)
            && callable_hole_forward_ids[0] != callable_hole_forward_ids[1]
            && callable_hole_forward_ids[0] == callable_hole_reverse_ids[0]
            && callable_hole_forward_ids[1] == callable_hole_reverse_ids[1]
            && callable_hole_forward.bytes.count == output.bytes.count
            && memcmp(callable_hole_forward.bytes.bytes, output.bytes.bytes, output.bytes.count) == 0
            && callable_hole_reverse.bytes.count == callable_hole_forward.bytes.count
            && memcmp(callable_hole_reverse.bytes.bytes, callable_hole_forward.bytes.bytes,
                callable_hole_forward.bytes.count) == 0
            && usage_equal(&callable_hole_forward.usage, &output.usage)
            && usage_equal(&callable_hole_reverse.usage, &output.usage));
        uint8_t callable_hole_forward_hash[32], callable_hole_reverse_hash[32];
        sha256(callable_hole_forward.bytes.bytes, callable_hole_forward.bytes.count,
            callable_hole_forward_hash);
        sha256(callable_hole_reverse.bytes.bytes, callable_hole_reverse.bytes.count,
            callable_hole_reverse_hash);
        CHECK(memcmp(callable_hole_forward_hash, callable_hole_hash,
            sizeof callable_hole_hash) == 0 && memcmp(callable_hole_reverse_hash,
                callable_hole_hash, sizeof callable_hole_hash) == 0);
        char forward_entry[256], reverse_entry[256];
        CHECK(entry_symbol(&callable_hole_forward.bytes, forward_entry, sizeof forward_entry)
            && entry_symbol(&callable_hole_reverse.bytes, reverse_entry, sizeof reverse_entry)
            && !strcmp(forward_entry, callable_hole_entry_name)
            && !strcmp(reverse_entry, callable_hole_entry_name)
            && invoke_named(&callable_hole_forward.bytes, forward_entry, 42, 0, 0)
            && invoke_named(&callable_hole_reverse.bytes, reverse_entry, 42, 0, 0));
        sol_wasm_represented_output_free(&callable_hole_reverse);
        sol_wasm_represented_output_free(&callable_hole_forward);
    }
    sol_wasm_represented_output_free(&output);
    CHECK(callable_hole_runtime_controls(callable_hole_directory));
    represented_control("callable-holes.full-baseline-owner");
    char callable_hole_full_directory[512];
    (void)snprintf(callable_hole_full_directory, sizeof callable_hole_full_directory,
        "%s/tests/conformance/p43_callable_hole_full", SOL_TEST_SOURCE_DIR);
    SolWasmRepresentedOutput callable_hole_full;
    ProvenanceRecord callable_hole_full_entry;
    char callable_hole_full_entry_name[256];
    static const SolWasmRepresentedUsage expected_callable_hole_full_usage = {7,2,0,6,52,881,2,15,0,0,32,22911,10240,16588,6348};
    static const uint8_t expected_callable_hole_full_hash[32] = {0x78,0x30,0xb2,0xee,0x61,0x29,0x99,0x79,0xe2,0xf3,0xa1,0x06,0x30,0x53,0x1b,0x57,0x3b,0xbc,0x19,0x3e,0x73,0x97,0xcd,0x2f,0x0f,0x56,0x55,0x18,0x3b,0x09,0x66,0xf1};
    sol_wasm_represented_output_init(&callable_hole_full);
    CHECK(build_named_root(callable_hole_full_directory, "launch", &callable_hole_full, NULL,
        SOL_WASM_REPRESENTED_OK) && sol_wasm_represented_validate(&callable_hole_full.bytes)
            == SOL_WASM_REPRESENTED_OK && provenance_record(&callable_hole_full.bytes, 1,
                &callable_hole_full_entry) && callable_hole_full_entry.symbol_count
                    < sizeof callable_hole_full_entry_name);
    if (callable_hole_full.bytes.bytes != NULL && provenance_record(&callable_hole_full.bytes, 1,
            &callable_hole_full_entry) && callable_hole_full_entry.symbol_count
                < sizeof callable_hole_full_entry_name) {
        memcpy(callable_hole_full_entry_name, callable_hole_full_entry.symbol,
            callable_hole_full_entry.symbol_count);
        callable_hole_full_entry_name[callable_hole_full_entry.symbol_count] = '\0';
        CHECK(invoke_named(&callable_hole_full.bytes, callable_hole_full_entry_name, 42, 0, 0));
        uint8_t full_hash[32]; sha256(callable_hole_full.bytes.bytes,
            callable_hole_full.bytes.count, full_hash);
        CHECK(usage_equal(&callable_hole_full.usage, &expected_callable_hole_full_usage)
            && memcmp(full_hash, expected_callable_hole_full_hash, sizeof full_hash) == 0);
    }
    sol_wasm_represented_output_free(&callable_hole_full);
    CHECK(callable_hole_owner_mutations(callable_hole_full_directory, 0));
    /* C3.1 instruments the emitted root/hole traversal, not allocator state.
     * The root action occurs more than once in the cleanup stream, but init is
     * cleared by the first pass, so every entry invocation observes one live
     * sibling/root drop and never drops the moved callable field. */
    sol_wasm_represented_test_callable_hole_cleanup_probe(true);
    represented_control("callable-holes.same-instance-cleanup");
    SolWasmRepresentedOutput callable_hole_probe;
    sol_wasm_represented_output_init(&callable_hole_probe);
    CHECK(build_named_root(callable_hole_directory, "launch", &callable_hole_probe, NULL,
        SOL_WASM_REPRESENTED_OK) && sol_wasm_represented_validate(&callable_hole_probe.bytes)
            == SOL_WASM_REPRESENTED_OK);
    if (callable_hole_probe.bytes.bytes != NULL) {
        static const int32_t expected_cleanup_counters[] = {0, 1, 1, 1};
        WasmInstance instance;
        CHECK(wasm_instance_open(&callable_hole_probe.bytes, callable_hole_entry_name, &instance));
        CHECK(wasm_instance_call(&instance, 42, 0, 0));
        CHECK(wasm_instance_cleanup_counters(&instance, expected_cleanup_counters));
        CHECK(wasm_instance_call(&instance, 42, 0, 0));
        CHECK(wasm_instance_cleanup_counters(&instance, expected_cleanup_counters));
        wasm_instance_close(&instance);
    }
    sol_wasm_represented_output_free(&callable_hole_probe);
    sol_wasm_represented_output_init(&callable_hole_probe);
    CHECK(build_named_root(callable_hole_full_directory, "launch", &callable_hole_probe, NULL,
        SOL_WASM_REPRESENTED_OK) && sol_wasm_represented_validate(&callable_hole_probe.bytes)
            == SOL_WASM_REPRESENTED_OK);
    if (callable_hole_probe.bytes.bytes != NULL) {
        static const int32_t expected_cleanup_counters[] = {1, 0, 1, 1};
        WasmInstance instance;
        CHECK(wasm_instance_open(&callable_hole_probe.bytes, callable_hole_full_entry_name, &instance));
        CHECK(wasm_instance_call(&instance, 42, 0, 0));
        CHECK(wasm_instance_cleanup_counters(&instance, expected_cleanup_counters));
        CHECK(wasm_instance_call(&instance, 42, 0, 0));
        CHECK(wasm_instance_cleanup_counters(&instance, expected_cleanup_counters));
        wasm_instance_close(&instance);
    }
    sol_wasm_represented_output_free(&callable_hole_probe);
    char callable_hole_failure_directory[512];
    (void)snprintf(callable_hole_failure_directory, sizeof callable_hole_failure_directory,
        "%s/tests/conformance/p43_callable_hole_failure", SOL_TEST_SOURCE_DIR);
    SolWasmRepresentedOutput callable_hole_failure;
    ProvenanceRecord callable_hole_failure_entry;
    char callable_hole_failure_entry_name[256];
    sol_wasm_represented_output_init(&callable_hole_failure);
    CHECK(build_named_root(callable_hole_failure_directory, "launch", &callable_hole_failure, NULL,
        SOL_WASM_REPRESENTED_OK) && provenance_record(&callable_hole_failure.bytes, 1,
            &callable_hole_failure_entry) && callable_hole_failure_entry.symbol_count
                < sizeof callable_hole_failure_entry_name);
    if (callable_hole_failure.bytes.bytes != NULL && provenance_record(&callable_hole_failure.bytes,
            1, &callable_hole_failure_entry) && callable_hole_failure_entry.symbol_count
                < sizeof callable_hole_failure_entry_name) {
        static const int32_t expected_cleanup_counters[] = {0, 1, 1, 1};
        memcpy(callable_hole_failure_entry_name, callable_hole_failure_entry.symbol,
            callable_hole_failure_entry.symbol_count);
        callable_hole_failure_entry_name[callable_hole_failure_entry.symbol_count] = '\0';
        WasmInstance instance;
        CHECK(wasm_instance_open(&callable_hole_failure.bytes, callable_hole_failure_entry_name,
                &instance) && wasm_instance_call(&instance, 0, 2, 4)
            && wasm_instance_cleanup_counters(&instance, expected_cleanup_counters));
        wasm_instance_close(&instance);
    }
    sol_wasm_represented_output_free(&callable_hole_failure);
    CHECK(callable_hole_owner_mutations(callable_hole_directory, 1));
    sol_wasm_represented_test_callable_hole_cleanup_probe(false);
    char callable_hole_c32_directory[512];
    represented_control("callable-holes.c32-runtime-owner-cap-rootorder");
    static const int32_t c32_conditional_true[] = {0, 1, 1, 1};
    static const int32_t c32_conditional_false[] = {1, 0, 1, 1};
    static const int32_t c32_repair[] = {1, 0, 1, 1};
    static const int32_t c32_reopen[] = {0, 1, 1, 1};
    static const int32_t c32_whole[] = {1, 0, 1, 1};
#define CHECK_C32_CASE(name, expected) do { \
    (void)snprintf(callable_hole_c32_directory, sizeof callable_hole_c32_directory, \
        "%s/tests/conformance/" name, SOL_TEST_SOURCE_DIR); \
    CHECK(callable_hole_c32_runtime(callable_hole_c32_directory, expected)); \
} while (0)
    CHECK_C32_CASE("p43_callable_hole_c32_conditional_true", c32_conditional_true);
    CHECK_C32_CASE("p43_callable_hole_c32_conditional_false", c32_conditional_false);
    CHECK_C32_CASE("p43_callable_hole_c32_repair", c32_repair);
    CHECK_C32_CASE("p43_callable_hole_c32_reopen", c32_reopen);
    CHECK_C32_CASE("p43_callable_hole_c32_whole", c32_whole);
#undef CHECK_C32_CASE
    (void)snprintf(callable_hole_c32_directory, sizeof callable_hole_c32_directory,
        "%s/tests/conformance/p43_callable_hole_c32_conditional_true", SOL_TEST_SOURCE_DIR);
    CHECK(callable_hole_c32_owner_mutations(callable_hole_c32_directory, 0));
    (void)snprintf(callable_hole_c32_directory, sizeof callable_hole_c32_directory,
        "%s/tests/conformance/p43_callable_hole_c32_repair", SOL_TEST_SOURCE_DIR);
    CHECK(callable_hole_c32_owner_mutations(callable_hole_c32_directory, 1));
    (void)snprintf(callable_hole_c32_directory, sizeof callable_hole_c32_directory,
        "%s/tests/conformance/p43_callable_hole_c32_reopen", SOL_TEST_SOURCE_DIR);
    CHECK(callable_hole_c32_owner_mutations(callable_hole_c32_directory, 2));
    (void)snprintf(callable_hole_c32_directory, sizeof callable_hole_c32_directory,
        "%s/tests/conformance/p43_callable_hole_c32_whole", SOL_TEST_SOURCE_DIR);
    CHECK(callable_hole_c32_owner_mutations(callable_hole_c32_directory, 3));
    (void)snprintf(callable_hole_c32_directory, sizeof callable_hole_c32_directory,
        "%s/tests/conformance/p43_callable_hole_c32_repair", SOL_TEST_SOURCE_DIR);
    CHECK(callable_hole_runtime_controls(callable_hole_c32_directory));
    CHECK(callable_hole_c32_repair_controls(callable_hole_c32_directory));
    CHECK(callable_hole_c32_repair_multiroot(callable_hole_c32_directory));
}

static void run_callback_inout(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("callback-inout.all-controls-mutations-quotas-sequences");
    char callback_mixed_directory[512];
    (void)snprintf(callback_mixed_directory, sizeof callback_mixed_directory,
        "%s/tests/conformance/p43_callback_mixed_signature", SOL_TEST_SOURCE_DIR);
    CHECK(callback_mixed_signatures_rejected(callback_mixed_directory));
    char callback_inout_directory[512];
    (void)snprintf(callback_inout_directory, sizeof callback_inout_directory,
        "%s/tests/conformance/p43_callback_inout", SOL_TEST_SOURCE_DIR);
    CHECK(callback_inout_authentication(callback_inout_directory));
    SolWasmRepresentedOutput callback_inout;
    represented_control("callback-inout.baseline-census-caps-defaults-faults");
    ProvenanceRecord callback_inout_entry;
    char callback_inout_entry_name[256];
    sol_wasm_represented_output_init(&callback_inout);
    CHECK(build_named_root(callback_inout_directory, "launch", &callback_inout, NULL,
        SOL_WASM_REPRESENTED_OK) && sol_wasm_represented_validate(&callback_inout.bytes)
        == SOL_WASM_REPRESENTED_OK && provenance_record(&callback_inout.bytes, 1,
            &callback_inout_entry) && callback_inout_entry.symbol_count
                < sizeof callback_inout_entry_name);
    if (callback_inout.bytes.bytes != NULL && provenance_record(&callback_inout.bytes, 1,
            &callback_inout_entry) && callback_inout_entry.symbol_count
                < sizeof callback_inout_entry_name) {
        memcpy(callback_inout_entry_name, callback_inout_entry.symbol,
            callback_inout_entry.symbol_count);
        callback_inout_entry_name[callback_inout_entry.symbol_count] = '\0';
        CHECK(invoke_named(&callback_inout.bytes, callback_inout_entry_name, 42, 0, 0));
    }
    static const SolWasmRepresentedUsage expected_callback_inout_usage = {7,4,2,11,59,1073,2,0,0,0,48,28064,10616,19099,8859};
    static const uint8_t expected_callback_inout_hash[32] = {0x9f,0xbe,0x58,0xa4,0xa5,0x45,0xbe,0xca,0x3e,0x8f,0xe1,0xd9,0x58,0x8e,0xff,0x9b,0xda,0xe4,0x61,0xca,0x09,0x2c,0xaf,0x86,0xd5,0xd4,0xbf,0x62,0x2f,0x2f,0x46,0xc4};
    uint8_t callback_inout_hash[32]; sha256(callback_inout.bytes.bytes, callback_inout.bytes.count,
        callback_inout_hash);
    CHECK(usage_equal(&callback_inout.usage, &expected_callback_inout_usage)
        && memcmp(callback_inout_hash, expected_callback_inout_hash, sizeof callback_inout_hash) == 0);
    sol_wasm_represented_output_free(&callback_inout);
    SolWasmRepresentedOutput callback_inout_roots, callback_inout_probe;
    size_t callback_inout_ids[2];
    sol_wasm_represented_output_init(&callback_inout_roots);
    CHECK(build_multiroot(callback_inout_directory, false, &callback_inout_roots,
        callback_inout_ids, NULL, SOL_WASM_REPRESENTED_OK));
    size_t callback_inout_allocation_attempts
        = sol_wasm_represented_test_allocation_attempts();
    CHECK(sol_wasm_represented_validate(&callback_inout_roots.bytes)
        == SOL_WASM_REPRESENTED_OK);
    static const SolWasmRepresentedUsage expected_callback_inout_roots_usage = {8,7,4,17,73,1431,2,0,0,0,77,39031,16683,23695,13455};
    static const uint8_t expected_callback_inout_roots_hash[32] = {0x51,0x35,0xdd,0x3d,0xfe,0x5b,0x55,0xeb,0x82,0x9f,0x80,0x4a,0xf7,0x11,0xc0,0x9e,0x70,0x1a,0xeb,0xdc,0xbf,0x0f,0xa5,0x70,0xb0,0xc1,0xd0,0xcf,0x02,0x07,0x20,0x6c};
    uint8_t callback_inout_roots_hash[32]; sha256(callback_inout_roots.bytes.bytes,
        callback_inout_roots.bytes.count, callback_inout_roots_hash);
    CHECK(usage_equal(&callback_inout_roots.usage, &expected_callback_inout_roots_usage)
        && memcmp(callback_inout_roots_hash, expected_callback_inout_roots_hash,
            sizeof callback_inout_roots_hash) == 0
        && callback_inout_allocation_attempts == 122);
#define CHECK_C2B_BUILD_CAP(field, exact) do { \
    SolWasmRepresentedLimits cap = sol_wasm_represented_default_limits(); \
    SolWasmRepresentedOutput capped; cap.field = (exact); sol_wasm_represented_output_init(&capped); \
    CHECK(build_multiroot(callback_inout_directory, false, &capped, callback_inout_ids, &cap, \
        SOL_WASM_REPRESENTED_OK) && capped.bytes.count == callback_inout_roots.bytes.count \
        && memcmp(capped.bytes.bytes, callback_inout_roots.bytes.bytes, capped.bytes.count) == 0 \
        && usage_equal(&capped.usage, &callback_inout_roots.usage)); \
    sol_wasm_represented_output_free(&capped); cap.field = (exact) - 1; \
    sol_wasm_represented_output_init(&capped); \
    CHECK(build_multiroot(callback_inout_directory, false, &capped, callback_inout_ids, &cap, \
        SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED) && capped.bytes.bytes == NULL \
        && capped.bytes.count == 0 && usage_zero(&capped.usage)); \
    sol_wasm_represented_output_free(&capped); \
} while (0)
    CHECK_C2B_BUILD_CAP(max_functions, 8u); CHECK_C2B_BUILD_CAP(max_blocks, 7u);
    CHECK_C2B_BUILD_CAP(max_edges, 4u); CHECK_C2B_BUILD_CAP(max_values, 17u);
    CHECK_C2B_BUILD_CAP(max_locals, 73u); CHECK_C2B_BUILD_CAP(max_generated_nodes, 1431u);
    CHECK_C2B_BUILD_CAP(max_table_elements, 2u); CHECK_C2B_BUILD_CAP(max_provenance_records, 77u);
    CHECK_C2B_BUILD_CAP(max_work_bytes, 39031u); CHECK_C2B_BUILD_CAP(max_scratch_bytes, 16683u);
    CHECK_C2B_BUILD_CAP(max_owned_bytes, 23695u); CHECK_C2B_BUILD_CAP(max_output_bytes, 13455u);
#undef CHECK_C2B_BUILD_CAP
    SolWasmRepresentedLimits callback_inout_zero = {0}; SolWasmRepresentedOutput callback_inout_copy;
    sol_wasm_represented_output_init(&callback_inout_copy);
    CHECK(build_multiroot(callback_inout_directory, false, &callback_inout_copy, callback_inout_ids,
        &callback_inout_zero, SOL_WASM_REPRESENTED_OK) && callback_inout_copy.bytes.count
            == callback_inout_roots.bytes.count && memcmp(callback_inout_copy.bytes.bytes,
                callback_inout_roots.bytes.bytes, callback_inout_copy.bytes.count) == 0
        && usage_equal(&callback_inout_copy.usage, &callback_inout_roots.usage));
    sol_wasm_represented_output_free(&callback_inout_copy);
#define CHECK_C2B_PARTIAL_ZERO(field) do { \
    SolWasmRepresentedLimits partial = sol_wasm_represented_default_limits(); \
    SolWasmRepresentedOutput rejected; partial.field = 0; sol_wasm_represented_output_init(&rejected); \
    CHECK(build_multiroot(callback_inout_directory, false, &rejected, callback_inout_ids, &partial, \
        SOL_WASM_REPRESENTED_INVALID_ARGUMENT) && rejected.bytes.bytes == NULL \
        && rejected.bytes.count == 0 && usage_zero(&rejected.usage)); \
    sol_wasm_represented_output_free(&rejected); \
} while (0)
    CHECK_C2B_PARTIAL_ZERO(max_functions); CHECK_C2B_PARTIAL_ZERO(max_blocks);
    CHECK_C2B_PARTIAL_ZERO(max_edges); CHECK_C2B_PARTIAL_ZERO(max_values);
    CHECK_C2B_PARTIAL_ZERO(max_locals); CHECK_C2B_PARTIAL_ZERO(max_generated_nodes);
    CHECK_C2B_PARTIAL_ZERO(max_table_elements); CHECK_C2B_PARTIAL_ZERO(max_static_data_bytes);
    CHECK_C2B_PARTIAL_ZERO(max_allocation_requests); CHECK_C2B_PARTIAL_ZERO(max_allocation_bytes);
    CHECK_C2B_PARTIAL_ZERO(max_provenance_records); CHECK_C2B_PARTIAL_ZERO(max_work_bytes);
    CHECK_C2B_PARTIAL_ZERO(max_scratch_bytes); CHECK_C2B_PARTIAL_ZERO(max_owned_bytes);
    CHECK_C2B_PARTIAL_ZERO(max_output_bytes);
#undef CHECK_C2B_PARTIAL_ZERO
    for (size_t attempt = 1; attempt <= callback_inout_allocation_attempts; ++attempt) {
        SolWasmRepresentedOutput failed, retry;
        represented_fault(FAULT_CALLBACK_INOUT, attempt, attempt); sol_wasm_represented_output_init(&failed);
        CHECK(build_multiroot(callback_inout_directory, false, &failed, callback_inout_ids, NULL,
            SOL_WASM_REPRESENTED_ALLOCATION_FAILED) && failed.bytes.bytes == NULL
            && failed.bytes.count == 0 && usage_zero(&failed.usage));
        sol_wasm_represented_output_free(&failed); sol_wasm_represented_test_fail_allocation_after(0);
        sol_wasm_represented_output_init(&retry);
        CHECK(build_multiroot(callback_inout_directory, false, &retry, callback_inout_ids, NULL,
            SOL_WASM_REPRESENTED_OK) && retry.bytes.count == callback_inout_roots.bytes.count
            && memcmp(retry.bytes.bytes, callback_inout_roots.bytes.bytes, retry.bytes.count) == 0
            && usage_equal(&retry.usage, &callback_inout_roots.usage));
        sol_wasm_represented_output_free(&retry);
    }
    SolWasmRepresentedOutput callback_inout_reverse;
    size_t callback_inout_reverse_ids[2]; sol_wasm_represented_output_init(&callback_inout_reverse);
    CHECK(build_multiroot(callback_inout_directory, true, &callback_inout_reverse,
        callback_inout_reverse_ids, NULL, SOL_WASM_REPRESENTED_OK)
        && callback_inout_reverse_ids[0] == callback_inout_ids[0]
        && callback_inout_reverse_ids[1] == callback_inout_ids[1]
        && callback_inout_reverse.bytes.count == callback_inout_roots.bytes.count
        && memcmp(callback_inout_reverse.bytes.bytes, callback_inout_roots.bytes.bytes,
            callback_inout_reverse.bytes.count) == 0
        && usage_equal(&callback_inout_reverse.usage, &callback_inout_roots.usage));
    sol_wasm_represented_output_free(&callback_inout_reverse);
    int32_t callback_inout_allocation_sites[2] = {0, 0}; size_t callback_inout_allocation_count = 0;
    represented_control("callback-inout.runtime-quotas-grow-retry");
    for (uint32_t record = 1; record <= callback_inout_roots.usage.provenance_records; ++record) {
        ProvenanceRecord candidate;
        if (!provenance_record(&callback_inout_roots.bytes, record, &candidate)) break;
        if (candidate.tag == 4 && callback_inout_allocation_count < 2)
            callback_inout_allocation_sites[callback_inout_allocation_count++] = (int32_t)record;
    }
    CHECK(callback_inout_allocation_count == 2);
    SolWasmRepresentedLimits callback_inout_quota = sol_wasm_represented_default_limits();
    callback_inout_quota.max_allocation_requests = 1; callback_inout_quota.max_allocation_bytes = 4;
    SolWasmRepresentedOutput callback_inout_runtime;
    sol_wasm_represented_test_allocator_quota(UINT64_MAX, UINT64_MAX);
    sol_wasm_represented_output_init(&callback_inout_runtime);
    CHECK(build_multiroot(callback_inout_directory, false, &callback_inout_runtime, callback_inout_ids,
        &callback_inout_quota, SOL_WASM_REPRESENTED_OK)
        && invoke_named(&callback_inout_runtime.bytes, callback_inout_entry_name, 42, 0, 0));
    sol_wasm_represented_output_free(&callback_inout_runtime);
    sol_wasm_represented_test_allocator_quota(0, UINT64_MAX);
    sol_wasm_represented_output_init(&callback_inout_runtime);
    CHECK(build_multiroot(callback_inout_directory, false, &callback_inout_runtime, callback_inout_ids,
        &callback_inout_quota, SOL_WASM_REPRESENTED_OK)
        && invoke_named(&callback_inout_runtime.bytes, callback_inout_entry_name, 0, 5,
            callback_inout_allocation_sites[0]));
    sol_wasm_represented_output_free(&callback_inout_runtime);
    sol_wasm_represented_test_allocator_quota(UINT64_MAX, UINT64_MAX);
    callback_inout_quota.max_allocation_requests = 1; callback_inout_quota.max_allocation_bytes = 3;
    sol_wasm_represented_output_init(&callback_inout_runtime);
    CHECK(build_multiroot(callback_inout_directory, false, &callback_inout_runtime, callback_inout_ids,
        &callback_inout_quota, SOL_WASM_REPRESENTED_OK)
        && invoke_named(&callback_inout_runtime.bytes, callback_inout_entry_name, 0, 5,
            callback_inout_allocation_sites[0]));
    sol_wasm_represented_output_free(&callback_inout_runtime);
    sol_wasm_represented_test_allocator_memory(1, UINT32_C(65534));
    sol_wasm_represented_output_init(&callback_inout_runtime);
    CHECK(build_multiroot(callback_inout_directory, false, &callback_inout_runtime, callback_inout_ids,
        NULL, SOL_WASM_REPRESENTED_OK) && invoke_named(&callback_inout_runtime.bytes,
            callback_inout_entry_name, 0, 4, callback_inout_allocation_sites[0]));
    sol_wasm_represented_output_free(&callback_inout_runtime);
    sol_wasm_represented_test_allocator_memory(0, 0);
    sol_wasm_represented_test_allocator_quota(UINT64_MAX, UINT64_MAX);
    sol_wasm_represented_output_init(&callback_inout_runtime);
    CHECK(build_multiroot(callback_inout_directory, false, &callback_inout_runtime, callback_inout_ids,
        NULL, SOL_WASM_REPRESENTED_OK) && invoke_named(&callback_inout_runtime.bytes,
            callback_inout_entry_name, 42, 0, 0));
    sol_wasm_represented_output_free(&callback_inout_runtime);
    char callback_inout_relocated[512], callback_inout_source[768], callback_inout_destination[768];
    represented_control("callback-inout.relocation");
    (void)snprintf(callback_inout_relocated, sizeof callback_inout_relocated,
        "%s/p43_callback_inout_relocated", SOL_TEST_BINARY_DIR);
    (void)mkdir(callback_inout_relocated, 0700);
    (void)snprintf(callback_inout_source, sizeof callback_inout_source, "%s/main.sol",
        callback_inout_directory);
    (void)snprintf(callback_inout_destination, sizeof callback_inout_destination, "%s/main.sol",
        callback_inout_relocated);
    FILE *callback_inout_input = fopen(callback_inout_source, "rb");
    FILE *callback_inout_output = fopen(callback_inout_destination, "wb");
    CHECK(callback_inout_input != NULL && callback_inout_output != NULL);
    if (callback_inout_input != NULL && callback_inout_output != NULL) {
        uint8_t copy[256]; size_t count = 0;
        while ((count = fread(copy, 1, sizeof copy, callback_inout_input)) != 0)
            CHECK(fwrite(copy, 1, count, callback_inout_output) == count);
    }
    if (callback_inout_input != NULL) fclose(callback_inout_input);
    if (callback_inout_output != NULL) fclose(callback_inout_output);
    sol_wasm_represented_output_init(&callback_inout_copy);
    CHECK(build_multiroot(callback_inout_relocated, false, &callback_inout_copy, callback_inout_ids,
        NULL, SOL_WASM_REPRESENTED_OK) && callback_inout_copy.bytes.count
            == callback_inout_roots.bytes.count && memcmp(callback_inout_copy.bytes.bytes,
                callback_inout_roots.bytes.bytes, callback_inout_copy.bytes.count) == 0
        && usage_equal(&callback_inout_copy.usage, &callback_inout_roots.usage));
    sol_wasm_represented_output_free(&callback_inout_copy);
    if (callback_inout_roots.bytes.bytes != NULL) {
        WasmInstance instance;
        CHECK(wasm_instance_open(&callback_inout_roots.bytes, callback_inout_entry_name, &instance)
            && wasm_instance_call(&instance, 42, 0, 0));
        wasm_instance_close(&instance);
    }
    sol_wasm_represented_test_callback_writeback_probe(true);
    sol_wasm_represented_test_p44_cleanup_trace_probe(true);
    represented_control("callback-inout.same-instance-failure-success-trace");
    sol_wasm_represented_output_init(&callback_inout_probe);
    CHECK(build_multiroot(callback_inout_directory, false, &callback_inout_probe, callback_inout_ids,
        NULL, SOL_WASM_REPRESENTED_OK) && sol_wasm_represented_validate(&callback_inout_probe.bytes)
        == SOL_WASM_REPRESENTED_OK);
    if (callback_inout_probe.bytes.bytes != NULL) {
        WasmInstance instance;
        P44TraceSlot failed_trace[64], success_trace[64]; size_t failed_count = 0, success_count = 0;
        bool failed_overflow = false, success_overflow = false;
        static const P44TraceSlot expected_failure[] = {
            {0, 13, 5}, {1, 13, 5}, {2, 13, 5}, {3, 525, 5},
            {24, 1, 0}, {25, 1, 0}, {26, 1, 0}, {27, 533, 5},
        };
        static const P44TraceSlot expected_success[] = {
            {4, 1, 0}, {5, 1, 0}, {6, 1, 0}, {11, 257, 0},
            {16, 1, 0}, {17, 1, 0}, {18, 1, 0},
        };
        CHECK(wasm_instance_open(&callback_inout_probe.bytes, callback_inout_entry_name, &instance)
        && wasm_instance_call_named(&instance, SOL_WASM_REPRESENTED_TEST_FAILURE_ENTRY_EXPORT, 0, 2, 5)
            && wasm_instance_writebacks(&instance, 0)
            && wasm_instance_trace(&instance, failed_trace, 64, &failed_count, &failed_overflow)
            && wasm_instance_call(&instance, 42, 0, 0) && wasm_instance_writebacks(&instance, 1)
            && wasm_instance_trace(&instance, success_trace, 64, &success_count, &success_overflow)
            && !failed_overflow && !success_overflow
            && failed_count == sizeof expected_failure / sizeof *expected_failure
            && success_count == sizeof expected_success / sizeof *expected_success
            && memcmp(failed_trace, expected_failure, sizeof expected_failure) == 0
            && memcmp(success_trace, expected_success, sizeof expected_success) == 0);
        for (size_t i = 0; i < failed_count; ++i) CHECK(failed_trace[i].action != 11);
        size_t writebacks = 0;
        for (size_t i = 0; i < success_count; ++i) writebacks += success_trace[i].action == 11;
        CHECK(writebacks == 1);
        wasm_instance_close(&instance);
    }
    sol_wasm_represented_output_free(&callback_inout_probe);
    sol_wasm_represented_test_callback_writeback_probe(false);
    sol_wasm_represented_test_p44_cleanup_trace_probe(false);
    sol_wasm_represented_output_free(&callback_inout_roots);
}

static void run_methods(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("methods.all-controls-mutations-quotas-sequences");
    char method_directory[512];
    (void)snprintf(method_directory, sizeof method_directory,
        "%s/tests/conformance/p43_method_prereq", SOL_TEST_SOURCE_DIR);
    CHECK(method_authentication(method_directory));
    SolWasmRepresentedOutput method, method_probe;
    ProvenanceRecord method_entry;
    char method_entry_name[256];
    sol_wasm_represented_output_init(&method);
    CHECK(build_named_root(method_directory, "launch", &method, NULL, SOL_WASM_REPRESENTED_OK)
        && method.usage.table_elements == 0
        && sol_wasm_represented_validate(&method.bytes) == SOL_WASM_REPRESENTED_OK
        && module_has_no_sections(&method.bytes, 2, 4, 8)
        && provenance_record(&method.bytes, 1, &method_entry)
        && method_entry.symbol_count < sizeof method_entry_name);
    if (method.bytes.bytes != NULL && provenance_record(&method.bytes, 1, &method_entry)
        && method_entry.symbol_count < sizeof method_entry_name) {
        memcpy(method_entry_name, method_entry.symbol, method_entry.symbol_count);
        method_entry_name[method_entry.symbol_count] = '\0';
        CHECK(invoke_named(&method.bytes, method_entry_name, 83, 0, 0));
    }
    static const SolWasmRepresentedUsage expected_method_usage = {7,7,4,14,56,1149,0,0,0,0,64,34083,14115,21352,11112};
    static const uint8_t expected_method_hash[32] = {0xcc,0x65,0xc4,0xc4,0x6e,0x7d,0x75,0xa4,0xef,0xf1,0x7e,0x01,0xda,0xb1,0x8f,0x68,0xdc,0x74,0x95,0x3e,0xe4,0x0e,0x75,0x9e,0xe7,0x7e,0x82,0x8f,0x76,0xad,0x7b,0x86};
    uint8_t method_hash[32]; sha256(method.bytes.bytes, method.bytes.count, method_hash);
    CHECK(usage_equal(&method.usage, &expected_method_usage)
        && memcmp(method_hash, expected_method_hash, sizeof method_hash) == 0);
    for (uint32_t record = 1; record <= method.usage.provenance_records; ++record) {
        ProvenanceRecord candidate;
        CHECK(provenance_record(&method.bytes, record, &candidate)
            && bytes_equal(candidate.path, candidate.path_count, "main.sol"));
    }
    /* Each source root produces its own direct sol.e1 entry, rather than using
       launch's combined observation as a proxy for either call convention. */
    char method_shared_directory[512], method_exclusive_directory[512];
    represented_control("methods.direct-entry-packets");
    (void)snprintf(method_shared_directory, sizeof method_shared_directory,
        "%s/tests/conformance/p43_method_shared", SOL_TEST_SOURCE_DIR);
    (void)snprintf(method_exclusive_directory, sizeof method_exclusive_directory,
        "%s/tests/conformance/p43_method_exclusive", SOL_TEST_SOURCE_DIR);
    const char *const method_directories[] = {method_shared_directory, method_exclusive_directory};
    const int64_t method_values[] = {41, 42};
    const int32_t method_codes[] = {0, 0};
    const int32_t method_sites[] = {0, 0};
    for (size_t i = 0; i < sizeof method_directories / sizeof method_directories[0]; ++i) {
        SolWasmRepresentedOutput direct; ProvenanceRecord direct_entry;
        char direct_name[256]; sol_wasm_represented_output_init(&direct);
        CHECK(build_named_root(method_directories[i], "launch", &direct, NULL,
            SOL_WASM_REPRESENTED_OK) && direct.usage.table_elements == 0
            && module_has_no_sections(&direct.bytes, 2, 4, 8)
            && provenance_record(&direct.bytes, 1, &direct_entry)
            && direct_entry.symbol_count < sizeof direct_name);
        if (direct.bytes.bytes != NULL && provenance_record(&direct.bytes, 1, &direct_entry)
            && direct_entry.symbol_count < sizeof direct_name) {
            memcpy(direct_name, direct_entry.symbol, direct_entry.symbol_count);
            direct_name[direct_entry.symbol_count] = '\0';
            CHECK(strncmp(direct_name, "sol.e1.", 7) == 0 && invoke_named(&direct.bytes,
                direct_name, method_values[i], method_codes[i], method_sites[i]));
        }
        sol_wasm_represented_output_free(&direct);
    }
    /* This two-root copy retains launch's preceding authenticated call sites,
       while making fail the sole ENTRY root.  It is therefore an independent
       sol.e1 overflow packet probe with the canonical site 6. */
    char method_failure_entry_directory[512];
    (void)snprintf(method_failure_entry_directory, sizeof method_failure_entry_directory,
        "%s/tests/conformance/p43_method_failure_entry", SOL_TEST_SOURCE_DIR);
    SolWasmRepresentedOutput method_failure_entry; size_t method_failure_ids[2];
    char method_failure_name[256]; sol_wasm_represented_output_init(&method_failure_entry);
    CHECK(build_multiroot(method_failure_entry_directory, false, &method_failure_entry,
        method_failure_ids, NULL, SOL_WASM_REPRESENTED_OK)
        && module_has_no_sections(&method_failure_entry.bytes, 2, 4, 8)
        && entry_symbol(&method_failure_entry.bytes, method_failure_name, sizeof method_failure_name)
        && invoke_named(&method_failure_entry.bytes, method_failure_name, 0, 2, 6));
    sol_wasm_represented_output_free(&method_failure_entry);
    /* All nonzero physical census fields are closed under an exact cap and
       fail one below.  The direct method slice owns neither table/static data
       nor runtime allocation quota demand. */
#define CHECK_METHOD_BUILD_CAP(field, exact) do { \
    SolWasmRepresentedLimits cap = sol_wasm_represented_default_limits(); \
    SolWasmRepresentedOutput capped; cap.field = (exact); sol_wasm_represented_output_init(&capped); \
    CHECK(build_named_root(method_directory, "launch", &capped, &cap, SOL_WASM_REPRESENTED_OK) \
        && capped.bytes.count == method.bytes.count \
        && memcmp(capped.bytes.bytes, method.bytes.bytes, capped.bytes.count) == 0 \
        && usage_equal(&capped.usage, &method.usage)); \
    sol_wasm_represented_output_free(&capped); cap.field = (exact) - 1; \
    sol_wasm_represented_output_init(&capped); \
    CHECK(build_named_root(method_directory, "launch", &capped, &cap, \
        SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED) && capped.bytes.bytes == NULL \
        && capped.bytes.count == 0 && usage_zero(&capped.usage)); \
    sol_wasm_represented_output_free(&capped); \
} while (0)
    CHECK_METHOD_BUILD_CAP(max_functions, 7u); CHECK_METHOD_BUILD_CAP(max_blocks, 7u);
    CHECK_METHOD_BUILD_CAP(max_edges, 4u); CHECK_METHOD_BUILD_CAP(max_values, 14u);
    CHECK_METHOD_BUILD_CAP(max_locals, 56u); CHECK_METHOD_BUILD_CAP(max_generated_nodes, 1149u);
    CHECK_METHOD_BUILD_CAP(max_provenance_records, 64u); CHECK_METHOD_BUILD_CAP(max_work_bytes, 34083u);
    CHECK_METHOD_BUILD_CAP(max_scratch_bytes, 14115u); CHECK_METHOD_BUILD_CAP(max_owned_bytes, 21352u);
    CHECK_METHOD_BUILD_CAP(max_output_bytes, 11112u);
#undef CHECK_METHOD_BUILD_CAP
    SolWasmRepresentedLimits method_defaults = {0}; SolWasmRepresentedOutput method_copy;
    sol_wasm_represented_output_init(&method_copy);
    CHECK(build_named_root(method_directory, "launch", &method_copy, &method_defaults,
        SOL_WASM_REPRESENTED_OK) && method_copy.bytes.count == method.bytes.count
        && memcmp(method_copy.bytes.bytes, method.bytes.bytes, method.bytes.count) == 0
        && usage_equal(&method_copy.usage, &method.usage));
    sol_wasm_represented_output_free(&method_copy);
    sol_wasm_represented_output_init(&method_copy);
    CHECK(build_named_root(method_directory, "launch", &method_copy, NULL,
        SOL_WASM_REPRESENTED_OK));
    size_t method_allocation_count = sol_wasm_represented_test_allocation_attempts();
    sol_wasm_represented_output_free(&method_copy);
    CHECK(method_allocation_count == 102);
    for (size_t attempt = 1; attempt <= method_allocation_count; ++attempt) {
        SolWasmRepresentedOutput failed, retry;
        represented_fault(FAULT_METHOD, attempt, attempt); sol_wasm_represented_output_init(&failed);
        CHECK(build_named_root(method_directory, "launch", &failed, NULL,
            SOL_WASM_REPRESENTED_ALLOCATION_FAILED) && failed.bytes.bytes == NULL
            && failed.bytes.count == 0 && usage_zero(&failed.usage));
        sol_wasm_represented_output_free(&failed);
        sol_wasm_represented_test_fail_allocation_after(0); sol_wasm_represented_output_init(&retry);
        CHECK(build_named_root(method_directory, "launch", &retry, NULL,
            SOL_WASM_REPRESENTED_OK) && retry.bytes.count == method.bytes.count
            && memcmp(retry.bytes.bytes, method.bytes.bytes, retry.bytes.count) == 0
            && usage_equal(&retry.usage, &method.usage));
        sol_wasm_represented_output_free(&retry);
    }
    sol_wasm_represented_test_fail_allocation_after(0);
#define CHECK_METHOD_PARTIAL_ZERO(field) do { \
    SolWasmRepresentedLimits partial = sol_wasm_represented_default_limits(); \
    SolWasmRepresentedOutput rejected; partial.field = 0; sol_wasm_represented_output_init(&rejected); \
    CHECK(build_named_root(method_directory, "launch", &rejected, &partial, \
        SOL_WASM_REPRESENTED_INVALID_ARGUMENT) && rejected.bytes.bytes == NULL \
        && rejected.bytes.count == 0 && usage_zero(&rejected.usage)); \
    sol_wasm_represented_output_free(&rejected); \
} while (0)
    CHECK_METHOD_PARTIAL_ZERO(max_functions); CHECK_METHOD_PARTIAL_ZERO(max_blocks);
    CHECK_METHOD_PARTIAL_ZERO(max_edges); CHECK_METHOD_PARTIAL_ZERO(max_values);
    CHECK_METHOD_PARTIAL_ZERO(max_locals); CHECK_METHOD_PARTIAL_ZERO(max_generated_nodes);
    CHECK_METHOD_PARTIAL_ZERO(max_table_elements); CHECK_METHOD_PARTIAL_ZERO(max_static_data_bytes);
    CHECK_METHOD_PARTIAL_ZERO(max_allocation_requests); CHECK_METHOD_PARTIAL_ZERO(max_allocation_bytes);
    CHECK_METHOD_PARTIAL_ZERO(max_provenance_records); CHECK_METHOD_PARTIAL_ZERO(max_work_bytes);
    CHECK_METHOD_PARTIAL_ZERO(max_scratch_bytes); CHECK_METHOD_PARTIAL_ZERO(max_owned_bytes);
    CHECK_METHOD_PARTIAL_ZERO(max_output_bytes);
#undef CHECK_METHOD_PARTIAL_ZERO
    represented_control("methods.same-instance-writebacks");
    sol_wasm_represented_test_callback_writeback_probe(true);
    size_t method_ids[2];
    sol_wasm_represented_output_init(&method_probe);
    CHECK(build_multiroot(method_directory, false, &method_probe, method_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && sol_wasm_represented_validate(&method_probe.bytes)
            == SOL_WASM_REPRESENTED_OK);
    if (method_probe.bytes.bytes != NULL) {
        WasmInstance instance;
        CHECK(wasm_instance_open(&method_probe.bytes, method_entry_name, &instance)
            && wasm_instance_call_named(&instance, SOL_WASM_REPRESENTED_TEST_FAILURE_ENTRY_EXPORT,
                0, 2, -1) && wasm_instance_writebacks(&instance, 0));
        wasm_val_t method_failure_site;
        wasm_global_get(instance.site, &method_failure_site);
        CHECK(method_failure_site.kind == WASM_I32 && method_failure_site.of.i32 == 6);
        CHECK(wasm_instance_call(&instance, 83, 0, 0) && wasm_instance_writebacks(&instance, 1));
        wasm_instance_close(&instance);
        CHECK(wasm_instance_open(&method_probe.bytes, method_entry_name, &instance)
            && wasm_instance_writebacks(&instance, 0) && wasm_instance_call(&instance, 83, 0, 0)
            && wasm_instance_writebacks(&instance, 1));
        wasm_instance_close(&instance);
    }
    sol_wasm_represented_output_free(&method_probe);
    sol_wasm_represented_test_callback_writeback_probe(false);
    /* Rebuild, reverse the two genuine roots, and relocate the exact source:
       all retain the direct-method physical bytes and census. */
    sol_wasm_represented_output_init(&method_copy);
    CHECK(build_named_root(method_directory, "launch", &method_copy, NULL,
        SOL_WASM_REPRESENTED_OK) && method_copy.bytes.count == method.bytes.count
        && memcmp(method_copy.bytes.bytes, method.bytes.bytes, method.bytes.count) == 0
        && usage_equal(&method_copy.usage, &method.usage));
    sol_wasm_represented_output_free(&method_copy);
    SolWasmRepresentedOutput method_forward, method_reverse;
    size_t method_forward_ids[2], method_reverse_ids[2];
    sol_wasm_represented_output_init(&method_forward); sol_wasm_represented_output_init(&method_reverse);
    CHECK(build_multiroot(method_directory, false, &method_forward, method_forward_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && build_multiroot(method_directory, true, &method_reverse,
        method_reverse_ids, NULL, SOL_WASM_REPRESENTED_OK)
        && method_forward_ids[0] == method_reverse_ids[0]
        && method_forward_ids[1] == method_reverse_ids[1]
        && method_forward.bytes.count == method_reverse.bytes.count
        && memcmp(method_forward.bytes.bytes, method_reverse.bytes.bytes, method_forward.bytes.count) == 0
        && usage_equal(&method_forward.usage, &method_reverse.usage));
    sol_wasm_represented_output_free(&method_reverse); sol_wasm_represented_output_free(&method_forward);
    char method_relocated[512], method_source_path[768], method_destination[768];
    represented_control("methods.rootorder-relocation");
    (void)mkdir(SOL_TEST_BINARY_DIR, 0700);
    (void)snprintf(method_relocated, sizeof method_relocated, "%s/p43_method_relocated",
        SOL_TEST_BINARY_DIR);
    (void)mkdir(method_relocated, 0700);
    (void)snprintf(method_source_path, sizeof method_source_path, "%s/main.sol", method_directory);
    (void)snprintf(method_destination, sizeof method_destination, "%s/main.sol", method_relocated);
    FILE *method_input = fopen(method_source_path, "rb"); FILE *method_output = fopen(method_destination, "wb");
    CHECK(method_input != NULL && method_output != NULL);
    if (method_input != NULL && method_output != NULL) {
        uint8_t copy[256]; size_t count = 0;
        while ((count = fread(copy, 1, sizeof copy, method_input)) != 0)
            CHECK(fwrite(copy, 1, count, method_output) == count);
    }
    if (method_input != NULL) fclose(method_input);
    if (method_output != NULL) fclose(method_output);
    sol_wasm_represented_output_init(&method_copy);
    CHECK(build_named_root(method_relocated, "launch", &method_copy, NULL,
        SOL_WASM_REPRESENTED_OK) && method_copy.bytes.count == method.bytes.count
        && memcmp(method_copy.bytes.bytes, method.bytes.bytes, method.bytes.count) == 0
        && usage_equal(&method_copy.usage, &method.usage));
    sol_wasm_represented_output_free(&method_copy);
    sol_wasm_represented_output_free(&method);
}

static void run_callback_shape_controls(const char *callback_directory);

static void run_callback_table_wire(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("callback-table-wire.all-controls-mutations-quotas-sequences");
    char callback_directory[512];
    (void)snprintf(callback_directory, sizeof callback_directory,
        "%s/tests/conformance/p43_callback_prereq", SOL_TEST_SOURCE_DIR);
    char callback_one_directory[512], callback_three_directory[512];
    (void)snprintf(callback_one_directory, sizeof callback_one_directory,
        "%s/tests/conformance/p43_callback_one", SOL_TEST_SOURCE_DIR);
    (void)snprintf(callback_three_directory, sizeof callback_three_directory,
        "%s/tests/conformance/p43_callback_three", SOL_TEST_SOURCE_DIR);
    SolWasmRepresentedOutput output;
    SolWasmRepresentedOutput callback_one, callback_three;
    ProvenanceRecord callback_compact_entry;
    char callback_compact_entry_name[256];
    sol_wasm_represented_output_init(&callback_one);
    CHECK(build_named_root(callback_one_directory, "launch", &callback_one, NULL,
        SOL_WASM_REPRESENTED_OK) && callback_one.usage.table_elements == 2
        && sol_wasm_represented_validate(&callback_one.bytes) == SOL_WASM_REPRESENTED_OK
        && provenance_record(&callback_one.bytes, 1, &callback_compact_entry)
        && callback_compact_entry.symbol_count < sizeof callback_compact_entry_name);
    if (callback_one.bytes.bytes != NULL
        && provenance_record(&callback_one.bytes, 1, &callback_compact_entry)
        && callback_compact_entry.symbol_count < sizeof callback_compact_entry_name) {
        memcpy(callback_compact_entry_name, callback_compact_entry.symbol,
            callback_compact_entry.symbol_count);
        callback_compact_entry_name[callback_compact_entry.symbol_count] = '\0';
        CHECK(invoke_named(&callback_one.bytes, callback_compact_entry_name, 42, 0, 0));
    }
    sol_wasm_represented_output_free(&callback_one);
    sol_wasm_represented_output_init(&callback_three);
    CHECK(build_named_root(callback_three_directory, "launch", &callback_three, NULL,
        SOL_WASM_REPRESENTED_OK) && callback_three.usage.table_elements == 4
        && sol_wasm_represented_validate(&callback_three.bytes) == SOL_WASM_REPRESENTED_OK
        && provenance_record(&callback_three.bytes, 1, &callback_compact_entry)
        && callback_compact_entry.symbol_count < sizeof callback_compact_entry_name);
    if (callback_three.bytes.bytes != NULL
        && provenance_record(&callback_three.bytes, 1, &callback_compact_entry)
        && callback_compact_entry.symbol_count < sizeof callback_compact_entry_name) {
        memcpy(callback_compact_entry_name, callback_compact_entry.symbol,
            callback_compact_entry.symbol_count);
        callback_compact_entry_name[callback_compact_entry.symbol_count] = '\0';
        CHECK(invoke_named(&callback_three.bytes, callback_compact_entry_name, 7, 0, 0));
    }
    sol_wasm_represented_output_free(&callback_three);
    sol_wasm_represented_output_init(&output);
    CHECK(build_named_root(callback_directory, "launch", &output, NULL,
        SOL_WASM_REPRESENTED_OK));
    ProvenanceRecord callback_entry;
    char callback_entry_name[256];
    CHECK(output.usage.table_elements == 3 && output.bytes.bytes != NULL
        && provenance_record(&output.bytes, 1, &callback_entry)
        && callback_entry.symbol_count < sizeof callback_entry_name);
    if (output.bytes.bytes != NULL && provenance_record(&output.bytes, 1, &callback_entry)
        && callback_entry.symbol_count < sizeof callback_entry_name) {
        memcpy(callback_entry_name, callback_entry.symbol, callback_entry.symbol_count);
        callback_entry_name[callback_entry.symbol_count] = '\0';
        CHECK(invoke_named(&output.bytes, callback_entry_name, 42, 0, 0));
    }
    /* This synthetic control preserves the source-produced canonical
     * provenance/export entry while widening the private callback table to
     * three homogeneous targets.  It must pass both validators before every
     * shape mutation below is considered meaningful. */
    ProvenanceLayout valid_table_layout; SolWasmBackendBytes valid_table = {0};
    CHECK(provenance_layout(&output.bytes, &valid_table_layout)
        && make_shape_variant(&output.bytes, &valid_table_layout, SHAPE_VALID_TABLE, &valid_table)
        && valid_table.bytes != NULL && valid_table.count != 0
        && wasmtime_module_valid(&valid_table)
        && sol_wasm_represented_validate(&valid_table) == SOL_WASM_REPRESENTED_OK);
    free(valid_table.bytes);
    SolWasmBackendBytes imported_table = {0};
    CHECK(make_shape_variant(&output.bytes, &valid_table_layout, SHAPE_IMPORT, &imported_table)
        && imported_table.bytes != NULL && imported_table.count != 0
        && wasmtime_module_valid(&imported_table)
        && sol_wasm_represented_validate(&imported_table) == SOL_WASM_REPRESENTED_INVALID_INPUT);
    free(imported_table.bytes);
    static const SolWasmRepresentedUsage expected_callback_usage = {8,7,4,16,75,1435,3,0,0,0,75,38647,16499,23445,13205};
    static const uint8_t expected_callback_hash[32] = {0x82,0xa7,0xe5,0xab,0x55,0x78,0xde,0x0c,0x5f,0x0c,0xfd,0x14,0x96,0x64,0x9a,0x4e,0xf7,0x7a,0x2c,0xb9,0xb6,0x6b,0x0d,0xc7,0xba,0x2a,0xf7,0x57,0x87,0x3c,0x96,0x34};
    uint8_t callback_hash[32]; sha256(output.bytes.bytes, output.bytes.count, callback_hash);
    CHECK(usage_equal(&output.usage, &expected_callback_usage)
        && memcmp(callback_hash, expected_callback_hash, sizeof callback_hash) == 0);
    int32_t callback_sites[2] = {0, 0}; size_t callback_site_count = 0;
    for (uint32_t record = 1; record <= output.usage.provenance_records; ++record) {
        ProvenanceRecord candidate;
        if (!provenance_record(&output.bytes, record, &candidate)) break;
        if (candidate.tag == 4 && callback_site_count < 2)
            callback_sites[callback_site_count++] = (int32_t)record;
    }
    CHECK(callback_site_count == 2 && callback_sites[0] == 9
        && callback_sites[1] == 10);
    static const char callback_source[] =
        "module conformance.p43_callback_prereq\n\n"
        "function increment(value: Int64) -> Int64 effects { pure } { return value + 1 }\n"
        "function decrement(value: Int64) -> Int64 effects { pure } { return value - 1 }\n\n"
        "@entry\npublic function launch() -> Int64 effects { pure } {\n"
        "    let alternate = decrement\n"
        "    let ignored = alternate(43)\n"
        "    let callback = increment\n"
        "    return callback(41)\n"
        "}\n";
    static const char launch_key[] =
        "sol.i1.7d1a8bd7cbca327e2d3f548d225b9de7.b446623075723e926df41e2e94dba6ec6bdf60a23a78b3d3b1b5871fb6935118";
    static const char entry_key[] =
        "sol.e1.7d1a8bd7cbca327e2d3f548d225b9de7.b446623075723e926df41e2e94dba6ec6bdf60a23a78b3d3b1b5871fb6935118";
    static const char producer_keys[][129] = {
        "sol.i1.1263b167d82565a44601707984eca9d7.e7bf7ac66c43567b1b6e47bd2808a8750d2feeb957c86d0c27706c2a35e6c910",
        "sol.i1.f16d89319e9c8accfae366b04a1c0c5f.904e6d4187713fce566248a42aaf8e666129d1b5de7b78d7fc551820dd53925d",
    };
    const char *first_function_value = strstr(callback_source, "decrement\n");
    const char *second_function_value = strstr(callback_source, "increment\n");
    ProvenanceRecord increment_callable, decrement_callable, first_producer, second_producer;
    CHECK(first_function_value != NULL && second_function_value != NULL
        && provenance_record(&output.bytes, 2, &increment_callable)
        && provenance_record(&output.bytes, 3, &decrement_callable)
        && increment_callable.tag == 2 && increment_callable.start == 40
        && increment_callable.end == 119 && bytes_equal(increment_callable.symbol,
            increment_callable.symbol_count, producer_keys[1])
        && decrement_callable.tag == 2 && decrement_callable.start == 120
        && decrement_callable.end == 199 && bytes_equal(decrement_callable.symbol,
            decrement_callable.symbol_count, producer_keys[0])
        && provenance_record(&output.bytes, (uint32_t)callback_sites[0], &first_producer)
        && provenance_record(&output.bytes, (uint32_t)callback_sites[1], &second_producer)
        && first_producer.tag == 4 && first_producer.kind == 0
        && first_producer.start == (uint32_t)(first_function_value - callback_source)
        && first_producer.end == (uint32_t)(first_function_value - callback_source)
            + (uint32_t)strlen("decrement")
        && bytes_equal(first_producer.symbol, first_producer.symbol_count, launch_key)
        && second_producer.tag == 4 && second_producer.kind == 0
        && second_producer.start == (uint32_t)(second_function_value - callback_source)
        && second_producer.end == (uint32_t)(second_function_value - callback_source)
            + (uint32_t)strlen("increment")
        && bytes_equal(second_producer.symbol, second_producer.symbol_count, launch_key));
    static const uint8_t callback_tags[] = {1,2,2,2,3,3,3,3,4,4};
    static const uint8_t callback_kinds[] = {0,0,0,0,0,0,1,1,0,0};
    static const uint32_t callback_starts[] = {259,40,120,201,108,188,309,363,281,342};
    static const uint32_t callback_ends[] = {377,119,199,377,117,197,322,375,290,351};
    static const uint32_t callback_ordinals[] = {0,0,0,0,2,3,0,1,0,0};
    const char *const callback_symbols[] = {entry_key, producer_keys[1], producer_keys[0], launch_key,
        producer_keys[1], producer_keys[0], launch_key, launch_key, launch_key, launch_key};
    for (uint32_t record = 1; record <= sizeof callback_tags / sizeof *callback_tags; ++record) {
        ProvenanceRecord candidate;
        CHECK(provenance_record(&output.bytes, record, &candidate)
            && bytes_equal(candidate.path, candidate.path_count, "main.sol")
            && candidate.tag == callback_tags[record - 1]
            && candidate.kind == callback_kinds[record - 1]
            && candidate.start == callback_starts[record - 1]
            && candidate.end == callback_ends[record - 1]
            && candidate.ordinal == callback_ordinals[record - 1]
            && bytes_equal(candidate.symbol, candidate.symbol_count,
                callback_symbols[record - 1]));
    }
    bool step_ordinals[75] = {false};
    size_t image_steps = 0, predicate_steps = 0;
    for (uint32_t record = 1; record <= output.usage.provenance_records; ++record) {
        ProvenanceRecord candidate;
        CHECK(provenance_record(&output.bytes, record, &candidate)
            && bytes_equal(candidate.path, candidate.path_count, "main.sol"));
        bool image_step = candidate.tag == 3
            && candidate.kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_STEP;
        bool predicate_step = candidate.tag == 3
            && candidate.kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_STEP;
        if (image_step || predicate_step) {
            CHECK(record > sizeof callback_tags / sizeof *callback_tags
                && candidate.ordinal < sizeof step_ordinals / sizeof *step_ordinals
                && !step_ordinals[candidate.ordinal]);
            if (candidate.ordinal < sizeof step_ordinals / sizeof *step_ordinals)
                step_ordinals[candidate.ordinal] = true;
        }
        image_steps += image_step;
        predicate_steps += predicate_step;
    }
    CHECK(image_steps == 65 && predicate_steps == 0
        && image_steps + predicate_steps + sizeof callback_tags / sizeof *callback_tags
            == output.usage.provenance_records);
    SolWasmBackendBytes callback_without_table = {0};
    CHECK(without_standard_section(&output.bytes, 4, &callback_without_table)
        && sol_wasm_represented_validate(&callback_without_table)
            == SOL_WASM_REPRESENTED_INVALID_INPUT);
    free(callback_without_table.bytes);
    /* Freeze every nonzero physical build field, including the sentinel plus
     * two private callback table entries.  Runtime quotas are exercised below. */
#define CHECK_CALLBACK_BUILD_CAP(field, exact) do { \
    SolWasmRepresentedLimits cap_limits = sol_wasm_represented_default_limits(); \
    SolWasmRepresentedOutput capped; cap_limits.field = (exact); \
    sol_wasm_represented_output_init(&capped); \
    CHECK(build_named_root(callback_directory, "launch", &capped, &cap_limits, \
        SOL_WASM_REPRESENTED_OK) && capped.bytes.count == output.bytes.count \
        && memcmp(capped.bytes.bytes, output.bytes.bytes, output.bytes.count) == 0 \
        && usage_equal(&capped.usage, &output.usage)); \
    sol_wasm_represented_output_free(&capped); cap_limits.field = (exact) - 1; \
    sol_wasm_represented_output_init(&capped); \
    CHECK(build_named_root(callback_directory, "launch", &capped, &cap_limits, \
        SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED) && capped.bytes.bytes == NULL \
        && capped.bytes.count == 0 && usage_zero(&capped.usage)); \
    sol_wasm_represented_output_free(&capped); \
} while (0)
    CHECK_CALLBACK_BUILD_CAP(max_functions, 8u); CHECK_CALLBACK_BUILD_CAP(max_blocks, 7u);
    CHECK_CALLBACK_BUILD_CAP(max_edges, 4u); CHECK_CALLBACK_BUILD_CAP(max_values, 16u);
    CHECK_CALLBACK_BUILD_CAP(max_locals, 75u); CHECK_CALLBACK_BUILD_CAP(max_generated_nodes, 1435u);
    CHECK_CALLBACK_BUILD_CAP(max_table_elements, 3u);
    CHECK_CALLBACK_BUILD_CAP(max_provenance_records, 75u); CHECK_CALLBACK_BUILD_CAP(max_work_bytes, 38647u);
    CHECK_CALLBACK_BUILD_CAP(max_scratch_bytes, 16499u); CHECK_CALLBACK_BUILD_CAP(max_owned_bytes, 23445u);
    CHECK_CALLBACK_BUILD_CAP(max_output_bytes, 13205u);
#undef CHECK_CALLBACK_BUILD_CAP
    SolWasmRepresentedLimits callback_defaults = {0}; SolWasmRepresentedOutput callback_copy;
    sol_wasm_represented_output_init(&callback_copy);
    CHECK(build_named_root(callback_directory, "launch", &callback_copy, &callback_defaults,
        SOL_WASM_REPRESENTED_OK) && callback_copy.bytes.count == output.bytes.count
        && memcmp(callback_copy.bytes.bytes, output.bytes.bytes, output.bytes.count) == 0
        && usage_equal(&callback_copy.usage, &output.usage));
    sol_wasm_represented_output_free(&callback_copy);
#define CHECK_CALLBACK_PARTIAL_ZERO(field) do { \
    SolWasmRepresentedLimits partial = sol_wasm_represented_default_limits(); \
    SolWasmRepresentedOutput rejected; partial.field = 0; sol_wasm_represented_output_init(&rejected); \
    CHECK(build_named_root(callback_directory, "launch", &rejected, &partial, \
        SOL_WASM_REPRESENTED_INVALID_ARGUMENT) && rejected.bytes.bytes == NULL \
        && rejected.bytes.count == 0 && usage_zero(&rejected.usage)); \
    sol_wasm_represented_output_free(&rejected); \
} while (0)
    CHECK_CALLBACK_PARTIAL_ZERO(max_functions); CHECK_CALLBACK_PARTIAL_ZERO(max_blocks);
    CHECK_CALLBACK_PARTIAL_ZERO(max_edges); CHECK_CALLBACK_PARTIAL_ZERO(max_values);
    CHECK_CALLBACK_PARTIAL_ZERO(max_locals); CHECK_CALLBACK_PARTIAL_ZERO(max_generated_nodes);
    CHECK_CALLBACK_PARTIAL_ZERO(max_table_elements); CHECK_CALLBACK_PARTIAL_ZERO(max_static_data_bytes);
    CHECK_CALLBACK_PARTIAL_ZERO(max_allocation_requests); CHECK_CALLBACK_PARTIAL_ZERO(max_allocation_bytes);
    CHECK_CALLBACK_PARTIAL_ZERO(max_provenance_records); CHECK_CALLBACK_PARTIAL_ZERO(max_work_bytes);
    CHECK_CALLBACK_PARTIAL_ZERO(max_scratch_bytes); CHECK_CALLBACK_PARTIAL_ZERO(max_owned_bytes);
    CHECK_CALLBACK_PARTIAL_ZERO(max_output_bytes);
#undef CHECK_CALLBACK_PARTIAL_ZERO
    sol_wasm_represented_output_init(&callback_copy);
    CHECK(build_named_root(callback_directory, "launch", &callback_copy, NULL,
        SOL_WASM_REPRESENTED_OK));
    size_t callback_allocation_count = sol_wasm_represented_test_allocation_attempts();
    CHECK(callback_allocation_count == 122);
    sol_wasm_represented_output_free(&callback_copy);
    for (size_t attempt = 1; attempt <= callback_allocation_count; ++attempt) {
        SolWasmRepresentedOutput failed, retry;
        represented_fault(FAULT_CALLBACK, attempt, attempt); sol_wasm_represented_output_init(&failed);
        CHECK(build_named_root(callback_directory, "launch", &failed, NULL,
            SOL_WASM_REPRESENTED_ALLOCATION_FAILED) && failed.bytes.bytes == NULL
            && failed.bytes.count == 0 && usage_zero(&failed.usage));
        sol_wasm_represented_output_free(&failed); sol_wasm_represented_test_fail_allocation_after(0);
        sol_wasm_represented_output_init(&retry);
        CHECK(build_named_root(callback_directory, "launch", &retry, NULL, SOL_WASM_REPRESENTED_OK)
            && retry.bytes.count == output.bytes.count
            && memcmp(retry.bytes.bytes, output.bytes.bytes, output.bytes.count) == 0
            && usage_equal(&retry.usage, &output.usage));
        sol_wasm_represented_output_free(&retry);
    }
    sol_wasm_represented_test_fail_allocation_after(0);
    SolWasmRepresentedOutput callback_forward, callback_reverse;
    size_t callback_forward_ids[2], callback_reverse_ids[2];
    sol_wasm_represented_output_init(&callback_forward); sol_wasm_represented_output_init(&callback_reverse);
    CHECK(build_multiroot(callback_directory, false, &callback_forward, callback_forward_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && build_multiroot(callback_directory, true, &callback_reverse,
            callback_reverse_ids, NULL, SOL_WASM_REPRESENTED_OK)
        && callback_forward_ids[0] != callback_forward_ids[1]
        && callback_reverse_ids[0] != callback_reverse_ids[1]
        && callback_forward_ids[0] == callback_reverse_ids[0]
        && callback_forward_ids[1] == callback_reverse_ids[1]
        && callback_forward.bytes.count == output.bytes.count
        && callback_reverse.bytes.count == output.bytes.count
        && memcmp(callback_forward.bytes.bytes, output.bytes.bytes, output.bytes.count) == 0
        && memcmp(callback_reverse.bytes.bytes, output.bytes.bytes, output.bytes.count) == 0
        && usage_equal(&callback_forward.usage, &output.usage)
        && usage_equal(&callback_reverse.usage, &output.usage));
    sol_wasm_represented_output_free(&callback_reverse); sol_wasm_represented_output_free(&callback_forward);
    char callback_relocated[512], callback_source_path[768], callback_destination[768];
    (void)mkdir(SOL_TEST_BINARY_DIR, 0700);
    (void)snprintf(callback_relocated, sizeof callback_relocated, "%s/p43_callback_relocated",
        SOL_TEST_BINARY_DIR);
    (void)mkdir(callback_relocated, 0700);
    (void)snprintf(callback_source_path, sizeof callback_source_path, "%s/main.sol", callback_directory);
    (void)snprintf(callback_destination, sizeof callback_destination, "%s/main.sol", callback_relocated);
    FILE *callback_input = fopen(callback_source_path, "rb");
    FILE *callback_output = fopen(callback_destination, "wb");
    CHECK(callback_input != NULL && callback_output != NULL);
    if (callback_input != NULL && callback_output != NULL) {
        uint8_t copied[256]; size_t copied_count = 0;
        while ((copied_count = fread(copied, 1, sizeof copied, callback_input)) != 0)
            CHECK(fwrite(copied, 1, copied_count, callback_output) == copied_count);
    }
    if (callback_input != NULL) fclose(callback_input);
    if (callback_output != NULL) fclose(callback_output);
    sol_wasm_represented_output_init(&callback_copy);
    CHECK(build_named_root(callback_relocated, "launch", &callback_copy, NULL,
        SOL_WASM_REPRESENTED_OK) && callback_copy.bytes.count == output.bytes.count
        && memcmp(callback_copy.bytes.bytes, output.bytes.bytes, output.bytes.count) == 0
        && usage_equal(&callback_copy.usage, &output.usage));
    sol_wasm_represented_output_free(&callback_copy);
    sol_wasm_represented_output_free(&output);
    SolWasmRepresentedLimits callback_wire_cap = sol_wasm_represented_default_limits();
    callback_wire_cap.max_functions = 256; callback_wire_cap.max_table_elements = 256;
    sol_wasm_represented_output_init(&output);
    CHECK(build_named_root(callback_directory, "launch", &output, &callback_wire_cap,
        SOL_WASM_REPRESENTED_OK) && output.bytes.bytes != NULL && output.bytes.count != 0);
    sol_wasm_represented_output_free(&output);
    callback_wire_cap.max_functions = 257;
    sol_wasm_represented_output_init(&output);
    CHECK(build_named_root(callback_directory, "launch", &output, &callback_wire_cap,
        SOL_WASM_REPRESENTED_INVALID_ARGUMENT) && output.bytes.bytes == NULL
        && output.bytes.count == 0 && usage_zero(&output.usage));
    sol_wasm_represented_output_free(&output);
    callback_wire_cap = sol_wasm_represented_default_limits();
    callback_wire_cap.max_table_elements = 257;
    sol_wasm_represented_output_init(&output);
    CHECK(build_named_root(callback_directory, "launch", &output, &callback_wire_cap,
        SOL_WASM_REPRESENTED_INVALID_ARGUMENT) && output.bytes.bytes == NULL
        && output.bytes.count == 0 && usage_zero(&output.usage));
    sol_wasm_represented_output_free(&output);
    SolWasmRepresentedLimits callback_table_limit = sol_wasm_represented_default_limits();
    callback_table_limit.max_table_elements = 2; /* sentinel plus one slot is insufficient. */
    sol_wasm_represented_output_init(&output);
    CHECK(build_named_root(callback_directory, "launch", &output, &callback_table_limit,
        SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED));
    CHECK(output.bytes.bytes == NULL && output.bytes.count == 0);
    sol_wasm_represented_output_free(&output);

    /* Each unbound producer has P2's four-byte callable header demand.  The
     * second producer is consequently the precise one-below quota/grow site. */
    SolWasmRepresentedLimits callback_quota = sol_wasm_represented_default_limits();
    represented_control("callback-table-wire.runtime-quotas-same-instance-grow");
    callback_quota.max_allocation_requests = 1; callback_quota.max_allocation_bytes = 4;
    sol_wasm_represented_test_allocator_quota(0, UINT64_MAX);
    sol_wasm_represented_output_init(&output);
    CHECK(build_named_root(callback_directory, "launch", &output, &callback_quota,
        SOL_WASM_REPRESENTED_OK));
    CHECK(invoke_named(&output.bytes, callback_entry_name, 0, 5, callback_sites[0]));
    sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_allocator_quota(UINT64_MAX, UINT64_MAX);
    callback_quota.max_allocation_requests = 1; callback_quota.max_allocation_bytes = 4;
    sol_wasm_represented_output_init(&output);
    CHECK(build_named_root(callback_directory, "launch", &output, &callback_quota,
        SOL_WASM_REPRESENTED_OK));
    CHECK(invoke_named(&output.bytes, callback_entry_name, 0, 5, callback_sites[1]));
    sol_wasm_represented_output_free(&output);
    callback_quota = sol_wasm_represented_default_limits();
    callback_quota.max_allocation_requests = 2; callback_quota.max_allocation_bytes = 8;
    sol_wasm_represented_output_init(&output);
    CHECK(build_named_root(callback_directory, "launch", &output, &callback_quota,
        SOL_WASM_REPRESENTED_OK));
    WasmInstance callback_instance;
    CHECK(wasm_instance_open(&output.bytes, callback_entry_name, &callback_instance));
    if (callback_instance.instance != NULL) {
        CHECK(wasm_instance_call(&callback_instance, 42, 0, 0));
        CHECK(wasm_instance_call(&callback_instance, 42, 0, 0));
    }
    wasm_instance_close(&callback_instance);
    sol_wasm_represented_output_free(&output);
    callback_quota = sol_wasm_represented_default_limits();
    callback_quota.max_allocation_bytes = 8;
    sol_wasm_represented_output_init(&output);
    CHECK(build_named_root(callback_directory, "launch", &output, &callback_quota,
        SOL_WASM_REPRESENTED_OK) && invoke_named(&output.bytes, callback_entry_name, 42, 0, 0));
    sol_wasm_represented_output_free(&output);
    callback_quota.max_allocation_bytes = 7;
    sol_wasm_represented_output_init(&output);
    CHECK(build_named_root(callback_directory, "launch", &output, &callback_quota,
        SOL_WASM_REPRESENTED_OK) && invoke_named(&output.bytes, callback_entry_name, 0, 5,
            callback_sites[1]));
    sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_allocator_memory(1, UINT32_C(65528));
    sol_wasm_represented_output_init(&output);
    CHECK(build_named_root(callback_directory, "launch", &output, NULL,
        SOL_WASM_REPRESENTED_OK));
    CHECK(invoke_named(&output.bytes, callback_entry_name, 0, 4, callback_sites[1]));
    sol_wasm_represented_output_free(&output);
    sol_wasm_represented_test_allocator_memory(0, 0);
    char callback_incompatible_directory[512];
    (void)snprintf(callback_incompatible_directory, sizeof callback_incompatible_directory,
        "%s/tests/conformance/p43_callback_incompatible_signature", SOL_TEST_SOURCE_DIR);
    CHECK(callback_signature_fail_closed(callback_incompatible_directory));
    run_callback_shape_controls(callback_directory);
    /* C1's closed zero-argument callback entry cannot source a checked
     * overflow/divide-by-zero target operand.  Leave leaf codes 6/7 to the
     * later operand-bearing callback slice; this checkpoint asserts no such
     * local packet. */

}

static void run_generic_roots(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("generic-roots.all-controls-mutations-quotas-sequences");
    /* Both roots are genuine, distinct IR IDs. Reversing their requested order
     * and relocating the package must not perturb physical bytes or census. */
    char multiroot_directory[512], relocated_directory[512], source_path[768], relocated_source[768];
    (void)snprintf(multiroot_directory, sizeof multiroot_directory,
        "%s/tests/conformance/p43_multiroot", SOL_TEST_SOURCE_DIR);
    SolWasmRepresentedOutput roots_forward, roots_reverse, roots_relocated;
    size_t forward_ids[2], reverse_ids[2], relocated_ids[2];
    sol_wasm_represented_output_init(&roots_forward);
    sol_wasm_represented_output_init(&roots_reverse);
    sol_wasm_represented_output_init(&roots_relocated);
    CHECK(build_multiroot(multiroot_directory, false, &roots_forward, forward_ids, NULL,
        SOL_WASM_REPRESENTED_OK));
    CHECK(build_multiroot(multiroot_directory, true, &roots_reverse, reverse_ids, NULL,
        SOL_WASM_REPRESENTED_OK));
    CHECK(forward_ids[0] != forward_ids[1] && reverse_ids[0] != reverse_ids[1]
        && forward_ids[0] == reverse_ids[0] && forward_ids[1] == reverse_ids[1]);
    CHECK(roots_forward.bytes.count != 0 && roots_forward.bytes.count == roots_reverse.bytes.count
        && memcmp(roots_forward.bytes.bytes, roots_reverse.bytes.bytes, roots_forward.bytes.count) == 0
        && memcmp(&roots_forward.usage, &roots_reverse.usage, sizeof roots_forward.usage) == 0);
    uint8_t roots_forward_hash[32], roots_reverse_hash[32];
    sha256(roots_forward.bytes.bytes, roots_forward.bytes.count, roots_forward_hash);
    sha256(roots_reverse.bytes.bytes, roots_reverse.bytes.count, roots_reverse_hash);
    CHECK(memcmp(roots_forward_hash, roots_reverse_hash, sizeof roots_forward_hash) == 0);
    (void)mkdir(SOL_TEST_BINARY_DIR, 0700);
    (void)snprintf(relocated_directory, sizeof relocated_directory, "%s/relocated", SOL_TEST_BINARY_DIR);
    (void)mkdir(relocated_directory, 0700);
    (void)snprintf(source_path, sizeof source_path, "%s/main.sol", multiroot_directory);
    (void)snprintf(relocated_source, sizeof relocated_source, "%s/main.sol", relocated_directory);
    FILE *source_file = fopen(source_path, "rb"); FILE *relocated_file = fopen(relocated_source, "wb");
    CHECK(source_file != NULL && relocated_file != NULL);
    if (source_file != NULL && relocated_file != NULL) {
        uint8_t copy_buffer[256]; size_t read = 0;
        while ((read = fread(copy_buffer, 1, sizeof copy_buffer, source_file)) != 0)
            CHECK(fwrite(copy_buffer, 1, read, relocated_file) == read);
    }
    if (source_file != NULL) fclose(source_file);
    if (relocated_file != NULL) fclose(relocated_file);
    CHECK(build_multiroot(relocated_directory, false, &roots_relocated, relocated_ids, NULL,
        SOL_WASM_REPRESENTED_OK));
    CHECK(relocated_ids[0] == forward_ids[0] && relocated_ids[1] == forward_ids[1]
        && roots_relocated.bytes.count == roots_forward.bytes.count
        && memcmp(roots_relocated.bytes.bytes, roots_forward.bytes.bytes, roots_forward.bytes.count) == 0
        && memcmp(&roots_relocated.usage, &roots_forward.usage, sizeof roots_forward.usage) == 0);
    sol_wasm_represented_output_free(&roots_relocated);
    sol_wasm_represented_output_free(&roots_reverse);
    sol_wasm_represented_output_free(&roots_forward);
}

static void run_scalar_products_late(SolWasmRepresentedOutput *forward,
    SolWasmRepresentedOutput *reverse);

static void run_scalar_products(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("scalar-products.all-controls-mutations-quotas-sequences");
    /* Closed Slice A: direct scalar P2 products allocate one fixed object and
     * projections load their exact P2 field offset.  The two-root harness also
     * proves the new physical helper remains deterministic. */
    char product_directory[512];
    (void)snprintf(product_directory, sizeof product_directory,
        "%s/tests/conformance/p43_scalar_product", SOL_TEST_SOURCE_DIR);
    SolWasmRepresentedOutput product_forward, product_reverse;
    size_t product_ids[2], product_reverse_ids[2];
    sol_wasm_represented_output_init(&product_forward);
    sol_wasm_represented_output_init(&product_reverse);
    CHECK(build_multiroot(product_directory, false, &product_forward, product_ids, NULL,
        SOL_WASM_REPRESENTED_OK));
    CHECK(build_multiroot(product_directory, true, &product_reverse, product_reverse_ids, NULL,
        SOL_WASM_REPRESENTED_OK));
    CHECK(product_ids[0] != product_ids[1] && product_ids[0] == product_reverse_ids[0]
        && product_ids[1] == product_reverse_ids[1] && product_forward.bytes.count != 0
        && product_forward.bytes.count == product_reverse.bytes.count
        && memcmp(product_forward.bytes.bytes, product_reverse.bytes.bytes,
            product_forward.bytes.count) == 0
        && sol_wasm_represented_validate(&product_forward.bytes) == SOL_WASM_REPRESENTED_OK);
    uint8_t product_hash[32];
    sha256(product_forward.bytes.bytes, product_forward.bytes.count, product_hash);
    static const uint8_t expected_product_hash[32] = {0xa3,0xc8,0xe7,0x93,0x76,0xd6,0xee,0x87,0xfb,0xb2,0x46,0x7d,0xdf,0xe4,0x24,0xd8,0x0c,0xaa,0x97,0x5f,0xd6,0x2b,0xa3,0x97,0x1d,0xc1,0xb8,0x11,0x8b,0xb0,0x93,0x7b};
    CHECK(memcmp(product_hash, expected_product_hash, sizeof product_hash) == 0);
    CHECK(usage_equal(&product_forward.usage, &(SolWasmRepresentedUsage){7,12,14,34,95,1861,0,0,0,0,91,45572,19872,26383,16143}));
    ProvenanceRecord product_entry;
    char product_entry_name[256];
    CHECK(provenance_record(&product_forward.bytes, 1, &product_entry)
        && product_entry.symbol_count < sizeof product_entry_name);
    if (provenance_record(&product_forward.bytes, 1, &product_entry)
        && product_entry.symbol_count < sizeof product_entry_name) {
        memcpy(product_entry_name, product_entry.symbol, product_entry.symbol_count);
        product_entry_name[product_entry.symbol_count] = '\0';
        CHECK(invoke_named(&product_forward.bytes, product_entry_name, 42, 0, 0));
    }
    int32_t first_product_site = 0, third_product_site = 0;
    size_t product_sites = 0;
    for (uint32_t record = 1; record <= 16; ++record) {
        ProvenanceRecord candidate;
        if (!provenance_record(&product_forward.bytes, record, &candidate)) break;
        if (candidate.tag != 4) continue;
        ++product_sites;
        if (product_sites == 1) first_product_site = (int32_t)record;
        if (product_sites == 3) { third_product_site = (int32_t)record; break; }
    }
    CHECK(first_product_site == 6); /* Pair construction's canonical P3 site. */
    CHECK(third_product_site == 8); /* Tuple construction's canonical P3 site. */
    /* All three direct P2 objects are 16 bytes.  Exact demand succeeds twice
     * in one instance because wrappers reset the private allocator; the caps
     * one request/byte below fail at the first authenticated constructor. */
    SolWasmRepresentedLimits product_limits = sol_wasm_represented_default_limits();
    represented_control("scalar-products.same-instance-quotas-grow");
    SolWasmRepresentedOutput product_constrained;
    product_limits.max_allocation_requests = 3;
    product_limits.max_allocation_bytes = 48;
    sol_wasm_represented_output_init(&product_constrained);
    CHECK(build_multiroot(product_directory, false, &product_constrained, product_ids,
        &product_limits, SOL_WASM_REPRESENTED_OK));
    WasmInstance product_instance;
    CHECK(wasm_instance_open(&product_constrained.bytes, product_entry_name, &product_instance));
    if (product_instance.instance != NULL) {
        CHECK(wasm_instance_call(&product_instance, 42, 0, 0));
        CHECK(wasm_instance_call(&product_instance, 42, 0, 0));
    }
    wasm_instance_close(&product_instance);
    sol_wasm_represented_output_free(&product_constrained);
    product_limits = sol_wasm_represented_default_limits();
    product_limits.max_allocation_requests = 2;
    sol_wasm_represented_output_init(&product_constrained);
    CHECK(build_multiroot(product_directory, false, &product_constrained, product_ids,
        &product_limits, SOL_WASM_REPRESENTED_OK));
    CHECK(invoke_named(&product_constrained.bytes, product_entry_name, 0, 5, third_product_site));
    sol_wasm_represented_output_free(&product_constrained);
    product_limits = sol_wasm_represented_default_limits();
    product_limits.max_allocation_bytes = 47;
    sol_wasm_represented_output_init(&product_constrained);
    CHECK(build_multiroot(product_directory, false, &product_constrained, product_ids,
        &product_limits, SOL_WASM_REPRESENTED_OK));
    CHECK(invoke_named(&product_constrained.bytes, product_entry_name, 0, 5, third_product_site));
    sol_wasm_represented_output_free(&product_constrained);
    sol_wasm_represented_test_allocator_quota(0, UINT64_MAX);
    sol_wasm_represented_output_init(&product_constrained);
    CHECK(build_multiroot(product_directory, false, &product_constrained, product_ids, NULL,
        SOL_WASM_REPRESENTED_OK));
    CHECK(invoke_named(&product_constrained.bytes, product_entry_name, 0, 5, first_product_site));
    sol_wasm_represented_output_free(&product_constrained);
    sol_wasm_represented_test_allocator_quota(UINT64_MAX, 15);
    sol_wasm_represented_output_init(&product_constrained);
    CHECK(build_multiroot(product_directory, false, &product_constrained, product_ids, NULL,
        SOL_WASM_REPRESENTED_OK));
    CHECK(invoke_named(&product_constrained.bytes, product_entry_name, 0, 5, first_product_site));
    sol_wasm_represented_output_free(&product_constrained);
    sol_wasm_represented_test_allocator_quota(UINT64_MAX, UINT64_MAX);
    sol_wasm_represented_test_allocator_memory(1, UINT32_C(65528));
    sol_wasm_represented_output_init(&product_constrained);
    CHECK(build_multiroot(product_directory, false, &product_constrained, product_ids, NULL,
        SOL_WASM_REPRESENTED_OK));
    CHECK(invoke_named(&product_constrained.bytes, product_entry_name, 0, 4, first_product_site));
    sol_wasm_represented_output_free(&product_constrained);
    sol_wasm_represented_test_allocator_memory(0, 0);
    run_scalar_products_late(&product_forward, &product_reverse);
}

static void run_patterns(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("patterns.all-controls-mutations-quotas-sequences");
    /* Slice B1 executes P2's flattened match rows directly.  The zero-argument
     * scalar entry constructs the one inhabited Text variant, then the binding
     * performs the sole PATTERN_VALUE deep copy before scalar equality. */
    char pattern_directory[512];
    (void)snprintf(pattern_directory, sizeof pattern_directory,
        "%s/tests/conformance/p43_pattern_copy", SOL_TEST_SOURCE_DIR);
    SolWasmRepresentedOutput pattern; size_t pattern_ids[2];
    sol_wasm_represented_output_init(&pattern);
    CHECK(build_multiroot(pattern_directory, false, &pattern, pattern_ids, NULL,
        SOL_WASM_REPRESENTED_OK));
    ProvenanceRecord pattern_entry; char pattern_entry_name[256] = {0};
    CHECK(provenance_record(&pattern.bytes, 1, &pattern_entry)
        && pattern_entry.symbol_count < sizeof pattern_entry_name);
    if (provenance_record(&pattern.bytes, 1, &pattern_entry)
        && pattern_entry.symbol_count < sizeof pattern_entry_name) {
        memcpy(pattern_entry_name, pattern_entry.symbol, pattern_entry.symbol_count);
        pattern_entry_name[pattern_entry.symbol_count] = '\0';
        CHECK(invoke_named(&pattern.bytes, pattern_entry_name, 42, 0, 0));
    }
    CHECK(sol_wasm_represented_validate(&pattern.bytes) == SOL_WASM_REPRESENTED_OK);
    static const uint8_t expected_pattern_hash[32] = {0x74,0x9e,0x45,0x56,0xd6,0xd4,0x01,0x6c,0x58,0x8b,0x51,0xfd,0xb5,0xd5,0x8d,0xe9,0x4d,0xdf,0xcd,0x26,0xdf,0x4d,0x5a,0x52,0x5d,0x4b,0x35,0x76,0xf1,0x2b,0xac,0xdc};
    uint8_t pattern_hash[32]; sha256(pattern.bytes.bytes, pattern.bytes.count, pattern_hash);
    CHECK(memcmp(pattern_hash, expected_pattern_hash, sizeof pattern_hash) == 0);
    CHECK(usage_equal(&pattern.usage, &(SolWasmRepresentedUsage){8,8,7,15,66,1421,0,40,0,0,62,35580,13760,21624,11384}));
    /* The binding's source span is the authenticated supplemental allocation
     * provenance record for PATTERN_VALUE, not either Text literal. */
    static const char pattern_source[] =
        "module conformance.p43_pattern_copy\n\n"
        "enum Payload {\n    item(value: Text),\n}\n\n"
        "@entry\npublic function launch() -> Int64 effects { pure } {\n"
        "    let source = Payload.item(\"pattern-copy\")\n"
        "    return match source {\n        item(selected) => if selected == \"pattern-copy\" { 42 } else { 0 }\n    }\n}\n\n"
        "function second() -> Int64 effects { pure } { return 0 }\n";
    const char *binding = strstr(pattern_source, "item(selected)");
    uint32_t pattern_copy_site = 0; size_t pattern_sites = 0;
    CHECK(binding != NULL);
    for (uint32_t record = 1; binding != NULL && record <= 64; ++record) {
        ProvenanceRecord candidate;
        if (!provenance_record(&pattern.bytes, record, &candidate)) break;
        if (candidate.tag != 4) continue;
        ++pattern_sites;
        if (candidate.start == (uint32_t)(binding - pattern_source + 5)
            && candidate.end == (uint32_t)(binding - pattern_source + 13)
            && bytes_equal(candidate.path, candidate.path_count, "main.sol")) {
            CHECK(pattern_copy_site == 0); pattern_copy_site = record;
        }
    }
    CHECK(pattern_sites == 6 && pattern_copy_site == 8);
    ProvenanceLayout pattern_layout;
    CHECK(provenance_layout(&pattern.bytes, &pattern_layout)
        && pattern_layout.count == pattern.usage.provenance_records);
    for (uint32_t record = 1; record <= pattern_layout.count; ++record) {
        ProvenanceRecord candidate;
        CHECK(provenance_record(&pattern.bytes, record, &candidate));
    }
    ProvenanceRecord pattern_copy_provenance;
    CHECK(provenance_record(&pattern.bytes, pattern_copy_site, &pattern_copy_provenance)
        && pattern_copy_provenance.tag == 4 && pattern_copy_provenance.kind == 0
        && bytes_equal(pattern_copy_provenance.path, pattern_copy_provenance.path_count, "main.sol")
        && pattern_copy_provenance.start == (uint32_t)(binding - pattern_source + 5)
        && pattern_copy_provenance.end == (uint32_t)(binding - pattern_source + 13));
    /* This independently lists every dynamic request's fault provenance.  The
     * two adjacent 7s are the extraction's allocation/copy substeps. */
    static const int32_t pattern_request_sites[] = {6,5,7,7,7,8,8,9,9,10,10};
    represented_control("patterns.runtime-request-loop");
    for (size_t i = 0; i < sizeof pattern_request_sites / sizeof pattern_request_sites[0]; ++i) {
        SolWasmRepresentedLimits probe_limits = sol_wasm_represented_default_limits();
        SolWasmRepresentedOutput probe; WasmInstance instance = {0};
        int64_t value = 1; int32_t code = 0, site = 0;
        probe_limits.max_allocation_requests = i + 1; sol_wasm_represented_output_init(&probe);
        CHECK(build_multiroot(pattern_directory, false, &probe, pattern_ids, &probe_limits,
            SOL_WASM_REPRESENTED_OK) && wasm_instance_open(&probe.bytes, pattern_entry_name, &instance)
            && wasm_instance_observe(&instance, &value, &code, &site) && value == 0 && code == 5
            && site == pattern_request_sites[i]);
        wasm_instance_close(&instance); sol_wasm_represented_output_free(&probe);
    }
    SolWasmRepresentedLimits pattern_exact = sol_wasm_represented_default_limits();
    represented_control("patterns.same-instance-exact-shortfall-grow");
    pattern_exact.max_allocation_requests = 12; pattern_exact.max_allocation_bytes = 116;
    SolWasmRepresentedOutput pattern_capped; WasmInstance pattern_instance = {0};
    sol_wasm_represented_output_init(&pattern_capped);
    CHECK(build_multiroot(pattern_directory, false, &pattern_capped, pattern_ids, &pattern_exact,
        SOL_WASM_REPRESENTED_OK) && wasm_instance_open(&pattern_capped.bytes, pattern_entry_name,
            &pattern_instance) && wasm_instance_call(&pattern_instance, 42, 0, 0)
        && wasm_instance_call(&pattern_instance, 42, 0, 0));
    wasm_instance_close(&pattern_instance); sol_wasm_represented_output_free(&pattern_capped);
    /* Six requests/56 bytes admit the complete prefix before PATTERN_VALUE but
     * reject its first allocation at precisely its supplemental provenance. */
    for (size_t dimension = 0; dimension < 2; ++dimension) {
        SolWasmRepresentedLimits capped_limits = sol_wasm_represented_default_limits();
        SolWasmRepresentedOutput capped; WasmInstance observed = {0};
        int64_t value = 1; int32_t code = 0, site = 0;
        if (dimension == 0) capped_limits.max_allocation_requests = 6;
        else capped_limits.max_allocation_bytes = 56;
        sol_wasm_represented_output_init(&capped);
        CHECK(build_multiroot(pattern_directory, false, &capped, pattern_ids, &capped_limits,
            SOL_WASM_REPRESENTED_OK) && wasm_instance_open(&capped.bytes, pattern_entry_name, &observed)
            && wasm_instance_observe(&observed, &value, &code, &site)
            && value == 0 && code == 5 && site == (int32_t)pattern_copy_site);
        value = 1; code = 0; site = 0;
        CHECK(wasm_instance_observe(&observed, &value, &code, &site)
            && value == 0 && code == 5 && site == (int32_t)pattern_copy_site);
        wasm_instance_close(&observed); sol_wasm_represented_output_free(&capped);
    }
    sol_wasm_represented_test_allocator_memory(1, UINT32_C(65456));
    SolWasmRepresentedOutput pattern_grow; WasmInstance pattern_grow_instance = {0};
    int64_t pattern_grow_value = 1; int32_t pattern_grow_code = 0, pattern_grow_site = 0;
    sol_wasm_represented_output_init(&pattern_grow);
    CHECK(build_multiroot(pattern_directory, false, &pattern_grow, pattern_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && wasm_instance_open(&pattern_grow.bytes, pattern_entry_name,
            &pattern_grow_instance) && wasm_instance_observe(&pattern_grow_instance,
                &pattern_grow_value, &pattern_grow_code, &pattern_grow_site)
        && pattern_grow_value == 0 && pattern_grow_code == 4
        && pattern_grow_site == (int32_t)pattern_copy_site);
    wasm_instance_close(&pattern_grow_instance); sol_wasm_represented_output_free(&pattern_grow);
    sol_wasm_represented_test_allocator_memory(0, 0);
    SolWasmRepresentedOutput pattern_probe;
    represented_control("patterns.faults-caps-defaults-rootorder-relocation-gates");
    sol_wasm_represented_output_init(&pattern_probe);
    CHECK(build_multiroot(pattern_directory, false, &pattern_probe, pattern_ids, NULL,
        SOL_WASM_REPRESENTED_OK));
    sol_wasm_represented_output_free(&pattern_probe);
    size_t pattern_allocation_count = sol_wasm_represented_test_allocation_attempts();
    CHECK(pattern_allocation_count == 114);
    for (size_t attempt = 1; attempt <= pattern_allocation_count; ++attempt) {
        SolWasmRepresentedOutput failed, retry;
        sol_wasm_represented_output_init(&failed); represented_fault(FAULT_PATTERN, attempt, attempt);
        CHECK(build_multiroot(pattern_directory, false, &failed, pattern_ids, NULL,
            SOL_WASM_REPRESENTED_ALLOCATION_FAILED) && failed.bytes.bytes == NULL
            && failed.bytes.count == 0 && usage_zero(&failed.usage));
        sol_wasm_represented_output_free(&failed); sol_wasm_represented_test_fail_allocation_after(0);
        sol_wasm_represented_output_init(&retry);
        CHECK(build_multiroot(pattern_directory, false, &retry, pattern_ids, NULL,
            SOL_WASM_REPRESENTED_OK) && retry.bytes.count == pattern.bytes.count
            && memcmp(retry.bytes.bytes, pattern.bytes.bytes, pattern.bytes.count) == 0
            && memcmp(&retry.usage, &pattern.usage, sizeof retry.usage) == 0);
        sol_wasm_represented_output_free(&retry);
    }
    /* Every nonzero build census field is exact-or-one-below.  Runtime Text
     * caps are covered above and intentionally do not constrain emission. */
#define CHECK_PATTERN_BUILD_CAP(field, exact) do { \
    SolWasmRepresentedLimits cap_limits = sol_wasm_represented_default_limits(); \
    SolWasmRepresentedOutput capped; cap_limits.field = (exact); \
    sol_wasm_represented_output_init(&capped); \
    CHECK(build_multiroot(pattern_directory, false, &capped, pattern_ids, &cap_limits, \
        SOL_WASM_REPRESENTED_OK) && capped.bytes.count == pattern.bytes.count \
        && memcmp(capped.bytes.bytes, pattern.bytes.bytes, pattern.bytes.count) == 0 \
        && memcmp(&capped.usage, &pattern.usage, sizeof pattern.usage) == 0); \
    sol_wasm_represented_output_free(&capped); cap_limits.field = (exact) - 1; \
    sol_wasm_represented_output_init(&capped); \
    CHECK(build_multiroot(pattern_directory, false, &capped, pattern_ids, &cap_limits, \
        SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED) && capped.bytes.bytes == NULL \
        && capped.bytes.count == 0 && usage_zero(&capped.usage)); \
    sol_wasm_represented_output_free(&capped); \
} while (0)
    CHECK_PATTERN_BUILD_CAP(max_functions, 8u); CHECK_PATTERN_BUILD_CAP(max_blocks, 8u);
    CHECK_PATTERN_BUILD_CAP(max_edges, 7u); CHECK_PATTERN_BUILD_CAP(max_values, 15u);
    CHECK_PATTERN_BUILD_CAP(max_locals, 66u); CHECK_PATTERN_BUILD_CAP(max_generated_nodes, 1421u);
    CHECK_PATTERN_BUILD_CAP(max_static_data_bytes, 40u);
    CHECK_PATTERN_BUILD_CAP(max_provenance_records, 62u); CHECK_PATTERN_BUILD_CAP(max_work_bytes, 35580u);
    CHECK_PATTERN_BUILD_CAP(max_scratch_bytes, 13760u); CHECK_PATTERN_BUILD_CAP(max_owned_bytes, 21624u);
    CHECK_PATTERN_BUILD_CAP(max_output_bytes, 11384u);
#undef CHECK_PATTERN_BUILD_CAP
    SolWasmRepresentedLimits pattern_zero = {0}; SolWasmRepresentedOutput pattern_default;
    sol_wasm_represented_output_init(&pattern_default);
    CHECK(build_multiroot(pattern_directory, false, &pattern_default, pattern_ids, &pattern_zero,
        SOL_WASM_REPRESENTED_OK) && pattern_default.bytes.count == pattern.bytes.count
        && memcmp(pattern_default.bytes.bytes, pattern.bytes.bytes, pattern.bytes.count) == 0);
    sol_wasm_represented_output_free(&pattern_default);
#define CHECK_PATTERN_PARTIAL_ZERO(field) do { \
    SolWasmRepresentedLimits partial = sol_wasm_represented_default_limits(); \
    SolWasmRepresentedOutput rejected; partial.field = 0; sol_wasm_represented_output_init(&rejected); \
    CHECK(build_multiroot(pattern_directory, false, &rejected, pattern_ids, &partial, \
        SOL_WASM_REPRESENTED_INVALID_ARGUMENT) && rejected.bytes.bytes == NULL \
        && rejected.bytes.count == 0 && usage_zero(&rejected.usage)); \
    sol_wasm_represented_output_free(&rejected); \
} while (0)
    CHECK_PATTERN_PARTIAL_ZERO(max_functions); CHECK_PATTERN_PARTIAL_ZERO(max_blocks);
    CHECK_PATTERN_PARTIAL_ZERO(max_edges); CHECK_PATTERN_PARTIAL_ZERO(max_values);
    CHECK_PATTERN_PARTIAL_ZERO(max_locals); CHECK_PATTERN_PARTIAL_ZERO(max_generated_nodes);
    CHECK_PATTERN_PARTIAL_ZERO(max_table_elements); CHECK_PATTERN_PARTIAL_ZERO(max_static_data_bytes);
    CHECK_PATTERN_PARTIAL_ZERO(max_allocation_requests); CHECK_PATTERN_PARTIAL_ZERO(max_allocation_bytes);
    CHECK_PATTERN_PARTIAL_ZERO(max_provenance_records); CHECK_PATTERN_PARTIAL_ZERO(max_work_bytes);
    CHECK_PATTERN_PARTIAL_ZERO(max_scratch_bytes); CHECK_PATTERN_PARTIAL_ZERO(max_owned_bytes);
    CHECK_PATTERN_PARTIAL_ZERO(max_output_bytes);
#undef CHECK_PATTERN_PARTIAL_ZERO
    SolWasmRepresentedOutput pattern_reverse; size_t pattern_reverse_ids[2];
    sol_wasm_represented_output_init(&pattern_reverse);
    CHECK(build_multiroot(pattern_directory, true, &pattern_reverse, pattern_reverse_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && pattern_reverse.bytes.count == pattern.bytes.count
        && memcmp(pattern_reverse.bytes.bytes, pattern.bytes.bytes, pattern.bytes.count) == 0
        && memcmp(&pattern_reverse.usage, &pattern.usage, sizeof pattern.usage) == 0);
    sol_wasm_represented_output_free(&pattern_reverse);
    char pattern_relocated[512], pattern_source_path[768], pattern_destination[768];
    (void)snprintf(pattern_relocated, sizeof pattern_relocated, "%s/p43_pattern_relocated",
        SOL_TEST_BINARY_DIR);
    (void)mkdir(pattern_relocated, 0700);
    (void)snprintf(pattern_source_path, sizeof pattern_source_path, "%s/main.sol", pattern_directory);
    (void)snprintf(pattern_destination, sizeof pattern_destination, "%s/main.sol", pattern_relocated);
    FILE *pattern_input = fopen(pattern_source_path, "rb");
    FILE *pattern_output = fopen(pattern_destination, "wb");
    CHECK(pattern_input != NULL && pattern_output != NULL);
    if (pattern_input != NULL && pattern_output != NULL) {
        uint8_t copy_bytes[256]; size_t copy_count = 0;
        while ((copy_count = fread(copy_bytes, 1, sizeof copy_bytes, pattern_input)) != 0)
            CHECK(fwrite(copy_bytes, 1, copy_count, pattern_output) == copy_count);
    }
    if (pattern_input != NULL) fclose(pattern_input);
    if (pattern_output != NULL) fclose(pattern_output);
    SolWasmRepresentedOutput pattern_relocated_output; size_t pattern_relocated_ids[2];
    sol_wasm_represented_output_init(&pattern_relocated_output);
    CHECK(build_multiroot(pattern_relocated, false, &pattern_relocated_output, pattern_relocated_ids,
        NULL, SOL_WASM_REPRESENTED_OK) && pattern_relocated_output.bytes.count == pattern.bytes.count
        && memcmp(pattern_relocated_output.bytes.bytes, pattern.bytes.bytes, pattern.bytes.count) == 0
        && memcmp(&pattern_relocated_output.usage, &pattern.usage, sizeof pattern.usage) == 0);
    sol_wasm_represented_output_free(&pattern_relocated_output);
    sol_wasm_represented_output_free(&pattern);
    char pattern_total_directory[512], pattern_non_total_directory[512], pattern_guard_directory[512];
    (void)snprintf(pattern_total_directory, sizeof pattern_total_directory,
        "%s/tests/conformance/p43_pattern_total", SOL_TEST_SOURCE_DIR);
    (void)snprintf(pattern_non_total_directory, sizeof pattern_non_total_directory,
        "%s/tests/conformance/p43_pattern_non_total_reject", SOL_TEST_SOURCE_DIR);
    (void)snprintf(pattern_guard_directory, sizeof pattern_guard_directory,
        "%s/tests/conformance/p43_pattern_guard_reject", SOL_TEST_SOURCE_DIR);
    sol_wasm_represented_output_init(&pattern);
    CHECK(build_multiroot(pattern_total_directory, false, &pattern, pattern_ids, NULL,
        SOL_WASM_REPRESENTED_OK));
    if (provenance_record(&pattern.bytes, 1, &pattern_entry)
        && pattern_entry.symbol_count < sizeof pattern_entry_name) {
        memcpy(pattern_entry_name, pattern_entry.symbol, pattern_entry.symbol_count);
        pattern_entry_name[pattern_entry.symbol_count] = '\0';
        CHECK(invoke_named(&pattern.bytes, pattern_entry_name, 42, 0, 0));
    } else CHECK(false);
    sol_wasm_represented_output_free(&pattern);
    CHECK(frontend_rejects_non_total(pattern_non_total_directory));
    sol_wasm_represented_output_init(&pattern);
    CHECK(build_multiroot(pattern_guard_directory, false, &pattern, pattern_ids, NULL,
        SOL_WASM_REPRESENTED_OK));
    if (entry_symbol(&pattern.bytes, pattern_entry_name, sizeof pattern_entry_name))
        CHECK(invoke_named(&pattern.bytes, pattern_entry_name, 42, 0, 0));
    else CHECK(false);
    sol_wasm_represented_output_free(&pattern);
}

static void run_terminal_packets(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("terminal-packets.all-controls-mutations-quotas-sequences");
    /* P4.4a terminal packets and guarded decisions are source-backed. Every
     * packet site below is the exact canonical one-based provenance record. */
    const struct {
        const char *directory; int64_t value; int32_t code, site; const char *detail;
        size_t detail_count; bool packet;
    } p44_cases[] = {
        {"p44_panic", 0, 1, 3, "represented terminal panic", 26, true},
        {"p44_panic_empty", 0, 1, 3, "", 0, true},
        {"p44_panic_191", 0, 1, 3,
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 191, true},
        {"p44_panic_192", 0, 1, 3,
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 191, true},
        {"p44_unreachable", 0, 12, 3, NULL, 0, false},
        {"p44_guarded_match", 7, 0, 0, NULL, 0, false},
        {"p44_guarded_true", 42, 0, 0, NULL, 0, false},
        {"p44_require_true", 42, 0, 0, "", 0, true},
        {"p44_require_panic", 0, 1, 3, "require fallback", 16, true},
        {"p44_nested_panic", 0, 1, 4, "nested panic", 12, true},
        {"p44_require_unreachable", 0, 12, 3, NULL, 0, false},
    };
    for (size_t p44 = 0; p44 < sizeof p44_cases / sizeof *p44_cases; ++p44) {
        char p44_directory[512], p44_entry[256];
        SolWasmRepresentedOutput p44_output;
        (void)snprintf(p44_directory, sizeof p44_directory, "%s/tests/conformance/%s",
            SOL_TEST_SOURCE_DIR, p44_cases[p44].directory);
        sol_wasm_represented_output_init(&p44_output);
        CHECK(build_named_root(p44_directory, "launch", &p44_output, NULL,
            SOL_WASM_REPRESENTED_OK));
        if (entry_symbol(&p44_output.bytes, p44_entry, sizeof p44_entry)) {
            WasmInstance instance;
            CHECK(wasm_instance_open(&p44_output.bytes, p44_entry, &instance)
                && wasm_instance_call(&instance, p44_cases[p44].value, p44_cases[p44].code,
                    p44_cases[p44].site)
                && (p44_cases[p44].packet
                    ? wasm_instance_panic_detail(&instance, (const uint8_t *)p44_cases[p44].detail,
                        p44_cases[p44].detail_count)
                    : instance.panic_detail_offset == NULL && instance.panic_detail_length == NULL));
            wasm_instance_close(&instance);
        } else CHECK(false);
        sol_wasm_represented_output_free(&p44_output);
    }
    /* Exact P3.1-to-emitted-packet provenance. The guarded MATCH_FAILURE is
     * intentionally cold: the unguarded fallback makes the source exhaustive,
     * but code 11 remains emitted, source-owned, and mutation-tested. */
    CHECK(p44_terminal_owner_controls(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_panic",
        &(P44TerminalOwner){SOL_MIR_TERM_PANIC, SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_PANIC,
            0, 0, 0, 3, 0, 4, 5, 3, 95, 129, 1, 0, true}));
    CHECK(p44_terminal_owner_controls(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_nested_panic",
        &(P44TerminalOwner){SOL_MIR_TERM_PANIC, SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_PANIC,
            0, 0, 0, 3, 1, 4, 5, 4, 95, 115, 1, 1, true}));
    CHECK(p44_terminal_owner_controls(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_unreachable",
        &(P44TerminalOwner){SOL_MIR_TERM_UNREACHABLE,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_UNREACHABLE,
            0, 0, 0, 2, 0, 2, 2, 3, 100, 128, 2048, 0, true}));
    CHECK(p44_terminal_owner_controls(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_guarded_match",
        &(P44TerminalOwner){SOL_MIR_TERM_MATCH_FAILURE,
            SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_NO_MATCH,
            7, 0, 7, 20, 0, 24, 16, 3, 149, 245, 1024, 0, false}));
    CHECK(p44_same_instance_packet_reset(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_panic", 3,
        (const uint8_t *)"represented terminal panic", 26));
    CHECK(p44_panic_runtime_quota_controls(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_panic"));
    CHECK(p44_allocation_failure_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_panic"));
    CHECK(p44_packet_reset_probe_authorization(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_panic"));
    CHECK(p44_terminal_multiroot(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_nested_panic"));
    CHECK(p44_b1_control_owners(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_b1"));
}

static void run_cleanup_trace(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("cleanup-trace.all-controls-mutations-quotas-sequences");
    /* B2 is source-owned through P3.6: its owned formal, nested lexical scopes,
     * and explicit region yield the full normal-path cleanup order.  The first
     * action is the authenticated Text equality temporary; DROP_PARAMETER is
     * deliberately later, after place, scope, and region cleanup. */
    static const P44TraceSlot b2_normal_trace[] = {
        {13, 1, 0}, {14, 1, 0}, {31, 1, 0}, {32, 1, 0}, {33, 1, 0}, {34, 1, 0},
        {35, 1, 0}, {36, 1, 0}, {37, 1, 0}, {38, 1, 0}, {39, 1, 0}, {40, 1, 0},
        {47, 1, 0}, {48, 1, 0},
    };
    static const P44TraceSlot b2_fold_trace[] = {
        {13, 1, 0}, {14, 1, 0}, {31, 1, 0}, {32, 1, 0}, {33, 1, 0}, {34, 1, 0},
        {35, 1, 0}, {36, 1, 0}, {37, 1, 0}, {38, 1, 0}, {39, 1, 0}, {40, 1, 0},
    };
    static const P44TraceSlot b2_stress_prefix[] = {{49, 1, 0}};
    static const P44TraceSlot b2_stress_tail[] = {
        {110, 1, 0}, {111, 1, 0}, {112, 1, 0},
    };
    static const P44TraceSlot callable_hole_failure_trace[] = {
        {14, 13, 4}, {15, 1037, 4}, {16, 13, 4}, {17, 13, 4}, {18, 525, 4},
    };
    static const P44TraceSlot callback_success_trace[] = {
        {4, 1, 0}, {5, 1, 0}, {6, 1, 0}, {11, 257, 0}, {16, 1, 0}, {17, 1, 0},
        {18, 1, 0},
    };
    static const P44TraceSlot nested_panic_trace[] = {
        {3, 1, 0}, {4, 1, 0}, {5, 517, 4}, {6, 1, 0}, {7, 1, 0}, {8, 533, 4},
    };
    /* P4.4b-B2c matrix rows.  These IDs are not emitter observations: each
     * row is cross-checked above against the exact P3.6 event/transition/action
     * that owns it before its raw wire ledger is compared. */
    static const P44TraceSlot arithmetic_overflow_trace[] = {
        {0, 13, 3}, {1, 13, 3}, {2, 525, 3},
    };
    static const P44TraceSlot arithmetic_divzero_trace[] = {
        {0, 13, 3}, {1, 13, 3}, {2, 525, 3},
    };
    static const P44TraceSlot arithmetic_normal_trace[] = {{6, 1, 0}, {7, 1, 0}};
    static const P44TraceSlot option_value_trace[] = {
        {20, 1, 0}, {21, 1, 0}, {22, 1, 0}, {23, 1, 0}, {38, 1, 0}, {40, 1, 0},
        {41, 1, 0},
    };
    static const P44TraceSlot option_residual_trace[] = {
        {24, 1, 0}, {25, 1, 0}, {26, 1, 0}, {36, 1, 0}, {38, 1, 0}, {39, 1, 0},
    };
    static const P44TraceSlot result_value_trace[] = {
        {15, 1, 0}, {16, 1, 0}, {17, 1, 0}, {18, 1, 0}, {33, 1, 0}, {35, 1, 0},
        {36, 1, 0},
    };
    static const P44TraceSlot result_residual_trace[] = {
        {19, 1, 0}, {20, 1, 0}, {21, 1, 0}, {39, 1, 0}, {41, 1, 0}, {42, 1, 0},
    };
    static const P44TraceSlot method_success_trace[] = {
        {19, 1, 0}, {20, 1, 0}, {25, 1, 0}, {26, 1, 0}, {27, 1, 0}, {4, 257, 0},
        {15, 1, 0}, {16, 1, 0}, {17, 1, 0}, {18, 1, 0},
    };
    static const P44TraceSlot hole_full_trace[] = {{14, 1, 0}, {15, 1, 0}, {16, 1, 0}};
    static const P44TraceSlot hole_conditional_true_trace[] = {
        {17, 1, 0}, {18, 1, 0}, {21, 1025, 0}, {22, 1, 0}, {23, 1, 0}, {24, 1, 0},
        {28, 1, 0}, {29, 1, 0},
    };
    static const P44TraceSlot hole_conditional_false_trace[] = {
        {19, 1, 0}, {20, 1, 0}, {21, 1025, 0}, {22, 1, 0}, {23, 1, 0}, {24, 1, 0},
        {28, 1, 0}, {29, 1, 0},
    };
    static const P44TraceSlot hole_repair_trace[] = {
        {14, 1026, 0}, {15, 1, 0}, {16, 1, 0}, {17, 1, 0},
    };
    static const P44TraceSlot hole_reopen_trace[] = {
        {14, 1026, 0}, {15, 1, 0}, {16, 1025, 0}, {17, 1, 0}, {18, 1, 0},
    };
    static const P44TraceSlot b1_normal_trace[] = {
        {8, 1, 0}, {9, 1, 0}, {12, 1, 0}, {23, 1, 0}, {24, 1, 0}, {33, 1, 0},
        {34, 1, 0}, {35, 1, 0}, {33, 1, 0}, {36, 1, 0}, {39, 1, 0}, {48, 1, 0},
        {49, 1, 0}, {33, 1, 0}, {36, 1, 0}, {37, 1, 0}, {38, 1, 0}, {0, 1, 0},
        {1, 1, 0}, {2, 1, 0}, {3, 1, 0}, {4, 1, 0}, {5, 1, 0}, {6, 1, 0},
        {7, 1, 0}, {62, 1, 0}, {63, 1, 0}, {64, 1, 0}, {65, 1, 0}, {66, 1, 0},
    };
    static const P44TraceSlot unreachable_trace[] = {{0, 1, 0}, {1, 1, 0}, {2, 517, 3}};
    static const P44TraceSlot guarded_true_trace[] = {
        {4, 1, 0}, {5, 1, 0}, {6, 1, 0}, {9, 1, 0}, {10, 1, 0},
    };
    static const SolWasmRepresentedUsage b2_trace_usage = {6,7,5,15,56,10431,0,56,0,0,75,40443,16883,48425,38185};
    static const uint8_t b2_trace_hash[32] = {0x4b,0xf6,0x00,0x09,0x15,0xc3,0x95,0x2f,0x80,0xff,0x56,0xf3,0x4d,0xfd,0xdf,0x99,0x6b,0xad,0x4d,0xdc,0x0a,0xb3,0x9e,0x26,0x38,0xba,0x12,0x90,0xc2,0xcb,0x2f,0xfc};
    static const SolWasmRepresentedUsage trace_failure_usage = {7,2,0,9,57,4245,2,15,0,0,40,27911,10240,26950,16710};
    static const uint8_t trace_failure_hash[32] = {0x8b,0x51,0x1a,0x34,0x2f,0x36,0x33,0xd2,0xc0,0x5f,0x73,0xaf,0x69,0xf1,0x79,0xc0,0xb6,0xdd,0xcf,0xe9,0x03,0x71,0x08,0x09,0x81,0xef,0x97,0x32,0x6a,0x4c,0x3d,0x95};
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_b2_trace", 43, 0,
        0, b2_normal_trace, sizeof b2_normal_trace / sizeof *b2_normal_trace, true));
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p42_scalar_add_overflow", 0,
        2, 3, arithmetic_overflow_trace, sizeof arithmetic_overflow_trace
            / sizeof *arithmetic_overflow_trace, false));
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p42_scalar_divzero", 0, 3,
        3, arithmetic_divzero_trace, sizeof arithmetic_divzero_trace / sizeof *arithmetic_divzero_trace,
        false));
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p42_scalar_div_success", -4,
        0, 0, arithmetic_normal_trace, sizeof arithmetic_normal_trace / sizeof *arithmetic_normal_trace,
        false));
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p43_propagate_option_success",
        101, 0, 0, option_value_trace, sizeof option_value_trace / sizeof *option_value_trace, false));
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p43_propagate_option_residual",
        102, 0, 0, option_residual_trace, sizeof option_residual_trace / sizeof *option_residual_trace,
        false));
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p43_propagate_result_success",
        103, 0, 0, result_value_trace, sizeof result_value_trace / sizeof *result_value_trace, false));
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p43_propagate_result_residual",
        104, 0, 0, result_residual_trace, sizeof result_residual_trace / sizeof *result_residual_trace,
        false));
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p43_method_prereq", 83, 0,
        0, method_success_trace, sizeof method_success_trace / sizeof *method_success_trace, false));
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p43_callable_hole_full", 42, 0,
        0, hole_full_trace, sizeof hole_full_trace / sizeof *hole_full_trace, false));
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR
        "/tests/conformance/p43_callable_hole_c32_conditional_true", 42, 0, 0,
        hole_conditional_true_trace, sizeof hole_conditional_true_trace / sizeof *hole_conditional_true_trace,
        false));
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR
        "/tests/conformance/p43_callable_hole_c32_conditional_false", 42, 0, 0,
        hole_conditional_false_trace, sizeof hole_conditional_false_trace / sizeof *hole_conditional_false_trace,
        false));
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p43_callable_hole_c32_repair",
        42, 0, 0, hole_repair_trace, sizeof hole_repair_trace / sizeof *hole_repair_trace, false));
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p43_callable_hole_c32_reopen",
        42, 0, 0, hole_reopen_trace, sizeof hole_reopen_trace / sizeof *hole_reopen_trace, false));
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p43_callable_hole_c32_whole",
        42, 0, 0, hole_full_trace, sizeof hole_full_trace / sizeof *hole_full_trace, false));
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_b1", 43, 0, 0,
        b1_normal_trace, sizeof b1_normal_trace / sizeof *b1_normal_trace, false));
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_unreachable", 0, 12, 3,
        unreachable_trace, sizeof unreachable_trace / sizeof *unreachable_trace, false));
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_guarded_true", 42, 0, 0,
        guarded_true_trace, sizeof guarded_true_trace / sizeof *guarded_true_trace, false));
    CHECK(p44_trace_stress_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_b2_trace_stress",
        b2_stress_prefix, sizeof b2_stress_prefix / sizeof *b2_stress_prefix, b2_fold_trace,
        sizeof b2_fold_trace / sizeof *b2_fold_trace, b2_stress_tail,
        sizeof b2_stress_tail / sizeof *b2_stress_tail));
    CHECK(p44_trace_skipped_case(
        SOL_TEST_SOURCE_DIR "/tests/conformance/p43_callable_hole_c32_repair"));
    CHECK(p44_trace_eventless_case(
        SOL_TEST_SOURCE_DIR "/tests/conformance/p43_callable_hole_c32_whole"));
    CHECK(p44_trace_pending_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_b2_pending"));
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p43_callable_hole_failure",
        0, 2, 4, callable_hole_failure_trace, sizeof callable_hole_failure_trace
            / sizeof *callable_hole_failure_trace, false));
    CHECK(p44_trace_freeze_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_b2_trace",
        &b2_trace_usage, b2_trace_hash));
    CHECK(p44_trace_freeze_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p43_callable_hole_failure",
        &trace_failure_usage, trace_failure_hash));
    CHECK(p44_trace_marker_owner_mutations(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_b2_trace"));
    CHECK(p44_trace_writeback_owner_mutations(
        SOL_TEST_SOURCE_DIR "/tests/conformance/p43_callback_inout"));
    CHECK(p44_method_failure_trace_case(
        SOL_TEST_SOURCE_DIR "/tests/conformance/p43_method_prereq"));
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p43_callback_inout", 42,
        0, 0, callback_success_trace, sizeof callback_success_trace / sizeof *callback_success_trace,
        false));
    /* The nested panic retains its original one-based provenance record through
     * the callee and caller cleanup sequence. */
    CHECK(p44_cleanup_trace_case(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_nested_panic", 0,
        1, 4, nested_panic_trace, sizeof nested_panic_trace / sizeof *nested_panic_trace, false));
    CHECK(p44_trace_wire_controls(SOL_TEST_SOURCE_DIR "/tests/conformance/p44_panic"));
    {
        char p44_guard_call_directory[512];
        SolWasmRepresentedOutput p44_guard_call;
        (void)snprintf(p44_guard_call_directory, sizeof p44_guard_call_directory,
            "%s/tests/conformance/p44_guard_call_reject", SOL_TEST_SOURCE_DIR);
        sol_wasm_represented_output_init(&p44_guard_call);
        CHECK(build_named_root(p44_guard_call_directory, "launch", &p44_guard_call, NULL,
            SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE));
        sol_wasm_represented_output_free(&p44_guard_call);
    }
}

static void run_panic_wire_resource(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("panic-wire-resource.all-controls-mutations-quotas-sequences");
    /* P4.4a's packet exports are private envelope fields: with panic provenance
     * either exact paired names exist or raw validation rejects the module. */
    {
        char p44_panic_directory[512];
        SolWasmRepresentedOutput p44_panic_wire;
        (void)snprintf(p44_panic_directory, sizeof p44_panic_directory,
            "%s/tests/conformance/p44_panic", SOL_TEST_SOURCE_DIR);
        sol_wasm_represented_output_init(&p44_panic_wire);
        CHECK(build_named_root(p44_panic_directory, "launch", &p44_panic_wire, NULL,
            SOL_WASM_REPRESENTED_OK));
        static const SolWasmRepresentedUsage expected_p44_panic_usage = {
            .functions = 6, .blocks = 1, .edges = 0, .values = 1, .locals = 35,
            .generated_nodes = 623, .table_elements = 0, .static_data_bytes = 34,
            .allocation_requests = 0, .allocation_bytes = 0, .provenance_records = 10,
            .work_bytes = 14198, .scratch_bytes = 10240, .owned_bytes = 13196,
            .output_bytes = 2956,
        };
        static const uint8_t expected_p44_panic_hash[32] = {0x8b,0x93,0x93,0xc1,0x78,0x22,0x5f,0xab,0xa2,0x81,0x36,0x59,0x21,0x86,0xa6,0x72,0x6c,0x81,0x9f,0x13,0x48,0x7c,0x84,0x40,0xf4,0xd3,0x4f,0xaf,0xde,0x21,0x77,0x42};
        uint8_t p44_panic_hash[32];
        sha256(p44_panic_wire.bytes.bytes, p44_panic_wire.bytes.count, p44_panic_hash);
        CHECK(usage_equal(&p44_panic_wire.usage, &expected_p44_panic_usage)
            && memcmp(p44_panic_hash, expected_p44_panic_hash, sizeof p44_panic_hash) == 0
            && sol_wasm_represented_test_allocation_attempts() == 27);
#define CHECK_P44_LIMIT(field, exact) do { \
    SolWasmRepresentedLimits p44_limit = sol_wasm_represented_default_limits(); \
    SolWasmRepresentedOutput p44_limited; sol_wasm_represented_output_init(&p44_limited); \
    p44_limit.field = (exact); \
    CHECK(build_named_root(p44_panic_directory, "launch", &p44_limited, &p44_limit, \
        SOL_WASM_REPRESENTED_OK) && usage_equal(&p44_limited.usage, &expected_p44_panic_usage)); \
    sol_wasm_represented_output_free(&p44_limited); sol_wasm_represented_output_init(&p44_limited); \
    p44_limit.field = (exact) - 1; \
    CHECK(build_named_root(p44_panic_directory, "launch", &p44_limited, &p44_limit, \
        (exact) == 1 ? SOL_WASM_REPRESENTED_INVALID_ARGUMENT : SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED) \
        && p44_limited.bytes.bytes == NULL && p44_limited.bytes.count == 0 && usage_zero(&p44_limited.usage)); \
    sol_wasm_represented_output_free(&p44_limited); \
} while (0)
        CHECK_P44_LIMIT(max_functions, expected_p44_panic_usage.functions);
        CHECK_P44_LIMIT(max_blocks, expected_p44_panic_usage.blocks);
        CHECK_P44_LIMIT(max_values, expected_p44_panic_usage.values);
        CHECK_P44_LIMIT(max_locals, expected_p44_panic_usage.locals);
        CHECK_P44_LIMIT(max_generated_nodes, expected_p44_panic_usage.generated_nodes);
        CHECK_P44_LIMIT(max_static_data_bytes, expected_p44_panic_usage.static_data_bytes);
        CHECK_P44_LIMIT(max_provenance_records, expected_p44_panic_usage.provenance_records);
        CHECK_P44_LIMIT(max_work_bytes, expected_p44_panic_usage.work_bytes);
        CHECK_P44_LIMIT(max_scratch_bytes, expected_p44_panic_usage.scratch_bytes);
        CHECK_P44_LIMIT(max_owned_bytes, expected_p44_panic_usage.owned_bytes);
        CHECK_P44_LIMIT(max_output_bytes, expected_p44_panic_usage.output_bytes);
#undef CHECK_P44_LIMIT
        /* The panic packet capture itself has zero allocator demand; its source
         * Text construction has the exact public demand of two requests/34
         * bytes. Zero remains the API's invalid partial-limit form. */
        { SolWasmRepresentedLimits p44_quota = sol_wasm_represented_default_limits();
          SolWasmRepresentedOutput p44_limited; char p44_entry[256];
          sol_wasm_represented_output_init(&p44_limited);
          p44_quota.max_allocation_requests = 2; p44_quota.max_allocation_bytes = 34;
          CHECK(build_named_root(p44_panic_directory, "launch", &p44_limited, &p44_quota,
              SOL_WASM_REPRESENTED_OK) && entry_symbol(&p44_limited.bytes, p44_entry,
                  sizeof p44_entry) && invoke_named(&p44_limited.bytes, p44_entry, 0, 1, 3));
          sol_wasm_represented_output_free(&p44_limited);
          p44_quota.max_allocation_requests = 0;
          CHECK(build_named_root(p44_panic_directory, "launch", &p44_limited, &p44_quota,
              SOL_WASM_REPRESENTED_INVALID_ARGUMENT) && p44_limited.bytes.bytes == NULL
              && usage_zero(&p44_limited.usage));
          sol_wasm_represented_output_free(&p44_limited);
          p44_quota = sol_wasm_represented_default_limits();
          p44_quota.max_allocation_bytes = 0;
          CHECK(build_named_root(p44_panic_directory, "launch", &p44_limited, &p44_quota,
              SOL_WASM_REPRESENTED_INVALID_ARGUMENT) && p44_limited.bytes.bytes == NULL
              && usage_zero(&p44_limited.usage));
          sol_wasm_represented_output_free(&p44_limited); }
        for (size_t ordinal = 0; ordinal < 27; ++ordinal) {
            SolWasmRepresentedOutput failed;
            sol_wasm_represented_output_init(&failed);
            represented_fault(FAULT_PANIC, ordinal + 1, ordinal + 1);
            CHECK(build_named_root(p44_panic_directory, "launch", &failed, NULL,
                SOL_WASM_REPRESENTED_ALLOCATION_FAILED) && failed.bytes.bytes == NULL
                && failed.bytes.count == 0 && usage_zero(&failed.usage));
            sol_wasm_represented_test_fail_allocation_after(0);
            sol_wasm_represented_output_free(&failed);
            /* Retry before moving to the next ordinal: no failed build may
             * leave accounting, bytes, or allocator-attempt state behind. */
            SolWasmRepresentedOutput retry; uint8_t retry_hash[32];
            sol_wasm_represented_output_init(&retry);
            CHECK(build_named_root(p44_panic_directory, "launch", &retry, NULL,
                SOL_WASM_REPRESENTED_OK));
            sha256(retry.bytes.bytes, retry.bytes.count, retry_hash);
            CHECK(usage_equal(&retry.usage, &expected_p44_panic_usage)
                && memcmp(retry_hash, expected_p44_panic_hash, sizeof retry_hash) == 0
                && sol_wasm_represented_test_allocation_attempts() == 27);
            sol_wasm_represented_output_free(&retry);
        }
        { SolWasmRepresentedOutput retry; uint8_t retry_hash[32];
          sol_wasm_represented_output_init(&retry);
          CHECK(build_named_root(p44_panic_directory, "launch", &retry, NULL,
              SOL_WASM_REPRESENTED_OK));
          sha256(retry.bytes.bytes, retry.bytes.count, retry_hash);
          CHECK(usage_equal(&retry.usage, &expected_p44_panic_usage)
              && memcmp(retry_hash, expected_p44_panic_hash, sizeof retry_hash) == 0
              && sol_wasm_represented_test_allocation_attempts() == 27);
          sol_wasm_represented_output_free(&retry); }
        const char *packet_exports[] = {SOL_WASM_REPRESENTED_PANIC_DETAIL_OFFSET_EXPORT,
            SOL_WASM_REPRESENTED_PANIC_DETAIL_LENGTH_EXPORT};
        for (size_t field = 0; field < sizeof packet_exports / sizeof *packet_exports; ++field) {
            const size_t name_count = strlen(packet_exports[field]);
            size_t at = 0;
            while (at + name_count <= p44_panic_wire.bytes.count
                && memcmp(p44_panic_wire.bytes.bytes + at, packet_exports[field], name_count) != 0) ++at;
            CHECK(at + name_count <= p44_panic_wire.bytes.count);
            if (at + name_count <= p44_panic_wire.bytes.count) {
                uint8_t *mutated = malloc(p44_panic_wire.bytes.count);
                CHECK(mutated != NULL);
                if (mutated != NULL) {
                    memcpy(mutated, p44_panic_wire.bytes.bytes, p44_panic_wire.bytes.count);
                    mutated[at] = 'x';
                    CHECK(sol_wasm_represented_validate(&(SolWasmBackendBytes){mutated,
                        p44_panic_wire.bytes.count}) == SOL_WASM_REPRESENTED_INVALID_INPUT);
                    free(mutated);
                }
            }
        }
        P44WireLayout wire;
        CHECK(p44_wire_layout(&p44_panic_wire.bytes, &wire) && wire.offset_global != wire.length_global
            && wire.offset_global < wire.count && wire.length_global < wire.count && wire.count > 2);
        /* A page-edge heap is valid only when it still contains the whole
         * reserved packet; a raised effective heap is rejected before module
         * creation, so no self-invalidating output can escape. */
        { SolWasmRepresentedOutput p44_heap; sol_wasm_represented_output_init(&p44_heap);
          sol_wasm_represented_test_allocator_memory(1, UINT32_C(65536));
          CHECK(build_named_root(p44_panic_directory, "launch", &p44_heap, NULL,
              SOL_WASM_REPRESENTED_OK) && sol_wasm_represented_validate(&p44_heap.bytes)
              == SOL_WASM_REPRESENTED_OK);
          sol_wasm_represented_output_free(&p44_heap);
          sol_wasm_represented_test_allocator_memory(1, UINT32_C(65537));
          CHECK(build_named_root(p44_panic_directory, "launch", &p44_heap, NULL,
              SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED) && p44_heap.bytes.bytes == NULL
              && usage_zero(&p44_heap.usage));
          sol_wasm_represented_test_allocator_memory(0, 0);
          sol_wasm_represented_output_free(&p44_heap); }
#define CHECK_P44_WIRE_MUTATION(edit) do { \
    uint8_t *mutated = malloc(p44_panic_wire.bytes.count); \
    CHECK(mutated != NULL); \
    if (mutated != NULL) { \
        memcpy(mutated, p44_panic_wire.bytes.bytes, p44_panic_wire.bytes.count); edit; \
        CHECK(sol_wasm_represented_validate(&(SolWasmBackendBytes){mutated, \
            p44_panic_wire.bytes.count}) == SOL_WASM_REPRESENTED_INVALID_INPUT); \
        free(mutated); \
    } \
} while (0)
        if (wire.offset_global < wire.count && wire.length_global < wire.count && wire.count > 2) {
            CHECK_P44_WIRE_MUTATION(mutated[wire.type[wire.offset_global]] = UINT8_C(0x7e));
            CHECK_P44_WIRE_MUTATION(mutated[wire.mutability[wire.offset_global]] = 1);
            CHECK_P44_WIRE_MUTATION(write_uleb_same_width(mutated + wire.initial[wire.offset_global],
                wire.initial_width[wire.offset_global], 0));
            CHECK_P44_WIRE_MUTATION(mutated[wire.type[wire.length_global]] = UINT8_C(0x7e));
            CHECK_P44_WIRE_MUTATION(mutated[wire.mutability[wire.length_global]] = 0);
            CHECK_P44_WIRE_MUTATION(write_uleb_same_width(mutated + wire.initial[wire.length_global],
                wire.initial_width[wire.length_global], 1));
            CHECK_P44_WIRE_MUTATION(write_uleb_same_width(mutated + wire.offset_export_index, 1,
                wire.length_global));
            CHECK_P44_WIRE_MUTATION(write_uleb_same_width(mutated + wire.initial[2],
                wire.initial_width[2], wire.initial_value[wire.offset_global] + 191));
        }
#undef CHECK_P44_WIRE_MUTATION
        sol_wasm_represented_output_free(&p44_panic_wire);
    }
}

static void run_scalar_products_late(SolWasmRepresentedOutput *forward,
    SolWasmRepresentedOutput *reverse) {
    represented_control("scalar-products.late-shape-controls-teardown");
    /* This is a source-valid, authenticated P3.6 closure: the only rejected
     * operation is a whole scalar-product Copy.  It must not reach the raw
     * handle load emitter or publish output. */
    char product_copy_directory[512];
    char unsupported_shapes_directory[512];
    (void)snprintf(product_copy_directory, sizeof product_copy_directory,
        "%s/tests/conformance/p43_scalar_product_copy_reject", SOL_TEST_SOURCE_DIR);
    SolWasmRepresentedOutput product_copy_rejected;
    size_t product_copy_ids[2];
    sol_wasm_represented_output_init(&product_copy_rejected);
    CHECK(build_multiroot(product_copy_directory, false, &product_copy_rejected, product_copy_ids,
        NULL, SOL_WASM_REPRESENTED_OK));
    CHECK(product_copy_rejected.bytes.bytes != NULL && product_copy_rejected.bytes.count != 0);
    sol_wasm_represented_output_free(&product_copy_rejected);
    sol_wasm_represented_output_free(&product_copy_rejected);
    (void)snprintf(unsupported_shapes_directory, sizeof unsupported_shapes_directory,
        "%s/tests/conformance/p43_scalar_product_equality_reject", SOL_TEST_SOURCE_DIR);
    sol_wasm_represented_output_init(&product_copy_rejected);
    CHECK(build_multiroot(unsupported_shapes_directory, false, &product_copy_rejected,
        product_copy_ids, NULL, SOL_WASM_REPRESENTED_OK));
    CHECK(product_copy_rejected.bytes.bytes != NULL && product_copy_rejected.bytes.count != 0);
    sol_wasm_represented_output_free(&product_copy_rejected);
    sol_wasm_represented_output_free(&product_copy_rejected);
    (void)snprintf(unsupported_shapes_directory, sizeof unsupported_shapes_directory,
        "%s/tests/conformance/p43_scalar_product_wrapper_reject", SOL_TEST_SOURCE_DIR);
    sol_wasm_represented_output_init(&product_copy_rejected);
    CHECK(build_multiroot(unsupported_shapes_directory, false, &product_copy_rejected,
        product_copy_ids, NULL, SOL_WASM_REPRESENTED_OK));
    CHECK(product_copy_rejected.bytes.bytes != NULL && product_copy_rejected.bytes.count != 0);
    sol_wasm_represented_output_free(&product_copy_rejected);
    (void)snprintf(unsupported_shapes_directory, sizeof unsupported_shapes_directory,
        "%s/tests/conformance/p43_scalar_product_sum_pattern_reject", SOL_TEST_SOURCE_DIR);
    sol_wasm_represented_output_init(&product_copy_rejected);
    CHECK(build_multiroot(unsupported_shapes_directory, false, &product_copy_rejected,
        product_copy_ids, NULL, SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE));
    CHECK(product_copy_rejected.bytes.bytes == NULL && product_copy_rejected.bytes.count == 0
        && usage_zero(&product_copy_rejected.usage));
    sol_wasm_represented_output_free(&product_copy_rejected);
    /* Nested and Text-field construction are part of the A2 product-tree
     * closure even when their roots are later dropped. */
    (void)snprintf(unsupported_shapes_directory, sizeof unsupported_shapes_directory,
        "%s/tests/conformance/p43_scalar_product_unsupported_shapes", SOL_TEST_SOURCE_DIR);
    sol_wasm_represented_output_init(&product_copy_rejected);
    CHECK(build_multiroot(unsupported_shapes_directory, false, &product_copy_rejected,
        product_copy_ids, NULL, SOL_WASM_REPRESENTED_OK));
    CHECK(product_copy_rejected.bytes.bytes != NULL && product_copy_rejected.bytes.count != 0);
    sol_wasm_represented_output_free(&product_copy_rejected);
    (void)snprintf(unsupported_shapes_directory, sizeof unsupported_shapes_directory,
        "%s/tests/conformance/p43_scalar_product_text_reject", SOL_TEST_SOURCE_DIR);
    sol_wasm_represented_output_init(&product_copy_rejected);
    CHECK(build_multiroot(unsupported_shapes_directory, false, &product_copy_rejected,
        product_copy_ids, NULL, SOL_WASM_REPRESENTED_OK));
    CHECK(product_copy_rejected.bytes.bytes != NULL && product_copy_rejected.bytes.count != 0);
    sol_wasm_represented_output_free(&product_copy_rejected);
    sol_wasm_represented_output_free(reverse);
    sol_wasm_represented_output_free(forward);
}

static void run_product_tree(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("product-tree.all-controls-mutations-quotas-sequences");
    /* A2's closed product tree: nested P2 offsets, Text transfer in both
     * constructors, whole/deep copies, structural ==/!=, and scope drops all
     * execute before the scalar result is returned. */
    char tree_directory[512];
    (void)snprintf(tree_directory, sizeof tree_directory,
        "%s/tests/conformance/p43_product_tree", SOL_TEST_SOURCE_DIR);
    SolWasmRepresentedOutput tree;
    size_t tree_ids[2];
    sol_wasm_represented_output_init(&tree);
    CHECK(build_multiroot(tree_directory, false, &tree, tree_ids, NULL,
        SOL_WASM_REPRESENTED_OK));
    ProvenanceRecord tree_entry;
    char tree_entry_name[256] = {0};
    CHECK(provenance_record(&tree.bytes, 1, &tree_entry)
        && tree_entry.symbol_count < sizeof tree_entry_name);
    if (provenance_record(&tree.bytes, 1, &tree_entry)
        && tree_entry.symbol_count < sizeof tree_entry_name) {
        memcpy(tree_entry_name, tree_entry.symbol, tree_entry.symbol_count);
        tree_entry_name[tree_entry.symbol_count] = '\0';
        CHECK(invoke_named(&tree.bytes, tree_entry_name, 42, 0, 0));
    }
    static const uint8_t expected_tree_hash[32] = {0x5c,0x0f,0x26,0x72,0x7a,0x24,0x07,0x58,0x23,0x01,0xc3,0xfa,0x22,0x05,0x20,0xbb,0x8f,0x80,0x74,0xf3,0xf7,0xe0,0x52,0x99,0x75,0x9d,0xbc,0x9b,0x26,0x83,0x95,0x01};
    uint8_t tree_hash[32]; sha256(tree.bytes.bytes, tree.bytes.count, tree_hash);
    CHECK(memcmp(tree_hash, expected_tree_hash, sizeof tree_hash) == 0);
    CHECK(usage_equal(&tree.usage, &(SolWasmRepresentedUsage){11,13,16,38,123,2860,0,49,0,0,106,56625,24097,30332,20092}));
    CHECK(sol_wasm_represented_validate(&tree.bytes) == SOL_WASM_REPRESENTED_OK);
    ProvenanceRecord tree_copy_site;
    CHECK(provenance_record(&tree.bytes, 8, &tree_copy_site)
        && tree_copy_site.tag == 4 && tree_copy_site.kind == 0
        && bytes_equal(tree_copy_site.path, tree_copy_site.path_count, "main.sol")
        && tree_copy_site.start == 306 && tree_copy_site.end == 311);
    for (uint32_t record = 1; record <= tree.usage.provenance_records; ++record) {
        ProvenanceRecord candidate;
        CHECK(provenance_record(&tree.bytes, record, &candidate));
    }
    /* Find the exact dynamic product-tree demand independently in each
     * dimension, then prove the immediately lower cap faults rather than
     * publishing a result.  Every wrapper resets the allocator. */
    uint64_t tree_requests = 0, tree_bytes = 0;
    represented_control("product-tree.runtime-quota-search");
    uint64_t low = 1, high = 128;
    while (low <= high) {
        uint64_t cap = low + (high - low) / 2;
        SolWasmRepresentedLimits tree_limits = sol_wasm_represented_default_limits();
        tree_limits.max_allocation_requests = cap;
        SolWasmRepresentedOutput capped; sol_wasm_represented_output_init(&capped);
        bool succeeds = build_multiroot(tree_directory, false, &capped, tree_ids, &tree_limits,
            SOL_WASM_REPRESENTED_OK) && invoke_named(&capped.bytes, tree_entry_name, 42, 0, 0);
        sol_wasm_represented_output_free(&capped);
        if (succeeds) { tree_requests = cap; if (cap == 0) break; high = cap - 1; }
        else low = cap + 1;
    }
    low = 1; high = 1024;
    while (low <= high) {
        uint64_t cap = low + (high - low) / 2;
        SolWasmRepresentedLimits tree_limits = sol_wasm_represented_default_limits();
        tree_limits.max_allocation_bytes = cap;
        SolWasmRepresentedOutput capped; sol_wasm_represented_output_init(&capped);
        bool succeeds = build_multiroot(tree_directory, false, &capped, tree_ids, &tree_limits,
            SOL_WASM_REPRESENTED_OK) && invoke_named(&capped.bytes, tree_entry_name, 42, 0, 0);
        sol_wasm_represented_output_free(&capped);
        if (succeeds) { tree_bytes = cap; if (cap == 0) break; high = cap - 1; }
        else low = cap + 1;
    }
    CHECK(tree_requests == 33 && tree_bytes == 313);
    if (tree_requests > 1) {
        SolWasmRepresentedLimits tree_limits = sol_wasm_represented_default_limits();
        tree_limits.max_allocation_requests = tree_requests - 1;
        SolWasmRepresentedOutput capped; sol_wasm_represented_output_init(&capped);
        CHECK(build_multiroot(tree_directory, false, &capped, tree_ids, &tree_limits,
            SOL_WASM_REPRESENTED_OK) && invoke_named(&capped.bytes, tree_entry_name, 0, 5, -1));
        sol_wasm_represented_output_free(&capped);
    }
    if (tree_bytes > 1) {
        SolWasmRepresentedLimits tree_limits = sol_wasm_represented_default_limits();
        tree_limits.max_allocation_bytes = tree_bytes - 1;
        SolWasmRepresentedOutput capped; sol_wasm_represented_output_init(&capped);
        CHECK(build_multiroot(tree_directory, false, &capped, tree_ids, &tree_limits,
            SOL_WASM_REPRESENTED_OK) && invoke_named(&capped.bytes, tree_entry_name, 0, 5, -1));
        sol_wasm_represented_output_free(&capped);
    }
    /* The independently enumerated prefix is five requests/48 bytes before
     * `outer` is copied.  Request 8 (or byte 95) has published the private
     * outer/Inner/empty-child staging only; the later `tree` Text child then
     * faults at the caller's LOAD_COPY supplemental record, never a child
     * constructor record. */
    const uint64_t later_request_cap = 8, later_byte_cap = 95;
    represented_control("product-tree.same-instance-late-copy-shortfall-success");
    for (size_t dimension = 0; dimension != 2; ++dimension) {
        SolWasmRepresentedLimits tree_limits = sol_wasm_represented_default_limits();
        SolWasmRepresentedOutput capped; WasmInstance observed = {0};
        int64_t value = 1; int32_t code = 0, site = 0;
        if (dimension == 0) tree_limits.max_allocation_requests = later_request_cap;
        else tree_limits.max_allocation_bytes = later_byte_cap;
        sol_wasm_represented_output_init(&capped);
        CHECK(build_multiroot(tree_directory, false, &capped, tree_ids, &tree_limits,
            SOL_WASM_REPRESENTED_OK) && wasm_instance_open(&capped.bytes, tree_entry_name, &observed)
            && wasm_instance_observe(&observed, &value, &code, &site)
            && value == 0 && code == 5 && site == 8);
        /* The entry wrapper resets all private state: a second invocation has
         * the same caller-site fault rather than inheriting a published root. */
        value = 1; code = 0; site = 0;
        CHECK(wasm_instance_observe(&observed, &value, &code, &site)
            && value == 0 && code == 5 && site == 8);
        wasm_instance_close(&observed); sol_wasm_represented_output_free(&capped);
    }
    WasmInstance successful_tree;
    CHECK(wasm_instance_open(&tree.bytes, tree_entry_name, &successful_tree));
    if (successful_tree.instance != NULL) {
        CHECK(wasm_instance_call(&successful_tree, 42, 0, 0));
        CHECK(wasm_instance_call(&successful_tree, 42, 0, 0));
    }
    wasm_instance_close(&successful_tree);
    /* A one-page heap can fail the final nested Text growth after several
     * preceding product/text allocations; this remains the caller's site. */
    sol_wasm_represented_test_allocator_memory(1, UINT32_C(65432));
    represented_control("product-tree.grow-caps-faults-rootorder-relocation");
    SolWasmRepresentedOutput tree_grow; WasmInstance grown = {0};
    int64_t grow_value = 1; int32_t grow_code = 0, grow_site = 0;
    sol_wasm_represented_output_init(&tree_grow);
    CHECK(build_multiroot(tree_directory, false, &tree_grow, tree_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && wasm_instance_open(&tree_grow.bytes, tree_entry_name, &grown)
        && wasm_instance_observe(&grown, &grow_value, &grow_code, &grow_site)
        && grow_value == 0 && grow_code == 4 && grow_site == 8);
    wasm_instance_close(&grown); sol_wasm_represented_output_free(&tree_grow);
    sol_wasm_represented_test_allocator_memory(0, 0);
#define CHECK_TREE_BUILD_CAP(field, exact) do { \
    SolWasmRepresentedLimits cap_limits = sol_wasm_represented_default_limits(); \
    SolWasmRepresentedOutput capped; sol_wasm_represented_output_init(&capped); \
    cap_limits.field = (exact); \
    CHECK(build_multiroot(tree_directory, false, &capped, tree_ids, &cap_limits, \
        SOL_WASM_REPRESENTED_OK) && capped.bytes.count == tree.bytes.count \
        && memcmp(capped.bytes.bytes, tree.bytes.bytes, tree.bytes.count) == 0 \
        && memcmp(&capped.usage, &tree.usage, sizeof tree.usage) == 0); \
    sol_wasm_represented_output_free(&capped); cap_limits.field = (exact) - 1; \
    sol_wasm_represented_output_init(&capped); \
    CHECK(build_multiroot(tree_directory, false, &capped, tree_ids, &cap_limits, \
        SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED) && capped.bytes.bytes == NULL \
        && capped.bytes.count == 0 && usage_zero(&capped.usage)); \
    sol_wasm_represented_output_free(&capped); \
} while (0)
    CHECK_TREE_BUILD_CAP(max_functions, 11u);
    CHECK_TREE_BUILD_CAP(max_blocks, 13u);
    CHECK_TREE_BUILD_CAP(max_edges, 16u);
    CHECK_TREE_BUILD_CAP(max_values, 38u);
    CHECK_TREE_BUILD_CAP(max_locals, 123u);
    CHECK_TREE_BUILD_CAP(max_generated_nodes, 2860u);
    CHECK_TREE_BUILD_CAP(max_static_data_bytes, 49u);
    CHECK_TREE_BUILD_CAP(max_provenance_records, 106u);
    CHECK_TREE_BUILD_CAP(max_work_bytes, 56625u);
    CHECK_TREE_BUILD_CAP(max_scratch_bytes, 24097u);
    CHECK_TREE_BUILD_CAP(max_owned_bytes, 30332u);
    CHECK_TREE_BUILD_CAP(max_output_bytes, 20092u);
#undef CHECK_TREE_BUILD_CAP
    /* A wholly zero limits record is the default selector; each individual
     * zero remains invalid rather than silently widening a tree budget. */
    SolWasmRepresentedOutput tree_default;
    SolWasmRepresentedLimits tree_zero = {0};
    sol_wasm_represented_output_init(&tree_default);
    CHECK(build_multiroot(tree_directory, false, &tree_default, tree_ids, &tree_zero,
        SOL_WASM_REPRESENTED_OK) && tree_default.bytes.count == tree.bytes.count
        && memcmp(tree_default.bytes.bytes, tree.bytes.bytes, tree.bytes.count) == 0);
    sol_wasm_represented_output_free(&tree_default);
#define CHECK_TREE_PARTIAL_ZERO(field) do { \
    SolWasmRepresentedLimits partial = sol_wasm_represented_default_limits(); \
    SolWasmRepresentedOutput rejected; partial.field = 0; \
    sol_wasm_represented_output_init(&rejected); \
    CHECK(build_multiroot(tree_directory, false, &rejected, tree_ids, &partial, \
        SOL_WASM_REPRESENTED_INVALID_ARGUMENT) && rejected.bytes.bytes == NULL \
        && rejected.bytes.count == 0 && usage_zero(&rejected.usage)); \
    sol_wasm_represented_output_free(&rejected); \
} while (0)
    CHECK_TREE_PARTIAL_ZERO(max_functions); CHECK_TREE_PARTIAL_ZERO(max_blocks);
    CHECK_TREE_PARTIAL_ZERO(max_edges); CHECK_TREE_PARTIAL_ZERO(max_values);
    CHECK_TREE_PARTIAL_ZERO(max_locals); CHECK_TREE_PARTIAL_ZERO(max_generated_nodes);
    CHECK_TREE_PARTIAL_ZERO(max_table_elements); CHECK_TREE_PARTIAL_ZERO(max_static_data_bytes);
    CHECK_TREE_PARTIAL_ZERO(max_allocation_requests); CHECK_TREE_PARTIAL_ZERO(max_allocation_bytes);
    CHECK_TREE_PARTIAL_ZERO(max_provenance_records); CHECK_TREE_PARTIAL_ZERO(max_work_bytes);
    CHECK_TREE_PARTIAL_ZERO(max_scratch_bytes); CHECK_TREE_PARTIAL_ZERO(max_owned_bytes);
    CHECK_TREE_PARTIAL_ZERO(max_output_bytes);
#undef CHECK_TREE_PARTIAL_ZERO
    SolWasmRepresentedOutput tree_allocation_probe;
    sol_wasm_represented_output_init(&tree_allocation_probe);
    CHECK(build_multiroot(tree_directory, false, &tree_allocation_probe, tree_ids, NULL,
        SOL_WASM_REPRESENTED_OK));
    sol_wasm_represented_output_free(&tree_allocation_probe);
    size_t tree_allocation_count = sol_wasm_represented_test_allocation_attempts();
    CHECK(tree_allocation_count == 208);
    for (size_t attempt = 1; attempt <= tree_allocation_count; ++attempt) {
        SolWasmRepresentedOutput failed, retry;
        sol_wasm_represented_output_init(&failed); represented_fault(FAULT_PRODUCT_TREE, attempt, attempt);
        CHECK(build_multiroot(tree_directory, false, &failed, tree_ids, NULL,
            SOL_WASM_REPRESENTED_ALLOCATION_FAILED) && failed.bytes.bytes == NULL
            && failed.bytes.count == 0 && usage_zero(&failed.usage));
        sol_wasm_represented_output_free(&failed); sol_wasm_represented_test_fail_allocation_after(0);
        sol_wasm_represented_output_init(&retry);
        CHECK(build_multiroot(tree_directory, false, &retry, tree_ids, NULL,
            SOL_WASM_REPRESENTED_OK) && retry.bytes.count == tree.bytes.count
            && memcmp(retry.bytes.bytes, tree.bytes.bytes, tree.bytes.count) == 0
            && memcmp(&retry.usage, &tree.usage, sizeof tree.usage) == 0);
        sol_wasm_represented_output_free(&retry);
    }
    SolWasmRepresentedOutput tree_reverse;
    size_t tree_reverse_ids[2];
    sol_wasm_represented_output_init(&tree_reverse);
    CHECK(build_multiroot(tree_directory, true, &tree_reverse, tree_reverse_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && tree_reverse.bytes.count == tree.bytes.count
        && memcmp(tree_reverse.bytes.bytes, tree.bytes.bytes, tree.bytes.count) == 0
        && memcmp(&tree_reverse.usage, &tree.usage, sizeof tree.usage) == 0);
    sol_wasm_represented_output_free(&tree_reverse);
    char tree_relocated[512], tree_source[768], tree_destination[768];
    (void)snprintf(tree_relocated, sizeof tree_relocated, "%s/p43_tree_relocated", SOL_TEST_BINARY_DIR);
    (void)mkdir(tree_relocated, 0700);
    (void)snprintf(tree_source, sizeof tree_source, "%s/main.sol", tree_directory);
    (void)snprintf(tree_destination, sizeof tree_destination, "%s/main.sol", tree_relocated);
    FILE *tree_input = fopen(tree_source, "rb"), *tree_output = fopen(tree_destination, "wb");
    CHECK(tree_input != NULL && tree_output != NULL);
    if (tree_input != NULL && tree_output != NULL) {
        uint8_t bytes[256]; size_t count = 0;
        while ((count = fread(bytes, 1, sizeof bytes, tree_input)) != 0)
            CHECK(fwrite(bytes, 1, count, tree_output) == count);
    }
    if (tree_input != NULL) fclose(tree_input);
    if (tree_output != NULL) fclose(tree_output);
    SolWasmRepresentedOutput tree_relocated_output;
    size_t tree_relocated_ids[2];
    sol_wasm_represented_output_init(&tree_relocated_output);
    CHECK(build_multiroot(tree_relocated, false, &tree_relocated_output, tree_relocated_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && tree_relocated_output.bytes.count == tree.bytes.count
        && memcmp(tree_relocated_output.bytes.bytes, tree.bytes.bytes, tree.bytes.count) == 0
        && memcmp(&tree_relocated_output.usage, &tree.usage, sizeof tree.usage) == 0);
    sol_wasm_represented_output_free(&tree_relocated_output);
    sol_wasm_represented_output_free(&tree);
}

static void run_sums(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("sums.all-controls-mutations-quotas-sequences");
    /* Slice A3: finite Option/Result/nominal sums plus transparent distinct
     * wrappers.  The entry itself remains zero-parameter and scalar-result;
     * every aggregate is constructed, copied, compared, and logically dropped
     * before its distinctive result is published. */
    char sums_directory[512];
    (void)snprintf(sums_directory, sizeof sums_directory,
        "%s/tests/conformance/p43_sums_wrappers", SOL_TEST_SOURCE_DIR);
    SolWasmRepresentedOutput sums;
    size_t sums_ids[2];
    sol_wasm_represented_output_init(&sums);
    CHECK(build_multiroot(sums_directory, false, &sums, sums_ids, NULL,
        SOL_WASM_REPRESENTED_OK));
    uint8_t sums_hash[32]; sha256(sums.bytes.bytes, sums.bytes.count, sums_hash);
    static const uint8_t expected_sums_hash[32] = {0x0e,0x5c,0xfb,0x59,0xb4,0x43,0x7e,0xe6,0x55,0xa8,0x06,0x2f,0x8b,0xf5,0x26,0x40,0xf3,0x3f,0x48,0xae,0xcd,0xf6,0xab,0xa9,0x27,0xf8,0x9f,0x69,0x06,0x39,0xb4,0x9b};
    CHECK(memcmp(sums_hash, expected_sums_hash, sizeof sums_hash) == 0);
    CHECK(usage_equal(&sums.usage, &(SolWasmRepresentedUsage){21,23,31,79,259,10756,0,19,0,0,237,149223,52831,66790,56550}));
    ProvenanceRecord sums_entry;
    char sums_entry_name[256] = {0};
    CHECK(provenance_record(&sums.bytes, 1, &sums_entry)
        && sums_entry.symbol_count < sizeof sums_entry_name);
    if (provenance_record(&sums.bytes, 1, &sums_entry)
        && sums_entry.symbol_count < sizeof sums_entry_name) {
        memcpy(sums_entry_name, sums_entry.symbol, sums_entry.symbol_count);
        sums_entry_name[sums_entry.symbol_count] = '\0';
        CHECK(invoke_named(&sums.bytes, sums_entry_name, 42, 0, 0));
    }
    SolWasmRepresentedOutput sums_reverse;
    size_t sums_reverse_ids[2];
    sol_wasm_represented_output_init(&sums_reverse);
    CHECK(build_multiroot(sums_directory, true, &sums_reverse, sums_reverse_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && sums_reverse.bytes.count == sums.bytes.count
        && memcmp(sums_reverse.bytes.bytes, sums.bytes.bytes, sums.bytes.count) == 0
        && memcmp(&sums_reverse.usage, &sums.usage, sizeof sums.usage) == 0);
    sol_wasm_represented_output_free(&sums_reverse);
    /* The full construction/copy path is 62 logical requests and 732 logical
     * bytes.  The immediately lower cap reaches its final wrapper-aggregate
     * copy site rather than publishing the scalar result. */
    SolWasmRepresentedLimits sums_limits = sol_wasm_represented_default_limits();
    represented_control("sums.runtime-exact-one-below");
    SolWasmRepresentedOutput sums_capped; sol_wasm_represented_output_init(&sums_capped);
    sums_limits.max_allocation_requests = 62; sums_limits.max_allocation_bytes = 732;
    CHECK(build_multiroot(sums_directory, false, &sums_capped, sums_ids, &sums_limits,
        SOL_WASM_REPRESENTED_OK) && invoke_named(&sums_capped.bytes, sums_entry_name, 42, 0, 0));
    sol_wasm_represented_output_free(&sums_capped);
    sums_limits.max_allocation_requests = 61;
    sol_wasm_represented_output_init(&sums_capped);
    CHECK(build_multiroot(sums_directory, false, &sums_capped, sums_ids, &sums_limits,
        SOL_WASM_REPRESENTED_OK));
    WasmInstance sums_failed = {0}; int64_t sums_value = 1; int32_t sums_code = 0, sums_site = 0;
    CHECK(wasm_instance_open(&sums_capped.bytes, sums_entry_name, &sums_failed)
        && wasm_instance_observe(&sums_failed, &sums_value, &sums_code, &sums_site));
    CHECK(sums_value == 0 && sums_code == 5 && sums_site == 45);
    wasm_instance_close(&sums_failed); sol_wasm_represented_output_free(&sums_capped);
    sums_limits.max_allocation_requests = 62; sums_limits.max_allocation_bytes = 731;
    sol_wasm_represented_output_init(&sums_capped);
    CHECK(build_multiroot(sums_directory, false, &sums_capped, sums_ids, &sums_limits,
        SOL_WASM_REPRESENTED_OK) && invoke_named(&sums_capped.bytes, sums_entry_name, 0, 5, 45));
    sol_wasm_represented_output_free(&sums_capped);
    sol_wasm_represented_output_free(&sums);
}

static void run_propagation(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("propagation.all-controls-mutations-quotas-sequences");
    ProvenanceRecord sums_entry;
    char sums_entry_name[256] = {0};
    /* Slice B2: propagation carries the live success payload without an
     * allocation and allocates only the residual destination sum. */
    char propagate_directory[512];
    (void)snprintf(propagate_directory, sizeof propagate_directory,
        "%s/tests/conformance/p43_propagate", SOL_TEST_SOURCE_DIR);
    SolWasmRepresentedOutput propagate; size_t propagate_ids[2];
    sol_wasm_represented_output_init(&propagate);
    CHECK(build_multiroot(propagate_directory, false, &propagate, propagate_ids, NULL,
        SOL_WASM_REPRESENTED_OK));
    if (propagate.bytes.count != 0 && provenance_record(&propagate.bytes, 1, &sums_entry)
        && sums_entry.symbol_count < sizeof sums_entry_name) {
        memcpy(sums_entry_name, sums_entry.symbol, sums_entry.symbol_count);
        sums_entry_name[sums_entry.symbol_count] = '\0';
        CHECK(invoke_named(&propagate.bytes, sums_entry_name, 73, 0, 0));
    } else CHECK(false);
    SolWasmRepresentedOutput propagate_reverse; size_t propagate_reverse_ids[2];
    sol_wasm_represented_output_init(&propagate_reverse);
    CHECK(build_multiroot(propagate_directory, true, &propagate_reverse, propagate_reverse_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && propagate_reverse.bytes.count == propagate.bytes.count
        && memcmp(propagate_reverse.bytes.bytes, propagate.bytes.bytes, propagate.bytes.count) == 0
        && memcmp(&propagate_reverse.usage, &propagate.usage, sizeof propagate.usage) == 0);
    sol_wasm_represented_output_free(&propagate_reverse);
    sol_wasm_represented_output_free(&propagate);
    static const struct { const char *fixture; int64_t code; } propagation_entries[] = {
        {"p43_propagate_option_success", 101}, {"p43_propagate_option_residual", 102},
        {"p43_propagate_result_success", 103}, {"p43_propagate_result_residual", 104},
        {"p43_propagate_unit_success", 105},
    };
    static const SolWasmRepresentedUsage propagation_usage[] = {
        {9,9,8,21,84,1467,0,0,0,0,63,35481,13829,21828,11588},
        {9,9,8,19,78,1411,0,0,0,0,59,34105,13013,21184,10944},
        {9,9,8,21,84,1488,0,0,0,0,62,35149,13625,21706,11466},
        {9,9,8,21,84,1536,0,26,0,0,64,36927,14123,22092,11852},
        {9,9,8,19,82,1375,0,0,0,0,60,34469,13217,21230,10990},
    };
    static const uint8_t propagation_hashes[][32] = {
        {0x03,0xdf,0x4e,0xe7,0xa9,0x27,0x41,0x81,0x90,0x62,0x79,0x5f,0x00,0x5f,0xbd,0x0e,0x7b,0xc8,0xa0,0x99,0x56,0x60,0x98,0x95,0x9d,0x27,0x42,0xbe,0x0e,0x85,0x24,0xd6},
        {0xd7,0x89,0xff,0x09,0xa9,0x94,0xd2,0x1c,0xbf,0xed,0x11,0xed,0xb8,0x34,0xb0,0xb4,0x9d,0x73,0x20,0xda,0x19,0xe4,0xc7,0xf1,0xb1,0xb2,0x15,0xf6,0xc4,0x2c,0x8f,0x47},
        {0x9b,0xdc,0x35,0x94,0x24,0x15,0x0c,0x29,0xbf,0x6c,0xbf,0xc8,0x15,0x42,0x3e,0x25,0x0e,0xc8,0x8d,0x84,0x53,0x41,0x15,0xfb,0x27,0x1b,0xae,0x37,0xa4,0x49,0xd9,0xdb},
        {0x28,0x1b,0xe8,0x42,0xbc,0x09,0x94,0x14,0x47,0x04,0x97,0x4f,0xe0,0xf5,0xa8,0xad,0xe0,0x60,0xd5,0x46,0x14,0x90,0xca,0x82,0x6b,0x71,0x45,0x50,0xfa,0xd0,0xc2,0x82},
        {0x07,0x1e,0xf2,0x32,0xab,0xfd,0x47,0x0f,0xc9,0xf3,0x72,0x8d,0x6c,0x24,0xb5,0xb1,0x47,0x1a,0x11,0xf9,0x40,0x3a,0x42,0xce,0x71,0xc5,0x62,0xd4,0x9e,0xe6,0x31,0xc0},
    };
    static const uint64_t propagation_requests[] = {4, 4, 4, 10, 4};
    static const uint64_t propagation_bytes[] = {64, 64, 48, 87, 16};
    /* One-below runtime caps have a canonical, exported failure site in each
     * fixture.  Keep request and byte maps distinct even where they coincide. */
    static const int32_t propagation_request_shortfall_sites[] = {10, 10, 9, 10, 9};
    static const int32_t propagation_byte_shortfall_sites[] = {10, 10, 9, 10, 9};
    static const char option_residual_callable[] =
        "sol.i1.64d68cdb7133fadb679baa737cd44580.2adc89e898bd8a9c226ff3bd3b5d26b86f206740ab1198c4838f5d3cace7080e";
    for (size_t i = 0; i < sizeof propagation_entries / sizeof propagation_entries[0]; ++i) {
        represented_control("propagation.entry-census-caps-runtime-loops-same-instance");
        char isolated_directory[512]; SolWasmRepresentedOutput isolated; ProvenanceRecord entry_record;
        char entry_name[256] = {0};
        (void)snprintf(isolated_directory, sizeof isolated_directory, "%s/tests/conformance/%s",
            SOL_TEST_SOURCE_DIR, propagation_entries[i].fixture);
        sol_wasm_represented_output_init(&isolated);
        CHECK(build_named_root(isolated_directory, "launch", &isolated, NULL,
            SOL_WASM_REPRESENTED_OK) && provenance_record(&isolated.bytes, 1, &entry_record)
            && entry_record.symbol_count < sizeof entry_name);
        if (provenance_record(&isolated.bytes, 1, &entry_record)
            && entry_record.symbol_count < sizeof entry_name) {
            memcpy(entry_name, entry_record.symbol, entry_record.symbol_count);
            entry_name[entry_record.symbol_count] = '\0';
            CHECK(invoke_named(&isolated.bytes, entry_name, propagation_entries[i].code, 0, 0));
        }
        uint8_t isolated_hash[32]; sha256(isolated.bytes.bytes, isolated.bytes.count, isolated_hash);
        if (isolated.usage.output_bytes != propagation_usage[i].output_bytes
            || memcmp(isolated_hash, propagation_hashes[i], sizeof isolated_hash) != 0)
        { fprintf(stderr, "propagation freeze mismatch %zu out=%zu expected=%zu hash=", i,
                isolated.usage.output_bytes, propagation_usage[i].output_bytes);
           for (size_t byte = 0; byte < sizeof isolated_hash; ++byte) fprintf(stderr, "%02x", isolated_hash[byte]);
          fputc('\n', stderr); }
        CHECK(usage_equal(&isolated.usage, &propagation_usage[i])
            && memcmp(isolated_hash, propagation_hashes[i], sizeof isolated_hash) == 0);
        if (i == 3) {
            /* Result residual is the only literal propagation fixture with an
             * active static Text image: 26 succeeds and 25 is rejected. */
            CHECK(isolated.usage.static_data_bytes == 26);
            SolWasmRepresentedLimits static_cap = sol_wasm_represented_default_limits();
            SolWasmRepresentedOutput static_exact, static_below;
            static_cap.max_static_data_bytes = 26;
            sol_wasm_represented_output_init(&static_exact);
            CHECK(build_named_root(isolated_directory, "launch", &static_exact, &static_cap,
                SOL_WASM_REPRESENTED_OK) && static_exact.bytes.count == isolated.bytes.count
                && memcmp(static_exact.bytes.bytes, isolated.bytes.bytes, isolated.bytes.count) == 0
                && usage_equal(&static_exact.usage, &isolated.usage));
            sol_wasm_represented_output_free(&static_exact);
            static_cap.max_static_data_bytes = 25;
            sol_wasm_represented_output_init(&static_below);
            CHECK(build_named_root(isolated_directory, "launch", &static_below, &static_cap,
                SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED) && static_below.bytes.bytes == NULL
                && static_below.bytes.count == 0 && usage_zero(&static_below.usage));
            sol_wasm_represented_output_free(&static_below);
        }
        CHECK(verify_propagation_build_controls(isolated_directory, &isolated, i == 3));
        for (size_t dimension = 0; dimension != 2; ++dimension) {
            SolWasmRepresentedLimits runtime = sol_wasm_represented_default_limits();
            SolWasmRepresentedOutput exact, shortfall;
            uint64_t demand = dimension == 0 ? propagation_requests[i] : propagation_bytes[i];
            if (dimension == 0) runtime.max_allocation_requests = demand;
            else runtime.max_allocation_bytes = demand;
            sol_wasm_represented_output_init(&exact);
            CHECK(build_named_root(isolated_directory, "launch", &exact, &runtime,
                SOL_WASM_REPRESENTED_OK) && invoke_named(&exact.bytes, entry_name,
                propagation_entries[i].code, 0, 0));
            sol_wasm_represented_output_free(&exact);
            if (dimension == 0) runtime.max_allocation_requests = demand - 1;
            else runtime.max_allocation_bytes = demand - 1;
            sol_wasm_represented_output_init(&shortfall);
            int32_t expected_site = dimension == 0
                ? propagation_request_shortfall_sites[i] : propagation_byte_shortfall_sites[i];
            CHECK(build_named_root(isolated_directory, "launch", &shortfall, &runtime,
                SOL_WASM_REPRESENTED_OK) && invoke_named(&shortfall.bytes, entry_name, 0, 5,
                    expected_site));
            sol_wasm_represented_output_free(&shortfall);
        }
        if (i == 1) {
            /* `value?` is the canonical supplemental provenance shared by the
             * source allocation and propagation prerequisite.  One source
             * nullary Option fits; allocating the destination must fault
             * before either propagation result edge can publish. */
            ProvenanceRecord canonical;
            CHECK(provenance_record(&isolated.bytes, 6, &canonical) && canonical.tag == 4
                && canonical.kind == 0 && bytes_equal(canonical.path, canonical.path_count, "main.sol")
                && canonical.start == 140 && canonical.end == 146
                && bytes_equal(canonical.symbol, canonical.symbol_count, option_residual_callable)
                && canonical.ordinal == 0);
            for (size_t dimension = 0; dimension != 2; ++dimension) {
                SolWasmRepresentedLimits runtime = sol_wasm_represented_default_limits();
                SolWasmRepresentedOutput capped;
                if (dimension == 0) runtime.max_allocation_requests = 1;
                else runtime.max_allocation_bytes = 16;
                sol_wasm_represented_output_init(&capped);
                CHECK(build_named_root(isolated_directory, "launch", &capped, &runtime,
                    SOL_WASM_REPRESENTED_OK) && invoke_named(&capped.bytes, entry_name, 0, 5, 6));
                sol_wasm_represented_output_free(&capped);
            }
            SolWasmRepresentedLimits exact = sol_wasm_represented_default_limits();
            SolWasmRepresentedOutput reset; WasmInstance instance = {0};
            exact.max_allocation_requests = 4; exact.max_allocation_bytes = 64;
            sol_wasm_represented_output_init(&reset);
            CHECK(build_named_root(isolated_directory, "launch", &reset, &exact,
                SOL_WASM_REPRESENTED_OK) && wasm_instance_open(&reset.bytes, entry_name, &instance)
                && wasm_instance_call(&instance, 102, 0, 0) && wasm_instance_call(&instance, 102, 0, 0));
            wasm_instance_close(&instance); sol_wasm_represented_output_free(&reset);
            sol_wasm_represented_test_allocator_memory(1, UINT32_C(65520));
            sol_wasm_represented_output_init(&reset);
            CHECK(build_named_root(isolated_directory, "launch", &reset, NULL,
                SOL_WASM_REPRESENTED_OK) && invoke_named(&reset.bytes, entry_name, 0, 4, 6));
            sol_wasm_represented_output_free(&reset);
            sol_wasm_represented_test_allocator_memory(0, 0);
        }
        sol_wasm_represented_output_free(&isolated);
    }
    char propagation_owner_directory[512];
    represented_control("propagation.owner-relocation");
    (void)snprintf(propagation_owner_directory, sizeof propagation_owner_directory,
        "%s/tests/conformance/p43_propagate_result_residual", SOL_TEST_SOURCE_DIR);
    CHECK(verify_propagation_owner(propagation_owner_directory));
    char propagation_relocated[512], propagation_source[768], propagation_destination[768];
    (void)mkdir(SOL_TEST_BINARY_DIR, 0700);
    (void)snprintf(propagation_relocated, sizeof propagation_relocated,
        "%s/p43_propagation_relocated", SOL_TEST_BINARY_DIR);
    (void)mkdir(propagation_relocated, 0700);
    (void)snprintf(propagation_source, sizeof propagation_source,
        "%s/tests/conformance/p43_propagate_option_success/main.sol", SOL_TEST_SOURCE_DIR);
    (void)snprintf(propagation_destination, sizeof propagation_destination, "%s/main.sol",
        propagation_relocated);
    FILE *propagation_input = fopen(propagation_source, "rb");
    FILE *propagation_output = fopen(propagation_destination, "wb");
    CHECK(propagation_input != NULL && propagation_output != NULL);
    if (propagation_input != NULL && propagation_output != NULL) {
        uint8_t copied[256]; size_t copied_count = 0;
        while ((copied_count = fread(copied, 1, sizeof copied, propagation_input)) != 0)
            CHECK(fwrite(copied, 1, copied_count, propagation_output) == copied_count);
    }
    if (propagation_input != NULL) fclose(propagation_input);
    if (propagation_output != NULL) fclose(propagation_output);
    SolWasmRepresentedOutput propagation_original, propagation_relocated_output;
    sol_wasm_represented_output_init(&propagation_original);
    sol_wasm_represented_output_init(&propagation_relocated_output);
    CHECK(build_named_root("" SOL_TEST_SOURCE_DIR "/tests/conformance/p43_propagate_option_success",
        "launch", &propagation_original, NULL, SOL_WASM_REPRESENTED_OK)
        && build_named_root(propagation_relocated, "launch", &propagation_relocated_output, NULL,
            SOL_WASM_REPRESENTED_OK)
        && propagation_relocated_output.bytes.count == propagation_original.bytes.count
        && memcmp(propagation_relocated_output.bytes.bytes, propagation_original.bytes.bytes,
            propagation_original.bytes.count) == 0
        && memcmp(&propagation_relocated_output.usage, &propagation_original.usage,
            sizeof propagation_original.usage) == 0);
    sol_wasm_represented_output_free(&propagation_relocated_output);
    sol_wasm_represented_output_free(&propagation_original);
}

static void run_unit_text_sums(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("unit-text-sums.all-controls-mutations-quotas-sequences");
    char unit_directory[512], sum_text_directory[512];
    (void)snprintf(unit_directory, sizeof unit_directory,
        "%s/tests/conformance/p43_sum_unit", SOL_TEST_SOURCE_DIR);
    (void)snprintf(sum_text_directory, sizeof sum_text_directory,
        "%s/tests/conformance/p43_sum_text", SOL_TEST_SOURCE_DIR);
    SolWasmRepresentedOutput unit_sum, sum_text;
    size_t unit_ids[2], sum_text_ids[2];
    sol_wasm_represented_output_init(&unit_sum);
    sol_wasm_represented_output_init(&sum_text);
    CHECK(verify_unit_sum_pipeline(unit_directory));
    CHECK(build_multiroot(unit_directory, false, &unit_sum, unit_ids, NULL,
        SOL_WASM_REPRESENTED_OK));
    CHECK(build_multiroot(sum_text_directory, false, &sum_text, sum_text_ids, NULL,
        SOL_WASM_REPRESENTED_OK));
    ProvenanceRecord unit_entry, sum_text_entry;
    char unit_entry_name[256] = {0}, sum_text_entry_name[256] = {0};
    CHECK(provenance_record(&unit_sum.bytes, 1, &unit_entry)
        && unit_entry.symbol_count < sizeof unit_entry_name);
    CHECK(provenance_record(&sum_text.bytes, 1, &sum_text_entry)
        && sum_text_entry.symbol_count < sizeof sum_text_entry_name);
    if (provenance_record(&unit_sum.bytes, 1, &unit_entry)
        && unit_entry.symbol_count < sizeof unit_entry_name) {
        memcpy(unit_entry_name, unit_entry.symbol, unit_entry.symbol_count);
        unit_entry_name[unit_entry.symbol_count] = '\0';
        CHECK(invoke_named(&unit_sum.bytes, unit_entry_name, 42, 0, 0));
    }
    if (provenance_record(&sum_text.bytes, 1, &sum_text_entry)
        && sum_text_entry.symbol_count < sizeof sum_text_entry_name) {
        memcpy(sum_text_entry_name, sum_text_entry.symbol, sum_text_entry.symbol_count);
        sum_text_entry_name[sum_text_entry.symbol_count] = '\0';
        CHECK(invoke_named(&sum_text.bytes, sum_text_entry_name, 42, 0, 0));
    }
    uint8_t sum_text_hash[32]; sha256(sum_text.bytes.bytes, sum_text.bytes.count, sum_text_hash);
    static const uint8_t expected_sum_text_hash[32] = {0x22,0x1b,0x78,0x31,0x5b,0x30,0x2d,0x28,0xa4,0xe8,0x55,0xd5,0xe2,0x76,0x6d,0xc1,0xd6,0x7e,0x6c,0x00,0x4e,0xc5,0xbb,0x04,0xb8,0x57,0x96,0xaa,0xb4,0x31,0x4b,0x32};
    CHECK(memcmp(sum_text_hash, expected_sum_text_hash, sizeof sum_text_hash) == 0);
    CHECK(usage_equal(&sum_text.usage, &(SolWasmRepresentedUsage){9,13,16,35,103,3090,0,37,0,0,105,58417,23713,30637,20397}));
    CHECK(sol_wasm_represented_validate(&sum_text.bytes) == SOL_WASM_REPRESENTED_OK);
    ProvenanceLayout sum_text_layout;
    CHECK(provenance_layout(&sum_text.bytes, &sum_text_layout)
        && sum_text_layout.count == sum_text.usage.provenance_records);
    for (uint32_t record = 1; record <= sum_text_layout.count; ++record) {
        ProvenanceRecord candidate;
        CHECK(provenance_record(&sum_text.bytes, record, &candidate));
    }
    ProvenanceRecord sum_text_late_site;
    CHECK(provenance_record(&sum_text.bytes, 22, &sum_text_late_site)
        && sum_text_late_site.tag == 4 && sum_text_late_site.kind == 0
        && bytes_equal(sum_text_late_site.path, sum_text_late_site.path_count, "main.sol"));
    /* Binaryen does not preserve private helper names while reading a serialized
     * module, so this uses the narrowly-scoped test-only build hook instead of
     * mutating a clone.  It is disabled by default, adds no production export,
     * and constructs/poisons the two objects before the generated helper. */
    SolWasmRepresentedOutput poisoned_sum; size_t poisoned_ids[2];
    represented_control("unit-text-sums.inactive-active-payload");
    sol_wasm_represented_output_init(&poisoned_sum);
    sol_wasm_represented_test_inactive_payload_probe(true);
    CHECK(build_multiroot(sum_text_directory, false, &poisoned_sum, poisoned_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && wasmtime_module_valid(&poisoned_sum.bytes));
    sol_wasm_represented_test_inactive_payload_probe(false);
    if (poisoned_sum.bytes.bytes != NULL) {
        WasmInstance blank_probe, active_probe;
        CHECK(wasm_instance_open(&poisoned_sum.bytes, "sol.p43.test.inactive-payload", &blank_probe)
            && wasm_instance_call(&blank_probe, 1, 0, 0));
        if (blank_probe.instance != NULL) {
            uint64_t left_poison = 0, right_poison = 0;
            const uint8_t *memory = (const uint8_t *)wasm_memory_data(blank_probe.memory);
            size_t memory_size = wasm_memory_data_size(blank_probe.memory);
            CHECK(memory != NULL && memory_size >= 64032);
            if (memory != NULL && memory_size >= 64032) {
                memcpy(&left_poison, memory + 64008, sizeof left_poison);
                memcpy(&right_poison, memory + 64024, sizeof right_poison);
                CHECK(left_poison == UINT64_C(0x1122334455667788)
                    && right_poison == UINT64_C(0x0223344556677889));
            }
        }
        wasm_instance_close(&blank_probe);
        CHECK(wasm_instance_open(&poisoned_sum.bytes, "sol.p43.test.active-payload", &active_probe)
            && wasm_instance_call(&active_probe, 0, 0, 0));
        wasm_instance_close(&active_probe);
    }
    sol_wasm_represented_output_free(&poisoned_sum);
    /* Runtime Text demand is independent of the build census.  Find each
     * exact dimension, then freeze its lower-bound failure below. */
    uint64_t sum_text_requests = 0, sum_text_bytes = 0;
    represented_control("unit-text-sums.runtime-quota-loops-shortfalls-grow");
    for (uint64_t cap = 1; cap <= 128 && sum_text_requests == 0; ++cap) {
        SolWasmRepresentedLimits capped_limits = sol_wasm_represented_default_limits();
        SolWasmRepresentedOutput capped; capped_limits.max_allocation_requests = cap;
        sol_wasm_represented_output_init(&capped);
        if (build_multiroot(sum_text_directory, false, &capped, sum_text_ids, &capped_limits,
                SOL_WASM_REPRESENTED_OK) && invoke_named(&capped.bytes, sum_text_entry_name, 42, 0, 0))
            sum_text_requests = cap;
        sol_wasm_represented_output_free(&capped);
    }
    for (uint64_t cap = 1; cap <= 1024 && sum_text_bytes == 0; ++cap) {
        SolWasmRepresentedLimits capped_limits = sol_wasm_represented_default_limits();
        SolWasmRepresentedOutput capped; capped_limits.max_allocation_bytes = cap;
        sol_wasm_represented_output_init(&capped);
        if (build_multiroot(sum_text_directory, false, &capped, sum_text_ids, &capped_limits,
                SOL_WASM_REPRESENTED_OK) && invoke_named(&capped.bytes, sum_text_entry_name, 42, 0, 0))
            sum_text_bytes = cap;
        sol_wasm_represented_output_free(&capped);
    }
    CHECK(sum_text_requests == 40 && sum_text_bytes == 402);
    for (size_t dimension = 0; dimension < 2; ++dimension) {
        SolWasmRepresentedLimits capped_limits = sol_wasm_represented_default_limits();
        SolWasmRepresentedOutput capped; WasmInstance observed = {0};
        int64_t value = 1; int32_t code = 0, site = 0;
        if (dimension == 0) capped_limits.max_allocation_requests = sum_text_requests - 1;
        else capped_limits.max_allocation_bytes = sum_text_bytes - 1;
        sol_wasm_represented_output_init(&capped);
        CHECK(build_multiroot(sum_text_directory, false, &capped, sum_text_ids, &capped_limits,
            SOL_WASM_REPRESENTED_OK) && wasm_instance_open(&capped.bytes, sum_text_entry_name, &observed)
            && wasm_instance_observe(&observed, &value, &code, &site)
            && value == 0 && code == 5 && site == 22);
        wasm_instance_close(&observed); sol_wasm_represented_output_free(&capped);
    }
    SolWasmRepresentedOutput sum_text_grown; WasmInstance grown_instance = {0};
    int64_t grown_value = 1; int32_t grown_code = 0, grown_site = 0;
    sol_wasm_represented_test_allocator_memory(1, UINT32_C(65104));
    sol_wasm_represented_output_init(&sum_text_grown);
    CHECK(build_multiroot(sum_text_directory, false, &sum_text_grown, sum_text_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && wasm_instance_open(&sum_text_grown.bytes, sum_text_entry_name, &grown_instance)
        && wasm_instance_observe(&grown_instance, &grown_value, &grown_code, &grown_site)
        && grown_value == 0 && grown_code == 4 && grown_site == 22);
    wasm_instance_close(&grown_instance); sol_wasm_represented_output_free(&sum_text_grown);
    sol_wasm_represented_test_allocator_memory(0, 0);
    SolWasmRepresentedLimits exact_runtime = sol_wasm_represented_default_limits();
    represented_control("unit-text-sums.same-instance-exact");
    SolWasmRepresentedOutput reset_runtime; WasmInstance reset_instance;
    exact_runtime.max_allocation_requests = 40; exact_runtime.max_allocation_bytes = 402;
    sol_wasm_represented_output_init(&reset_runtime);
    CHECK(build_multiroot(sum_text_directory, false, &reset_runtime, sum_text_ids, &exact_runtime,
        SOL_WASM_REPRESENTED_OK) && wasm_instance_open(&reset_runtime.bytes, sum_text_entry_name,
            &reset_instance));
    if (reset_instance.instance != NULL) {
        CHECK(wasm_instance_call(&reset_instance, 42, 0, 0));
        CHECK(wasm_instance_call(&reset_instance, 42, 0, 0));
    }
    wasm_instance_close(&reset_instance); sol_wasm_represented_output_free(&reset_runtime);
    /* Every nonzero build quota is exact-or-one-below.  Dynamic allocator caps
     * above remain runtime checks and are covered by the 40/402 probes. */
#define CHECK_SUM_TEXT_BUILD_CAP(field, exact) do { \
    SolWasmRepresentedLimits capped_limits = sol_wasm_represented_default_limits(); \
    SolWasmRepresentedOutput capped; capped_limits.field = (exact); \
    sol_wasm_represented_output_init(&capped); \
    CHECK(build_multiroot(sum_text_directory, false, &capped, sum_text_ids, &capped_limits, \
        SOL_WASM_REPRESENTED_OK) && capped.bytes.count == sum_text.bytes.count \
        && memcmp(capped.bytes.bytes, sum_text.bytes.bytes, sum_text.bytes.count) == 0 \
        && memcmp(&capped.usage, &sum_text.usage, sizeof sum_text.usage) == 0); \
    sol_wasm_represented_output_free(&capped); capped_limits.field = (exact) - 1; \
    sol_wasm_represented_output_init(&capped); \
    CHECK(build_multiroot(sum_text_directory, false, &capped, sum_text_ids, &capped_limits, \
        SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED) && capped.bytes.bytes == NULL \
        && capped.bytes.count == 0 && usage_zero(&capped.usage)); \
    sol_wasm_represented_output_free(&capped); \
} while (0)
    CHECK_SUM_TEXT_BUILD_CAP(max_functions, 9u); CHECK_SUM_TEXT_BUILD_CAP(max_blocks, 13u);
    CHECK_SUM_TEXT_BUILD_CAP(max_edges, 16u); CHECK_SUM_TEXT_BUILD_CAP(max_values, 35u);
    CHECK_SUM_TEXT_BUILD_CAP(max_locals, 103u); CHECK_SUM_TEXT_BUILD_CAP(max_generated_nodes, 3090u);
    CHECK_SUM_TEXT_BUILD_CAP(max_static_data_bytes, 37u);
    CHECK_SUM_TEXT_BUILD_CAP(max_provenance_records, 105u);
    CHECK_SUM_TEXT_BUILD_CAP(max_work_bytes, 58417u); CHECK_SUM_TEXT_BUILD_CAP(max_scratch_bytes, 23713u);
    CHECK_SUM_TEXT_BUILD_CAP(max_owned_bytes, 30637u); CHECK_SUM_TEXT_BUILD_CAP(max_output_bytes, 20397u);
#undef CHECK_SUM_TEXT_BUILD_CAP
    SolWasmRepresentedLimits sum_text_zero = {0}; SolWasmRepresentedOutput sum_text_default;
    sol_wasm_represented_output_init(&sum_text_default);
    CHECK(build_multiroot(sum_text_directory, false, &sum_text_default, sum_text_ids, &sum_text_zero,
        SOL_WASM_REPRESENTED_OK) && sum_text_default.bytes.count == sum_text.bytes.count
        && memcmp(sum_text_default.bytes.bytes, sum_text.bytes.bytes, sum_text.bytes.count) == 0);
    sol_wasm_represented_output_free(&sum_text_default);
#define CHECK_SUM_TEXT_PARTIAL_ZERO(field) do { \
    SolWasmRepresentedLimits partial = sol_wasm_represented_default_limits(); \
    SolWasmRepresentedOutput rejected; partial.field = 0; sol_wasm_represented_output_init(&rejected); \
    CHECK(build_multiroot(sum_text_directory, false, &rejected, sum_text_ids, &partial, \
        SOL_WASM_REPRESENTED_INVALID_ARGUMENT) && rejected.bytes.bytes == NULL \
        && rejected.bytes.count == 0 && usage_zero(&rejected.usage)); \
    sol_wasm_represented_output_free(&rejected); \
} while (0)
    CHECK_SUM_TEXT_PARTIAL_ZERO(max_functions); CHECK_SUM_TEXT_PARTIAL_ZERO(max_blocks);
    CHECK_SUM_TEXT_PARTIAL_ZERO(max_edges); CHECK_SUM_TEXT_PARTIAL_ZERO(max_values);
    CHECK_SUM_TEXT_PARTIAL_ZERO(max_locals); CHECK_SUM_TEXT_PARTIAL_ZERO(max_generated_nodes);
    CHECK_SUM_TEXT_PARTIAL_ZERO(max_table_elements); CHECK_SUM_TEXT_PARTIAL_ZERO(max_static_data_bytes);
    CHECK_SUM_TEXT_PARTIAL_ZERO(max_allocation_requests); CHECK_SUM_TEXT_PARTIAL_ZERO(max_allocation_bytes);
    CHECK_SUM_TEXT_PARTIAL_ZERO(max_provenance_records); CHECK_SUM_TEXT_PARTIAL_ZERO(max_work_bytes);
    CHECK_SUM_TEXT_PARTIAL_ZERO(max_scratch_bytes); CHECK_SUM_TEXT_PARTIAL_ZERO(max_owned_bytes);
    CHECK_SUM_TEXT_PARTIAL_ZERO(max_output_bytes);
#undef CHECK_SUM_TEXT_PARTIAL_ZERO
    SolWasmRepresentedOutput sum_text_probe;
    represented_control("unit-text-sums.faults-rootorder-relocation");
    sol_wasm_represented_output_init(&sum_text_probe);
    CHECK(build_multiroot(sum_text_directory, false, &sum_text_probe, sum_text_ids, NULL,
        SOL_WASM_REPRESENTED_OK));
    sol_wasm_represented_output_free(&sum_text_probe);
    size_t sum_text_allocation_count = sol_wasm_represented_test_allocation_attempts();
    CHECK(sum_text_allocation_count == 225);
    for (size_t attempt = 1; attempt <= sum_text_allocation_count; ++attempt) {
        SolWasmRepresentedOutput failed, retry;
        sol_wasm_represented_output_init(&failed);
        represented_fault(FAULT_SUM_TEXT, attempt, attempt);
        CHECK(build_multiroot(sum_text_directory, false, &failed, sum_text_ids, NULL,
            SOL_WASM_REPRESENTED_ALLOCATION_FAILED) && failed.bytes.bytes == NULL
            && failed.bytes.count == 0 && usage_zero(&failed.usage));
        sol_wasm_represented_output_free(&failed);
        sol_wasm_represented_test_fail_allocation_after(0);
        sol_wasm_represented_output_init(&retry);
        CHECK(build_multiroot(sum_text_directory, false, &retry, sum_text_ids, NULL,
            SOL_WASM_REPRESENTED_OK) && retry.bytes.count == sum_text.bytes.count
            && memcmp(retry.bytes.bytes, sum_text.bytes.bytes, sum_text.bytes.count) == 0
            && memcmp(&retry.usage, &sum_text.usage, sizeof retry.usage) == 0);
        sol_wasm_represented_output_free(&retry);
    }
    SolWasmRepresentedOutput sum_text_reverse;
    size_t sum_text_reverse_ids[2];
    sol_wasm_represented_output_init(&sum_text_reverse);
    CHECK(build_multiroot(sum_text_directory, true, &sum_text_reverse, sum_text_reverse_ids, NULL,
        SOL_WASM_REPRESENTED_OK) && sum_text_reverse.bytes.count == sum_text.bytes.count
        && memcmp(sum_text_reverse.bytes.bytes, sum_text.bytes.bytes, sum_text.bytes.count) == 0
        && memcmp(&sum_text_reverse.usage, &sum_text.usage, sizeof sum_text.usage) == 0);
    sol_wasm_represented_output_free(&sum_text_reverse);
    char sum_text_relocated[512], sum_text_source[768], sum_text_destination[768];
    (void)snprintf(sum_text_relocated, sizeof sum_text_relocated, "%s/p43_sum_text_relocated",
        SOL_TEST_BINARY_DIR);
    (void)mkdir(sum_text_relocated, 0700);
    (void)snprintf(sum_text_source, sizeof sum_text_source, "%s/main.sol", sum_text_directory);
    (void)snprintf(sum_text_destination, sizeof sum_text_destination, "%s/main.sol", sum_text_relocated);
    FILE *sum_text_input = fopen(sum_text_source, "rb");
    FILE *sum_text_output = fopen(sum_text_destination, "wb");
    CHECK(sum_text_input != NULL && sum_text_output != NULL);
    if (sum_text_input != NULL && sum_text_output != NULL) {
        uint8_t copy_bytes[256]; size_t copy_count = 0;
        while ((copy_count = fread(copy_bytes, 1, sizeof copy_bytes, sum_text_input)) != 0)
            CHECK(fwrite(copy_bytes, 1, copy_count, sum_text_output) == copy_count);
    }
    if (sum_text_input != NULL) fclose(sum_text_input);
    if (sum_text_output != NULL) fclose(sum_text_output);
    SolWasmRepresentedOutput sum_text_relocated_output; size_t sum_text_relocated_ids[2];
    sol_wasm_represented_output_init(&sum_text_relocated_output);
    CHECK(build_multiroot(sum_text_relocated, false, &sum_text_relocated_output,
        sum_text_relocated_ids, NULL, SOL_WASM_REPRESENTED_OK)
        && sum_text_relocated_output.bytes.count == sum_text.bytes.count
        && memcmp(sum_text_relocated_output.bytes.bytes, sum_text.bytes.bytes, sum_text.bytes.count) == 0
        && memcmp(&sum_text_relocated_output.usage, &sum_text.usage,
            sizeof sum_text.usage) == 0);
    sol_wasm_represented_output_free(&sum_text_relocated_output);
    sol_wasm_represented_output_free(&unit_sum);
    sol_wasm_represented_output_free(&sum_text);
}

static void run_text(size_t first, size_t last) {
    (void)first; (void)last;
    represented_control("text.all-controls-mutations-quotas-sequences");
    enum {
        P43_LITERAL_SITE = 4, P43_COPY_SITE = 5, P43_FINAL_SITE = 12,
        P43_ENTRY_REQUESTS = 19, P43_ENTRY_BYTES = 116,
    };
    SolWasmRepresentedLimits limits = sol_wasm_represented_default_limits();
    SolWasmRepresentedOutput output;
    SolDiagnostics diagnostics; SolHirModule hir; SolTypeTable types; SolEffectTable effects;
    SolContractTable contracts; SolIr ir; SolPackage package;
    sol_diagnostics_init(&diagnostics); sol_hir_module_init(&hir); sol_type_table_init(&types);
    sol_effect_table_init(&effects); sol_contract_table_init(&contracts); sol_ir_init(&ir);
    sol_package_init(&package);
    char error[256]; char directory[512];
    (void)snprintf(directory, sizeof directory, "%s/tests/conformance/p43_text_literal",
        SOL_TEST_SOURCE_DIR);
    CHECK(sol_package_load_directory(&package, directory, &diagnostics, error, sizeof error));
    SolHirFileScope scope = {package.files[0].module_name, package.files[0].import_start,
        package.files[0].import_count, package.files[0].item_start, package.files[0].item_count};
    CHECK(sol_hir_lower_scoped(&package.source, &package.syntax, &scope, 1, &hir, &diagnostics));
    CHECK(sol_type_check(&package.source, &package.syntax, &hir, &types, &diagnostics));
    CHECK(sol_effect_check(&package.source, &package.syntax, &hir, &types, &effects, &diagnostics));
    CHECK(sol_contract_lower(&package.source, &package.syntax, &hir, &types, &effects, &contracts,
        &diagnostics));
    CHECK(sol_ir_lower_scoped(&package.source, &package.syntax, &hir, &types, &effects, &contracts,
        package.files, 1, &ir, &diagnostics));
    SolMirProgramRoot roots[1]; size_t root_count = 0;
    for (size_t i = 0; i < ir.callable_count; ++i) if (ir.callables[i].kind == SOL_IR_CALLABLE_FUNCTION
        && !strcmp(ir.callables[i].name, "launch"))
        roots[root_count++] = (SolMirProgramRoot){i, SOL_MIR_PROGRAM_ROOT_ENTRY};
    SolMirConcreteProgram concrete; SolMirRuntimeConventions conventions; SolMirRuntimeValues values;
    SolMirRuntimeCleanup cleanup; SolMirRuntimeHostAbi host; SolMirRuntimeHandlerAbi handler;
    SolMirRuntimeLoweredProgram lowered;
    sol_mir_concrete_program_init(&concrete); sol_mir_runtime_conventions_init(&conventions);
    sol_mir_runtime_values_init(&values); sol_mir_runtime_cleanup_init(&cleanup);
    sol_mir_runtime_host_abi_init(&host); sol_mir_runtime_handler_abi_init(&handler);
    sol_mir_runtime_lowered_program_init(&lowered);
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    CHECK(root_count == 1 && sol_mir_concrete_program_build(&(SolMirConcreteBuildRequest){&ir, roots,
        root_count, NULL, 0, &target, NULL}, &concrete, &diagnostics)
        == SOL_MIR_CONCRETE_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_conventions_build(&(SolMirRuntimeConventionsBuildRequest){&concrete, NULL},
        &conventions, &diagnostics) == SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_values_build(&(SolMirRuntimeValuesBuildRequest){&conventions, NULL},
        &values, &diagnostics) == SOL_MIR_RUNTIME_VALUES_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_cleanup_build(&(SolMirRuntimeCleanupBuildRequest){&conventions, &values, NULL},
        &cleanup, &diagnostics) == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_host_abi_build(&(SolMirRuntimeHostAbiBuildRequest){&conventions, &values,
        &cleanup, NULL}, &host, &diagnostics) == SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_handler_abi_build(&(SolMirRuntimeHandlerAbiBuildRequest){&conventions, &values,
        &cleanup, &host, NULL}, &handler, &diagnostics) == SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_SUCCEEDED);
    CHECK(sol_mir_runtime_lowered_program_build(&(SolMirRuntimeLoweredProgramBuildRequest){&conventions,
        &values, &cleanup, &host, &handler, NULL}, &lowered, &diagnostics)
        == SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED);
    size_t literals = 0, text_copies = 0, bool_copies = 0, first_failure = SIZE_MAX;
    for (size_t i = 0; i < concrete.materialization.instruction_count; ++i) {
        const SolMirMaterializedInstruction *instruction = &concrete.materialization.instructions[i];
        bool text_copy = instruction->kind == SOL_MIR_INST_LOAD_COPY
            && instruction->place < concrete.materialization.place_count
            && concrete.materialization.places[instruction->place].final_type < concrete.layout.type_count
            && concrete.layout.types[concrete.materialization.places[instruction->place].final_type].recipe
                < values.allocation_plan_count
            && values.allocation_plans[concrete.layout.types[
                concrete.materialization.places[instruction->place].final_type].recipe].kind
                == SOL_MIR_RUNTIME_ALLOCATION_PLAN_TEXT;
        bool routed = instruction->kind == SOL_MIR_INST_CONST_TEXT || text_copy;
        if (!routed) {
            if (instruction->kind == SOL_MIR_INST_LOAD_COPY) {
                ++bool_copies;
                CHECK(lowered.image_instructions[i].cleanup_event == SOL_MIR_RUNTIME_NONE
                    && lowered.image_instructions[i].failure_site == SOL_MIR_RUNTIME_NONE);
            }
            continue;
        }
        if (instruction->kind == SOL_MIR_INST_CONST_TEXT) ++literals; else ++text_copies;
        const SolMirRuntimeLoweredImageInstruction *row = &lowered.image_instructions[i];
        CHECK(row->cleanup_event < cleanup.event_count && row->failure_site == SOL_MIR_RUNTIME_NONE);
        if (row->cleanup_event >= cleanup.event_count) continue;
        const SolMirRuntimeCleanupEvent *event = &cleanup.events[row->cleanup_event];
        CHECK(event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION
            && event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
            && event->operation == i && event->supplemental_site < cleanup.supplemental_site_count
            && event->inherited_failure_site == SOL_MIR_RUNTIME_NONE
            && event->producer == SOL_MIR_RUNTIME_CLEANUP_PRODUCER_SUPPLEMENTAL_ALLOCATION
            && event->transitions.count == 2);
        if (event->supplemental_site >= cleanup.supplemental_site_count
            || event->transitions.count != 2) continue;
        const SolMirRuntimeCleanupSupplementalSite *site = &cleanup.supplemental_sites[
            event->supplemental_site];
        const SolMirRuntimeCleanupTransition *normal = &cleanup.transitions[event->transitions.offset];
        const SolMirRuntimeCleanupTransition *failure = &cleanup.transitions[event->transitions.offset + 1];
        CHECK(site->event == row->cleanup_event && site->allowed_codes == 24
            && normal->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL
            && failure->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
            && failure->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_SUPPLEMENTAL_P33
            && failure->failure_site == event->supplemental_site && failure->failure_mask == 24
            && failure->continuation == SOL_MIR_RUNTIME_NONE);
        if (first_failure == SIZE_MAX) first_failure = event->transitions.offset + 1;
    }
    CHECK(literals == 6 && text_copies == 4 && bool_copies == 2 && first_failure != SIZE_MAX);
    if (first_failure != SIZE_MAX) {
        uint32_t saved_mask = cleanup.transitions[first_failure].failure_mask;
        cleanup.transitions[first_failure].failure_mask ^= 1u;
        CHECK(!sol_mir_runtime_cleanup_validate(&cleanup, NULL));
        cleanup.transitions[first_failure].failure_mask = saved_mask;
        CHECK(sol_mir_runtime_cleanup_validate(&cleanup, NULL));
    }
    sol_wasm_represented_output_init(&output);
    SolWasmRepresentedResult represented = sol_wasm_represented_build(
        &(SolWasmRepresentedBuildRequest){&lowered, directory, NULL}, &output, &diagnostics);
    if (represented != SOL_WASM_REPRESENTED_OK) {
        fprintf(stderr, "represented result=%d\n", represented);
        sol_diagnostics_render_human(stderr, &package.source, &diagnostics);
    }
    CHECK(represented == SOL_WASM_REPRESENTED_OK);
    CHECK(output.bytes.count != 0 && sol_wasm_represented_validate(&output.bytes)
        == SOL_WASM_REPRESENTED_OK);
    static const uint8_t expected_sha256[32] = {0x9a,0xa7,0x76,0xff,0x47,0x9e,0x0e,0x8f,0xb2,0x0b,0xe8,0x0b,0x49,0x0d,0xd3,0x27,0x20,0x1d,0x22,0x2c,0x6b,0xd3,0x40,0x22,0xdc,0x1c,0x2b,0x31,0xeb,0x4d,0xf7,0x11};
    uint8_t output_sha256[32];
    sha256(output.bytes.bytes, output.bytes.count, output_sha256);
    CHECK(memcmp(output_sha256, expected_sha256, sizeof output_sha256) == 0);
    /* This is the complete physical census, including both private Text
     * helpers.  The allocation fields deliberately meter runtime Text demand,
     * not Binaryen/Wasmtime's third-party allocations. */
    CHECK(usage_equal(&output.usage, &(SolWasmRepresentedUsage){5,8,10,22,60,1891,0,65,0,0,71,41761,15973,23729,13489}));
    /* The custom payload is parsed before Wasmtime.  This corruption preserves
     * a structurally valid custom section while invalidating its private wire
     * schema, so it must be INVALID_INPUT rather than a runtime rejection. */
    uint8_t *malformed = malloc(output.bytes.count);
    represented_control("text.late-census-wire-mutations");
    CHECK(malformed != NULL);
    if (malformed != NULL) {
        memcpy(malformed, output.bytes.bytes, output.bytes.count);
        for (size_t i = 0; i + 4 <= output.bytes.count; ++i) {
            if (memcmp(malformed + i, "P43P", 4) == 0) {
                malformed[i] = 'X';
                break;
            }
        }
        CHECK(sol_wasm_represented_validate(&(SolWasmBackendBytes){malformed, output.bytes.count})
            == SOL_WASM_REPRESENTED_INVALID_INPUT);
        free(malformed);
    }
    /* Raw validation owns four bounded parser-map allocations.  Fault every
     * ordinal independently; unlike a build, this API has no accounting
     * context, so allocation failure must retain its public classification. */
    for (size_t ordinal = 1; ordinal <= 4; ++ordinal) {
        represented_control("text.parser-faults");
        size_t before = sol_wasm_represented_test_allocation_attempts();
        represented_fault(FAULT_PARSER_MAP, ordinal, before + ordinal);
        CHECK(sol_wasm_represented_validate(&output.bytes)
            == SOL_WASM_REPRESENTED_ALLOCATION_FAILED);
        sol_wasm_represented_test_fail_allocation_after(0);
    }
    CHECK(sol_wasm_represented_validate(&output.bytes) == SOL_WASM_REPRESENTED_OK);
    ProvenanceLayout layout;
    CHECK(provenance_layout(&output.bytes, &layout) && layout.count == 71);
#define CHECK_PRIVATE_MUTATION(edit) do { \
    uint8_t *mutated = malloc(output.bytes.count); \
    CHECK(mutated != NULL); \
    if (mutated != NULL) { \
        memcpy(mutated, output.bytes.bytes, output.bytes.count); \
        edit; \
        CHECK(sol_wasm_represented_validate(&(SolWasmBackendBytes){mutated, output.bytes.count}) \
            == SOL_WASM_REPRESENTED_INVALID_INPUT); \
        free(mutated); \
    } \
} while (0)
    ExportLayout exports;
    CHECK(export_layout(&output.bytes, &exports));
#define CHECK_PRIVATE_TRUNCATION(length) do { \
    CHECK(sol_wasm_represented_validate(&(SolWasmBackendBytes){output.bytes.bytes, (length)}) \
        == SOL_WASM_REPRESENTED_INVALID_INPUT); \
} while (0)
    /* A bounded export section must consume every byte.  These inputs are
     * deliberately raw/untrusted modules, so all short forms must stop at the
     * file bound without touching the bytes beyond it. */
    CHECK_PRIVATE_TRUNCATION(exports.payload);
    CHECK_PRIVATE_TRUNCATION(exports.first_name);
    CHECK_PRIVATE_TRUNCATION(exports.first_kind);
    CHECK_PRIVATE_TRUNCATION(exports.first_index);
    CHECK_PRIVATE_MUTATION(mutated[exports.payload] = UINT8_C(0x80));
    CHECK_PRIVATE_MUTATION(mutated[exports.first_name_length] = UINT8_C(0xff));
    CHECK_PRIVATE_MUTATION(mutated[exports.first_kind] = UINT8_C(0xff));
    CHECK_PRIVATE_MUTATION(mutated[exports.first_index] = UINT8_C(0x80));
    size_t padded_size = output.bytes.count + 1;
    uint8_t *padded = malloc(padded_size);
    CHECK(padded != NULL);
    if (padded != NULL) {
        memcpy(padded, output.bytes.bytes, exports.section_end);
        padded[exports.section_end] = 0;
        memcpy(padded + exports.section_end + 1, output.bytes.bytes + exports.section_end,
            output.bytes.count - exports.section_end);
        uint32_t export_bytes = (uint32_t)(exports.section_end - exports.payload);
        CHECK(write_uleb_same_width(padded + exports.size_offset, exports.size_width,
            export_bytes + 1));
        CHECK(sol_wasm_represented_validate(&(SolWasmBackendBytes){padded, padded_size})
            == SOL_WASM_REPRESENTED_INVALID_INPUT);
        free(padded);
    }
    size_t duplicate_export_size = output.bytes.count + exports.section_end - exports.section_start;
    uint8_t *duplicate_export = malloc(duplicate_export_size);
    CHECK(duplicate_export != NULL);
    if (duplicate_export != NULL) {
        memcpy(duplicate_export, output.bytes.bytes, exports.section_end);
        memcpy(duplicate_export + exports.section_end, output.bytes.bytes + exports.section_start,
            exports.section_end - exports.section_start);
        memcpy(duplicate_export + exports.section_end + exports.section_end - exports.section_start,
            output.bytes.bytes + exports.section_end, output.bytes.count - exports.section_end);
        CHECK(sol_wasm_represented_validate(&(SolWasmBackendBytes){duplicate_export,
            duplicate_export_size}) == SOL_WASM_REPRESENTED_INVALID_INPUT);
        free(duplicate_export);
    }
#undef CHECK_PRIVATE_TRUNCATION
    /* Every one of these keeps the enclosing custom section well-formed Wasm;
     * rejection is therefore owned by the independent P43 wire parser. */
    CHECK_PRIVATE_MUTATION(mutated[layout.payload + 4] = 2);
    CHECK_PRIVATE_MUTATION(mutated[layout.payload + 8] = 11); /* trailing record */
    CHECK_PRIVATE_MUTATION(mutated[layout.payload + 8] = 1;
        mutated[layout.payload + 9] = UINT8_C(0x80)); /* 32769 records */
    CHECK_PRIVATE_MUTATION(mutated[layout.payload + 8] = 0;
        mutated[layout.payload + 9] = UINT8_C(0x80)); /* capped 32768 records */
    CHECK_PRIVATE_MUTATION(mutated[layout.record_start[0] + 2] = 1);
    CHECK_PRIVATE_MUTATION(mutated[layout.record_start[0]] = 0);
    CHECK_PRIVATE_MUTATION(mutated[layout.record_start[0] + 1] = 1);
    CHECK_PRIVATE_MUTATION(mutated[layout.record_start[0] + 8] = '/');
    CHECK_PRIVATE_MUTATION(memcpy(mutated + layout.record_start[0] + 8, "../n.sol", 8));
    CHECK_PRIVATE_MUTATION(mutated[layout.record_start[0] + 4] = UINT8_C(0xff));
    CHECK_PRIVATE_MUTATION(memset(mutated + layout.record_start[0] + 20, 0, 4));
    if (layout.record_end[4] - layout.record_start[4] == layout.record_end[5] - layout.record_start[5]) {
        CHECK_PRIVATE_MUTATION(memcpy(mutated + layout.record_start[5],
            mutated + layout.record_start[4], layout.record_end[4] - layout.record_start[4]));
        uint8_t *swapped = malloc(output.bytes.count);
        CHECK(swapped != NULL);
        if (swapped != NULL) {
            memcpy(swapped, output.bytes.bytes, output.bytes.count);
            size_t length = layout.record_end[4] - layout.record_start[4];
            uint8_t *temporary = malloc(length);
            CHECK(temporary != NULL);
            if (temporary != NULL) {
                memcpy(temporary, swapped + layout.record_start[4], length);
                memcpy(swapped + layout.record_start[4], swapped + layout.record_start[5], length);
                memcpy(swapped + layout.record_start[5], temporary, length);
                CHECK(sol_wasm_represented_validate(&(SolWasmBackendBytes){swapped, output.bytes.count})
                    == SOL_WASM_REPRESENTED_INVALID_INPUT);
                free(temporary);
            }
            free(swapped);
        }
    } else CHECK(false);
    /* Renaming the section makes an otherwise valid Wasm module lack P43
     * provenance.  Duplicating the whole custom section is likewise valid
     * Wasm but forbidden by the physical P43 shape. */
    CHECK_PRIVATE_MUTATION(mutated[layout.payload - strlen(SOL_WASM_REPRESENTED_PROVENANCE_SECTION)] = 'x');
#undef CHECK_PRIVATE_MUTATION
    size_t duplicate_size = output.bytes.count + layout.section_end - layout.section_start;
    uint8_t *duplicate = malloc(duplicate_size);
    CHECK(duplicate != NULL);
    if (duplicate != NULL) {
        memcpy(duplicate, output.bytes.bytes, output.bytes.count);
        memcpy(duplicate + output.bytes.count, output.bytes.bytes + layout.section_start,
            layout.section_end - layout.section_start);
        CHECK(sol_wasm_represented_validate(&(SolWasmBackendBytes){duplicate, duplicate_size})
            == SOL_WASM_REPRESENTED_INVALID_INPUT);
        free(duplicate);
    }
    CHECK(sol_wasm_represented_validate(&(SolWasmBackendBytes){output.bytes.bytes,
        layout.section_start}) == SOL_WASM_REPRESENTED_INVALID_INPUT);
    ProvenanceRecord literal_provenance, copy_provenance;
    CHECK(provenance_record(&output.bytes, P43_LITERAL_SITE, &literal_provenance));
    CHECK(provenance_record(&output.bytes, P43_COPY_SITE, &copy_provenance));
    static const char callable_key[] =
        "sol.i1.02d2bac763040fd05e23baf82be514ef.9308ad8e9a769930b6e9876f172f94b2b910ace0c0d6c61ee02837011f8a5319";
    CHECK(literal_provenance.tag == 4 && literal_provenance.kind == 0
        && bytes_equal(literal_provenance.path, literal_provenance.path_count, "main.sol")
        && literal_provenance.start == 132 && literal_provenance.end == 139
        && bytes_equal(literal_provenance.symbol, literal_provenance.symbol_count, callable_key)
        && literal_provenance.ordinal == 0);
    CHECK(copy_provenance.tag == 4 && copy_provenance.kind == 0
        && bytes_equal(copy_provenance.path, copy_provenance.path_count, "main.sol")
        && copy_provenance.start == 157 && copy_provenance.end == 162
        && bytes_equal(copy_provenance.symbol, copy_provenance.symbol_count, callable_key)
        && copy_provenance.ordinal == 0);
    CHECK(conventions.entry_count == 1 && invoke_named(&output.bytes,
        conventions.entries[0].symbol.bytes, 1, 0, 0));
    /* Every backend-owned allocation is independently faulted.  The hook is
     * relative to each build, every failed output is fully empty, and an
     * immediate retry proves cleanup/resettable fault injection. */
    SolWasmRepresentedOutput allocation_probe;
    represented_control("text.build-faults-retries");
    sol_wasm_represented_output_init(&allocation_probe);
    CHECK(sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory, NULL},
        &allocation_probe, &diagnostics) == SOL_WASM_REPRESENTED_OK);
    sol_wasm_represented_output_free(&allocation_probe);
    size_t allocation_count = sol_wasm_represented_test_allocation_attempts();
    CHECK(allocation_count == 147);
    for (size_t attempt = 1; attempt <= allocation_count; ++attempt) {
        SolWasmRepresentedOutput failed;
        sol_wasm_represented_output_init(&failed);
        represented_fault(FAULT_TEXT_BUILD, attempt, attempt);
        CHECK(sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory, NULL},
            &failed, &diagnostics) == SOL_WASM_REPRESENTED_ALLOCATION_FAILED);
        CHECK(failed.bytes.bytes == NULL && failed.bytes.count == 0 && usage_zero(&failed.usage));
        sol_wasm_represented_output_free(&failed);
        sol_wasm_represented_test_fail_allocation_after(0);
        sol_wasm_represented_output_init(&failed);
        CHECK(sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory, NULL},
            &failed, &diagnostics) == SOL_WASM_REPRESENTED_OK);
        CHECK(failed.bytes.count == output.bytes.count
            && memcmp(failed.bytes.bytes, output.bytes.bytes, output.bytes.count) == 0
            && memcmp(&failed.usage, &output.usage, sizeof failed.usage) == 0);
        sol_wasm_represented_output_free(&failed);
    }
    /* Every build-time quota is tested at the frozen exact census and exactly
     * one unit below it.  Runtime Text allocation quotas are exercised below
     * through entry invocation, since they intentionally do not reject module
     * construction. */
#define CHECK_BUILD_CAP(field, exact) do { \
    SolWasmRepresentedOutput capped; \
    SolWasmRepresentedLimits cap_limits = limits; \
    cap_limits.field = (exact); \
    sol_wasm_represented_output_init(&capped); \
    CHECK(sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory, \
        &cap_limits}, &capped, &diagnostics) == SOL_WASM_REPRESENTED_OK); \
    CHECK(capped.bytes.count == output.bytes.count \
        && memcmp(capped.bytes.bytes, output.bytes.bytes, output.bytes.count) == 0); \
    sol_wasm_represented_output_free(&capped); \
    cap_limits.field = (exact) - 1; \
    sol_wasm_represented_output_init(&capped); \
    CHECK(sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory, \
        &cap_limits}, &capped, &diagnostics) == SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED); \
    CHECK(capped.bytes.bytes == NULL && capped.bytes.count == 0 && usage_zero(&capped.usage)); \
    sol_wasm_represented_output_free(&capped); \
} while (0)
    CHECK_BUILD_CAP(max_functions, 5u);
    CHECK_BUILD_CAP(max_blocks, 8u);
    CHECK_BUILD_CAP(max_edges, 10u);
    CHECK_BUILD_CAP(max_values, 22u);
    CHECK_BUILD_CAP(max_locals, 60u);
    CHECK_BUILD_CAP(max_generated_nodes, 1891u);
    CHECK_BUILD_CAP(max_static_data_bytes, 65u);
    CHECK_BUILD_CAP(max_provenance_records, 71u);
    CHECK_BUILD_CAP(max_work_bytes, 41761u);
    CHECK_BUILD_CAP(max_scratch_bytes, 15973u);
    CHECK_BUILD_CAP(max_owned_bytes, 23729u);
    CHECK_BUILD_CAP(max_output_bytes, 13489u);
#undef CHECK_BUILD_CAP
    /* Complete request limits are bounded by the private wire grammar even
     * when this small fixture emits fewer records. */
    SolWasmRepresentedLimits provenance_wire_cap = limits;
    represented_control("text.caps-defaults-partial-wirebounds");
    SolWasmRepresentedOutput provenance_wire_output;
    provenance_wire_cap.max_provenance_records = 32768;
    sol_wasm_represented_output_init(&provenance_wire_output);
    CHECK(sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory,
        &provenance_wire_cap}, &provenance_wire_output, &diagnostics) == SOL_WASM_REPRESENTED_OK
        && provenance_wire_output.bytes.count == output.bytes.count
        && memcmp(provenance_wire_output.bytes.bytes, output.bytes.bytes, output.bytes.count) == 0);
    sol_wasm_represented_output_free(&provenance_wire_output);
    provenance_wire_cap.max_provenance_records = 32769;
    sol_wasm_represented_output_init(&provenance_wire_output);
    CHECK(sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory,
        &provenance_wire_cap}, &provenance_wire_output, &diagnostics)
            == SOL_WASM_REPRESENTED_INVALID_ARGUMENT
        && provenance_wire_output.bytes.bytes == NULL && provenance_wire_output.bytes.count == 0
        && usage_zero(&provenance_wire_output.usage));
    sol_wasm_represented_output_free(&provenance_wire_output);
    SolWasmRepresentedOutput identity;
    sol_wasm_represented_output_init(&identity);
    SolWasmRepresentedLimits zero_limits = {0};
    CHECK(sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory,
        &zero_limits}, &identity, &diagnostics) == SOL_WASM_REPRESENTED_OK);
    CHECK(identity.bytes.count == output.bytes.count
        && memcmp(identity.bytes.bytes, output.bytes.bytes, output.bytes.count) == 0);
    sol_wasm_represented_output_free(&identity);
#define CHECK_PARTIAL_ZERO(field) do { \
    SolWasmRepresentedOutput rejected; \
    SolWasmRepresentedLimits partial = limits; \
    partial.field = 0; \
    sol_wasm_represented_output_init(&rejected); \
    CHECK(sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory, \
        &partial}, &rejected, &diagnostics) == SOL_WASM_REPRESENTED_INVALID_ARGUMENT); \
    CHECK(rejected.bytes.bytes == NULL && rejected.bytes.count == 0 && usage_zero(&rejected.usage)); \
    sol_wasm_represented_output_free(&rejected); \
} while (0)
    CHECK_PARTIAL_ZERO(max_functions); CHECK_PARTIAL_ZERO(max_blocks); CHECK_PARTIAL_ZERO(max_edges);
    CHECK_PARTIAL_ZERO(max_values); CHECK_PARTIAL_ZERO(max_locals);
    CHECK_PARTIAL_ZERO(max_generated_nodes); CHECK_PARTIAL_ZERO(max_table_elements);
    CHECK_PARTIAL_ZERO(max_static_data_bytes); CHECK_PARTIAL_ZERO(max_allocation_requests);
    CHECK_PARTIAL_ZERO(max_allocation_bytes); CHECK_PARTIAL_ZERO(max_provenance_records);
    CHECK_PARTIAL_ZERO(max_work_bytes); CHECK_PARTIAL_ZERO(max_scratch_bytes);
    CHECK_PARTIAL_ZERO(max_owned_bytes); CHECK_PARTIAL_ZERO(max_output_bytes);
#undef CHECK_PARTIAL_ZERO
    /* These exact caps permit one entry only.  The one-page heap placement
     * additionally makes an unreset heap fail before the second entry can
     * publish a header, so both calls prove packet, quota, and heap reset. */
    SolWasmRepresentedLimits constrained = limits;
    represented_control("text.same-instance-heap-quota-reset");
    SolWasmRepresentedOutput constrained_output;
    constrained.max_allocation_requests = P43_ENTRY_REQUESTS;
    constrained.max_allocation_bytes = P43_ENTRY_BYTES;
    sol_wasm_represented_test_allocator_memory(1, UINT32_C(65384));
    sol_wasm_represented_output_init(&constrained_output);
    CHECK(sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory,
        &constrained}, &constrained_output, &diagnostics) == SOL_WASM_REPRESENTED_OK);
    WasmInstance same_instance;
    CHECK(wasm_instance_open(&constrained_output.bytes, conventions.entries[0].symbol.bytes,
        &same_instance));
    if (same_instance.instance != NULL) {
        CHECK(wasm_instance_call(&same_instance, 1, 0, 0));
        CHECK(wasm_instance_call(&same_instance, 1, 0, 0));
    }
    wasm_instance_close(&same_instance);
    sol_wasm_represented_output_free(&constrained_output);
    sol_wasm_represented_test_allocator_memory(0, 0);
    /* The hook admits only production-default (zero selector) or the one-page
     * forced-grow proof.  Two pages must fail before any module/output exists. */
    sol_wasm_represented_test_allocator_memory(2, 0);
    sol_wasm_represented_output_init(&constrained_output);
    CHECK(sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory,
        &limits}, &constrained_output, &diagnostics) == SOL_WASM_REPRESENTED_INVALID_ARGUMENT);
    CHECK(constrained_output.bytes.bytes == NULL && constrained_output.bytes.count == 0
        && usage_zero(&constrained_output.usage));
    sol_wasm_represented_output_free(&constrained_output);
    sol_wasm_represented_test_allocator_memory(0, 0);

    /* The immediately lower caps freeze the full successful-entry demand. */
    represented_control("text.runtime-shortfalls-grow");
    sol_wasm_represented_output_init(&constrained_output);
    constrained = limits; constrained.max_allocation_requests = P43_ENTRY_REQUESTS - 1;
    CHECK(sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory,
        &constrained}, &constrained_output, &diagnostics) == SOL_WASM_REPRESENTED_OK);
    CHECK(invoke_named(&constrained_output.bytes, conventions.entries[0].symbol.bytes, 0, 5,
        P43_FINAL_SITE));
    sol_wasm_represented_output_free(&constrained_output);
    sol_wasm_represented_output_init(&constrained_output);
    constrained = limits; constrained.max_allocation_bytes = P43_ENTRY_BYTES - 1;
    CHECK(sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory,
        &constrained}, &constrained_output, &diagnostics) == SOL_WASM_REPRESENTED_OK);
    CHECK(invoke_named(&constrained_output.bytes, conventions.entries[0].symbol.bytes, 0, 5,
        P43_FINAL_SITE));
    sol_wasm_represented_output_free(&constrained_output);

    /* Route 2 is the empty literal (one request/eight bytes); route 8 is the
     * first Text LOAD_COPY after the empty and "hello" literal allocations.
     * Each cap is exactly one logical unit below the target allocation. */
    sol_wasm_represented_output_init(&constrained_output);
    constrained.max_allocation_requests = 1;
    CHECK(sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory,
        &constrained}, &constrained_output, &diagnostics) == SOL_WASM_REPRESENTED_OK);
    CHECK(invoke_named(&constrained_output.bytes, conventions.entries[0].symbol.bytes, 0, 5,
        P43_LITERAL_SITE));
    sol_wasm_represented_output_free(&constrained_output);

    sol_wasm_represented_output_init(&constrained_output);
    constrained = limits; constrained.max_allocation_bytes = 8;
    CHECK(sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory,
        &constrained}, &constrained_output, &diagnostics) == SOL_WASM_REPRESENTED_OK);
    CHECK(invoke_named(&constrained_output.bytes, conventions.entries[0].symbol.bytes, 0, 5,
        P43_LITERAL_SITE));
    sol_wasm_represented_output_free(&constrained_output);

    sol_wasm_represented_output_init(&constrained_output);
    constrained = limits; constrained.max_allocation_requests = 4;
    CHECK(sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory,
        &constrained}, &constrained_output, &diagnostics) == SOL_WASM_REPRESENTED_OK);
    CHECK(invoke_named(&constrained_output.bytes, conventions.entries[0].symbol.bytes, 0, 5,
        P43_COPY_SITE));
    sol_wasm_represented_output_free(&constrained_output);

    sol_wasm_represented_output_init(&constrained_output);
    constrained = limits; constrained.max_allocation_bytes = 33;
    CHECK(sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory,
        &constrained}, &constrained_output, &diagnostics) == SOL_WASM_REPRESENTED_OK);
    CHECK(invoke_named(&constrained_output.bytes, conventions.entries[0].symbol.bytes, 0, 5,
        P43_COPY_SITE));
    sol_wasm_represented_output_free(&constrained_output);

    /* Generous logical quotas isolate a real memory.grow -1.  The private
     * one-page cursor proof leaves the empty header in-page, then forces the
     * next literal's checked grow to fail without trapping. */
    sol_wasm_represented_test_allocator_memory(1, UINT32_C(65528));
    sol_wasm_represented_output_init(&constrained_output);
    constrained = limits;
    CHECK(sol_wasm_represented_build(&(SolWasmRepresentedBuildRequest){&lowered, directory,
        &constrained}, &constrained_output, &diagnostics) == SOL_WASM_REPRESENTED_OK);
    CHECK(invoke_named(&constrained_output.bytes, conventions.entries[0].symbol.bytes, 0, 4,
        P43_LITERAL_SITE));
    sol_wasm_represented_output_free(&constrained_output);
    sol_wasm_represented_test_allocator_memory(0, 0);
    sol_wasm_represented_output_free(&output);
    sol_mir_runtime_lowered_program_free(&lowered); sol_mir_runtime_handler_abi_free(&handler);
    sol_mir_runtime_host_abi_free(&host); sol_mir_runtime_cleanup_free(&cleanup);
    sol_mir_runtime_values_free(&values); sol_mir_runtime_conventions_free(&conventions);
    sol_mir_concrete_program_free(&concrete); sol_package_free(&package); sol_ir_free(&ir);
    sol_contract_table_free(&contracts); sol_effect_table_free(&effects); sol_type_table_free(&types);
    sol_hir_module_free(&hir); sol_diagnostics_free(&diagnostics);
}

static void run_callback_shape_controls(const char *callback_directory) {
    represented_control("callback-table-wire.shape-mutations");
    /* The matrix builder starts from this exact source provenance/export set
     * and its accepted table-four control; every following case changes only
     * the listed private-envelope predicate. */
    static const ShapeVariant shape_variants[] = {SHAPE_IMPORT, SHAPE_START,
        SHAPE_MEMORY_EXPORT, SHAPE_WRONG_MEMORY_EXPORT, SHAPE_MEMORY_INITIAL, SHAPE_MEMORY_MAXIMUM,
        SHAPE_CODE_GLOBAL, SHAPE_SITE_GLOBAL, SHAPE_MISSING_ENTRY, SHAPE_RENAMED_ENTRY, SHAPE_EXTRA_ENTRY,
        SHAPE_HELPER_EXPORT, SHAPE_TABLE_EXTRA, SHAPE_TABLE_MAXIMUM, SHAPE_TABLE_EXPORT,
        SHAPE_ELEMENT_MISSING,
        SHAPE_ELEMENT_EXTRA, SHAPE_ELEMENT_OFFSET, SHAPE_ELEMENT_DUPLICATE, SHAPE_ELEMENT_FUNCTION,
        SHAPE_ELEMENT_MANY_LATE_TYPE, SHAPE_FUNCTION_OVERLIMIT};
    SolWasmRepresentedOutput callback_shape;
    sol_wasm_represented_output_init(&callback_shape);
    CHECK(build_named_root(callback_directory, "launch", &callback_shape, NULL,
        SOL_WASM_REPRESENTED_OK));
    ProvenanceLayout callback_shape_layout;
    CHECK(provenance_layout(&callback_shape.bytes, &callback_shape_layout)
        && callback_shape.usage.table_elements == 3 && callback_shape_layout.count == 75);
    SolWasmBackendBytes matrix_control = {0};
    CHECK(make_shape_variant(&callback_shape.bytes, &callback_shape_layout, SHAPE_VALID_TABLE,
        &matrix_control) && matrix_control.bytes != NULL && matrix_control.count != 0
        && wasmtime_module_valid(&matrix_control)
        && sol_wasm_represented_validate(&matrix_control) == SOL_WASM_REPRESENTED_OK);
    SolWasmBackendBytes externref_table = {0};
    CHECK(table_type_externref(&matrix_control, &externref_table)
        && sol_wasm_represented_validate(&externref_table) == SOL_WASM_REPRESENTED_INVALID_INPUT);
    free(externref_table.bytes);
    for (size_t i = 0; i < sizeof shape_variants / sizeof shape_variants[0]; ++i) {
        SolWasmBackendBytes shaped = {0};
        CHECK(make_shape_variant(&callback_shape.bytes, &callback_shape_layout, shape_variants[i], &shaped)
            && shaped.bytes != NULL && shaped.count != 0);
        if (shaped.bytes == NULL) continue;
        CHECK(wasmtime_module_valid(&shaped));
        CHECK(sol_wasm_represented_validate(&shaped) == SOL_WASM_REPRESENTED_INVALID_INPUT);
        free(shaped.bytes);
    }
    free(matrix_control.bytes);
    sol_wasm_represented_output_free(&callback_shape);
}

typedef struct {
    const char *name;
    void (*run)(size_t, size_t);
    size_t fault_chunks, fault_total;
} RepresentedRunner;
#define RUNNER(name, function, chunks, total) {name, run_##function, chunks, total}
static const RepresentedRunner represented_runners[] = {
    RUNNER("api", api, 0, 0),
    RUNNER("e1", e1, 0, 0),
    RUNNER("refined", refined, 0, 0),
    RUNNER("step-owner", step_owner, 0, 0),
    RUNNER("depth", depth, 3, 151),
    RUNNER("contracts", contracts, 0, 0),
    RUNNER("qualified-frozen", qualified_frozen, 0, 0),
    RUNNER("qualified-result", qualified_result, 10, 628),
    RUNNER("stress", stress, 16, 994),
    RUNNER("callable-holes", callable_holes, 0, 0),
    RUNNER("callback-inout", callback_inout, 0, 0),
    RUNNER("methods", methods, 0, 0),
    RUNNER("callback-table-wire", callback_table_wire, 0, 0),
    RUNNER("generic-roots", generic_roots, 0, 0),
    RUNNER("scalar-products", scalar_products, 0, 0),
    RUNNER("patterns", patterns, 0, 0),
    RUNNER("terminal-packets", terminal_packets, 0, 0),
    RUNNER("cleanup-trace", cleanup_trace, 0, 0),
    RUNNER("panic-wire-resource", panic_wire_resource, 0, 0),
    RUNNER("product-tree", product_tree, 0, 0),
    RUNNER("sums", sums, 0, 0),
    RUNNER("propagation", propagation, 0, 0),
    RUNNER("unit-text-sums", unit_text_sums, 0, 0),
    RUNNER("text", text, 0, 0),
};
#undef RUNNER

/* Composite control IDs retain their entire original assertion/teardown block.
 * Runtime quota loops and same-instance sequences are indivisible controls. */
static const char *const represented_controls[] = {
    "api.all-controls-mutations-quotas-sequences",
    "e1.all-controls-mutations-quotas-sequences",
    "refined.all-controls-mutations-quotas-sequences",
    "refined.gates-index-certificate-mutations-traces-markers",
    "refined.resources-faults-rootorder-relocation-rebuild-seal",
    "step-owner.all-controls-mutations-quotas-sequences",
    "depth.recursion-routes-boundaries-packets-repeat",
    "depth.static-catalog-chains-api-wire-owner-mutations",
    "depth.recursive-multiroot-census-caps-faults-reset-relocation",
    "depth.step-precedence-authoritative-pending-writebacks-holes",
    "contracts.all-controls-mutations-quotas-sequences",
    "contracts.requires-ensures-qualified-gates-routes",
    "contracts.allocations-owners-mutations-exact-traces-packets",
    "contracts.old-snapshot-mutations-resources-deltas-rootorder",
    "contracts.requires-forward-bool-owner-arithmetic-mutations",
    "qualified-frozen.all-controls-mutations-quotas-sequences",
    "qualified-result.all-controls-mutations-quotas-sequences",
    "qualified-result.rootorder-relocation",
    "stress.all-controls-mutations-quotas-sequences",
    "callable-holes.all-controls-mutations-quotas-sequences",
    "callable-holes.full-baseline-owner",
    "callable-holes.same-instance-cleanup",
    "callable-holes.c32-runtime-owner-cap-rootorder",
    "callback-inout.all-controls-mutations-quotas-sequences",
    "callback-inout.baseline-census-caps-defaults-faults",
    "callback-inout.runtime-quotas-grow-retry",
    "callback-inout.relocation",
    "callback-inout.same-instance-failure-success-trace",
    "methods.all-controls-mutations-quotas-sequences",
    "methods.direct-entry-packets",
    "methods.same-instance-writebacks",
    "methods.rootorder-relocation",
    "callback-table-wire.all-controls-mutations-quotas-sequences",
    "callback-table-wire.runtime-quotas-same-instance-grow",
    "callback-table-wire.shape-mutations",
    "generic-roots.all-controls-mutations-quotas-sequences",
    "scalar-products.all-controls-mutations-quotas-sequences",
    "scalar-products.same-instance-quotas-grow",
    "scalar-products.late-shape-controls-teardown",
    "patterns.all-controls-mutations-quotas-sequences",
    "patterns.runtime-request-loop",
    "patterns.same-instance-exact-shortfall-grow",
    "patterns.faults-caps-defaults-rootorder-relocation-gates",
    "terminal-packets.all-controls-mutations-quotas-sequences",
    "cleanup-trace.all-controls-mutations-quotas-sequences",
    "panic-wire-resource.all-controls-mutations-quotas-sequences",
    "product-tree.all-controls-mutations-quotas-sequences",
    "product-tree.runtime-quota-search",
    "product-tree.same-instance-late-copy-shortfall-success",
    "product-tree.grow-caps-faults-rootorder-relocation",
    "sums.all-controls-mutations-quotas-sequences",
    "sums.runtime-exact-one-below",
    "propagation.all-controls-mutations-quotas-sequences",
    "propagation.entry-census-caps-runtime-loops-same-instance",
    "propagation.owner-relocation",
    "unit-text-sums.all-controls-mutations-quotas-sequences",
    "unit-text-sums.inactive-active-payload",
    "unit-text-sums.runtime-quota-loops-shortfalls-grow",
    "unit-text-sums.same-instance-exact",
    "unit-text-sums.faults-rootorder-relocation",
    "text.all-controls-mutations-quotas-sequences",
    "text.late-census-wire-mutations",
    "text.parser-faults",
    "text.build-faults-retries",
    "text.caps-defaults-partial-wirebounds",
    "text.same-instance-heap-quota-reset",
    "text.runtime-shortfalls-grow",
};
typedef struct {
    const char *id, *owner;
    size_t count;
    bool seen[995];
} RepresentedFaultSweep;
static RepresentedFaultSweep represented_sweeps[FAULT_SWEEP_COUNT] = {
    [FAULT_C32_REPAIR] = {"c32-repair", "callable-holes", 66, {false}},
    [FAULT_PROPAGATION_RESULT] = {"propagation-result", "propagation", 130, {false}},
    [FAULT_B1_FIRST_ALLOCATION] = {"b1-first-allocation", "terminal-packets", 1, {false}},
    [FAULT_REQUIRES_FORWARD] = {"requires-forward", "contracts", 102, {false}},
    [FAULT_QUALIFIED_FROZEN] = {"qualified-frozen", "qualified-frozen", 117, {false}},
    [FAULT_QUALIFIED_RESULT] = {"qualified-result", "qualified-result", 628, {false}},
    [FAULT_STRESS] = {"stress", "stress", 994, {false}},
    [FAULT_OLD_SNAPSHOT] = {"old-snapshot", "contracts", 79, {false}},
    [FAULT_ENSURES_ROOTS] = {"ensures-roots", "contracts", 67, {false}},
    [FAULT_REFINED] = {"refined", "refined", 162, {false}},
    [FAULT_CALLABLE_HOLE] = {"callable-hole", "callable-holes", 87, {false}},
    [FAULT_CALLBACK_INOUT] = {"callback-inout", "callback-inout", 122, {false}},
    [FAULT_METHOD] = {"method", "methods", 102, {false}},
    [FAULT_CALLBACK] = {"callback", "callback-table-wire", 122, {false}},
    [FAULT_PATTERN] = {"pattern", "patterns", 114, {false}},
    [FAULT_PANIC] = {"panic", "panic-wire-resource", 27, {false}},
    [FAULT_PRODUCT_TREE] = {"product-tree", "product-tree", 208, {false}},
    [FAULT_SUM_TEXT] = {"sum-text", "unit-text-sums", 225, {false}},
    [FAULT_PARSER_MAP] = {"parser-map", "text", 4, {false}},
    [FAULT_TEXT_BUILD] = {"text-build", "text", 147, {false}},
    [FAULT_DEPTH] = {"depth", "depth", 151, {false}},
};
static const RepresentedRunner *active_runner;
static bool control_seen[sizeof represented_controls / sizeof *represented_controls];
static size_t observed_faults;

static bool control_owned(const char *id, const RepresentedRunner *runner) {
    size_t length = strlen(runner->name);
    return !strncmp(id, runner->name, length) && id[length] == '.';
}

static void represented_control(const char *id) {
    if (active_runner == NULL) return; /* Legacy focus remains separate. */
    for (size_t i = 0; i < sizeof represented_controls / sizeof *represented_controls; ++i) {
        if (!strcmp(id, represented_controls[i])) {
            CHECK(control_owned(id, active_runner));
            control_seen[i] = true;
            fprintf(stderr, "CONTROL\t%s\n", id);
            return;
        }
    }
    CHECK(false); /* Execution cannot invent an ID absent from the manifest. */
}

static void represented_fault(RepresentedFault sweep, size_t ordinal, size_t hook_after) {
    if (active_runner != NULL) {
        CHECK((size_t)sweep < FAULT_SWEEP_COUNT);
        if ((size_t)sweep < FAULT_SWEEP_COUNT) {
            RepresentedFaultSweep *record = &represented_sweeps[sweep];
            CHECK(!strcmp(record->owner, active_runner->name) && ordinal != 0
                && ordinal <= record->count && !record->seen[ordinal]);
            if (ordinal != 0 && ordinal <= record->count) {
                record->seen[ordinal] = true; ++observed_faults;
                fprintf(stderr, "FAULT\t%s.%04zu\n", record->id, ordinal);
            }
        }
    }
    /* Preserve the original capture/arming point, especially parser-map offsets. */
    sol_wasm_represented_test_fail_allocation_after(hook_after);
}

static void shard_name(const RepresentedRunner *runner, size_t chunk, char name[80]) {
    if (runner->fault_chunks == 0) (void)snprintf(name, 80, "%s", runner->name);
    else (void)snprintf(name, 80, "%s-%02zu", runner->name, chunk + 1);
}

static void fault_bounds(const RepresentedRunner *runner, size_t chunk,
    size_t *first, size_t *last) {
    *first = runner->fault_chunks == 0 ? 1 : chunk * 64 + 1;
    *last = runner->fault_chunks == 0 ? 0 : (chunk + 1) * 64;
    if (*last > runner->fault_total) *last = runner->fault_total;
}

static bool find_shard(const char *name, const RepresentedRunner **runner, size_t *chunk) {
    for (size_t i = 0; i < sizeof represented_runners / sizeof *represented_runners; ++i) {
        const RepresentedRunner *candidate = &represented_runners[i];
        size_t count = candidate->fault_chunks == 0 ? 1 : candidate->fault_chunks;
        for (size_t j = 0; j < count; ++j) {
            char actual[80]; shard_name(candidate, j, actual);
            if (!strcmp(actual, name)) { *runner = candidate; *chunk = j; return true; }
        }
    }
    return false;
}

/* TSV: kind, stable coverage ID, owning shard, duplication flag. Full emits
 * every shard's manifest; repeated heavy prerequisites are explicitly marked.
 * Fault IDs are unique over the union, including all 3503 sweep ordinals and
 * the separate B1 first-allocation fault control. */
static void manifest_shard(const RepresentedRunner *runner, size_t chunk) {
    char name[80]; size_t first, last;
    shard_name(runner, chunk, name); fault_bounds(runner, chunk, &first, &last);
    for (size_t i = 0; i < sizeof represented_controls / sizeof *represented_controls; ++i)
        if (control_owned(represented_controls[i], runner))
            printf("control\t%s\t%s\t%s\n", represented_controls[i], name,
                runner->fault_chunks == 0 ? "unique" : "duplicate-prerequisite");
    for (size_t i = 0; i < FAULT_SWEEP_COUNT; ++i) {
        const RepresentedFaultSweep *sweep = &represented_sweeps[i];
        if (strcmp(sweep->owner, runner->name)) continue;
        size_t begin = runner->fault_chunks == 0 ? 1 : first;
        size_t end = runner->fault_chunks == 0 ? sweep->count : last;
        for (size_t ordinal = begin; ordinal <= end; ++ordinal)
            printf("fault\t%s.%04zu\t%s\tunique\n", sweep->id, ordinal, name);
    }
}

static void execute_runner(const RepresentedRunner *runner, size_t first, size_t last,
    const char *name) {
    struct timespec start, finish;
    (void)timespec_get(&start, TIME_UTC);
    active_runner = runner; observed_faults = 0;
    memset(control_seen, 0, sizeof control_seen);
    for (size_t i = 0; i < FAULT_SWEEP_COUNT; ++i)
        memset(represented_sweeps[i].seen, 0, sizeof represented_sweeps[i].seen);
    int before = failures;
    fprintf(stderr, "SHARD_START\t%s\tfirst=%zu\tlast=%zu\n", name, first, last);
    runner->run(first, last);
    size_t controls = 0, expected_faults = 0;
    for (size_t i = 0; i < sizeof represented_controls / sizeof *represented_controls; ++i)
        if (control_owned(represented_controls[i], runner)) {
            CHECK(control_seen[i]); ++controls;
        }
    for (size_t i = 0; i < FAULT_SWEEP_COUNT; ++i) {
        const RepresentedFaultSweep *sweep = &represented_sweeps[i];
        if (strcmp(sweep->owner, runner->name)) continue;
        size_t begin = runner->fault_chunks == 0 ? 1 : first;
        size_t end = runner->fault_chunks == 0 ? sweep->count : last;
        expected_faults += end - begin + 1;
        for (size_t ordinal = begin; ordinal <= end; ++ordinal) CHECK(sweep->seen[ordinal]);
    }
    CHECK(observed_faults == expected_faults);
    (void)timespec_get(&finish, TIME_UTC);
    double elapsed = (double)(finish.tv_sec - start.tv_sec)
        + (double)(finish.tv_nsec - start.tv_nsec) / 1000000000.0;
    fprintf(stderr, "SHARD_COMPLETE\t%s\tstatus=%s\tseconds=%.3f\tcontrols=%zu\tfaults=%zu/%zu\n",
        name, failures == before ? "PASS" : "FAIL", elapsed, controls,
        observed_faults, expected_faults);
    active_runner = NULL;
}

int main(int argc, char **argv) {
    bool manifest = false, list = false;
    const char *selected = NULL;
    const RepresentedRunner *runner = NULL;
    size_t chunk = 0;
    if (argc == 2 && !strcmp(argv[1], "--list-shards")) list = true;
    else if (argc == 2 && !strcmp(argv[1], "--manifest")) manifest = true;
    else if (argc == 3 && !strcmp(argv[1], "--shard")) selected = argv[2];
    else if (argc == 4 && !strcmp(argv[1], "--manifest") && !strcmp(argv[2], "--shard")) {
        manifest = true; selected = argv[3];
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--list-shards | --manifest [--shard NAME] | --shard NAME]\n", argv[0]);
        return 2;
    }
    const char *focus = getenv("SOL_P44E_FOCUS");
    if (selected != NULL && focus != NULL) {
        fprintf(stderr, "--shard rejects SOL_P44E_FOCUS; unset it for exhaustive shard coverage\n");
        return 2;
    }
    if (selected != NULL && !find_shard(selected, &runner, &chunk)) {
        fprintf(stderr, "unknown represented shard: %s\n", selected); return 2;
    }
    if (list || manifest) {
        if (selected != NULL) manifest_shard(runner, chunk);
        else for (size_t i = 0; i < sizeof represented_runners / sizeof *represented_runners; ++i) {
            const RepresentedRunner *item = &represented_runners[i];
            size_t count = item->fault_chunks == 0 ? 1 : item->fault_chunks;
            for (size_t j = 0; j < count; ++j) {
                if (manifest) manifest_shard(item, j);
                else { char name[80]; shard_name(item, j, name); puts(name); }
            }
        }
        return 0;
    }
    if (argc == 1 && focus != NULL) {
        if (!strcmp(focus, "depth-gate")) return p44e_depth_gate_probe() ? 0 : 1;
        if (!strcmp(focus, "depth")) {
            if (!p44e_depth_source_probe()) return 1;
            run_depth(0, 0); return failures == 0 ? 0 : 1;
        }
        if (!strcmp(focus, "helpers")) return p44e_step_helper_boundaries() ? 0 : 1;
        return p44e_step_execution() ? 0 : 1;
    }
    /* Relocation controls must work without a preceding full-suite runner. */
    (void)mkdir(SOL_TEST_BINARY_DIR, 0700);
    struct stat scratch;
    if (stat(SOL_TEST_BINARY_DIR, &scratch) != 0 || !S_ISDIR(scratch.st_mode)) {
        fprintf(stderr, "test scratch directory unavailable: %s\n", SOL_TEST_BINARY_DIR); return 1;
    }
    if (selected != NULL) {
        size_t first, last; fault_bounds(runner, chunk, &first, &last);
        execute_runner(runner, first, last, selected);
    } else for (size_t i = 0; i < sizeof represented_runners / sizeof *represented_runners; ++i) {
        const RepresentedRunner *item = &represented_runners[i];
        execute_runner(item, 1, item->fault_total, item->name);
    }
    return failures == 0 ? 0 : 1;
}
