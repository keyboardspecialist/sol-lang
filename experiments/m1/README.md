# M1 Infeasibility Report

**Date:** September 25, 2026  
**Outcome:** completed as infeasible; no workload experiment began

## Objective

M1 was intended to select one capability-restricted hosted business-logic or
validation workload and an actual user, then establish an experiment charter
with protected inputs, separated builder and verifier authority, and independent
`approve`, `reject`, or `escalate` adjudication. The experiment was also meant to
assess whether the existing inspection projections were sufficient and to set the
subsequent work interleaving.

The feasibility assessment reached a governance hard stop. No named independent
participant, externally controlled verifier, protected held-out material, or
independent adjudicator is available. Proceeding would turn repository-authored
examples and self-evaluation into purported user evidence, so M1 cannot be
completed feasibly under its approved controls.

## Candidate Workload And Boundary

`expense-policy` is an unvalidated candidate for hosted expense-claim decision
logic. It is not an adopted workload, an actual user's component, or an
authoritative business policy. No user or policy owner has reviewed or accepted
its categories, thresholds, precedence, outcomes, reasons, or host interface.

Its exact candidate host boundary is:

- Six read-only `ExpenseClaimInput` operations: `amount_cents`, `category`,
  `receipt`, `employee_active`, `domestic`, and `submitted_days_ago`.
- One `ExpenseDecisionOutput.publish` operation carrying the outcome, reason,
  and reimbursable amount.
- A pure `decide` function between input acquisition and output publication.

The candidate grants no ambient filesystem, network, clock, randomness,
environment, or console authority. This is a source-level boundary demonstrated
by compiler checking and effect inspection, not evidence that it is the right
boundary for a real participant.

## Candidate Evidence Status

The retained candidate consists of four repository-visible files:

- `expense-policy/domain.sol`
- `expense-policy/main.sol`
- `expense-policy/policy.sol`
- `expense-policy/visible_tests.sol`

These files are unprotected development evidence only. The tests are visible
author-authored examples; they are never held-out tests, independent acceptance
evidence, or business authority. Their passing results show consistency with the
candidate's currently written examples, not policy correctness or user value.

## Unmet M1 Controls

The following required controls do not exist:

- A named independent participant acting as an actual user.
- An identified policy owner who can supply authoritative change intent and
  acceptance policy.
- A protected base snapshot, held-out maintenance tasks, held-out tests, and
  acceptance policy that the builder cannot read or modify.
- A verifier controlled outside the builder and repository boundary.
- Separation between builder authority and verifier inputs, execution, and
  results.
- A named independent adjudicator with authority and a defined protocol to emit
  `approve`, `reject`, or `escalate`.

The absence of any one of these controls prevents the planned comparison. Their
combined absence is the M1 governance hard stop, not a compiler implementation
blocker.

## Threat Model And Protection Boundary

The experiment must resist a builder, whether human or automated, who can
intentionally or accidentally tailor a change to known tests, alter the policy or
baseline, omit unfavorable evidence, choose a favorable verifier configuration,
rewrite evaluation criteria after seeing results, or self-approve the outcome.
It must also prevent accidental leakage of held-out tasks and tests before a
candidate submission is fixed.

Everything committed to this shared repository is visible to, and generally
modifiable by, the builder. A repository file therefore cannot serve as a
protected held-out input or an externally controlled verifier. A hash can detect
a change relative to an already trusted value, but it cannot decide who was
authorized to choose that value, prove that the builder never saw the content,
authenticate the evaluator, or establish independent adjudication. Repository
visibility plus content hashes supplies integrity and addressing, not authority
separation, confidentiality, producer authentication, or business provenance.

## Stock Validation Boundary

The stock compiler was run against the candidate on September 25, 2026:

| Command | Outcome |
| --- | --- |
| `./build/sol fmt --check experiments/m1/expense-policy` | Passed with no formatting drift. |
| `./build/sol check experiments/m1/expense-policy` | Passed: 4 files and 23 declarations checked. |
| `./build/sol test experiments/m1/expense-policy` | Passed: 12 visible tests, 12 passed, 0 failed. |
| `./build/sol effects experiments/m1/expense-policy` | Passed: 25 callables reported; `decide` is pure and `evaluate` has exactly the six input effects plus one publish effect. |
| `./build/sol inspect experiments/m1/expense-policy` | Passed and emitted the supported versioned inspection projections. |
| `./build/sol run experiments/m1/expense-policy` | Expected `SOL-RUN-003`: host operation `amount_cents` is not allowed. |

The stock `sol run` host grants only its approved standard host profiles; it does
not register this candidate's custom expense operations. `SOL-RUN-003` is the
expected enforcement of that boundary, not a compiler defect or a failed policy
decision. A custom host registry or C harness could execute the boundary, but one
was deliberately not added: it is outside M1's approved scope and would not cure
the missing independent governance or protection.

## Projection Assessment

The existing projections are sufficient to inspect this small candidate's source
structure, top-level declarations and identities, types, static calls, effects,
contracts, and diagnostics. Together with `check`, `test`, and `effects`, they are
enough to confirm the source-level candidate boundary described above.

They are not sufficient for the proposed independently governed maintenance
experiment. A bounded experiment packet would still need explicit, authenticated
records for the source snapshot, compiler and schema versions, compiler options,
selection metadata, inclusion reasons, omissions, explicit unknowns, and test
associations. Current top-level stable identities do not provide stable identity
for nested members, statements, expressions, call sites, or other fine-grained
elements. Effect records also retain name/path-sensitive references that can move
or be renamed, and omit provenance and negative information needed to distinguish
unknown relationships from confirmed absence. Candidate test associations would
be heuristic, not authoritative relevance or business authorization.

These are recorded projection gaps, not approved prerequisites for new compiler,
host, projection, or patch functionality.

## Metrics

No protected maintenance trial or adjudicated decision occurred.

| Metric | Result |
| --- | --- |
| False accepts | Not observed; no independent decisions occurred. |
| False rejects | Not observed; no independent decisions occurred. |
| Implementation-view request rate | Not observed; no participant performed a protected task. |
| Decision time | Not observed; no verifier or adjudicator issued a decision. |
| Comparative correctness or regressions | Not applicable without protected tasks, tests, and acceptance policy. |
| User benefit or usability | Not observed; no actual user participated. |

Compiler validation counts above are candidate validation outcomes, not substitutes
for these experiment metrics. No unobserved metric is recorded as zero.

## Stop/Go Decision And Interleaving

**Decision: stop M1 as infeasible.** The stop condition is the missing independent
governance and protected evaluation boundary; the go condition required all of
those controls before builder access to evaluation material. The feasibility
assessment closed on September 25, 2026, before an experimental timebox began.
The candidate remains as unprotected source evidence, but it does not validate
Sol's maintenance thesis and does not activate M2-M5.

The sole live execution cursor remains in `TODO.md`: P3.1 has since completed, and
P3.2 is now both the next overall item and next production checkpoint. M2-M5 remain
gated unless M1 is reopened and completed feasibly. This report records that
decision; it does not create a competing work queue.

## Reopening Conditions

M1 may be reopened only when all of the following are established before builder
access to evaluation material:

- A named independent participant and an identified policy owner commit to a
  concrete hosted workload and authoritative change intent.
- The base snapshot, maintenance tasks, tests, acceptance policy, and any
  evaluation secrets are held outside builder control with documented access and
  release procedures.
- An externally controlled verifier fixes the compiler, schema, options, resource
  limits, and evidence-capture procedure before submissions are evaluated.
- Builder and verifier authority are separated, including control of verifier
  execution and results.
- A named independent adjudicator accepts a documented
  `approve`/`reject`/`escalate` protocol and conflict procedure.
- The timebox, stop/go thresholds, metrics, disclosure policy, and permitted
  implementation-view requests are fixed before the trial begins.

Until every condition is met, the expense-policy files remain only an unvalidated
candidate and M2-M5 remain gated.
