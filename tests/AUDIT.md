# Test speed and progress audit — October 17, 2026

**All 60 configured UNIX ON children actually started: 58 PASS, zero FAIL,
two final ordinary `AUDIT_TIMEOUT` results. This is PARTIAL / NOT_ACCEPTANCE,
not all tests passing or all tests fast.** No test was disabled, removed, skipped
by policy, sampled away or replaced with a shard to obtain these results.

## Status and source identity

[`TODO.md`](../TODO.md#execution-cursor) is the single execution cursor. Test speed
and continuous progress remain the active overall priority. T1 observability and
bounded auditing are independently approved as
`6eef9cadf917b3978278fed9b656b35fc95646ef` (`6eef9ca`).
T2's nine-file cleanup/portability source change is independently review-approved;
source and audit documentation are committed as `5012c62`. Both checkpoints are
integrated and available on pushed `master` at `35f4027`.
Approval covers correctness, portability and bounded diagnostics,
**not full speed acceptance**. After this documentation handoff no new
implementation is active without coordinator steering and a new Sol task brief.
P4.4, P4.4f1a and the resource-prerequisite WIPs remain paused and unchanged.
T1/T2 are not the separate, gated maintenance-experiment M1–M5 track.

The ALL60 evidence predates `5012c62`; its audited code is unchanged in that
source commit, which follows T1 `6eef9ca`. Post-audit documentation-only
differences mean the receipt does not match `5012c62`'s complete all-source identity.
Audited cleanup test SHA256:
`cf4746c2d3471a83ac2ef9d059005c0aabd688e55584e49465cbba441174d5c2`.
Guarded engine/header SHA256, respectively:
`b84fef3c48e1479348f98cf7fd980030a4ceab46b945a9647019db69781e1c8a` /
`8ef946295aa4f8040dc0c5ea970ad5be2cf262f7cc5c60d371bdcab7897b1e44`.
The nine source paths are `include/sol/mir_runtime_cleanup.h`,
`src/mir_runtime_cleanup.c`, `tests/support/path_work.h`, and
`tests/{mir_concrete,mir_runtime_conventions,mir_runtime_values,mir_runtime_cleanup,mir_runtime_host_abi,mir_runtime_handler_abi}_tests.c`.

This tracked summary transcribes the reviewed `implementation-fresh-audit-all60`
handoff's `validation-report.md`, `all60-status-table.md` and
`cutoff-boundaries.json`; its detailed raw receipts remain local coordinator
evidence, not portable repository links. It also qualifies earlier T2 batch and
completion evidence rather than treating it as fresh whole-suite validation.
**Post-audit documentation edits change the audit's all-source hashes.** Audited
code is unchanged at `5012c62`, but old receipts cannot be relabeled as matching
that commit or later documentation-only source states; any new source-pinned run
needs a new receipt.

## What actually ran

The full existing ON build completed once before pinning: 39 steps, 2.041561s
supervised, no pending Ninja work afterward. Configuration was Debug/Werror,
`BUILD_TESTING=ON`, `SOL_ENABLE_WASM_BACKEND=ON`, sanitizers OFF, on Darwin/macOS.
The 60-name CTest JSON inventory matched T1: **58 original UNIX ON registrations
plus the two added support registrations**. Historical OFF inventory is 54
original plus two support; no fresh full OFF inventory run is inferred here.
The actual full-default represented argv had no `--shard` or inherited
`SOL_P44E_FOCUS` selection.

| Cycle | Actual attempts | Cumulative result | Audit invocation s | Driver outer wall s |
| --- | ---: | --- | ---: | ---: |
| 1 | 33 | 31 PASS, two cutoffs, 27 unrun | 116.113232 | 116.163778 |
| 2 | 28 | 58 PASS, two ordinary cutoffs, zero unrun | 45.085128 | 45.165681 |

Each invocation had a 120s overall diagnostic budget. There were **61 attempts**:
60 distinct names and exactly one eligible retry of the suite-budget-shortened
represented attempt. No PASS, FAIL or ordinary per-test cutoff was repeated;
no third cycle or deadline increase followed. Both cycles had stable pins, clean
owned teardown and no invocation/per-test total budget overrun. Audit exit 1
means partial diagnostics, not a failed completed registration.

Normal allocation was **30s TOTAL = 27s active + 3s cleanup reserve**. Five
completed cycle-1 tests had shorter allocations as the suite budget decreased;
they passed and were not retried. Represented's first allocation was
2.238934s active / 2.985245s total, outer 2.273204s, `SUITE_BUDGET`. Its one
retry received 27s/30s and ended at an ordinary `PER_TEST_BUDGET` cutoff, as did
lowered. Neither cutoff proves a full runtime above 30s or whole correctness.

### All configured names: final outcomes and timings

Seconds below are copied from the final-attempt receipt: **outer** is the audit
CTest/process-tree wall clock; **native** is the terminal native supervisor's
elapsed clock (not CPU time). These clocks nest; do not add them. A cutoff has no
terminal native runtime. The retry's first attempt is recorded above, not omitted
from the attempt total. † marks added support; all other rows are original names.

| Configured name | Final status | Outer s | Native s |
| --- | --- | ---: | ---: |
| progress_support † | PASS | 3.535844 | 3.427924 |
| progress_integration † | PASS | 4.550627 | 4.458157 |
| frontend | PASS | 0.335312 | 0.263763 |
| compilation | PASS | 0.343544 | 0.275330 |
| formatter | PASS | 0.338504 | 0.263628 |
| sema | PASS | 0.366009 | 0.298849 |
| typecheck | PASS | 0.353696 | 0.285593 |
| effectcheck | PASS | 0.372403 | 0.283301 |
| contract | PASS | 0.333339 | 0.262710 |
| package | PASS | 0.344052 | 0.269576 |
| ir | PASS | 0.365807 | 0.287538 |
| mir | PASS | 0.444287 | 0.374739 |
| mir_eval | PASS | 0.381064 | 0.312973 |
| mir_program | PASS | 0.516384 | 0.442147 |
| mir_plan | PASS | 0.457330 | 0.377925 |
| mir_materialize | PASS | 0.543381 | 0.475688 |
| mir_representation | PASS | 0.801416 | 0.724508 |
| mir_layout | PASS | 1.027550 | 0.958057 |
| mir_operations | PASS | 1.805224 | 1.741477 |
| mir_linkage | PASS | 0.586730 | 0.501699 |
| mir_concrete | PASS | 2.553695 | 2.484021 |
| mir_runtime_conventions | PASS | 2.633469 | 2.566265 |
| mir_runtime_values | PASS | 6.950069 | 6.882477 |
| mir_runtime_cleanup | PASS | 12.942662 | 12.864952 |
| mir_runtime_host_abi | PASS | 11.788102 | 11.672120 |
| mir_runtime_handler_abi | PASS | 19.404432 | 19.307268 |
| mir_runtime_lowered_program | AUDIT_TIMEOUT — PER_TEST_BUDGET | 27.028283 | unknown |
| interpreter | PASS | 0.412161 | 0.313715 |
| inspection | PASS | 0.450052 | 0.388148 |
| test_declarations | PASS | 0.367475 | 0.296706 |
| wasm_backend | PASS | 0.378936 | 0.314079 |
| wasm_scalar | PASS | 9.346218 | 9.275754 |
| wasm_represented | AUDIT_TIMEOUT — PER_TEST_BUDGET | 27.045592 | unknown |
| wasm_backend_config_rejections | PASS | 2.837773 | 2.736390 |
| cli_valid | PASS | 0.321148 | 0.255551 |
| cli_mutable | PASS | 0.172391 | 0.083848 |
| cli_package_valid | PASS | 0.156102 | 0.085628 |
| cli_package_mutable | PASS | 0.196423 | 0.127663 |
| cli_package_private_import | PASS | 0.155476 | 0.090008 |
| cli_package_missing_import | PASS | 0.151053 | 0.088444 |
| cli_package_duplicate | PASS | 0.167109 | 0.096299 |
| cli_invalid | PASS | 0.164982 | 0.095977 |
| cli_unresolved | PASS | 0.163362 | 0.095754 |
| cli_type_error | PASS | 0.153326 | 0.090002 |
| cli_effect_error | PASS | 0.156598 | 0.091505 |
| cli_authority_error | PASS | 0.164568 | 0.093687 |
| cli_ownership_error | PASS | 0.164496 | 0.091407 |
| cli_borrow_error | PASS | 0.158432 | 0.091095 |
| cli_region_error | PASS | 0.156555 | 0.088111 |
| formatter_cli_stdout | PASS | 0.242563 | 0.178746 |
| formatter_cli_check | PASS | 0.233466 | 0.168781 |
| formatter_cli_rewrite | PASS | 0.406137 | 0.326960 |
| formatter_cli_malformed_directory | PASS | 0.229552 | 0.164570 |
| formatter_cli_symlink | PASS | 0.216904 | 0.142736 |
| formatter_cli_exclusive_options | PASS | 0.215796 | 0.138482 |
| test_cli | PASS | 3.032752 | 2.959672 |
| run_cli | PASS | 3.069322 | 2.976573 |
| effects_cli | PASS | 0.776027 | 0.687574 |
| inspection_cli | PASS | 1.533901 | 1.427472 |
| e6_conformance | PASS | 0.715422 | 0.646130 |

All five prior root-path portability failures are now whole-registration green:
`mir_concrete`, `mir_runtime_conventions`, `mir_runtime_values`,
`mir_runtime_host_abi`, `mir_runtime_handler_abi`. Both original handler E6
zero-owner invocations remain. Root-dependent byte/work expectations now derive
from the actual physical fixture path; no failure gate or assertion was removed.
This fresh run used one physical root, not an extra relocation experiment; the
separately reviewed two-root portability proof remains separate. The old
52 PASS / five FAIL / two cutoffs / one unrun receipt is prior-source history.

## Exact incomplete boundaries

**Lowered:** last completed full validate was delivered at 26.881678s; the next
full validate began at 26.881789s in `test_slice_d_raw_owner_preflight`
(`tests/mir_runtime_lowered_program_tests.c`, audited lines 880–980). There were
66 lowered-validator starts / 65 completions in the registration, including
10 / 9 in that case. Exact hostile field, iteration and instruction at kill are
unknown; absence of CHECK failures is not PASS.

Site domains/vocabulary, build-work/render identity, E6 census, graph joins,
event/value joins and slice-C cleanup/host completed. Raw-owner preflight did not.
The 20 later named cases never started:
`test_slice_d_e6_usage`, `test_slice_e_render`, `test_slice_d_resource_limits`,
`test_slice_d_allocation_faults`, `check_pre_operation_render_stability`,
`test_erased_loop_census`, `test_propagation_value_demand`,
`test_pattern_copy_pipeline`, `test_nontrivial_copy_demand`,
`test_callback_prerequisite`, `test_callable_hole_prerequisite`,
`test_callable_hole_exact`, `test_immediate_method_prerequisite`,
`test_require_never_fallback_pipeline`, `test_bound_environment_indirect_graph`,
`test_source_backed_p36_graph_keys`, `test_generic_type_place_keys`,
`test_slice_c_handlers`, `test_long_virtual_path_validation_work`,
`test_snapshot_cleanup_mirror`. Final main render, authentication
mutation/restoration, below-limit constructor, owner/fixture teardown and terminal
result were not reached. Lowered full correctness, portability and later fault
coverage remain unknown.

**Full represented:** the default runners `api` and `e1` completed PASS, then
`refined` began. Five distinct CONTROL starts and seven FAULT arming IDs
(`refined.0001`–`refined.0007`) were observed. Refined backend failure calls 1–6
returned with subsequent cleanup/reset/next-ordinal progress. Ordinal 7 was armed
but its backend call had **not started**: last completed predecessor was host ABI
build at 27.001365s; active delivered phase was handler ABI build at 27.001456s
in `build_named_root` for `tests/conformance/p44c_refined_leaf`.

Backend fault 7, ordinals 8–162, the final successful post-sweep rebuild,
relocation/source-rebuild seal and enclosing refined runner completion were
unobserved. The audited fault loop (lines 9172–9187 of
`tests/wasm_represented_tests.c`) checks failure/output and resets each ordinal,
then performs one successful rebuild after the full sweep; there is no
per-ordinal successful retry to infer. All 21 later default runners were unstarted:
`step-owner`, `depth`, `contracts`, `qualified-frozen`, `qualified-result`, `stress`,
`callable-holes`, `callback-inout`, `methods`, `callback-table-wire`, `generic-roots`,
`scalar-products`, `patterns`, `terminal-packets`, `cleanup-trace`,
`panic-wire-resource`, `product-tree`, `sums`, `propagation`, `unit-text-sums`, `text`.
The 1459 completed represented-build adapters include controls/helper sweeps;
they are not 1459 fault-ID completions. No exact instruction at kill is claimed.

## Preserved work and measured cleanup contribution

T2 uses an immutable, callback-free **guarded test hook**, not cached trust or a
production compiler/runtime semantic change. Standalone selectors still fully
authenticate; full validators, hostile mutations and restore checks remain.
Hooks-off byte-identity/symbol evidence is earlier reviewed evidence, not a new
ALL60 production proof. No production validation is skipped.

The current whole ON cleanup completed all original 1059 event obligations,
1025 A queries, 1074 B queries (3 contract / 23 pending / 1048 rejection rows),
and **17 actual original local requests** (3 selected / 14 rejected). Its printed
A/B/local counters are **2116 selections / 3 authentications**, not a whole-program
authentication total. The small 102-query standalone-vs-batch contract, all 4096
boundary rows, fault/quota/precedence/public-detail/hostile-mutation checks,
both exact-limit renders/equality and final teardown completed. The 4096 rows
are source-plus-terminal-completion evidence, not a separately printed dynamic
census. Actual cleanup test adapters completed **255/255 full validates,
31/31 renders, 45/45 builds**, plus all five persistent-fault ordinals, eight
quota rows, 18 plain precedence calls (one negative / 17 positive), two
attempted-later and three scoped contract-later calls. The expensive tail was
retained, not bypassed by batching or replaced with expected counts.
Whole PASS establishes the original default conditional/loop behavior, not that
every syntactic branch executed. Exact dynamic CHECK evaluations, unprinted public
detail call counts and library-internal validator counts remain unknown.

Whole cleanup correctness is PASS: prior final-source OFF **11.887436s** and fresh
ON **12.864952s native** (12.92s CTest, 12.942662s audit outer). Both **MISS5**;
OFF/ON differ by configuration and are not a controlled speedup comparison.
Earlier T1 trace evidence was only **280 completed calls / 21.359s**, with cutoff
at event 307/1059. Earlier T2 A/B diagnostics measured **1025 / 0.216502s** and
**1074 / 0.081105s**, each with one authentication, on an earlier candidate test
source before final boundary/pending-copy additions. They establish a bounded
contribution, not final-source whole-test before/after timings or a speedup factor.
Later final-source A/B/local contribution probes preserved all 2116 actual
requests; their partial scope likewise does not replace the whole cleanup run.

Current independently emitted represented metadata matches the retained manifest:
**69 distinct controls / 3779 distinct fault IDs / 50 shards**. The 110 raw control
rows include repeated IDs across shards; they are not 110 distinct controls.
This preserves exhaustive coverage **metadata**, not execution of all those
faults in this audit. CONTROL/FAULT starts and runner-prefix PASS cannot establish
full represented correctness.

## Progress, performance and remaining acceptance

Fresh maximum delivered gap was **1.019882s**, consistent with approximately 1s
liveness. Raw stdout/stderr streamed unchanged; assertions retain authority.
Heartbeat repeats the last observed real stage/count without claiming completion,
percentage or ETA. `progress_integration` had no semantic record stream: inner
stage is UNKNOWN, with raw unittest labels and command liveness, not invented
phases. Some direct CLI rows likewise have unknown inner stages.
T1's 13 support / three integration checks remain prior code-only support proof;
T2 changed no support implementation. Both support registrations also passed in
this fresh run. This is bounded observability evidence, not portable acceptance
of every platform/configuration.

**Targets remain 1s unit / 5s integration / 60s suite: UNMET.** Five completed
registrations miss 5s: handler 19.307268s, cleanup 12.864952s, host 11.672120s,
scalar 9.275754s, values 6.882477s native. Lowered and represented have censored
attempts, not measured full runtimes. The two cycles are not a full-suite timing.
Serial attempt spans sum 157.488488s (including the shortened retry); invocations
sum 161.198360s and driver outer walls sum 161.329459s. Build/preparation is
outside those cycle totals. Support consumes 8.086471s of attempt spans; no
support, wrapper or discovery overhead is subtracted to manufacture a speed pass.

Measured residuals for a **new reviewed brief**, not active implementation:
represented `e1` 20.881415s completed; handler exits 8.307373s completed; lowered
slice-C 8.060112s and E6 census 5.141628s completed inside an incomplete owner;
values inventory 6.072175s; host exact-limit/fault group 5.977306s; scalar depth65
and depth64 boundaries 3.493779s / 3.447639s; cleanup's 17 positive CFG-search
API spans total about 2.692s. These delivery spans nest in whole timings and are
not CPU attribution or additive to native/outer totals. Represented/lowered
unobserved tails remain unknown and cannot be ranked as measured totals.

No fresh full SAN, Windows, Linux/leak, separate stress/fuzz or wider product/CI
matrix acceptance is established. Earlier tiny sanitizer evidence is scoped and
historical; Darwin leak limitations remain. The fresh source/build/artifact,
inventory, environment and manifest pins were stable, with supplemental linked
dependency checks, but Darwin shared-cache-only system library bytes could not
be hashed. Do not infer reproducibility on another OS image or claim the complete
product matrix from this one configuration.

## Bounded live audit and resume

From the repository root, first configure and build the intended configuration
using [the README commands](README.md). Canonical **live** full CTest is:

```sh
ctest --test-dir build --verbose --output-on-failure -j 1
```

This is not a bounded speed probe. Keep `--verbose` even when redirecting output;
`--output-on-failure` alone buffers successful output. Do not dispatch known slow
full owners solely to reproduce this document.

The following small diagnostic example uses the **actual audit CLI**, one frozen
copy of the actual inherited environment, explicit eligible name selection and
explicit resume. It streams output live, does not print/save secret environment
values, does not replay completed names, and is not a replacement suite. Use a
new output path; no source/configuration/binary/environment changes may occur
between cycles. The audit hashes all tracked/untracked source, including docs.

```sh
python3 - <<'PY'
import json
import os
from pathlib import Path
import subprocess
import sys

env = dict(os.environ)  # Same actual snapshot for both parents; no env dump.
receipt = Path("build/audit-example-new.json")
if receipt.exists():
    raise SystemExit("Choose a new receipt path; existing evidence is not overwritten")
base = [sys.executable, "tests/audit_tests.py", "--build-dir", "build",
        "--per-test-seconds", "30", "--suite-seconds", "120",
        "--output", str(receipt)]
first = subprocess.run(base + ["--names", "frontend"], env=env)
if first.returncode != 1 or not receipt.exists():
    raise SystemExit("Inspect the first diagnostic before any resume")
state = json.loads(receipt.read_text())
if (not state.get("pins_stable")
        or state["tests"]["frontend"]["status"] != "PASS"
        or state["tests"]["compilation"]["attempts"]):
    raise SystemExit("Unverified pins or ineligible next name; stop")
second = subprocess.run(base + ["--resume", str(receipt),
                                "--names", "compilation"], env=env)
raise SystemExit(second.returncode)  # Normally 1: other inventory names unrun.
PY
```

The tool's default resume selects `NOT_RUN*` names; explicit `--names` overrides
that selection and does **not** itself forbid replaying a PASS/ordinary cutoff.
The caller must enforce the approved scope. Existing output is never silently
overwritten or reused: choose a new path or explicit `--resume`. Exit 1 is
partial/failing diagnostic; exit 2 is invalid/stale input. Unverified interrupted
receipts cannot resume. Changed pins
require a new audit, never receipt migration or filtering volatile environment
keys to force a match. A matching previous observation retains its original cycle.

For a separately authorized all-name diagnostic, omit `--names` on the first
cycle, retaining `--per-test-seconds 30 --suite-seconds 120` and a new receipt.
Resume only the explicitly approved union of still-unattempted names and any
eligible suite-shortened timeout, with the same environment and pins. The recorded
ALL60 run used full represented first in cycle 2, then 27 previously unattempted
names; its final ordinary cutoffs are not eligible for another retry. This is a
reproduction method, not standing authorization for another full run.

Receipts and per-cycle raw/semantic/delivery logs belong in ignored build paths
or external evidence storage. Read-only metadata inspection, when built, is:

```sh
ctest --test-dir build --show-only=json-v1
build/sol_wasm_represented_tests --manifest
build/sol_wasm_represented_tests --list-shards
```

These commands inventory the actual configuration; manifests do not execute fault
coverage. All cutoffs, interruptions, disabled/skip-policy names and selection/
suite-budget omissions remain explicit partial outcomes, never inferred passes.
