# Stress Baselines

The runner generates deterministic sparse generic-call, repeated validated-execution,
and multi-file package workloads. Functional outcomes and generated SHA-256 values
are portable gates. The checked wall-time ceilings are intentionally broad and are
enforced only by the scheduled Ubuntu release workflow.

Run locally from the repository root:

```sh
python3 tests/stress/run_stress.py \
  --sol build-release/sol \
  --baseline tests/stress/baselines.json \
  --work-dir build-release/stress
```

Baseline hash or threshold changes require review and an explanation of the changed
generator, semantics, or expected host performance.
