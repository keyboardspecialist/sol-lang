#include "wasm_backend_internal.h"

#include <binaryen-c.h>
#include <wasm.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SOL_WASM_BACKEND_FUNCTION_ID "sol.probe.v1"
#define SOL_WASM_BACKEND_MEMORY_ID "sol.memory.v1"

const char *const sol_wasm_backend_canonical_import_modules[
    SOL_WASM_BACKEND_IMPORT_MODULE_COUNT] = {
    SOL_WASM_BACKEND_RUNTIME_IMPORT_MODULE,
    SOL_WASM_BACKEND_HOST_IMPORT_MODULE,
};

static bool exact_target(const SolMirTargetDescriptor *target) {
    SolMirTargetDescriptor wasm32 = sol_mir_target_wasm32();
    return target != NULL
        && target->pointer_size == wasm32.pointer_size
        && target->pointer_alignment == wasm32.pointer_alignment
        && target->int64_alignment == wasm32.int64_alignment
        && target->endianness == wasm32.endianness
        && target->max_object_bytes == wasm32.max_object_bytes
        && target->pointer_size == 4 && target->pointer_alignment == 4
        && target->int64_alignment == 8
        && target->endianness == SOL_MIR_ENDIAN_LITTLE
        && target->max_object_bytes == UINT32_MAX;
}

void sol_wasm_backend_bytes_init(SolWasmBackendBytes *bytes) {
    if (bytes != NULL) { bytes->bytes = NULL; bytes->count = 0; }
}

void sol_wasm_backend_bytes_free(SolWasmBackendBytes *bytes) {
    if (bytes == NULL) return;
    free(bytes->bytes);
    sol_wasm_backend_bytes_init(bytes);
}

static bool names_equal(const char *left, const char *right) {
    return left != NULL && strcmp(left, right) == 0;
}

static bool binaryen_structure(BinaryenModuleRef module) {
    if (module == NULL || BinaryenGetStart(module) != NULL
        || BinaryenGetNumFunctions(module) != 1
        || BinaryenGetNumTables(module) != 1
        || BinaryenGetNumExports(module) != 2
        || BinaryenGetNumElementSegments(module) != 0
        || !BinaryenHasMemory(module)
        || BinaryenMemoryGetInitial(module, SOL_WASM_BACKEND_MEMORY_ID) != 1
        || !BinaryenMemoryHasMax(module, SOL_WASM_BACKEND_MEMORY_ID)
        || BinaryenMemoryGetMax(module, SOL_WASM_BACKEND_MEMORY_ID) != 1
        || BinaryenMemoryIsShared(module, SOL_WASM_BACKEND_MEMORY_ID)
        || BinaryenMemoryIs64(module, SOL_WASM_BACKEND_MEMORY_ID)) return false;

    BinaryenFunctionRef function = BinaryenGetFunction(module,
        SOL_WASM_BACKEND_FUNCTION_ID);
    BinaryenTableRef table = BinaryenGetTable(module, SOL_WASM_BACKEND_TABLE_ID);
    BinaryenExportRef probe = BinaryenGetExport(module,
        SOL_WASM_BACKEND_PROBE_EXPORT);
    BinaryenExportRef memory = BinaryenGetExport(module,
        SOL_WASM_BACKEND_MEMORY_EXPORT);
    return function != NULL && table != NULL && probe != NULL && memory != NULL
        && names_equal(BinaryenFunctionGetName(function), SOL_WASM_BACKEND_FUNCTION_ID)
        && BinaryenFunctionGetParams(function) == BinaryenTypeNone()
        && BinaryenFunctionGetResults(function) == BinaryenTypeInt32()
        && names_equal(BinaryenTableGetName(table), SOL_WASM_BACKEND_TABLE_ID)
        && BinaryenTableGetInitial(table) == 1 && BinaryenTableHasMax(table)
        && BinaryenTableGetMax(table) == 1
        && BinaryenTableGetType(table) == BinaryenTypeFuncref()
        && BinaryenExportGetKind(probe) == BinaryenExternalFunction()
        && names_equal(BinaryenExportGetValue(probe), SOL_WASM_BACKEND_FUNCTION_ID)
        && BinaryenExportGetKind(memory) == BinaryenExternalMemory()
        && names_equal(BinaryenExportGetValue(memory), SOL_WASM_BACKEND_MEMORY_ID);
}

static BinaryenModuleRef build_module(void) {
    BinaryenModuleRef module = BinaryenModuleCreate();
    if (module == NULL) return NULL;
    /* Explicit MVP avoids memory64 and optional proposal features. */
    BinaryenModuleSetFeatures(module, BinaryenFeatureMVP());
    BinaryenExpressionRef constant = BinaryenConst(module, BinaryenLiteralInt32(4));
    if (constant == NULL || BinaryenAddFunction(module,
            SOL_WASM_BACKEND_FUNCTION_ID, BinaryenTypeNone(),
            BinaryenTypeInt32(), NULL, 0, constant) == NULL) goto failed;
    BinaryenAddFunctionExport(module, SOL_WASM_BACKEND_FUNCTION_ID,
        SOL_WASM_BACKEND_PROBE_EXPORT);
    BinaryenSetMemory(module, 1, 1, NULL, NULL, NULL, NULL, NULL, NULL, 0,
        false, false, SOL_WASM_BACKEND_MEMORY_ID);
    BinaryenAddMemoryExport(module, SOL_WASM_BACKEND_MEMORY_ID,
        SOL_WASM_BACKEND_MEMORY_EXPORT);
    if (BinaryenAddTable(module, SOL_WASM_BACKEND_TABLE_ID, 1, 1,
            BinaryenTypeFuncref(), NULL) == NULL) goto failed;
    return module;
failed:
    BinaryenModuleDispose(module);
    return NULL;
}

SolWasmBackendResult sol_wasm_backend_build_probe(
    const SolMirTargetDescriptor *target, int optimized,
    SolWasmBackendBytes *output) {
    if (output == NULL) return SOL_WASM_BACKEND_INVALID_ARGUMENT;
    sol_wasm_backend_bytes_free(output);
    if (!exact_target(target)) return SOL_WASM_BACKEND_INVALID_TARGET;

    BinaryenModuleRef module = build_module();
    if (module == NULL) return SOL_WASM_BACKEND_BUILD_FAILED;
    SolWasmBackendResult result = SOL_WASM_BACKEND_IR_VALIDATION_FAILED;
    if (!binaryen_structure(module) || !BinaryenModuleValidate(module)) goto done;
    if (optimized != 0) {
        /* This local instruction pass preserves module namespaces and tables. */
        const char *passes[] = {"optimize-instructions"};
        BinaryenModuleRunPasses(module, passes, 1);
        if (!binaryen_structure(module) || !BinaryenModuleValidate(module)) {
            result = SOL_WASM_BACKEND_OPTIMIZATION_FAILED;
            goto done;
        }
    }
    BinaryenModuleAllocateAndWriteResult serialized =
        BinaryenModuleAllocateAndWrite(module, NULL);
    if (serialized.binary == NULL || serialized.binaryBytes == 0) {
        free(serialized.binary);
        result = SOL_WASM_BACKEND_SERIALIZATION_FAILED;
        goto done;
    }
    output->bytes = serialized.binary;
    output->count = serialized.binaryBytes;
    result = SOL_WASM_BACKEND_OK;
done:
    BinaryenModuleDispose(module);
    return result;
}

static bool wasm_name_equal(const wasm_name_t *name, const char *text) {
    size_t length = strlen(text);
    return name != NULL && name->size == length
        && memcmp(name->data, text, length) == 0;
}

static bool wasmtime_module_structure(const wasm_module_t *module) {
    wasm_importtype_vec_t imports;
    wasm_exporttype_vec_t exports;
    wasm_module_imports(module, &imports);
    wasm_module_exports(module, &exports);
    bool valid = imports.size == 0 && exports.size == 2
        && wasm_name_equal(wasm_exporttype_name(exports.data[0]),
            SOL_WASM_BACKEND_PROBE_EXPORT)
        && wasm_externtype_kind(wasm_exporttype_type(exports.data[0]))
            == WASM_EXTERN_FUNC
        && wasm_name_equal(wasm_exporttype_name(exports.data[1]),
            SOL_WASM_BACKEND_MEMORY_EXPORT)
        && wasm_externtype_kind(wasm_exporttype_type(exports.data[1]))
            == WASM_EXTERN_MEMORY;
    wasm_importtype_vec_delete(&imports);
    wasm_exporttype_vec_delete(&exports);
    return valid;
}

SolWasmBackendResult sol_wasm_backend_validate_and_run(
    const SolWasmBackendBytes *bytes) {
    if (bytes == NULL || bytes->bytes == NULL || bytes->count == 0)
        return SOL_WASM_BACKEND_INVALID_ARGUMENT;
    wasm_engine_t *engine = wasm_engine_new();
    wasm_store_t *store = engine == NULL ? NULL : wasm_store_new(engine);
    wasm_module_t *module = NULL;
    wasm_instance_t *instance = NULL;
    wasm_extern_vec_t exports = WASM_EMPTY_VEC;
    wasm_byte_vec_t input = {bytes->count, (wasm_byte_t *)bytes->bytes};
    SolWasmBackendResult result = SOL_WASM_BACKEND_RUNTIME_VALIDATION_FAILED;
    if (store == NULL || !wasm_module_validate(store, &input)) goto done;
    module = wasm_module_new(store, &input);
    if (module == NULL || !wasmtime_module_structure(module)) goto done;
    wasm_extern_vec_t imports = WASM_EMPTY_VEC;
    wasm_trap_t *trap = NULL;
    instance = wasm_instance_new(store, module, &imports, &trap);
    bool instantiation_trapped = trap != NULL;
    if (trap != NULL) wasm_trap_delete(trap);
    if (instance == NULL || instantiation_trapped) {
        result = SOL_WASM_BACKEND_RUNTIME_INSTANTIATION_FAILED;
        goto done;
    }
    wasm_instance_exports(instance, &exports);
    if (exports.size != 2 || wasm_extern_kind(exports.data[0]) != WASM_EXTERN_FUNC
        || wasm_extern_kind(exports.data[1]) != WASM_EXTERN_MEMORY
        || wasm_memory_size(wasm_extern_as_memory(exports.data[1])) != 1) {
        result = SOL_WASM_BACKEND_RUNTIME_INSTANTIATION_FAILED;
        goto done;
    }
    wasm_functype_t *type = wasm_func_type(wasm_extern_as_func(exports.data[0]));
    bool type_ok = type != NULL && wasm_functype_params(type)->size == 0
        && wasm_functype_results(type)->size == 1;
    if (type != NULL) wasm_functype_delete(type);
    if (!type_ok) { result = SOL_WASM_BACKEND_RUNTIME_INVOCATION_FAILED; goto done; }
    wasm_val_t value[1];
    wasm_val_vec_t arguments = WASM_EMPTY_VEC;
    wasm_val_vec_t values = WASM_ARRAY_VEC(value);
    trap = wasm_func_call(wasm_extern_as_func(exports.data[0]), &arguments, &values);
    bool invocation_trapped = trap != NULL;
    if (trap != NULL) wasm_trap_delete(trap);
    if (invocation_trapped || value[0].kind != WASM_I32 || value[0].of.i32 != 4) {
        result = SOL_WASM_BACKEND_RUNTIME_INVOCATION_FAILED;
        goto done;
    }
    result = SOL_WASM_BACKEND_OK;
done:
    wasm_extern_vec_delete(&exports);
    if (instance != NULL) wasm_instance_delete(instance);
    if (module != NULL) wasm_module_delete(module);
    if (store != NULL) wasm_store_delete(store);
    if (engine != NULL) wasm_engine_delete(engine);
    return result;
}
