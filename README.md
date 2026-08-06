# Sol

**A programming language for explicit intent, safe implementation, progressive verification, and reliable change.**

> **Project status:** Sol is currently a language-design and compiler-research project. A bootstrap compiler front end is under development, but no production-ready compiler or stable toolchain exists yet.

Sol is designed for software written and maintained collaboratively by humans and AI systems. It treats effects, authority, contracts, resource behavior, semantic identity, and change consequences as first-class parts of the program rather than context that must be reconstructed from conventions, comments, and repository archaeology.

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

Values are immutable by default. Sol uses affine ownership and lexical borrowing to provide memory and resource safety without requiring a garbage collector. Explicit regions support arenas, frame allocators, embedded systems, and bounded-lifetime workloads.

```sol
region frame {
    let vertices = Array<Vertex>.allocate(count, in: frame)
    render(vertices)
}
```

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

Effect rows may be inferred locally, but public APIs expose a canonical effect signature. Tests can replace capabilities directly without a separate dependency-injection framework.

### Contracts and progressive verification

Preconditions, postconditions, invariants, refinements, ghost state, examples, and properties are native language constructs.

```sol
function withdraw(
    account: Account,
    amount: Money
) -> Account
requires {
    amount > Money.zero
    account.state is active
    account.balance >= amount
}
ensures {
    result.id == account.id
    result.balance == account.balance - amount
    result.state == account.state
}
effects {
    pure
}
```

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
sol test                Run examples, properties, and tests
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

The bootstrap compiler is written in C17 and currently provides a lossless lexer, a recovering parser for core declarations and function-body expressions, an arena-backed syntax AST, deterministic definition IDs, lexical name resolution, structural `Option`/`Result` types, checked records and enums, exhaustive matching, semantic effect rows with local inference across statically known calls, and structured human or JSON diagnostics through `sol check`.

```text
cmake -S . -B build -G Ninja -DSOL_ENABLE_SANITIZERS=ON
cmake --build build
cmake --build build --target test
./build/sol check tests/valid.sol
```

This implementation is an experimental edition-2027 front end. Function bodies are parsed, names are resolved into an initial HIR, and expressions, calls, returns, built-in generic types, records, enum constructors, and matches over user enums and `Bool` are checked. Explicit effect rows form modular boundaries, while omitted private acyclic functions infer a least union row across direct operations and statically known calls, including formal-argument and `Self` substitution through immutable capability aliases. Public functions, capability members, and recursive functions require explicit rows. Bound capability operations, computed capability provenance, recursive fixed-point inference, and general higher-order effect types are not implemented. Contract blocks remain delimiter-checked. User-defined generics, ownership, and execution are also not implemented yet.

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

Define the grammar, core calculus, effect-row behavior, ownership rules, contract semantics, and serialized diagnostic/IR schemas. Build small interpreters and model checkers before committing to production compiler architecture.

### Phase 1 — Front end and interpreter

Implement canonical parsing and formatting, algebraic data types, pattern matching, basic traits, refinement checks, effect inference, capabilities, structured diagnostics, and an interpreter suitable for language tests.

### Phase 2 — Ownership and executable output

Add affine ownership, borrowing, regions, deterministic cleanup, unsafe boundaries, FFI foundations, and initial native or WebAssembly code generation.

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
