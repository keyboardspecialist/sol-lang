# Sol Compiler Status

**Snapshot date:** September 9, 2026. **Implementation baseline:**
[`6ec4ba9`](https://github.com/keyboardspecialist/sol-lang/commit/6ec4ba9).
E1-E6, P1, and P2.1-P2.6 are complete at this baseline; P2 overall remains open.
This relocates the detailed baseline README documentation, reconciled with the
approved ledger. It is not a second checklist, a new test report, or a stable ABI
specification. [TODO.md](../TODO.md) preserves checkpoints and links the external
sole project execution cursor; this document does not own a queue.

**Status precedence — October 24, 2026:** S3/S4a/S4b are published/remote-verified;
[S4c is technically accepted, publication PENDING](#s4c-accepted-evidence).
Older checkpoints below, including S3/S4a/S4b pre-push status, are historical.
Only [H/project-execution-cursor.md](../../sol-lang/.git/agent-handoffs/p44-f1e/project-execution-cursor.md)
owns the live step; the [old S3 cursor](../../sol-lang/.git/agent-handoffs/p44-f1e/s3-faults/execution-cursor.md)
is a completed checkpoint. [TODO.md](../TODO.md) is checkpoint navigation.

**Historical worktree addendum, October 3, 2026:** P2.7, P2.8, P3.1-P3.6, and
P4.1-P4.3 were independently approved and complete at that snapshot. P4.3 completes
bounded private represented-value/place/indirect-call emission. P4 remained open;
P4.4 was the next production checkpoint. The baseline hash above remains the
historical September 9 snapshot; no post-baseline commit hash is asserted here. M1
completed as infeasible and M2-M5 remain gated unless it is reopened feasibly.

**Prior status addendum, October 18, 2026:** The bounded P4.4f1a metadata
checkpoint is independently accepted and review-approved, committed as `5c9c056`,
fast-forwarded onto `master`, and pushed to `origin/master`; it does not close P4.4.
P4.4 remained open and unchecked. At that prior snapshot, the sole live cursor was bounded planning for
residual predicate execution over richer forms; the test-speed goals remain unmet,
not resolved. Publication does not refresh or extend the handoff's accepted test
receipts. See [TODO.md](../TODO.md#execution-cursor) for execution order and the
[P4.4f1a handoff](p4.4f1a-handoff.md) for evidence and remaining boundaries.

**P4.4f1b published-slice record, October 19, 2026:** The bounded leaf-predicate
milestone received independent technical and evidence acceptance and was committed
as `ca18c6d37c6c464836be01f02b06713319d0d069` (`Execute scalar leaf calls in requires
predicates`) and pushed to `origin/master`. The subsequent status-only history
checkpoint is `aacd482`. The implementation/validation source baseline remains
`df97d7385e7aead65c78d2bba08a2e756cc9ab8c`; publication does not refresh its test
receipts.

Per mode, the original 372 ON/SAN selectors (349 f1b +23 f1a) are accepted at their
frozen pins; two ordinary identity keys additionally passed on final ON/SAN binaries.
This is a 374-obligation semantic/configuration union per mode, not a fresh
374-case run of the final runners. At the f1b publication snapshot, the list was
351 f1b +23 f1a and the next cursor was f1c planning; that cursor has since advanced
through implementation and correction review. The f1b addition was one pure direct
source-call leaf (`owned Int64 -> Bool`) in an INSTANCE-owned ALWAYS `requires`
predicate, with whole-target scalar/dead-block restrictions. That historical f1b
boundary does not describe the wider transitive callee admission in f1c below. See
the [P4.4f1b handoff](p4.4f1b-handoff.md) for its pinned evidence.

**Historical status addendum, October 20, 2026 (P4.4f1c; accepted and published):**
Independent final technical/evidence review and documentation review are approved.
The bounded implementation and its exact evidence were published as
`24e2d16f6047dd390900c36b2b9c7cb583ba985a` (`Execute ordinary scalar callee chains
in predicates`), pushed to `origin/master`. Read-only verification found HEAD and
`origin/master` equal to that commit; the worktree was clean at publication
verification. Astra readiness, coordinator integration, three corrected-binary
smokes, and preserved-WIP checks are complete. This accepts the bounded f1c slice,
not broad P4.4 closure, R-AUTH closure, a full suite, or project-speed success. The
status-only documentation refresh was published as `7d19435`.

Under the unchanged f1b predicate shape, f1c admits pure ordinary scalar callee
chains and ordinary recursive SCCs. The predicate remains exactly one direct
FUNCTION call in an INSTANCE-owned, non-refinement ALWAYS `requires`, with one owned
plain `Int64` argument and a plain `Bool` result. Each reachable ordinary callee has
one owned plain `Int64` parameter and a plain `Int64` or `Bool` VALUE result. Whole
reachable image, owned-predicate, snapshot/projection and cleanup checks remain
plain `Int64`/`Bool`/`Unit` and include dead blocks. Nested owned-predicate `INVOKE`,
mixed image/predicate cycles, unsupported signatures, and rich/dead-Text targets
reject. Ordinary SCCs are admitted; predicate-call recursion is not. Existing
catalog authentication and Kahn/SCC metadata are reused; the closure walk is bounded
by actual authenticated V/C, not a universal 256-function catalog limit. No emitter,
public ABI, header/schema, IR observer, or allocation request changed. Ordinary calls
retain real frames, inline predicates add none, step code 6 precedes depth code 7,
and pending callee failures remain primary.

The reviewed fault-only test correction replaces three intermediate full snapshot
renders with complete byte comparisons: five seven-owner render sets become two per
fault. Complete root/header/capacity/requested-extent byte checks remain at every
build boundary. A complete source/input/context proof supports only inferred
intermediate rendered-content equality; initial/final fresh renders, authentication,
validators, witnesses, cleanup, snapshots, canonical identities, full artifact bytes,
and all-15-`Usage` comparisons remain. There is no trust cache/seal shortcut and no
change to shared f1b snapshot defaults. The source/binary pins are recorded in the
[f1c handoff](p4.4f1c-handoff.md).

The exact semantic/configuration union is **506 keys per mode =457 f1c +49 named
old** (23 f1a +19 mutations +6 resources +1 `launch.traced` reset/semantic key).
Original classification was 492 fresh +14 reviewed reuse; overlapping targeted
replacements count once. Final current-binary accounting is **ON 139 executions
+367 historical carries** (356 old-fresh +11 reviewed reuse) and **SAN 495 executions
+11 historical carries**. Each mode has eight already-reviewed current-pin targets
(five originally fresh and three reuse), followed by ON 131/131 and SAN 487/487
continuation passes. All 506 unique keys are now evidenced in each mode; historical
carries retain their original source/binary pins and timing, and are not current
fresh runs. There is no native-to-SAN inference.

Both modes account for all **424 new fault IDs**: Q registered IDs 1–127 map to
physical allocations 3–129; R IDs 1–297 map to 1–297. The two legacy Q physical
allocations 1–2 retain their original f1a identities and failure semantics; after
the preserved two-attempt failure, the unarmed retry succeeds with 129 requests,
three catalog rows, and result 4. The 74-control /4,515-fault /50-shard manifest
preserves all 4,204 old ordered tuples and multiplicity. It is an inventory, not a
claim that all 4,515 faults or all shards ran.

The two supplementary ordinary selectors also passed per mode against the retained
f1b reference: direct result 7 (4,001 bytes) and recursive result 42 (8,944 bytes),
with full module bytes, all 15 `Usage` fields, and typed catalog identities. All 20
CLI negatives behaved as specified: exit 2, empty stdout, and no fixture/backend
work. Q/R digests `172cc966…` and `8a47dee2…` are allocation-inventory digests, not
module hashes. D96/T203 line-only bridges and unchanged backend-gated OFF compile/
dependency evidence are conditionally reused; no OFF build or owner test was run.

Coordinator smoke verification used the accepted ON executable
`8af1edb75efdc6dfc04583a3d618b97073c0a3d379e27b4dd58325bf107beeae`: recursive
preflight PASS (0.228s), retained-nonleaf traced PASS (0.172s), and full R164 fault
PASS (0.991s), all under their strict caps. These checks close the integration gate;
they do not replace the exact validation ledger above.

Every selected case stayed within active/full caps of native 3s/<5s and SAN 27s/<30s,
including receipt/witness closure. SAN used
`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` and `UBSAN_OPTIONS=halt_on_error=1`;
leak detection was disabled. Preserve the historical native R discovery TIMEOUT/124
at 3.006150583s, old-pin R164 TIMEOUT/124 at 3.025444458s, and six old false-baseline
fault FAILs; none is relabeled or retried unchanged. No default/full-suite, all-fault
execution, leak, platform, or broad performance claim follows. P4.4 remains unchecked;
R-AUTH and held `build_work`/`build_scratch_bytes` ±1 checks remain open; speed goals
are unmet. P4.5, host/E6, and P5 remain deferred.
See the [P4.4f1c handoff](p4.4f1c-handoff.md) and
[TODO.md](../TODO.md#execution-cursor) for the cursor.

**Historical status addendum, October 20, 2026 (P4.4f1d; technical/evidence accepted
and feature published):** The accepted slice is one direct pure predicate call from a
type-owned refinement: `type T = refined Int64 where helper(self)`. It requires the
exact refinement context and SELF binding, a plain owned `Int64` backing, and a
direct `Int64 -> Bool` call. Existing pure ordinary scalar callee chains and
recursive SCCs remain beneath that call, subject to prior plain-image restrictions
and call-free target-owned predicates. Wrappers/computed SELF, nested calls or mixed
cycles, ensures, Bool/Unit signatures, refined or Text target interiors, PanicText,
and the existing guard-call case remain excluded. Full indexed refinement-body and
`CHECK_REFINED` preflight remain required; the NULL-index failure-preflight caller
gets no fallback scan, cache, or authentication bypass. No emitter, ABI, schema/header,
IR, observer, or allocation-interface change is part of this slice.

**All bounded f1d source and the final dependency-based semantic evidence union are
independently approved.** Frozen inputs are test
`d03cd55767bdc831f9a3086a2658dc4ab9e8c796a5d4599574802b71fe8c27f8`, production
`8ed0b9a6b2c2a877d12270b2f70714a7681741790bb3bd1823ffe581d403eca2`, header
`6c10e20828b9e0e0fb7ed0cae4c16a7f48385b61240723bb1288b6de36a4b3c7`, fixture
`24a3e83c5496082bc7cf9759befa837ff18eb42bb82ec1e17fc16bdc19416cd5`, ON binary
`9de1615caaf81b485cdf589de81245019f3693ae68dd27ad6a30a67cbadbfb5f`, and SAN
binary `fc5897fe894f8a5c8a1b6e46bf43b3017f0308c88a74c728a7c570a0c1081fd6`.
The feature `Execute direct refinement predicate calls` was committed as `42fd4cd`
and pushed to `origin/master` (`7d19435..42fd4cd`). This publication does not refresh
the accepted source/binary evidence pins or timings. It is not broad P4/P4.4 or speed
acceptance.

Each mode resolves **486 selected obligations = 412 f1d (45 nonfault + 367 new
faults) + 74 compatibility**, with zero pending validation keys:

| Mode | Fresh final-batch PASS | Reused/carry PASS | Total selected | Pending |
| --- | ---: | ---: | ---: | ---: |
| ON | 111 | 375 | 486 | 0 |
| SAN | 318 | 168 | 486 | 0 |

The carry columns include eight earlier matching-d03 mutation receipts per mode;
they are not all historical carries. Thus current-binary semantic observations are
119 ON / 326 SAN, not fresh launches. Older source-equivalent new-fault evidence is
367 ON / 160 SAN; SAN freshly executed the remaining 207 V faults, V64–135 and
V137–271. All 367 new fault IDs have evidence in both modes, and legacy IDs 3/4/5
(physical 1/2/3, attempts 1/3/3) were freshly run in both modes. The ON final batch
ran no new native U/V fault sweep. Compatibility is exactly 23 f1a (including the
three legacy faults), 22 f1b, and 29 f1c. Per-key evidence preserves original pins
and timings; carry timing is never presented as current-binary timing.

U=98 and V=272 are independently captured allocation inventories;
`M=(98-3)+272=367`, comprising the U tail 95 at physical 4–98 plus V physical 1–272.
Inventory digests are distinct from module hashes; module sizes are 9,433 and 63,357 bytes.
Full records, module bytes, all 15 Usage fields, and witnesses were compared. These
are measured inventory identities, not semantic or resource-exactness goldens. The
old execution-authority deviation (19 ON /55 SAN slots) remains explicit and was
not retrospectively authorized; historical observations are credited only under
the coordinator's source-equivalence disposition and reviewed per-key ledger.

Runtime metadata certification passed pre/post in both modes: lists 23/351/457/412,
the old three lists byte-identical, all 50 shard names unchanged, and all six outputs
byte-identical across pre/post and ON/SAN captures. The 4,999-row manifest has 76
unique controls, 117 control rows, and 4,882 distinct faults; removing only the two
new controls and 367 f1d faults reproduces the 4,630 old ordered tuples exactly,
including order, multiplicity, and ownership. This is not execution of all 4,882
faults or the full default suite. The exact 74-key compatibility checks compared full
Wasm bytes, all 15 Usage fields, typed catalog identity, and full typed identity
records against eligible `df97d73` references: direct result 7/4,001 bytes, recursive
result 42/8,944 bytes, and plain module 3,707 bytes. These are compatibility
comparisons, not new compiler-output golden files.

The 44 frozen negative-argv cases (22 per mode) returned status 2 with empty stdout
and no semantic progress; only the unconditional wrapper startup pair occurred.
The original ON missing-value runner-classification FAIL and raw receipt remain
unchanged; read-only reconciliation classifies its actual behavior as
`PASS_EXPECTED_NONZERO`, without rerun or receipt replacement. The original SUP
pre-summary FAIL remains distinct. Both assigned SUP slots were used: 8.558700s
pre/reconciliation and 1.957007s post; the disclosed 138.732016s coordinator idle
was excluded from charged runner time, and the slot was not renewed.

Fresh-case maxima were ON active2.562463s /complete2.577694s and SAN active
11.601809s /complete11.646119s, within unchanged active<=3s/full<5s and
active<=27s/full<30s limits. Receipt closure remains raw stream/result, primary
receipt, then post-close witness before the complete-time sample. SAN used
`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` and `UBSAN_OPTIONS=halt_on_error=1`;
there is no leak, full-suite, host/platform, or whole-suite speed claim. Preserve
historical V64 INTERRUPTED, earlier failures/unknowns, original pins and times; the
fresh authorized V64 pass discharges its obligation without relabeling that history.

The corrected core closes CR1–CR3 and CCR1–CCR2; allocation AR1/AR2 and nonfault
NF1/NF2/NF1-R1/NF2-R1 are also approved. `CHECK_REFINED` consumes pending
representation ownership while physical scalar backing remains available to SELF;
success alone clears backing initialization and publishes the nominal result.
False/pending failure preserves backing without an invented scope-exit drop;
zero-action route markers and source/P2/P3 joins remain authenticated. The D/T/Q/R
22-anchor inverse line bridge remains strict except for cross-version line
coordinates; same-pin record and raw comparisons retain actual lines. The NF2
hostile-owner cases retain the selected-scalar mutation, expected-delta fences,
seal-failure semantics, byte-safe original-token restoration, and sticky blocking
described in the handoff. Existing OFF reuse is compile/build-graph-only; no OFF
runtime result is claimed.

Final Sol review and the documentation audit are approved; Astra aligned, the three
current-binary coordinator smokes passed (0.243s, 0.452s, 1.863s) within strict caps,
and protected original/resource WIPs were verified unchanged. Coordinator integration
and publication checks are closed. This status-only refresh adds no evidence or
timing. Broad P4.4 remains unchecked; R-AUTH and held `build_work` /
`build_scratch_bytes` ±1 checks remain open; speed goals remain unmet. P4.5,
host/E6, and P5 remain deferred. At the f1d publication snapshot, the sole next
cursor was P4.4f1e bounded Sol 6.1 High planning to analyze remaining predicate-call
contexts/signatures and choose a technical priority. That planning cursor
has since advanced under a separately approved brief; see the current f1e addendum
below. This historical paragraph does not describe current f1e execution status.
See the [P4.4f1d handoff](p4.4f1d-handoff.md) and
[TODO.md](../TODO.md#execution-cursor).

**Historical status snapshot, October 21, 2026 (P4.4f1e; bounded CORE and S1
technical/candidate source+evidence accepted; S1 commit/push then active):** The design, retained-entry
amendment, CORE source and bounded evidence have independent approval. Frozen CORE
source pins
are test `695e01d09b15d511ce99d7c51bc72c4067306d40809b05cb6e84628686cebd9d`,
production `49a241484fcc5a31de2b7512704c803d44c5c86add69a84df5b049d3b02cc988`,
runtime-conventions header
`f946b024ce91ba5844b7a69a7de93fe50b032d570540e62e4305f5640b684935`, and F1E
fixture `cdaef751298f9cda6147868952d7e00d1cb061a6879064c5aaa921c290f47db9`.
The unchanged F1D fixture is
`24a3e83c5496082bc7cf9759befa837ff18eb42bb82ec1e17fc16bdc19416cd5`. The accepted
CORE ON/SAN binaries were `82923e3fb25c02d77e57f4653eb6f1e812383057ef83b2768fffda7fa713c61e`
and `ded8a8dc5769ca1a9c872ebaf364d3f194adbfb429b7e4e94a7abe626dbafb1d`; they are
historical identities for the approved CORE checkpoint, **not** a current allocator
test hash. The f1d feature remains published at `42fd4cd`; at this earlier
prepublication snapshot, full f1e remained incomplete and unpublished. S1 has since
been published; the external sole cursor and later accepted-evidence record
supersede this historical status.

The admitted slice is one direct pure plain `ALWAYS` ensures `FUNCTION` call, with
one owned plain `Int64` input from the enclosing complete `result` or `old` of the
whole owned plain `Int64` parameter, returning `Bool` in a plain `Int64` result
function. Existing requires/refinement-call admission and pure ordinary scalar
callee/SCC behavior remain unchanged. Wrappers/computed or direct-parameter ensures,
qualified ensures, Bool/Unit widening, refined/Text targets, nested predicate calls,
mixed cycles, PanicText and richer interiors remain excluded. The narrow route-marker
exception is probe-ON only for the new ensures route, including zero-action failure;
old profiles and probe-OFF emit no marker/NOP. The retained positive pairs the real
unchanged F1D `launch` ENTRY with private `ensures_root` INTERNAL_FIXTURE. Only the
raw alias runs twice for value 4; only `TRACE_COUNT` is reset after authenticating
the first trace. The ENTRY/refinement companion is static census, not executed.

The separate CORE evidence review accepted both frozen target builds and all nine
selectors in each mode, closing E1 for these **18 fresh runs only**. Maximum ON
active/outer was 1.211297s /1.252686s; maximum SAN active/outer was 4.968905s /
5.044370s. The distinct outer clock sample followed raw/Result closure, primary
receipt fsync/close and post-close witness fsync/close. Active limits were 3s/27s;
complete limits were <5s/<30s. SAN used ASan/UBSan halt-on-error with leak detection
disabled; no leak or full-suite claim follows. The four older semantic PASS results
remain at their original pins with full durable-close timing **UNKNOWN**: their
5s/30s active limits and duplicate primary/witness elapsed values are not repaired.
Prior failures likewise remain at original pins and are not retroactively passes.
The per-key timings and full closure evidence are in the
[interim handoff](p4.4f1e-handoff.md).

The separate S1 native-performance review accepted one strict native ON target build
and one exact traced-X discovery at source test `950d5b50abe9293f15660e29aa9f08e656fea1fa0dd8fc7adfab989290175e2b`, production
`49a241484fcc5a31de2b7512704c803d44c5c86add69a84df5b049d3b02cc988`, and fixture
`cdaef751298f9cda6147868952d7e00d1cb061a6879064c5aaa921c290f47db9`. The selected
X measured 2.839310250s active, 2.859486750s driver durable-complete, and
2.882960292s distinct outer, within the unchanged 3s / <5s limits. Source checks
covered two independent owners, the 23-row anchor bridge through largest required
END 12485, full captured bytes and all 15 Usage fields, four actual seven-render
sets, fences/use gates, and both real closes. The 344 physical rows 1..344 are
observed S1 X evidence, **not frozen N_X**. The original slot and outer receipts
remain FAIL; the independent review accepts the 1-based ordinal and duplicate
observer-delivery erratum without changing those receipts or authorizing a retry.
The earlier four semantic PASS results retain their pins and historical durable-close
timing UNKNOWN; all other historical failures remain unchanged. No W, SAN, M, full-49,
or broad-matrix acceptance follows.

The separate extracted candidate at
`/Users/jsobotka/code/sol-lang-p44f1e-s1-publication` (`p44-f1e-s1-publication`) has
test SHA-256
`fbb0806bc0cd52cdb15f5ea0abf031478c2e4939b8cfa4457b821b9f526c6ceb`, production
`49a241484fcc5a31de2b7512704c803d44c5c86add69a84df5b049d3b02cc988`, and fixture
`cdaef751298f9cda6147868952d7e00d1cb061a6879064c5aaa921c290f47db9`. The independent
S1 publication review approves this exact candidate source plus bounded evidence.
One fresh configure/build passed; two fresh native semantic cases passed:

| Candidate selector | Active | Complete | Distinct outer |
| --- | ---: | ---: | ---: |
| `represented.ensures-calls.discovery.traced` | 2.670197250s | 2.714858500s | 2.733556625s |
| `represented.ensures-calls.retained.traced` | 1.061952667s | 1.076693625s | 1.095858458s |

Candidate executable/object SHA-256 are `e2734001faf16002ddbc59f79b40fd2ecfbd318bbecafd5f35d867dc11d551e8` /
`3726447a8ab7fd90eff8211416649899b615807aa1bd78af57b0267331f542d1`. Thirteen metadata
invocations passed: manifest, six list outputs (shards, old A–D, new E), and six
expected CLI negatives. The correction chronology is configure/build, a no-child
pre-semantic bytecode correction (no rebuild), then the two semantic and thirteen
metadata invocations. This is the evidence-review interpretation; immutable raw
validation reports and receipts are not rewritten. Historical CORE evidence remains
at its own pins with only review-approved bounded continuity; there is no fresh
candidate SAN or nine-case replay. Original 950 evidence is separately attributed.
At this prepublication snapshot, final documentation review, staged-tree/preservation
review, commit, and verified push remained pending, with S2 blocked until verified
push. S1 was subsequently published and S2 technical acceptance has since closed;
the later dated status addendum below supersedes status only. The original seven-path
WIP remains preserved and untouched.

The original finite allocator unit described 12 launches per mode (eight profile
controls spanning W, X, D, T, Q, R, U and V, plus four fault endpoints), ON2/SAN3/SUP0
120-second windows, and at most two target builds per mode. That unit is historical
accounting, not current live allowance.

The historical corrected-source discovery observations are from test pin
`46e16a9d3c249331438880496ced647591d332ecc5916c259f29dbb9aac192f9`: ON plain W
passed with the historical ON binary `f8ee4a1302c2b75e21a66e3f6264ac2fc20458d9f6d23f98776b3273a6b8fb17`
(active 2.405744s / outer 2.468456s; two owners, 257 rows, both closes). 257 remains
provisional, not accepted `N_W`. ON traced X failed during test-side source
preparation before interpreter/backend/capture; the sequence stopped after two of
four planned selectors with no retry, and SAN was unlaunched. These are closed
historical dispositions for that discovery only, not outcomes of the current S1
source or a carry into it.
At this pre-freeze snapshot, `N_W`, `N_X`, and `M=N_W+N_X` plus their digests were
unknown; no old or current W carry was claimed. The later S2 pin freeze uses its own
fresh source-pinned captures and does not adopt this old W observation.

The historical ON traced-X result at test `6e10fdfe…` is UNKNOWN cutoff: active
3.004135583s against 3s and outer 3.079325s, not PASS. No unchanged-source retry or
deadline increase is claimed. The scanner change was source-approved at test snapshot
`950d5b50abe9293f15660e29aa9f08e656fea1fa0dd8fc7adfab989290175e2b`; the later
independent evidence review accepted the one ON build / one X selector launch (which
contains two prepared owners and within-mode equality) only at those source,
production, fixture, and binary pins. This acceptance does not transfer to the distinct
publication candidate described above.

The controller `AttributeError` before a frozen plan/build/selector is a preserved
historical setup interruption: **zero launches, no owned jobs, no runtime result** for
that attempt. It was superseded by the accepted original one-build/one-X S1 evidence
above; the original failure/slot receipts were not rewritten. Candidate configure,
build, two semantic cases, and metadata evidence are separately accepted as described
above; no candidate validation is running now. Prior
historical W at `46e16a9d…` / binary `f8ee4a13…` passed at active 2.405744s / outer
2.468456s with two owners and 257 provisional rows; this is not accepted `N_W` and
is not carried to current W. At that historical pre-freeze snapshot, `N_W`, `N_X`,
and `M` were unknown; stored zero pins were placeholders, not measured zero. The later
S2 review freezes the current pins separately.

| State | Step | Boundary |
| --- | --- | --- |
| COMPLETED | Bounded CORE | Test `695e01…`; 9 ON + 9 SAN accepted, E1 closed for those runs only. Reopen only for an actually affected dependency. |
| COMPLETED | S1 — published native ensures-call discovery checkpoint | Commit `e93d375e2e9f59d9a819ad470770e741a1511f4a`; `origin/master` verified at the same commit. This does not close full f1e. |
| **ACTIVE at this earlier snapshot** | **S2 — initial W/X captures, cross-mode comparison, and pin freeze** | Before the authorized captures and final review, source review had approved the W/plain expression and S1 native-X carry; `N_W`, `N_X`, `M`, and digests were still unknown. The final S2 acceptance and current publication-only cursor are recorded in the later dated addendum below. |
| BLOCKED on S2 | S3 — helpers and every new fault ordinal | Review complete helpers first; then accept every ordinal in finite ON/SAN batches. |
| BLOCKED on S3 | S4 — behavior/compatibility groups | Future 4a–4d small groups; no group is active or parallel. |
| BLOCKED on S1–S4 | S5 — integration/publication | Final evidence/diff review and affected checks; earlier accepted, independently publishable milestones need not wait. |

Technical acceptance and publishability are distinct. In this earlier pre-acceptance
snapshot, S1 was published and S2 evidence/pin review remained open. Do not stage the
original mixed WIP wholesale. At the S1
publication snapshot the candidate listed nine real E keys and added only
`contracts.ensures-call-core-s1`; S2 source now adds one W/plain selector, for ten
current E keys (eight CORE plus W and X). No new fault/default/sweep registration is
added. Static inventory remains 77 unique controls /118 expanded control tuples /4,882
old fault IDs /29 sweeps /24 runners /50 shards /5,000 total ordered tuples; the full
manifest and old A–D lists remain source-identical to S1. This static inventory is not
new CLI output or runtime evidence. At this pre-freeze snapshot, S1 X=344 was observed
but not yet frozen as `N_X`; W had no current-source runtime observation and `N_W`,
`N_X`, `M`, and digests were unknown placeholders. The later S2 review freezes the
values recorded below without changing that historical attribution. The original
78-control /`(4,882+M)` plan is historical monolith accounting; reconcile future fault
registration against the published S1 inventory and do not infer 79 or guess pins.
R-AUTH/held resealed ±1 checks remain OPEN;
whole-suite speed UNMET; broad P4/P4.4 OPEN; host/E6/P4.5/P5 deferred. These are not
added S1 gates. See `TODO.md` for external-cursor navigation and the [interim handoff](p4.4f1e-handoff.md)
for step exits, historical evidence and publication conditions.

Earlier source reviews remain snapshot-specific: `7c1fb217…` had R1–R5 request
changes, `de3c6fb1…` was intermediate F1–F3 request changes, and `2fb9ae2c…` was a
separate source approval. The currently scoped S1 source approval is `950d5b50…`;
none of these approvals is runtime evidence. Prior allocator helper/accounting reviews
(`allocation-source-review.md:242–280`, `allocation-followup-review.md:242–280`,
`allocation-size-review.md:228–327`) do not establish six-profile runtime
compatibility. No full incremental diff from `46e16a9d…` to later cleanup snapshots
is claimed.

W is genuine ENTRY `launch` only with three probes OFF; X is ENTRY `launch` plus
`old_callee_arithmetic` INTERNAL_FIXTURE with three probes ON. Each selected-case launch
prepares two independent owners/captures and requires full ordered-record and complete
module-byte equality, all 15 `Usage` fields, complete witnesses, and both closes. This
is a within-mode assertion, not a requirement for two separate launches or duplicate
dumped inventories. `M=N_W+N_X`, with no legacy-tail subtraction; old `ensures_old`
IDs 6/7 remain unsupported.

The six-profile runtime bridge and controls remain unaccepted: D96/T203/Q129/R297
line-only inverse-map to accepted f1c anchors; U98/V272 map to accepted f1d anchors.
All non-line fields/order/dimensions/bytes/`Usage` remain strict, and old-profile
marker/probe-OFF invariants still require runtime evidence. The accepted f1d inventory
remains 76 controls /4,882 fault IDs /50 shards and lists 23/351/457/412. Its
486-per-mode count is mixed history: ON 111 fresh /375 carry, SAN 318 fresh /168
carry; each carry includes eight matching-pin mutation receipts, and ON119/SAN326
are not fresh f1e runs. The accepted S1 candidate adds one control to that historical
baseline: 77 unique controls, 118 expanded control tuples, 4,882 old fault IDs and
5,000 total ordered tuples. Future M/fault additions and the 49 nonfault obligations
remain unknown or pending, not registered as false tests or claimed complete. Existing
OFF evidence is compile/build-graph-only, not a fresh OFF runtime result.

The earlier four-path DRAFT review approved the prior draft with one nonblocking
stale cursor phrase at `TODO.md:439`; the coordinator corrected it, and this refresh
preserves that correction. At this prepublication snapshot, separate final
documentation review remained. R-AUTH/held resource checks remain open, speed goals unmet,
broad P4.4 unchecked, and P4.5/host/E6/P5 deferred. `TODO.md` recorded the cursor at
that historical snapshot. See the [f1e interim handoff](p4.4f1e-handoff.md) and
[TODO.md](../TODO.md#execution-cursor).

## Historical status addendum — October 21, 2026 (S3 recovery R2 then active)

The R2 preparation, absent-capture and zero-qualifier statements in this section
are superseded for status by the October 23 accepted-evidence addendum below.

CORE, S1, and S2 are complete at their own pins; S1 and S2 are published as
`e93d375e2e9f59d9a819ad470770e741a1511f4a` and
`b1f5e540f9b76ee2cc5d11589de09c6c319612c5`. S3 is IN PROGRESS as a container, with
**R2 the sole ACTIVE leaf** in the [TODO cursor](../TODO.md#execution-cursor). R1
repository persistence/review is COMPLETED: the selected decision and five-step plan
were independently approved, and Astra's exit steering is READY_R2.
The bounded diagnosis is complete: retained evidence does not contain the original
complete environment values, and the later drift cause remains UNKNOWN. The user
authorized preparation of a new actual captured baseline; no original values are
guessed or hash-reconstructed.

Four historical C pilots remain accepted under their old context but do not qualify
the new baseline. No new-baseline qualifier has run; all 1,198 remaining cases are
unconsumed. The original preflight/census halt, C continuation halt, D
failed-environment draft, and all historical receipts remain unchanged. The actual
private snapshot and manifest do not yet exist; all environment hashes remain null.
No gate, freeze, coordinator execution authority, configure/build, runtime, or S3
publication has occurred or is granted by this status update.

R2's external B source/controller/freezer/capture preparation is
`APPROVED_PENDING_ACTUAL_CAPTURE`, pending the actual private capture. After capture,
independent final controller/pin-completion review remains required. This does not mean
R2 is complete or grant execution authority.
Source `tests/wasm_represented_tests.c` at
`dd700c09b8fe4da69967e1147c2078cf71c5f8d3ae0029256306335289e634bd` is already
separately approved. The pinned utility
`83d219c8f56c7cde092bf486aa57e763c680d3703ba91cccff3ed3f52ff915c4` is prepared to
capture the actual full inherited map once,
privately and durably, derives only the existing mode controls and monitor transform,
and binds full expected and actual-Popen maps. It reuses the already built ON
`19e1c16b…` / object `6abcf5b…` and SAN `fc50ae77…` / object `60e3200…` outputs with
zero configure/builds. Actual capture is pending; after capture, final controller/pin-
completion review, gate, coordinator authority, and freeze remain pending. Only after
those separate gates, R2 permits exactly four
new-context pilot cases in two windows, then pauses for independent evidence review.
R3 is blocked on accepted R2 and a separate continuation authority;
it covers the same 1,198 cases in 232 windows and requires exactly 1,202 unique new-
baseline receipts (601/mode) before independent S3 acceptance/publication. R4=S4
and R5=S5 remain blocked and sequential. The full five-step scope, caps, stop rules,
and publication boundaries are in the handoff linked above.

The R1 change was documentation-only. Its independent review confirmed the exact
896-entry C baseline, four changed documents, 892 unchanged paths/modes/bytes, and all
11 explicit source pins. The current R2 cursor update is also documentation-only; B's
refreshed tree/document/source-pin proof records that delta without changing source,
configuration, or reused outputs. The `8204c77e…` value is a historical tree-hash
prefix, not an entry count. The refreshed document/tree pins still require scoped
independent final-pin review before freeze; historical whole-tree hashes remain
historical evidence.

## Historical status addendum — October 21, 2026 (S3 helper source review before recovery)

S1 and S2 are complete and published. S1 is commit
`e93d375e2e9f59d9a819ad470770e741a1511f4a`; S2 is commit
`b1f5e540f9b76ee2cc5d11589de09c6c319612c5`, with push and remote verification
complete. Their accepted evidence and source pins remain attributed to their own
checkpoints. **S3 helper source review is the sole ACTIVE item.** The bounded source
implementation is proposed and awaits independent review; source acceptance has not
been granted. Runtime is UNLAUNCHED: no S3 build, selector, CLI, or fault child ran,
and zero of the planned 1,202 receipts are claimed. Proposed registrations are not
accepted coverage. S4 remained blocked on S3; S5 remained blocked on S1–S4.
`TODO.md` recorded the cursor at that historical snapshot.

The proposed S3 source delta is limited to `tests/wasm_represented_tests.c` and the
four cursor/status documents. It restores the six approved helpers, adds the single
fault-private checker, shares the two current positive inventory digest constants,
and appends the two W/X physical-fault sweeps and their contracts control. The
original mixed 950 source remains read-only and untouched; production, fixture,
headers, IR, P3, CMake, and support code are outside scope. The explicit final-render
call is removed only from the fault helper call site; the unchanged unconditional
owner close retains final renders and safe destruction. No shared owner, render,
discovery, or classifier implementation is changed.

If accepted, the source expansion is proposed as 611 E keys (10 existing plus 601
fault keys), 78 unique controls /119 expanded control tuples, 5,483 fault tuples,
5,602 ordered tuples, 24 runners, 50 shards, and 31 sweep IDs. These are static
proposal counts, not fresh CLI/list/manifest output, not a current accepted
registration, and not runtime evidence. The S2 inventory remains W=257, X=344,
M=601 arithmetic, with its frozen actual-line digests; the two digests were hoisted
mechanically to shared private constants and must receive explicit carry-equivalence
review. No new count, digest, or runtime result is inferred from that source edit.

The required source review must inspect the complete per-ordinal chain: a fresh live
owner and source-classified pinned baseline; registered ordinal `j` mapped identically
to physical ordinal `j`; one `represented_fault(sweep,j,j)` arm; immediate byte-first
owner fence after each backend return; full `g(j)` failed prefix including siblings,
exact diagnostic, empty output and zero 15-field `Usage`; same-owner unarmed retry with
full record/byte/Usage equality and authenticated witness; final validator fences; and
an unconditional real close before any `P44F1E_FAULT_COMPLETE` success record. The
review must also check all private checker vectors, all 601 source-producer/classifier
joins, all 23 fresh per-owner source anchors ending at required END 12485 (not EOF),
the static old-list/manifest filter, and source-expansion/window-plan/parser negatives.
Until that review approves the exact source, no build or runtime phase is authorized.

The original 950 FAIL/UNKNOWN receipts, S1/S2 accepted pins and evidence, publication
history, citation errata, leak-disabled SAN caveat, R-AUTH and held resource ±1 debt,
and P4.5/host/E6/P5 deferrals remain unchanged. Full f1e, S3 acceptance/publication,
S4/S5, broad P4/P4.4, and whole-suite speed acceptance remain open or blocked as
previously recorded.

<a id="s4c-accepted-evidence"></a>

## S4c accepted evidence — October 24, 2026; publication PENDING

The [S4b publication receipt](../../sol-lang/.git/agent-handoffs/p44-f1e/s4-behavior/s4b/publication-receipt.md)
verifies `dacf0a21c63263c93123e0e278d4b8188342be5f`. The appended
[S4c final review](../../sol-lang/.git/agent-handoffs/p44-f1e/s4-behavior/s4c/evidence-review.md#final-independent-s4c-c1c4-acceptance)
accepts **six resources,98 metadata children and two builds:106 actual project
children**. The **49 nonfault inventory=discovery2+S4a26+S4b18+S4c3** is supported
in both modes by **92 qualified original-context carries +six fresh references**.
Live E650, unchanged A/B/C/D lists, manifest/shards and original4999 ordered tuples
are reconciled; same-expression default mapping is source-proved, not fresh49/
default backend/full-suite execution. Metadata negatives retain original exit2/
supervisor FAIL with separate expected-nonzero acceptance, not rewritten receipts.

Three real X resource controls per mode retain defaults→exact observed selected
Usage→one-below→restored-default retry with actual limits. All15 Usage fields,
full bytes/catalog/provenance/runtime/trace witnesses, independent module-local and
typed source-edge arity joins, immediate false-return fences, final validators/
renders/owner close are accepted. No resource-authentication or historical-golden
claim follows. Current test/build/runtime pins and failure/carry history are linked
in the [handoff](p4.4f1e-handoff.md#s4c-accepted-evidence). S4b's accepted SHA/fence/
deadline correction did not raise caps; old contexts are not current-binary replays.

S4c publication PENDING docs/exact five-path staged review, Astra readiness and
coordinator commit/push/remote verification. No source repin/rebuild/extra runtime
for this carry. S4d compatibility is next; full S4/S5 remain unclosed. R-AUTH/held
resealed ±1 OPEN, leaks disabled (not leak-freedom), speed UNMET; no broad P4/P4.4/
full-f1e, full-suite/platform/default-run acceptance; host/E6/P4.5/P5 deferred.

<a id="s4b-accepted-evidence"></a>

## Historical S4b accepted evidence — October 24, 2026; pre-push checkpoint

S4b publication-PENDING and S4c-next statements below are pre-push history,
superseded for status by the S4b receipt/current S4c record above.

The [S4a publication receipt](../../sol-lang/.git/agent-handoffs/p44-f1e/s4-behavior/s4a/publication-receipt.md)
verifies `acf3de0d1c23b377605fd682f207c28bc68002b5`. The appended
[S4b final review](../../sol-lang/.git/agent-handoffs/p44-f1e/s4-behavior/s4b/deadline-correction/tail-binding-evidence-review.md#final-s4b-b1b6-actual-evidence-acceptance)
accepts **36 identities (18/mode): nine original-context ON case carries +27 fresh
(nine ON +18 SAN)** and **14 original metadata invocations**. E647=46 nonfault+601
faults is accepted stage metadata, not final S4c metadata/default execution.
Eight rejections, two canonical controls and eight typed hostile mutations per
mode retain full source/bytes/Usage15/runtime/reset/owner assertions.

All fresh deadlines pass unchanged: worst ON relocation active2.853264s /
post-close3.492279s /distinct outer4.720051s; SAN10.847718s /11.614223s /
13.013285s. Caps remain3/<5/<5 and27/<30/<30, parent<120/reserve10. The reviewed
SHA core/padding/hex/fence and fresh-owned relocation correction preserves failure
states, all allocation/fault ordering, seven renders and security, not sampling,
validation caching or a deadline increase. Prior pure52/19-group checks remain
source qualification, not a runtime replacement; none is rerun for this doc carry.

All eight changed source paths and actual build/runtime pins are accepted in the
final review; [the handoff](p4.4f1e-handoff.md#s4b-accepted-evidence) links them and
the explicit production-helper equivalence/carry. S3/S4a results stay at original
source/build contexts, not fresh faults or S4a executions. Original failures,
partial regressions, missing historical windows and UNKNOWN outer outcomes remain
unchanged by pointer. S4b publication remains PENDING docs/exact 12-path staged
review, Astra readiness and coordinator commit/push/remote verification. No source
repin/rebuild/runtime. S4c3/resources+final metadata/default checks are next after
S4b publication; S4d compatibility pending, S5 not started. No full-suite/default/
platform/leak/speed/broad P4/P4.4/full-f1e acceptance; leaks disabled, R-AUTH/held ±1
OPEN, speed UNMET, host/E6/P4.5/P5 deferred.

## Historical S4a accepted evidence — October 24, 2026; pre-push checkpoint

The publication-PENDING and S4b-next statements below are the S4a pre-push state;
the S4a receipt and current S4b checkpoint above supersede status only.

The [S3 publication receipt](../../sol-lang/.git/agent-handoffs/p44-f1e/s3-faults/s3-publication-receipt.md)
records successful push and remote-master verification of
`eb1aeb276977ab940020307b52914a2f9a8948db`. The
[S4a actual-evidence review](../../sol-lang/.git/agent-handoffs/p44-f1e/s4-behavior/s4a/old-clause-correction/evidence-review.md)
accepts **26 keys per mode =52 identities: 12 original-context ON individual
case carries +40 fresh (14 ON +26 SAN)**, with four separate current W/X discovery
witnesses. The historical S4a test SHA256 is
`355f3ed5759f4cbfd73c1979bcd1a2208f32d7cb8dbc2f70c67d7afda5ed5fa8`;
source/build/evidence pins remain in that review, not migrated from S3's `dd700…`.
S4a publication remains PENDING docs/exact staged-tree review, Astra readiness and
coordinator commit/push/remote verification; this is no staged-tree approval.

Source/P2-derived n20/m39/S147 and all nine limited modules, eight root pairs,
genuine ENTRY resets and unchanged retained raw-repeat behavior are accepted.
Full packet/ordered ledger/owner checks are source-backed success gates joined to
actual raw protocols/closed receipts, not retained full module/owner dumps.
The [handoff](p4.4f1e-handoff.md#s4a-accepted-evidence) links the source-bounds
reviews and preserved source1 build failure, source2 failed13/missing window close,
and original derive child0/adapter1 reconsumption history.

E633=32 nonfault+601 faults is **static inventory**, not actual current metadata
CLI/default runtime or all633 execution. Source-specific S2/S3 carry relies on
false-default branch isolation and four fresh discovery witnesses, not new S3 fault
executions or historical repinning: 1,202 historical identities /525 FINALs /three
honest no-FINAL leaves remain qualified. The
[sole S4 index](../../sol-lang/.git/agent-handoffs/p44-f1e/s4-behavior/evidence-index.json)
is navigation only. S4b18 is next after S4a publication; S4c3/resources+metadata,
S4d compatibility and S5 remain pending. No source repin/rebuild/runtime is performed
for this docs carry. R-AUTH/held resealed ±1 OPEN; speed UNMET; broad P4/P4.4/full-f1e,
full-suite/platform/leak acceptance unclaimed; host/E6/P4.5/P5 deferred.

## Historical S3 technical evidence — October 23, 2026; pre-push checkpoint

Publication-PENDING and blocked-on-S3 statements here are explicitly the October 23
pre-push state, superseded by the October 24 receipt/status above.

The [independent final1202 review](../../sol-lang/.git/agent-handoffs/p44-f1e/s3-faults/s3-final1202-review.md)
records `APPROVED_FINAL_1202_IDENTITY_COVERAGE_AND_INTEGRITY` and
`DOCS_ONLY_PUBLICATION_CARRY_ALLOWED`, accepting C3–C8. The exact union is
**1,058 original-context references +144 new SAN-X200..342,344 cases =1,202
physical identities, 601/mode**, ON/SAN × (W1..257, X1..344). It contains **525 real
FINALs (381 prior +144 new), not 1,202 FINALs**. SAN-W-0167 (N3), SAN-W-0229 (N4)
and SAN-X-0199 (original active run) remain three honest, distinct no-FINAL
`CASE_LEVEL_REFERENCE_CARRY` leaves. Their missing-fact lists, failed/halted N3,
UNKNOWN historical exits/causes/signals and T attempt41 `UNKNOWN_NO_CHILD` remain
preserved; new acceptance does not backfill original timing/frontier/FINAL facts.

The [sole index](../../sol-lang/.git/agent-handoffs/p44-f1e/s3-faults/s3-evidence-index.json)
links Q's successful final-integrity receipt and five-member waited capture, with
durable controller0/wrapper0 and normal coordinator-preserved outer completion.
The accepted execution uses test source
`dd700c09b8fe4da69967e1147c2078cf71c5f8d3ae0029256306335289e634bd`, unchanged
compiler/fixtures and reused ON/SAN outputs, with the actual protected recovery
environment, not S2's or guessed old maps. Complete attribution and missing-fact
pointers are in the [handoff](p4.4f1e-handoff.md#s3-accepted-evidence).

Failed-prefix/sibling/diagnostic/empty-output/Usage15, immediate unarmed retry/full
records/module `memcmp`/Usage, witnesses, validators, seven-renderer fences/sticky
preservation and real close are **source-backed success-gating assertions joined
to actual raw protocols/receipts**, not retained full owner/module/record blobs or
a syscall trace. S2 cross-mode module identity remains length+SHA256 only.
Static source registrations are not fresh CLI/default/metadata certification;
none was rerun for this docs carry. ON active≤3/full+distinct outer<5, SAN
active≤27/full+distinct outer<30, parent<120/reserve10 remain unchanged, including
historical timing qualifications. SAN retains
`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`, `UBSAN_OPTIONS=halt_on_error=1`:
leak detection is disabled, with no leak-freedom claim.

S3 technical acceptance is complete; publication is **PENDING** independent exact
staged-tree review and coordinator commit/push/remote verification. S4/S5 remain
blocked until verified publication and separately assigned work. The
[old S3 cursor](../../sol-lang/.git/agent-handoffs/p44-f1e/s3-faults/execution-cursor.md)
is now a completed checkpoint, not live authority. No full-f1e, broad P4/P4.4, platform/full-suite or speed
acceptance follows; R-AUTH and held resealed ±1 debt remain open.

Use the [README](../README.md) for onboarding and runnable examples, and
[project analysis](project-analysis.md) for the dated product assessment. The
design specification is a target-language manual with implementation boundaries;
the current audit is a dated document-based assessment. Both September 9, 2026
revisions reference `6ec4ba9` through P2.6. Their
[PDFs and editable sources](../README.md#documentation) are not evidence that
every target-language form is executable or that a fresh full suite was rerun.

## Scope and API Boundaries

The implemented CLI provides `check`, `test`, `run`, `effects`, `inspect`, and
`fmt`. Reference execution uses owning typed IR, not the separate experimental
MIR/P2/P3 owners. There is no complete backend, physical ABI,
`sol build`, full public IR, or SMT discharge. P3.1 supplies target-independent
call/result/failure conventions, P3.2a adds a recipe operation-demand inventory, P3.2b adds independently validated allocation plans and pure quota checking, P3.2c adds static ownership plans, P3.2d adds static copy plans, and P3.2e adds static equality plans with allocation-free bounded test modeling, P3.2f adds static E3 host-result transfer plans, P3.3 adds static cleanup/panic/failure policy, P3.4 adds static capability/trusted-host ABI policy, P3.5 adds static exact-handler ABI policy, and P3.6 joins the complete authenticated runtime-lowered graph: there is no executor, physical ABI, allocation implementation, production host adapter, Sol execution, CLI/build artifact, or tokens. P4.1 adds the opt-in Binaryen 129/Wasmtime 49.0.1 core-Wasm probe. P4.2 then emits and validates authenticated P3.6 whole-scalar direct-call closures using a private provisional scalar convention, not a stable/public physical ABI or adapter. P4.3 completes the bounded represented-value/place/indirect-call checkpoint with `sol.p43.provenance.v1`, but remains private provisional Wasm32 lowering, not full E6 Wasm execution, an adapter, production optimizer policy, or a stable public ABI. Validation was OFF Debug Werror 54/54, ON Debug Werror 58/58, with the current ON ASan/UBSan Werror represented-emitter and focused MIR tests passing; no current full ASan suite or Linux run is claimed. Wasm is the first production target; native output is deferred and Component Model integration is not implemented. See [Wasm toolchain](wasm-toolchain.md).

Compilation sessions use configurable deterministic ceilings for per-file and package source bytes, source files, directory depth and entries, tokens, persistent arena entries, diagnostics, cumulative allocation bytes, and allocation requests. Disk packages are discovered relative to verified open directory descriptors; source reads reject symbolic links, non-regular or duplicate identities, growth, truncation, replacement, and metadata changes. Raw interpreter host failures are copied through interpreter-owned length-delimited storage rather than borrowed C strings.

The bootstrap exposes a shared compilation session for file, directory,
NUL-terminated text, and length-delimited in-memory source compilation. Frontend
package and semantic arenas remain private: callers receive copied scalar
summaries and diagnostic records, borrowed strings whose lifetime ends at IR
transfer or session destruction, and session-owned diagnostic/inspection render
operations. Successful compilation can transfer one opaque validated-IR handle
that remains self-contained after session destruction. Narrow read-only
definition/path projections, effects rendering, and interpretation never expose
its internal `SolIr`. Validated-handle interpretation rejects raw-IR host callbacks
before execution; hosted applications use exact opaque root/member grants and
immutable data-only callbacks.

The normal compilation pipeline transfers owning IR and frees frontend data before
reference execution. The interpreter executes the bounded core, including checked
mutation, explicit trait evidence, root-preserving capabilities, exact deep
handlers, callable contracts, and refined constructors under deterministic limits.
`sol test` runs authored Boolean unit tests; `sol run` executes one explicit
application entrypoint with bounded console, arguments, deterministic configuration,
and checked contracts. This is not a production runtime: there is no VM, bytecode,
code generation, or `sol build`. Raw IR/interpreter APIs and raw host callbacks
remain unstable compiler internals for phase and malformed-input tests, not
serialized formats or stable public ABIs.

The normal consumer API is [compilation.h](../include/sol/compilation.h).
Raw phase APIs remain available for compiler development and malformed-input
tests, not as public serialized interfaces.

## Patterns and Ownership

The bootstrap provides `Int64`, `Bool`, `Text`, Unit, Never, records, enums,
structural tuples of arity 2 through 16, bounded generics/traits, and nominal
distinct/refined types. There is no implicit null: absence uses `Option<T>` and
recoverable failure uses `Result<T, E>`. Pattern matching is exhaustive.

The bootstrap supports recursive wildcard, Boolean, binding, positional enum-variant, nominal-record, and structural-tuple patterns. Record fields may be reordered or omitted, and pure `if` guards may use pattern bindings. A top-level bare payloadless variant keeps its existing spelling; inside another pattern a bare identifier binds, so a nested payloadless variant is written explicitly as `variant()`.

If a nested binding on an enum subject collides with a variant spelling, checking
reports `SOL-MATCH-001`; use `variant()` for a payloadless match or rename the
binding. Record children preserve resolved fields and tuple children exact ordinals.

### Places, Mutation, and Loans

Values are immutable by default. Sol uses affine ownership and lexical borrowing to provide memory and resource safety without requiring a garbage collector. The bootstrap supports definite initialization, local-rooted record-field and tuple-projection mutation and loans, path-sensitive affine moves, explicit owned mutation and lexical regions, and deterministic runtime-storage cleanup.

The bootstrap implements callable access modes over an underlying type: an unqualified parameter is owned, `borrow T` is shared, and `inout T` is exclusive. Borrowed call operands may be mutable/local-rooted record-field or tuple-projection places. Paths overlap when they share a root and either projection path prefixes the other; sibling fields are disjoint, and loans last through call completion. Source-defined functions, callbacks, recursion, and implementation methods copy `inout` values into the callee and write the complete value back on success. Bodyless host operations and top-level interpreter entries reject exclusive writeback. These qualifiers are preserved in exact callback and trait-method signatures but are not general types or values. `region name { ... }` is a Unit-valued statement: its label is retained for diagnostics and internal IR but is not a value and cannot be passed to an allocator. Regions nest, run ordinary type/effect checking, and are forbidden in expression positions, contracts, and refinement predicates.

`var name = initializer` infers one fixed slot type. `place = rhs` is a Unit-valued statement whose target may be a mutable local or its record-field or tuple-projection path. The RHS is checked with the leaf's exact contextual type and evaluated before replacement. Assignment can exactly reinitialize a moved projection or ancestor; a live old leaf is cleaned before transfer, while filling a moved hole performs no old-value cleanup. Moving an affine projection makes that path and its ancestors unavailable but leaves sibling projections usable. Projected assignment conservatively rejects capability-bearing, function/operation, `Self`, and unresolved-generic leaf types until field-sensitive authority provenance exists. Active overlapping loans reject replacement, and affine writeback cannot cross an explicit-region boundary.

`var name: Type` declares an uninitialized authority-free local. A whole-local assignment must initialize it on every reachable path before any read, borrow, projection, compound update, or `modify`; terminating branches do not constrain the continuing path, and leaving a slot uninitialized is allowed. `+=`, `-=`, `*=`, `/=`, and `%=` are checked `Int64` read-modify-write operations. They read the target once, evaluate the RHS, check arithmetic, meter the write path, and commit atomically. `modify local { ... }` grants lexical write and `inout` authority for one initialized immutable owned binding or owned parameter. It does not mutate binding metadata or escape the block, and authority-bearing whole replacement remains conservatively rejected.

### Loops and Termination

The bootstrap provides statement-only `loop { ... }` and `while condition { ... }` with payloadless `break` and `continue` targeting the nearest loop. Either form may carry one `invariant { ... }` clause followed by one `decreases { ... }` clause. Invariants are nonempty newline- or comma-separated exact `Bool` expressions; the decreases measure is one exact `Int64` expression. Both resolve in pre-body lexical scope, pass the contract purity firewall, and reject `result`, `old`, mutation, effect propagation, handlers, loops, and control transfer. A reachable backedge requires `decreases` unless the finalized callable row contains concrete unparameterized `diverge`; an open symbolic effect tail is not an exemption.

A finite ownership fixed point joins fallthrough and continue backedges: unavailable paths join by union and definite initialization by intersection. A `while` retains its zero-iteration condition-false exit; a `loop` falls through only through reachable breaks, whose states are joined for subsequent code. Loop-body locals unregister after every iteration, and block cleanup runs exactly once on fallthrough, continue, break, return, propagation, runtime error, and step-limit failure. Every invariant emits entry and preservation templates; a decreases measure emits nonnegative and strict-decrease templates. Owning IR retains their exact callable, loop, expression, type, span, cardinality, and lexical ownership, but the interpreter erases them completely. Labels, loop values, and proof discharge remain deferred.

Three statement-only termination forms build on `Never`. `panic message` requires one exact `Text` expression, evaluates it once, contributes the authority-free `panic` effect, and terminates with a structured interpreter diagnostic after exact lexical cleanup. `unreachable because { proof }` requires one pure exact `Bool` expression, emits a deterministic unresolved callable-owned obligation, contributes no runtime effect or step, and terminates statically; reaching it in the interpreter is a distinct defensive error. `require condition else { fallback }` evaluates one exact `Bool`; true continues as Unit without introducing a flow-sensitive refinement, while false executes an exact-`Never` fallback and propagates its return, panic, loop transfer, or divergence. Moves confined to the terminating fallback do not affect the continuing ownership state.

### Cleanup and Validation

Every executable block records its exact successfully introduced `let` or `var` cleanup order, and every match arm records its pattern-binding cleanup order. Live owned locals are released in reverse order at lexical exit on normal completion, return, propagation, and runtime error; moved slots are skipped, borrowed parameters do not own cleanup, and Copy values still release interpreter storage. Return/propagation preserves its outgoing value before cleanup. While an explicit region is active, the bootstrap conservatively rejects every affine explicit return or affine propagation residual with `SOL-REGION-001`; Copy returns are allowed. Calls may consume region locals because this subset has no global storage or closures.

This cleanup is effect-free compiler/runtime storage reclamation only. Apart from the explicitly unstable observation hook described below, it invokes no Sol code, host operation, destructor, finalizer, resource `close`, or allocator action and is not a backend lifetime implementation. Index/dereference mutation, field-by-field first initialization, authority-bearing uninitialized slots, escaping references, loop labels/values and obligation discharge, flow-sensitive authority provenance, lifetime generics, allocator/arena APIs, first-class region values, user `Drop`, fallible close protocols, unsafe, FFI, and backend behavior remain deferred.

Owning IR represents every executable local or resolved field/tuple access as one canonical typed place: a local or computed-value root followed by an exclusively owned root-to-leaf projection slice. Every root and projection retains its exact source span and instantiated result type; nested chains are flattened, and independent validation rejects malformed, shared, orphaned, ill-typed, or authority-unsafe updates. Explicit DECLARE, MODIFY, LOOP, WHILE, BREAK, CONTINUE, PANIC, UNREACHABLE, and REQUIRE statements preserve initialization, lexical authority, structured transfer, and contiguous proof-only obligation slices; assignment retains its exact operator. Ownership tracks definite initialization, unavailable path frontiers, prefix-overlap loans, and bounded loop fixed points. The interpreter represents unbound slots and moved aggregate fields without synthetic values, meters projected reads and updates, stages all `inout` results before writeback, and commits only after every destination path passes its preflight. Index and dereference projection kinds remain reserved and rejected. Tuple syntax/type projection remains in historical inspection v2 and current v3; owning IR remains excluded.

## Effects and Capabilities

Effects describe behavior; capabilities provide authority. The standard host
profile is limited to console output, argument access, and explicit configuration,
not a general filesystem/network/clock interface. Capability wrappers are nominal
and single-source: every operation has a body, the source remains private, and
`Self` names the original source root across the wrapper boundary. Effect rows
may be inferred locally, while public APIs expose canonical effect signatures.

Exact capability-backed `handle effect<authority> with provider { body }`
expressions replace one operation lexically while preserving the body's value and type.

The handled target must be one parameterized atom and identify exactly one
module-local capability member whose complete row contains only that effect
parameterized by `Self` (for example, `{ clock.read<Self> }` for a
`clock.read<clock>` target). The authority must have exactly one known root because
runtime dynamic authority matching is unsupported. The provider may conservatively
have multiple possible roots: its root is not matched or subtracted, and it only
needs the same operation name and signature with a pure row. Its expression is
evaluated outside the handler. Body calls are handled deeply and lexically only
when the instantiated effect has the exact target name and authority root. Other
roots and effects remain in the row. This form has no resumptions, row
transformation, static/unparameterized targets, or dynamic authority matching.

Capability values and bound operations carry an interned, normalized nonempty set of possible lexical capability-parameter roots. A runtime value still has one root; the set is conservative provenance for computed `if` and `match` values. Immutable aliases, exact authority-preserving calls, restrictions, wrappers, and blocks preserve these sets, while reachable branches union them. Parameter- and `Self`-dependent effects expand to one ordinary parameter atom per possible root, so explicit rows must cover every possibility and inferred rows union them, including through recursive call components. Exact authority-return declarations and handler targets remain singleton guarantees.

## Contracts and Evidence

The bootstrap compiler stores each `requires` and `ensures` clause and each
newline- or comma-separated condition in deterministic arenas, then lowers one
public semantic obligation per condition after effect inference. Contract
expressions resolve in a fresh declaration-signature scope, use ordinary
expression typing, must produce `Bool`, and may call only finalized-pure
functions, operations, or closed callbacks. Body locals and a derived
capability wrapper's private source are not part of its interface scope.
Postconditions expose an ordinary return through `result`. Outcome prefixes are
available only on declarations returning `Result<T, E>`: a `success =>`
condition exposes `T`, while `failure =>` has no result binding. Every
`old(expression)` records a distinct typed entry-state
snapshot. Nested `old` and `old(result)` are rejected. The deterministic public
table records owner, clause kind, outcome, predicate and type, result binding,
and snapshot metadata. The public table is a semantic template, not evidence of
proof. Runtime callable CHECK policy and direct refined construction are executable
at this baseline; logical call-site substitution, normalized solver IR, and proof
discharge remain deferred. The separate P2.6 operation owner materializes bounded
executable predicate CFGs, not solver proofs.

Loop specifications use the same purity boundary but retain separate deterministic
templates. Each invariant contributes entry and preservation obligations, and one
`decreases` expression contributes nonnegative and strict-decrease obligations.
These templates are compiler-internal owning IR, are not added to external inspection,
and are never evaluated by the reference interpreter.

Evidence must be labeled **declared**, **runtime-checked**,
**proved-under-assumptions**, or **unknown**. A typed predicate is not a proof;
an executed check covers that execution, not all inputs. `pure` does not mean
total. Loop/decreases and unreachable templates are not discharged. Ghost state,
properties, solver-backed policies, and broad progressive-verification tooling
remain target work, not available interpreter policy choices.

## Bootstrap and CLI

The bootstrap compiler is written in C17 and currently provides a lossless lexer, a recovering parser for core declarations and authored tests, structured function contracts, refined predicates, loop invariants and termination measures, executable statement termination forms, structural tuple expressions/types/projections, recursive patterns and pure match guards, and function-body expressions, an arena-backed syntax AST, a token-preserving canonical formatter, deterministic package-session definition IDs, stable top-level semantic IDs and reference occurrences, lexical, module, import, and declaration-owned type/effect resolution, exact invariant interning for built-in and user type applications, bounded first-order generic records/enums/functions/declared types, nominal distinct construction, checked refined predicate templates, coherent traits and exact implementations, type-directed method calls, callback-inferred effect-row instantiation, checked records, enums, and tuples, nested usefulness and exhaustive matching, semantic effect rows with local inference across statically known calls, capability-backed exact handlers, deterministic contract, loop, and unreachable-obligation templates, mutable whole-local slots and checked replacement, path-sensitive ownership/borrowing, statement-only loops and lexical regions with exact cleanup metadata, an owning typed IR, a deterministic compiler-internal reference interpreter, source-aware human or JSON diagnostics through `sol check`, deterministic authored-unit-test execution through `sol test`, and deterministic effect, lexical-authority, and call-edge inspection through `sol effects`.

The following commands run from the repository root, not this document's directory:

```sh
cmake -S . -B build -G Ninja -DSOL_ENABLE_SANITIZERS=ON
cmake --build build
cmake --build build --target test
./build/sol check tests/valid.sol
./build/sol check tests/packages/valid
./build/sol test tests/test_cli/pass.sol
./build/sol run tests/run_cli/console.sol
./build/sol effects tests/valid.sol
./build/sol inspect tests/valid.sol
./build/sol fmt --check tests/valid.sol
```

The bootstrap is POSIX-only. Baseline CI configures fresh warning-as-error GCC and
Clang ASan/UBSan builds and the complete test suite. On Clang installations with
libFuzzer, `-DSOL_BUILD_FUZZERS=ON` builds parser, package, and bounded scalar-IR
mutation targets; unsupported toolchains report fuzzing unavailable without
breaking normal configuration. Fixed-seed smoke runs use [fuzz/corpus](../fuzz/corpus).
Deterministic stress workloads and reviewed hashes live in [tests/stress](../tests/stress).
From the repository root, run
`python3 tests/stress/run_stress.py --sol build/sol --baseline tests/stress/baselines.json --work-dir build/stress`.

### Formatting

`sol fmt <file-or-directory>` formats syntactically valid edition-2027 source in place; `--check` reports drift without writing, and `--stdout` prints one file. Directory formatting discovers files in package order, validates and formats every source before starting a transactional staged rewrite, rejects direct symbolic-link operands, preserves mode bits, and detects concurrent source changes before commit. The formatter preserves non-whitespace token and comment bytes, source order, commas, and deliberate hard line breaks. It canonicalizes spaces, four-space indentation, LF line endings outside comments, at most one blank line, one final newline, unary/binary operators, generic/effect angle delimiters, comparisons, braces, and delimiters. Every result is re-lexed, reparsed, checked for token preservation, and formatted a second time for byte idempotence.

Width-based reflow, insertion or removal of trailing commas, import/declaration
sorting, comment reflow, and semantic effect/order normalization remain future
formatter work. Malformed source is diagnosed and never rewritten.

### Effects and Inspection Output

`sol effects [--diagnostic-format=human|json] <file.sol|package-directory>` compiles the same bounded source/package subset and inspects the owning typed IR after frontend teardown. It reports every callable's normalized effect row and lexical authority, plus source-ordered direct-function, capability-operation, method, and callback call sites. Open callback rows retain their declaration-owned effect parameter. Callback targets and forwarded generic method dispatch remain explicitly dynamic rather than claiming a whole-program target set; constructors and contract-template calls are not graph edges. The deterministic version-1 JSON object is a bounded command result, not the stable inspection schema or a serialization of syntax, HIR, semantic tables, or internal IR. Effect rows disclose behavior and do not grant authority.

`sol inspect <file.sol|package-directory>` compiles the same bounded input and, on
success, emits exactly one deterministic compact `sol.inspection` version-3 JSON
object. Its six independently tagged artifacts are selected stable external
projections: syntax and types are version 3; HIR, effects, contracts, and
diagnostics remain version 1. Syntax v3 includes explicit recursive patterns,
match arms, and match ownership; types v3 includes exact per-pattern types,
resolved variants, fields, tuple ordinals, and child type relationships.
Original source bytes are standard padded base64. Paths are package-relative;
spans are file-local, zero-based half-open byte offsets. Top-level targets use
`sem:1:<32 lowercase hex>`; labeled dense IDs are snapshot-local, not cross-build
identities. Arrays retain documented insertion/source order or set normalization.

This is not a raw table dump, full public IR, or a stable encoding of internal
typed IR. [Schema compatibility rules](../schemas/README.md) require consumers
to ignore unknown fields but reject unknown closed versioned kinds. Changing
required-field meanings/types, ID interpretation, or ordering requires an
appropriate version change. Artifact versions advance independently only in
combinations permitted by the outer schema.
[sol-inspection-3.schema.json](../schemas/sol-inspection-3.schema.json) is current;
versions 1 and 2 remain unchanged historical contracts. C layout/ABI, pointers,
capacities, internal sentinels/enums, owning/machine IR, deserialization, complete
fact coverage, and cache-key suitability are excluded.

Compilation failures retain `sol.diagnostic/1` JSON, load/infrastructure failures
retain `sol.cli-error/1`, and usage returns 2. Construction failures write no
partial successful envelope; output transport can truncate an already constructed
envelope and returns 1. Allocation failure returns 1.

### Reference Interpreter API

[interpreter.h](../include/sol/interpreter.h) exposes the unstable library-only
reference interpreter. Requests select a free-function callable or definition,
provide owned-value arguments plus optional generic substitutions/evidence,
deterministic limits, and optional raw host-operation and cleanup-observer callbacks.
The limits cover steps, call depth, value nodes, text bytes, and host calls.
Cleanup observation is synchronous and infallible, receives only a local ID and
monotonic ordinal, and is counted in `result.cleanup_actions`; cleanup is not
limitable because a started unwind must finish. No observation occurs before IR
and complete argument validation, and the callback exposes no pointer. TEST
callables require an explicit `test_entry` request and remain unavailable as
source calls or function values.

A raw host-operation result is borrowed host-owned data valid through the
enclosing interpretation call and may alias callback inputs; the interpreter
validates and deep-clones it. Results own either an immutable runtime value or a
stable structured diagnostic whose bounded source-file path remains valid
independently of the IR. External capability values recursively validate their
definition-specific private source chain and exact shared root before binding;
bound operations and handlers additionally require callable ownership and exact
operation shape, preventing dispatch across capability domains. Host capability
results are checked against retained result-authority metadata.

Operands execute in **canonical formal order** because owning IR canonicalizes
named arguments and does not retain source operand order. Matches consume the
scrutinee once, recursively test structure, then evaluate a guard; a false guard
cleans candidate bindings and continues, while a true guard evaluates the arm
body, with deterministic reverse cleanup on every exit. Equality is statically
limited to primitives and immutable aggregates whose complete recursive contents
support structural equality. Capabilities, functions, bound operations, generic
parameters, `Self`, and aggregates containing them are rejected with `SOL-TYPE-027`.
A runtime invariant remains as defense against malformed IR.

### Hosting, Contracts, and Run

Opaque application hosting uses `SolHostRegistry` rather than the raw callback. Every entrypoint capability parameter receives a distinct registry-owned root, each bodyless data-only member is granted for one exact root, and the complete closed authority row is preflighted before Sol code executes. Safe callbacks receive no IR, callable ID, root token, or private capability source. The minimal profile models console output, immutable argument count/index access, and deterministic optional configuration lookup; filesystem, network, clock, randomness, console input, live environment access, and process mutation remain absent.

`SOL_INTERPRETER_CONTRACTS_CHECK` evaluates callable `requires` before execution, captures each `old` operand at entry, and evaluates applicable `ensures` after a valid return. `Result` success clauses bind the success payload, failure clauses have no `result`, and unprefixed clauses bind the complete return value. Bodyless hosted capability members use the same framing. Predicate calls consume ordinary deterministic limits; runtime failures keep their original diagnostic, while false predicates report dedicated require/ensure codes. Contract exits unwind ordinary owned bindings exactly once, and logical copies are not cleanup-observer actions. `SOL_INTERPRETER_CONTRACTS_IGNORE` remains available for explicit library compatibility. Refined constructors always evaluate their type-owned predicate and report a dedicated refinement violation on false. Loop invariant/decreases templates remain runtime-erased.

`sol run [--diagnostic-format=human|json] [--config=KEY=VALUE] <file.sol|package-directory> [-- arguments...]` executes the package's unique `@entry` with checked contracts after transferring owning IR and freeing frontend state. Human console output is byte-exact. JSON emits `sol.run-result` version 1 with base64 console data and symbolic application/runtime diagnostics. `()` maps to status 0, in-range `Int64` maps identically, driver failures use 1, and usage errors use 2. Arguments and configuration are explicit bounded snapshots; no process environment or source path is injected.

The argumentless `@entry` annotation selects a public nongeneric free function
with owned root-capability parameters, explicit closed effects, and a `()` or
`Int64` result. Compilation allows zero or one; application resolution requires
exactly one. Integer exit results must be in 0 through 255.

[tests/conformance/e6](../tests/conformance/e6) freezes the bounded executable-core
profile as one canonical three-module application. It covers imports and stable
identities; records, enums, tuples, generics, traits, recursive matching, ownership,
mutation, regions and cleanup; `Option`/`Result` propagation, typed and runtime-error
paths; executable contracts and refined construction; and exact
Console/Arguments/Configuration hosting. The baseline CTest configuration runs
that directory through `fmt --check`, `check`, `test`, `effects`, `inspect`, and
`run --config=mode=e6 ... -- fixture`, requiring four passing tests and exact
`E6 ok\n` output, then checks a configured panic's structured runtime diagnostic.

### Owning-IR Checks

Completed typed IR is ownership-checked in a separate transactional pass before
interpretation. Local reads are explicitly `COPY`, `MOVE`, `SHARED`, `EXCLUSIVE`,
or `UPDATE`; the validator independently recomputes access and use metadata.
`Int64`, `Bool`, `Text`, Unit, Never, structural tuples and `Option`/`Result` with
copyable contents, and nongeneric records/enums/distinct/refined values with
recursively copyable contents use the bootstrap structural `Copy` rule.
Capabilities, functions, bound operations, `Self`, generic parameters, and generic
nominal values are affine. Whole-local and projected moves consume the selected
path; `SOL-OWNERSHIP-001` reports later unavailable use. Borrow conflicts, owned
use while loaned, and invalid places/reborrows/escapes use `SOL-OWNERSHIP-002`
through `004`.

Shared reborrows accept shared or exclusive parameters; exclusive reborrows
require exclusive parameters. Borrowed affine parameters may be forwarded
compatibly or used as immediate capability receivers but cannot escape through
returns, owned calls, aggregates, variants, or bound-operation capture. Immediate
capability calls and handlers borrow authority/provider locals; standalone bound
operations remain owning. Branches, short-circuit paths, and loops join ownership
as described above. Partial/field borrows, projected mutation, and loops are
implemented, not remaining whole-local-only restrictions.

Successful syntax trees validate every arena count/pointer pair, discriminant,
active payload span, linked-arena owner, cycle, share, and orphan before semantic
projection. Owning IR independently validates discriminants before payload access,
generic-aware literal/operator/call/aggregate/branch/match/block/propagation type
relations, callable returns, current-callable provenance roots, structural IDs,
ownership, ordering, sharing, and nesting before execution. Source-driven tests
census every successful AST expression kind and all owning-IR expression,
statement, and pattern kinds; composite second-file package fixtures exercise
populated expression and linked arenas at nonzero relocation offsets. Historical
inspection v1 remains unchanged; current inspection is v3. First-class references,
allocator regions, lifetime generics, user drop/destructors, close protocols,
unsafe, and FFI remain deferred.

### Authored Tests and Diagnostics

Top-level `test "label" expression` declarations are zero-parameter, nongeneric, private, non-importable, and non-addressable from source. Their body is contextually `Bool`; `true` passes, `false` fails, and a runtime diagnostic fails the test. Tests cannot use annotations, visibility modifiers, contracts, authority clauses, or explicit effect clauses. Effects infer as for private function callers. `sol test [--diagnostic-format=human|json] <file.sol|package-directory>` compiles exactly as `sol check`, frees all package and frontend state, then discovers dedicated TEST definitions and exact callable IDs from owning IR in bytewise package-path and source-declaration order. Every test receives fresh default interpreter limits, checked transitive contracts, and no host callback or ambient capability. Execution continues after false results and runtime failures; exit status is 0 only when compilation and every test succeed, 1 otherwise, and 2 for usage errors. Output contains no timing.

Successful JSON execution emits one bounded versioned result object: `{"schema":"sol.test-results","version":1,"tests":[...],"summary":...}`. Each test entry has `path`, decoded `label`, `status` (`passed`, `false`, or `runtime_error`), and `diagnostic`; runtime diagnostics contain numeric `code`, `message`, owned `path`, and aggregate `offset`. Human runtime failures retain the declaration label/path and additionally report the diagnostic file and aggregate offset. The summary has integer `total`, `passed`, and `failed`. Compilation failures use the same diagnostic JSON as `sol check`, not the test-result schema. Package-load failures use `{"schema":"sol.cli-error/1","kind":"load","message":...,"path":...}` on stdout with no JSON-mode stderr; infrastructure failures use the same schema with kind `infrastructure`. JSON serializers emit valid UTF-8 sequences unchanged, use ordinary JSON escapes for syntax and controls, and represent each invalid input byte deterministically as the Unicode scalar escape `\u00XX`. Thus valid text retains its JSON string meaning; an invalid byte is a visible safe marker and is not claimed to round-trip through JSON scalar semantics. Human test labels likewise display valid UTF-8 normally and escape invalid bytes. Examples, authored or generated properties, boundary generation, shrinking, seeds, and ghost state remain roadmap work.

## Packages and Semantic Identity

Directory checking recursively discovers regular `.sol` files in deterministic bytewise path order and treats them as one dependency-free package. Each file declares its module; files with the same module path share declarations, while each file has its own explicit `use module.path.Symbol` scope. Cross-module imports require a public declaration. Imported types, functions, generic bounds, traits, effects, and contract calls use package-wide checked identities. Import cycles are permitted because this subset has no top-level initialization. Direct single-file checking remains compatible with the earlier bootstrap and parses but does not enforce imports.

Every top-level declaration receives a versioned 128-bit semantic ID separate
from its dense package-session `SolDefId`. Without an annotation, identity derives
from normalized module path, declaration kind, and declaration name, independently
of file and declaration ordering. A named public declaration may use
`@stable("package-local-token")`; retaining that token preserves identity through
module moves and renames. Duplicate, malformed, private, and implementation
identity annotations are rejected. HIR records declaration, import, expression,
type, implementation-trait, and trait-bound occurrences with both dense targets
and semantic target IDs. Member/local identities, package-global identity across
dependencies, compatibility validation beyond collision detection, and a full
public Sol IR with a stable serialized schema remain future work. The selected
inspection projections have their own narrower compatibility contract above.

This bounded package subset has no manifest, external dependencies, package aliases, re-exports, relative imports, grouped imports, wildcard imports, import aliases, module-to-filesystem-path requirement, or imported extension-method search. Consequently explicit stable tokens are package-local rather than globally namespaced. Direct symbolic-link operands are rejected and directory traversal does not follow nested symbolic links. Multiple files may contribute to one module, and private declarations are module-visible in this subset; the target language's distinct private/module/package visibility levels remain future work.

## Abstraction and Authority Restrictions

Structural function types carry normalized semantic effect rows, including one
optional declaration-owned row parameter in the bounded generic subset. Calls
through function values propagate instantiated rows through inference and explicit
checks. Exact functions and bound operations coerce contextually to compatible
callback shapes; finalized effects must fit the callback row.

Capability-valued members can declare `authority { result derives_from Self }`;
exact capability-returning functions can derive result authority from a named
capability parameter. Both preserve provenance through aliases and nominal,
type-changing restrictions. Closed nominal wrappers have exactly one private
capability source, checked member bodies, cycle rejection, and root-preserving
construction and `Self` substitution without subtyping or coercion.

Explicit declaration rows form modular boundaries. Omitted private function rows
infer a least union across direct operations and statically known calls, including
self-/mutually recursive call-graph components and formal-argument/`Self`
substitution through immutable capability and bound-operation aliases. Exact
handlers retain source/provider operation metadata for runtime interception and
subtract matching name-and-root atoms without removing call-graph edges; provider
evaluation and residual effects remain outward. Reachable `if`/`match` provenance
joins union lexical parameter roots, expanding dependent effects over the whole
set. Public functions and capability members require explicit rows.

Lambdas, resumptions, transformed open handler rows, dynamic handler targets,
computed joins of distinct function identities, parameter-dependent callback
authority rows, and `Self`-dependent bound-operation coercions are unsupported.
Unknown computed provenance remains unsupported. Contracts are resolved, typed,
purity-checked, and executable under CHECK; ownership and reference execution are
implemented at this baseline.

User generics are deliberately bounded to type parameters on records, enums, distinct/refined declarations, and free functions plus at most one trailing `effects E` parameter on a free function. Type applications are invariant and require every type argument when written. Type arguments are either all explicit (`identity<Int64>(1)`) or inferred from value arguments (`identity(1)`); generic declared-type constructors require explicit arguments. Effect arguments cannot be written explicitly; they are inferred only from structural callback parameters such as `function(T) -> U effects E`. The least authority-independent closed row is substituted into the owning function's explicit `effects { E ... }` row and recorded per call. Multiple bounds, defaults, variance, higher-kinded/lifetime/const parameters, explicit or multiple effect arguments, all result-position row parameters, generic capabilities or members, recursive row polymorphism, generic callback coercion, open-row handler transformation, and separately compiled dependency instantiation are not supported. Each generic body, contract, and refinement predicate is checked once symbolically.

The bounded trait subset uses nongeneric `trait` declarations, `implementation Trait for Type` declarations, and at most one inline `T: Trait` bound on each free-function type parameter. Trait requirements and implementation methods begin with `self: Self`, have explicit canonical parameter/result types and exact closed effect rows, and cannot declare defaults, contracts, authority clauses, generic parameters, or row parameters. Implementation targets are exact closed built-in, record, enum, distinct/refined, or fully applied user types; one coherent implementation must provide every requirement exactly once with no extra methods. Immediate `value.method(...)` calls resolve from a concrete receiver's exact implementation or a rigid parameter's declared bound, record deterministic checked metadata, and use the implementation or requirement row respectively. Generic bound evidence forwards through generic calls. Method values, generic/blanket/conditional implementations, multiple bounds, associated items, trait inheritance, specialization, trait objects, imported extension lookup, and dependent method effects remain unsupported.

`type Name<T> = distinct Representation` creates a fresh nominal identity with no implicit conversion to or from its representation. Distinct and refined construction use exactly one positional argument, and generic construction requires explicit type arguments. `type Name<T> = refined Representation where predicate` binds contextual `self` at the representation type, requires a `Bool` predicate, applies the finalized contract purity firewall, and emits one deterministic type-owned obligation. Every direct refined construction executes that predicate before producing the nominal wrapper. `using` predicates, inline refinements, base projection, predicate assumptions, refined pattern reasoning, static proof discharge, capability-containing representations, and cyclic representations remain unsupported.

Mixed capability and bound-operation provenance is exposed by the type table as checked, interned root sets. Effect checking defensively recomputes propagation and validates canonical sets before expanding every dependent atom across all possible roots.

Effect rows disclose behavior but never grant authority. In executable bootstrap
source, `panic` and `diverge` are the only compiler-defined authority-free atoms.
Every other non-pure atom requires a lexical capability parameter, or `Self` on a
capability member; unresolved static authorities such as `network.call<Production>`
and unparameterized resource effects are rejected with `SOL-EFFECT-010`. Imports
add names but no authority. Closed structural callback rows may contain only
`panic`, `diverge`, and the bounded row parameter because callback types cannot
capture lexical capability roots. Static/unparameterized handlers, package-global
authority, manifests, host wiring beyond E3's exact grants, unsafe gates, FFI,
and a broader intrinsic-effect registry remain deferred.

## Internal MIR (P1)

P1 is complete for every bodyful frozen-E6 callable. The unstable callable-scoped
MIR in [mir.h](../include/sol/mir.h) and [mir.c](../src/mir.c) is separate from
owning IR and from the production compilation session/CLI. P1 does not choose
representation or ABI and is not a backend; downstream P2.4-P2.6 provide
representation, layout, and operation plans. The API lowers one selected validated
free/test callable or executable
trait-implementation body from owning IR into deterministic target-neutral basic
blocks with SSA temporaries and
block parameters over explicit local storage, whole-local copy/move/store,
conditional cleanup/storage death, scalar operations, `if`, short-circuit Boolean
control, return, panic, lexical regions, `require`, proof-backed unreachable, and
statement-form `loop`/`while` with nearest-loop break/continue.
Nongeneric direct function calls use an abstract invoke terminator with distinct
normal and failure-cleanup edges; ordered operands distinguish owned values from
call-scoped whole-local shared and exclusive borrows, with exclusive writeback only
on normal completion. `sol_mir_validate` independently checks arena ownership,
edges, invoke/refinement result transport and argument provenance, source/type/control-event
relations, exact parameter activation, local initialization/moves, lexical cleanup
order, balanced regions, loop source/parent identity, condition/body/exit edges,
natural backedges, and cleanup at transfer boundaries. Loop obligations are retained
as proof-only metadata and not executed. Infallible records, tuples, enum variants,
Option/Result cases, non-refined distinct wrappers, and derived capability wrappers
use semantic construction instructions with ordered operands and exact capability and
operation-root provenance without choosing layout. Typed reusable temporary slots
stage multiple owned call/construction
operands, consume exact pending suffixes on invoke/construction, preserve outer
values across loop transfers, and destroy abandoned operands in interpreter order.
Checked refined construction retains its exact type-owned predicate
obligation in a target-neutral terminator, consumes the representation on either
outcome, transports the nominal result only on success, and performs pending,
local, and region cleanup before resuming failure. Unsupported constructs fail
transactionally. Recursive matches stage one scrutinee across source-ordered
structural tests and false guards, use source-bound provisional pattern values,
consume the scrutinee only on selection, and retain explicit no-match failure and
binding cleanup CFG. Option/Result propagation uses a two-result semantic terminator
that transfers either an unwrapped success value or a callable-result-typed residual;
the residual path performs complete cleanup before an ordinary return. MIR retains
semantic predicate, pattern, and sum operations; the separate P2.6 operation owner
closes their executable plans without defining a backend ABI. Canonical local-rooted
record/tuple paths support projected copies, fully initialized replacement, and
direct shared/exclusive operands with sibling-aware overlap and abstract normal-edge
writeback. A separate layout-free path-state fixed point tracks projected moves,
sibling availability, exact/ancestor hole restoration, conservative joins and loop
backedges, borrow availability, and whole-local cleanup of partial aggregates. Checked
compound assignment stages an update-read before the RHS, retains the exact operator
and target place, and commits replacement only after the abstract checked arithmetic
succeeds. Dynamic callback calls stage the
callee before operands and use the same abstract normal/failure invoke edges, with
exact structural signature, access, formal-operand-order, temporary-consumption, and cleanup
validation. Concrete nongeneric method calls select implementation callables from
exact non-forwarded evidence and retain owned/shared/exclusive receivers before
operands, with receiver-first normal writeback and failure suppression. Direct
capability invokes borrow the exact bound-operation receiver and retain operation
identity plus independently expanded root-specific effects without materializing a
bound-operation value. Operand-dependent capability effects remain deferred in
this MIR invoke boundary. MIR retains exact source-bound handler enter/exit
scopes around nested bodies. Lexical
cleanup emits handler exits on normal, transfer, propagation, and failure paths, while
an independent CFG fixed point verifies exact metadata, LIFO nesting, identical join
stacks, and terminal balance.

### Callable Contracts and Generic Bodies

A bounded callable-contract envelope retains
item-owned requires as ordered three-way semantic checks, captures exact infallible
direct-local `old` snapshots once after successful preconditions, and routes every
successful body result through one complete-result epilogue of outcome-qualified
ensures. Semantic violations and predicate runtime failures remain distinct cleanup terminals; panic
and invoke failure bypass postconditions. This vocabulary chooses no predicate,
snapshot, Result, layout, or ABI representation. At this callable-lowering boundary,
contracted generic/effect-parameter and exclusive/inout forms remain transactionally
unsupported, as do fallible or projected snapshot operands. Snapshots are direct
unprojected locals of exact `Int64`, `Bool`, or Unit type. Capability-member bodies
do not have standalone P1 MIR; approved bodyless import contracts are instead
closed by P2.6 and must not be described as globally unsupported.
Standalone bounded generic and
effect-polymorphic free-function bodies preserve their exact symbolic owning-IR
parameter metadata. Invokes retain owning-IR type-argument, instantiated effect-row
and tail, and dispatch-evidence slices without monomorphization; forwarded trait
evidence keeps its requirement, binding, and caller parameter rather than selecting
a concrete method. Executable trait implementations activate and clean their exact
receiver before ordinary parameters. The baseline reported all 14 executable E6 callables lowering
deterministically; the five bodyless trait requirements and hosted capability members
intentionally have no standalone MIR.

### SSA, Rendering, and Evaluation

The independent validator computes deterministic
reverse-postorder immediate dominators from compact predecessor slices and validates
every ordinary instruction, terminator, and edge-argument SSA use. Dominating
instruction and block-parameter values may cross blocks, same-block use-before-definition
remains invalid, and terminator-produced values remain scoped to their designated
edges. It shares the owning IR's structural `Copy` fixed point and validates
affine SSA availability path-sensitively. Consuming instruction and terminator uses
make values unavailable, edge transport consumes each source at most once per edge
and defines fresh target parameters, alternative edges remain independent, and
may-unavailable joins converge across branches and loop backedges.

`sol_mir_render` emits a validated, overflow-checked, versionless internal text form.
It emits fixed ASCII spellings and escaping, explicit sentinel IDs, source-owned
relations, and every semantic MIR block, value, instruction, place, edge, temporary,
loop, call, construction, contract, and proof relation in deterministic arena or
block order. Rendering is buffered before one output write and is intentionally not
a stable serialization format. `sol_mir_evaluate` is an independent bounded
CFG evaluator over an explicit entry MIR and trusted immutable callee provider. It
executes the frozen instruction and terminator vocabulary with MIR-local step, depth,
value, text, host, and trace limits; emits deterministic structured call, block,
instruction, edge, host, cleanup, return, and failure events; and reuses only the
unstable runtime-value model and raw host hooks, not owning-IR control evaluation.
Source-owned contract and refinement obligations use a separate bounded pure
predicate evaluator. Preflight rejects unavailable bodyful capability closures and
runtime-produced callable closures before effects. Differential tests compare final
values or failure codes/spans, host behavior, and exact owned-local cleanup against
the reference interpreter. This is MIR-vs-owning-IR differential testing, not the
future interpreter/Wasm suite.

## Symbolic Program and Planning (P2.1-P2.2)

The separate unstable [SolMirProgram](../include/sol/mir_program.h) symbolic owner
uses deterministic entry/test/fixture roots to retain one
validated MIR template per reachable lowerable bodyful callable, approved safe
bodyless capability imports, concrete trait specialization demands, and
path-free source relations under local callable, reference, and discovery-work
limits. The discovery meter begins after canonical request deduplication and
does not claim prerequisite owning-IR validation or nested public MIR
lowering/validation work. Forwarded evidence propagates only through compatible
concrete incoming call contexts; trait-bounded generic roots are independent and
always reject without an explicit root-context API.

### Finite Callable-Producer Closure

Every ordinary or predicate
callback site requires its exact callee expression to be a direct static
definition or bound operation. Current P1 lowering cannot materialize an ordinary
first-class static callback form, so ordinary body callbacks and dynamic,
conditional, or aliased producers reject. P2.2 plans static callable-value
references retained by P2.1, including pure predicate references. This is a trusted
mutable compiler-internal owner, not a hostile arbitrary-pointer format:
validation checks canonical null-empty headers and detectable top-level/nested
allocation overlap, while teardown requires the original builder-owned allocation
bases. The baseline reported the E6 entry closure as exactly 8 templates, 4 imports, 1 specialization
demand, and 12 invoke sites;
entry plus all four tests is 14, 4, 1, and 18. This owner borrows immutable owning
IR and deliberately performs no monomorphization, substitution, representation,
layout, ABI, symbol selection, linkage, or backend emission.

### Canonical Instances and Limits

The separate unstable [SolMirPlan](../include/sol/mir_plan.h) owner borrows this
validated program. It structurally interns
closed concrete types and effect rows, canonical callable instances and resolved
evidence dictionaries, typed imports, provenance-bearing demand edges, and complete
flat type overlays without cloning or mutating any MIR CFG arena. Identical recursive
keys become cycle edges; expanding generic recursion, unresolved evidence, malformed
owners, four explicit cardinality limits, bounded substitution depth, and the
narrow planning-work probe meter fail transactionally. That work meter covers
structural interner/equality probes, recursive substitutions, and instance-key
probes; other loops are bounded by the explicit dimensions and
validated source domains. Every owned plan arena records its allocation capacity,
and effect atom strings carry validated lengths.
Contextless generic, receiver, or effect-polymorphic fixture roots reject because
the API intentionally has no explicit root-context input. The baseline reported the E6 entry plan as
exactly 8 instances, 4 typed imports, 12 invokes, and 13 demands; entry plus tests
is 14, 4, 18, and 23.

## Concrete Materialization (P2.3)

The separate unstable [SolMirMaterialization](../include/sol/mir_materialize.h)
owner borrows a validated `SolMirPlan`, which must outlive it
along with the plan's borrowed program and owning IR. It owns one image per plan
instance in plan-instance order. Plans assign explicit canonical IDs to one body
context per instance and every refinement site/obligation; typed uses and demands name
those records rather than deriving a context from a block number. Materialization
consumes every typed use exactly once into a context-bearing semantic record and owns
concrete types, signatures, locals, places/projections, values, temporaries,
instructions, operands, closed effect rows, imports, complete blocks and terminators,
edges and their arguments, block parameters, and loops. One canonical binding exists
for every root, invoke, callback, predicate, handler, function-value, bound-operation,
and predicate-function-value plan demand, and one explicit concrete semantic-site
record consumes each binding. Signatures own exact receiver and parameter access modes,
and activated receiver/parameter locals must match them in order. Invokes name only a binding and closed
concrete effect row; successful normal edges own ordered exclusive writebacks, with
the receiver first and arguments in formal order. Handler enter/exit instructions name
explicit nested-capable frames containing exact source/provider bindings, concrete
operation keys, and concrete authority/provider places. The builder records nominal
categories and substituted ownership components from authenticated owning IR;
independent concrete validation then recomputes the exact Copy fixed point solely from
those materialized type records, including recursive records/enums and
distinct/refined representations. Concrete type metadata separately owns
substituted source-order nominal field and variant shapes, preserving source IDs,
ordinals, and per-variant boundaries; distinct/refined backing types, optional
derived-capability source types, nominal openness, produced function types, and
bound-operation receiver types are explicit rather than inferred from ownership
components. Derived-capability private source chains are separate concrete plan/type
edges, so selecting only the most-derived capability still closes every source type
without treating private sources as Copy ownership components. Recursive and generic
nominal fields and capability sources resolve through canonical plan contexts without
recursively expanding C records. A symbolic generic move that
specializes to a concrete Copy type is normalized to an executable copy, while the
authentication-only topology retains its source opcode.

Each image also contains an independently allocated exact symbolic MIR clone. That
symbolic topology is authentication-only: executable dispatch does not use borrowed
plan imports, generic/effect/evidence slices, symbolic callable targets, or symbolic
CFG IDs. Validation first reconstructs every concrete record from the immutable
canonical plan for topology and provenance authentication, then runs independent
bounded algorithms over only materialized records. Those algorithms compute concrete
CFG predecessors, reverse postorder, and immediate dominators; validate same-block
order, edge-scoped results, affine transport from concrete Copy flags, local/place and
partial-move fixed points, temporary/region/handler stacks, contract and refinement
envelopes, loop records, invoke signatures/writebacks, exact semantic-site consumption
and callable-producer target/operation identity, and orphan-free whole-graph closure.
Contract/refinement terminators own their complete context-linked semantic-site sets;
helper results may have any declared type while the final obligation remains exactly
`Bool`. Validation records a deterministic conservative cardinality-derived upper bound.
It explicitly multiplies the storage/path pass ceiling by instruction, argument, merge,
place, and projection scan bounds, in addition to global, SSA/affine, closure, contract,
handler, and superlinear dominator terms. An exact separate validation-work limit rejects
exhaustion transactionally. Source
IDs remain provenance associations and are not type or Copy truth. The baseline
reported the frozen E6 materialization closure as exactly 8 images, 4 imports, and 12 invokes for entry, and
14, 4, and 18 for entry plus all tests; every bodyful E6 callable is represented.
Builds are transactional and bound instances, cloned and concrete CFG items, concrete
records/bindings, owned bytes, and work; rendering validates and buffers before one
write. Concrete source-shape substitution also has an exact dedicated work sub-limit;
exhausting it preserves the resource-exhausted outcome rather than being relabeled as
an unsupported shape.

## Canonical Representation (P2.4)

The separate unstable [SolMirRepresentation](../include/sol/mir_representation.h)
owner borrows a validated
materialization and owns exactly one same-ID target-neutral recipe for every concrete
type, plus flat field, variant, recipe-ID/access, and callable-producer arenas. Recipes
cover `Int64`, `Bool`, `Text`, Unit, Never, tuples, records, enums, `Option`, `Result`,
distinct/refined wrappers, structural functions, and capabilities. Exact function
values and bound operations are producer records attached to function recipes, so one
signature may have multiple producers; a bound producer retains its resolved
instance/import, concrete captured receiver type, optional place/temporary/value,
source expression/context, normalized concrete root set, and effects. Computed legal
receivers, including authority-preserving call results, therefore do not require a
place; conditional or aliased dynamic callable producers rejected by P2.1 remain
outside this boundary rather than failing later in materialization. Capability recipes
retain the concrete nominal definition and optional concrete capability source type,
not runtime roots or source pointers.

Fields and variants retain source order. Every sum has an explicit semantic tag:
source enum ordinal, `Option` None=0/Some=1, and `Result` Ok=0/Err=1. No niche is
selected. Bounded least fixed points classify inhabitedness and then zero-sized values;
an independent greatest fixed point recomputes Copy and must agree with materialization.
Unit and inhabited all-zero products have no abstract storage, uninhabited types retain
a separate flag and no value storage, sums always reserve abstract tagged aggregate
storage, and inhabited aggregates use an abstract aggregate-value storage class.
Recursive recipe edges do not choose indirection; the downstream layout selects uniform indirect
storage for nonzero inhabited aggregate values. Distinct/refined
recipes retain nominal identity while following backing storage. Text, functions, and
capabilities use distinct abstract handles. Explicit Copy and drop kinds describe
trivial, text, aggregate, wrapper, forbidden/unreachable, callable, and capability
behavior without selecting byte sizes.

The representation builder has complete limits and usage, all-zero transactional
destinations, rejects reachable open enums, and validates allocation independence from
all transitive borrowed arenas. `owned_bytes` measures persistent output allocations;
separate exact peak build- and validation-scratch byte limits cover fixed-point facts
and a conservative bound for prerequisite materialization validation. Build work charges
counting, nested shape population, producer/root copying, fixed-point scans, and
wrapper traversal. Independent validation exactly replays those charges from validated
materialization dimensions and validation-only facts, including dynamic convergence
and short-circuit field/variant scans; it does not trust recorded build work. Validation
preflights checked counts, capacities, persistent bytes,
work, scratch, and raw allocation ranges before reading any owned arena element, then
rejects borrowed overlap before typed traversal. As with the other unstable mutable
owners, malformed pointer/capacity tests must restore original allocation bases and
extents before `free`; freeing a deliberately aliased rejected mutation is invalid.
Rendering validates and buffers before one write. The validator is in a
separate translation unit and independently derives every recipe field, flat-arena
boundary, fixed point, abstract classification, producer, and receiver-root slice from
the validated materialization without invoking representation construction or building
a second representation. The baseline reported an exact exhaustive E6
entry-plus-four-tests census.

## Target Layout (P2.5)

The separate unstable [SolMirLayout](../include/sol/mir_layout.h) owner borrows a
validated representation. Every build requires an
explicit validated target descriptor; there is no ambient host default. The initial
`sol_mir_target_wasm32()` profile is pointer-size/alignment 4, Int64 alignment 8,
little-endian, with object sizes bounded by `UINT32_MAX`. Descriptors accept pointer
sizes 4 or 8, power-of-two alignments that divide their scalar sizes, either explicit
endianness, and a nonzero `uint64_t` object bound no larger than the target pointer's
unsigned addressable maximum (`UINT32_MAX` for pointer-4 and `UINT64_MAX` for
pointer-8).

The chosen internal profile represents `Int64` as an 8-byte, target-aligned two's-
complement scalar and `Bool` as one byte aligned to one with values 0/1. Unit, Never,
uninhabited values, and inhabited zero-sized products occupy zero bytes aligned to one.
Every other inhabited nonzero tuple, record, enum, `Option`, or `Result` value is a
pointer-sized owning handle to a separately described object; empty inhabited products
have no object and uninhabited aggregates have neither value storage nor an object.
Distinct and refined types remain nominally distinct layout records while transparently
following both backing value and object layout. There are no niches.

Product objects use checked source-order packing and record each field's offset,
alignment, size, leading padding, object maximum alignment, and tail padding. Sum
objects always contain an explicit u32 tag at offset zero, including one-variant sums.
Their common payload starts at `align_up(4, maximum inhabited payload alignment)`;
each inhabited variant has independently source-packed fields with absolute object
offsets, and the object reserves the maximum inhabited payload capacity. Empty enums remain
uninhabited and tags above `UINT32_MAX` reject. Zero-sized fields may share offsets.
Shape records remain complete for unreachable aggregates: fields of an uninhabited
product and fields of an uninhabited sum variant have `has_storage=false` with canonical
zero offset/size/padding and alignment one. Every variant retains its semantic tag and
inhabitedness; `has_payload_storage=false` identifies variants that contribute no
physical payload. Object bounds therefore apply only to physically present storage.

Text objects contain a target handle followed by a pointer-sized length. Callable
objects contain a target token and add an environment handle when the represented
signature has a bound-operation producer. Capability objects contain a root token and
a private-source handle. These are object layouts only: target-token assignment, null
encoding, allocation headers, transfer semantics, and function-table policy remain
deferred. Every materialized record-field or tuple-ordinal projection maps its exact
base/result recipe and immediate object-relative field offset; reserved index and
dereference projections reject.

Layout construction has exact count, persistent-byte, build scratch/work, and
validation scratch/work limits and an all-zero transactional destination. Validation
is in a separate translation unit. It independently checks the descriptor and
derives every value/object layout, source-order product and sum packing decision,
canonical dead record, projection map, exact arena consumption, and successful-builder
resource charge without calling construction helpers or building a second layout.
Validation preflights checked dimensions, disjoint owned ranges, and each borrowed
owner-record link for null and overlap, validates the representation, and then rejects
overlap with every transitive representation, materialization, plan, program,
template/image MIR, IR, and string arena before reading layout records. Borrowed links
must still be trusted readable pointers; validation detects malformed structure and
overlap but is not an arbitrary-address recovery API. The baseline reported the
frozen E6 Wasm32 closure's exact 21-type, 11-field, 9-variant, 5-projection census,
including every record and source identity. Rendering
validates and buffers before one write. As with other trusted mutable internal owners,
rejected pointer/capacity mutations must be restored before free.

## Semantic Operations (P2.6)

The separate bounded [SolMirOperations](../include/sol/mir_operations.h) owner
borrows a validated layout. It retains one concrete access plan per materialized
place plus operation-local
paths; exact constructor, recursive pattern, propagation, checked arithmetic/equality,
snapshot, callable-producer, and handler plans; segregated authenticated source
provenance; independent reconstruction validation; and transactional buffered
rendering. Propagation records separately identify source success extraction, source
residual extraction, and destination residual construction variants, tags, fields,
and offsets. Equality graphs carry exact recursive recipe, field, variant, and tag
edges. Build dimensions and scratch are checked before owner allocation; build and
validation work are incrementally charged by their actual scans, recursive walks,
allocations, and record traversals, with transactional cleanup on exhaustion.
Operation validation work includes the layout validator's authenticated recorded work,
and validation scratch is the exact peak of the sequential layout and operation-local
phases. Limit sets are either wholly populated or wholly zero. Executable operation
records require no source expression, pattern, field, variant, obligation, or
snapshot IDs.

### Predicate CFGs and Import Contracts

P2.6 is complete, including rich predicate CFGs from P2.6b2. There is no
`UNRESOLVED_BODY` state and no general scalar-single-block-only restriction.
Separate synthetic predicate IDs own immutable contextual inputs, SSA values,
instructions, blocks, and exact Bool returns. These bounded source-independent
CFGs are shared by instance and import contexts. The frozen profile lowers
scalar/Text/Unit constants, direct contextual inputs, checked unary/binary
arithmetic/comparison/equality, short-circuit and conditional control;
exact calls and function/bound-operation
values under its purity and finite-closure rules; aggregate, tuple, sum, wrapper, and
refined construction; recursive matches, guards, and bindings; immutable blocks and
locals; and nested refinement body references. Canonical interning is cycle-safe.
Contract expression propagation remains semantically rejected under
`SOL-CONTRACT-002`; propagation records remain in the validation vocabulary so
malformed records are rejected, not positively lowerable.

Approved bodyless capability contracts are discovered independently of a body
instance, retain import-owned substitutions, typed uses, contexts, and helper demands,
and materialize one ordered require/snapshot/ensure envelope per concrete import.
Snapshot inputs use envelope-local capture slots; global source snapshot IDs occur only
in segregated provenance. Import-owned semantic sites carry explicit import ownership
and no fake CFG coordinate. Independent reconstructive validation authenticates the
complete executable field anatomy, SSA/CFG structure, types, calls, nested body
references, segregated source provenance, and exact resource census. Source IDs occur
only as authenticated provenance; executable predicate records do not depend on them.
Loop proof obligations remain erased. All borrowed upstream owners must remain
alive and immutable through downstream validation, rendering, and destruction;
the lifetime chain is operations -> layout -> representation -> materialization
-> plan -> program -> owning IR. These trusted internal APIs are not arbitrary
pointer recovery or public serialization boundaries.

### Baseline Evidence

The baseline reported an all-roots E6 census of 28 constructors, 16 binary operations plus one compound
update, three pattern tests and three extractions, two propagations, one snapshot,
three contract slots plus one refinement slot, no handlers or callable producers,
and one access plan per materialized place with five total access steps. Its separate
predicate census is four bodies, four blocks, four inputs, ten values, three constants,
three comparisons, and four returns, with no predicate edges, calls, constructions,
locals, patterns, or propagations; one snapshot is referenced. The four E6 imports
each have an empty contract envelope. Independent
opcode classification finds five checked arithmetic operations and twelve
comparison/equality operations in that closure. This fixture's four single-block
predicates do not bound the richer supported predicate CFG vocabulary or count
the entire compiler suite.

Baseline documentation reported 46/46 normal and ASan/UBSan suite results.
These are historical reports, with no fresh full-suite rerun for this snapshot. The
CMake project configures CTest; use `cmake --build build --target test` from the
repository root with a configured testing build. Test discovery must be evaluated
in the actual build directory, not inferred from a source-directory invocation.

## Whole-Program Linkage (P2.7 Current-Worktree Addendum)

The separate unstable [SolMirLinkage](../include/sol/mir_linkage.h) owner follows
operations and borrows one validated `SolMirOperations`, which must remain alive and
immutable. The complete lifetime chain is linkage -> operations -> layout ->
representation -> materialization -> plan -> program -> owning IR. The public
compiler-internal API comprises `sol_mir_linkage_init`, `sol_mir_linkage_free`,
`sol_mir_linkage_default_limits`, `sol_mir_linkage_build`,
`sol_mir_linkage_validate`, and `sol_mir_linkage_render`.

Each materialized image has one canonical internal callable. Its stable, versioned,
lowercase ASCII `sol.i1` symbol combines the callable's 128-bit semantic identity
with the full 256-bit SHA-256 digest of its canonical structural instance key; an
entry root receives the corresponding `sol.e1` export symbol. Structural keys cover
the complete concrete callable identity rather than arena positions. Symbols,
identities, ordering, and rendering exclude source paths, pointers, source ordinals,
and unstable dense IDs. Distinct structural values that collide at any instance,
host-requirement, table, runtime-recipe, or exported-symbol identity are rejected
rather than silently deduplicated.

The owner resolves every materialized binding to exactly one internal callable or
approved host requirement and exports each entry binding to its internal callable.
Approved-host requirements retain typed receiver recipe/access, ordered parameter
recipes/accesses, result recipe, and closed effect row. Function and bound-operation
values map to deduplicated abstract whole-program table identities; `SolMirLinkageTableId`
values are not concrete target table indices. Recipe requirements symbolically name
the typed create, copy, drop, equality, and bound-environment operations needed by
the reachable closure. Host and runtime requirements are ABI-neutral: linkage does
not choose calling conventions, external ABI names, wrappers/adapters, Wasm names,
indices, sections, or encoding.

Construction is transactional into an all-zero destination. Limits and live usage
cover callable, binding, entry-export, table-entry, callable-value, host-requirement,
and runtime-requirement counts; persistent owned bytes; peak build/validation scratch
bytes; and incrementally charged build/validation work. Canonical sorting is charged.
Validation lives in a separate translation unit, reconstructs identities, bindings,
requirements, canonical order, exact arena consumption, successful-build work, and
the E6 census without invoking construction helpers. It preflights counts, capacities,
usage, and disjoint owned ranges, then rejects overlap with all transitive borrowed
owners before typed traversal. Rendering independently validates and buffers the
complete canonical text before one output write.

The all-roots E6 linkage census is 14 internal callables, 23 resolved bindings, one
entry export, four approved host requirements, no callable values or table entries,
and 17 typed runtime requirements. Focused coverage also exercises internal and host
table identities, generic/trait/recursive closure, import-contract helpers, path/root/
dense-ID independence, exact limit boundaries, collision injection, malformed owner
records, transactional failure, SHA-256 known answers, and deterministic repeated
rendering. Approved validation reported the Debug Werror suite passing 47/47 and the
AppleClang ASan/UBSan Werror suite passing 47/47 on macOS. Leak detection was disabled
because Apple ASan reports it unsupported, so leak checking did not run. The focused
linkage test passed, all three deterministic stress hashes matched, and
`git diff --check` passed. At validation time, Typst 0.15.1 regenerated the checked
PDF byte-for-byte identically.

## Complete Concrete Program (P2.8 Current-Worktree Addendum)

The unstable compiler-internal
[SolMirConcreteProgram](../include/sol/mir_concrete.h) embeds and owns the symbolic
program, monomorphic plan, materialization, representation, target layout,
source-independent operations, and linkage. Only its source `SolIr` is borrowed
and must remain alive and immutable. Because every embedded stage borrows its
embedded predecessor, a successfully built owner is address-stable and non-copyable;
callers initialize, build, validate/render in place, and free it through the
`sol_mir_concrete_program_*` API.

`sol_mir_concrete_program_build` accepts roots, approved imports, a target, and
either defaults or one complete set of nested stage limits. Successful transactional
publication requires an initialized empty destination. The function builds all stages
in scratch storage, validates the result, and publishes it only on complete success.
Failures leave an initially empty destination empty; a non-empty destination is
rejected unchanged. Invalid arguments/IR/target, unsupported closure, expanding
recursion, symbol collision, resource exhaustion, allocation failure, and internal
failure map to distinct `SolMirConcreteBuildOutcome` values.

The separate concrete validator authenticates exact embedded predecessor links,
invokes transitive stage validation, rejects symbolic residue, proves dense and
unique demand resolution, closes invoke sites and executable blocks, reconstructs
instruction/terminator-to-operation counts, checks cross-stage owner censuses, and
inherits each stage's exact arena-consumption and transitive anti-aliasing checks.
The versionless renderer first validates and buffers one canonical form containing
all seven stages plus complete materialization, representation, layout, operations,
and linkage supplements, then performs one caller-visible output write. It excludes
source paths, pointer values, capacities, and allocator artifacts.

The E6 all-roots test freezes complete counts and resource usages for every stage,
including concrete generic and trait instances, three contract predicates plus one
refinement predicate, four hosted imports, all 14 bodyful callables, and complete
instruction, terminator, failure, and cleanup closure. Focused tests cover repeated
independent lowering, reversed roots and approvals, relocated paths, differing
sufficient limits, malformed owners/embedded links/aliases/slices, exact resource
outcomes, unsupported closure, and symbol collision. Deterministic allocator fault
injection and an aggregate expanding-recursion fixture remain test gaps; they are
not statements that those outcomes are unsupported.

Approved P2.8 validation reported the full Werror suite and the full AppleClang
ASan/UBSan suite each passing 48/48 on macOS. `git diff --check` also passed. This
evidence does not claim leak detection, allocator fault injection, or an aggregate
expanding-recursion fixture.

## Runtime Call, Result, and Failure Conventions (P3.1 Current-Worktree Addendum)

The separate unstable compiler-internal
[SolMirRuntimeConventions](../include/sol/mir_runtime_conventions.h) owner borrows
one completed, validated `SolMirConcreteProgram`, which must remain alive and
immutable. Its owned arenas are disjoint from the owner, from each other, and from
the complete borrowed predecessor chain. Construction into an initialized empty
destination is transactional and bounded; failure leaves it empty, while a nonempty
destination is rejected unchanged. The API initializes/frees the owner, supplies
default limits, builds, independently validates, canonically renders, validates
structured failure records, and maps authenticated entry outcomes to E2 exits.

Signatures cover exact P2 internal callables, approved host requirements, and each
reachable function recipe needed by an indirect call. A receiver, when present, is
always the first slot, followed by formal parameters. Every slot retains its concrete
recipe and owned, shared, or exclusive access. Calls distinguish direct internal,
direct host, and indirect abstract-table targets and retain exact normal and failure
edges. Owned values are consumed on either outcome, shared borrows end on either
outcome, and exclusive values use copy-in with receiver/formal-order writeback only
on normal completion. Results are classified as VALUE (normal payload), UNIT
(normal without a payload), or NEVER (no normal edge). These are compiler-internal
executable conventions, not new source-language types or semantics.

Entries retain the P2 export symbol, binding, callable, source provenance, signature,
and result class. E2 Unit returns map to application/driver status 0; `Int64` values
from 0 through 255 map identically; other integers map to driver status 1 and
`SOL-RUN-002`; authenticated runtime failures map to driver status 1 with their
structured failure code. The owner derives stable full SHA-256 identities from each
exact P2 host requirement or recipe-plus-operation requirement. Canonical host symbols
use `sol.h1.<semantic-id>.<digest>`; create/copy/drop/equal/bound-environment recipe
symbols use `sol.r1.<operation>.<digest>`. Construction and the separate validator
independently reject distinct descriptors that collide by identity or symbol. These
identities do not assign physical ABI layouts, machine addresses, or Wasm function/
table indices.

P3.1 owns canonical source-file/offset failure sites for materialized and predicate
arithmetic, calls, panic, no-match, reached-unreachable, and predicate result checks.
Each site has an independently reconstructed code-compatible mask. Failure records
must match one owned site and its allowed code; panic text and host bytes are bounded
to 191 non-NUL bytes, while other codes carry no detail. After full owner
authentication, failure-record validation and exit mapping are allocation-free and
do not revalidate predecessors. The complete symbolic failure taxonomy is frozen,
but P3.1 itself does not own later allocation/resource cleanup policy; P3.3 subsequently supplies supplemental sites only for uncovered allocation/resource producers, without making execution available merely because codes have names.

The validator is in a separate translation unit and reconstructs signatures, calls,
operands, writebacks, entries, imports, failure sites, collision checks, exact arena
consumption, successful-build work, validation work/scratch, and ownership/alias
guarantees without invoking construction helpers. Rendering validates first, derives
stable references from P2 identities rather than paths or unstable dense IDs, sorts
complete lines canonically, buffers the whole versionless form, and performs one
caller-visible write.

The E6 all-roots census is exactly 18 signatures, 19 signature slots, 18 calls,
24 operands, one writeback, one entry, 52 imports, and 29 failure sites. In field
order `(signatures, signature slots, calls, operands, writebacks, entries, imports,
failure sites, owned bytes, build scratch bytes, build work, validation scratch bytes,
validation work)`, exact usage is
`(18, 19, 18, 24, 1, 1, 52, 29, 16328, 21, 9427, 584692564, 73649839)`.
The default maximum tuple in the same order is
`(4000000, 16000000, 16000000, 64000000, 32000000, 1, 4000000, 32000000,
1073741824, 268435456, 4000000000, 1073741824, 4000000000)`.

Focused tests cover direct internal/host and indirect internal/host table target
logic, receiver-first ordering, every access class, VALUE/UNIT classification,
normal-only ordered writeback, import identity/symbol known answers and collision
injection, failure masks/provenance/details, exact limits and usage, transactional
failure, malformed owners and aliases, stable rendering, allocation-free failure
validation/exits, and E2 Unit/Int64/runtime mappings. Callback execution remains the
upstream P2 unsupported-closure boundary, and source-level `Never` callables remain
rejected with `SOL-TYPE-009`; explicit boundary tests retain those limitations while
classification tests exercise the reachable NEVER production logic. Approved P3.1
validation reported 49/49 normal and 49/49 AppleClang ASan/UBSan tests on macOS.

P3.1 provides no backend or physical ABI, allocation operations, cleanup/unwind
lowering, host marshalling/adapters, handler ABI, complete runtime-lowered program,
compiled `sol build`, or backend-backed `sol run`. Those remain P3.2-P5 work.

## Runtime Value Plans (P3.2a-P3.2f Current-Worktree Addendum)

The separate unstable compiler-internal
[SolMirRuntimeValues](../include/sol/mir_runtime_values.h) owner borrows one
authenticated, address-stable, immutable `SolMirRuntimeConventions` owner, which
must outlive it. It owns exactly one same-ID `SolMirRuntimeRecipeOperations` record
for every concrete representation recipe. Each record contains only the recipe ID,
the create/copy/drop/equal demand mask derived from P2 linkage requirements, and the
matching existing P3.1 import IDs. Bound-environment is stripped from the mask;
host and bound-environment imports are not recipe operation plans. Every demanded
bit has exactly one matching import and every absent bit retains
`SOL_MIR_RUNTIME_NONE`.

Construction is transactional and rejects a nonempty destination unchanged. Limits
cover records, persistent bytes, peak build scratch, build work, validation scratch,
and validation work; null or wholly zero limits select defaults, while partial zero
is invalid. Preflight authenticates P3.1 and checks dimensions, arithmetic, work,
and scratch before persistent allocation. Persistent and validator-scratch fault
injection distinguish allocation failure and leave construction empty or validation
false.

The validator is a separate translation unit. It validates P3.1 first, preflights
headers, capacities, arithmetic, resource usage, and raw ranges before record
traversal, rejects aliases with the owner, P3.1 arenas, the complete P2 predecessor
chain, nested MIR arenas, owning IR arenas, and borrowed strings, then reconstructs
expected masks and import IDs directly from linkage and P3.1 imports. It consumes
the same-ID recipe and relevant-import domains completely and reconstructs exact
usage without invoking builder helpers or constructing another owner.

Canonical rendering validates and buffers before one caller-visible write. It states
that the form is an operation-demand inventory rather than executable operations,
uses stable linkage recipe digests and existing P3.1 import symbols, sorts semantic
lines, and excludes source paths, pointers, capacities, allocator state, and raw
dense IDs. It duplicates no predecessor symbols, digests, layouts, recipes, or
records.

The E6 census is 21 records, of which 17 recipes demand operations: 16 create,
10 copy, 17 drop, and 5 equal. Four host imports remain host imports rather than
recipe plans. Focused tests cover exact pair reconstruction, zero-demand records,
bound-environment exclusion, repeated/reordered-root deterministic builds, exact
and one-below limits, persistent and validator allocation faults, malformed IDs,
masks, import IDs, predecessor data, counts, capacities, usage, aliases, and
zero-write malformed rendering while preserving P3.1 symbols/rendering.

P3.2b extends the inventory with one same-ID target-parameterized plan per recipe:
NONE for uninhabited, zero-sized, scalar, callable, and capability layouts; exact
fixed outer objects for products and sums; and exact Text headers. The E6 Wasm32
census is 21 plans: eight fixed objects totaling 92 bytes, one eight-byte
four-aligned Text header, and twelve NONE plans. The allocation-free O(1) checker
handles zero-length and positive Text payload demand, target/object bounds, checked
cumulative request/byte quotas, and existing P3.1 allocation-limit/failure mapping.
It performs no physical allocation or value operation.

P3.2c adds one same-ID static descriptive ownership plan per recipe, nine sum-variant descriptors, and 12 owned edges, with producer-specific captured-receiver edges where applicable. It freezes the exclusive ownership graph, zero-allocation root/projected move with exact hole/repair semantics, and deterministic depth-first postorder drop semantics. Construction and validation are bounded, transactional, independently reconstructive, and fully anti-aliased; canonical rendering is stable and producer-sensitive without paths or raw IDs. The exact E6 census is 21 ownership plans, 9 ownership variants, 12 owned edges, 3,360 owned bytes, build work 253, and validation work 73,654,894. The existing 21 recipes, 17 demanded drop imports, and P3.2b allocation census are unchanged. Test-only modeling verifies move/hole/repair and recursive postorder behavior; the full Debug suite and focused ASan passed.

P3.2d adds 21 static same-ID copy plans: unreachable 1, forbidden 7, trivial 3, Text 1, product 3, sum 5, and wrapper 1. Test-modeled bounded preflight, private staging, single publication, and postorder rollback specify transactional deep copy with no partially published destination. Cumulative successful allocation quota charges are not refunded on rollback. The exact E6 census is 3,696 owned bytes, build work 275, validation work 73,654,915, and 10 demanded copy imports; prior censuses are unchanged. The full Debug suite passed 50/50 and focused ASan passed. This defines no production copy executor, storage, allocator, or new failure sites.

P3.2e adds 21 static same-ID equality plans: unreachable 1, forbidden 7, trivial 3, Text 1, product 3, sum 5, and wrapper 1. The independent validator completes plan authentication before a comparison is permitted. The bounded test model recursively compares represented values without allocation; the five demanded equal imports are unchanged.

P3.2f closes P3.2 with 21 static E3 host-result plans: one unreachable, 14 forbidden, one each for `Int64`, `Bool`, `Text`, and `Unit`, two `Option`, and zero `Result`; the plans cover four host requirements. Test-modeled checked borrowed-view-to-fresh-owned transfer uses full shape preflight and checker-composed quotas before private staging, postorder rollback, nonrefunded successful quota charges, and single publication. Exact E6 usage is 4,432 owned bytes, build scratch 94, build work 357, validation scratch 584,692,564, and validation work 73,655,017; prior counts are unchanged. The full Debug suite passed 50/50 and focused ASan passed. P3.2a-P3.2f collectively define and test-model allocation, ownership, move/drop, copy, equality, and host-result transfer, not production execution. They add no production executor, adapter, authority handling, or failure sites.

## Runtime Cleanup, Panic, and Failure Policy (P3.3 Current-Worktree Addendum)

`SolMirRuntimeCleanup` is a separate bounded static owner borrowing authenticated immutable P3.1 conventions and P3.2 values. It owns five arenas: same-ID events, ordered actions, outcome transitions, drop paths, and sites. It replays exact P1/P2 CFG cleanup and drop paths, including prerequisite executable lexical scope markers; a narrowly sound immediate callable-field hole repair closes the remaining represented-place case without broadening value semantics.

Actions encode normal-only receiver/formal-order writeback, contract gates, reverse temporary and place drops, lexical-scope and nested-region exits, reverse snapshot and owned-parameter drops, and final failure propagation. Sites retain exact inherited P3.1 provenance; supplemental sites cover only uncovered allocation/resource producers. The producing panic/host detail is captured before cleanup; cleanup cannot replace it. A normal contract/refinement failure becomes primary before cleanup, while an authenticated later contract failure is suppressed and the pre-existing primary failure is retained.

The E6 census is exactly 368 events, 622 actions, 452 transitions, 262 drop paths, and 44 sites; usage is 121792 owned bytes, 246744 build scratch bytes, 72327 build work, 246744 validation scratch bytes, and 54643 validation work. Construction and independent reconstruction are bounded, transactional, and anti-aliased; canonical rendering declares policy without execution. A test-only finite trace models selected transitions/actions/failure detail and does not execute drops, values, hosts, ABI marshalling, storage, or allocation. There is no cleanup executor, backend, or compiled execution. Full Debug validation passed 51/51 and focused ASan passed.

## Runtime Capability and Trusted-Host ABI (P3.4 Current-Worktree Addendum)

`SolMirRuntimeHostAbi` is a separate bounded static, backend-independent owner borrowing authenticated immutable P3.1 conventions, P3.2 values, and P3.3 cleanup policy. It owns capability plans/sources, entry roots, host operations, E3 argument descriptors and shapes, entry requirements, and root-specific grants. Capability provenance reconstructs the authoritative P2 `ROOT_SOURCE`, `BASE_SOURCE`, and `PRIVATE_SOURCE` construction lineage: entry roots remain distinct even when typed alike, derived values retain their exact parent/root closure, and private sources remain metadata rather than host-visible values.

The frozen exact-source correction is preserved: the frontend/IR require the exact declared capability source, so a derived capability cannot substitute for its private source; `PRIVATE_SOURCE` construction is not source-reachable, while authenticated forged private-source substitutions reject. The sealed E3 surface admits only four P3.1 host requirement/import profiles: `Console.write(Text) -> Unit` with `console.write<Self>`, `Arguments.count() -> Int64` with `process.arguments.count<Self>`, `Arguments.get(Int64) -> Option<Text>` with `process.arguments.get<Self>`, and `Configuration.read(Text) -> Option<Text>` with `configuration.read<Self>`. Each retains its exact P3.1 identity, `sol.h1` symbol, receiver provenance, closed effect, ordered formal descriptors, result plan, and failure relation; a fifth lookalike is rejected.

After owner authentication, sealed entry/grant preflight is pure, allocation-free, and non-mutating. It verifies complete root binding, exact root-specific grants, host imports, receiver/formal E3 descriptor closure, P3.2f result-plan eligibility, effects, and failure masks before any model callback, allocation, quota charge, cleanup, token/object creation, or transfer. The separate test-only bounded model borrows data-only argument views, invokes only the exact root-specific fixture callback, transfers a successful result through P3.2f fresh-owned staging/rollback/publication, and selects the existing P3.3 first-failure policy for bounded `HOST_ERROR` detail. It does not expose raw IR, callables, arbitrary pointers, roots/tokens, private sources, layouts, or live values, and adds no adapter, capability token/object, production executor, physical ABI, or failure code/site.

The exact E6 census is 3 capability plans, 3 roots, 4 host operations, 3 arguments, 3 formals, 3 shapes, 0 shape cases, 5 requirements, and 4 grants; usage is 1816 owned bytes, 185 build-scratch bytes, 27868 build work, 77 validation-scratch bytes, and 15916 validation work. Construction and independent reconstruction are bounded, transactional, anti-aliased, and canonically rendered. Final Debug and ASan/UBSan validation each passed 52/52.

## Runtime Exact-Handler ABI (P3.5 Current-Worktree Addendum)

`SolMirRuntimeHandlerAbi` is a separate bounded, backend-independent owner borrowing authenticated immutable P3.1 conventions, P3.2 values, P3.3 cleanup policy, and P3.4 host ABI. The prerequisite commit `c6c6041` narrowly permits internally lowered bodyful root-capability members with receiver-first signatures/contracts; derived/private-source bodyful members remain unsupported, and bodyless members remain imports.

The owner statically retains one same-ID frame plan per reachable P2 concrete handler binding: exact source operation, authority root, normalized closed effect, provider place and value, exact internal provider operation and receiver-first P3.1 signature, lexical parent, and P2 enter/exit marker provenance. Dispatch matches only exact operation/root/effect identities. Every P2 marker and P3.3 `EXIT_HANDLER` cleanup exit is paired and validated; crossed, duplicate, or unbalanced exits reject.

The test-only model uses caller-owned bounded activation storage. It evaluates a provider before pushing, selects the innermost exact match, hides the matched frame plus the complete younger suffix while invoking the retained provider target, and restores that identical suffix in LIFO order before either success or failure continues, including nested older-provider invocation under an already hidden suffix. It introduces no executor, adapter, storage ownership, capability tokens, backend, runtime values, or production execution.

Focused census: 7 frames, 14 exit markers, 8 cleanup exits, 29 marker references, maximum stack depth 7, test work 7, 2152 owned bytes, build scratch 56, build work 7809, validation scratch 56, and validation work 15296. Empty E6 has zero frames, markers, cleanup exits, marker references, owned bytes, and scratch; build work is 22 and validation work is 6358. Construction and independent reconstruction are transactional, bounded, anti-aliased, and canonically rendered. Final Debug and ASan/UBSan validation each passed 53/53.

## Runtime-Lowered Program (P3.6 Current-Worktree Addendum)

`SolMirRuntimeLoweredProgram` is a separate address-stable, backend-independent
owner borrowing authenticated immutable P3.1-P3.5/P2 owners. It owns typed same-ID
complete-graph joins for image/predicate CFG and semantic operations; P3.1
calls/signatures/imports/failures; P3.2 recipe/value demands; P3.3 cleanup/failure;
P3.4 requirements, distinct root grants, and incidence; and P3.5 frames/exits. A
separate hostile validator authenticates predecessor chains, reconstructs the closure,
and proves full transitive anti-aliasing before typed traversal. Draft-first build,
validation, and rendering have exact resource limits; rendering validates first,
buffers the complete typed semantic canonical form, and performs one caller-visible
write. This adds no executor, physical ABI, Wasm, adapter, allocator implementation,
CLI/build artifact, or tokens.

The frozen E6 census is image 471/65/64/64/64/65; predicate 4/6/4/0/0/0/4;
semantic plans 75, calls 12, signatures 12, imports 48, recipes 20, value plans 20,
recipe demands 295, cleanup failures 1426, host requirements/grants/incidences 5/4/5,
and handlers 0/0/0. Usage is 481520 owned bytes, 482098 build-scratch bytes,
1175568 build work, 5466 validation-scratch bytes, 842837 validation work, 11195392
render bytes, and 11216232 render-scratch bytes. `p35` is image 198/16/10/10/10/16,
predicate all zero, semantic/calls/signatures/imports 28/5/7/10, recipes/value
plans/demands 7/7/54, cleanup 401, host 0/0/0, handlers 7/14/8, with
140960/141174/304979/1616/250947/3310592/3316032 owned/build-scratch/build-work/
validation-scratch/validation-work/render-bytes/render-scratch bytes. `p35_reordered`
uses 253227 validation work because authenticated borrowed path text is exact-work
input; canonical rendered bytes remain path-independent. Conformance includes generic,
copy, and indirect fixtures. Final Debug and ASan/UBSan validation each passed 54/54.

## Scalar Wasm Emission (P4.2 Current-Worktree Addendum)

P4.2 is an internal opt-in Binaryen 129/Wasmtime 49.0.1 emitter over authenticated P3.6 whole scalar direct-call closures. It emits `Int64`, `Bool`, and Unit constants and parameters; SSA values and block parameters including parallel edges; whole-local and temporary lifetime; arbitrary CFG dispatch and loops; infallible and checked unary, binary, and compound scalar operations without Wasm traps; direct ordinary internal owned-scalar calls; VALUE/Unit normal and failure dispatch; and returns. It consumes only the authenticated scalar subset, rather than interpreting source IR or inventing a general runtime representation.

The linker mapping retains exact P2 `sol.i1` internal definitions. Only exact P2 `sol.e1` functions are externally callable exports; no `sol.i1` function exports exist. The module additionally exports exactly two explicitly unstable mutable diagnostic globals for private packet observation, not as a stable/public ABI. A private entry wrapper resets those globals once. The convention is private and provisional: it is neither a stable/public physical ABI nor a host adapter. Provenance is canonical package-relative `sol.p42.provenance.v1`; nested leaf failure packet code and site survive callers. Static construction rejects direct-call cycles and call chains longer than 64. P4.2 does not emit runtime depth code 7; P4.4 owns runtime checks. P3.3's `LOCAL_OR_PENDING` prerequisite authenticates a future caller-local code-7 packet or an unchanged pending callee packet only for ordinary image direct-function `INVOKE`, excluding host, indirect, capability, and predicate calls.

The representative nested-overflow fixture uses `(functions, blocks, edges, values, locals, generated nodes, provenance records, work bytes, scratch bytes, owned bytes, output bytes)` = `(4, 7, 4, 7, 13, 146, 7, 4115, 1703, 1703, 1504)` and SHA-256 `6a75a3627de544741885fcd12aad0ef27237e9d7d9f3c616f8ceb181f115ddb4`. Limits are exact backend limits: every field has exact, one-below, and partial-zero coverage; backend-owned allocation sweeps and rollback are covered. Binaryen/Wasmtime private allocations are outside quotas, while serialized output bytes are metered. The narrow P3.2 prerequisite adds only the no-import/nonzero-record scratch-allocation event: exact work 23 succeeds and one-below work 22 exhausts; import-bearing E6 frozen build work remains 357. This is not a broader P3.2 semantic change.

Final verification on Darwin arm64 was OFF Debug Werror 54/54, ON Debug Werror 57/57, and ON RelWithDebInfo ASan/UBSan Werror 57/57, plus `git diff --check` and independent final review. Linux execution was not run. P4.2 excludes represented values, places/projections, indirect calls/callbacks/receivers/writeback, patterns/propagation; P4.4 owns contracts/refinements/snapshots/general cleanup/handlers/runtime checks/unwind; P4.5 owns the host adapter and E6 execution; P5 owns artifacts and `sol build`.

## Represented Wasm Emission (P4.3 Current-Worktree Addendum)

P4.3 is the private opt-in Binaryen 129/Wasmtime 49.0.1 Wasm32 represented emitter over authenticated P3.6. Canonical package-relative provenance is `sol.p43.provenance.v1`. It covers bounded Text; finite fixed products/tuples, sums, distinct wrappers, and Unit fields; constructors/projections; deep copy, equality, and logical cleanup; guard-free total patterns and Copy extraction; Option/Result propagation; private sentinel `funcref` table callbacks with exact unbound internal callbacks; whole-local scalar `inout` callback normal-only writeback; immediate shared/exclusive `Int64` methods; and exact callable-plus-Text product projected moves, conditional holes, repair/reopen, intact whole-root moves, and hole-aware cleanup.

Deterministic bump memory, exact runtime request/byte quotas, code 5 resource failure before code 4 physical allocation failure with canonical supplemental sites, exact resource/fault rollback, deterministic module validation, bounded/capped raw parsing, and source-backed mutations are covered. Bound stored trait-method values remain intentionally unsupported by the language immediate-method rule; broader represented shapes and callable signatures fail closed. P4.3 adds no host/capability adapter or E6 execution and remains a private provisional convention, not a stable public ABI.

Final validation on Darwin arm64 was OFF Debug Werror **54/54** and ON Debug Werror **58/58**; the current ON ASan/UBSan Werror represented-emitter test and focused MIR tests passed. No current full ASan suite or Linux execution is claimed. P4.4 owns general runtime checks, non-total/guarded-match failure, contracts/refinements/snapshots, complete cleanup/unwind/handlers/provider dispatch, and runtime depth/step codes 6/7; P4.5 owns host adapter/E6 execution; P5 owns artifacts and `sol build`.

## Remaining Boundary

P2 and P3 are complete for the frozen E6 profile. They select no physical ABI,
concrete table indices, wrappers/adapters, Wasm linkage/emission or execution,
runtime executor or allocator implementation, build artifact, or public format. Those boundaries, reproducible
artifacts, and interpreter/Wasm differential execution remain later tracks. No
internal census is an empirical maintenance advantage, user-facing performance
guarantee, or proof of application behavior. The proposed M experiment is not
implemented by these compiler owners. P4.1 through P4.3 are complete; P4 remains
open and P4.4 remains unchecked. The bounded f1a metadata checkpoint and accepted,
published f1b leaf-execution slice do not close it. See the [f1a handoff](p4.4f1a-handoff.md),
[f1b handoff](p4.4f1b-handoff.md), and
[external sole project cursor](../../sol-lang/.git/agent-handoffs/p44-f1e/project-execution-cursor.md).
Consult [TODO.md](../TODO.md) for checkpoint navigation and
[the analysis](project-analysis.md) for the experiment rationale.

## S1 publication candidate and bounded evidence — October 21, 2026

The original WIP remains in `/Users/jsobotka/code/sol-lang-test-speed` and is preserved
by the external receipt at
`/Users/jsobotka/code/sol-lang/.git/agent-handoffs/p44-f1e/s1-publication/preservation/`
(receipt SHA-256 `f38bd6f0f91c24e127f18f2af4821cf084ae055f21aa50354cd445d64a3dfc26`). The
separate candidate is based on `dfceed6b3e6630271cdc6ba02cb011314f1b31f1` at
`/Users/jsobotka/code/sol-lang-p44f1e-s1-publication`, branch
`p44-f1e-s1-publication`. Candidate test SHA-256 is
`fbb0806bc0cd52cdb15f5ea0abf031478c2e4939b8cfa4457b821b9f526c6ceb`; exact production
and fixture pins are `49a241484fcc5a31de2b7512704c803d44c5c86add69a84df5b049d3b02cc988`
and `cdaef751298f9cda6147868952d7e00d1cb061a6879064c5aaa921c290f47db9`. It is
ledger-assembled from the baseline plus the approved selected source closure; it is
not the full 950 or 695 test snapshot. The 215 selected new 950 functions are retained,
with a bounded dispatch replacement; eight unaccepted WIP implementations are
excluded. This candidate's results are separately attributed and do not inherit the
original 950 run or claim exact byte identity to the unavailable full `695e01…`
snapshot. CORE's 9 ON + 9 SAN results remain accepted at their own pins, with only
review-approved bounded continuity used here; no fresh candidate SAN or nine-case
replay is claimed.

Independent S1 source and evidence reviews approve this exact candidate for the
bounded S1 checkpoint. One fresh configure/build passed, followed by the two authorized
native semantic cases:

| Candidate selector | Active | Complete | Distinct outer |
| --- | ---: | ---: | ---: |
| `represented.ensures-calls.discovery.traced` | 2.670197250s | 2.714858500s | 2.733556625s |
| `represented.ensures-calls.retained.traced` | 1.061952667s | 1.076693625s | 1.095858458s |

Each passed once within active ≤3s and complete/outer <5s. Candidate executable/object
SHA-256 are `e2734001faf16002ddbc59f79b40fd2ecfbd318bbecafd5f35d867dc11d551e8` /
`3726447a8ab7fd90eff8211416649899b615807aa1bd78af57b0267331f542d1`. Thirteen metadata
invocations passed: seven output successes (manifest plus six lists: shards, old
A–D, new E) and six expected rc2/empty-output negatives. Evidence review clarifies
the immutable chronology: configure/build first, then a no-child pre-semantic
bytecode correction with no rebuild, then the semantic and metadata invocations. Raw
validation report/receipts are not rewritten.

At the S1 publication snapshot, the candidate listed/defaulted nine real E selectors
(eight CORE and traced-X discovery) and added only `contracts.ensures-call-core-s1`.
Its static inventory was 77
unique controls / 118 expanded control tuples / 4,882 unchanged old fault IDs / 29
sweeps / 24 runners / 50 shards / 5,000 ordered tuples (baseline 4,999 plus one new
control). No new fault coverage, W/plain discovery, or future49 obligation was
registered in that snapshot. At that time X=344 was observed native evidence, not
frozen `N_X`; `N_W`, `N_X`, `M`, and digests remained unknown, not zero pins. Reconcile future S2/S3 additions against
the published 77-control/5,000-tuple base; do not infer 79 or guess pins. The original
seven-path WIP remains preserved by the receipt above.

**Historical S2 source-only preparation — October 21, 2026:** S1 is COMPLETE and published as
`e93d375e2e9f59d9a819ad470770e741a1511f4a`; `origin/master` was verified at the same
commit. At this source-only snapshot, S2 was the sole active step for initial captures,
source classification, cross-mode comparison, and pin freeze. This worktree change added
only the W/plain expression before the existing X expression and left the count/digest
pins at zero. No W runtime observation existed yet. The exact fresh initial set was
ON/W plus SAN/W and SAN/X; the S1 candidate native-X observation was an approved carry.
That status snapshot is superseded by the accepted S2 evidence below; historical receipts
and reviews remain unchanged.
Current E is ten source-listed keys (eight CORE, W, X); the static registration
inventory remains 77 unique controls /118 expanded controls /4,882 fault tuples /5,000
ordered tuples /24 runners /50 shards /29 sweeps. No metadata/default/fault/sweep
runtime or new CLI output was claimed in that source-only preparation. No S2 configure,
build, semantic case, or runtime was performed in that earlier phase. See the
historical S2 status below for its later authorized evidence and publication boundary.

## Historical status addendum — October 21, 2026 (S2 technical acceptance; before publication)

At this earlier October 21 snapshot, S1 was complete and published as
`e93d375e2e9f59d9a819ad470770e741a1511f4a`; `origin/master` was verified at that
commit. CORE remains accepted at its own test pin `695e01…` for exactly 9 ON + 9 SAN
runs, not as a fresh candidate-wide replay. S2's technical inventory-freeze exit is
independently accepted by
`/Users/jsobotka/code/sol-lang/.git/agent-handoffs/p44-f1e/s2-inventories/final-evidence-review.md`
(SHA-256 `07b210d5a602734f8d920fcaabaef2b86bad0b765b0d86c9777c18c707564ecf`).
At that snapshot the sole ACTIVE item was S2 publication—separate documentation/staged-tree review,
Astra alignment, coordinator commit, push, and verification. No S2 publication commit
or push was yet claimed. At that snapshot [TODO.md](../TODO.md) held the cursor;
S3 was blocked until verified S2 push and approved helper review, S4 on S3, and S5
on S1–S4. That checkpoint is superseded by the accepted S3 evidence record above;
it is not current execution authority.

### Frozen S2 source and inventory identities

The four fresh post-pin captures used test source SHA-256
`dce792c20186e86a098f52360ce9de454afa8bcb20491574758f746c3b869872`, unchanged
production SHA-256
`49a241484fcc5a31de2b7512704c803d44c5c86add69a84df5b049d3b02cc988`, and fixture
`cdaef751298f9cda6147868952d7e00d1cb061a6879064c5aaa921c290f47db9`. The frozen
inventory is `s2-inventories/pin-implementation/frozen-inventory.json`; the final
review independently approves it. No normalized-line digest is used: the row
serialization preserves actual current source lines.

| Profile | N | Actual-current-line SHA-256 | Module length / SHA-256 |
| --- | ---: | --- | --- |
| W | 257 | `defb27c27204ea4ddb0d51ccc088d202a3ed94ae4627213057868cea209d1c1d` | 27,789 / `9a455f12439190a2ea41116907c24967d3490988c16fefd9c0f4f743be79cb87` |
| X | 344 | `78c31c56e9447ea9654d4345b7874da167a5d2cb39fd926b80e7ccbeb4cb261a` | 75,850 / `352897de96bbe643b71e0625be6119dae0393517cd3948f48bb8f5127cf5f3a9` |

`M=601` is only the arithmetic sum of W and X; no M registration or fault coverage
was added. The independent review parsed all 1,202 initial and 1,202 post-pin ordered
ten-field observations, found each list exactly `1..N`, and recomputed canonical
actual-line digests. Each fresh list equals its accepted initial profile list and
the other mode. The source classifier independently covers every physical request,
group, prefix and sibling suffix. The retained frozen inventory contains the full
ordered rows; they are referenced rather than duplicated here.

| Profile | All 15 `Usage` fields (emitted order) |
| --- | --- |
| W | `[8,38,30,46,103,3010,0,0,0,0,160,71601,34445,38029,27789]` |
| X | `[10,53,41,59,130,17836,0,0,0,0,214,92940,45736,86090,75850]` |

Each selected-case launch asserted two independent owners, full ordered-record and
complete module-byte equality within that launch, `Usage` equality, complete source and
witness equality, four actual seven-render sets, and both real closes. The initial
fresh captures were ON/W, SAN/W, and SAN/X; initial ON/X was the approved S1 candidate
carry from test `fbb0806…`, binary `e2734001…`, with its own two-owner within-mode
assertions. Post-pin validation then used exactly four fresh repeats: ON/W, ON/X,
SAN/W, SAN/X. Cross-mode module identity is length+SHA-256, not raw-byte equality
(cross-mode raw module blobs were not retained); within-mode full byte equality is
assertion-backed.

### Actual bounded build and capture timings

Seconds below are the accepted final-review table values (rounded to six decimals),
not caps or estimates. Active direct limits were 3s native / 27s SAN; complete limits
were strictly under 5s / 30s after closure.

| Fresh selector | Active | Complete after witness close | Distinct outer |
| --- | ---: | ---: | ---: |
| ON/W | 2.075130 | 2.127275 | 2.148134 |
| ON/X | 2.439430 | 2.455822 | 2.476123 |
| SAN/W | 8.606935 | 8.655070 | 8.700591 |
| SAN/X | 10.889483 | 10.935070 | 10.979267 |

The two fresh target builds passed (zero configure commands in the post-pin phase):
ON inner-complete 2.369486s / distinct parent outer 2.476360s; SAN inner-complete
6.971162s / parent outer 7.180182s. The separate initial capture phase used one SAN
configure, two target builds, and three fresh selectors plus the approved ON/X carry.
Across the post-pin phase there were zero retries, zero configure, two builds, and four
fresh semantic children. SAN used ASan/UBSan halt-on-error with
`detect_leaks=0`; leak detection was disabled and no leak-checking claim follows.

The source-listed E set is ten keys (eight CORE, W/plain, X/traced). Static source
inventory remains 77 unique controls / 118 expanded control tuples / 4,882 old fault
tuples / 5,000 ordered tuples / 24 runners / 50 shards / 29 sweeps. The four historical
A–D lists are unchanged by source proof. These are not fresh default/list/manifest CLI
results: no default run, metadata/list invocation, or old fault sweep ran in S2, and
no new fault registration was made. S3's `W1..257` and `X1..344` ordinals in both
modes remain unlaunched; helpers are not yet accepted. Source-classified would-fail
paths are not injected failures. The historical 78-control /`(4,882+M)` plan and
future 49 nonfault obligations must be reconciled against the current checkpoint;
do not infer a final 79-control count.

The prior 950 source's 2.839310250s native-X active timing and candidate S1's
2.670197250s timing remain separate historical observations. Original 950 slot/outer
FAIL receipts, the `6e10fdfe…` cutoff, old-source W=257 provisional observation,
initial evidence, citation erratum (catalog rows 1–2 producer return is line 6007;
6203 is the Kahn return), and all prior non-approvals remain immutable; none is
rewritten by the later frozen inventory. Broad P4/P4.4 stays open, whole-suite speed
remains unmet, R-AUTH and held resource ±1 checks remain open, and host/E6/P4.5/P5
remain deferred. Full f1e and S3–S5 are not accepted.
