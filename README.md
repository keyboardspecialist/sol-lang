# Sol

**A programming language for explicit intent, safe implementation, progressive verification, and reliable change.**

> **Project status:** Sol is currently a language-design and compiler-research project. A bootstrap compiler front end is under development, but no production-ready compiler or stable toolchain exists yet.

The bootstrap lowers successful package compilation into an owning, deterministic canonical typed IR and includes a compiler-internal reference interpreter that consumes only validated IR after all frontend data is freed. The interpreter executes the bounded immutable core, explicit trait evidence, root-preserving capabilities, and exact deep handlers under deterministic resource limits. `sol test` runs authored Boolean unit tests through that interpreter. It is not a production runtime: there is no `sol run`, VM, bytecode, code generation, or runtime contract enforcement. Contracts are retained as IR templates but ignored during test execution. The IR and interpreter APIs are explicitly unstable compiler internals, not serialized formats or public ABIs.

Sol is designed for software written and maintained collaboratively by humans and AI systems. It treats effects, authority, contracts, resource behavior, semantic identity, and change consequences as first-class parts of the program rather than context that must be reconstructed from conventions, comments, and repository archaeology.

## Design Specification

The current official manual is [Design Specification v0.2](Sol_Programming_Language_Design_Specification_v0.2.pdf). Its authoritative editable source is [`docs/specification.typ`](docs/specification.typ); the PDF is generated and should not be edited directly. The v0.1 PDF remains available as a historical artifact.

Build the manual offline with Typst 0.15.1:

```text
typst compile docs/specification.typ Sol_Programming_Language_Design_Specification_v0.2.pdf
```

When CMake finds Typst during configuration, `cmake --build build --target manual` runs the same build.

The central idea is simple:

> Make intent machine-checkable, make context local, and make ambiguity syntactically expensive.

## Why Sol?

Most maintenance failures are not caused by an inability to express an algorithm. They come from hidden assumptions:

- whether a value may be absent;
- whether a function mutates state or performs I/O;
- which authority a component possesses;
- which invariants callers rely on;
- whether a task may outlive its parent;
- which behaviors must remain unchanged during a refactor;
- whether a source edit changes an API, effect set, schema, cost bound, or security boundary.

Sol aims to move those assumptions into declarations the compiler can inspect, verify, test, and report on.

## Language at a Glance

```sol
module inventory.reservation

use commerce.OrderId
use inventory.ItemId
use inventory.Quantity
use time.Instant

enum ReservationError {
    item_not_found(item: ItemId)
    insufficient_stock(
        item: ItemId,
        requested: Quantity,
        available: Quantity
    )
}

record Reservation {
    order: OrderId
    item: ItemId
    quantity: Quantity where quantity > 0
    created_at: Instant
}

public transaction reserve(
    order: OrderId,
    item: ItemId,
    quantity: Quantity where quantity > 0
) -> Result<Reservation, ReservationError>
effects {
    database.transaction
    clock.read
}
ensures {
    success => inventory[item].available
        == old(inventory[item].available) - quantity

    failure => inventory[item].available
        == old(inventory[item].available)
} {
    let stock = inventory.lock(item)
        .or_error(ReservationError.item_not_found(item))?

    require stock.available >= quantity else {
        return ReservationError.insufficient_stock(
            item = item,
            requested = quantity,
            available = stock.available
        )
    }

    stock.available -= quantity

    return Reservation {
        order
        item
        quantity
        created_at = clock.now()
    }
}
```

The function signature exposes its domain types, possible failures, operational authority, transaction boundary, and postconditions without requiring the reader to inspect its dependencies.

## Core Design

### Canonical source

Sol intentionally provides one conventional representation for common constructs: one formatter, one import style, one absence type, one typed-error model, and minimal optional syntax. Equivalent formatting and stylistic variation should not consume review or model context.

### Types that carry domain meaning

Sol uses records, algebraic enums, generics, traits, distinct types, and refinements to make invalid states difficult to represent.

```sol
type UserId = distinct Uuid
type AccountId = distinct Uuid
type Percentage = refined Decimal where 0 <= self <= 100

enum AccountState {
    active
    frozen(reason: FreezeReason)
    closed(at: Instant)
}
```

There is no implicit null. Absence is represented by `Option<T>`, recoverable failure by `Result<T, E>`, and pattern matching is exhaustive.

### Ownership and resource safety

Values are immutable by default. Sol uses affine ownership and lexical borrowing to provide memory and resource safety without requiring a garbage collector. The bootstrap supports definite initialization, local-rooted record-field mutation and loans, path-sensitive affine moves, explicit owned mutation and lexical regions, and deterministic runtime-storage cleanup.

The bootstrap implements callable access modes over an underlying type: an unqualified parameter is owned, `borrow T` is shared, and `inout T` is exclusive. Borrowed call operands may be mutable/local-rooted record-field places. Paths overlap when they share a root and either field path prefixes the other; sibling fields are disjoint, and loans last through call completion. Source-defined functions, callbacks, recursion, and implementation methods copy `inout` values into the callee and write the complete value back on success. Bodyless host operations and top-level interpreter entries reject exclusive writeback. These qualifiers are preserved in exact callback and trait-method signatures but are not general types or values. `region name { ... }` is a Unit-valued statement: its label is retained for diagnostics and internal IR but is not a value and cannot be passed to an allocator. Regions nest, run ordinary type/effect checking, and are forbidden in expression positions, contracts, and refinement predicates.

```sol
region frame {
    let vertices = build_vertices(mesh)
    render(vertices)
}
```

`var name = initializer` infers one fixed slot type. `place = rhs` is a Unit-valued statement whose target may be a mutable local or its record-field path. The RHS is checked with the leaf's exact contextual type and evaluated before replacement. Assignment can exactly reinitialize a moved field or ancestor; a live old leaf is cleaned before transfer, while filling a moved hole performs no old-value cleanup. Moving an affine field makes that path and its ancestors unavailable but leaves sibling fields usable. Projected assignment conservatively rejects capability-bearing, function/operation, `Self`, and unresolved-generic leaf types until field-sensitive authority provenance exists. Active overlapping loans reject replacement, and affine writeback cannot cross an explicit-region boundary.

`var name: Type` declares an uninitialized authority-free local. A whole-local assignment must initialize it on every reachable path before any read, borrow, projection, compound update, or `modify`; terminating branches do not constrain the continuing path, and leaving a slot uninitialized is allowed. `+=`, `-=`, `*=`, `/=`, and `%=` are checked `Int64` read-modify-write operations. They read the target once, evaluate the RHS, check arithmetic, meter the write path, and commit atomically. `modify local { ... }` grants lexical write and `inout` authority for one initialized immutable owned binding or owned parameter. It does not mutate binding metadata or escape the block, and authority-bearing whole replacement remains conservatively rejected.

The bootstrap provides statement-only `loop { ... }` and `while condition { ... }` with payloadless `break` and `continue` targeting the nearest loop. Either form may carry one `invariant { ... }` clause followed by one `decreases { ... }` clause. Invariants are nonempty newline- or comma-separated exact `Bool` expressions; the decreases measure is one exact `Int64` expression. Both resolve in pre-body lexical scope, pass the contract purity firewall, and reject `result`, `old`, mutation, effect propagation, handlers, loops, and control transfer. A reachable backedge requires `decreases` unless the finalized callable row contains concrete unparameterized `diverge`; an open symbolic effect tail is not an exemption.

A finite ownership fixed point joins fallthrough and continue backedges: unavailable paths join by union and definite initialization by intersection. A `while` retains its zero-iteration condition-false exit; a `loop` falls through only through reachable breaks, whose states are joined for subsequent code. Loop-body locals unregister after every iteration, and block cleanup runs exactly once on fallthrough, continue, break, return, propagation, runtime error, and step-limit failure. Every invariant emits entry and preservation templates; a decreases measure emits nonnegative and strict-decrease templates. Owning IR retains their exact callable, loop, expression, type, span, cardinality, and lexical ownership, but the interpreter erases them completely. Labels, loop values, and proof discharge remain deferred.

Three statement-only termination forms build on `Never`. `panic message` requires one exact `Text` expression, evaluates it once, contributes the authority-free `panic` effect, and terminates with a structured interpreter diagnostic after exact lexical cleanup. `unreachable because { proof }` requires one pure exact `Bool` expression, emits a deterministic unresolved callable-owned obligation, contributes no runtime effect or step, and terminates statically; reaching it in the interpreter is a distinct defensive error. `require condition else { fallback }` evaluates one exact `Bool`; true continues as Unit without introducing a flow-sensitive refinement, while false executes an exact-`Never` fallback and propagates its return, panic, loop transfer, or divergence. Moves confined to the terminating fallback do not affect the continuing ownership state.

Every executable block records its exact successfully introduced `let` or `var` cleanup order, and every match arm records its pattern-binding cleanup order. Live owned locals are released in reverse order at lexical exit on normal completion, return, propagation, and runtime error; moved slots are skipped, borrowed parameters do not own cleanup, and Copy values still release interpreter storage. Return/propagation preserves its outgoing value before cleanup. While an explicit region is active, the bootstrap conservatively rejects every affine explicit return or affine propagation residual with `SOL-REGION-001`; Copy returns are allowed. Calls may consume region locals because this subset has no global storage or closures.

This cleanup is effect-free compiler/runtime storage reclamation only. Apart from the explicitly unstable observation hook described below, it invokes no Sol code, host operation, destructor, finalizer, resource `close`, or allocator action and is not a backend lifetime implementation. Index/dereference mutation, field-by-field first initialization, authority-bearing uninitialized slots, escaping references, loop labels/values and obligation discharge, flow-sensitive authority provenance, lifetime generics, allocator/arena APIs, first-class region values, user `Drop`, fallible close protocols, unsafe, FFI, and backend behavior remain deferred.

Owning IR represents every executable local or resolved-field access as one canonical typed place: a local or computed-value root followed by an exclusively owned root-to-leaf projection slice. Every root and field projection retains its exact source span and instantiated result type; nested chains are flattened, and independent validation rejects malformed, shared, orphaned, ill-typed, or authority-unsafe updates. Explicit DECLARE, MODIFY, LOOP, WHILE, BREAK, CONTINUE, PANIC, UNREACHABLE, and REQUIRE statements preserve initialization, lexical authority, structured transfer, and contiguous proof-only obligation slices; assignment retains its exact operator. Ownership tracks definite initialization, unavailable path frontiers, prefix-overlap loans, and bounded loop fixed points. The interpreter represents unbound slots and moved aggregate fields without synthetic values, meters projected reads and updates, stages all `inout` results before writeback, and commits only after every destination path passes its preflight. Index and dereference projection kinds remain reserved and rejected. No external inspection schema changes.

Unsafe operations are isolated, capability-gated, auditable, and excluded from ordinary safe code.

### Effects and capabilities

A function type states what the function may observe or change.

```sol
function submit_order(
    order: PendingOrder
) -> Result<OrderId, SubmitError>
effects {
    database.write<OrderStore>
    network.call<PaymentGateway>
    clock.read
    random.secure
}
```

Effects describe behavior; capabilities provide authority. Code cannot access ambient network, filesystem, clock, database, randomness, or process state without receiving the corresponding capability.

Capabilities can be closed into nominal, single-source wrappers. Every wrapper operation has an implementation, the source remains private, and `Self` names the original source root on both sides of the wrapper boundary:

```sol
capability ReadFileSystem derives_from source: capability FileSystem {
    function read(path: Text) -> Text
    effects { filesystem.read<Self> } {
        return source.read(path)
    }
}

let restricted = ReadFileSystem { source = filesystem }
```

Effect rows may be inferred locally, but public APIs expose a canonical effect signature. Tests can replace capabilities directly without a separate dependency-injection framework.

Exact capability-backed handlers replace one operation lexically while preserving the body's value and type:

```sol
capability TestClock {
    function now() -> Int64 effects { pure }
}

function deterministic(
    clock: capability Clock,
    provider: capability TestClock,
) -> Int64 effects { pure } {
    return handle clock.read<clock> with provider {
        timestamp(clock)
    }
}
```

The handled target must be one parameterized atom and must identify exactly one module-local capability member whose complete row is `{ clock.read<Self> }`. The authority must have exactly one known root because runtime dynamic authority matching is unsupported. The provider may conservatively have multiple possible roots: its root is not matched or subtracted, and it only needs to expose the same operation name and signature with a pure row. Its expression is evaluated outside the handler; calls in the body are handled deeply and lexically only when the instantiated effect has the exact target name and authority root. Other roots and effects remain in the row. This bootstrap form has no resumptions, row transformation, static or unparameterized targets, or dynamic authority matching.

Capability values and bound operations carry an interned, normalized nonempty set of possible lexical capability-parameter roots. A runtime value still has one root; the set is conservative provenance for computed `if` and `match` values. Immutable aliases, exact authority-preserving calls, restrictions, wrappers, and blocks preserve these sets, while reachable branches union them. Parameter- and `Self`-dependent effects expand to one ordinary parameter atom per possible root, so explicit rows must cover every possibility and inferred rows union them, including through recursive call components. Exact authority-return declarations and handler targets remain singleton guarantees.

### Contracts and progressive verification

Preconditions, postconditions, invariants, refinements, ghost state, examples, and properties are native language constructs.

```sol
function withdraw(
    account: Account,
    amount: Money
) -> Result<Account, WithdrawalError>
effects {
    pure
}
requires {
    amount > Money.zero
    account.state is active
    account.balance >= amount
}
ensures {
    success => result.id == account.id
    success => result.balance == old(account.balance) - amount
    failure => account.balance == old(account.balance)
}
```

The bootstrap compiler stores each `requires` and `ensures` clause and each
newline- or comma-separated condition in deterministic arenas, then lowers one
public semantic obligation per condition after effect inference. Contract
expressions resolve in a fresh declaration-signature scope, use ordinary
expression typing, must produce `Bool`, and may call only finalized-pure
functions, operations, or closed callbacks. Body locals and a derived
capability wrapper's private source are not part of its interface scope.
Postconditions expose an ordinary return through `result`. Outcome prefixes are
available only on declarations returning `Result<T, E>`: a `success =>`
condition exposes `T`, while `failure =>` has no result binding. Every
`old(expression)` records a distinct typed entry-state
snapshot. Nested `old` and `old(result)` are rejected. The deterministic public
table records owner, clause kind, outcome, predicate and type, result binding,
and snapshot metadata. It is a semantic template only: runtime checks,
call-site substitution, proof discharge, and normalized solver IR are not yet
implemented.

Loop specifications use the same purity boundary but retain separate deterministic
templates. Each invariant contributes entry and preservation obligations, and one
`decreases` expression contributes nonnegative and strict-decrease obligations.
These templates are compiler-internal owning IR, are not added to inspection v1,
and are never evaluated by the reference interpreter.

Verification is progressive rather than all-or-nothing. A project may choose among:

- static proof;
- generated runtime checks;
- test-only checks;
- explicitly trusted assumptions;
- solver-backed proof obligations.

The goal is to make useful contracts affordable in ordinary software while allowing high-assurance components to demand stronger proofs.

### Structured concurrency and protocol safety

Child tasks are scoped to their parent. Cancellation, timeout, retry, and failure propagation are explicit. Detached work requires a durable or process-level owner rather than an accidental task escape.

Protocols can encode legal resource states in types:

```sol
protocol Connection {
    state disconnected
    state connected(socket: Socket)
    state authenticated(socket: Socket, session: Session)

    transition connect(
        from: disconnected,
        address: Address
    ) -> connected

    transition authenticate(
        from: connected,
        credentials: Credentials
    ) -> Result<authenticated, AuthError>

    transition query(
        from: authenticated,
        request: Query
    ) -> Result<Response, QueryError>
}
```

Transactions, actors, channels, durable workflows, and sagas use the same explicit effect, capability, and state-transition model.

### Units, dimensions, and currencies

Physical dimensions and currency identity participate in type checking.

```sol
let distance: Meter = 100 meters
let elapsed: Second = 9.58 seconds
let speed: Meter / Second = distance / elapsed

let price: Money<USD> = 25.00 USD
```

Invalid arithmetic such as adding meters to seconds or combining USD and EUR without an exchange operation is rejected.

## AI-Native Development

Sol is not intended to replace human-readable source with natural-language programming. Instead, it gives humans, models, compilers, and review tools a shared semantic interface.

### Intent blocks

Intent metadata records the behavior and constraints a change must preserve.

```sol
intent transfer {
    purpose:
        "Move funds atomically between two accounts."

    preserve:
        - total money across both accounts
        - account identity
        - unrelated account fields

    forbid:
        - partial transfer
        - negative balance
        - transfer to the same account

    priority:
        correctness > observability > performance
}
```

Intent is not treated as magical executable prose. Structured fields become compiler, test, review, and proof obligations wherever possible.

### Semantic patches

Source changes can target declarations and behavior rather than raw text and line numbers.

```sol
patch billing.transfer::transfer {
    add parameter memo: Option<Text> after amount

    replace effect database.write
        with database.transaction

    preserve {
        existing error variants
        atomicity
        public behavior except memo support
    }
}
```

Stable semantic identities allow tools to follow declarations through moves and renames.

### Structured diagnostics

The compiler emits both human-readable messages and machine-readable diagnostics containing:

- stable error codes;
- semantic declaration identities;
- exact source spans;
- violated constraints;
- relevant inferred facts;
- bounded classes of valid repair;
- proof, effect, ownership, and compatibility consequences.

The compiler may suggest valid repair categories, but it should not silently choose an architectural tradeoff.

### Change-oriented compilation

Sol can report what a change altered, not merely whether the resulting program compiles.

```text
$ sol check-change HEAD~1..HEAD

Public behavior:
  + Transfer requests may include an optional memo.

Effects:
  ! billing.transfer.transfer now calls AuditService.

Schemas:
  no changes

Cost contracts:
  ! Worst-case network calls increased from 1 to 2.

Verification:
  842 checks passed
  3 checks generated
  1 proof obligation unresolved
```

This semantic change report is intended to become a first-class code-review artifact.

## Proposed Toolchain

The planned CLI centers the complete development loop around one tool:

```text
sol new                 Create a package
sol fmt                 Apply canonical formatting
sol check               Parse, type-check, and validate effects
sol prove               Discharge verification obligations
sol test                Run authored Boolean unit tests
sol build               Produce an executable or library
sol run                 Build and execute a target
sol doc                 Generate semantic documentation
sol effects             Inspect authority and effect graphs
sol patch               Apply or validate a semantic patch
sol check-change        Produce a semantic change report
sol explain             Expand a structured diagnostic
```

The language server is expected to expose typed syntax, inferred effects, ownership state, contract obligations, semantic references, API compatibility, and suggested repair classes directly to editors and agents.

## Bootstrap Compiler

The bootstrap compiler is written in C17 and currently provides a lossless lexer, a recovering parser for core declarations and authored tests, structured function contracts, refined predicates, loop invariants and termination measures, executable statement termination forms, and function-body expressions, an arena-backed syntax AST, a token-preserving canonical formatter, deterministic package-session definition IDs, stable top-level semantic IDs and reference occurrences, lexical, module, import, and declaration-owned type/effect resolution, exact invariant interning for built-in and user type applications, bounded first-order generic records/enums/functions/declared types, nominal distinct construction, checked refined predicate templates, coherent traits and exact implementations, type-directed method calls, callback-inferred effect-row instantiation, checked records and enums, exhaustive matching, semantic effect rows with local inference across statically known calls, capability-backed exact handlers, deterministic contract, loop, and unreachable-obligation templates, mutable whole-local slots and checked replacement, whole-local ownership/borrowing, statement-only loops and lexical regions with exact cleanup metadata, an owning typed IR, a deterministic compiler-internal reference interpreter, source-aware human or JSON diagnostics through `sol check`, deterministic authored-unit-test execution through `sol test`, and deterministic effect, lexical-authority, and call-edge inspection through `sol effects`.

```text
cmake -S . -B build -G Ninja -DSOL_ENABLE_SANITIZERS=ON
cmake --build build
cmake --build build --target test
./build/sol check tests/valid.sol
./build/sol check tests/packages/valid
./build/sol test tests/test_cli/pass.sol
./build/sol effects tests/valid.sol
./build/sol inspect tests/valid.sol
./build/sol fmt --check tests/valid.sol
```

`sol fmt <file-or-directory>` formats syntactically valid edition-2027 source in place; `--check` reports drift without writing, and `--stdout` prints one file. Directory formatting discovers files in package order, validates and formats every source before starting a transactional staged rewrite, rejects direct symbolic-link operands, preserves mode bits, and detects concurrent source changes before commit. The formatter preserves non-whitespace token and comment bytes, source order, commas, and deliberate hard line breaks. It canonicalizes spaces, four-space indentation, LF line endings outside comments, at most one blank line, one final newline, unary/binary operators, generic/effect angle delimiters, comparisons, braces, and delimiters. Every result is re-lexed, reparsed, checked for token preservation, and formatted a second time for byte idempotence.

`sol effects [--diagnostic-format=human|json] <file.sol|package-directory>` compiles the same bounded source/package subset and inspects the owning typed IR after frontend teardown. It reports every callable's normalized effect row and lexical authority, plus source-ordered direct-function, capability-operation, method, and callback call sites. Open callback rows retain their declaration-owned effect parameter. Callback targets and forwarded generic method dispatch remain explicitly dynamic rather than claiming a whole-program target set; constructors and contract-template calls are not graph edges. The deterministic version-1 JSON object is a bounded command result, not the stable inspection schema or a serialization of syntax, HIR, semantic tables, or internal IR. Effect rows disclose behavior and do not grant authority.

`sol inspect <file.sol|package-directory>` compiles the same bounded input and, on success, emits exactly one deterministic compact `sol.inspection` version-1 JSON object. Its six independently tagged artifacts are selected stable external projections of lossless source syntax, HIR definitions/resolutions/semantic occurrences, semantic type facts and major resolution tables, finalized effect rows and call instantiations, contract obligations/snapshots, and ordered diagnostics. Original source bytes are standard base64, paths and byte spans are package-relative/file-local, top-level targets use `sem:1:<32 hex>`, and explicitly labeled dense IDs are snapshot-local. This is not a raw table dump or a stable encoding of internal typed IR. Compatibility and exclusion rules are checked in under `schemas/README.md`; `schemas/sol-inspection-1.schema.json` provides structural validation. Compilation failures retain existing diagnostic JSON, load failures retain `sol.cli-error/1`, and usage returns 2. Construction failures write no partial inspection envelope; output transport can truncate an already constructed envelope and returns 1.

`include/sol/interpreter.h` exposes the library-only reference interpreter. Requests select a free-function callable or definition, provide owned-value arguments plus optional generic substitutions/evidence, deterministic limits, and optional host-operation and unstable cleanup-observer callbacks. Cleanup observation is synchronous and infallible, receives only a local ID and monotonic ordinal, and is counted in `result.cleanup_actions`; cleanup is not limitable because a started unwind must finish. No observation occurs before IR and complete argument validation, and the callback exposes no pointer. TEST callables require an explicit `test_entry` request and remain unavailable as source calls or function values. A host-operation result is borrowed host-owned data valid through callback return and may alias callback inputs; the interpreter validates and deep-clones it. Results own either an immutable runtime value or a stable structured diagnostic whose bounded source-file path remains valid independently of the IR. Operands execute in canonical formal order because the current IR intentionally canonicalizes named arguments and does not retain source operand order. Equality is statically limited to primitives and immutable aggregates whose complete recursive contents support structural equality; capabilities, functions, bound operations, generic parameters, `Self`, and aggregates containing them are rejected with `SOL-TYPE-027`. A runtime invariant remains as defense against malformed IR. External capability values recursively validate their definition-specific private source chain and exact shared root before binding; bound operations and handlers additionally require callable ownership and exact operation shape, preventing host or handler dispatch across capability domains. Host capability results are checked against retained result-authority metadata. Contracts remain ignored during execution.

Completed typed IR is ownership-checked in a separate transactional pass before interpretation. Local reads are explicitly `COPY`, `MOVE`, `SHARED`, `EXCLUSIVE`, or `UPDATE`; the validator independently recomputes access and use metadata. `Int64`, `Bool`, `Text`, `Unit`, `Never`, `Option`/`Result` with copyable arguments, and nongeneric records/enums/distinct/refined values with recursively copyable contents use the bootstrap structural `Copy` rule. Capabilities, functions, bound operations, `Self`, generic parameters, and generic nominal values are affine. Moves consume the whole local, and `SOL-OWNERSHIP-001` reports later use; borrow conflicts, owned use while loaned, and invalid places/reborrows/escapes use `SOL-OWNERSHIP-002` through `004`. Shared reborrows accept shared or exclusive parameters; exclusive reborrows require exclusive parameters. Borrowed affine parameters may be forwarded compatibly or used as immediate capability receivers but cannot escape through returns, owned calls, aggregates, variants, or bound-operation capture. Immediate capability calls and handlers borrow authority/provider locals; standalone bound operations remain owning. `if`, `match`, and short-circuit paths retain whole-local move joins. Successful syntax trees now validate every arena count/pointer pair, discriminant, active payload span, linked-arena owner, cycle, share, and orphan before semantic projection. Owning compiler-internal IR independently validates discriminants before payload access, generic-aware literal/operator/call/aggregate/branch/match/block/propagation type relations, callable returns, current-callable provenance roots, structural IDs, ownership, ordering, sharing, and nesting before execution. Source-driven tests census every successful AST expression kind and all owning-IR expression, statement, and pattern kinds; composite second-file package fixtures exercise populated expression and linked arenas at nonzero relocation offsets. Inspection v1 remains unchanged. Partial/field borrows, first-class references, projected mutation, loops, allocator regions, lifetime generics, user drop/destructors, close protocols, unsafe, and FFI remain future work.

Top-level `test "label" expression` declarations are zero-parameter, nongeneric, private, non-importable, and non-addressable from source. Their body is contextually `Bool`; `true` passes, `false` fails, and a runtime diagnostic fails the test. Tests cannot use annotations, visibility modifiers, contracts, authority clauses, or explicit effect clauses. Effects infer as for private function callers. `sol test [--diagnostic-format=human|json] <file.sol|package-directory>` compiles exactly as `sol check`, frees all package and frontend state, then discovers dedicated TEST definitions and exact callable IDs from owning IR in bytewise package-path and source-declaration order. Every test receives fresh default interpreter limits, ignored contracts, and no host callback or ambient capability. Execution continues after false results and runtime failures; exit status is 0 only when compilation and every test succeed, 1 otherwise, and 2 for usage errors. Output contains no timing.

Successful JSON execution emits one bounded versioned result object: `{"schema":"sol.test-results","version":1,"tests":[...],"summary":...}`. Each test entry has `path`, decoded `label`, `status` (`passed`, `false`, or `runtime_error`), and `diagnostic`; runtime diagnostics contain numeric `code`, `message`, owned `path`, and aggregate `offset`. Human runtime failures retain the declaration label/path and additionally report the diagnostic file and aggregate offset. The summary has integer `total`, `passed`, and `failed`. Compilation failures use the same diagnostic JSON as `sol check`, not the test-result schema. Package-load failures use `{"schema":"sol.cli-error/1","kind":"load","message":...,"path":...}` on stdout with no JSON-mode stderr; infrastructure failures use the same schema with kind `infrastructure`. JSON serializers emit valid UTF-8 sequences unchanged, use ordinary JSON escapes for syntax and controls, and represent each invalid input byte deterministically as the Unicode scalar escape `\u00XX`. Thus valid text retains its JSON string meaning; an invalid byte is a visible safe marker and is not claimed to round-trip through JSON scalar semantics. Human test labels likewise display valid UTF-8 normally and escape invalid bytes. Examples, authored or generated properties, boundary generation, shrinking, seeds, and ghost state remain roadmap work.

Width-based reflow, insertion or removal of trailing commas, import/declaration sorting, comment reflow, and semantic effect/order normalization remain future formatter work. Malformed source is diagnosed and never rewritten.

Directory checking recursively discovers regular `.sol` files in deterministic bytewise path order and treats them as one dependency-free package. Each file declares its module; files with the same module path share declarations, while each file has its own explicit `use module.path.Symbol` scope. Cross-module imports require a public declaration. Imported types, functions, generic bounds, traits, effects, and contract calls use package-wide checked identities. Import cycles are permitted because this subset has no top-level initialization. Direct single-file checking remains compatible with the earlier bootstrap and parses but does not enforce imports.

Every top-level declaration receives a versioned 128-bit semantic ID separate from its dense package-session `SolDefId`. Without an annotation, identity derives from normalized module path, declaration kind, and declaration name and is therefore independent of file and declaration ordering. A named public declaration may use `@stable("package-local-token")`; retaining that token preserves identity through module moves and renames. Duplicate, malformed, private, and implementation identity annotations are rejected. HIR records declaration, import, expression, type, implementation-trait, and trait-bound occurrences with both their dense target and semantic target ID. Member/local identities, package-global identity across dependencies, serialized schema stability, compatibility validation beyond collision detection, and a public Sol IR remain future work.

This bounded package subset has no manifest, external dependencies, package aliases, re-exports, relative imports, grouped imports, wildcard imports, import aliases, module-to-filesystem-path requirement, or imported extension-method search. Consequently explicit stable tokens are package-local rather than globally namespaced. Directory traversal does not follow nested symbolic links. Multiple files may contribute to one module, and private declarations are module-visible in this subset; the target language's distinct private/module/package visibility levels remain future work.

This implementation is an experimental edition-2027 front end. Function bodies are parsed, names are resolved into an initial HIR, and expressions, calls, returns, built-in and bounded user generic types/functions, records, enum constructors, matches over user enums and `Bool`, and exact `handle effect<authority> with provider { body }` expressions are checked. Structural function types carry normalized semantic effect rows, including one optional declaration-owned row parameter in the bounded generic subset, and calls through function values propagate instantiated rows through inference and explicit checks. Exact functions and bound operations coerce contextually to compatible callback shapes; their finalized effects must fit the callback row. Capability-valued members can declare `authority { result derives_from Self }`, while exact capability-returning functions can derive result authority from a named capability parameter. Both forms preserve provenance through aliases and nominal, type-changing restrictions. Closed nominal capability wrappers have exactly one private capability source, checked member bodies, cycle rejection, and root-preserving construction and `Self` substitution without subtyping or coercion. Explicit declaration rows form modular boundaries, while omitted private functions infer a least union row across direct operations and statically known calls, including self- and mutually recursive call-graph components and formal-argument and `Self` substitution through immutable capability and bound-operation aliases. Exact handlers retain source and provider operation metadata for runtime interception and subtract matching instantiated name-and-root atoms inside the body without removing call-graph edges; provider evaluation and residual effects remain outward. Computed capability and bound-operation provenance joins union every reachable lexical parameter root for `if` and `match`, and dependent effects expand across the complete set. Public functions and capability members require explicit rows. Lambdas, resumptions, transformed open handler rows, dynamic handler targets, computed joins of distinct function identities, parameter-dependent callback authority rows, and `Self`-dependent bound-operation coercions are not yet supported. Unknown computed provenance remains unsupported. Contract blocks remain delimiter-checked. Ownership and execution are also not implemented yet.

User generics are deliberately bounded to type parameters on records, enums, distinct/refined declarations, and free functions plus at most one trailing `effects E` parameter on a free function. Type applications are invariant and require every type argument when written. Type arguments are either all explicit (`identity<Int64>(1)`) or inferred from value arguments (`identity(1)`); generic declared-type constructors require explicit arguments. Effect arguments cannot be written explicitly; they are inferred only from structural callback parameters such as `function(T) -> U effects E`. The least authority-independent closed row is substituted into the owning function's explicit `effects { E ... }` row and recorded per call. Multiple bounds, defaults, variance, higher-kinded/lifetime/const parameters, explicit or multiple effect arguments, all result-position row parameters, generic capabilities or members, recursive row polymorphism, generic callback coercion, open-row handler transformation, and separately compiled dependency instantiation are not supported. Each generic body, contract, and refinement predicate is checked once symbolically.

The bounded trait subset uses nongeneric `trait` declarations, `implementation Trait for Type` declarations, and at most one inline `T: Trait` bound on each free-function type parameter. Trait requirements and implementation methods begin with `self: Self`, have explicit canonical parameter/result types and exact closed effect rows, and cannot declare defaults, contracts, authority clauses, generic parameters, or row parameters. Implementation targets are exact closed built-in, record, enum, distinct/refined, or fully applied user types; one coherent implementation must provide every requirement exactly once with no extra methods. Immediate `value.method(...)` calls resolve from a concrete receiver's exact implementation or a rigid parameter's declared bound, record deterministic checked metadata, and use the implementation or requirement row respectively. Generic bound evidence forwards through generic calls. Method values, generic/blanket/conditional implementations, multiple bounds, associated items, trait inheritance, specialization, trait objects, imported extension lookup, and dependent method effects remain unsupported.

`type Name<T> = distinct Representation` creates a fresh nominal identity with no implicit conversion to or from its representation. Distinct construction uses exactly one positional argument, and generic construction requires explicit type arguments. `type Name<T> = refined Representation where predicate` binds contextual `self` at the representation type, requires a `Bool` predicate, applies the finalized contract purity firewall, and emits a deterministic type-owned obligation template. Refined construction is intentionally unsupported because the bootstrap cannot prove or execute validation; existing refined values may only be propagated at their exact type. `using` predicates, inline refinements, base projection, predicate assumptions, refined pattern reasoning, runtime validation, proof discharge, capability-containing representations, and cyclic representations remain unsupported.

Mixed capability and bound-operation provenance is exposed by the type table as checked, interned root sets. Effect checking defensively recomputes propagation and validates canonical sets before expanding every dependent atom across all possible roots.

Effect rows disclose behavior but never grant authority. In executable bootstrap source, `panic` and `diverge` are the only compiler-defined authority-free atoms. Every other non-pure atom requires a lexical capability parameter, or `Self` on a capability member; unresolved static authorities such as `network.call<Production>` and unparameterized resource effects are rejected with `SOL-EFFECT-010`. Imports add names but no authority. Closed structural callback rows may contain only `panic`, `diverge`, and the bounded row parameter because callback types cannot capture lexical capability roots. Static/unparameterized handlers, package-global authority, manifests, host wiring, unsafe gates, FFI, and a broader intrinsic-effect registry remain future work.

Concrete remaining bootstrap work is tracked in [TODO.md](TODO.md).

## Compiler Architecture

The proposed compiler pipeline is:

```text
canonical source
    -> parser and lossless syntax tree
    -> name resolution and stable semantic graph
    -> type, ownership, effect, and capability analysis
    -> contract and proof-obligation generation
    -> canonical Sol IR
    -> optimized machine IR
    -> native, WebAssembly, or embedded backend
```

The semantic graph and canonical IR are public tool interfaces, not private compiler implementation details. They support semantic search, refactoring, diagnostics, documentation, patching, compatibility checks, and AI context bundles.

Initial implementation work should favor an established code-generation backend rather than inventing a complete optimizer and machine-code generator. Native and WebAssembly output are the primary early targets.

## Design Principles

1. **Intent is part of the program.** Important assumptions should be represented in types, effects, contracts, protocols, schemas, or structured intent.
2. **Authority is explicit.** Code receives the capabilities it needs and no ambient authority by default.
3. **Safe defaults, visible escape hatches.** Mutation, allocation, panic, unsafe behavior, detached work, reflection, and FFI are deliberate.
4. **Verification is progressive.** Ordinary software benefits from contracts without requiring every project to become a theorem-proving exercise.
5. **Changes are semantic objects.** The toolchain reports altered behavior, authority, schemas, costs, and proof obligations.
6. **One canonical representation.** Regularity improves review, tooling, reproducibility, and model reliability.
7. **Local reasoning scales.** A declaration should expose enough context to understand and safely modify it without reading the entire repository.
8. **Human approval remains meaningful.** Tools may generate and validate changes, but policy and architectural decisions remain explicit.

## Non-Goals

Sol is not intended to be:

- an English-like or prompt-only programming language;
- a source-compatible replacement for Rust, C++, TypeScript, or Python;
- a proof assistant disguised as a general-purpose language;
- a language with unrestricted macros, implicit conversions, ambient authority, or hidden control flow;
- a promise that types eliminate business-logic or security vulnerabilities;
- optimized primarily for code-golf, syntax novelty, or the shortest possible programs.

## Minimum Viable Sol

The first useful implementation must demonstrate more than a new surface syntax. The proposed minimum vertical slice includes:

- an editioned parser and canonical formatter;
- records, enums, generics, traits, `Option`, `Result`, and exhaustive matching;
- affine ownership and common lexical borrows;
- parameterized effect rows and capability injection;
- `requires`, `ensures`, invariants, `old`, and `result`;
- runtime and solver-backed contract policies;
- stable semantic identities and a canonical semantic graph;
- structured JSON diagnostics;
- a basic semantic patch API;
- semantic change reports;
- native or WebAssembly executable output.

A compiler that implements ownership but omits explicit effects, executable contracts, semantic identity, and change-oriented tooling would not yet validate the central Sol thesis.

## Roadmap

### Phase 0 — Executable language model

Define the grammar, core calculus, effect-row behavior, ownership rules, contract semantics, and serialized diagnostic/IR schemas. The bootstrap's internal typed IR is intentionally distinct from those future stable schemas. Build small interpreters and model checkers before committing to production compiler architecture.

### Phase 1 — Front end and interpreter

Implement canonical parsing and formatting, algebraic data types, pattern matching, basic traits, refinement checks, effect inference, capabilities, effect/authority inspection, structured diagnostics, and an interpreter suitable for language tests.

### Phase 2 — Ownership and executable output

Extend the bounded ownership/region foundation with user resource protocols, unsafe boundaries, FFI foundations, and initial native or WebAssembly code generation.

### Phase 3 — Verification

Integrate proof-obligation generation, solver-backed contracts, ghost state, generated boundary tests, property testing, proof caching, and proof-oriented diagnostics.

### Phase 4 — Semantic tooling

Deliver stable semantic IDs, public IR, semantic patches, context bundles, change reports, compatibility analysis, IDE integration, and agent-facing APIs.

### Phase 5 — Concurrency and ecosystem

Stabilize structured concurrency, actors, protocol types, transactions, durable workflows, package signing, reproducible builds, capability policies, and the standard library.

## Design Influences

Sol combines ideas that currently live across multiple language families and compiler systems:

- **F\*** and **Pulse** — refinement types, computational effects, proof-oriented programming, mutable-state and concurrency verification;
- **Dafny** — readable contracts, framing, ghost state, and automated verification;
- **Rust** — ownership, borrowing, explicit failure, and production systems programming;
- **Koka** — effect rows, effect polymorphism, and handlers;
- **Idris 2** — dependent and linear types for resource protocols;
- **SPARK** — contract-driven high-assurance engineering;
- **Pony** — reference capabilities and race-safe concurrency;
- **MLIR** — extensible, inspectable intermediate representations;
- **WebAssembly Component Model** — typed, composable cross-language components.

Sol’s differentiating claim is not any single feature. It is the integration of safe implementation, explicit authority, progressive proof, stable semantic representation, and change-oriented tooling into one coherent workflow.

## Contributing

The project is currently best served by design critiques, executable semantics, prototype implementations, adversarial examples, and small vertical slices that test whether the core concepts work together.

High-value contribution areas include:

- grammar and canonical formatting;
- effect rows and capability passing;
- ownership/effect/handler interactions;
- contract lowering and SMT integration;
- stable semantic identity across refactors;
- machine-readable diagnostic and patch schemas;
- semantic API and change-diff algorithms;
- protocol-typed concurrency and transactions;
- WebAssembly and native backend experiments;
- representative application case studies.

Proposals should include motivating examples, rejected alternatives, interactions with existing language features, and a testing or verification strategy.

## Current Status

Sol is a specification-first project. Syntax, semantics, implementation choices, and names may change substantially while the executable core is developed. Code examples in the design document describe intended behavior and are not yet guaranteed to compile.

The most important early experiment is not whether Sol wins a benchmark. It is whether a human or model can modify a nontrivial system with less hidden context, receive a bounded set of meaningful compiler obligations, and demonstrate that the requested behavior changed while unrelated behavior did not.
