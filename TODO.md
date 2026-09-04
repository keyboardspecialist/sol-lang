# Sol Compiler TODO

This list tracks concrete bootstrap-compiler work. The broader language and
toolchain phases remain documented in the [README](README.md#roadmap). Impact
and complexity use a 1-5 scale, where 5 is foundational or architectural.
Ordering accounts for dependencies rather than only the impact/complexity ratio.

## Completed Foundation Through E6 and P1

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
- [x] Shared bounded compilation sessions, opaque validated-IR transfer, explicit
      application entrypoints, trusted root/member host registration, interpreter-based
      `sol run`, runtime callable contracts/refinements, and the E6 conformance application.
- [x] Separate unstable target-neutral callable-scoped CFG MIR for every bodyful E6
      callable, with SSA/dominance and affine-value validation, canonical internal
      rendering, and bounded MIR-vs-owning-IR differential evaluation. Representation,
      runtime ABI, backend integration, and production-pipeline use remain open.

## Construct Coverage

This ledger accounts for every current syntax category and the target-language
families already described by the README and design specification. Adding a new
surface form requires either a prioritized row below or an explicit deferral here.

| Surface | Implemented bootstrap coverage | Accounted remaining work |
| --- | --- | --- |
| Top-level structures | Edition-2027 modules/imports; records; enums; distinct/refined types; capabilities; functions; traits/implementations; tests; contracts; effects; stable annotations | Constants and associated constants (31); package manifests/features/visibility/re-exports/dependencies (40); versioned schemas/migrations (41); FFI declarations (46); `spec`/property declarations (51); public semantic IR (54); protocols/transactions/workflows (59); typed derives/reflection (60) |
| Statements | `let`; initialized or explicitly typed uninitialized `var`; local/record-field/tuple-projection assignment and checked arithmetic compound assignment; owned `modify` scopes; statement-only `loop`/`while` with payloadless nearest-loop `break`/`continue`, pure `invariant` lists, and one `decreases` measure; `panic Text`; proof-backed `unreachable`; `require Bool else Never`; `return`; expression statements; lexical `region` | Loop labels/values (explicitly deferred; no numbered task); logical obligation normalization/substitution (50) and SMT-backed discharge (52); array indexing expressions/places (34); lifetime-bearing safe views (35, 38); raw-pointer dereference and mutation (44); allocator-bearing regions and `using` resource scopes (37); lexical unsafe blocks (44); protocol `emit` and concurrency control forms (59) |
| Expressions | Bootstrap primitive/unit/path literals; structural tuple literals and numeric projections; unary/binary operators; calls and type applications; fields/method calls; records/variants; direct checked distinct/refined construction; `if`; `match`; blocks; `?`; exact handlers; contract `result`/`old`; canonical local/computed-root places with flattened field/tuple projections | Operational arrays/indexing (34); lifetime-bearing views and safe reference relationships (35, 38); raw-pointer dereference and unsafe pointer operations (44); closures/method values (28-29); refinement projection and pattern reasoning (50); unsafe assumptions/establishments (44); async/protocol expressions (59); general resumptive handlers (61) |
| Patterns | Recursive wildcard, Boolean, binding, positional enum-variant, nominal-record, and structural-tuple patterns; pure guards; nested usefulness/exhaustiveness for `Bool`, records, tuples, and closed/open generic enums | Refined patterns (50); protocol-state patterns (59) |
| Types and callable structure | `Int64`, `Bool`, `Text`, `Unit`, and `Never`; structural tuples of arity 2 through 16; nominal applications; `Option`/`Result`; capability and structural function types; bounded type/effect parameters; one trait bound; `borrow`/`inout` parameters | Richer traits/generic methods and required associated items (30); constants and constrained const parameters (31, 33); remaining numeric/byte/rune primitives and units/dimensions (32); arrays/collections (34, 38); lifetime relationships/views (35); resource/allocator and cost types/clauses (36-38); general effect rows/aliases (42); raw pointers/ABI types (44, 46); concurrency traits/types (59) |
| Cross-cutting representation | Every successful-program AST statement/expression/pattern kind is parsed, structurally traversed, semantically analyzed, relocated, lowered through owning IR, and executable or explicitly non-runtime; discriminants, spans, linked-arena ownership, current executable type relations, callable context, canonical place/projection ownership, per-kind censuses, and composite relocation fixtures are independently validated. Separately, P1 lowers and validates every bodyful E6 callable into unstable target-neutral CFG MIR, with canonical rendering and bounded evaluation. | Per-form cross-phase acceptance coverage (43); monomorphization, representation, layout, symbols, and linkage (P2); executable-core runtime ABI and WebAssembly lowering/adapters (P3-P4 and tasks 47-48); reproducible artifacts and interpreter/Wasm differential testing (P5 and task 49) |

Unchecked exceptions and `throw`/`catch` are not planned; recoverable failure remains
typed through `Result`, and cancellation remains typed. Unrestricted token/text macros
remain a non-goal. Mutable globals and top-level initialization remain deferred until
an explicit authority, initialization-order, and concurrency model is approved.
Higher-kinded types, arbitrary variance, specialization, polymorphic recursion, and
general dependent typing remain deferred unless a concrete row below requires and
bounds them. Generic capabilities/members, trait defaults/contracts/authority clauses,
associated effects, inheritance, trait objects, and dependent method effects remain
deferred until a concrete core API requires their smallest coherent subset.

## End-to-End and Production Track

E1-E6 and P1 are complete. P2 is the next production milestone. Deferred language
breadth is not a prerequisite for P2-P5 unless a milestone explicitly activates a
bounded numbered-task slice. Existing numbered capability IDs remain stable; named
slices such as `48A` account for completed portions without renumbering the backlog.

| Done | Order | Milestone | Exit criteria | Source tasks |
| --- | ---: | --- | --- | --- |
| [x] | E1a | Create the shared compilation session and validated-IR handle | One API owns phase order, private frontend state, diagnostics, narrow value/render projections, and teardown; the production CLI and normal consumer tests use the session while phase-corruption and malformed-IR tests intentionally retain direct APIs; repeated execution consumes one truly opaque immutable validated-IR handle, and raw-IR host callbacks are rejected pending E3 | task 43, E1 session slice; current pipeline |
| [x] | E1b | Harden source/package and host boundaries | Configurable package/compiler byte, file, directory depth/work, token, persistent-arena, diagnostic, cumulative allocation-byte, and allocation-count budgets fail deterministically; descriptor-relative discovery and descriptor reads verify regular opened identities and stable metadata; duplicate file identities and symbolic-link operands are rejected; raw host failures use interpreter-owned length-delimited text | hardening debt |
| [x] | E1c | Remove immediate scaling/release hazards | Generic recursion uses sparse adjacency plus iterative SCCs and effect inference uses packed SCC membership; opaque handles reuse final immutable-IR validation while raw mutable-IR APIs retain validation; warning-clean normal/sanitizer CI, Clang parser/package/IR fuzz smoke targets, and deterministic hashed stress baselines are checked in | E1a, E1b |
| [x] | E2 | Define the entrypoint and bounded application ABI | Argumentless `@entry` metadata reaches syntax, HIR, and owning IR; compilation permits zero or one while application resolution requires one; public nongeneric free-function visibility, owned root-capability parameters, explicit closed effects, `()`/`Int64` results, exact successful exit mapping, structured runtime failure, existing interpreter limits, and source/forged-IR diagnostics are specified and checked through the opaque validated handle | task 49A; existing package resolution |
| [x] | E3 | Define the trusted interpreter host profile | Opaque-handle entrypoint execution binds every required capability parameter to a distinct registry-owned root and exact bodyless-member allowlist; safe data-only callbacks expose no IR, callable, root, or private source; console output, bounded argument count/index access, optional deterministic configuration lookup, owned host failures, root-specific dispatch, preflight authority rejection, and default/configurable interpreter limits are specified and checked | task 48A |
| [x] | E4 | Implement interpreter-based `sol run` | `sol run [options] <file-or-package> [-- arguments...]` compiles through the shared session, transfers owning IR and frees frontend state, resolves E2, grants only exact E3 standard profiles, preflights authority, executes under deterministic limits, preserves exact human console bytes, emits `sol.run-result` version 1 JSON with base64 console data, and maps successful/boundary/runtime outcomes to stable statuses and diagnostics | task 49B |
| [x] | E5 | Execute core contracts and refinements | Interpreter policy checks `requires`, `old`, `ensures`, `Result` outcomes, and refined construction with exact cleanup and structured failures; loop obligations remain proof-only pending tasks 50 and 52 | task 50A |
| [x] | E6 | Freeze an executable-core conformance application | One multi-file application passes `fmt`, `check`, `test`, `effects`, `inspect`, and `run`, covering imports, ownership, effects, capabilities, contracts, errors, and cleanup | conformance suite; existing authored-test capability from task 12 |

The compilation API tests and production `check`, `test`, `run`, `effects`, and
`inspect` CLI integrations are the representative normal consumers for E1a. Package
and phase tests intentionally exercise lower-level frontend APIs, while declaration,
raw-IR, MIR-lowering, MIR-evaluator, and raw-interpreter tests retain direct internal
access for malformed IR/MIR mutation, phase-corruption assertions, differential
evaluation, and trusted host-callback coverage.

The production track is underway. P1 through P2.4 and P2.5a are complete for the
frozen E6 profile; P2.5b is the next open checkpoint:

| Done | Order | Milestone | Dependency and numbered-backlog scope |
| --- | ---: | --- | --- |
| [x] | P1 | Introduce ownership-explicit target-neutral CFG/MIR for the frozen executable core | E6; task 45 |
| [ ] | P2 | Define monomorphization, representation, target layout, symbols, and linkage | P1 |
| [ ] | P3 | Define the target-independent runtime ABI and production panic/cleanup policy | P1, P2; task 48B |
| [ ] | P4 | Integrate a WebAssembly backend and host adapter | P2, P3; task 47W and task 48C |
| [ ] | P5 | Implement reproducible `sol build` artifacts and interpreter/Wasm differential tests | P4; task 49C |

P1 and task 45 are complete for every bodyful E6 callable. The MIR remains an
unstable callable-scoped internal form: it does not choose representation or ABI,
is not consumed by the production compilation session or CLI, and is not a backend.
P1 was split into these independently reviewable checkpoints:

| Done | Order | Checkpoint | Exit criteria |
| --- | ---: | --- | --- |
| [x] | P1a.1 | Establish callable-scoped CFG MIR invariants | An unstable target-neutral MIR owner lowers validated nongeneric free/test callables containing scalar literals, whole-local copy/move/store, unary/binary operations, blocks, `if`, return, and panic into deterministic basic blocks with SSA temporaries/block parameters, explicit parameter/local storage lifetime, conditional cleanup, transactional unsupported results, and independent structure/type/value/storage validation |
| [x] | P1a.2 | Add direct calls and remaining local control | Lower call-scoped shared/exclusive borrows, direct calls, short-circuit Boolean control, lexical regions, `require`, and proof-backed unreachable with explicit normal/failure and cleanup edges |
| [x] | P1a.3a | Add structured loop CFG | Lower `loop`, `while`, nearest-loop break/continue, condition-side transfers, natural backedges, lexical/region cleanup to loop boundaries, and erased loop-obligation metadata with independent loop/source/CFG validation |
| [x] | P1a.3b1 | Add bounded semantic value construction | Lower authority-free infallible zero/one-payload records, enum variants, Option/Result cases, and non-refined distinct wrappers without choosing layout, with exact constructor tags, source provenance, deterministic operand arenas, and transactional rejection of unsound forms |
| [x] | P1a.3b2 | Add owned temporary cleanup and multi-operand construction | Stage owned call/construction operands in typed reusable temporary slots, consume exact suffixes on invoke/construct, preserve outer temporaries across loop transfers, clean abandoned prefixes in interpreter order, and lower multi-operand records/tuples/enums with independent transition/provenance validation |
| [x] | P1a.3b3 | Add checked refined construction | Retain the exact type-owned predicate obligation in a target-neutral check terminator that consumes its staged representation on both outcomes, transports the nominal result only on success, and performs pending/local/region cleanup before resuming failure, with independent provenance and transition validation |
| [x] | P1a.3b4 | Lower recursive matches and guards | Evaluate one staged scrutinee, test recursive source patterns in arm order, materialize source-bound provisional bindings, preserve the scrutinee after mismatch/false guards, consume it on selection, and validate exact arm CFG, cleanup, and failure provenance |
| [x] | P1a.3b5 | Lower typed Option/Result propagation | Consume one staged sum in an abstract two-result terminator, transport the success payload to continuation, retype None/Err to the callable result on the residual edge, and preserve that residual through exact pending/local/region/parameter cleanup before return |
| [x] | P1a.3b6 | Add bounded projected places | Retain canonical local-rooted record/tuple paths for projected copies, fully initialized plain replacement, and direct shared/exclusive call operands with sibling-aware overlap checks and normal-edge-only abstract writeback |
| [x] | P1a.3b7 | Add partial-move path state | Track unavailable projection frontiers through moves, exact-hole reinitialization, joins, loops, and whole-local cleanup without choosing aggregate layout |
| [x] | P1a.3b8 | Add callback invocation | Stage dynamic function callees before ordered operands, retain exact structural signature/access metadata, consume callee and owned arguments atomically, and preserve normal/failure cleanup and writeback edges |
| [x] | P1a.3b9 | Add concrete method invocation | Select nongeneric implementation callables from exact non-forwarded evidence, lower owned/shared/exclusive receivers before operands, and retain receiver-first normal writeback with failure suppression |
| [x] | P1a.3b10 | Add capability invocation | Borrow the exact receiver embedded by a direct bound operation, retain exact capability member identity, recompute `Self` effects over receiver roots, and reuse invoke failure cleanup without materializing a bound-operation value |
| [x] | P1a.3b11 | Add exact handler scopes | Retain source handler operation, authority root, and provider metadata in target-neutral enter/exit markers; lower nested bodies under exact lexical scopes; unwind handlers on every normal, transfer, and failure edge; and independently validate marker provenance, completeness, LIFO nesting, joins, and terminal balance |
| [x] | P1a.3b12 | Add bounded callable-contract envelopes | Retain ordered item-owned requires, infallible direct-scalar entry snapshots, complete-result postcondition epilogues, outcome-qualified ensures, semantic violations, predicate failures, and cleanup in target-neutral CFG; reject fallible snapshots and generic, capability-member, or exclusive contracted forms transactionally |
| [x] | P1a.3b13 | Retain bounded generic invocation and implementation bodies | Lower standalone bounded generic/effect-polymorphic free-function bodies and executable trait implementations; activate exact receivers before ordinary parameters; retain owning-IR type arguments, instantiated effects/tails, and concrete or forwarded evidence on invokes without selecting forwarded methods or choosing layout |
| [x] | P1a.3b14 | Complete structured executable-core CFG lowering | Lower compound assignment, authority-bearing construction, and every remaining E6 callable; expand contract coverage only where those forms require it |
| [x] | P1b | Freeze target-neutral value/CFG invariants | Complete dominance/SSA and ownership validation, canonical internal rendering, and a bounded MIR evaluator/trace with focused MIR-vs-owning-IR differential tests, without choosing representation, ABI, or a backend execution interface |

P1b was split into independently reviewable checkpoints and is complete now that
validated MIR can be rendered and evaluated by an independent bounded CFG engine,
with focused MIR-vs-owning-IR differential tests:

| Done | Order | Checkpoint | Exit criteria |
| --- | ---: | --- | --- |
| [x] | P1b.1 | Freeze structural SSA dominance | Compute deterministic reverse-postorder immediate dominators over compact predecessor slices; permit instruction and block-parameter values exactly in dominated blocks; retain strict same-block definition order and edge-scoped terminator results; reject sibling, join-bypass, loop-backward, and edge-argument leaks |
| [x] | P1b.2 | Freeze affine SSA ownership | Classify every value use as copy, consume, borrow, or transport and validate non-Copy ownership across instructions, edges, joins, loops, calls, and cleanup |
| [x] | P1b.3 | Add canonical internal MIR rendering | Emit one bounded deterministic versionless text form covering every semantic block, value, place, edge, temporary, loop, and source relation |
| [x] | P1b.4 | Add a bounded MIR evaluator and trace | Execute the supported frozen MIR vocabulary in an independent bounded CFG engine; preflight unsupported bodyful capability members and runtime-produced callable closures; and use focused tests to compare values or failure codes/spans, raw-host behavior, and exact owned-local cleanup with the owning-IR interpreter. This is not the interpreter/Wasm differential suite planned by P5. |

P2-P5 retain the frozen E6 language and package profile. They do not implicitly pull
in closures, collections, user resources/allocators, unsafe, C FFI, manifests or
external dependencies, public IR, concurrency, or broader handlers.

### P2 - Concrete Program, Representation, and Linkage

| Done | Order | Checkpoint | Exit criteria |
| --- | ---: | --- | --- |
| [x] | P2.1 | Establish a symbolic production-program MIR owner | Seed one deterministic owner from selected entry, test, or internal-fixture roots; cache each bodyful callable template once; record approved bodyless capability members as import demands and evidence-dispatched trait requirements as typed specialization demands; discover direct function/provider references; reject references that are neither bodyful, approved imports, nor resolvable specialization demands; and enforce callable/edge/work budgets |
| [x] | P2.2 | Plan canonical monomorphic instances | Intern instances by callable, concrete receiver/type arguments, instantiated effects/tail, and dispatch evidence; process the demand graph deterministically; deduplicate recursive identical instances; reject expanding generic recursion or budget exhaustion; and produce complete substitutions for signatures, locals, places, temporaries, results, snapshots, and obligations |
| [x] | P2.3a | Materialize bounded specialization images and invoke dispatch | Build one independently owned exact template-topology clone per canonical plan instance; attach exhaustive context-zero concrete signature/type overlays; bind every executable invoke to exactly one canonical instance or approved import demand; validate provenance, clone independence, limits, and deterministic buffered rendering; reject handler-demand plans until P2.3b2 rather than claiming unresolved handler semantics |
| [x] | P2.3 | Materialize specialized callable CFGs and dispatch | Clone and substitute every demanded MIR instance; resolve `Self`, type/effect parameters, concrete and forwarded evidence, method requirements, callback signatures, and implementation receivers; iterate specialization and callable discovery to a deterministic fixed point; reject every remaining bodyless trait requirement or unavailable body; eliminate generic parameters, effect tails, and forwarded evidence; then revalidate SSA, ownership, cleanup, and source provenance |
| [x] | P2.3b1 | Add concrete type/local/place/value specialization metadata | Replace implicit refinement coordinates with canonical body/refinement contexts; consume every context's typed uses; materialize independently owned concrete types and owning-semantics fixed-point Copy flags, signatures, locals, places/projections, values, temporaries, and checked instruction/construct/call-operand specialization overlays; retain each exact symbolic MIR clone as the structurally executable CFG; validate recursive nominal graphs, exact overlay reconstruction, image-local IDs/chains, ownership ranges, limits, determinism, and transactional rendering |
| [x] | P2.3b2 | Complete concrete CFG, dispatch, handlers, and writeback | Own complete concrete blocks, terminators, edges, parameter/edge values, loops, instruction/operand payloads, closed effect rows, imports, and one canonical binding per plan demand; bind invokes and nested handler frames image-locally; emit normal-edge exclusive receiver/argument writeback in formal order; retain symbolic topology only for authentication; and validate the fixed plan graph reconstructively without re-resolving evidence |
| [x] | P2.3b3 | Revalidate complete concrete dataflow | Independently validate concrete SSA dominance, affine ownership, moves/borrows/transports, cleanup on every exit, contracts/refinements, handler balance, and full E6 closure after P2.3b2 establishes one complete concrete executable vocabulary |
| [x] | P2.4 | Define canonical concrete representations | P2.4a builds the complete target-neutral recipe graph; P2.4b independently validates and censuses the full frozen closure without selecting layout or ABI |
| [x] | P2.4a | Complete concrete shapes and build canonical target-neutral recipes | Materialization owns substituted source-order nominal fields and variants, explicit wrapper backing and derived-capability source types, open/closed flags, and callable value/receiver types. A separate bounded owner assigns one same-ID recipe per concrete type, flat source-order fields/variants with explicit semantic tags, abstract storage, iterative inhabited/zero-size/Copy and explicit drop classifications, and one resolved producer per exact function or bound-operation site; open enums reject transactionally. No byte layout, ABI, symbols, or source-semantic operation plans are selected. |
| [x] | P2.4b | Independently validate and census canonical representations | A separate validator derives exact recipes, flat-arena consumption, fixed points, classifications, producers, receiver roots, limits, and successful-build work from validated materialization scans and validation-only facts without calling construction helpers or building a second representation; the E6 entry-plus-four-tests closure has an exact exhaustive census. P2.5 layout and ABI and P2.6 source-semantic operation plans remain open. |
| [ ] | P2.5 | Compute target-parameterized layouts and access maps | Define checked layout parameterized by pointer width, integer alignment, endianness, and object-size bounds; provide the initial Wasm32 descriptor; compute size/alignment/padding, field and tuple offsets, sum tag/payload locations, projected-place maps, and callable/capability handle layouts; reject overflow and incomplete layouts structurally |
| [x] | P2.5a | Build usable target-parameterized layouts and access maps | A separate bounded owner borrows a validated representation; validates explicit pointer-4/8 target descriptors and the initial little-endian Wasm32 profile; assigns same-ID type, field, and variant layouts plus one map per materialized projection; uses checked `uint64_t` packing for uniform indirect aggregates, explicit-u32 sums, transparent nominal wrappers, and text/callable/capability objects; rejects unsupported projections, overflow, object bounds, cycles, aliases, and malformed reconstruction transactionally; and validates/renders without partial output |
| [ ] | P2.5b | Independently validate and census target layouts | Replace P2.5a reconstructive validation with an independent derivation and complete mutation census while preserving the frozen layout contract and exact E6 Wasm32 census |
| [ ] | P2.6 | Close source-owned semantic operations | Convert constructors, projected accesses, pattern tests/bindings, propagation, checked arithmetic, contract/refinement predicates, snapshots, and handler-provider references into concrete representation-aware plans or synthetic monomorphic bodies so backend input no longer evaluates owning-IR expressions or source-owned obligations |
| [ ] | P2.7 | Freeze symbols and whole-program linkage | Derive collision-checked ASCII symbols from semantic identity and canonical instance keys; assign deterministic internal-callable, entry-export, and function-table identities; resolve every ordinary callable internally; retain only typed symbolic runtime and approved-host requirements for P3; and make ordering independent of addresses and filesystem roots |
| [ ] | P2.8 | Freeze and census the concrete-program contract | Exhaustively validate and canonically render the complete concrete owner; add malformed-input and repeated-lowering equality tests; require the E6 entry closure to be finite and concrete, including generic/trait instances, contracts/refinements, hosted imports, and all reachable failure/cleanup paths; do not select a runtime ABI or emit Wasm |

### P3 - Target-Independent Runtime ABI

| Done | Order | Checkpoint | Exit criteria |
| --- | ---: | --- | --- |
| [ ] | P3.1 | Freeze call, result, and failure conventions | Define backend-neutral operations and signatures for direct/indirect calls, receiver-first methods, owned/shared/exclusive arguments, normal/failure returns, normal-only writeback, entry invocation, E2 exit mapping, stable runtime-import symbol IDs derived from P2 typed requirements, and structured source-aware failure records |
| [ ] | P3.2 | Define bounded allocation and owned-value operations | Specify compiler-owned allocation for current `Text` and aggregate representations, including checked sizing, zero-length behavior, quotas, copying, moving, equality, recursive destruction, allocation failure, and host-result ownership transfer without introducing the user allocator/resource model from tasks 36-38 |
| [ ] | P3.3 | Freeze cleanup, panic, and failure policy | Lower local/place/temporary/snapshot destruction and region exits into exact cleanup actions on return, propagation, panic, host/arithmetic/allocation failure, no-match, contract/refinement violation, and reached-unreachable; preserve failure writeback/postcondition rules and deterministic failure precedence |
| [ ] | P3.4 | Define capability and trusted-host ABI | Use the P2 capability representation to preserve root identity and derived private source across ABI calls; assign exact host operation and import IDs; encode E3 data-only argument/result forms; preflight every authority/import; and define bounded host success/failure transfer without exposing raw IR, arbitrary pointers/functions, or private sources across the safe adapter boundary |
| [ ] | P3.5 | Define exact handler ABI | Specify handler frames containing source operation, root, provider operation/value, and parent; intercept only exact operation/root/effect matches; hide the current frame during provider invocation; preserve nested LIFO behavior; and remove frames on every normal, transfer, and failure path |
| [ ] | P3.6 | Freeze the runtime-lowered program | Validate and canonically render every runtime operation, import, call signature, allocation, cleanup action, failure edge, capability grant, and handler frame; add table-driven ABI conformance and malformed-program tests over the complete P2 graph; retain backend-independent types and symbols; complete task 48B |

### P4 - WebAssembly Backend and Host Adapter

| Done | Order | Checkpoint | Exit criteria |
| --- | ---: | --- | --- |
| [ ] | P4.1 | Select and pin the WebAssembly toolchain | Evaluate and pin an established emitter/optimizer plus execution runtime; define deterministic discovery/version policy, instantiate the P2 Wasm32 layout, emit and validate a minimal module, and freeze module/import/export/table/memory namespaces; incompatible dependencies fail configuration explicitly |
| [ ] | P4.2 | Emit scalar CFG and ordinary calls | Generate validated Wasm for constants, SSA/block parameters, storage lifetime, unary/checked binary operations, branches, loops, direct calls, returns, and normal/failure result dispatch with deterministic P2 symbols and package-relative provenance |
| [ ] | P4.3 | Emit represented values, places, and indirect calls | Implement Text, aggregates, sums, nominal wrappers, constructors, projections, partial moves/reinitialization, copy/drop, propagation, pattern plans, function tables, callbacks, receivers, and normal-edge writeback using P2 layouts and P3 runtime operations |
| [ ] | P4.4 | Emit runtime checks, cleanup, and handlers | Realize panic, arithmetic/allocation errors, no-match, require/unreachable, contracts, snapshots, refinements, complete unwind, handler scopes, and provider dispatch; validate modules and compare instrumented cleanup/failure identity with the P3 contract |
| [ ] | P4.5 | Integrate the trusted host adapter and E6 execution | Map only the E3 profiles to approved imports, preserve distinct roots and authority preflight, marshal bounded data-only values, retain exact console/host failures, and execute E6 success and panic paths from emitted Wasm; every P1 vocabulary category executes or has an explicit unreachable-by-profile rule; complete tasks 47W and 48C |

### P5 - Build, Differential Execution, and Reproducibility

| Done | Order | Checkpoint | Exit criteria |
| --- | ---: | --- | --- |
| [ ] | P5.1 | Create the production build API and target/profile model | Extend the opaque compilation pipeline to return an owned immutable artifact for the initial Wasm target and explicit development/release profiles; reject unknown combinations; and make artifact bytes depend only on validated source, pinned compiler/backend versions, and normalized options |
| [ ] | P5.2 | Implement `sol build` and deterministic artifact writes | Add documented target/profile/output options, package-relative diagnostics, buffered generation, symlink/non-regular-output rejection, atomic replacement, validated Wasm output, and versioned deterministic metadata for ABI/profile/import requirements without adding manifests, lockfiles, caches, or C linkage |
| [ ] | P5.3 | Execute built artifacts through the production adapter | Run source-built and existing artifacts through the trusted Wasm adapter while retaining the interpreter as an explicit engine; reuse E3 arguments/configuration/console policy and existing run-result/exit semantics; reject corrupt, incompatible, over-budget, or unauthorized modules before application execution |
| [ ] | P5.4 | Add interpreter/Wasm differential conformance | Compare results, exit status, stable runtime failure identity, console bytes, host-call sequence/results, and semantic cleanup/drop traces across every frozen executable construct, contracts policy, generic/trait instances, callbacks, handlers, ownership/writeback, propagation, panic, host failure, and allocation limits; do not compare engine-local step counts |
| [ ] | P5.5 | Freeze reproducibility and release acceptance | Require byte-identical artifacts across repeated builds, checkout/output roots, mtimes, locale/timezone, discovery order, and supported CI hosts using pinned dependencies; verify package-relative metadata, stable import/export order, both profiles, clean-tree rebuilding, warning-clean tests, and ASan/UBSan; complete task 49C and P5 |

Tasks 28-42, 44, 46, the allocation/resource/FFI extensions of task 48, the
remaining verification scope of task 50, and tasks 51-61 stay deferred until a
concrete requirement activates their smallest coherent subset. Tasks 43 and 47-50
retain explicitly partial production scope below; task 45 is complete through P1.

## Numbered Capability Backlog

Numbers below are stable capability identifiers, not the immediate execution order.
The active order is defined by the E and P tracks above. Status uses `[x]` for a
completed capability, `[~]` for a completed named slice with remaining scope, and
`[ ]` for open scope with no completed slice. Slice names such as `48A` do not create
or renumber stable capability IDs.

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
| [x] | 17 | Track lexical regions, deterministic cleanup, and bootstrap compiler/interpreter storage lifetimes | 5 | 5 | Borrow checking |
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
| [ ] | 40 | Complete package manifests, feature and edition policy, external dependencies and lockfiles, visibility, aliases/re-exports, sandboxed package builds, and dependency-qualified semantic IDs; this capability is deferred and is not a prerequisite for P2-P5 | 5 | 5 | Existing package resolution; a concrete ecosystem requirement |
| [ ] | 41 | Add versioned schema declarations, canonical schema identities, checked migration declarations, and deterministic migration planning | 4 | 5 | Constants, records, and packages |
| [ ] | 42 | Add bounded general effect-row polymorphism and effect aliases, including explicit/multiple/result-position arguments and authority capture | 5 | 5 | Existing effect parameters and closures |
| [~] | 43 | Maintain cross-phase acceptance coverage: E1 and P1 cover the frozen executable surface; each later language/resource form must add applicable syntax, HIR, owning-IR, MIR, ownership, effect, contract, diagnostic, inspection, formatter, relocation, and malformed-input coverage | 5 | 5 | E1/P1 for the completed slice; each future capability for added forms |
| [ ] | 44 | Define lexical unsafe blocks, assumptions/establishments, raw pointer primitives, audit records, and unsafe effects | 5 | 5 | Places, lifetimes, resources, and obligations |
| [x] | 45 | Introduce target-neutral ownership-explicit callable-scoped CFG MIR for the frozen E6 core, including blocks/SSA, moves, call-scoped borrows/writeback, abstract cleanup/storage lifetimes, regions, panic/failure control flow, generic/effect/evidence metadata, independent validation, canonical rendering, and bounded evaluator/trace semantics without selecting representation or ABI | 5 | 5 | E6 and completed executable-core semantics |
| [ ] | 46 | Define C ABI layouts and FFI declarations with ownership, nullability, threading, blocking, error, and effect metadata | 5 | 5 | Unsafe boundaries (44), package policy (40), and P2 representation/target layout |
| [~] | 47 | 47A selects WebAssembly as the first production target; 47W integrates a pinned established WebAssembly backend for the frozen executable core; native/additional backends remain deferred | 5 | 5 | 47A complete; P2 representation/layout and P3 runtime ABI for 47W |
| [~] | 48 | Complete compiled-runtime support after 48A/E3: 48B defines target-independent executable-core value, allocation, cleanup, panic, capability, and handler ABI policy; 48C supplies the WebAssembly adapter; user resource/allocation and FFI extensions remain deferred with tasks 36-37 and 46 | 5 | 5 | E3 and P2 for 48B; P3 and task 47W for 48C |
| [~] | 49 | Complete application tooling after 49A/E2 and 49B/E4: 49C adds WebAssembly `sol build`, artifact execution, target/profile selection, linkage metadata, reproducibility, and interpreter/Wasm differential tests over the bounded existing package model | 5 | 4 | P4 and existing package resolution; task 40 only if separately activated |
| [~] | 50 | Complete verification beyond 50A/E5 and P1 runtime-preserving lowering: normalized obligations and call-site substitution; refinement projection, destructuring, and exhaustiveness; cost/resource obligations only when tasks 36-37 activate them | 5 | 5 | E5 and P1; tasks 36-37 only for cost/resource checks |
| [ ] | 51 | Add `spec`/example declarations, authored and generated properties, boundary generation, shrinking, reproducible seeds, and ghost state | 4 | 4 | `sol test`, interpreter, and 50A runtime checks |
| [ ] | 52 | Integrate SMT proof policies, isolated solver execution, deterministic caching, counterexamples, cost proofs, and proof diagnostics | 4 | 5 | Logical obligation IR |
| [ ] | 53 | Complete formatter width reflow, trailing-comma policy, sorting, comment reflow, and syntax-category fixtures without semantic reordering | 3 | 3 | Current parser/token-preserving formatter; each later syntax task owns its formatter integration |
| [ ] | 54 | Define the versioned public semantic graph and canonical serialized Sol IR separately from internal interpreter IR and MIR | 5 | 5 | Stable IDs and mature semantics |
| [ ] | 55 | Expose semantic information through a language server | 4 | 5 | Public schemas and graph |
| [ ] | 56 | Implement intent/semantic patch declarations and patch validation | 4 | 5 | Public IR |
| [ ] | 57 | Produce schema/API compatibility and semantic change reports, including migration requirements | 4 | 5 | Public IR, schemas, and patches |
| [ ] | 58 | Generate bounded context bundles for editor and agent workflows | 3 | 4 | Semantic graph |
| [ ] | 59 | Stage structured async/concurrency, `Send`/`Share`, cancellation, actors/channels, protocol-state patterns and `emit`, transactions, and workflows | 5 | 5 | Closures (28-29), resources (36-38), completed P1/task 45 MIR, and P3 runtime ABI |
| [ ] | 60 | Stage typed derives, sandboxed build transforms, and opt-in reflection without unrestricted macros | 3 | 5 | Package sandboxing and public IR |
| [ ] | 61 | Generalize handlers after defining ownership across suspension and resumptions, multiple operations, dynamic authority matching, and row transformation | 5 | 5 | Effect polymorphism, concurrency, and runtime |

## Milestone Discipline

Each compiler increment should:

- preserve deterministic IDs and structured diagnostics;
- include focused positive, negative, and malformed-input tests;
- pass the full C17 warning-clean ASan/UBSan suite;
- update implementation-status documentation;
- be committed and pushed as an isolated checkpoint.
