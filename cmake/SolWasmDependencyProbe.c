#include <binaryen-c.h>
#include <wasm.h>

#include <stdint.h>
#include <stdlib.h>

int main(void) {
    BinaryenModuleRef module = BinaryenModuleCreate();
    if (module == NULL) return 10;
    BinaryenExpressionRef body = BinaryenConst(module, BinaryenLiteralInt32(4));
    if (body == NULL
        || BinaryenAddFunction(module, "probe", BinaryenTypeNone(),
            BinaryenTypeInt32(), NULL, 0, body) == NULL) {
        BinaryenModuleDispose(module);
        return 11;
    }
    BinaryenAddFunctionExport(module, "probe", "probe");
    if (!BinaryenModuleValidate(module)) {
        BinaryenModuleDispose(module);
        return 12;
    }
    BinaryenModuleAllocateAndWriteResult written =
        BinaryenModuleAllocateAndWrite(module, NULL);
    BinaryenModuleDispose(module);
    if (written.binary == NULL || written.binaryBytes == 0) return 13;

    wasm_engine_t *engine = wasm_engine_new();
    wasm_store_t *store = engine == NULL ? NULL : wasm_store_new(engine);
    wasm_byte_vec_t bytes = {written.binaryBytes, written.binary};
    if (store == NULL || !wasm_module_validate(store, &bytes)) {
        free(written.binary);
        if (store != NULL) wasm_store_delete(store);
        if (engine != NULL) wasm_engine_delete(engine);
        return 14;
    }
    wasm_module_t *compiled = wasm_module_new(store, &bytes);
    free(written.binary);
    if (compiled == NULL) {
        wasm_store_delete(store); wasm_engine_delete(engine);
        return 15;
    }
    wasm_extern_vec_t imports = WASM_EMPTY_VEC;
    wasm_trap_t *trap = NULL;
    wasm_instance_t *instance = wasm_instance_new(store, compiled, &imports, &trap);
    if (instance == NULL || trap != NULL) {
        if (trap != NULL) wasm_trap_delete(trap);
        wasm_module_delete(compiled); wasm_store_delete(store); wasm_engine_delete(engine);
        return 16;
    }
    wasm_extern_vec_t exports;
    wasm_instance_exports(instance, &exports);
    int result = 17;
    if (exports.size == 1 && wasm_extern_kind(exports.data[0]) == WASM_EXTERN_FUNC) {
        wasm_val_t value[1];
        wasm_val_vec_t args = WASM_EMPTY_VEC;
        wasm_val_vec_t values = WASM_ARRAY_VEC(value);
        trap = wasm_func_call(wasm_extern_as_func(exports.data[0]), &args, &values);
        if (trap == NULL && value[0].kind == WASM_I32 && value[0].of.i32 == 4) result = 0;
        if (trap != NULL) wasm_trap_delete(trap);
    }
    wasm_extern_vec_delete(&exports);
    wasm_instance_delete(instance); wasm_module_delete(compiled);
    wasm_store_delete(store); wasm_engine_delete(engine);
    return result;
}
