# Running tests with live output

The [October 17 audit](AUDIT.md) records all 60 configured UNIX ON names:
**58 PASS, zero FAIL, two cutoffs; NOT_ACCEPTANCE**. Observability is accepted for
its bounded scope; the 1s unit / 5s integration / 60s suite speed goals are not.
It also records source identity, preserved coverage and unresolved platform gaps.

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
