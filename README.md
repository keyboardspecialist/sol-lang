# Sol

**Explicit intent, safe implementation, progressive verification, and reliable change.**

Sol is an experimental programming language and C17 compiler project for software
maintained by humans and AI systems. Its core combines domain types, ownership,
effects, explicit capability authority, executable contracts, and semantic identity.
The aim is to make important assumptions inspectable rather than reconstructing
them from conventions and repository history.

**Status, September 9, 2026:** the baseline at `6ec4ba9` has an executable reference
interpreter and CLI, not a production-ready compiler or stable toolchain. E1-E6,
P1, and P2.1-P2.6 are complete; P2 remains open. There is no backend, production
runtime ABI, `sol build`, full public IR, or SMT proof discharge.

[TODO.md](TODO.md) is the sole live status and execution-order ledger.
[Compiler status](docs/compiler-status.md) records the detailed baseline and its
compatibility limits; [project analysis](docs/project-analysis.md) explains the
product hypothesis, risks, and proposed experiments.

## Why Sol?

Maintenance often depends on assumptions that a signature does not reveal: which
values may be absent, which authority a component holds, which operations can
fail, and what a change must preserve. Sol's strong core makes several of these
questions explicit:

- **Domain meaning:** nominal distinct/refined types, records, enums, `Option`, and `Result` distinguish values and outcomes.
- **Local reasoning:** explicit public interfaces coexist with bounded local type and effect inference.
- **Authority:** effects describe possible behavior; capabilities grant permission to perform it. Imports grant names, not authority.
- **Ownership:** affine moves, lexical borrows, checked mutation, and deterministic cleanup make value use and lifetime visible.
- **Executable intent:** callable contracts and direct refined construction can check predicates during reference execution.
- **Tooling facts:** stable top-level identities, structured diagnostics, and selected inspection projections give tools checked context.

These are useful ingredients, not evidence of a product advantage. It has **not
been empirically established** that Sol makes human or AI maintenance safer,
faster, or cheaper. Generated code faces the same compiler and host boundaries as
human-written code; unchanged interfaces do not establish unchanged behavior.

The proposed first-domain direction is a **capability-restricted hosted
business-logic or validation component**. M1 must still select the concrete user,
workload, host boundary, and evaluation charter. No workload has been chosen by
this documentation, and Sol is not yet a complete service ecosystem.

## Try It

The bootstrap is POSIX-only. Building requires a C17 compiler, CMake 3.17 or newer,
and Ninja for the command below. The default test configuration also requires
Python 3. Run these commands from the repository root:

```sh
cmake -S . -B build -G Ninja -DSOL_ENABLE_SANITIZERS=ON
cmake --build build
cmake --build build --target test
./build/sol run tests/run_cli/console.sol
```

The final command runs this complete existing
[console fixture](tests/run_cli/console.sol), not a target-language sketch:

```sol
module run.console

capability Console {
    function write(value: Text) -> () effects { console.write<Self> }
}

@entry
public function launch(console: capability Console) -> ()
effects { console.write<console> } {
    console.write("hello")
    console.write(" world\n")
}
```

It prints `hello world` followed by a newline and exits with status 0. The entry
function explicitly receives console authority; the trusted CLI host grants the
exact supported operation. This is interpreter execution, not compilation to an
executable artifact.

Explore the other implemented commands:

```sh
./build/sol check tests/valid.sol
./build/sol check tests/packages/valid
./build/sol test tests/test_cli/pass.sol
./build/sol effects tests/valid.sol
./build/sol inspect tests/valid.sol
./build/sol fmt --check tests/valid.sol
```

`fmt --check` reports formatting drift without writing. `fmt <path>` rewrites
syntactically valid files, including transactional directory formatting.
Directory inputs form one deterministic, dependency-free package; they do not
resolve a manifest or download dependencies.

For a larger executable example, [the E6 application](tests/conformance/e6)
combines three modules, authored Boolean tests, ownership, generics/traits,
contracts, typed errors, and exact console/arguments/configuration hosting:

```sh
./build/sol test tests/conformance/e6
./build/sol run --config=mode=e6 tests/conformance/e6 -- fixture
```

The success path prints `E6 ok` followed by a newline. E6 is a conformance fixture,
not a demonstrated first-user workload or a usability benchmark.

## Supported Scope

The current edition-2027 subset is deliberately bounded. See the
[detailed snapshot](docs/compiler-status.md) before relying on a particular form.

| Area | Implemented baseline |
| --- | --- |
| Language core | `Int64`, `Bool`, `Text`, Unit/Never, records, enums, tuples, nominal distinct/refined types, `Option`/`Result`, exhaustive recursive patterns and pure guards |
| Abstraction | Bounded first-order generics, one inline trait bound, exact coherent implementations, structural callbacks, callback-inferred effect rows |
| Ownership/control | Owned/shared/exclusive parameters, local/field/tuple places, partial moves, definite initialization, checked mutation, regions, loops, `panic`, `require`, and unresolved proof-backed `unreachable` |
| Authority | Lexical capability roots, root-preserving wrappers and returns, conservative mixed-root provenance, exact deep lexical handlers |
| Execution | `check`, `test`, `run`, `effects`, `inspect`, `fmt`; deterministic resource limits and bounded explicit hosting |
| Contracts | Typed pure templates, runtime callable CHECK policy, entry-state `old`, applicable `ensures`, and executable direct refined construction |
| Tool interfaces | Structured diagnostics, package-local stable top-level identities, selected versioned inspection projections |
| Experimental internals | Full frozen-E6 callable CFG MIR; concrete specialization, representation, target layout, and semantic-operation plans through P2.6 |

Important boundaries are part of the behavior, not incidental missing polish:

- Named arguments execute in **canonical formal-parameter order**, not written operand order. Reordering a signature can therefore change effects.
- `borrow` and `inout` are callable access modes, not general reference values. Bodyless host operations and top-level interpreter entries reject exclusive writeback.
- Only `panic` and `diverge` are compiler-defined authority-free effect atoms. Exact handlers require a singleton target root and a matching pure provider operation; they are not general resumptive handlers.
- Direct refined construction can fail a runtime predicate. Recoverable validation APIs, safe base projection, and refinement proof reasoning are not implemented.
- Loop invariants and decreases are typed proof templates but runtime-erased; unreachable obligations are unresolved, with a defensive error if execution reaches the statement.
- Hosting supplies bounded console output, argument access, and explicit configuration snapshots. There is no ambient filesystem, network, clock, randomness, console input, or live environment access.
- The internal production owners impose finite callable-producer closure restrictions beyond reference-interpreter support. They are not the CLI execution pipeline.

### Evidence Labels

Keep these categories separate when describing a guarantee:

| Label | Meaning |
| --- | --- |
| **Declared** | Source states an intent or predicate; acceptance and typing alone do not prove it. |
| **Runtime-checked** | A supported check was evaluated on a particular execution under the selected policy. `sol run` and `sol test` enable callable checks; direct refined construction always checks. |
| **Proved-under-assumptions** | A property has a proof within explicit assumptions and a modeled boundary. The bootstrap does not provide SMT discharge; do not apply this label merely because an obligation exists. |
| **Unknown** | A property is unresolved, unsupported, omitted, or outside the checked/proved boundary. Unknown is not success or an empty dependency set. |

`pure` does not mean total or automatically proved. Runtime checks do not prove
all executions; capability permission does not establish business authorization,
tenant isolation, or information-flow safety. Tests are evidence for tested cases.

### Target, Not Delivered

The design specification extends beyond the executable subset: public semantic
IR, context bundles, semantic patches/change reports, full editor integration,
solver-backed proof, ghost state/properties, richer libraries and numerics,
resource/allocator APIs, unsafe/FFI, concurrency, protocols, transactions, and
durable workflows remain future or separately gated work. `sol new`, `prove`,
`build`, `doc`, `patch`, `check-change`, and `explain` are not current CLI commands.

`sol inspect` exposes selected stable projections, **not full public IR**, raw
owning IR, MIR, a deserialization format, or a guaranteed semantic cache key.
Its versioning and exclusion rules are in [schemas/README.md](schemas/README.md).

WebAssembly is the **first production target**, not an existing backend or current
Component Model integration. Native output remains a deferred long-term goal.
The reference interpreter is retained as a semantic reference, not a new VM plan.

## Roadmap

This orientation mirrors the [live execution cursor](TODO.md#execution-cursor),
not a second checklist. E1-E6 established bounded application execution; P1
completed frozen-core CFG MIR; P2.1-P2.6 supplied concrete planning through
source-independent predicate and import-contract bodies. **P2 is still open.**

1. **P2.7:** freeze symbols and whole-program linkage. This is next overall and next production.
2. **M1:** select a first-user workload and publish the experiment charter and subsequent interleaving. If infeasible, record the blocker and continue P2.8 without inventing prerequisites.
3. **P2.8:** freeze, render, and census the complete concrete-program contract, without selecting a runtime ABI or emitting Wasm.
4. **P3:** define the target-independent runtime ABI, owned-value operations, failure/cleanup policy, host boundary, and exact handler ABI.
5. **P4-P5:** integrate a pinned Wasm backend/host adapter, then reproducible `sol build` artifacts and interpreter/Wasm differential conformance.

After P2.8, **M2-M5 may proceed independently alongside P3**, subject to their
experiment dependencies, staffing, and exclusive file ownership. All M rows are
open proposals: M2 creates declaration-centered context packets after M1
feasibility; M3 depends on M2 for conservative checked-snapshot deltas; M4 depends
on M2/M3 for ordinary-edit validation and human approval; M5 follows M4 with a
protected held-out comparison against source-only work. M4E is an optional basic
editor slice after M1/M2 projection feasibility, not full LSP or an M5 prerequisite.

The early **P3.W1** Wasm ABI integration experiment is timeboxed after P3.1 has
tested call/result/failure conventions. Allocation, cleanup, and host imports
enter only after the relevant P3.2-P3.4 conventions are tested. It informs the
backend choice; it does not complete P4, component support, E6 Wasm execution, or
build tooling. One coordinator maintains the sole next-overall cursor in TODO.

The M track uses existing inspection, interpreter execution, ordinary edits, and
protected tests/approval. It does not wait for full public IR, SMT, or patch syntax.
Its evaluation must report negative and inconclusive results as well as gains.
Broader language work stays deferred unless a separately approved, workload-gated
slice demonstrates a need.

## Documentation

| Document | How to use it |
| --- | --- |
| [TODO](TODO.md) | Sole live work status, dependencies, acceptance criteria, and execution cursor |
| [Compiler status](docs/compiler-status.md) | Detailed `6ec4ba9` snapshot: APIs, packages, ownership, effects, inspection, MIR, materialization, representation, layout, operations, and limitations |
| [Project analysis](docs/project-analysis.md) | Dated document-based assessment and proposed maintenance-workflow evaluation, not a fresh implementation audit |
| [Design Specification v0.2 PDF](Sol_Programming_Language_Design_Specification_v0.2.pdf) / [Typst source](docs/specification.typ) | September 9, 2026 revision, implementation baseline `6ec4ba9` through P2.6; target-language design with explicit implementation boundaries, not all examples executable |
| [Current-State Audit PDF](Sol_Current_State_Audit.pdf) / [Typst source](docs/current-state-audit.typ) | September 9, 2026 document-based assessment of `6ec4ba9` through P2.6, replacing the August 25 assessment; not a fresh full-suite test report |

Both documents' authoritative editable sources are Typst; generated PDFs are
not edited directly. With Typst 0.15.1, rebuild them offline using:

```sh
typst compile docs/specification.typ Sol_Programming_Language_Design_Specification_v0.2.pdf
typst compile docs/current-state-audit.typ Sol_Current_State_Audit.pdf
```

When CMake finds Typst at configuration time, `cmake --build build --target manual`
provides the same manual build. The [v0.1 PDF](Sol_Programming_Language_Design_Specification_v0.1.pdf)
is retained as a historical artifact.

## Contributing

Start with TODO's current cursor and keep changes bounded to an explicit objective,
preserved behavior, and validation strategy. Adversarial examples, compiler
regressions, reference-execution tests, and realistic maintenance tasks are useful
contributions. New language breadth is not an implicit prerequisite for production
delivery or the proposed product experiment.

Sol draws on refinement/contract systems such as F*, Pulse, Dafny, and SPARK;
ownership and capability systems such as Rust and Pony; effect work such as Koka;
and typed resource and compiler ideas from Idris 2 and MLIR. These influences are
not claims of equivalent implementation, assurance, or performance. Sol does not
aim for source compatibility, unrestricted macros, ambient authority, or a promise
that types eliminate business-logic and security vulnerabilities.
