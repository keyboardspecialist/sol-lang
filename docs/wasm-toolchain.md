# P4.1 WebAssembly Toolchain

**Status:** P4.1 is complete in the current worktree (September 29, 2026).
This document is the technical authority for its selected tools, configuration
boundary, and probe contract. [TODO.md](../TODO.md) remains the sole live
execution-order ledger; P4 remains open and P4.2 is next.

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

Checkpoint evidence on Darwin arm64: OFF + Werror passed **54/54**; ON +
Werror passed the current full suite **56/56**; ON + ASan/UBSan passed
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

This is not P3.6 or CFG lowering; it selects no physical ABI, adapter, Sol
execution, production optimizer policy, artifact format, or `sol build`.
P4.2 must lower scalar CFG and ordinary calls; later P4 checkpoints own values,
checks/cleanup/handlers, and the trusted adapter/E6 execution.

## Provenance and release limits

The local artifact hash establishes what was verified for this Darwin checkpoint;
it does not create a repository lockfile, signed provenance record, reproducible
release artifact, or cross-host compatibility guarantee. P5 must define owned
build artifacts and target/profile identity, deterministic writes and metadata,
artifact execution, interpreter/Wasm differential conformance, and byte-identical
release acceptance across supported hosts with pinned-dependency provenance.
