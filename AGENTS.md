# Agent Orchestration

Separate planning, implementation, and review. Match each phase to the agent
best suited for it, and preserve a written handoff between agents.

## Agent Roles

### Luna: Search and Discovery

Spawn Luna for simple, read-only searches when the target and scope are known:

- Locating files, symbols, tests, fixtures, or configuration
- Finding references, call sites, definitions, and naming patterns
- Identifying the small set of files relevant to a bounded question
- Reporting existing commands, conventions, or implementations with file and
  line references

Give Luna the exact search target, repository scope, desired thoroughness, and
expected result format. Luna must not edit files, make design decisions, or
infer behavior beyond the evidence it finds. It should return concise findings
with paths and line references, note searches that produced no results, and
escalate to Sol High when the question requires architectural interpretation,
root-cause analysis, or an expanded investigation.

### Sol High: Planning

Spawn Sol High to investigate, make design decisions, and define coding tasks:

- Project structure, architecture, and public API design
- Requirements analysis and resolution of ambiguous behavior
- Compiler pipeline, ownership, type-system, effect-system, and IR design
- Cross-cutting changes, migrations, and large refactors
- Root-cause analysis when the failure location is unknown
- Security, correctness, compatibility, and performance-sensitive decisions
- Decomposition of work into independently implementable tasks

Before implementation begins, Sol High must produce a written task brief that
defines:

- The objective and expected behavior
- Relevant context, files, and interfaces
- Constraints and invariants that must remain unchanged
- A bounded implementation scope
- Acceptance criteria and required tests
- Validation commands
- Known risks, dependencies, and open questions

Sol High owns design decisions and task boundaries. It should not perform the
implementation unless integration or an unresolved architectural blocker requires it.

### Terra High: Implementation

Spawn Terra High to implement coding tasks defined by Sol High:

- Production code and localized refactors
- Unit, integration, regression, and conformance tests
- Test fixtures and small supporting utilities
- Mechanical edits required by the approved design
- Focused debugging when the suspected component and expected behavior are known

Terra High must follow the task brief, avoid unrelated scope, and preserve
behavior outside the stated task. It must run the specified validation and
return a concise implementation report containing files changed, tests run,
deviations, and remaining risks.

If requirements are unclear, the scope expands, or an architectural decision is
needed, Terra High must stop and return the task to Sol High rather than inventing
a new design.

### Sol: Review

After implementation, spawn a separate Sol agent to review the result. Use Sol
High for architecture, public behavior, security, compatibility, or
multi-subsystem changes. Use a lower Sol tier for bounded, localized changes.

The reviewer must:

- Compare the implementation with the Sol High task brief
- Inspect the diff for correctness, regressions, and unintended scope
- Check tests against the acceptance criteria and identify missing coverage
- Verify that validation results support the implementation report
- Report findings first, ordered by severity, with file and line references
- Approve only when no blocking findings remain

The reviewer should not implement fixes unless explicitly reassigned. Return
findings to Terra High, then repeat implementation and review until accepted.

### Terra: Documentation

Spawn Terra for documentation and document-production work:

- README, guide, reference, and release-note updates
- Specification prose and terminology consistency
- Typst or other document-source changes
- PDF generation, layout corrections, and rendered-output validation
- Diagrams, tables, examples, and documentation organization
- Checking documentation against implemented behavior

Give Terra the authoritative source files, approved design or implementation,
required output format, and rendering or validation command. Use Sol High first
when documentation requires unresolved architectural or product decisions.

## Routing Rules

1. Use Luna for bounded search and discovery when no design judgment or code
   modification is required.
2. Use Sol High to analyze and document every non-trivial coding task before
   implementation.
3. Give the completed task brief to Terra High for implementation.
4. Use a separate Sol agent to review every Terra High implementation.
5. Return review findings to Terra High for correction, then review again.
6. Route documentation and PDF work to Terra after the underlying design or
   behavior is approved.
7. Keep dependent work sequential. Parallelize only tasks with independent file
   scopes and no unresolved shared decisions.
8. Do not spawn an agent for trivial work when coordination would cost more than
   completing it directly.

## Delegation Contract

Every spawned agent must receive:

- A single, explicit objective
- Its assigned role: discovery, planning, implementation, review, or documentation
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

1. Luna performs bounded discovery when it can reduce the planning search space.
2. Sol High investigates and writes the task brief.
3. Terra High implements the brief and runs focused validation.
4. Sol reviews the diff, tests, and validation results.
5. Terra High resolves findings and requests another review.
6. Terra updates documentation and generated PDFs when required.
7. The coordinating agent runs final relevant verification and reports the
   outcome.
