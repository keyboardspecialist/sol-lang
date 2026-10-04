#ifndef SOL_WASM_REPRESENTED_INTERNAL_H
#define SOL_WASM_REPRESENTED_INTERNAL_H

/* P4.3 is deliberately private.  These names are not a host adapter ABI. */
#include "wasm_backend_internal.h"

#include "sol/diagnostic.h"
#include "sol/mir_runtime_lowered_program.h"

typedef enum {
    SOL_WASM_REPRESENTED_OK,
    SOL_WASM_REPRESENTED_INVALID_ARGUMENT,
    SOL_WASM_REPRESENTED_INVALID_INPUT,
    SOL_WASM_REPRESENTED_UNSUPPORTED_CLOSURE,
    SOL_WASM_REPRESENTED_RESOURCE_EXHAUSTED,
    SOL_WASM_REPRESENTED_ALLOCATION_FAILED,
    SOL_WASM_REPRESENTED_BINARYEN_VALIDATION_FAILED,
    SOL_WASM_REPRESENTED_SERIALIZATION_FAILED,
    SOL_WASM_REPRESENTED_WASMTIME_VALIDATION_FAILED,
} SolWasmRepresentedResult;

typedef struct {
    /* Complete request values must not exceed the private P4.3 wire cap (256). */
    size_t max_functions;
    size_t max_blocks;
    size_t max_edges;
    size_t max_values;
    size_t max_locals;
    size_t max_generated_nodes;
    /* Private P4.3 wire cap is 256; callbacks currently reserve a sentinel. */
    size_t max_table_elements;
    size_t max_static_data_bytes;
    uint64_t max_allocation_requests;
    uint64_t max_allocation_bytes;
    size_t max_provenance_records;
    size_t max_work_bytes;
    size_t max_scratch_bytes;
    size_t max_owned_bytes;
    size_t max_output_bytes;
} SolWasmRepresentedLimits;

typedef struct {
    size_t functions;
    size_t blocks;
    size_t edges;
    size_t values;
    size_t locals;
    size_t generated_nodes;
    size_t table_elements;
    size_t static_data_bytes;
    uint64_t allocation_requests;
    uint64_t allocation_bytes;
    size_t provenance_records;
    size_t work_bytes;
    size_t scratch_bytes;
    size_t owned_bytes;
    size_t output_bytes;
} SolWasmRepresentedUsage;

typedef struct {
    const SolMirRuntimeLoweredProgram *program;
    /* Required absolute, lexically normalized package directory. */
    const char *package_directory;
    /* NULL or a wholly zero value selects finite defaults. Partial zero is rejected. */
    const SolWasmRepresentedLimits *limits;
} SolWasmRepresentedBuildRequest;

typedef struct {
    SolWasmBackendBytes bytes;
    SolWasmRepresentedUsage usage;
} SolWasmRepresentedOutput;

#define SOL_WASM_REPRESENTED_FAILURE_CODE_EXPORT "sol.p43.failure_code.unstable"
#define SOL_WASM_REPRESENTED_FAILURE_SITE_EXPORT "sol.p43.failure_site.unstable"
/* Present iff the authenticated provenance contains an image PANIC site.
 * These are deliberately unstable private represented-Wasm observability, not
 * a host adapter ABI. Offset is immutable; length is the captured byte count
 * and is reset by every entry. */
#define SOL_WASM_REPRESENTED_PANIC_DETAIL_OFFSET_EXPORT "sol.p44.panic_detail_offset.unstable"
#define SOL_WASM_REPRESENTED_PANIC_DETAIL_LENGTH_EXPORT "sol.p44.panic_detail_length.unstable"
#define SOL_WASM_REPRESENTED_TEST_WRITEBACK_EXPORT "sol.p43.test.writebacks"
#define SOL_WASM_REPRESENTED_TEST_FAILURE_ENTRY_EXPORT "sol.p43.test.c2b-failure"
#define SOL_WASM_REPRESENTED_TEST_CLEANUP_OLD_CALLABLE_EXPORT "sol.p43.test.cleanup.old-callable"
#define SOL_WASM_REPRESENTED_TEST_CLEANUP_MOVED_CALLABLE_EXPORT "sol.p43.test.cleanup.moved-callable"
#define SOL_WASM_REPRESENTED_TEST_CLEANUP_TEXT_SIBLING_EXPORT "sol.p43.test.cleanup.text-sibling"
#define SOL_WASM_REPRESENTED_TEST_CLEANUP_ROOT_EXPORT "sol.p43.test.cleanup.root"
#define SOL_WASM_REPRESENTED_TEST_P44_PACKET_RESET_SUCCESS_EXPORT "sol.p44.test.packet-reset-success"
#define SOL_WASM_REPRESENTED_TEST_P44_PACKET_RESET_NONPANIC_EXPORT "sol.p44.test.packet-reset-nonpanic"
#define SOL_WASM_REPRESENTED_PROVENANCE_SECTION "sol.p43.provenance.v1"

/* Private `sol.p43.provenance.v1` payload: `P43P`, u32 version (=1), u32
 * record count, then canonical records sorted by (tag, path bytes, start, end,
 * symbol bytes, operation ordinal, kind).  A record is u8 tag, u8 kind, u16
 * reserved-zero, u32 path bytes, path, u32 start, u32 end, u32 symbol bytes,
 * symbol, u32 operation ordinal.  Tags are entry export, emitted callable,
 * inherited failure site, and supplemental allocation site.  Paths are normalized package-relative UTF-8;
 * no dense P2/P3 IDs, absolute paths, pointers, or timestamps are encoded. */

void sol_wasm_represented_output_init(SolWasmRepresentedOutput *output);
void sol_wasm_represented_output_free(SolWasmRepresentedOutput *output);
SolWasmRepresentedLimits sol_wasm_represented_default_limits(void);
SolWasmRepresentedResult sol_wasm_represented_build(
    const SolWasmRepresentedBuildRequest *request, SolWasmRepresentedOutput *output,
    SolDiagnostics *diagnostics);
SolWasmRepresentedResult sol_wasm_represented_validate(const SolWasmBackendBytes *bytes);

#ifdef SOL_MIR_PLAN_TEST_HOOKS
typedef struct {
    size_t calls;
    size_t edges;
    size_t work_bytes;
    /* Number of call edges in the longest acyclic chain; zero for a cyclic
     * graph because no finite recursive-depth classification is available. */
    size_t longest_chain;
    bool has_cycle;
} SolWasmRepresentedTestCallCatalog;

typedef struct {
    size_t caller_image, caller_block, call;
    size_t callee_callable, signature;
    size_t operand_count;
    SolMirMaterializedValueId result;
    SolMirRuntimeResultClass result_class;
    size_t normal_edge, failure_edge, failure_site;
    bool cyclic;
    size_t chain_depth;
} SolWasmRepresentedTestCallCatalogEntry;

typedef enum {
    SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_INVALID,
    SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_EVENTLESS,
    SOL_WASM_REPRESENTED_TEST_CLEANUP_MARKER_ACTION,
} SolWasmRepresentedTestCleanupMarkerRoute;

/* Test-only authenticated catalog view.  It never emits a module and exposes
 * only IDs already named by the P3.6 owner.  `entries == NULL, cap == 0` is a
 * summary-only query; otherwise `cap` must cover every reported call. */
SolWasmRepresentedResult sol_wasm_represented_test_call_catalog(
    const SolWasmRepresentedBuildRequest *, SolWasmRepresentedTestCallCatalogEntry *entries,
    size_t cap, SolWasmRepresentedTestCallCatalog *summary);
bool sol_wasm_represented_test_call_catalog_operand(const SolWasmRepresentedBuildRequest *,
    size_t call, size_t ordinal, SolMirMaterializedTemporaryId *temporary);
void sol_wasm_represented_test_fail_allocation_after(size_t attempt);
size_t sol_wasm_represented_test_allocation_attempts(void);
/* Test-only Binaryen/Wasmtime proof of the backend's parallel-copy sequence. */
bool sol_wasm_represented_test_parallel_moves(void);
/* Classifier-only hooks; these do not authenticate or expose raw MIR. */
bool sol_wasm_represented_test_rejects_capture_snapshot(void);
bool sol_wasm_represented_test_rejects_resume_failure(void);
bool sol_wasm_represented_test_size_add_overflow(void);
/* Exercises the bounded sum-helper body builder at the exact arm count. */
bool sol_wasm_represented_test_sum_helper_items(size_t variants);
/* Constrains the physical Wasm memory and heap cursor for a focused allocator
 * failure proof.  Zero restores production defaults. */
void sol_wasm_represented_test_allocator_memory(size_t max_pages, uint32_t heap_base);
/* UINT64_MAX selects the build limit for either dimension. */
void sol_wasm_represented_test_allocator_quota(uint64_t max_requests, uint64_t max_bytes);
/* Test-only module probe: emit private inactive-payload equality controls.
 * Disabled by default and absent from non-test builds. */
void sol_wasm_represented_test_inactive_payload_probe(bool enabled);
/* Emits a test-only commit counter for authenticated C2b writeback actions.
 * Disabled by default; it is neither a production ABI nor part of frozen bytes. */
void sol_wasm_represented_test_callback_writeback_probe(bool enabled);
/* Emits hook-only counters for the admitted callable-product cleanup path.
 * Disabled by default; no production module bytes or exports depend on it. */
void sol_wasm_represented_test_callable_hole_cleanup_probe(bool enabled);
/* Emits two test-only P4.4 packet-reset probes. Both run the exact entry
 * reset prologue; the second publishes code 2 with the test sentinel site 0.
 * Disabled by default and absent from production module bytes. */
void sol_wasm_represented_test_p44_packet_reset_probe(bool enabled);
/* Runs B1's selector without the outer whole-owner validation, so hostile
 * owner mutations can exercise the selector itself. `transition` and `actions`
 * name borrowed P3.3 records on success. */
bool sol_wasm_represented_test_control_transition(const SolWasmRepresentedBuildRequest *,
    size_t block, SolMirRuntimeCleanupEdgeRole role, size_t *transition,
    SolMirRuntimeSlice *actions);
/* Runs the explicit cleanup-marker selector alone.  A successful action is an
 * index into the authenticated cleanup action arena; eventless has no action. */
SolWasmRepresentedTestCleanupMarkerRoute sol_wasm_represented_test_cleanup_marker(
    const SolWasmRepresentedBuildRequest *, size_t instruction, size_t *action);
#endif

#endif
