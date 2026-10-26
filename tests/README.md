# Running tests with live output

The [October 17 audit](AUDIT.md) records all 60 configured UNIX ON names:
**58 PASS, zero FAIL, two cutoffs; NOT_ACCEPTANCE**. Observability is accepted for
its bounded scope; the 1s unit / 5s integration / 60s suite speed goals are not.
It also records source identity, preserved coverage and unresolved platform gaps.
Reviewed T2 source and audit documentation (`5012c62`), following T1 (`6eef9ca`),
are integrated and available on pushed `master` at `35f4027`. Audited code is
unchanged; post-audit documentation-only differences do not match the receipts'
all-source identity.

From the repository root:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DSOL_ENABLE_WERROR=ON
cmake --build build --parallel 2
ctest --test-dir build --verbose --output-on-failure -j 1
```

The last command is the canonical live full CTest invocation, not a bounded speed
probe: its safety caps are larger than performance goals. For investigations use
the bounded audit below; do not rerun known ordinary cutoffs without a new brief.

`--verbose` is essential for live output, even with stdout/stderr redirected.
Bare CTest, `--output-on-failure` alone, and the build's `test` target buffer
successful test output. Child flushing cannot fix CTest's independent UI.
Serial execution makes phase/timing evidence attributable to one test; it is not
an optimization. All existing tests remain enabled by default.

## Shared harness and semantic phases

Every registration uses `cmake/SolTestSupport.cmake` and the Python standard-library
supervisor. All 31 native test sources report their own begin/done and named real
groups. Heavy MIR/Wasm test-translation-unit adapters report fixture load, compile,
predecessor build, validation, rendering, fault hook/ordinal, quota and teardown
operations without touching compiler assertion buffers. They evaluate each original
operation once and count it only after return (a completed rejection is still a
completed operation, not a passing test). CMake wrappers identify CLI and schema
subcommands outside their exact captured stdout/stderr. Progress does not make
tests fast; incomplete/over-budget runtime validation stays explicit in audit receipts.

```sh
# Fast synthetic children, including one intentionally failing CHECK followed by
# another case; the support test asserts failure propagation and continuation.
ctest --test-dir build --verbose --output-on-failure -R '^progress_support$'
python3 tests/support/test_progress_support.py --child build/sol_progress_child --cmake "$(command -v cmake)"
ctest --test-dir build --verbose --output-on-failure -R '^progress_integration$'

# Run argv directly without a shell. Deadlines are safety ceilings, not targets.
python3 tests/support/run_with_progress.py --name frontend --timeout 30 -- build/sol_frontend_tests
```

Standalone support tests without `--child`/`--cmake` explicitly skip the C/CMake
checks; CTest supplies both and runs the complete support group. Synthetic waits
are concurrent only within that group (target total <=5s). No full compiler fault
matrix is run for the failure-continuation check.

## P4.4f1a bounded diagnostic selectors

The three P4.4f1a runners expose test-only case listing and one-key selection. For
example, with the existing OFF build directory:

```sh
build-p44-off/sol_mir_runtime_cleanup_tests --list-p44f1a-cases
python3 tests/support/run_with_progress.py \
  --name p44f1a:cleanup.bool_unit.frozen-false --timeout 3 -- \
  build-p44-off/sol_mir_runtime_cleanup_tests \
  --p44f1a-case cleanup.bool_unit.frozen-false
```

The corresponding runners are `sol_mir_runtime_lowered_program_tests` and
`sol_wasm_represented_tests`; use the matching existing `build-p44-off`,
`build-p44-on`, or sanitizer build directory. The exact keys are source-listed by
`--list-p44f1a-cases`. These selectors run only the named diagnostic case and are
not a CTest registration or a replacement for the default run: invoking a runner
with no arguments still executes all old and new cases. A selected receipt is only
the bounded case evidence it reports; it does not certify full-suite, full-shard,
resource-authentication, or P4.4 acceptance. See the [P4.4f1a handoff](../docs/p4.4f1a-handoff.md)
for the accepted scope and remaining boundaries. The native selected-case policy
uses a 3-second active timeout. This supervisor option does not provide a 2-second
reserve or enforce the complete-case 5-second horizon: acceptance separately checks
the finished case's post-close receipt against that total, leaving the 2-second
operating reserve. Use the separately approved bounded policy for SAN cases.

## P4.4f1b leaf-call selectors and accepted evidence

The represented runner exposes a source-listed f1b selector set. List it and
select one exact key with an existing matching build:

```sh
build-p44f1b-on/sol_wasm_represented_tests --list-p44f1b-cases
python3 tests/support/run_with_progress.py \
  --name p44f1b:represented.predicate-leaf.ordinary-direct-identity \
  --timeout 3 -- build-p44f1b-on/sol_wasm_represented_tests \
  --p44f1b-case represented.predicate-leaf.ordinary-direct-identity

build-p44f1b-san/sol_wasm_represented_tests --list-p44f1b-cases
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/support/run_with_progress.py \
  --name p44f1b:represented.predicate-leaf.ordinary-recursive-identity \
  --timeout 27 -- build-p44f1b-san/sol_wasm_represented_tests \
  --p44f1b-case represented.predicate-leaf.ordinary-recursive-identity
```

The accepted current f1b list contains 351 keys; the f1a list retains 23. Per mode,
372 original keys (349 f1b +23 f1a) passed at their frozen pins, and two ordinary
direct/recursive identity keys passed on final ON/SAN binaries. Thus the accepted
union is 374 obligations per mode, **not** a fresh 374-case run of the final runner.
The two ordinary sources return 7 and 42 respectively; the recursive case is an
ordinary call profile at depth 64, not recursive predicate-call admission.

These selectors add no CTest/CMake registrations and do not filter the default
runner. The source-reviewed manifest remains 72 controls /4091 fault IDs /50 shards;
all 297 new faults and legacy D1/2 passed in the accepted selected matrix, but this
does not claim all 4091 faults, all shards, or the full default suite were run. The
historical combined SAN 120-second cutoff remains UNKNOWN; the separately supervised
per-selector evidence is described in the [P4.4f1b handoff](../docs/p4.4f1b-handoff.md).

The example `--timeout` values enforce active work only: native 3s / SAN 27s. The
accepted protocol also measures complete wall after capture, log, primary-receipt,
and post-close-witness close, under 5s / under 30s with the 2/3-second cleanup
reserve. A bare selector command is
not by itself that full acceptance supervisor. The ten reviewed negative CLI
controls return status 2 with empty stdout and no case execution for unknown,
missing, duplicate, mixed, and focus-conflicting requests. Current registrations
remain ON 60 / OFF 56. The backend-OFF configuration compiled the two owner targets
and inspected registrations, but did not execute either owner or any OFF CTest test.

## P4.4f1c ordinary-callee-chain selectors

The represented runner also exposes the source-listed f1c selector set. These are
examples of the list/read/dispatch CLI, not claims that these commands were run:

```sh
build-p44f1c-on/sol_wasm_represented_tests --list-p44f1a-cases
build-p44f1c-on/sol_wasm_represented_tests --list-p44f1b-cases
build-p44f1c-on/sol_wasm_represented_tests --list-p44f1c-cases

python3 tests/support/run_with_progress.py \
  --name p44f1c:represented.predicate-callees.launch.traced --timeout 3 -- \
  build-p44f1c-on/sol_wasm_represented_tests \
  --p44f1c-case represented.predicate-callees.launch.traced

ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/support/run_with_progress.py \
  --name p44f1c:represented.predicate-callees.fault.predicate-callee-recursive-tail.0127 \
  --timeout 27 -- build-p44f1c-san/sol_wasm_represented_tests \
  --p44f1c-case represented.predicate-callees.fault.predicate-callee-recursive-tail.0127
```

The current source lists 23 f1a, 351 f1b, and 457 f1c keys; the f1c set is 33
nonfault cases plus 424 new fault IDs. The exact semantic/configuration union is
**506 keys per mode =457 f1c +49 named old keys** (originally 492 fresh +14 reuse).
The coordinator-amended old49 dispatch is 23 f1a +19 mutations +6 resources +one
`launch.traced` reset/semantic key; it is not an expanded retest gate. The
source-reviewed manifest is 74 controls /4,515 faults /50 shards. The f1c selector
uses the same source-listed case dispatcher for explicit selection and the default
runner, adds no CTest registration, and does not change the no-argument full-run
behavior. Its predicate-call boundary remains one owned plain `Int64` argument to a
`Bool` predicate in an INSTANCE-owned ALWAYS `requires`; below that edge it admits
pure scalar ordinary chains and ordinary recursive SCCs, while nested predicate
invocation, mixed cycles, and unsupported transitive targets still reject.

The exact 506-key union in each mode and its supplementary obligations are complete;
independent final review accepts the bounded f1c technical/evidence scope. The feature
was published to `origin/master` as `24e2d16`; coordinator integration and documentation
review are complete. Accounting
distinguishes **139 ON current-binary executions +367 historical carries** (356
old-fresh +11 reviewed reuse) and **495 SAN current-binary executions +11 historical
carries**. Each mode includes eight reviewed current-pin targets; ON then has 131/131
and SAN 487/487 continuation passes. These are 506 unique evidenced keys, not 506
fresh current-binary runs. Historical PASS/reuse records retain their original source
and binary pins and timings; no native result is carried to SAN. The old native R
discovery and R164 timeouts remain UNKNOWN, and six earlier false-baseline fault
FAILs remain failures rather than retrospective passes.

The 424 new fault IDs per mode are Q 1–127 (physical allocations 3–129) and R 1–297
(physical allocations 1–297); the legacy Q physical 1/2 identities retain their
original f1a semantics. The two supplementary ordinary identity selectors passed in
each mode against the retained f1b reference (direct result 7 /4,001 bytes; recursive
result 42 /8,944 bytes), matching full module bytes, all-15-`Usage`, and typed catalog
identity. All ten CLI negative controls per mode behaved as specified: exit 2, empty
stdout, and no fixture/backend work. These 24 supplementary checks are outside the
506-key union.

Bounded f1c technical/evidence acceptance is not broad P4.4 or full-suite acceptance.
The 74-control /4,515-fault /50-shard manifest is an inventory; it does not mean all
faults or shards ran. No default/full-suite, leak-check, or platform-matrix pass is
claimed. See the [P4.4f1c handoff](../docs/p4.4f1c-handoff.md) for evidence pins,
historical outcomes, and remaining project-level boundaries.

The wrapper timeouts above are active-work ceilings only: native 3s / SAN 27s. The
reviewed protocol measures complete time after capture/log close, primary-receipt
close, and post-close-witness close, under 5s / under 30s. The examples are not that
complete acceptance supervisor by themselves; do not use the old 5s/30s values as
active-work timeouts. See the
[P4.4f1c handoff](../docs/p4.4f1c-handoff.md) for the accepted scope, pins, and
preserved historical outcomes.

## P4.4f1d refinement-call selectors — accepted and published

The approved f1d core adds one direct pure type-owned refinement call,
`type T = refined Int64 where helper(self)`, with an owned plain `Int64 -> Bool`
predicate call. Existing ordinary scalar callee chains and recursive SCCs remain
available below it. This does not admit wrappers or computed `self`, nested
predicate calls/mixed cycles, ensures calls, Bool/Unit signatures, refined or Text
callee interiors, PanicText, or the existing guard-call case. All bounded f1d
source, including the corrected core, allocation helpers, and nonfault-safety fixes,
is independently approved. Final technical/evidence acceptance and feature
publication are closed at `42fd4cd` (`7d19435..42fd4cd`). This status-only refresh
adds no validation evidence or timing and does not close broad P4.4.

The represented runner's f1d list/selection syntax and the approved core key are:

```sh
build-p44f1c-on/sol_wasm_represented_tests --list-p44f1d-cases
python3 tests/support/run_with_progress.py \
  --name p44f1d:represented.refinement-calls.launch.traced --timeout 3 -- \
  build-p44f1c-on/sol_wasm_represented_tests \
  --p44f1d-case represented.refinement-calls.launch.traced
```

These commands illustrate selector syntax, not the final acceptance evidence; a
single selected case remains focused evidence only. The final report records
**486 selected obligations per mode = 412 f1d (45 nonfault + 367 new faults) + 74
compatibility**, with zero pending. Fresh final-batch PASS counts are ON 111 and
SAN 318; carry/reuse counts are ON 375 and SAN 168. Each carry count includes eight
earlier matching-d03 mutation receipts, so these are not all historical carries or
486 fresh launches. Including those eight, current-binary semantic observations are
119 ON / 326 SAN. The remaining older source-equivalent fault evidence is 367 ON /
160 SAN; SAN freshly passed the 207 V faults V64–135 and V137–271. All 367 new fault
IDs have evidence in both modes, and legacy IDs 3/4/5 were freshly run in both modes.
The ON final batch ran no new native U/V fault sweep. The original audited 370 ON /
161 SAN PASS observations remain historical and are eligible only under the
coordinator's source-equivalence disposition; source-equivalent carries retain their
old pins/timings and are not current-binary timing. Original V64 remains an
INTERRUPTED history record; its separate newly authorized fresh pass discharges the
key without rewriting the interruption.

Native active/full limits remain <=3s/<5s and SAN <=27s/<30s after closure. The
fresh-batch maxima were 2.562463s active / 2.577694s complete ON and 11.601809s /
11.646119s SAN. Complete-time measurement follows raw/log and Result closure,
primary-receipt closure, and post-close-witness closure. SAN uses
`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1`; leak detection is disabled. There is no suite-speed
or leak-coverage claim.

Two independent captures establish U=98 and V=272 allocation requests. The new
fault set is `M=(98-3)+272=367`: U-tail requests map to physical 4–98, and V covers
physical 1–272. Inventory digests differ from module hashes; 9,433/63,357 are module
byte counts. Full ordered records, module bytes, all 15 Usage fields, and witnesses
were compared. These are measured inventory identities, not semantic or
resource-exactness goldens. Legacy IDs3/4/5 remain physical1/2/3 with initial
failed attempts1/3/3.

Runtime inventory certification passed pre/post in both modes: lists 23/351/457/412,
old lists byte-identical, all 50 shard names unchanged, and the 4,999-row manifest
preserves the 4,630 old ordered tuples exactly. It records 76 controls, 117 control
rows, and 4,882 fault IDs; this does not mean all 4,882 faults or the full default
suite ran. All 44 frozen negative-argv cases (22 per mode) returned status 2 with
empty stdout and no semantic progress. The original ON missing-value classification
failure and raw receipt remain unchanged; read-only reconciliation confirms the
expected nonzero behavior without rerun. The prior f1c 506-key mixed-pin accounting
(ON 139 current-binary executions +367 carries; SAN 495 +11) remains historical,
not f1d evidence. The earlier 19-ON/55-SAN authority deviation remains explicit,
not retroactively authorized. Final Sol review, documentation audit, Astra
alignment, coordinator checks, and publication gates are closed; three native
current-binary smokes passed within strict caps and protected original/resource WIPs
were verified unchanged. See the [P4.4f1d handoff](../docs/p4.4f1d-handoff.md) for
exact pins, carry provenance, and preserved failures. The current f1e execution
status is recorded below and in the [interim handoff](../docs/p4.4f1e-handoff.md).

## P4.4f1e full S4 — technical evidence accepted, publication PENDING (October 25, 2026)

S4c is published/remote-verified at `50c7475816c7feb4c1f18c0d5a64365cb1f88498`
in its [publication receipt](../../sol-lang/.git/agent-handoffs/p44-f1e/s4-behavior/s4c/publication-receipt.md);
S4a/S4b receipts remain authoritative. The appended
[S4d final review](../../sol-lang/.git/agent-handoffs/p44-f1e/s4-behavior/s4d/evidence-review.md#final-san70--d1d5--full-s4-technical-evidence-acceptance--2026-10-25)
accepts **D1–D5/full S4 technical evidence**, **170 compatibility identities =140
fresh +30 standalone original-context carries**, **28 actual windows**. The separate
**49 nonfault keys/98 mode identities** and98 C metadata children retain their own
accepted original contexts, not fresh S4d runs. E650, exact old4999 ordered tuples,
manifest/shards/default mapping carry without new default/full-suite execution.

All23 physical typed anchors, six profiles D96/T203/Q129/R297/U98/V272 and complete
ten-field/non-line obligations pass: D/T/Q/R retain summary formats/full C equality
gates, U/V all actual rows with only line normalized to F1D. Ordinary full module
bytes/Usage15/catalog and two-path-only projection pass. Full controls/renders/
fences/witnesses/closure remain; S4b's accepted deadline correction retained the
3/27 active caps. The [handoff](../docs/p4.4f1e-handoff.md#s4d-full-s4-accepted-evidence) links
current accepted source413/artifact10 pins, all804 remainder dispositions and
immutable failure/UNKNOWN history. S3 remains original1202 identities/525 FINALs/
three honest no-FINAL leaves, not new replay or reconstructed closing facts.

**S4d/full-S4 publication PENDING** exact four-doc staged review, Astra full-S4 audit
and coordinator commit/push/exact remote verification/receipt. No source edit/repin/
rebuild/new test/case is needed for this frozen docs carry. Only the external sole
cursor owns live execution; AGENTS/mixed WIP excluded. **S5 bounded affected-integration
planning next/NOT STARTED** after verified publication. R-AUTH/held±1
OPEN, speed UNMET, SAN leaks disabled (not leak-freedom), platform/broad P4/P4.4/
full-f1e unclosed; OFF compile-only, host/E6/P4.5/P5 deferred.

## Historical P4.4f1e S4c — October 24, 2026 accepted pre-push checkpoint

S4c publication-PENDING and S4d-next statements below are pre-push history;
the S4c receipt/current full-S4 technical checkpoint above supersede status only.

S4b is published/remote-verified at `dacf0a21c63263c93123e0e278d4b8188342be5f`
in its [publication receipt](../../sol-lang/.git/agent-handoffs/p44-f1e/s4-behavior/s4b/publication-receipt.md).
The appended [S4c final review](../../sol-lang/.git/agent-handoffs/p44-f1e/s4-behavior/s4c/evidence-review.md#final-independent-s4c-c1c4-acceptance)
accepts **six resource cases +98 metadata children +two builds =106 actual children**.
The **49 nonfault inventory=discovery2+S4a26+S4b18+S4c3** is supported by **92
original-context carries +six fresh references** across two modes. Live E650 and
unchanged old lists/manifest/shards/original4999 tuples are reconciled; default
mapping is source-proved with metadata/selected evidence, **not actual default
backend/full-suite execution** or650 runtime passes. Metadata negatives retain
original exit2/supervisor FAIL plus separate expected-nonzero acceptance.

Real backend limits implement defaults→exact observed Usage→one-below→same-owner
retry; all15 Usage, complete module bytes/witnesses, independent module-local and
typed source-edge arity joins, byte-first fences/final validators/renders/close
remain required. These are observations, not independent resource authentication.
The [handoff](../docs/p4.4f1e-handoff.md#s4c-accepted-evidence) links exact current
test/build/runtime pins, literal source carry and immutable failure/UNKNOWN history.
S4b's accepted deadline fix raised no caps; no old receipt is current-binary evidence
and no fresh S3 fault or S4a replay is claimed.

S4c publication PENDING exact five-path docs/test staged review, Astra readiness and
coordinator commit/push/remote verification. No source repin/rebuild/extra runtime
for this carry. Only the sole project cursor owns the live step; AGENTS/external
evidence/mixed WIP excluded. S4d compatibility is next after publication; full S4/
S5 unclosed. R-AUTH/held ±1 OPEN, leaks disabled (not leak-freedom), speed UNMET;
no broad P4/P4.4/full-f1e/platform/full-suite/speed claim; host/E6/P4.5/P5 deferred.

## Historical P4.4f1e S4b — October 24, 2026 accepted pre-push checkpoint

S4b publication-PENDING and S4c-next statements below are pre-push history,
superseded by the S4b receipt/current S4c checkpoint above.

S4a is published/remote-verified at `acf3de0d1c23b377605fd682f207c28bc68002b5`
in its [publication receipt](../../sol-lang/.git/agent-handoffs/p44-f1e/s4-behavior/s4a/publication-receipt.md).
The appended [S4b final review](../../sol-lang/.git/agent-handoffs/p44-f1e/s4-behavior/s4b/deadline-correction/tail-binding-evidence-review.md#final-s4b-b1b6-actual-evidence-acceptance)
accepts **36 identities, 18/mode: nine original-context ON carries +27 fresh
(nine ON +18 SAN)**, plus **14 original metadata invocations**. E647=46 nonfault+
601 faults is accepted stage metadata, not final S4c metadata/default execution.
All eight rejections, two canonical controls and eight hostile mutations are
accepted per mode; old carries are not current-binary or completed-window results.

Reviewed SHA arithmetic/padding/hex/exact-fence and fresh-owned relocation changes
preserve allocation/fault sequence, failure states, full bytes/Usage15/witnesses,
seven renders, secure cleanup and hostile restore/retry. The per-render pure-key
workspace is not a validation cache; no sampling, validation waiver or cap increase.
Worst fresh ON active/post-close/distinct outer is2.853264/3.492279/4.720051s;
SAN10.847718/11.614223/13.013285s. All pass unchanged3/<5/<5 and27/<30/<30 caps,
parent<120/reserve10. Prior52 pure tests/19 groups are source qualification only.
The [handoff](../docs/p4.4f1e-handoff.md#s4b-accepted-evidence) links actual current
eight-source/build/runtime pins, production-helper equivalence and immutable
failure/UNKNOWN-window history. S3/S4a results retain original contexts; no fresh
faults, S4a replay or broad default/full-suite/speed claim follows.

S4b publication remains PENDING docs/exact 12-path staged review, Astra readiness
and coordinator commit/push/remote verification; no source repin/rebuild/runtime
for this carry. Only the project cursor owns the live step; AGENTS/external evidence/
original WIP are excluded. S4c3/resources+final metadata/default checks are next
after publication, then S4d compatibility; S5 not started. SAN leaks disabled, not
leak-freedom; R-AUTH/held ±1 OPEN, speed UNMET, broad P4/P4.4/full-f1e OPEN and
host/E6/P4.5/P5 deferred.

## Historical P4.4f1e S4a — October 24, 2026 accepted pre-push checkpoint

The S4a publication-PENDING and S4b-next statements below are pre-push history,
superseded by the S4a receipt and current S4b checkpoint above.

S3 is published/remote-verified at `eb1aeb276977ab940020307b52914a2f9a8948db`
in the [publication receipt](../../sol-lang/.git/agent-handoffs/p44-f1e/s3-faults/s3-publication-receipt.md).
The [S4a actual-evidence review](../../sol-lang/.git/agent-handoffs/p44-f1e/s4-behavior/s4a/old-clause-correction/evidence-review.md)
accepts **26×2 obligations: 12 original-context ON case carries +40 fresh
current-source identities (14 ON +26 SAN)**, plus **four separate W/X discovery
witnesses**, not new fault executions. Historical S4a test SHA256 is
`355f3ed5759f4cbfd73c1979bcd1a2208f32d7cb8dbc2f70c67d7afda5ed5fa8`.
The original12 are per-case carries, not a completed original window. Source/P2
derives n20/m39/S147; actual roots, retained and nine limited paths retain complete
packet/ledger/owner success gates joined to raw protocols and closed receipts.
S4a is technically accepted but publication remains PENDING docs/exact five-file
staged review, Astra readiness and coordinator commit/push/remote verification.

**E633=32 nonfault+601 faults** is static registration only, not current metadata
CLI/default runtime/all633 acceptance. Source-specific S2/S3 carry uses the isolated
false-default walker branch and four actual discovery witnesses: historical S3's
1,202 identities /525 FINALs /three no-FINAL leaves are not fresh S3 runs. The
[handoff](../docs/p4.4f1e-handoff.md#s4a-accepted-evidence) links exact pins, source1
build failure, source2 failed13/missing original window close, original derive
project0/adapter1 and corrected raw reconsumption; none is rewritten by acceptance.
CORE's unavailable full695e01 snapshot qualification remains. S4b18 is next only
after S4a publication; S4c3/resources+metadata, S4d compatibility and S5 remain pending.
Only [H/project-execution-cursor.md](../../sol-lang/.git/agent-handoffs/p44-f1e/project-execution-cursor.md)
owns execution status; the [sole S4 index](../../sol-lang/.git/agent-handoffs/p44-f1e/s4-behavior/evidence-index.json)
is navigation, not authority. No tests/build/runtime/source repin for this docs carry;
existing accepted evidence requires no rerun. No full-suite/platform/leak/speed/broad
P4/P4.4/full-f1e claim: leak detection remains disabled, R-AUTH/held resealed ±1
OPEN, speed UNMET, host/E6/P4.5/P5 deferred.

## P4.4f1e direct ensures-call — historical S1 prepublication snapshot

**Historical status at this S1 prepublication snapshot (October 21, 2026):** The
statements below record the state before S1 publication and before S2 evidence
acceptance; they do not describe the current cursor. S1/S2/S3/S4a/S4b/S4c are now
published; S4d/full-S4 technical evidence is accepted with publication PENDING.
See the [accepted handoff](../docs/p4.4f1e-handoff.md#s4d-full-s4-accepted-evidence) and
[external sole project cursor](../../sol-lang/.git/agent-handoffs/p44-f1e/project-execution-cursor.md).
Historical receipts, timings, pins, failures and errata below remain unchanged.

The bounded f1e CORE source and its nine-key-per-mode validation are independently
accepted; E1 is closed for those 18 fresh runs only. Separately, the original
950/49a/cdae S1 technical evidence and the extracted candidate's bounded source/evidence
are independently accepted for S1. At that prepublication snapshot, the five-step
ledger had exactly one active step: final S1 documentation/tree checks, commit and
verified push. An earlier controller setup interruption is historical, not the latest
S1 disposition. Full f1e remains incomplete. The accepted CORE
selectors were:
`represented.ensures-calls.launch.traced`,
`represented.ensures-calls.retained.traced`,
`represented.ensures-calls.caller_false.traced`,
`represented.ensures-calls.callee_arithmetic.traced`,
`represented.ensures-calls.reject.wrapper`,
`represented.ensures-calls.reject.qualified-success`,
`represented.ensures-calls.reject.target-refinement`,
`represented.ensures-calls.reject.target-dead-text`, and the retained old identity
`represented.refinement-calls.reject.ensures-call` (now a paired-root positive).
All nine passed in both ON and SAN. Native maximum active/outer times were
1.211297s/1.252686s; SAN maximum active/outer times were 4.968905s/5.044370s.
Outer sampling followed raw/Result close, primary receipt fsync/close and post-close
witness fsync/close; active limits were 3s/27s and full limits <5s/<30s. SAN leak
detection is disabled. This does not accept the complete 49 nonfault keys or any
W/X allocation-fault inventory. These results bind the frozen CORE checkpoint, not
later allocator test-C revisions.

The scope is one direct pure plain `ALWAYS` ensures `FUNCTION` call, with one owned
plain `Int64` argument from `result` or `old` of the whole owned plain `Int64`
parameter and a `Bool` result. The accepted requires/refinement-call behavior and
ordinary scalar callee/SCC closure remain unchanged. The retained case uses the
unchanged genuine old F1D `launch` ENTRY plus private `ensures_root` as an
`INTERNAL_FIXTURE`; it invokes only the private raw alias twice for value 4. Its
static companion closure does not claim that ENTRY/refinement ran, and its trace
reset clears only `TRACE_COUNT` after the first complete ledger is authenticated.

The earlier four semantic PASS results remain historical at their original pins.
Their active limits were 5s/30s rather than 3s/27s, and their saved outer elapsed
fields copy primary receipt samples rather than sampling after durable witness
closure. Keep those results as semantic PASS with historical full timing UNKNOWN;
they are not runtime FAILs and were not repaired or relabeled. No leak or suite-speed
claim follows from the fresh CORE validation; the accepted CORE pin does not validate
the later allocator test-C revisions.

The S1 source test pin `950d5b50abe9293f15660e29aa9f08e656fea1fa0dd8fc7adfab989290175e2b`,
production `49a241484fcc5a31de2b7512704c803d44c5c86add69a84df5b049d3b02cc988`, and
fixture `cdaef751298f9cda6147868952d7e00d1cb061a6879064c5aaa921c290f47db9` were used
for one strict native ON target build and the exact traced-X discovery. Active was
2.839310s, durable complete 2.859487s, and distinct outer 2.882960s. The review accepts
the source-enforced checks, including 23 anchor rows through largest required END
12485, two independent owners, full bytes/15 Usage, four actual seven-render sets and
both closes. X344 rows numbered 1..344 are observed evidence, not frozen `N_X`.
Original slot and outer receipts remain FAIL; their ordinal-base and duplicate-observer
false negatives are reviewed errata, not rewritten receipts or a reason to rerun. The
four older PASS records retain historical full-timing UNKNOWN.

The 950 source snapshot is the accepted original S1 execution source, not the extracted
candidate. The controller setup stopped with `AttributeError` before a frozen plan or
selector; that partial setup audit is a preserved historical failed attempt, not a
validation receipt. The separate candidate test pin is
`fbb0806bc0cd52cdb15f5ea0abf031478c2e4939b8cfa4457b821b9f526c6ceb` (production
`49a241…`, fixture `cdaef7…`). Independent source and evidence reviews accept this
exact candidate for bounded S1 publication, pending documentation/final-tree review
and commit/push.

The candidate had one fresh native configure/build, followed by a no-child
pre-semantic bytecode correction (no rebuild), two native semantic cases, and 13
metadata invocations. Discovery X passed at 2.670197250s active / 2.714858500s
complete / 2.733556625s outer; retained-traced passed at 1.061952667s /
1.076693625s / 1.095858458s. Candidate binary/object SHA-256 are
`e2734001faf16002ddbc59f79b40fd2ecfbd318bbecafd5f35d867dc11d551e8` /
`3726447a8ab7fd90eff8211416649899b615807aa1bd78af57b0267331f542d1`. Metadata had
seven successful output invocations (manifest plus six lists) and six expected
empty-output rc2 negatives. This chronology/count correction is attributed to the
independent evidence review; do not edit the immutable validation report or receipts.
Candidate results are native only: no fresh candidate SAN or nine-case CORE replay.
The CORE 9 ON + 9 SAN checkpoint remains accepted at its own pins; the evidence review
approves only bounded historical continuity for S1, not automatic pin migration or
byte identity to the unavailable full `695e01…` test snapshot.

At this historical S1 prepublication snapshot, X at `6e10fdfe…` was an UNKNOWN cutoff
(active 3.004135583s / outer 3.079325s); there was no unchanged-source retry or deadline increase. Historical W at
`46e16a9d…` / binary `f8ee4a1302c2b75e21a66e3f6264ac2fc20458d9f6d23f98776b3273a6b8fb17`
passed at 2.405744s / 2.468456s with two owners and 257 provisional rows. These do
not establish the current S2 `N_W`; at this historical snapshot `N_W`, `N_X`, and `M`
and their digests were still unknown, and no W carry was claimed. S2 has since accepted
`N_W=257`, `N_X=344`, `M=601` arithmetic, and both actual-line digests; see the current
S2 handoff above. Prior failures, unknown timings, pins, and receipts remain unchanged.

At this S1 prepublication snapshot, only S1 publication was active: separate
documentation review, Astra/coordinator staged-tree and preservation checks, commit,
and verified push remained. S2 (independent W/X ON/SAN capture and pin freeze) was
blocked until verified S1 push; S3 was blocked on S2; S4 on S3; S5 on S1–S4. The
current post-publication cursor appears below.

At S1 publication, the candidate listed exactly nine real E keys (eight CORE plus the
single traced-X discovery key) and added only `contracts.ensures-call-core-s1`. It
registered no new fault selector/coverage, W/plain discovery, or future49 obligation;
omitted keys returned2, not false PASS. Its static inventory was 77 unique controls /118 expanded
control tuples /4,882 unchanged old fault IDs /29 sweeps /24 runners /50 shards /5,000
ordered tuples (baseline 4,999 plus one new control). X344 is observed native evidence,
not frozen `N_X`; `N_W`, `N_X`, `M` and digests remain unknown, not zero pins. That X
selector launch already included two independent owners and within-mode equality; it is
not fault coverage or the required W/SAN, source-classification, cross-mode, and reviewed
pin freeze. No second identical native-X launch or duplicate dumped inventory is
requested. The old 78-control /
`(4,882+M)` plan is original-monolith planning only and must be reconciled against the
published 77-control/5,000-tuple S1 base before S2/S3 additions; do not infer 79 or
guess pins. The original 12-launch unit is historical accounting, not current
allowance. No old-486, full-4,882, SAN replay, or broad CORE replay gate is added to S1.

The accepted f1d inventory remains 76 controls /4,882 fault IDs /50 shards and its
old lists 23/351/457/412. The 486 selected obligations per mode remain mixed
fresh/carry history (ON 111/375; SAN 318/168); each carry includes eight earlier
matching-pin mutation receipts. ON119/SAN326 are current-binary semantic
observations, not fresh f1e runs. The accepted S1 candidate adds one control to that
baseline: 77 unique controls, 118 expanded control tuples, 4,882 old fault IDs and
5,000 ordered tuples. Future M/fault additions and the 49 nonfault keys remain
unknown/pending, not registered as false tests or claimed complete.
The six-profile runtime bridge, remaining 49 nonfaults, all M faults, bounds/resources,
mutations/canonical controls, full default/selector inventory, metadata/CLI,
compatibility, independent reviews and milestone publication remain pending. R-AUTH
and held resource checks remain open, speed goals unmet, broad P4.4 unchecked, and
host/E6, P4.5 and P5 deferred. Preserve the active 3s/27s limits and complete
<5s/<30s durable-close limits for any future authorized discovery. See the
[interim f1e handoff](../docs/p4.4f1e-handoff.md) and
[TODO cursor](../TODO.md#execution-cursor).

### Records and real progress

Test-owned stderr records start with `SOL_TEST_PROGRESS` and tab-separated
`key=value` tokens: `event`, `test`, `case`, `phase`, `unit`, `completed`, optional
`total`. Tokens are restricted ASCII; totals are omitted when unknown. Entry and
exit records always flush. C code calls `sol_test_progress_completed` **after**
real work; repeated counters are throttled to about 1s (C wall time is only a
display throttle, with clock-jump guards). Keep case/phase name strings alive
until the next transition. Assertions and their failure counters stay authoritative;
`sol_test_progress_finish(status)` returns the supplied status unchanged.
Optional `fixture`, `ordinal_kind`, `ordinal`, `ordinal_total`, `hook`, `hook_after`
identify real nested work. Ordinal/hook records describe the **next attempt**, not
completed faults; `FAULT` coverage records retain their original arming point.
Only operation/iteration completion records advance completed counts. Existing
represented CONTROL/FAULT/SHARD lines and their IDs remain unchanged and flushed.

The supervisor forwards stdout and stderr as raw bytes, including partial lines,
and separately writes `SOL_TEST_MONITOR` + tab + JSON records to its own stderr.
Start identifies argv/cwd. `still_running` uses a monotonic clock and repeats the
last real phase/count, explicitly **unchanged**; when the child has no semantic
records, inner progress is **unavailable**. It never estimates a percentage or ETA.
Final records distinguish PASS, FAIL, TIMEOUT and INTERRUPTED. Child signal exits
remain signals on POSIX. Timeout exits 124; missing executables fail nonzero.

Readers drain both pipes through a bounded queue (32 x 4096-byte chunks). Only
bounded protocol lines (8192 bytes) and the last 128 semantic records are retained.
`run(..., capture=True)` spools exact stdout/stderr to temporary files; callers
must close the returned context-managed `Result`. Reading an entire capture into
RAM is the caller's responsibility, not a supervisor operation. A binary sink
must accept writes promptly, as with ordinary redirected process output.

Concurrent imported calls require an explicit main-thread cancellation owner:

```python
with cancellation_scope() as scope, ThreadPoolExecutor(max_workers=2) as pool:
    futures = [pool.submit(run, argv, name=name, scope=scope) for name, argv in cases]
    results = [future.result() for future in futures]
```

Import both functions from `run_with_progress`. Calls on the main thread create
a scope automatically; worker calls without an active explicit scope fail before
spawning. Keep the scope alive until the pool has joined. Its signal handler only
sets a scalar flag (no signal-handler locks); each registered worker performs its
own cleanup. Scope exit also cancels and waits for registered runs. Do not use
blocking sinks/observers: arbitrary Python callbacks cannot be interrupted safely.

### Ownership and cancellation

On POSIX each command starts in a private session/process group. Timeout, SIGINT,
SIGTERM, and importable `cancel` events terminate that owned group, then escalate
to SIGKILL and reap the leader while draining both streams. A successful leader
also cannot leave its group running. Nested supervisors own **distinct** child
groups: cancellation reaches the nested supervisor in its containing group; it
cleans its own group before the parent's strictly longer escalation grace. The
grace is 0.6/(nesting depth+1) seconds. Test children must not daemonize or detach
from their owned group. No name-based or broad process matching is used.

Forwarding/capture/observer exceptions use the same cooperative grace before
escalation. Cleanup drains and discards bounded output without calling the failed
sink or observer again. An I/O failure remains FAIL/127 with `Result.error`;
non-I/O callback exceptions propagate unchanged after cleanup. A secondary
cleanup failure is reported separately, not substituted for the original error.

On Windows a Python gate launcher waits before spawning the target. The supervisor
first assigns the launcher to an owned kill-on-close Job Object, then releases the
gate. Closing/terminating the job kills its descendants; nested jobs require
Windows 8 or newer. If job assignment fails, the gated launcher is killed without
ever launching the target. Target stdin on this path is noninteractive/DEVNULL
(CTest tests do not consume interactive input). SIGINT/SIGBREAK handlers support
graceful cancellation; forced external `TerminateProcess` cannot print a final
record, but kernel job-handle closure still cleans the owned tree. **The Windows
implementation is not validated on macOS; do not infer Windows acceptance from
POSIX results.** POSIX signal-specific tests skip on Windows pending platform proof.

Each CTest registration has a temporary local safety cap of 120s native / 180s
sanitized. Its supervisor deadline is 5s shorter to reserve cleanup time. These
are not performance acceptance budgets, and they do not justify longer tests.
Any later audit probe must independently use <=30s and classify timeouts as
partial/not acceptance, never a pass or skip.

## Bounded diagnostic inventory audit

```sh
python3 tests/audit_tests.py --build-dir build --per-test-seconds 30 --suite-seconds 120 --names frontend mir_runtime_conventions --output build/audit.json
```

For explicit bounded resume, see the [frozen-environment example](AUDIT.md#bounded-live-audit-and-resume).
The entire actual inherited environment is pinned, including volatile values;
launching later from another shell/tool call need not match. No secret values
need be printed or stored. With `--names`, the caller must select only authorized
eligible work: the CLI does not prevent repeating a prior PASS or ordinary cutoff.
Without `--names`, resume selects previously `NOT_RUN*` names, potentially including
expensive owners; that is not permission to launch them.

Inventory always comes from actual CTest JSON, never a handwritten fast suite.
Selection is exact-name, serial diagnostic execution, not reduced acceptance.
Every configured name stays in the receipt: `NOT_RUN_SELECTION`,
`NOT_RUN_SUITE_BUDGET`, `AUDIT_TIMEOUT`, `INTERRUPTED`, `PASS` or `FAIL`. Cutoffs and
unrun outcomes are **PARTIAL / NOT_ACCEPTANCE**, never skips or inferred passes.
Even a complete diagnostic inventory is not full acceptance. Exit 1 means partial
or failing diagnostics; exit 2 is an invalid/stale request. No output is silently
overwritten/reused: choose a new receipt or request `--resume` explicitly.
Disabled tests and tests with CTest skip policies remain explicitly
`NOT_RUN_DISABLED` /`NOT_RUN_SKIP_POLICY`; CTest's zero exit cannot prove that a
child ran. Neither is counted as PASS or complete. None of the original native
registrations uses these policies.

Use ignored build directories or external evidence paths for receipts/logs.
Resume requires equal tracked/untracked source hashes, HEAD/diff, build cache and
generated CTest configuration, inventory/argv/properties, executables/libraries,
represented read-only manifests and relevant runtime/sanitizer environment.
Artifacts must already be built from that source; hashing isn't a build. Pins
are rechecked after the cycle. Changing source/configuration/binaries requires a
**new** audit, not data migration. Interrupted receipts with unverified final pins
cannot be resumed. Reusing a matching old observation is explicit in its cycle ID.

The overall wall budget includes discovery and tiny-child overhead measurement;
4s is reserved before each child for owned cleanup/final receipt work. Each probe
deadline is <=30s, independent of the larger CTest safety ceiling. Per-probe flags
report budget overruns. The per-probe budget includes a cleanup
reserve (up to 3s), so a configured 30s probe stops active work at 27s.
The final cycle flag
reports any overall budget overrun. Audit output includes argv/cwd, full streamed
raw logs, monotonic delivery/semantic JSONL, bounded recent observations, maximum
delivery gap, wrapper overhead (not subtracted), and **1s unit /5s integration
/60s suite goals**. Timeouts cannot establish a goal or expected product outcome.
The recorded ALL60 validation separately authorized one suite-budget-shortened
full-default represented attempt and one eligible full-budget retry; the final ordinary
cutoff stays incomplete. That finished dispatch is not standing authorization for
further full-owner runs. Future represented/full-union probes require a new reviewed
scope with <=30s total bounds; read-only manifests or approved shards never replace
whole-registration correctness or full fault execution.

## Stress and CI fuzz

[Stress instructions](stress/README.md) describe live generation/digest/command/
summary phases and unchanged 30s per-case safety ceilings. CI fuzz commands use
the same supervisor with 180s smoke ceilings and finite job ceilings; all original
seeds, corpus/dictionary arguments, run counts, max lengths and per-input timeouts
are retained. Only libFuzzer's real iteration/status lines establish iteration
progress. The heartbeat names configured runs but does not invent a completed
count. Local macOS support proofs do not establish Linux CI, leak or Windows safety.

## S1 publication candidate — October 21, 2026 prepublication evidence snapshot

Candidate path `/Users/jsobotka/code/sol-lang-p44f1e-s1-publication`, branch
`p44-f1e-s1-publication`, base `dfceed6b3e6630271cdc6ba02cb011314f1b31f1`. Candidate
test SHA-256 is `fbb0806bc0cd52cdb15f5ea0abf031478c2e4939b8cfa4457b821b9f526c6ceb`; exact
production/fixture pins are `49a241…` / `cdaef7…`. The test is reconstructed from
baseline with the accepted 950 dependency closure, a bounded nine-key dispatcher, and
one new control. It preserves historical definitions/assertions, the converted legacy
F1D selector, 29 old sweeps and 4,882 old fault IDs. No new fault arm or E fault
registration is present.

Independent source and evidence reviews approve this exact candidate for the bounded
S1 checkpoint. One configure/build, two native semantic cases (discovery X and
retained-traced), and 13 metadata invocations passed at candidate pins. This does not
claim more than those two fresh candidate-native semantic cases; no candidate SAN,
full-default execution, or fresh 9×2 CORE replay occurred. At this prepublication
snapshot, S1 publication was pending and S2 was blocked. S1 has since been published;
the current cursor and S2 technical-acceptance status follow. The original seven-path WIP and
external preservation receipt remain untouched.

## P4.4f1e S2 — October 21, 2026 accepted prepublication snapshot

At the recorded S2 snapshot, S1 was COMPLETE and published as
`e93d375e2e9f59d9a819ad470770e741a1511f4a`; `origin/master` was verified at that
commit. Independent final evidence review accepts S2 technical inventory freeze.
At that snapshot, **only S2 publication was active:** documentation/staged-tree review and Astra
alignment precede the coordinator's commit, push, and remote verification. S2 has not
yet been committed or pushed. S3 is blocked on verified S2 publication and helper
review; S4 is blocked on S3; S5 is blocked on S1–S4. See the [sole cursor](../TODO.md#execution-cursor)
and [f1e handoff](../docs/p4.4f1e-handoff.md).

The post-pin validation source was
`dce792c20186e86a098f52360ce9de454afa8bcb20491574758f746c3b869872`; production was
`49a241484fcc5a31de2b7512704c803d44c5c86add69a84df5b049d3b02cc988`, and fixture was
`cdaef751298f9cda6147868952d7e00d1cb061a6879064c5aaa921c290f47db9`. The accepted
counts are **N_W=257**, **N_X=344**, and **M=601** (arithmetic only, not a new
registration). Digests preserve actual source lines; there is no normalized-line
digest:

| Profile | Actual-line digest | Module length / SHA-256 |
| --- | --- | --- |
| W | `defb27c27204ea4ddb0d51ccc088d202a3ed94ae4627213057868cea209d1c1d` | 27,789 / `9a455f12439190a2ea41116907c24967d3490988c16fefd9c0f4f743be79cb87` |
| X | `78c31c56e9447ea9654d4345b7874da167a5d2cb39fd926b80e7ccbeb4cb261a` | 75,850 / `352897de96bbe643b71e0625be6119dae0393517cd3948f48bb8f5127cf5f3a9` |

The reviewed initial evidence is three fresh captures (ON/W, SAN/W, SAN/X) plus the
approved S1 candidate ON/X carry (test `fbb0806…`, binary `e2734001…`). The post-pin
phase then passed exactly four fresh repeats, with no retries or configure commands:

| Fresh selector | Active (s) | Complete after witness close (s) | Distinct outer (s) |
| --- | ---: | ---: | ---: |
| ON/W | 2.075130 | 2.127275 | 2.148134 |
| ON/X | 2.439430 | 2.455822 | 2.476123 |
| SAN/W | 8.606935 | 8.655070 | 8.700591 |
| SAN/X | 10.889483 | 10.935070 | 10.979267 |

Each selected launch asserted two-owner full ordered records and complete module-byte
equality, all 15 `Usage` fields, complete witnesses, four actual seven-render sets,
and both closes. All 1,202 post-pin ordered rows match their initial profile rows and
the cross-mode row identities; the source classifier covers all 601 requests. Full
module-byte equality is asserted within each launch; cross-mode module identity is
length+SHA-256 because cross-mode raw module blobs were not retained. The full arrays
remain in the frozen inventory/report rather than being duplicated here. Exact Usage
vectors are documented in the handoff and final evidence review.

This is not acceptance of S3 faults or later f1e groups. The 10 current E source-listed
keys (eight CORE plus W/plain and X/traced) and static counts—77 unique controls, 118
expanded control tuples, 4,882 old fault tuples, 5,000 ordered tuples, 24 runners, 50
shards, and 29 sweeps—remain unchanged; the single S1 checkpoint control remains the
only added control. The historical 78-control /`(4,882+M)` plan must reconcile to this
77-control /5,000-tuple base with the future 49 nonfault obligations and M=601
arithmetic; do not infer a final 79. The old four lists are source-identical, but
no fresh default, metadata/list CLI, manifest, or sweep was run for S2. No new fault
registration was made. Source-classified would-fail paths are not injected failures;
`W1..257` and `X1..344` fault ordinals in both modes remain unlaunched, and helpers are
not accepted/published. SAN used ASan/UBSan halt-on-error with leak detection disabled;
no leak-checking claim is made. The original 950/49a/cdae S1 timing, FAIL receipts,
historical cutoff, old-source provisional W observation, and prior non-approvals remain
unchanged and are not replaced by candidate or S2 timings.

## Historical P4.4f1e S3 test-source proposal — October 21, 2026; before recovery decision

At the pre-recovery snapshot, S1 and S2 were complete and published (S1 `e93d375e2e9f59d9a819ad470770e741a1511f4a`,
S2 `b1f5e540f9b76ee2cc5d11589de09c6c319612c5`; S2 push and remote verification are
complete). The S3 test implementation and appended registrations were proposed, not
accepted. No build, selector, CLI, or runtime fault was launched for that proposal;
its proposed 1,202 ON/SAN receipts were not claimed. The S2 inventory remains frozen
at W=257 and X=344 with its accepted actual-line digests; source-classified would-fail
paths are not injected fault evidence. This historical source proposal is not the
current execution cursor or the new-baseline qualifier plan.

The proposed addition was 257 plain plus 344 traced registered
fault IDs, physically injected at the same ordinal `j`; it adds one contracts control
and no runner/shard. The proposed static expansion is 611 E keys, 78 unique controls,
119 expanded control tuples, 5,483 fault tuples, and 5,602 ordered tuples. Those
counts remain unaccepted until the complete source expansion and old-list/manifest
byte-preservation review. The four C pilots are accepted historical observations
under their old environment, not qualifiers for a new baseline. See the current
[sole cursor](../TODO.md#execution-cursor) and [f1e handoff](../docs/p4.4f1e-handoff.md).

## Historical P4.4f1e recovery checkpoint — October 21, 2026 (R2 then active)

The absent-snapshot, zero-qualifier and unlaunched-remainder statements below are
historical and superseded for status by the accepted S3 record that follows.

CORE, S1, and S2 are complete at their own pins; S1 and S2 are published. S3 is IN
PROGRESS as a container, and R2 reviewed baseline/pilot requalification is its sole
ACTIVE leaf. R1 repository persistence/review is COMPLETED following independent
approval of the selected decision and five-step plan; Astra's exit steering is
READY_R2.
The bounded diagnosis found that original complete environment values were not
retained; the specific later drift cause is UNKNOWN. The user authorized preparing a
new actual captured baseline without guessing or reconstructing old values. Four C
pilots remain historical accepted observations only. There are zero new-baseline
qualifiers and zero of the 1,198 remaining cases launched. The actual private
snapshot and manifest do not exist, and environment hashes remain null. No gate,
freeze, coordinator execution authority, build, configure, runtime, or S3 publication
has occurred.

Source/controller/freezer/capture preparation is
`APPROVED_PENDING_ACTUAL_CAPTURE`, pending actual capture. After capture, independent
final controller/pin-completion review remains required; this is not R2 completion or
execution authority. Source
`tests/wasm_represented_tests.c` at
`dd700c09b8fe4da69967e1147c2078cf71c5f8d3ae0029256306335289e634bd` is already
separately approved; source approval is not new-baseline runtime acceptance. The
pinned B capture utility is ready for a later one-shot actual capture. R2 will reuse
the existing ON/SAN binaries with zero new configure/builds; only after actual
capture, independent final controller/pin-completion review, gate, coordinator
authority, and freeze may it
run exactly four new-context qualifiers in two bounded windows, then pause for
independent evidence review. R3 is blocked on accepted R2 evidence and separate
continuation authority; R4/S4 and R5/S5 are sequentially blocked thereafter. Both original halts,
the failed D draft, prior receipts, and the preserved mixed WIP remain unchanged.

## Historical P4.4f1e S3 — October 23, 2026 accepted pre-push checkpoint

The publication-PENDING statements below are October 23's pre-push history,
superseded by the October 24 S3 receipt and S4a record above.

The [independent final1202 review](../../sol-lang/.git/agent-handoffs/p44-f1e/s3-faults/s3-final1202-review.md)
accepts C3–C8 and permits docs-only publication carry: **1,058 original-context
references +144 new SAN-X200..342,344 cases =1,202 physical identities, 601/mode**,
covering ON/SAN × (W1..257, X1..344). There are **525 real FINALs (381+144), not
1,202 FINALs**. SAN-W-0167, SAN-W-0229 and SAN-X-0199 are three distinct honest
`CASE_LEVEL_REFERENCE_CARRY` leaves without FINALs. Their original seven-item
missing-fact lists, historical failed/UNKNOWN exits and T attempt41
`UNKNOWN_NO_CHILD` remain unchanged; acceptance does not repair original
frontier/timing/closing/FINAL facts. See the [handoff](../docs/p4.4f1e-handoff.md#s3-accepted-evidence)
for those pointers and actual current source/build/environment attribution.

The accepted execution uses unchanged test source
`dd700c09b8fe4da69967e1147c2078cf71c5f8d3ae0029256306335289e634bd`, actual protected
recovery environment and reused ON/SAN outputs, not S2's initial maps or fresh
builds. The [sole index](../../sol-lang/.git/agent-handoffs/p44-f1e/s3-faults/s3-evidence-index.json)
links successful Q final integrity and sealed, durable waited capture with
controller0/wrapper0 and coordinator-preserved normal outer completion. Prior
original A/P exit/closing remain missing/UNKNOWN, not replaced by Q success.

Per-ordinal failed-prefix/sibling/diagnostic/empty-output/Usage15, immediate unarmed
retry/full records/module `memcmp`/Usage, witnesses, validators, seven-renderer
fences/sticky preservation and real close are **source-backed success-gating
assertions joined to actual raw protocols/receipts**, not retained full
owner/module/record blobs or a syscall trace. S2 cross-mode module identity remains
length+SHA256 only. Source/static expansion is 611 E keys, 78 unique controls /119
expanded controls /5,483 fault tuples /5,602 ordered tuples /24 runners /50 shards
/31 sweeps; these registrations and shared selected/default mapping are source
facts, not fresh CLI/list/manifest/default-run certification or execution of all
old faults. **CLI/default/metadata checks, tests, builds, qualifier and runtime
were NOT_RUN for this docs carry.** No replay or rerun is needed for publication.

Caps remain ON active≤3/full+distinct outer<5, SAN active≤27/full+distinct outer<30,
parent<120/reserve10, retaining historical timing qualifications. SAN uses
`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`, `UBSAN_OPTIONS=halt_on_error=1`;
leak detection disabled, no leak-freedom claim. No full-suite/platform/speed,
broad P4.4/full-f1e or R-AUTH/held resealed ±1 acceptance follows. S3 technical
acceptance is complete; publication stays **PENDING** exact staged-tree review and
coordinator commit/push/remote verification. S4/S5 remain blocked until verified
publication and separate assignments. The [old S3 cursor](../../sol-lang/.git/agent-handoffs/p44-f1e/s3-faults/execution-cursor.md)
is now a completed checkpoint, not active authority; these are dated evidence records.
