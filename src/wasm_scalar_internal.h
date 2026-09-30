#ifndef SOL_WASM_SCALAR_INTERNAL_H
#define SOL_WASM_SCALAR_INTERNAL_H

/* P4.2 is deliberately private.  In particular these names are not the
 * adapter ABI promised by a later checkpoint. */
#include "wasm_backend_internal.h"

#include "sol/diagnostic.h"
#include "sol/mir_runtime_lowered_program.h"

typedef enum {
    SOL_WASM_SCALAR_OK,
    SOL_WASM_SCALAR_INVALID_ARGUMENT,
    SOL_WASM_SCALAR_INVALID_INPUT,
    SOL_WASM_SCALAR_UNSUPPORTED_CLOSURE,
    SOL_WASM_SCALAR_RESOURCE_EXHAUSTED,
    SOL_WASM_SCALAR_ALLOCATION_FAILED,
    SOL_WASM_SCALAR_BINARYEN_VALIDATION_FAILED,
    SOL_WASM_SCALAR_SERIALIZATION_FAILED,
    SOL_WASM_SCALAR_WASMTIME_VALIDATION_FAILED,
} SolWasmScalarResult;

typedef struct {
    size_t max_functions;
    size_t max_blocks;
    size_t max_edges;
    size_t max_values;
    size_t max_locals;
    size_t max_generated_nodes;
    size_t max_provenance_records;
    size_t max_work_bytes;
    size_t max_scratch_bytes;
    size_t max_owned_bytes;
    size_t max_output_bytes;
} SolWasmScalarLimits;

typedef struct {
    size_t functions;
    size_t blocks;
    size_t edges;
    size_t values;
    size_t locals;
    size_t generated_nodes;
    size_t provenance_records;
    size_t work_bytes;
    size_t scratch_bytes;
    size_t owned_bytes;
    size_t output_bytes;
} SolWasmScalarUsage;

typedef struct {
    const SolMirRuntimeLoweredProgram *program;
    /* Required absolute, lexically normalized package directory. */
    const char *package_directory;
    /* NULL selects the finite P4.2 defaults.  Partial zero is rejected. */
    const SolWasmScalarLimits *limits;
} SolWasmScalarBuildRequest;

typedef struct {
    SolWasmBackendBytes bytes;
    SolWasmScalarUsage usage;
} SolWasmScalarOutput;

#define SOL_WASM_SCALAR_FAILURE_CODE_EXPORT "sol.p42.failure_code.unstable"
#define SOL_WASM_SCALAR_FAILURE_SITE_EXPORT "sol.p42.failure_site.unstable"
#define SOL_WASM_SCALAR_PROVENANCE_SECTION "sol.p42.provenance.v1"

/* Private `sol.p42.provenance.v1` payload: `P42P`, u32 version (=1), u32
 * record count, then canonical records sorted by (tag, path bytes, start, end,
 * symbol bytes, operation ordinal, kind).  A record is u8 tag, u8 kind, u16
 * reserved-zero, u32 path bytes, path, u32 start, u32 end, u32 symbol bytes,
 * symbol, u32 operation ordinal.  Tags are entry export, emitted callable,
 * and inherited failure site.  Paths are normalized package-relative UTF-8;
 * no dense P2/P3 IDs, absolute paths, pointers, or timestamps are encoded. */

void sol_wasm_scalar_output_init(SolWasmScalarOutput *output);
void sol_wasm_scalar_output_free(SolWasmScalarOutput *output);
SolWasmScalarLimits sol_wasm_scalar_default_limits(void);
SolWasmScalarResult sol_wasm_scalar_build_scalar(
    const SolWasmScalarBuildRequest *request, SolWasmScalarOutput *output,
    SolDiagnostics *diagnostics);
SolWasmScalarResult sol_wasm_scalar_validate_scalar(const SolWasmBackendBytes *bytes);

#ifdef SOL_MIR_PLAN_TEST_HOOKS
typedef struct {
    size_t calls;
    size_t edges;
    size_t work_bytes;
    /* Number of call edges in the longest acyclic chain; zero for a cyclic
     * graph because no finite recursive-depth classification is available. */
    size_t longest_chain;
    bool has_cycle;
} SolWasmScalarTestCallCatalog;

typedef struct {
    size_t caller_image, caller_block, call;
    size_t callee_callable, signature;
    size_t operand_count;
    SolMirMaterializedValueId result;
    SolMirRuntimeResultClass result_class;
    size_t normal_edge, failure_edge, failure_site;
    bool cyclic;
    size_t chain_depth;
} SolWasmScalarTestCallCatalogEntry;

/* Test-only authenticated catalog view.  It never emits a module and exposes
 * only IDs already named by the P3.6 owner.  `entries == NULL, cap == 0` is a
 * summary-only query; otherwise `cap` must cover every reported call. */
SolWasmScalarResult sol_wasm_scalar_test_call_catalog(
    const SolWasmScalarBuildRequest *, SolWasmScalarTestCallCatalogEntry *entries,
    size_t cap, SolWasmScalarTestCallCatalog *summary);
bool sol_wasm_scalar_test_call_catalog_operand(const SolWasmScalarBuildRequest *,
    size_t call, size_t ordinal, SolMirMaterializedTemporaryId *temporary);
void sol_wasm_scalar_test_fail_allocation_after(size_t attempt);
size_t sol_wasm_scalar_test_allocation_attempts(void);
/* Test-only Binaryen/Wasmtime proof of the backend's parallel-copy sequence. */
bool sol_wasm_scalar_test_parallel_moves(void);
/* Classifier-only hooks; these do not authenticate or expose raw MIR. */
bool sol_wasm_scalar_test_rejects_capture_snapshot(void);
bool sol_wasm_scalar_test_rejects_resume_failure(void);
bool sol_wasm_scalar_test_size_add_overflow(void);
#endif

#endif
