# Sol Compiler TODO

This list is the sole live authority for work status and execution order. The
foundation was audited September 9, 2026 against baseline `6ec4ba9`; live status
and the cursor were revised September 26, 2026 for the current worktree. The
broader language and toolchain phases remain documented in the [README](README.md#roadmap).
[Project analysis](docs/project-analysis.md) is an analysis snapshot;
[Compiler status](docs/compiler-status.md) is the detailed implementation snapshot
with a current-worktree addendum, not a competing queue. Impact and complexity
use a 1-5 scale, where 5 is foundational or architectural.
Ordering accounts for dependencies rather than only the impact/complexity ratio.

## Execution Cursor

- **Next overall and next production checkpoint: P3.6.**
- M1 is resolved through its infeasibility path: the
  [September 25 report](experiments/m1/README.md) records the missing independent
  governance and protected evaluation boundary.
- M2-M5 remain gated unless M1 is reopened and completed feasibly. M4E remains
  optional within that gated track.
- One coordinator maintains this single next-overall cursor; parallel tracks do
  not create competing priority lists.

## Completed Foundation Through E6, P1, P2, P3.1-P3.5

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
      rendering, and bounded MIR-vs-owning-IR differential evaluation. At the P1
      handoff, representation moved to P2; runtime ABI, backend integration, and
      production-pipeline use remain later-track work.
- [x] Separate target-independent runtime-conventions ownership over the immutable
      completed P2 concrete program, with frozen call/result/failure signatures,
      stable runtime-import identities, canonical failure provenance and E2 exit
      mapping, exact bounded resources, independent validation, and rendering.
- [x] Separate runtime-value operation-demand inventory over authenticated P3.1
      conventions, with one same-ID record per recipe and exact create/copy/drop/
       equal import pairing, excluding bound-environment and executable value plans.
- [x] Target-parameterized runtime allocation plans over the same inventory, with
      checked Text sizing, cumulative quotas, allocation-limit/failure outcomes,
      independent validation, and canonical rendering, without allocation execution.
- [x] Static capability and trusted-host ABI plans over P3.1-P3.3, with exact
      root/private-source lineage, the frozen four-operation E3 surface,
      allocation-free root-specific grant preflight, and test-only bounded host
      transfer/failure selection, without adapters, tokens, or execution.
- [x] Separate bounded `SolMirRuntimeHandlerAbi` over P3.1-P3.4, with exact
      source operation/root/closed-effect interception, provider place/value/internal
      operation/receiver-first signature provenance, lexical parents, complete P2 and
      P3.3 handler exits, and test-only caller-storage activation modeling.

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
| Cross-cutting representation | Every successful-program AST statement/expression/pattern kind is parsed, structurally traversed, semantically analyzed, relocated, lowered through owning IR, and executable or explicitly non-runtime; discriminants, spans, linked-arena ownership, current executable type relations, callable context, canonical place/projection ownership, per-kind censuses, and composite relocation fixtures are independently validated. Separately, P1 lowers and validates every bodyful E6 callable into unstable target-neutral CFG MIR, with canonical rendering and bounded evaluation. P2 completes concrete monomorphic planning/materialization, representation, target layout, source-independent operation plans, whole-program symbols/linkage, and the validated/rendered complete concrete-program owner for the frozen E6 profile. P3.1 freezes a separate target-independent call/result/failure conventions owner over that immutable P2 program; P3.2a-P3.2f add static allocation, ownership, move/drop, copy, equality, and host-result transfer plans; P3.3 adds static cleanup/failure policy; P3.4 and P3.5 add static trusted-host and exact-handler ABI policy. | Per-form cross-phase acceptance coverage (43); remaining value semantics, complete runtime lowering, and WebAssembly lowering/adapters (P3.6-P4 and tasks 47-48); reproducible artifacts and interpreter/Wasm differential testing (P5 and task 49) |

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

E1-E6, P1, P2, P3.1, P3.2a-P3.2f, P3.3, P3.4, and P3.5 are complete for the frozen E6 profile. P3 remains the
current production milestone, with P3.6 next overall and as the next production checkpoint.
Deferred language breadth is not a prerequisite for P2-P5 unless a
milestone explicitly activates a bounded numbered-task slice. Existing numbered
capability IDs remain stable; named slices such as `48A` account for completed portions
without renumbering the backlog.

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

The production track is underway. P1, P2, and P3.1 are complete for the frozen E6
profile; P3.2a-P3.2f, P3.3, P3.4, and P3.5 are complete, and P3.6 is the next overall item and next production checkpoint:

| Done | Order | Milestone | Dependency and numbered-backlog scope |
| --- | ---: | --- | --- |
| [x] | P1 | Introduce ownership-explicit target-neutral CFG/MIR for the frozen executable core | E6; task 45 |
| [x] | P2 | Define monomorphization, representation, target layout, symbols, and linkage | P1; P2.1-P2.8 complete for the frozen E6 profile |
| [ ] | P3 | Define the target-independent runtime ABI and production panic/cleanup policy | P1, P2; P3.1-P3.5 complete; P3.6 next; task 48B |
| [ ] | P4 | Integrate a WebAssembly backend and host adapter | P2, P3; task 47W and task 48C |
| [ ] | P5 | Implement reproducible `sol build` artifacts and interpreter/Wasm differential tests | P4; task 49C |

P1 and task 45 are complete for every bodyful E6 callable. The P1 MIR remains an
unstable callable-scoped internal form: that phase does not choose representation
or ABI, is not consumed by the production compilation session or CLI, and is not a
backend. P2.4-P2.6 now supply downstream representation, layout, and operation plans.
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
| [x] | P2.4b | Independently validate and census canonical representations | A separate validator derives exact recipes, flat-arena consumption, fixed points, classifications, producers, receiver roots, limits, and successful-build work from validated materialization scans and validation-only facts without calling construction helpers or building a second representation; the E6 entry-plus-four-tests closure has an exact exhaustive census. At this checkpoint, layout and source-semantic operation plans were handed to P2.5 and P2.6 (now complete); runtime ABI remains P3 work. |
| [x] | P2.5 | Compute target-parameterized layouts and access maps | Define checked layout parameterized by pointer width, integer alignment, endianness, and object-size bounds; provide the initial Wasm32 descriptor; compute size/alignment/padding, field and tuple offsets, sum tag/payload locations, projected-place maps, and callable/capability handle layouts; reject overflow and incomplete layouts structurally |
| [x] | P2.5a | Build usable target-parameterized layouts and access maps | A separate bounded owner borrows a validated representation; validates explicit pointer-4/8 target descriptors and the initial little-endian Wasm32 profile; assigns same-ID type, field, and variant layouts plus one map per materialized projection; uses checked `uint64_t` packing for uniform indirect aggregates, explicit-u32 sums, transparent nominal wrappers, and text/callable/capability objects; rejects unsupported projections, overflow, object bounds, cycles, aliases, and malformed reconstruction transactionally; and validates/renders without partial output |
| [x] | P2.5b | Independently validate and census target layouts | A separate validator derives every type/object, source-packed field/variant, projected-place map, arena-consumption proof, and exact resource dimension without construction helpers or a second owner; it rejects transitive borrowed aliases before layout-record traversal, and the complete E6 Wasm32 layout and mutation censuses are frozen. |
| [x] | P2.6 | Close source-owned semantic operations | Convert constructors, projected accesses, pattern tests/bindings, propagation, checked arithmetic, contract/refinement predicates, snapshots, and handler-provider references into concrete representation-aware plans or synthetic monomorphic bodies so backend input no longer evaluates owning-IR expressions or source-owned obligations |
| [x] | P2.6a | Build and independently validate concrete operation plans | A separate bounded owner borrowing one validated target layout closes projected accesses, constructors and capability inheritance, recursive pattern tests/extractions, Option/Result propagation, normalized checked arithmetic and recipe-directed equality, concrete snapshot capture, materialized callable producers, and handler bindings/root matching. Executable records use only concrete IDs, recipes, layouts, offsets, tags, and backend-neutral opcodes; source IDs are segregated authenticated provenance. P2.6a handed each contract/refinement check to P2.6b as an unresolved body envelope, and loop proof obligations remain runtime-erased. |
| [x] | P2.6b | Materialize predicate and import-contract bodies | P2.6b1 and P2.6b2 materialize the combined source-independent predicate and import-contract boundary and independently validate its complete executable anatomy and exact resource census; loop proof obligations remain runtime-erased. |
| [x] | P2.6b1 | Close scalar single-block predicates and import contract ownership | Remove `UNRESOLVED_BODY`; lower only scalar/Text/Unit constants, direct unprojected contextual inputs, checked non-short-circuit unary/binary arithmetic/comparison/equality, and one Bool return. Reject every richer form transactionally. Retain exact instance/import context ownership, import-owned helper demands without fake CFG coordinates, envelope-local snapshot slots with segregated source provenance, and ordered requires/snapshots/ensures. Independently authenticate every executable field and exact arena/resource census. |
| [x] | P2.6b2 | Complete rich predicate CFG bodies | Lower short-circuit and conditional control, exact calls and function/bound values within the frozen purity and finite-closure rules, aggregate/tuple/sum/wrapper/refined construction, recursive matches/guards/bindings, immutable blocks/locals, and nested refinement body references into bounded immutable CFGs shared by instance and import contexts. Contract expression propagation remains semantically rejected under `SOL-CONTRACT-002`, while malformed propagation records are validated and rejected. Canonical interning is cycle-safe; independent reconstructive validation covers complete executable fields, SSA/CFG/type/call/body-reference/provenance invariants, and the exact resource census, with source IDs confined to authenticated provenance. |
| [x] | P2.7 | Freeze symbols and whole-program linkage | A separate `SolMirLinkage` owner derives collision-checked versioned ASCII `sol.i1` internal and `sol.e1` entry symbols from semantic identity plus full SHA-256 canonical structural instance keys; resolves all bindings to internal callables or approved host requirements; assigns abstract whole-program function-table identities; retains typed symbolic ABI-neutral runtime requirements; and validates canonical ordering/rendering independently of paths, pointers, source ordinals, and unstable dense IDs |
| [x] | P2.8 | Freeze and census the concrete-program contract | The unstable address-stable `SolMirConcreteProgram` owns the program, plan, materialization, representation, layout, operations, and linkage stages while borrowing immutable `SolIr`; builds transactionally with nested stage limits and exact outcome mapping; independently validates predecessor links, transitive stages, finite concrete and executable closure, cross-stage censuses, and anti-aliasing; and renders one canonical versionless buffered form. The E6 all-roots fixture freezes every stage count/usage and complete generic, trait, predicate, import, callable, instruction, terminator, failure, and cleanup closure without selecting a runtime ABI or emitting Wasm. |

### P3 - Target-Independent Runtime ABI

| Done | Order | Checkpoint | Exit criteria |
| --- | ---: | --- | --- |
| [x] | P3.1 | Freeze call, result, and failure conventions | A separate bounded `SolMirRuntimeConventions` owner borrows the immutable completed P2 concrete program; freezes receiver-first internal, host, and indirect-table signatures and calls with owned/shared/exclusive access, VALUE/UNIT/NEVER outcomes, failure edges, and normal-only exclusive writeback; maps entry results to E2 exits; derives collision-checked SHA-256 `sol.h1` host and `sol.r1` recipe-operation identities/symbols from exact P2 requirements; owns canonical source-aware failure sites and code masks; independently validates exact resources and aliases; and renders one canonical buffered form without choosing a physical ABI or Wasm index. |
| [ ] | P3.W1 | Bounded Wasm ABI Integration Experiment | Timebox after P3.1 has tested conventions: record candidate tool versions, validate and execute a minimal scalar call/result/failure module, and report ABI/tool mismatches. Add allocation, cleanup, or host imports only after the relevant P3.2-P3.4 conventions are tested. Inform, but do not complete, P4.1, 47W, 48C, component support, E6 Wasm execution, backend integration, or build tooling. |
| [x] | P3.2 | Define bounded allocation and owned-value operations | P3.2a-P3.2f collectively define and test-model static allocation, ownership, move/drop, copy, equality, and E3 host-result transfer for the frozen profile, without production execution or the user allocator/resource model from tasks 36-38. |
| [x] | P3.2a | Inventory recipe operation demands | A separate bounded `SolMirRuntimeValues` owner borrows authenticated immutable P3.1 conventions and owns one same-ID record per concrete recipe. It strips bound-environment from linkage requirements and pairs every demanded create/copy/drop/equal bit with exactly one existing P3.1 import, while absent bits remain `NONE`; independent validation reconstructs masks, IDs, exact resources, complete arena consumption, and transitive anti-aliasing. This inventory defines no executable operations or allocation plans. |
| [x] | P3.2b | Define checked allocation and quota plans | One same-ID target-parameterized plan per recipe classifies NONE, fixed aggregate outer objects, and Text headers; checked preflight covers zero-length Text, target/object bounds, cumulative request/byte quotas, and allocation-limit/failure outcomes. Independent validation and canonical rendering retain no physical allocation execution. |
| [x] | P3.2c | Define the ownership graph, move, and drop | `SolMirRuntimeValues` owns static descriptive same-ID ownership plans, sum-variant descriptors, and 12 owned edges, with producer-specific captured-receiver edges where applicable. It freezes the exclusive ownership graph, zero-allocation root/projected move and hole/repair semantics, and deterministic depth-first postorder drop semantics; construction/validation are bounded, transactional, independently reconstructive, and fully anti-aliased, with canonical producer-sensitive rendering. No physical runtime values, executor, storage, allocation, or new failure sites are introduced. |
| [x] | P3.2d | Define transactional deep copy | `SolMirRuntimeValues` owns static same-ID copy plans. Test-modeled bounded preflight, private staging, single publication, and postorder rollback specify no partially published destination; successful allocation quota charges are not refunded on rollback. No production copy executor, storage, allocator, or new failure sites are introduced. |
| [x] | P3.2e | Define allocation-free equality | `SolMirRuntimeValues` owns static same-ID recursive equality plans; complete independent validation precedes any test-modeled bounded compare, which allocates nothing and introduces no production executor or failure sites. |
| [x] | P3.2f | Define host-result transfer and closure | `SolMirRuntimeValues` owns static E3 host-result plans with checked borrowed-view-to-fresh-owned transfer, full shape preflight, checker-composed quotas, private staging, postorder rollback, nonrefunded quota charges, and single publication. No production executor, adapter, authority handling, or failure sites are introduced. |
| [x] | P3.3 | Freeze cleanup, panic, and failure policy | `SolMirRuntimeCleanup` borrows immutable P3.1 conventions and P3.2 values to own static event, action, transition, drop-path, and site policy. It replays exact P1/P2 CFG cleanup/drop paths and executable lexical scope markers, with narrowly sound immediate callable-field hole repair; orders scope/temp/region/snapshot/parameter actions; retains exact inherited P3.1 sites plus supplemental sites only for uncovered allocation/resource producers, panic/host detail, and first-failure precedence; and remains test-modeled with no cleanup executor or backend. |
| [x] | P3.4 | Define capability and trusted-host ABI | `SolMirRuntimeHostAbi` statically owns exact P2 capability root/derived/private-source lineage, four frozen E3 host profiles (`Console.write`, `Arguments.count`, `Arguments.get`, `Configuration.read`), entry-root/formal closure, E3 shapes, root-specific requirements/grants, allocation-free sealed preflight, and test-only bounded host-result transfer/P3.3 failure selection; no adapter, tokens, or execution |
| [x] | P3.5 | Define exact handler ABI | `SolMirRuntimeHandlerAbi` borrows P3.1-P3.4 and statically owns bodyful root-capability provider/value/place/internal-operation/receiver-first-signature plans with lexical parents. It requires exact source operation, root, and closed-effect matching; pairs every P2 enter/exit marker and P3.3 cleanup exit; and test-models caller-owned activation storage, matched-plus-younger hidden suffixes, and exact LIFO restoration on provider success/failure, including nested older-provider invocation under an already hidden suffix. No executor, adapter, storage ownership, tokens, backend, or production execution. |
| [ ] | P3.6 | Freeze the runtime-lowered program | Validate and canonically render every runtime operation, import, call signature, allocation, cleanup action, failure edge, capability grant, and handler frame; add table-driven ABI conformance and malformed-program tests over the complete P2 graph; retain backend-independent types and symbols; complete task 48B |

P3.1 owns its arenas independently and borrows one authenticated, immutable
`SolMirConcreteProgram` that must outlive it. Transactional construction is bounded
by exact counts, owned bytes, scratch, and work; a separate reconstructive validator
authenticates predecessor data, complete arena consumption, ownership and non-aliasing,
calls, signatures, imports, failure sites, and usage. After full owner authentication,
failure-record validation and E2 exit mapping are allocation-free and do not revalidate
the predecessor chain. Unit maps to application status 0; `Int64` 0 through 255 maps
identically; out-of-range values map to driver status 1 with `SOL-RUN-002`; authenticated
runtime failures map to driver status 1 with their structured code.

P3.2a adds only an authenticated operation-demand inventory. Its separate
`SolMirRuntimeValues` owner borrows immutable P3.1 conventions, owns one record for
each of the 21 E6 recipes, and records 17 recipes with demands: 16 create, 10 copy,
17 drop, and 5 equal. The four host imports are not recipe plans, and
bound-environment imports are intentionally excluded. The owner reuses P3.1 symbols
and recipe digests in canonical output and duplicates no predecessor symbols,
digests, layouts, recipes, or records. P3.2a-P3.2f collectively define and test-model allocation, ownership, move/drop,
copy, equality, and host-result transfer; P3.2 is complete, while P3 remains open.
They do not production-execute allocation or value operations.

P3.2b adds one same-ID target-parameterized allocation plan per recipe: canonical
NONE for uninhabited, zero-size, scalar, callable, and capability layouts; exact
fixed aggregate outer objects; and exact Text headers. The E6 Wasm32 census is 21
plans: eight fixed objects totaling 92 bytes, one Text header of eight bytes aligned
to four, and twelve NONE plans. The allocation-free checker preflights zero-length
Text and positive byte payloads against target/object bounds and cumulative
request/byte quotas; it maps success, allocation-limit, and future physical failure
to the existing P3.1 failure codes. It performs no allocation or value execution.

P3.2c extends the same owner with 21 static descriptive ownership plans, nine sum-variant descriptors, and 12 owned edges, with producer-specific captured-receiver edges where applicable. It freezes the exclusive ownership graph; zero-allocation root and projected moves with exact holes and repair; and deterministic depth-first postorder drop semantics. Construction and independent reconstruction are bounded, transactional, and fully anti-aliased; canonical rendering is stable and producer-sensitive without paths or raw IDs. The exact E6 usage is 3,360 owned bytes, build work 253, and validation work 73,654,894. The existing 21 recipes, 17 demanded drop imports, and P3.2b allocation census are unchanged. Test-only modeling verifies move/hole/repair and recursive postorder behavior; the full Debug suite and focused ASan passed. This adds no physical runtime values, executor, storage, allocation, copy/equality/host transfer, cleanup-edge lowering, or failure sites.

P3.2d adds 21 static same-ID copy plans: unreachable 1, forbidden 7, trivial 3, Text 1, product 3, sum 5, and wrapper 1. Test-modeled bounded preflight, private staging, single publication, and postorder rollback specify transactional deep copy with no partially published destination. Cumulative successful allocation quota charges are not refunded on rollback. The exact E6 usage is 3,696 owned bytes, build work 275, and validation work 73,654,915; 10 copy imports are demanded, and prior censuses are unchanged. The full Debug suite passed 50/50 and focused ASan passed. This adds no production copy executor, storage, allocator, or new failure sites.

P3.2e adds 21 static same-ID equality plans: unreachable 1, forbidden 7, trivial 3,
Text 1, product 3, sum 5, and wrapper 1. Independent validation completely
authenticates the plans before a compare; the bounded test model recursively compares
represented values without allocation. The five demanded equal imports are unchanged.
The full Debug suite passed 50/50 and focused ASan passed.

P3.2f closes P3.2 with 21 static E3 host-result plans: unreachable 1, forbidden 14,
one each for `Int64`, `Bool`, `Text`, and `Unit`, two `Option`, and no `Result`;
four host requirements are covered. Test-modeled checked borrowed-view-to-fresh-owned
transfer performs full shape preflight and checker-composed quotas, then uses private
staging, postorder rollback, nonrefunded successful quota charges, and single
publication. The exact E6 usage is 4,432 owned bytes, build scratch 94, build work
357, validation scratch 584,692,564, and validation work 73,655,017; prior counts
are unchanged. The full Debug suite passed 50/50 and focused ASan passed. P3.2a-P3.2f
collectively define and test-model allocation, ownership, move/drop, copy, equality,
and host-result transfer, not production execution. This introduces no production
executor, adapter, authority handling, or failure sites.

P3.3 adds a separate static `SolMirRuntimeCleanup` owner borrowing immutable P3.1 conventions and P3.2 values. Its five arenas contain 368 same-ID events, 622 ordered actions, 452 transitions, 262 drop paths, and 44 sites; it replays exact P1/P2 CFG cleanup and drop paths, including prerequisite executable lexical scope markers and narrowly sound immediate callable-field hole repair. Actions order normal-only writeback, contract gates, temporary/place drops, scope and region exits, snapshot and owned-parameter drops, and final failure propagation. Sites retain exact inherited P3.1 provenance; supplemental sites cover only uncovered allocation/resource producers. Producing panic/host detail is captured before cleanup, while the first failure remains primary and an authenticated later contract failure is suppressed. The E6 census has 262 drop paths, 121792 owned bytes, build scratch 246744, build work 72327, validation scratch 246744, and validation work 48218. Construction and validation are bounded, transactional, independently reconstructive, and anti-aliased; rendering is canonical. Test-only traces model policy only: there is no cleanup executor, runtime-value execution, storage, allocator, physical ABI, backend, or compiled execution. Full Debug passed 51/51 and focused ASan passed.

P3.4 adds a separate static, backend-independent `SolMirRuntimeHostAbi` owner borrowing authenticated immutable P3.1 conventions, P3.2 values, and P3.3 cleanup policy. It retains exact P2 capability construction lineage: distinct entry roots, `BASE_SOURCE` derived closure, and private-source metadata; the exact-source correction is frozen, so a derived capability cannot substitute for its declared private source and `PRIVATE_SOURCE` construction is not source-reachable. The sealed E3 surface has exactly four approved profiles: `Console.write(Text) -> Unit`, `Arguments.count() -> Int64`, `Arguments.get(Int64) -> Option<Text>`, and `Configuration.read(Text) -> Option<Text>`, each preserving its exact P3.1 identity/import/symbol and root-specific receiver provenance. It owns entry-formal root descriptors, E3 formal/shape forests, exact requirements, and root-specific grants. After authentication, pure preflight is allocation-free and non-mutating: it checks the complete root/grant/import/descriptor/result closure before any model callback, allocation, quota charge, token/object creation, cleanup, or transfer. Test-only bounded invocation borrows only data views, uses the P3.2f transfer plan for fresh-owned result staging/rollback/publication, and selects existing P3.3 first-failure policy for bounded `HOST_ERROR` detail. It adds no production adapter, capability token/object, raw IR, private-source exposure, physical ABI, host-function pointer, runtime execution, or new failure code/site. The exact E6 census is 3 capability plans, 3 roots, 4 host operations, 3 arguments, 3 formals, 3 shapes, 0 shape cases, 5 requirements, and 4 grants; usage is 1816 owned bytes, 185 build scratch bytes, 27868 build work, 77 validation scratch bytes, and 8347 validation work. Final Debug and ASan/UBSan validation each passed 52/52.


P3.5 adds a separate bounded, backend-independent `SolMirRuntimeHandlerAbi` owner borrowing authenticated immutable P3.1 conventions, P3.2 values, P3.3 cleanup policy, and P3.4 host ABI. Prerequisite commit `c6c6041` narrowly supports bodyful root-capability members internally with receiver-first signatures/contracts; derived/private-source bodyful members remain unsupported and bodyless members remain imports. The owner retains same-ID bodyful provider place/value/internal-operation/receiver-first-signature plans, exact source operation/root/closed-effect interception, lexical parents, every P2 enter/exit marker, and every P3.3 cleanup exit. Its test-only caller-storage activation model evaluates before push, selects only exact matches, hides the matched frame plus complete younger suffix during provider invocation, and restores that exact suffix in LIFO order before either success or failure proceeds, including for nested older-provider invocation under an already hidden suffix. It adds no executor, adapter, storage ownership, tokens, backend, or production execution. The focused census is 7 frames, 14 exit markers, 8 cleanup exits, 29 marker references, stack depth 7, test work 7, 2152 owned bytes, build scratch 56, build work 7809, validation scratch 56, and validation work 12251. Empty E6 has all counts/storage/scratch zero, build work 22, and validation work 2707. Final Debug and ASan/UBSan each passed 53/53.

The E6 all-roots census is exactly 18 signatures, 19 signature slots, 18 calls,
24 operands, one writeback, one entry, 52 imports, and 29 failure sites. In field
order `(signatures, signature slots, calls, operands, writebacks, entries, imports,
failure sites, owned bytes, build scratch bytes, build work, validation scratch bytes,
validation work)`, exact usage is
`(18, 19, 18, 24, 1, 1, 52, 29, 16328, 21, 9427, 584692564, 73649839)`.
The corresponding default maxima are
`(4000000, 16000000, 16000000, 64000000, 32000000, 1, 4000000, 32000000,
1073741824, 268435456, 4000000000, 1073741824, 4000000000)`.
Approved validation reported 49/49 normal and 49/49 AppleClang ASan/UBSan tests.
Ordinary callback execution still rejects at the upstream P2 unsupported-closure
boundary, and source-level `Never` callables still fail with `SOL-TYPE-009`; boundary
tests cover both while directly exercising reachable direct/host and indirect-table
target logic, access classes, result classes, failure masks, provenance, and exits.

These completed checkpoints define static/test-only handler ABI policy but no physical
backend ABI or Wasm indices, executable allocation/value operations, cleanup/unwind
lowering, production host adapter, runtime handler stack/execution, complete
runtime-lowered program, compiled `sol build`, or backend-backed `sol run`.
The P3.3 policy retains exact inherited P3.1 sites and adds supplemental sites only for uncovered allocation/resource producers. The P3.W1 experiment remains bounded and does not replace the P3.6 cursor.

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
remaining verification scope of task 50, and tasks 51-62 remain deferred except
for the proposed M2-M4E slices below or a separately approved workload-gated slice.
Tasks 43 and 47-50 retain explicitly partial production scope below; task 45 is
complete through P1. P3.W1 is a bounded integration experiment, not completion of
the broader backend track.

## Bounded Maintenance-Workflow Experiment

M1 is completed through its documented infeasibility path; the remaining rows are
OPEN proposals, not implemented features. This experiment does not wait for full
public IR (54), SMT (52), a production backend, or patch syntax (56). The M1 report
records projection gaps without adding those systems as prerequisites. The
Execution Cursor governs order.

| Done | Order | Experiment | Bounded exit criteria |
| --- | --- | --- | --- |
| [x] | M1 | First-user workload and experiment charter (completed as infeasible) | The [infeasibility report](experiments/m1/README.md) evaluates an unvalidated expense-policy candidate and records the governance hard stop: no actual user or policy owner, protected held-out material, externally controlled verifier, or independent adjudicator exists. Repository visibility and hashes do not provide authority separation. P2.8 has since completed; this closure implements no experiment feature. |
| [ ] | M2 | Declaration-centered context packets (58 slice) | Only after M1 is reopened and completed feasibly, and after P2.8, produce bounded deterministic packets of source, signatures, effects, contracts, known available callers/callees, and checked lexical authority roots. Include source snapshot hash, compiler/schema/options, selection metadata, provenance, inclusion reasons, heuristic candidate test associations, omissions, and explicit unknowns. Unavailable is not empty; use existing IDs only within their supported scope. Do not claim complete reachability, global minimality, authoritative test relevance, or business authorization. Inspection bytes are not a guaranteed semantic cache key. |
| [ ] | M3 | Conservative checked-snapshot deltas (57 slice) | After M2, report declaration, signature, effect, contract, known call-edge, and checked lexical-authority edits between checked snapshots; validate stable IDs and reject stale, ambiguous, or unsupported comparisons. Report contract direction only for a precisely defined and tested structural subset; otherwise report changed or unknown. Keep body edits and unknowns visible: a changed predicate is not established weakening, and an unchanged interface does not establish preserved behavior. |
| [ ] | M4 | Ordinary-edit validation and approval loop (56 and bounded 62 slices) | After M2/M3, exercise ordinary edit/check/test/delta/independent human adjudication against the protected change intent, base snapshot, tests, policy, and verifier. The builder cannot modify those inputs. An independently controlled verifier evaluates checked base/candidate snapshots and emits the bounded unsigned local task-62 evidence record defined below. Authority or contract changes require explicit adjudication. This is not patch syntax, architectural auto-repair, or proof of correctness. |
| [ ] | M4E | Optional basic editor slice (55 slice) | Only after a feasibly reopened M1 and M2 establish projection feasibility, timebox diagnostics, navigation, and packet presentation. Not full LSP and not a prerequisite for M5. |
| [ ] | M5 | Held-out workflow comparison | After M4, compare against source-only on protected held-out tasks with the same compiler/test access and, where relevant, model and budget. The independent adjudicator emits `approve`, `reject`, or `escalate`; on escalation, the adjudicator may inspect precisely scoped additional source and records its reason and scope rather than silently granting the builder broader context. Measure decision quality, false accepts/rejects, implementation-view request rate, decision time, correctness, regressions, context size, iterations, and diagnostic usefulness. Protected tests are evidence, not complete ground truth. Report negative/inconclusive results and decide continue, narrow, or stop. |

M2 maps to 58, M3 to 57, M4 to the validation/approval slice of 56 and a bounded
unsigned local evidence-record slice of 62, and M4E to 55. For M4, the independently
controlled verifier records base and candidate source identities and
compiler/schema/options, protected change-intent/baseline/test/policy/verifier identities, the M3
semantic delta, evaluations and results, failures or incomplete states, omissions,
and explicit unknowns. The deterministic record is content-addressed but unsigned:
hashing supplies integrity and addressing, not producer authentication or deployment
provenance. These proposals do not earn `[~]` status; full scopes of 54-58 and 62
remain in the numbered backlog. M1's completed-as-infeasible status records a
governance blocker, not a charter or tool feature. A protected external change
contract would be an experiment artifact if M1 were reopened, not the future Sol
intent or semantic-patch syntax proposed by tasks 54/56.

## Workload-Gated Decisions

These are evidence-driven decisions, not implicit P2-P5 requirements or newly
implemented semantics. A feasibly reopened M track may justify a separately bounded
task; otherwise retain the existing behavior and deferrals.

| Decision | Evidence and boundary |
| --- | --- |
| Recoverable refinement validation and safe projection | Determine whether the workload needs `Result`-returning validation and safe access to the representation (50); define a bounded API/semantic slice before implementation, without claiming existing checked construction already provides it. |
| Small data facilities | Select only demonstrated text/bytes, constants, collections, or serialization needs (31-34, 38, 41 as applicable); do not activate their full families or dependencies by implication. |
| Test ergonomics | Use observed task friction to decide bounded improvements over current Boolean tests (12, 51); property infrastructure is not an experiment prerequisite. |
| Developer workflow | Measure editor, latency, installation, and debugging friction; use M4E or another explicit slice only where justified, not full LSP by default. |
| Evaluation and contract semantics | Preserve current formal-parameter evaluation order for named arguments. Pure does not mean total, and an accepted typed contract is not a proof. Any alternatives require a separate decision and tests. |
| Formatter policy (53) | Reconsider whether broader mandatory sorting is warranted; this refresh neither enacts a new sorting policy nor permits semantic reordering. |
| Fine-grained semantic anchors | Consider member/local/expression persistent IDs only if M2/M3 show that supported top-level identities and checked provenance cannot identify required context or deltas. First define stability, stale-target rejection, and migration costs; otherwise defer fine-grained persistent IDs. |
| Executable architecture policy | Consider a machine-executable policy only after M4/M5 identify repeated decisions that cannot be represented by the protected external policy and independent adjudication. First define policy ownership, versioning, conflict handling, and failure behavior; otherwise defer an architecture DSL. |
| Automated risk lanes and signing | Consider calibrated automatic lanes only after M5 provides enough adjudicated approve/reject/escalate outcomes to measure false accepts/rejects by class. Consider signatures or deployment attestations only after task 62 has a stable envelope plus an explicit identity, key, trust, and deployment-provenance model; otherwise retain unsigned local records and human adjudication. |
| Runtime effect/outcome evidence | Consider execution evidence only after P5 provides reproducible artifacts, stable runtime event identities, and interpreter/Wasm comparison, followed by explicit privacy, retention, sampling, and budget policy. Otherwise defer production telemetry and do not infer runtime outcomes from static effects. |

General handlers, additional backends or a VM, workflows/transactions, broad
numerics and units, real-time features, and reflection remain deferred until
concrete evidence justifies a bounded decision. Future alternatives are not
implemented language guarantees. Full semantic patch syntax, an architecture DSL,
generalized risk scoring, signed or deployment attestations, fine-grained persistent
IDs, production telemetry, and proof remain outside the bounded M experiment.

## Numbered Capability Backlog

Numbers below are stable capability identifiers, not the immediate execution order.
The active order is defined by the Execution Cursor and the E, P, and M tracks
above, not by numeric ID or table position. Status uses `[x]` for a
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
| [~] | 48 | Complete compiled-runtime support after 48A/E3 and P3.1 call/result/failure conventions: 48B continues with target-independent executable-core allocation, cleanup, panic, capability, and handler ABI policy; 48C supplies the WebAssembly adapter; user resource/allocation and FFI extensions remain deferred with tasks 36-37 and 46 | 5 | 5 | E3, P2, and P3.1 for the completed slices; remaining P3 and task 47W for 48C |
| [~] | 49 | Complete application tooling after 49A/E2 and 49B/E4: 49C adds WebAssembly `sol build`, artifact execution, target/profile selection, linkage metadata, reproducibility, and interpreter/Wasm differential tests over the bounded existing package model | 5 | 4 | P4 and existing package resolution; task 40 only if separately activated |
| [~] | 50 | Complete verification beyond 50A/E5 and P1 runtime-preserving lowering: normalized obligations and call-site substitution; refinement projection, destructuring, and exhaustiveness; cost/resource obligations only when tasks 36-37 activate them | 5 | 5 | E5 and P1; tasks 36-37 only for cost/resource checks |
| [ ] | 51 | Add `spec`/example declarations, authored and generated properties, boundary generation, shrinking, reproducible seeds, and ghost state | 4 | 4 | `sol test`, interpreter, and 50A runtime checks |
| [ ] | 52 | Integrate SMT proof policies, isolated solver execution, deterministic caching, counterexamples, cost proofs, and proof diagnostics | 4 | 5 | Logical obligation IR |
| [ ] | 53 | Complete formatter width reflow, trailing-comma policy, sorting, comment reflow, and syntax-category fixtures without semantic reordering | 3 | 3 | Current parser/token-preserving formatter; each later syntax task owns its formatter integration |
| [ ] | 54 | Define the versioned public semantic graph and canonical serialized Sol IR separately from internal interpreter IR and MIR | 5 | 5 | Stable IDs and mature semantics |
| [ ] | 55 | Expose semantic information through a language server; M4E proposes only an optional basic editor slice | 4 | 5 | Public schemas and graph for full scope; feasibly reopened M1 and M2 projection feasibility for M4E |
| [ ] | 56 | Implement intent/semantic patch declarations and patch validation; M4 proposes only ordinary-edit validation and human approval, not patch syntax | 4 | 5 | Public IR for full scope; M2/M3 for M4 |
| [ ] | 57 | Produce schema/API compatibility and semantic change reports, including migration requirements; M3 proposes only conservative checked-snapshot deltas | 4 | 5 | Public IR, schemas, and patches for full scope; M2 for M3 |
| [ ] | 58 | Generate bounded context bundles for editor and agent workflows; M2 proposes declaration-centered packets from available projections | 3 | 4 | Semantic graph for full scope; feasibly reopened M1 and P2.8 for M2 |
| [ ] | 59 | Stage structured async/concurrency, `Send`/`Share`, cancellation, actors/channels, protocol-state patterns and `emit`, transactions, and workflows | 5 | 5 | Closures (28-29), resources (36-38), completed P1/task 45 MIR, and P3 runtime ABI |
| [ ] | 60 | Stage typed derives, sandboxed build transforms, and opt-in reflection without unrestricted macros | 3 | 5 | Package sandboxing and public IR |
| [ ] | 61 | Generalize handlers after defining ownership across suspension and resumptions, multiple operations, dynamic authority matching, and row transformation | 5 | 5 | Effect polymorphism, concurrency, and runtime |
| [ ] | 62 | Define a versioned canonical change-evidence envelope and later optional attestations. The envelope references or embeds, and records execution of, outputs from tasks 54, 56, 57, 58 and later P5; it must not implement a second semantic graph, delta engine, context packet, patch format, or reproducible-build system. M4 activates only the unsigned deterministic content-addressed local evidence-record slice with independently controlled verification; signatures, producer authentication, and deployment provenance remain later gated scope. | 4 | 4 | M2/M3 and protected inputs from a feasibly reopened M1 for the M4 slice; tasks 54/56-58 and P5 for the full envelope |

## Milestone Discipline

Each compiler increment should:

- preserve deterministic IDs and structured diagnostics;
- include focused positive, negative, and malformed-input tests;
- pass the full C17 warning-clean ASan/UBSan suite;
- update implementation-status documentation;
- be committed and pushed as an isolated checkpoint.

Roadmap and experiment updates should also:

- keep this file's cursor/status authoritative and documentation examples consistent
  with the implemented CLI and semantics; label snapshot dates and historical handoffs;
- distinguish implemented behavior, runtime checks, static acceptance, proofs, and
  experimental evidence; record omissions, blockers, and negative results explicitly;
- mark slices complete only with bounded exit evidence, not merely a proposal;
  historical test counts are not fresh validation of a documentation-only refresh.
