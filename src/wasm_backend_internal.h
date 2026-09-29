#ifndef SOL_WASM_BACKEND_INTERNAL_H
#define SOL_WASM_BACKEND_INTERNAL_H

#include "sol/mir_layout.h"

#include <stddef.h>
#include <stdint.h>

#define SOL_WASM_BACKEND_LOGICAL_MODULE_ID "sol.core.v1"
#define SOL_WASM_BACKEND_RUNTIME_IMPORT_MODULE "sol.runtime.v1"
#define SOL_WASM_BACKEND_HOST_IMPORT_MODULE "sol.host.v1"
#define SOL_WASM_BACKEND_PROBE_EXPORT "sol.probe.v1"
#define SOL_WASM_BACKEND_MEMORY_EXPORT "sol.memory.v1"
#define SOL_WASM_BACKEND_TABLE_ID "sol.table.v1"
#define SOL_WASM_BACKEND_IMPORT_MODULE_COUNT 2

extern const char *const sol_wasm_backend_canonical_import_modules[
    SOL_WASM_BACKEND_IMPORT_MODULE_COUNT];

typedef enum {
    SOL_WASM_BACKEND_OK,
    SOL_WASM_BACKEND_INVALID_ARGUMENT,
    SOL_WASM_BACKEND_INVALID_TARGET,
    SOL_WASM_BACKEND_BUILD_FAILED,
    SOL_WASM_BACKEND_IR_VALIDATION_FAILED,
    SOL_WASM_BACKEND_OPTIMIZATION_FAILED,
    SOL_WASM_BACKEND_SERIALIZATION_FAILED,
    SOL_WASM_BACKEND_RUNTIME_VALIDATION_FAILED,
    SOL_WASM_BACKEND_RUNTIME_INSTANTIATION_FAILED,
    SOL_WASM_BACKEND_RUNTIME_INVOCATION_FAILED,
} SolWasmBackendResult;

typedef struct {
    uint8_t *bytes;
    size_t count;
} SolWasmBackendBytes;

void sol_wasm_backend_bytes_init(SolWasmBackendBytes *bytes);
void sol_wasm_backend_bytes_free(SolWasmBackendBytes *bytes);

/* Builds only the fixed P4.1 probe; it does not translate Sol MIR or CFGs. */
SolWasmBackendResult sol_wasm_backend_build_probe(
    const SolMirTargetDescriptor *target, int optimized,
    SolWasmBackendBytes *output);
SolWasmBackendResult sol_wasm_backend_validate_and_run(
    const SolWasmBackendBytes *bytes);

#endif
