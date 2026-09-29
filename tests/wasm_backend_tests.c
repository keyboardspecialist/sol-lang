#include "wasm_backend_internal.h"

#include "sol/mir_linkage.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #value); \
    ++failures; \
} } while (0)

bool sol_mir_linkage_test_sha256(const void *bytes, size_t length,
    SolMirLinkageDigest *digest);

static void test_namespaces(void) {
    CHECK(strcmp(SOL_WASM_BACKEND_LOGICAL_MODULE_ID, "sol.core.v1") == 0);
    CHECK(strcmp(SOL_WASM_BACKEND_RUNTIME_IMPORT_MODULE, "sol.runtime.v1") == 0);
    CHECK(strcmp(SOL_WASM_BACKEND_HOST_IMPORT_MODULE, "sol.host.v1") == 0);
    CHECK(strcmp(SOL_WASM_BACKEND_PROBE_EXPORT, "sol.probe.v1") == 0);
    CHECK(strcmp(SOL_WASM_BACKEND_MEMORY_EXPORT, "sol.memory.v1") == 0);
    CHECK(strcmp(SOL_WASM_BACKEND_TABLE_ID, "sol.table.v1") == 0);
    CHECK(strcmp(sol_wasm_backend_canonical_import_modules[0],
        SOL_WASM_BACKEND_RUNTIME_IMPORT_MODULE) == 0);
    CHECK(strcmp(sol_wasm_backend_canonical_import_modules[1],
        SOL_WASM_BACKEND_HOST_IMPORT_MODULE) == 0);
}

static void test_probe(int optimized) {
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    SolWasmBackendBytes first, second;
    sol_wasm_backend_bytes_init(&first);
    sol_wasm_backend_bytes_init(&second);
    SolWasmBackendResult built = sol_wasm_backend_build_probe(&target, optimized,
        &first);
    CHECK(built == SOL_WASM_BACKEND_OK);
    if (built != SOL_WASM_BACKEND_OK) goto done;
    CHECK(sol_wasm_backend_validate_and_run(&first) == SOL_WASM_BACKEND_OK);
    built = sol_wasm_backend_build_probe(&target, optimized, &second);
    CHECK(built == SOL_WASM_BACKEND_OK);
    if (built != SOL_WASM_BACKEND_OK) goto done;
    CHECK(first.count != 0 && first.count == second.count
        && memcmp(first.bytes, second.bytes, first.count) == 0);
    SolMirLinkageDigest digest;
    CHECK(sol_mir_linkage_test_sha256(first.bytes, first.count, &digest));
    static const unsigned char expected_raw[32] = {
        0x19, 0x35, 0x48, 0xfb, 0x23, 0x4d, 0x59, 0x9b,
        0x22, 0xcd, 0xc2, 0x69, 0xe9, 0x08, 0xd3, 0x0b,
        0xa5, 0x4f, 0xc2, 0xfa, 0x98, 0xea, 0x52, 0x15,
        0x85, 0x1e, 0xb6, 0xf6, 0x8b, 0x97, 0xc6, 0x74,
    };
    /* Generated with Binaryen 129 from the genuine exact-pinned installation. */
    CHECK(memcmp(digest.bytes, expected_raw, sizeof(expected_raw)) == 0);
done:
    sol_wasm_backend_bytes_free(&first);
    sol_wasm_backend_bytes_free(&second);
}

static void test_rejections(void) {
    SolMirTargetDescriptor target = sol_mir_target_wasm32();
    SolWasmBackendBytes bytes;
    sol_wasm_backend_bytes_init(&bytes);
    target.pointer_size = 8;
    CHECK(sol_wasm_backend_build_probe(&target, 0, &bytes)
        == SOL_WASM_BACKEND_INVALID_TARGET && bytes.bytes == NULL && bytes.count == 0);
    target = sol_mir_target_wasm32(); target.pointer_alignment = 8;
    CHECK(sol_wasm_backend_build_probe(&target, 0, &bytes)
        == SOL_WASM_BACKEND_INVALID_TARGET && bytes.bytes == NULL && bytes.count == 0);
    target = sol_mir_target_wasm32(); target.int64_alignment = 4;
    CHECK(sol_wasm_backend_build_probe(&target, 0, &bytes)
        == SOL_WASM_BACKEND_INVALID_TARGET && bytes.bytes == NULL && bytes.count == 0);
    target = sol_mir_target_wasm32(); target.endianness = SOL_MIR_ENDIAN_BIG;
    CHECK(sol_wasm_backend_build_probe(&target, 0, &bytes)
        == SOL_WASM_BACKEND_INVALID_TARGET && bytes.bytes == NULL && bytes.count == 0);
    target = sol_mir_target_wasm32(); target.max_object_bytes = UINT32_MAX - 1;
    CHECK(sol_wasm_backend_build_probe(&target, 0, &bytes)
        == SOL_WASM_BACKEND_INVALID_TARGET && bytes.bytes == NULL && bytes.count == 0);
    target = sol_mir_target_wasm32();
    SolWasmBackendResult built = sol_wasm_backend_build_probe(&target, 0, &bytes);
    CHECK(built == SOL_WASM_BACKEND_OK);
    if (built != SOL_WASM_BACKEND_OK) goto done;
    SolMirTargetDescriptor wrong = target;
    wrong.pointer_size = 8;
    CHECK(sol_wasm_backend_build_probe(&wrong, 0, &bytes)
        == SOL_WASM_BACKEND_INVALID_TARGET && bytes.bytes == NULL && bytes.count == 0);
    CHECK(sol_wasm_backend_build_probe(&target, 0, &bytes) == SOL_WASM_BACKEND_OK);
    bytes.bytes[0] ^= 0xff;
    CHECK(sol_wasm_backend_validate_and_run(&bytes)
        == SOL_WASM_BACKEND_RUNTIME_VALIDATION_FAILED);
    bytes.bytes[0] ^= 0xff;
    bytes.count = 1;
    CHECK(sol_wasm_backend_validate_and_run(&bytes)
        == SOL_WASM_BACKEND_RUNTIME_VALIDATION_FAILED);
done:
    sol_wasm_backend_bytes_free(&bytes);
    CHECK(sol_wasm_backend_build_probe(&target, 1, &bytes) == SOL_WASM_BACKEND_OK);
    sol_wasm_backend_bytes_free(&bytes);
}

int main(void) {
    test_namespaces();
    test_probe(0);
    test_probe(1);
    test_rejections();
    return failures == 0 ? 0 : 1;
}
