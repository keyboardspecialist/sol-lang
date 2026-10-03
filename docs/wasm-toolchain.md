# P4.1-P4.3 WebAssembly Toolchain

**Status:** P4.1 through P4.3 are complete in the current worktree (October 3, 2026).
This document is the technical authority for the selected tools, configuration
boundary, probe, and scalar-emitter contracts. [TODO.md](../TODO.md) remains the
sole live execution-order ledger; P4 remains open and P4.4 is next.

## Selected tools and boundary

P4.1 uses **Binaryen 129 C API** to construct the probe module, validate it,
serialize deterministic bytes, and run exactly one explicit
`optimize-instructions` pass for the optimized probe. It uses the independent
**Wasmtime 49.0.1 C API** to validate the serialized bytes, instantiate the
module, and invoke its exported function. This is deliberately not a single
library validating its own output.

The backend is opt-in:

- `SOL_ENABLE_WASM_BACKEND=OFF` is the default.
- With `ON`, `SOL_BINARYEN_ROOT` and `SOL_WASMTIME_ROOT` must each name an
  existing **absolute** installation root.
- Discovery is only below those roots (`include`, `lib`, and Binaryen `bin`),
  with `NO_DEFAULT_PATH`; it does not search `PATH`, system locations, or
  other CMake roots. It does not download or vendor either dependency.
- Every discovered header, library, and executable is canonicalized and must
  remain under its selected canonical root. A root may itself be a symlink;
  an item escaping it through a symlink is rejected. Cached discovery inputs
  are cleared, so they cannot inject a different root or mix installations.
- Binaryen is accepted only when the selected `wasm-opt --version` is exactly
  `wasm-opt version 129`. Wasmtime is accepted only when its selected
  `wasmtime.h` reports exactly `WASMTIME_VERSION` `49.0.1` and major/minor/patch
  `49`/`0`/`1`.
- Cross compilation is rejected: configuration must compile, link, and run a
  compatibility probe. That probe uses Binaryen serialization followed by
  Wasmtime validation, instantiation, and a call returning `4`. Any failure is
  a configure-time fatal diagnostic.

These are repository selection and containment rules, not a claim that a local
installation root is an endorsed distribution channel or a stable public ABI.
The verified host was **Darwin arm64**. Its Binaryen root was
`/opt/homebrew/opt/binaryen`; that is one local root used for this checkpoint,
not repository policy. The Wasmtime input was the official C API release
artifact `wasmtime-v49.0.1-aarch64-macos-c-api.tar.xz`, verified as SHA-256
`5dd2eb69091572ff7d6cda38810dc63abd779e7de00025978541578931056de3`.

## Configuration and validation

Default (backend disabled):

```sh
cmake -S . -B build-off -G Ninja -DSOL_ENABLE_WERROR=ON
cmake --build build-off
ctest --test-dir build-off --output-on-failure
```

Opt in with explicitly selected roots:

```sh
cmake -S . -B build-wasm -G Ninja \
  -DSOL_ENABLE_WERROR=ON \
  -DSOL_ENABLE_WASM_BACKEND=ON \
  -DSOL_BINARYEN_ROOT=/absolute/path/to/binaryen-129 \
  -DSOL_WASMTIME_ROOT=/absolute/path/to/wasmtime-49.0.1
cmake --build build-wasm
ctest --test-dir build-wasm --output-on-failure
```

Historical P4.1 checkpoint evidence on Darwin arm64: OFF + Werror passed **54/54**; ON +
Werror passed the then-current full suite **56/56**; ON + ASan/UBSan passed
**56/56**. The full ON Werror suite was rerun after later corrections limited
to the CMake rejection fixture/root gate. Configure-negative coverage rejects
missing Binaryen/Wasmtime roots, wrong Binaryen/Wasmtime versions, a symlink
escape, and cache-injected discovery paths. The Linux fixture is portable by
construction, but neither this checkpoint nor its independent review executed
it on Linux. `git diff --check` passed; independent final review approved.

## Frozen probe namespace and module contract

The logical module identity is `sol.core.v1`. The canonical future import
module order is first `sol.runtime.v1`, then `sol.host.v1`; the fixed probe has
**no imports**. Its exports are `sol.probe.v1` and `sol.memory.v1`; its
internal table name is `sol.table.v1`. This canonical order is a naming/order
policy only: imports grant names, never authority. There are no WASI, `env`, or
intrinsic imports.

The probe is core Wasm32/MVP only: no imports or start function, one bounded
non-shared non-memory64 memory (initial and maximum one page), one funcref
table (initial and maximum one; index 0 is null), and one `[] -> [i32]`
function returning `4`. It has no element segments. Raw and optimized output
are independently built twice and byte-for-byte deterministic; the frozen
probe SHA-256 is
`193548fb234d599b22cdc269e908d30ba54fc2fa98ea5215851eb6f68b97c674`.

This P4.1 probe is not P3.6 or CFG lowering; it selects no physical ABI, adapter, Sol
execution, production optimizer policy, artifact format, or `sol build`.

## Provenance and release limits

The local artifact hash establishes what was verified for this Darwin checkpoint;
it does not create a repository lockfile, signed provenance record, reproducible
release artifact, or cross-host compatibility guarantee. P5 must define owned
build artifacts and target/profile identity, deterministic writes and metadata,
artifact execution, interpreter/Wasm differential conformance, and byte-identical
release acceptance across supported hosts with pinned-dependency provenance.


## P4.2 scalar-emitter contract

P4.2 uses the P4.1 opt-in Binaryen 129/Wasmtime 49.0.1 toolchain to emit and validate authenticated P3.6 **whole scalar direct-call closures**. It supports `Int64`, `Bool`, and Unit constants and parameters; SSA values/block parameters and parallel edges; whole-local and temporary lifetime; arbitrary dispatcher CFG and loops; infallible plus checked unary/binary/compound operations with no Wasm traps; direct ordinary internal owned-scalar calls; VALUE/Unit normal/failure dispatch; and returns.

The module keeps exact P2 `sol.i1` definitions internally. Only exact P2 `sol.e1` functions are externally callable exports; no `sol.i1` function exports exist. The module additionally exports exactly two explicitly unstable mutable diagnostic globals for private packet observation, not as a stable/public ABI. A private entry wrapper resets those globals once. This is a **private provisional scalar convention**, not a stable/public physical ABI or a host adapter. Provenance is canonical package-relative `sol.p42.provenance.v1`; a nested leaf failure packet's code/site survives callers. Cycles and call chains over 64 are rejected statically. There is no runtime depth-code-7 emission: P4.4 owns runtime checks. The P3.3 `LOCAL_OR_PENDING` prerequisite permits only authenticated future caller-local code 7 or an unchanged pending callee packet, and only on ordinary image direct-function `INVOKE`; host, indirect, capability, and predicate calls remain excluded.

P4.2 historically excluded represented values, places/projections, indirect calls/callbacks/receivers/writeback, and patterns/propagation. P4.3 completes its bounded represented-emitter scope below. P4.4 owns general runtime checks, non-total/guarded-match failure, contracts/refinements/snapshots, complete cleanup/unwind/handlers/provider dispatch, and runtime depth/step codes 6/7; P4.5 owns the host adapter/E6; P5 owns artifacts and `sol build`.

### Limits and validation

Backend limits are exact and have exact, one-below, and partial-zero tests; backend-owned allocation sweeps and rollback are covered. Binaryen and Wasmtime private allocations are outside Sol quotas, while serialized output bytes are metered. The narrow P3.2 prerequisite adds only the no-import/nonzero-record scratch-allocation event: exact work 23 succeeds and one-below work 22 exhausts; import-bearing E6 frozen build work remains 357. This is not a broader P3.2 semantic change. The representative nested-overflow fixture uses `(functions, blocks, edges, values, locals, generated nodes, provenance records, work bytes, scratch bytes, owned bytes, output bytes)` = `(4, 7, 4, 7, 13, 146, 7, 4115, 1703, 1703, 1504)` with SHA-256 `6a75a3627de544741885fcd12aad0ef27237e9d7d9f3c616f8ceb181f115ddb4`.

Final verification on Darwin arm64: OFF Debug Werror **54/54**; ON Debug Werror **57/57**; ON RelWithDebInfo ASan/UBSan Werror **57/57**. `git diff --check` passed and independent final review approved. No Linux execution is claimed.

## P4.3 represented-emitter contract

P4.3 is a private opt-in Binaryen 129/Wasmtime 49.0.1 Wasm32 represented emitter over authenticated P3.6, with canonical package-relative provenance `sol.p43.provenance.v1`. It emits bounded Text; finite fixed products/tuples, sums, distinct wrappers, and Unit fields; constructors/projections; deep copy, equality, and logical cleanup; guard-free total patterns and Copy extraction; Option/Result propagation; a private sentinel `funcref` table with exact unbound internal callbacks; whole-local scalar `inout` callback normal-only writeback; immediate shared/exclusive `Int64` methods; and exact callable-plus-Text product projected moves, conditional holes, repair/reopen, intact whole-root moves, and hole-aware cleanup.

It preserves deterministic bump memory, exact runtime request/byte quotas, code 5 resource failure before code 4 physical allocation failure with canonical supplemental sites, exact resource/fault rollback, deterministic module validation, bounded/capped raw parsing, and source-backed mutations. Bound stored trait-method values remain intentionally unsupported under the language immediate-method rule; broader represented shapes and callable signatures fail closed. It adds no host/capability adapter or E6 execution and remains a private provisional convention, not a stable public ABI.

Validation on Darwin arm64: OFF Debug Werror **54/54**; ON Debug Werror **58/58**; the current ON ASan/UBSan Werror represented-emitter test and focused MIR tests passed. This does not claim a current full ASan suite or Linux execution.
