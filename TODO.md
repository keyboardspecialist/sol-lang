# Sol Compiler TODO

This list tracks concrete bootstrap-compiler work. The broader language and
toolchain phases remain documented in the [README](README.md#roadmap). Impact
and complexity use a 1-5 scale, where 5 is foundational or architectural.
Ordering accounts for dependencies rather than only the impact/complexity ratio.

## Completed Foundation

- [x] Closed normalized effect rows, recursive SCC inference, higher-order calls,
      callback checking, and exact function and bound-operation effects.
- [x] Capability provenance, authority-preserving returns, nominal single-source
      wrappers, exact handlers, and normalized finite mixed-root sets.
- [x] Structured contracts and resolved, typed, pure semantic obligation templates.
- [x] Reproducible Typst Design Specification v0.2 and generated official PDF.
- [x] Bounded first-order generic records, enums, and free functions with exact
      invariant applications and argument-only call inference.
- [x] Input-determined effect-row parameters on generic free functions with
      callback-driven least-row inference and per-call instantiation metadata.
- [x] Bounded coherent traits, exact closed implementations, one inline generic
      bound, and deterministic type-directed immediate method calls.
- [x] Deterministic directory packages, multi-file modules, explicit public imports,
      source-aware diagnostics, and package-wide semantic checking.
- [x] Token-preserving canonical formatting with checked/idempotent file and
      transactional directory rewrites.
- [x] Non-ambient effect authority: capability roots are lexical, while only
      `panic` and `diverge` are authority-free bootstrap atoms.
- [x] Nominal distinct types and refined declaration predicates with checked,
      pure, deterministic obligation templates.
- [x] Versioned stable top-level semantic identities, explicit rename/move tokens,
      collision validation, and resolved semantic occurrence records.
- [x] Owning deterministic compiler-internal typed IR with exact executable types,
      resolved dispatch/member metadata, normalized effects, and contract templates.
- [x] Deterministic compiler-internal reference interpreter over validated owning IR,
      with bounded execution, explicit host capabilities, and ignored contracts.
- [x] Deterministic `sol test` discovery and authored Boolean unit tests over owning IR.
- [x] Versioned bounded external inspection projections for syntax, HIR, types,
      effects, contracts, and diagnostics.
- [x] Whole-local affine moves, structural bootstrap copies, ownership joins,
      use-after-move diagnostics, and ownership-explicit interpreter reads.
- [x] Statement-only lexical regions, exact block/arm cleanup metadata,
      deterministic interpreter storage cleanup, and affine region-escape checks.
- [x] Initialized mutable whole-local bindings, checked statement assignment,
      ownership UPDATE metadata, replacement cleanup, and region-safe reinitialization.

## Prioritized Work

| Done | Order | Task | Impact | Complexity | Primary dependency |
| --- | ---: | --- | ---: | ---: | --- |
| [x] | 1 | Decide and restrict `success` and `failure` on non-`Result` contracts | 4 | 2 | Contract templates |
| [x] | 2 | Implement bounded first-order generic records/enums/functions and exact instantiation | 5 | 5 | Type interning |
| [x] | 3 | Integrate bounded effect-row variables with generic function instantiation | 5 | 5 | Generics |
| [x] | 4 | Add traits, implementations, constraints, and type-directed method resolution | 5 | 5 | Generics |
| [x] | 5 | Resolve modules and imports across multiple files and packages | 5 | 5 | Name resolution |
| [x] | 6 | Implement the canonical formatter and enforce idempotence | 4 | 3 | Parser and token stream |
| [x] | 7 | Close authority gaps for static and unparameterized effects | 5 | 4 | Capability model |
| [x] | 8 | Add distinct and refined types with checked predicates | 4 | 4 | Generics and contracts |
| [x] | 9 | Define stable semantic IDs, references, and rename identity | 5 | 5 | Modules and packages |
| [x] | 10 | Define a canonical typed Sol IR | 5 | 5 | Types, traits, and IDs |
| [x] | 11 | Implement an interpreter for language conformance tests | 5 | 4 | Typed IR |
| [x] | 12 | Add deterministic `sol test` and authored Boolean unit tests | 4 | 3 | Interpreter |
| [x] | 13 | Add `sol effects` authority and call-graph inspection | 3 | 2 | Effect tables |
| [x] | 14 | Stabilize selected external syntax, HIR, type, effect, contract, and diagnostic projections | 4 | 5 | Stable IDs and IR |
| [x] | 15 | Define and check affine moves, copies, and use-after-move errors | 5 | 5 | Typed IR |
| [x] | 16 | Implement lexical shared and exclusive borrows | 5 | 5 | Affine ownership |
| [x] | 17 | Track regions, deterministic cleanup, and resource lifetimes | 5 | 5 | Borrow checking |
| [x] | 18 | Add mutable local bindings, assignment, and checked whole-place updates | 5 | 5 | Exclusive borrows and cleanup |
| [ ] | 19 | Add `loop`, `while`, `break`, and `continue` with ownership fixed points and exact unwinding | 5 | 5 | Mutable places and regions |
| [ ] | 20 | Define iterator traits and add protocol-based `for` loops | 4 | 4 | Traits and core loops |
| [ ] | 21 | Add explicit unsafe boundaries and initial FFI rules | 4 | 4 | Ownership and effects |
| [ ] | 22 | Integrate ownership facts with effects, contracts, and diagnostics | 5 | 5 | Ownership pipeline |
| [ ] | 23 | Generate runtime contract checks and normalized proof obligations | 5 | 5 | IR and ownership |
| [ ] | 24 | Add examples, authored/generated properties, boundary generation, shrinking, seeds, and ghost state | 4 | 4 | Interpreter and obligations |
| [ ] | 25 | Integrate SMT proof policies, solver execution, and caching | 4 | 5 | Logical obligation IR |
| [ ] | 26 | Select and integrate an established native or WebAssembly backend | 5 | 5 | IR and ownership |
| [ ] | 27 | Implement package entry points and `sol build` / `sol run` | 5 | 4 | Modules and backend |
| [ ] | 28 | Expose semantic information through a language server | 4 | 5 | Stable schemas and IDs |
| [ ] | 29 | Implement semantic patches and patch validation | 4 | 5 | Stable IDs and public IR |
| [ ] | 30 | Produce API compatibility and semantic change reports | 4 | 5 | Public IR and patches |
| [ ] | 31 | Generate bounded context bundles for editor and agent workflows | 3 | 4 | Semantic graph |

## Milestone Discipline

Each compiler increment should:

- preserve deterministic IDs and structured diagnostics;
- include focused positive, negative, and malformed-input tests;
- pass the full C17 warning-clean ASan/UBSan suite;
- update implementation-status documentation;
- be committed and pushed as an isolated checkpoint.
