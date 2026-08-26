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
      with bounded execution, explicit host capabilities, and executable contracts.
- [x] Deterministic `sol test` discovery and authored Boolean unit tests over owning IR.
- [x] Versioned bounded external inspection projections for syntax, HIR, types,
      effects, contracts, and diagnostics.
- [x] Whole-local affine moves, structural bootstrap copies, ownership joins,
      use-after-move diagnostics, and ownership-explicit interpreter reads.
- [x] Statement-only lexical regions, exact block/arm cleanup metadata,
      deterministic interpreter storage cleanup, and affine region-escape checks.
- [x] Initialized mutable whole-local bindings, checked statement assignment,
      ownership UPDATE metadata, replacement cleanup, and region-safe reinitialization.
- [x] Exhaustive syntax/owning-IR discriminant, span, arena-owner, executable-type,
      and callable-context validation, with per-kind censuses, composite relocation,
      inspection/formatter, and malformed-input fixtures.
- [x] Canonical typed owning-IR places with local/computed roots, flattened field
      projections, exact intermediate types/spans, and reserved index/dereference kinds.
- [x] Typed pure loop invariants and `decreases`, concrete divergence policy, and
      deterministic erased loop-obligation templates in owning IR.
- [x] Statement-form panic, proof-backed unreachable, and checked `require` guards
      with exact effects, ownership flow, cleanup, and reference-interpreter behavior.
- [x] Recursive wildcard/Boolean/binding/enum/record/tuple patterns, pure match guards,
      nested usefulness/exhaustiveness, and owning-IR/interpreter support.

## Construct Coverage

This ledger accounts for every current syntax category and the target-language
families already described by the README and design specification. Adding a new
surface form requires either a prioritized row below or an explicit deferral here.

| Surface | Implemented bootstrap coverage | Accounted remaining work |
| --- | --- | --- |
| Top-level structures | Edition-2027 modules/imports; records; enums; distinct/refined types; capabilities; functions; traits/implementations; tests; contracts; effects; stable annotations | Constants and associated constants (31); package manifests/features/visibility/re-exports/dependencies (40); versioned schemas/migrations (41); FFI declarations (46); `spec`/property declarations (51); public semantic IR (54); protocols/transactions/workflows (59); typed derives/reflection (60) |
| Statements | `let`; initialized or explicitly typed uninitialized `var`; local/field assignment and checked arithmetic compound assignment; owned `modify` scopes; payloadless `loop`/`while` with nearest `break`/`continue`, pure `invariant` lists, and one `decreases` measure; `panic Text`; proof-backed `unreachable`; `require Bool else Never`; `return`; expression statements; lexical `region` | Loop labels/values (29); obligation discharge (52); operational index/dereference mutation (34-35); allocator-bearing regions and `using` resource scopes (37); lexical unsafe blocks (44); protocol `emit` and concurrency control forms (59) |
| Expressions | Bootstrap primitive/unit/path literals; structural tuple literals and numeric projections; unary/binary operators; calls and type applications; fields/method calls; records/variants; direct checked distinct/refined construction; `if`; `match`; blocks; `?`; exact handlers; contract `result`/`old`; canonical local/computed-root places with flattened field/tuple projections | Operational arrays/indexing (34); pointer/reference dereference (35, 44); closures/method values (28-29); refined patterns/assumptions (50); unsafe pointer operations (44); async/protocol expressions (59); general resumptive handlers (61) |
| Patterns | Recursive wildcard, Boolean, binding, positional enum-variant, nominal-record, and structural-tuple patterns; pure guards; nested usefulness/exhaustiveness for `Bool`, records, tuples, and closed/open generic enums | Refined patterns (50); protocol-state patterns (59) |
| Types and callable structure | `Int64`, `Bool`, `Text`, `Unit`, and `Never`; structural tuples of arity 2 through 16; nominal applications; `Option`/`Result`; capability and structural function types; bounded type/effect parameters; one trait bound; `borrow`/`inout` parameters | Richer traits/generic methods and required associated items (30); constants and constrained const parameters (31, 33); remaining numeric/byte/rune primitives and units/dimensions (32); arrays/collections (34, 38); lifetime relationships/views (35); resource/allocator and cost types/clauses (36-38); general effect rows/aliases (42); raw pointers/ABI types (44, 46); concurrency traits/types (59) |
| Cross-cutting representation | Every successful-program AST statement/expression/pattern kind is parsed, structurally traversed, semantically analyzed, relocated, lowered, and executable or explicitly non-runtime; discriminants, spans, linked-arena ownership, current executable type relations, callable context, canonical place/projection ownership, per-kind censuses, and composite relocation fixtures are independently validated | Ownership/effect/contract integration for later forms (43); control-flow MIR and runtime lowering (45, 48) |

Unchecked exceptions and `throw`/`catch` are not planned; recoverable failure remains
typed through `Result`, and cancellation remains typed. Unrestricted token/text macros
remain a non-goal. Mutable globals and top-level initialization remain deferred until
an explicit authority, initialization-order, and concurrency model is approved.
Higher-kinded types, arbitrary variance, specialization, polymorphic recursion, and
general dependent typing remain deferred unless a concrete row below requires and
bounds them. Generic capabilities/members, trait defaults/contracts/authority clauses,
associated effects, inheritance, trait objects, and dependent method effects remain
deferred until a concrete core API requires their smallest coherent subset.

## Active End-to-End Track

The immediate goal is a bounded, useful application profile executed by the existing
owning IR and reference interpreter. Language breadth, MIR, and backend work are not
on this critical path. Existing numbered tasks remain stable identifiers; the `E`
milestones split the application-facing parts of tasks 40, 43, 48, 49, and 50 from
their larger production scopes.

| Done | Order | Milestone | Exit criteria | Source tasks |
| --- | ---: | --- | --- | --- |
| [x] | E1a | Create the shared compilation session and validated-IR handle | One API owns phase order, private frontend state, diagnostics, narrow value/render projections, and teardown; the production CLI and normal consumer tests use the session while phase-corruption and malformed-IR tests intentionally retain direct APIs; repeated execution consumes one truly opaque immutable validated-IR handle, and raw-IR host callbacks are rejected pending E3 | 43A; current pipeline |
| [x] | E1b | Harden source/package and host boundaries | Configurable package/compiler byte, file, directory depth/work, token, persistent-arena, diagnostic, cumulative allocation-byte, and allocation-count budgets fail deterministically; descriptor-relative discovery and descriptor reads verify regular opened identities and stable metadata; duplicate file identities and symbolic-link operands are rejected; raw host failures use interpreter-owned length-delimited text | hardening debt |
| [x] | E1c | Remove immediate scaling/release hazards | Generic recursion uses sparse adjacency plus iterative SCCs and effect inference uses packed SCC membership; opaque handles reuse final immutable-IR validation while raw mutable-IR APIs retain validation; warning-clean normal/sanitizer CI, Clang parser/package/IR fuzz smoke targets, and deterministic hashed stress baselines are checked in | E1a, E1b |
| [x] | E2 | Define the entrypoint and bounded application ABI | Argumentless `@entry` metadata reaches syntax, HIR, and owning IR; compilation permits zero or one while application resolution requires one; public nongeneric free-function visibility, owned root-capability parameters, explicit closed effects, `()`/`Int64` results, exact successful exit mapping, structured runtime failure, existing interpreter limits, and source/forged-IR diagnostics are specified and checked through the opaque validated handle | 40A, 49A |
| [x] | E3 | Define the trusted interpreter host profile | Opaque-handle entrypoint execution binds every required capability parameter to a distinct registry-owned root and exact bodyless-member allowlist; safe data-only callbacks expose no IR, callable, root, or private source; console output, bounded argument count/index access, optional deterministic configuration lookup, owned host failures, root-specific dispatch, preflight authority rejection, and default/configurable interpreter limits are specified and checked | 48A |
| [x] | E4 | Implement interpreter-based `sol run` | `sol run [options] <file-or-package> [-- arguments...]` compiles through the shared session, transfers owning IR and frees frontend state, resolves E2, grants only exact E3 standard profiles, preflights authority, executes under deterministic limits, preserves exact human console bytes, emits `sol.run-result` version 1 JSON with base64 console data, and maps successful/boundary/runtime outcomes to stable statuses and diagnostics | 49B |
| [x] | E5 | Execute core contracts and refinements | Interpreter policy checks `requires`, `old`, `ensures`, `Result` outcomes, and refined construction with exact cleanup and structured failures; loop runtime checks may be a follow-up | 50A |
| [x] | E6 | Freeze an executable-core conformance application | One multi-file application passes `fmt`, `check`, `test`, `effects`, `inspect`, and `run`, covering imports, ownership, effects, capabilities, contracts, errors, and cleanup | 51A; conformance |

The compilation API tests and production `check`, `test`, `run`, `effects`, and `inspect`
CLI integrations are the representative normal consumers for E1a. Package and
phase tests intentionally exercise lower-level frontend APIs, while declaration,
IR, and raw-interpreter tests retain direct IR access for malformed-IR mutation,
phase-corruption assertions, and trusted host-callback coverage.

The production track begins only after E6:

| Order | Milestone | Dependency |
| ---: | --- | --- |
| P1 | Introduce ownership-explicit CFG/MIR for the frozen executable core | E6, 45A |
| P2 | Define monomorphization, representation, target layout, symbols, and linkage | P1 |
| P3 | Define the target-independent runtime ABI and panic/cleanup policy | P1, P2, 48B |
| P4 | Integrate a WebAssembly backend and host adapter | P2, P3, 47W, 48C |
| P5 | Implement reproducible `sol build` artifacts and interpreter/Wasm differential tests | P4, 49C |

P1 is split into independently reviewable internal checkpoints; P1 and task 45 remain
open until the complete frozen executable core lowers:

| Done | Order | Checkpoint | Exit criteria |
| --- | ---: | --- | --- |
| [x] | P1a.1 | Establish callable-scoped CFG MIR invariants | An unstable target-neutral MIR owner lowers validated nongeneric free/test callables containing scalar literals, whole-local copy/move/store, unary/binary operations, blocks, `if`, return, and panic into deterministic basic blocks with SSA temporaries/block parameters, explicit parameter/local storage lifetime, conditional cleanup, transactional unsupported results, and independent structure/type/value/storage validation |
| [x] | P1a.2 | Add direct calls and remaining local control | Lower call-scoped shared/exclusive borrows, direct calls, short-circuit Boolean control, lexical regions, `require`, and proof-backed unreachable with explicit normal/failure and cleanup edges |
| [x] | P1a.3a | Add structured loop CFG | Lower `loop`, `while`, nearest-loop break/continue, condition-side transfers, natural backedges, lexical/region cleanup to loop boundaries, and erased loop-obligation metadata with independent loop/source/CFG validation |
| [x] | P1a.3b1 | Add bounded semantic value construction | Lower authority-free infallible zero/one-payload records, enum variants, Option/Result cases, and non-refined distinct wrappers without choosing layout, with exact constructor tags, source provenance, deterministic operand arenas, and transactional rejection of unsound forms |
| [x] | P1a.3b2 | Add owned temporary cleanup and multi-operand construction | Stage owned call/construction operands in typed reusable temporary slots, consume exact suffixes on invoke/construct, preserve outer temporaries across loop transfers, clean abandoned prefixes in interpreter order, and lower multi-operand records/tuples/enums with independent transition/provenance validation |
| [x] | P1a.3b3 | Add checked refined construction | Retain the exact type-owned predicate obligation in a target-neutral check terminator that consumes its staged representation on both outcomes, transports the nominal result only on success, and performs pending/local/region cleanup before resuming failure, with independent provenance and transition validation |
| [ ] | P1a.3b4 | Complete structured executable-core CFG lowering | Lower recursive matches/guards, propagation, projected places, handlers, capability/method/callback calls, contracts/snapshots, authority-bearing construction, and every E6 callable |
| [ ] | P1b | Freeze backend-facing value/CFG invariants | Complete dominance/SSA and ownership validation, generic/evidence retention, canonical internal rendering, and a bounded MIR evaluator/trace for interpreter differential semantics without choosing representation or ABI |

Items 28-39, 41-42, 44, 46, 50B+, and 52-61 are deferred until a
concrete E- or P-track requirement pulls in their smallest coherent subset.

## Numbered Capability Backlog

Numbers below are stable capability identifiers, not the immediate execution order.
The active order is defined by the E and P tracks above.

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
| [x] | 10 | Define an owning deterministic compiler-internal typed IR | 5 | 5 | Types, traits, and IDs |
| [x] | 11 | Implement an interpreter for language conformance tests | 5 | 4 | Typed IR |
| [x] | 12 | Add deterministic `sol test` and authored Boolean unit tests | 4 | 3 | Interpreter |
| [x] | 13 | Add `sol effects` authority and call-graph inspection | 3 | 2 | Effect tables |
| [x] | 14 | Stabilize selected external syntax, HIR, type, effect, contract, and diagnostic projections | 4 | 5 | Stable IDs and IR |
| [x] | 15 | Define and check affine moves, copies, and use-after-move errors | 5 | 5 | Typed IR |
| [x] | 16 | Implement lexical shared and exclusive borrows | 5 | 5 | Affine ownership |
| [x] | 17 | Track regions, deterministic cleanup, and resource lifetimes | 5 | 5 | Borrow checking |
| [x] | 18 | Add mutable local bindings, assignment, and checked whole-place updates | 5 | 5 | Exclusive borrows and cleanup |
| [x] | 19 | Make syntax and owning-IR validation exhaustive for every kind, span, owner, type relation, and callable result; add per-kind relocation, inspection, formatter, and malformed-input tests | 5 | 4 | Current executable baseline |
| [x] | 20 | Define canonical place/access-path IR for locals, fields, indices, and dereferences | 5 | 5 | Whole-local mutation |
| [x] | 21 | Implement projected loans, partial moves, projected mutation, `inout` caller writeback, and exact replacement cleanup | 5 | 5 | Place representation |
| [x] | 22 | Add definite initialization, uninitialized `var`, compound assignment, and explicit `modify` scopes | 4 | 4 | Projected mutation |
| [x] | 23 | Add `loop`, `while`, `break`, and `continue` with ownership fixed points and exact cleanup on every edge | 5 | 5 | Places and regions |
| [x] | 24 | Integrate loop invariants, `decreases`, divergence, and loop diagnostics with contract templates | 5 | 4 | Core loops and contracts |
| [x] | 25 | Define executable `panic`, proof-backed `unreachable`, and `require condition else` termination semantics | 4 | 4 | Control-flow effects and IR |
| [x] | 26 | Add structural tuples as the minimum local product form for patterns, iterators, and protocols | 4 | 4 | Type interning |
| [x] | 27 | Expand patterns to nested record/enum/tuple destructuring and pure guards | 4 | 4 | Tuples and exhaustiveness checking |
| [ ] | 28 | Define lambda syntax, capture classification, capture authority/effects, borrow escape, and closure ownership | 5 | 5 | Places, loans, and regions |
| [ ] | 29 | Implement nonescaping and owned closures plus method values across AST, HIR, IR, ownership, and interpreter | 5 | 5 | Closure semantics |
| [ ] | 30 | Add the bounded richer trait/generic subset required by core APIs: multiple bounds where needed, associated types, generic methods/implementations, and deterministic evidence | 5 | 5 | Existing traits and generics |
| [ ] | 31 | Add immutable top-level and associated constants with bounded constant evaluation; continue rejecting mutable statics and top-level initialization | 4 | 4 | Interpreter and type checking |
| [ ] | 32 | Define the remaining sized/unsigned/large integer, floating, decimal, byte, and rune primitives plus unit/dimension/currency types with checked conversions and arithmetic | 4 | 5 | Constants and nominal types |
| [ ] | 33 | Add only the constrained const parameters required for fixed array lengths and compile-time natural values | 4 | 5 | Constants and generics |
| [ ] | 34 | Add fixed arrays, array literals, indexing expressions/places, and structural ownership/equality | 4 | 4 | Const naturals and places |
| [ ] | 35 | Define lifetime parameters and borrow-escape relationships required by `View` and `Slice`, while continuing to infer ordinary local lifetimes | 5 | 5 | Partial loans and generics |
| [ ] | 36 | Define user resource traits and deterministic `Drop`/fallible `close`, including effect restrictions and cleanup precedence | 5 | 5 | Places, loops, and ownership |
| [ ] | 37 | Add allocator capabilities, allocator-bearing regions, `using` resource scopes, allocation effects, callable cost/resource clauses, and runtime/allocation profiles | 5 | 5 | Lifetimes and resource cleanup |
| [ ] | 38 | Implement the core collection layer required by examples: `Vector`, persistent `List`, explicit map variants, `View`, and `Slice` | 4 | 5 | Arrays, allocators, and lifetimes |
| [ ] | 39 | Define iterator traits and add protocol-based `for` loops | 4 | 4 | Richer traits, collections, and loops |
| [ ] | 40 | Complete package capabilities after E2's minimal application metadata: manifests, features/edition policy, dependencies/locks, visibility, aliases/re-exports, sandboxed builds, and dependency-qualified semantic IDs | 5 | 5 | E2 and existing package resolution |
| [ ] | 41 | Add versioned schema declarations, canonical schema identities, checked migration declarations, and deterministic migration planning | 4 | 5 | Constants, records, and packages |
| [ ] | 42 | Add bounded general effect-row polymorphism and effect aliases, including explicit/multiple/result-position arguments and authority capture | 5 | 5 | Existing effect parameters and closures |
| [ ] | 43 | Maintain cross-phase integration as acceptance criteria; E1 handles the current-surface audit/session/hardening, while later resource and control-flow facts integrate with the same diagnostics and inspection contracts | 5 | 5 | E1 and completed ownership surface |
| [ ] | 44 | Define lexical unsafe blocks, assumptions/establishments, raw pointer primitives, audit records, and unsafe effects | 5 | 5 | Places, lifetimes, resources, and obligations |
| [ ] | 45 | Introduce ownership-explicit control-flow MIR with blocks/SSA, moves, borrows, drops, regions, panic policy, generic lowering, and target-independent layout | 5 | 5 | Complete core control/resource semantics |
| [ ] | 46 | Define C ABI layouts and FFI declarations with ownership, nullability, threading, blocking, error, and effect metadata | 5 | 5 | Unsafe boundaries, packages, and MIR layout |
| [ ] | 47 | Select and integrate an established native or WebAssembly backend | 5 | 5 | Control-flow MIR |
| [ ] | 48 | Complete runtime ABI after E3's interpreter host profile: target-independent allocation, cleanup, panic, capabilities, handlers, FFI, and backend-specific adapters | 5 | 5 | E3, MIR, and backend |
| [ ] | 49 | Complete application tooling after E2/E4: backend `sol build`, artifact execution, target/profile selection, linkage, and reproducibility | 5 | 4 | E4, packages, backend, and runtime ABI |
| [ ] | 50 | Complete verification after E5's runtime core: normalized logical obligations, call-site substitution, refinement projection/destructuring/exhaustiveness, and cost/resource checks | 5 | 5 | E5, ownership, and resources |
| [ ] | 51 | Add `spec` examples, authored/generated properties, boundary generation, shrinking, seeds, and ghost state | 4 | 4 | Runtime checks and interpreter |
| [ ] | 52 | Integrate SMT proof policies, isolated solver execution, deterministic caching, counterexamples, cost proofs, and proof diagnostics | 4 | 5 | Logical obligation IR |
| [ ] | 53 | Complete formatter width reflow, trailing-comma policy, sorting, comment reflow, and syntax-category fixtures without semantic reordering | 3 | 3 | Grammar implemented through the current milestone |
| [ ] | 54 | Define the versioned public semantic graph and canonical serialized Sol IR separately from internal interpreter IR and MIR | 5 | 5 | Stable IDs and mature semantics |
| [ ] | 55 | Expose semantic information through a language server | 4 | 5 | Public schemas and graph |
| [ ] | 56 | Implement intent/semantic patch declarations and patch validation | 4 | 5 | Public IR |
| [ ] | 57 | Produce schema/API compatibility and semantic change reports, including migration requirements | 4 | 5 | Public IR, schemas, and patches |
| [ ] | 58 | Generate bounded context bundles for editor and agent workflows | 3 | 4 | Semantic graph |
| [ ] | 59 | Stage structured async/concurrency, `Send`/`Share`, cancellation, actors/channels, protocol-state patterns and `emit`, transactions, and workflows | 5 | 5 | Closures, resources, MIR, and runtime |
| [ ] | 60 | Stage typed derives, sandboxed build transforms, and opt-in reflection without unrestricted macros | 3 | 5 | Package sandboxing and public IR |
| [ ] | 61 | Generalize handlers after defining ownership across suspension and resumptions, multiple operations, dynamic authority matching, and row transformation | 5 | 5 | Effect polymorphism, concurrency, and runtime |

## Milestone Discipline

Each compiler increment should:

- preserve deterministic IDs and structured diagnostics;
- include focused positive, negative, and malformed-input tests;
- pass the full C17 warning-clean ASan/UBSan suite;
- update implementation-status documentation;
- be committed and pushed as an isolated checkpoint.
