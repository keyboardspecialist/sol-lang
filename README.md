# Sol

**Explicit intent, safe implementation, progressive verification, and reliable change.**

Sol is an experimental programming language and C17 compiler project for software
maintained by humans and AI systems. Its core combines domain types, ownership,
effects, explicit capability authority, executable contracts, and semantic identity.
The aim is to make important assumptions inspectable rather than reconstructing
them from conventions and repository history.

**Status, September 26, 2026:** the September 9 baseline at `6ec4ba9` has an
executable reference interpreter and CLI, not a production-ready compiler or stable
toolchain. The current worktree completes E1-E6, P1, P2, and P3.1 for the frozen E6
profile; P3.2 is the next overall item and production checkpoint. M1 evaluated an
unvalidated candidate and stopped as infeasible because independent governance and
protected evaluation material are unavailable. There is no backend, production
physical ABI, complete runtime lowering, `sol build`, full public IR, or SMT proof
discharge.

[TODO.md](TODO.md) is the sole live status and execution-order ledger.
[Compiler status](docs/compiler-status.md) records the detailed baseline,
current-worktree P2.7-P3.1 addenda, and compatibility limits;
[project analysis](docs/project-analysis.md) explains the product hypothesis,
risks, and proposed experiments.

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

The evaluated first-domain candidate is a **capability-restricted hosted expense
policy component** with six read-only inputs, one output publication, and pure
decision logic. It is unvalidated: no actual user or policy owner adopted it, and
no protected held-out material, externally controlled verifier, or independent
adjudicator exists. The [M1 report](experiments/m1/README.md) records the resulting
governance hard stop; Sol is not yet a complete service ecosystem.

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
| Experimental internals | Full frozen-E6 callable CFG MIR; complete P2 `SolMirConcreteProgram`; and separate P3.1 `SolMirRuntimeConventions` for target-independent calls, results, runtime imports, failure provenance, and E2 exits, with independent validation and canonical rendering |

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
completed frozen-core CFG MIR; P2 completed concrete planning through the
validated, canonically rendered whole-program owner for the frozen E6 profile;
P3.1 froze the separate target-independent call/result/failure contract.

1. **M1 (completed as infeasible):** evaluated an unvalidated expense-policy candidate and stopped because no independent participant, protected held-out material, externally controlled verifier, or independent adjudicator exists.
2. **P2 (complete):** P2.8 froze, validated, rendered, and censused the complete concrete-program contract without selecting a runtime ABI or emitting Wasm.
3. **P3.1 (complete):** froze target-independent receiver-first signatures, direct/indirect calls, access/result/failure conventions, stable runtime-import identities, canonical failure provenance, and E2 exit mapping over the immutable P2 owner.
4. **P3.2 (next overall and production checkpoint):** define bounded allocation and owned-value operations without widening the frozen language profile.
5. **P4-P5:** integrate a pinned Wasm backend/host adapter, then reproducible `sol build` artifacts and interpreter/Wasm differential conformance.

**M2-M5 remain gated** unless M1 is reopened and completed feasibly with independent
governance and protected evaluation inputs. If that occurs, M2 would create
declaration-centered context packets after P2.8; M3 would depend on M2 for
conservative checked-snapshot deltas; M4 would depend on M2/M3 for ordinary-edit
validation and human approval; and M5 would follow M4 with a protected held-out
comparison against source-only work. M4E would remain an optional basic editor
slice after M1/M2 projection feasibility, not full LSP or an M5 prerequisite.

The early **P3.W1** Wasm ABI integration experiment is eligible only because P3.1
has tested call/result/failure conventions, but it does not replace the P3.2 cursor.
Allocation, cleanup, and host imports
enter only after the relevant P3.2-P3.4 conventions are tested. It informs the
backend choice; it does not complete P4, component support, E6 Wasm execution, or
build tooling. One coordinator maintains the sole next-overall cursor in TODO.

A feasibly reopened M track would require a protected external experiment contract,
existing inspection, ordinary edits, independent verification, and
`approve`/`reject`/`escalate` adjudication. Its bounded local evidence record would
be deterministic, unsigned, and content-addressed: hashing supports integrity and
addressing, not producer authentication, authority separation, or deployment
provenance. It would not wait for full public IR, SMT, or patch syntax, and it would
report negative and inconclusive results as well as gains. Detailed gates and task
boundaries remain solely in TODO.

## Documentation

| Document | How to use it |
| --- | --- |
| [TODO](TODO.md) | Sole live work status, dependencies, acceptance criteria, and execution cursor |
| [Compiler status](docs/compiler-status.md) | Detailed `6ec4ba9` snapshot plus current-worktree P2.7-P3.1 addenda: APIs, packages, ownership, effects, inspection, MIR, complete concrete-program ownership, runtime conventions, validation/rendering, and limitations |
| [Project analysis](docs/project-analysis.md) | Dated document-based assessment and proposed maintenance-workflow evaluation, not a fresh implementation audit |
| [AI-native workflow review notes](sol_ai_native_workflow_notes.pdf) | Advisory external review input incorporated into the live roadmap; not a status, execution-order, specification, or implementation authority |
| [Design Specification v0.2 PDF](Sol_Programming_Language_Design_Specification_v0.2.pdf) / [Typst source](docs/specification.typ) | September 26, 2026 source revision; September 9, 2026 implementation baseline `6ec4ba9` through P2.6. The source has clearly marked current-worktree P2.7-P3.1 addenda; the unchanged checked PDF remains pre-P2.8 until Typst 0.15.1 is available to regenerate it. Target-language design with explicit implementation boundaries; not all examples are executable. |
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
