# Sol Compiler TODO

This list tracks concrete bootstrap-compiler work. The broader language and
toolchain phases remain documented in the [README](README.md#roadmap).

## P0: Effect System

- [ ] Infer recursive effect rows with a least fixed point over call-graph SCCs.
- [ ] Represent general function types with semantic effect rows.
- [ ] Check higher-order calls, callbacks, and function values with declared effects.
- [ ] Model restricted and wrapped capabilities without losing authority provenance.
- [ ] Define and implement effect handlers and handled-row subtraction.
- [ ] Decide how dynamic or mixed authority provenance is represented beyond the
      current same-origin `if` and `match` joins.

## P0: Front-End Semantics

- [ ] Parse contracts into structured syntax instead of balanced blocks.
- [ ] Lower `requires`, `ensures`, `old`, and `result` into semantic obligations.
- [ ] Implement user-defined generic types and generic function instantiation.
- [ ] Add traits, implementations, constraints, and type-directed method resolution.
- [ ] Add distinct and refined types with checked refinement predicates.
- [ ] Resolve modules and imports across multiple source files and packages.

## P1: Ownership And Resources

- [ ] Define and check affine moves, copies, and use-after-move errors.
- [ ] Implement lexical shared and exclusive borrows.
- [ ] Track regions, deterministic cleanup, and resource lifetimes.
- [ ] Add explicit unsafe boundaries and initial FFI rules.
- [ ] Integrate ownership facts with effects, contracts, and diagnostics.

## P1: Execution

- [ ] Define a canonical typed Sol IR.
- [ ] Implement an interpreter suitable for language and conformance tests.
- [ ] Add `sol test` for examples, unit tests, and properties.
- [ ] Select and integrate an established native or WebAssembly backend.
- [ ] Implement package entry points and `sol build` / `sol run`.

## P1: Canonical Tooling

- [ ] Implement the canonical formatter and enforce idempotence.
- [ ] Stabilize serialized syntax, HIR, type, effect, and diagnostic schemas.
- [ ] Add `sol effects` authority and call-graph inspection.
- [ ] Expose semantic information through a language server.
- [ ] Add stable semantic IDs, references, and rename support.

## P2: Verification And Change Tooling

- [ ] Generate runtime checks and proof obligations from contracts.
- [ ] Integrate an SMT solver with configurable proof policies and caching.
- [ ] Add examples, properties, generated boundary tests, and ghost state.
- [ ] Implement semantic patches and patch validation.
- [ ] Produce API compatibility and semantic change reports.
- [ ] Generate bounded context bundles for editor and agent workflows.

## Milestone Discipline

Each compiler increment should:

- preserve deterministic IDs and structured diagnostics;
- include focused positive, negative, and malformed-input tests;
- pass the full C17 warning-clean ASan/UBSan suite;
- update implementation-status documentation;
- be committed and pushed as an isolated checkpoint.
