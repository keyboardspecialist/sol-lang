# Sol Project Analysis

## Scope and Sources

**Analysis date:** September 9, 2026. **Baseline:** commit
[`6ec4ba9`](https://github.com/keyboardspecialist/sol-lang/commit/6ec4ba9),
with the documented production track complete through P2.6.

This is a document-based analysis snapshot preserving the project-analysis
findings, not an implementation audit, a fresh test run, or a live status ledger.
Statements about implementation reflect the documents at that baseline, rather
than independent inspection of compiler behavior. The ratings and documentation
findings apply to the pre-refresh baseline. The accompanying README/TODO refresh
addresses the presentation and prioritization issues; this snapshot must not be
read as claiming that the historical contradictions remain unresolved forever.
Use [TODO.md](../TODO.md) for live milestone status and the [README](../README.md)
for the current project introduction.

The source set read for the original analysis was:

| Source | Version and extent | Role in this analysis |
| --- | --- | --- |
| [Design Specification v0.2](https://github.com/keyboardspecialist/sol-lang/blob/6ec4ba9/Sol_Programming_Language_Design_Specification_v0.2.pdf) | August 9, 2026; 61 pages | Language vision, semantics, examples, and intended toolchain |
| [Current-State Audit](https://github.com/keyboardspecialist/sol-lang/blob/6ec4ba9/Sol_Current_State_Audit.pdf) | August 25, 2026; 8 pages | Historical implementation assessment and end-to-end plan |
| [Baseline README](https://github.com/keyboardspecialist/sol-lang/blob/6ec4ba9/README.md) | At `6ec4ba9` | Public positioning, documented implementation, and roadmap |
| [Baseline TODO](https://github.com/keyboardspecialist/sol-lang/blob/6ec4ba9/TODO.md) | At `6ec4ba9`, through P2.6 | Completed foundation, frozen executable profile, and production checkpoints |

The current regenerated PDFs are available separately as the
[current Design Specification v0.2](../Sol_Programming_Language_Design_Specification_v0.2.pdf)
and [latest Current-State Audit](../Sol_Current_State_Audit.pdf). These are not
the historical artifacts linked in the source table.

The PDFs' editable sources are [specification.typ](specification.typ) and
[current-state-audit.typ](current-state-audit.typ). Publication dates and page
counts identify the analyzed artifacts, not a promise that future regenerated
PDFs will retain their contents or pagination. Historical README/TODO references
below use baseline section names and commit-pinned links, not moving line numbers.

## Overall Assessment

Sol has a strong core idea and a credible compiler roadmap. Making types,
effects, authority, contracts, semantic identity, and change consequences explicit
is a coherent response to real maintenance problems. The documented bootstrap is
substantially more than a syntax demonstration, and the frozen-E6 production path
is a sensible way to avoid uncontrolled language expansion.

The largest risk is building a sophisticated compiler before measuring whether
Sol actually improves human and AI maintenance. Compiler correctness and useful
maintenance tooling are related, but they are different hypotheses. A successful
Wasm build would validate delivery of the executable core, not by itself the
claim that a person or model can make safer changes with less context and review
effort.

The recommendation is not to abandon P2-P5 or remove implemented features. Keep
the production path bounded while running a smaller, explicit product experiment
using the inspector, interpreter, ordinary source edits, and human approval.

## Findings

### 1. High: Production Progress Does Not Test the Product Thesis

The baseline TODO's **End-to-End and Production Track**, including **P2 - Concrete
Program, Representation, and Linkage** through **P5 - Build, Differential
Execution, and Reproducibility**, is technically credible. Freezing E6 while
closing representation, runtime ABI, Wasm lowering, and reproducibility is better
than making every deferred language feature a prerequisite for execution.

However, this path primarily answers whether Sol can compile and run its current
language correctly. The baseline README's **AI-Native Development** and **Minimum
Viable Sol** promise something further: reliable change with bounded context,
visible consequences, and meaningful approval. Those benefits need their own
acceptance criteria and workloads.

A first experiment can assemble bounded context packets from existing inspection
projections, compare conservative before/after semantic facts, and present an
ordinary source edit for approval. It does not require a complete public semantic
IR, SMT integration, or a new patch language. Unknown dependencies and behavioral
consequences should be reported as unknown rather than hidden behind a confident
change summary. This is a proposed tool layer, not support already provided by
`sol inspect` or a shipped change-report command.

### 2. High: The Initial Audience Is Too Broad

The design spans domain modeling, services, systems programming, real-time
software, protocols, transactions, and durable workflows. These areas impose
different runtime, ecosystem, failure, interoperability, and assurance demands.
Trying to satisfy all of them before establishing a first user would make almost
every deferred feature appear foundational.

The strongest first-user direction is a **capability-restricted hosted
business-logic or validation component**. It fits explicit authority, nominal
domain types, typed errors, contracts, deterministic hosting, and eventual Wasm
delivery. It also permits the surrounding host to provide infrastructure instead
of requiring Sol to supply a complete service ecosystem immediately.

The concrete component, user, host boundary, and maintenance tasks remain a
future M1 choice, not a decision completed by this analysis. M1 should select one
bounded workload and identify the smallest actual blockers. It should not silently
activate collections, dependencies, transactions, or other broad language work.

### 3. High: Historical Documentation Drift Obscures the State

At the baseline, readers could draw materially different conclusions depending
on which document or section they trusted. This matters especially for a language
whose stated goal is to reduce context reconstruction.

| Historical evidence | Why it matters | Appropriate reconciliation |
| --- | --- | --- |
| Baseline README **Contracts and progressive verification** says runtime checks are not implemented, while **Bootstrap Compiler** describes `SOL_INTERPRETER_CONTRACTS_CHECK` and checked `sol run` | A reader cannot tell whether contracts are templates or executable checks | Distinguish semantic templates, implemented runtime policy, and deferred proof |
| Baseline README **Roadmap / Near-term critical path** presents the E milestones as upcoming, while TODO marks E1-E6 complete | The apparent next step is already behind the project | State the completed executable baseline and the next production checkpoint |
| The analyzed audit PDF contains contract-absence statements despite documented executable contracts | An assessment intended to clarify status becomes another conflicting source | Date the historical assessment and distinguish it from subsequent implementation |
| The analyzed PDFs describe initial MIR-era work rather than the complete P2.6 baseline | Representation and semantic-operation progress is understated | Keep historical snapshots explicit and point to the live ledger |
| The specification's native-first ambition differs from TODO's Wasm-first delivery | Long-term target breadth can be mistaken for immediate backend commitments | Separate target-language ambition from the first production target |

These are findings about the analyzed baseline, not permanent allegations about
the refreshed documents. The concurrent README/TODO revisions address the status
presentation and add a place to track the proposed maintenance experiments.
Historical PDFs should remain clearly dated, and current status should have one
obvious entry point rather than repeated checkpoint narratives that drift apart.

### 4. Medium-High: Guarantee Status Must Be Explicit

Declared intent, runtime-checked conditions, proved properties, and unknown or
unsupported obligations are not interchangeable. Every user-facing guarantee
should make its status and scope visible. A predicate represented in IR is not
necessarily executed; an executed predicate is not a proof over all executions;
a proof is only as strong as its assumptions and modeled host behavior.

The reservation example on page 51 of the analyzed Design Specification v0.2
illustrates the risk. Its idempotent retry path returns an existing reservation,
but its unconditional success postcondition requires available inventory to equal
entry inventory minus the requested quantity. For a positive quantity, a retry
that leaves inventory unchanged cannot satisfy that postcondition. This is a
contradiction in the example's stated behavior, not evidence that an implemented
transaction runtime has this bug.

A future corrected example should distinguish new reservation from replay, frame
the entry state and reservation identity, and specify the inventory change for
each case. Both creation and retry need tests. The broader lesson is that intent
prose and convincing contract syntax do not make a specification consistent.

The latest documentation refresh corrected this target pseudocode only; it did
not implement transaction semantics or change the language implementation.

The documented runtime callable checks and refined construction are valuable.
Loop invariants and decreases remain proof-only, runtime-erased templates at the
baseline; neither their presence nor proof-backed syntax should be presented as
completed solver discharge.

### 5. Medium: Conformance Is Not Usability Evidence

E6 is a useful executable-core integration fixture. Its cross-command coverage and
finite compiler closure help freeze semantics and detect regressions. They do not
show that a realistic user can implement, debug, or maintain a useful component.

The cited E6 predicate census contains four single-block predicate bodies. That
observation describes this fixture's predicate shape, not the entire compiler's
test suite. In particular, it does not mean there are only four tests in the
repository or no richer predicate tests; the documented P2.6b2 scope includes
richer predicate CFG coverage.

Add representative workloads with nontrivial happy paths, failures, repeated
changes, and review tasks. Keep conformance, compiler regression coverage,
application usefulness, and maintenance outcomes as separate evidence categories.

## Qualitative Ratings

These are qualitative judgments on a ten-point scale, not empirical measurements,
benchmark results, or release-readiness scores. They apply to the pre-refresh
baseline, and the potential scores must not be mistaken for delivered benefit.

| Dimension | Rating | Interpretation |
| --- | ---: | --- |
| Core concept | 8.5/10 | Strong, coherent response to hidden maintenance assumptions |
| Language design for AI | 8/10 potential | Explicit semantic facts could improve bounded-context work |
| Language design for humans | 7/10 potential | Local reasoning is promising; everyday ergonomics need workloads |
| Current workflow delivery | 4/10 | Useful compiler inspection exists, but the proposed maintenance loop is not delivered |
| Production compiler roadmap | 8/10 | Sensible frozen-core dependencies and staged backend work |
| Product prioritization | 6/10 | Compiler milestones are clearer than first-user validation |
| Documentation and context quality | 5/10 | Substantial detail, weakened by historical status drift and mixed ambition/status |

## Strengths to Preserve

- **Effects versus capabilities:** describing possible behavior separately from granting authority is a strong foundation for review and restricted hosting.
- **Nominal domain types and explicit outcomes:** distinct types, `Option`, `Result`, and exhaustive matching make important distinctions visible and machine-checkable.
- **Explicit public interfaces with local inference:** public types and effects can support modular reasoning without requiring every local expression to repeat known facts.
- **Runtime contracts before proof:** useful executable checks can arrive before a solver and help validate specifications against actual execution.
- **Semantic identities and structured diagnostics:** stable top-level identities and machine-readable facts provide credible ingredients for change-oriented tools without exposing unstable compiler internals.
- **No AI bypass:** generated code should face the same type, ownership, effect, capability, contract, and approval boundaries as human-written code.
- **Reference execution and differential validation:** the interpreter provides a semantic reference; documented MIR comparisons and planned interpreter/Wasm comparisons reduce backend divergence risk.

Explicit syntax is not automatically AI-friendly. A new language also creates
novelty, training-data, examples, and repair-pattern debt. Models may import
incorrect assumptions from familiar languages even when Sol's syntax is regular.
The practical question is whether authoritative examples, diagnostics, and bounded
semantic context overcome that debt, not simply whether source is more verbose.

## Design Pressure Points

| Area | Assessment and proposed direction |
| --- | --- |
| Purity and proof | `pure` must not be read as total, terminating, or automatically proof-eligible. Keep effect freedom, runtime failure, termination, supported proof fragments, and trusted assumptions distinct. |
| Refinement ergonomics | Recoverable, `Result`-based validation and safe projection of a refined value's representation are important proposed ergonomics for untrusted input. Direct checked construction is not a substitute for a convenient ordinary validation path. Do not imply these proposed APIs already exist. |
| Named arguments | The documented current interpreter evaluates in canonical formal-parameter order. Reordering a signature can therefore change effect order even when named call syntax is unchanged. Reconsider this in a future explicit semantic decision; do not change current behavior as part of documentation work. |
| Effect granularity | Choose finer effect families and aliases in response to real workload and review needs. Very broad rows hide useful distinctions; excessive granularity creates annotation and inference burden. |
| Security meaning | Permission to invoke a capability does not establish per-request authorization, tenant isolation, or information-flow safety. Those require host policy, domain checks, and appropriately scoped guarantees. |
| Behavior preservation | Unchanged types and effects do not imply unchanged behavior. Preservation claims need framing, postconditions, tests, and explicit unknowns for unmodeled dependencies. |

## Deferral Guidance

These recommendations defer additional scope. They do not call for removing
implemented features or undoing the frozen executable core.

| Area | Recommended boundary |
| --- | --- |
| Extra backends and a VM | Deliver the first Wasm target before adding native targets, another execution engine, or a new VM. Retain the reference interpreter. |
| General resumptive handlers | Defer until ownership across suspension and resumption is justified by a concrete use case. Preserve exact implemented handlers. |
| Workflows and sagas | Start with library and host-runtime experiments before committing language syntax and durable execution semantics. |
| Transactions | Gain provider experience with isolation, retries, idempotency, and failure before freezing a general language construct. |
| Intent syntax | Test structured metadata and protected review constraints before requiring new surface syntax. |
| Patch DSL | Start with typed tool operations and explicit base hashes, plus ordinary edits and approval. A dedicated language should follow demonstrated need. |
| Broad numerics, units, and currency | Add only workload-required primitives and domain distinctions, not the entire proposed numerical tower. |
| Real-time targets | Defer certification and timing claims until a specific target, runtime, and assurance requirement exists. |
| Reflection and build transforms | Defer until a demonstrated use case justifies their authority, determinism, and tooling complexity. |
| Mandatory global declaration sorting | Reconsider as a universal requirement. Stable identity and canonical formatting need not sacrifice locality or cause broad review churn. |

## Missing or Underweighted Work

- **A first workload and user:** define a concrete hosted component, its boundary, and the changes users must make successfully.
- **Bounded context queries:** retrieve the declaration, relevant types, callers, effects, contracts, and selected tests under an explicit budget, while disclosing omitted or unresolved relationships.
- **Conservative semantic deltas:** compare known API, type, effect, authority, and contract facts without claiming behavioral equivalence from unchanged metadata.
- **A basic editor slice:** prioritize useful diagnostics, navigation, and inspection access rather than waiting for the complete public graph or a full language server.
- **Protected repair constraints:** prevent apparent success by weakening contracts, broadening authority, deleting tests, or changing acceptance policy without explicit approval.
- **Practical testing:** improve assertions, expected/actual failure values, fixtures, and eventually properties where workloads show the need. Boolean units are a foundation, not a complete testing experience.
- **Minimal practical data facilities:** investigate text, bytes, parsing, constants, collections, and serialization against the selected workload. Prefer narrow host or library solutions where sufficient.
- **Edit-loop latency:** measure check, test, inspection, and repeated-edit turnaround on realistic packages, not just deterministic internal work counts.
- **Installation and debugging:** evaluate clean installation, useful runtime failure reports, source navigation, and host-boundary diagnosis from a new user's perspective.
- **Held-out maintenance evaluation:** reserve unseen tasks and protected acceptance tests so tooling can be assessed without optimizing only for its demonstrations.

These are prioritization gaps, not an instruction to implement every item before
P5. None automatically makes a broad language capability a production-track
prerequisite. M1 should identify which bounded slices matter for the first useful
component and which can remain deferred.

## Compiler Investment

The documented emphasis on safety, deterministic bounds, malformed-input
rejection, independent validation, cleanup, and source provenance is justified.
These are important compiler and host-boundary properties, not incidental polish.
They should remain acceptance requirements during product experimentation.

Distinguish those guarantees from exact internal traversal counts, arena censuses,
and particular reconstruction shapes. Such metrics are useful implementation
regression checks and can expose accidental work or missing cases. They are not
user-facing performance guarantees, usability results, or evidence that a change
preserves application behavior. An intentional implementation improvement may
legitimately require reviewing and updating an exact census.

Do not rewrite validators, IR ownership, or phase structure merely because the
design looks elaborate. Use measured latency, memory, defect patterns, or actual
feature-blocking evidence to justify architectural changes. Preserve strong
invariants while avoiding maintenance of implementation detail as an end in itself.

## Recommended Sequence

1. **Reconcile documentation.** Separate implemented behavior, experimental internals, planned features, and historical snapshots. Make the live TODO authoritative for milestone status.
2. **Finish P2.7 and P2.8, with M1 interleaved after P2.7.** Close symbols/linkage and the concrete-program contract without reopening the frozen language profile. Write the first-workload and evaluation charter before the next long compiler-only stretch.
3. **Maintain P3-P5.** Continue the runtime ABI, Wasm adapter, build, and differential path. Use a bounded early Wasm ABI experiment to test call/failure/host/cleanup assumptions before treating the full ABI as settled; do not create a second production backend path.
4. **Experiment with context packets, deltas, and approval.** Build on existing inspection and interpreter support. Use ordinary source edits, explicit base identity, protected constraints, and human review, without requiring public IR, SMT, or a patch DSL.
5. **Let the hosted workload drive future slices.** Activate only demonstrated blockers, and keep their interfaces, authority, tests, and delivery scope bounded.
6. **Run a held-out maintenance evaluation.** Compare semantic-tool assistance with source-only work using the same compiler, tests, task definitions, and resource budgets. Treat a negative or inconclusive result as useful evidence rather than moving the success criteria.

## TODO M-Track Mapping

The revised [TODO M track](../TODO.md#bounded-maintenance-workflow-experiment) is the implementation-proposal home for the
maintenance and product work below. These are proposed deliverables, not completed
tool support, and this analysis does not mark any M milestone complete. The live
TODO owns final task identifiers, dependencies, and acceptance criteria.

| Analysis recommendation | M-track implementation proposal |
| --- | --- |
| Choose a first user and hosted component | M1, first-user workload and experiment charter: select the workload, host boundary, maintenance tasks, explicit non-goals, feasibility, timebox, and evaluation design; interleave after P2.7 |
| Test local context before a public IR | M2, declaration-centered context packets, a task 58 slice: use existing inspection projections, supported semantic IDs, source excerpts, snapshot/compiler/schema/options metadata, and explicit omissions |
| Make change consequences reviewable | M3, conservative checked-snapshot deltas, a task 57 slice: validate identities and base hashes; reject stale or ambiguous comparisons; keep body edits, known semantic changes, and unknown behavior visible |
| Preserve meaningful approval | M4, ordinary-edit validation and approval, only a task 56 slice: protect tests, contracts, capability policy, and constraints against repair weakening; this is not patch syntax or automatic architectural repair |
| Improve immediate editor access | M4E, optional basic editor slice of task 55: timebox diagnostics, navigation, and packet presentation after projection feasibility; neither full LSP nor a prerequisite for M5 |
| Discover actual language and tooling gaps | M1/M5 evidence may justify separately bounded hosted-workload, testing, or ergonomics tasks; these are not automatic M-track language prerequisites |
| Establish whether the thesis works | M5, held-out workflow comparison: compare semantic tools with source-only access under matched compiler, tests, and budgets; report gains, negative results, and inconclusive outcomes |

M1 publishes the subsequent interleaving; if infeasible, record the blocker and
continue P2.8. After M1 feasibility and P2.8, M2 can begin alongside P3. M3 depends
on M2, M4 on M2/M3, and M5 on M4. Parallel work remains subject to staffing and
exclusive file ownership. These slices do not complete the broader numbered
capabilities or require a complete public semantic IR.

### Evaluation Contract

The comparison should isolate the tool contribution: both conditions use the same
Sol compiler and acceptance tests, equivalent starting source and tasks, and
matched time, model, token, and attempt budgets where applicable. Source-only does
not mean removing the compiler or tests. Protect held-out tests and acceptance
policy from the repair process, and separately record explicitly approved changes
to requirements or authority.

Measure correct-change rate, introduced regressions, reviewer effort, context
consumption, and attempts to reach an accepted change. Record failures and unknowns
as well as successes, and report human review outcomes rather than only whether
generated source compiles. Small demonstrations can establish feasibility, but
held-out tasks are needed to test whether the benefit generalizes.

The desired result is evidence that semantic context and review tools improve
maintenance without weakening constraints. If they do not, that is a valid result
that should change priorities. The core language and production compiler can still
be worthwhile without claiming an AI maintenance advantage that has not been
measured.
