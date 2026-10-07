# Agent Orchestration

Separate planning, implementation, and review. Match each phase to the agent
best suited for it, and preserve a written handoff between agents.

## Agent Roles

### Astra 6: Project Audit and Steering

Spawn Astra 6 at milestone boundaries, before opening parallel workstreams, or
when scope, priorities, or progress appear to drift:

- Compare active work and delivered behavior with user goals, approved briefs,
  and the authoritative `TODO.md` execution cursor
- Track milestone exit criteria against implementation, review, and validation
  evidence; distinguish completed, active, blocked, and deferred work
- Detect scope creep, conflicting interfaces or ownership, dependency violations,
  duplicated effort, and discrepancies between code and status documents
- Assess whether parallel lanes advance the current goal and identify blockers,
  sequencing changes, or bounded corrective tasks
- Check that previous audit actions have owners and evidence of resolution

Give Astra 6 the current goals, TODO, relevant briefs, implementation/review
reports, validation results, and active workstream ownership. It returns a concise
steering report with goal-by-goal status, evidence-linked drift findings ordered
by impact, and recommended next actions with owners and dependencies. Missing
evidence must remain explicit rather than being treated as completion.

Astra 6 owns project-level alignment assessment. Sol 6.1 High retains technical
design and task boundaries; the coordinator maintains the single execution cursor
and integrates approved steering actions. Goal changes go back to the user,
technical corrections go to Sol 6.1 High for a bounded brief, and status-document
corrections go to Luna 6 xhigh. Audits are read-only unless an edit scope is
explicitly assigned, and complement the separate implementation review.

### Luna 6 Fast: Search and Discovery

Spawn Luna 6 Fast for simple, read-only searches when the target and scope are known:

- Locating files, symbols, tests, fixtures, or configuration
- Finding references, call sites, definitions, and naming patterns
- Identifying the small set of files relevant to a bounded question
- Reporting existing commands, conventions, or implementations with file and
  line references

Give Luna 6 Fast the exact search target, repository scope, desired thoroughness, and
expected result format. Luna 6 Fast must not edit files, make design decisions, or
infer behavior beyond the evidence it finds. It should return concise findings
with paths and line references, note searches that produced no results, and
escalate to Sol 6.1 High when the question requires architectural interpretation,
root-cause analysis, or an expanded investigation.

### Sol 6.1 High: Planning

Spawn Sol 6.1 High to investigate, make design decisions, and define coding tasks:

- Project structure, architecture, and public API design
- Requirements analysis and resolution of ambiguous behavior
- Compiler pipeline, ownership, type-system, effect-system, and IR design
- Cross-cutting changes, migrations, and large refactors
- Root-cause analysis when the failure location is unknown
- Security, correctness, compatibility, and performance-sensitive decisions
- Decomposition of work into independently implementable tasks

Before implementation begins, Sol 6.1 High must produce a written task brief that
defines:

- The objective and expected behavior
- Relevant context, files, and interfaces
- Constraints and invariants that must remain unchanged
- A bounded implementation scope
- Acceptance criteria and required tests
- Validation commands
- Known risks, dependencies, and open questions

Sol 6.1 High owns design decisions and task boundaries. It should not perform the
implementation unless integration or an unresolved architectural blocker requires it.

### Luna 6 xhigh: Implementation

Spawn Luna 6 xhigh to implement coding tasks defined by Sol 6.1 High:

- Production code and localized refactors
- Unit, integration, regression, and conformance tests
- Test fixtures and small supporting utilities
- Mechanical edits required by the approved design
- Focused debugging when the suspected component and expected behavior are known

Luna 6 xhigh must follow the task brief, avoid unrelated scope, and preserve
behavior outside the stated task. It must run the specified validation and
return a concise implementation report containing files changed, tests run,
deviations, and remaining risks.

If requirements are unclear, the scope expands, or an architectural decision is
needed, Luna 6 xhigh must stop and return the task to Sol 6.1 High rather than inventing
a new design.

### Sol 6.1: Review

After implementation, spawn a separate Sol 6.1 agent to review the result. Use
Sol 6.1 High for architecture, public behavior, security, compatibility, or
multi-subsystem changes. Use a lower Sol 6.1 tier for bounded, localized changes.

The reviewer must:

- Compare the implementation with the Sol 6.1 High task brief
- Inspect the diff for correctness, regressions, and unintended scope
- Check tests against the acceptance criteria and identify missing coverage
- Verify that validation results support the implementation report
- Report findings first, ordered by severity, with file and line references
- Approve only when no blocking findings remain

The reviewer should not implement fixes unless explicitly reassigned. Return
findings to Luna 6 xhigh, then repeat implementation and review until accepted.

### Luna 6 xhigh: Documentation

Spawn Luna 6 xhigh for documentation and document-production work:

- README, guide, reference, and release-note updates
- Specification prose and terminology consistency
- Typst or other document-source changes
- PDF generation, layout corrections, and rendered-output validation
- Diagrams, tables, examples, and documentation organization
- Checking documentation against implemented behavior

Give Luna 6 xhigh the authoritative source files, approved design or implementation,
required output format, and rendering or validation command. Use Sol 6.1 High first
when documentation requires unresolved architectural or product decisions.

## Routing Rules

1. Use Luna 6 Fast for bounded search and discovery when no design judgment or code
   modification is required.
2. Use Sol 6.1 High to analyze and document every non-trivial coding task before
   implementation.
3. Give the completed task brief to Luna 6 xhigh for implementation.
4. Use a separate Sol 6.1 agent to review every Luna 6 xhigh implementation.
5. Return review findings to Luna 6 xhigh for correction, then review again.
6. Route documentation and PDF work to Luna 6 xhigh after the underlying design or
   behavior is approved.
7. Keep dependent work sequential. Parallelize only tasks with independent file
   scopes and no unresolved shared decisions.
8. Use Astra 6 for milestone alignment audits, parallel-workstream readiness, and
   drift correction; route its actions through the coordinator and assigned owners.
9. Do not spawn an agent for trivial work when coordination would cost more than
   completing it directly.

## Delegation Contract

Every spawned agent must receive:

- A single, explicit objective
- Its assigned role: discovery, planning, implementation, review, documentation,
  or project audit/steering
- The exact scope and relevant file paths
- Known constraints and behavior that must remain unchanged
- The expected deliverable
- The command or method used to verify completion
- An instruction not to modify unrelated work

Every agent must return a concise summary of work performed, files changed or
reviewed, validation run, decisions made, and unresolved risks. The coordinating
agent is responsible for preserving the handoffs, resolving conflicts, and
running final verification.

## Default Workflow

1. Luna 6 Fast performs bounded discovery when it can reduce the planning search space.
2. Sol 6.1 High investigates and writes the task brief.
3. Luna 6 xhigh implements the brief and runs focused validation.
4. Sol 6.1 reviews the diff, tests, and validation results.
5. Luna 6 xhigh resolves findings and requests another review.
6. At milestone or parallel-dispatch boundaries, Astra 6 audits goal alignment,
   progress evidence, dependencies, and outstanding drift; the coordinator routes
   corrective actions before advancing the cursor or opening affected lanes.
7. Luna 6 xhigh updates documentation and generated PDFs when required.
8. The coordinating agent runs final relevant verification and reports the
   outcome.
