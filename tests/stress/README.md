# Stress Baselines

The runner generates deterministic sparse generic-call, repeated validated-execution,
and multi-file package workloads. Functional outcomes and generated SHA-256 values
are portable gates. The checked wall-time ceilings are intentionally broad and are
used as hard per-case safety deadlines on **every** run (currently 30s, unchanged).
The scheduled Ubuntu release workflow also retains `--enforce-time` wall-time
checking. A cutoff fails; it is not a skipped or completed workload.

Run locally from the repository root:

```sh
python3 tests/stress/run_stress.py \
  --sol build-release/sol \
  --baseline tests/stress/baselines.json \
  --work-dir build-release/stress
```

Generation (1000 declarations /250 tests /128 files), digest verification,
compiler invocation and expected-summary verification report flushed phases on
stderr. The shared supervisor streams both child streams immediately, keeps exact
file-backed captures for bounded-memory substring checks, and emits honest 1s
liveness when the compiler is opaque. Sanitizer environment is inherited; locale
remains C. No generator, count, digest or expected summary is changed by logging.

`--print-hashes` still writes **only JSON to stdout**, with generation progress on
stderr. For one bounded diagnostic case, add `--case generic_chain` (or
`validated_tests` /`package`); this is explicitly PARTIAL, with other cases unrun.
The default continues to generate and run all three unchanged workloads.

Baseline hash or threshold changes require review and an explanation of the changed
generator, semantics, or expected host performance.
