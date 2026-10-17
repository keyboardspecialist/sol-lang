#!/usr/bin/env python3
"""Serial, source-pinned DIAGNOSTIC audit. Never a replacement acceptance suite.

All configured names are retained, including unselected/unrun names. Resume is
explicit and requires identical source, diff, configuration and artifact pins.
Logs and observations are file-backed; only the supervisor's bounded history is
included inline. No expected product failure is inferred from an audit cutoff.
"""
import time
INVOCATION_START = time.monotonic()

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent / "support"))
import run_with_progress as supervisor

ROOT = Path(__file__).resolve().parents[1]
INTEGRATION_NAMES = {"compilation", "package", "ir", "interpreter", "inspection",
                     "test_cli", "run_cli", "effects_cli", "inspection_cli", "e6_conformance"}


class BudgetExpired(Exception):
    pass


class Budget:
    def __init__(self, deadline):
        self.deadline = deadline
        self.stage = "startup"

    def remaining(self, reserve=0):
        remaining = self.deadline - time.monotonic() - reserve
        if remaining <= 0:
            raise BudgetExpired(self.stage)
        return remaining

    def check(self):
        self.remaining()


def check(budget):
    if budget is not None:
        budget.check()


def environment_identity(environment):
    """All keys, no raw values (including secrets), no volatile exclusions."""
    values = {key: hashlib.sha256(os.fsencode(value)).hexdigest()
              for key, value in sorted(environment.items())}
    return dict(values_sha256=values, sha256=hashlib.sha256(
        json.dumps(values, sort_keys=True, separators=(",", ":")).encode()).hexdigest())


def child_environment(environment):
    # The shared supervisor changes exactly this one key before Popen. Pin both
    # the supplied snapshot AND the effective child snapshot, including depth.
    child = dict(environment)
    try:
        depth = min(100, max(0, int(child.get("SOL_TEST_MONITOR_DEPTH", "0"))))
    except ValueError:
        depth = 0
    child["SOL_TEST_MONITOR_DEPTH"] = str(depth + 1)
    return child


def sha(path, budget=None):
    digest = hashlib.sha256()
    check(budget)
    with open(path, "rb") as stream:
        while True:
            check(budget)
            chunk = stream.read(1024 * 1024)
            check(budget)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def git(*args, budget=None, environment=None):
    return captured(["git", *args], cwd=ROOT, budget=budget, environment=environment)


def captured(argv, timeout=10, cwd=ROOT, budget=None, environment=None):
    check(budget)
    if budget is not None:
        timeout = min(timeout, budget.remaining())
    with open(os.devnull, "wb") as sink, supervisor.run(
            argv, name="audit-discovery", timeout=timeout, cwd=cwd,
            stdout=sink, stderr=sys.stderr.buffer, monitor=sys.stderr.buffer,
            capture=True, env=environment) as result:
        check(budget)
        if result.outcome == "TIMEOUT":
            raise BudgetExpired(budget.stage if budget is not None else "metadata")
        if result.returncode:
            raise RuntimeError("discovery failed: " + repr(argv) + ": " + str(result.returncode))
        # Discovery JSON/manifest is finite metadata, not test output.
        data = result.stdout.read(16 * 1024 * 1024 + 1)
        check(budget)
        if len(data) > 16 * 1024 * 1024:
            raise RuntimeError("configured metadata exceeded 16 MiB")
        return data


def source_pins(budget=None, environment=None):
    def command(*args):
        return git(*args, budget=budget, environment=environment)
    names = set(command("ls-files", "-z").decode().split("\0"))
    names.update(command("ls-files", "--others", "--exclude-standard", "-z").decode().split("\0"))
    files = {}
    for name in sorted(names - {""}):
        check(budget)
        path = ROOT / name
        # Deleted tracked files and symlink targets are both part of identity.
        files[name] = {"sha256": sha(path, budget) if path.is_file() else None,
                       "symlink": os.readlink(path) if path.is_symlink() else None}
    return dict(head=command("rev-parse", "HEAD").decode().strip(), files=files,
                tracked_diff_sha256=hashlib.sha256(command("diff", "--binary", "HEAD")).hexdigest())


def pins(build, inventory, ctest, budget=None, environment=None):
    environment = dict(os.environ) if environment is None else environment
    check(budget)
    cache_path = build / "CMakeCache.txt"
    config = {}
    for line in cache_path.read_text().splitlines():
        check(budget)
        match = re.match(r"([^/#][^:]*):[^=]+=(.*)$", line)
        if match:
            key, value = match.groups()
            if key.startswith(("SOL_", "CMAKE_", "BUILD_TESTING", "Python3_", "_Python3_")):
                config[key] = value
    paths = {cache_path, Path(ctest), Path(sys.executable)}
    for path in build.rglob("CTestTestfile.cmake"):
        check(budget)
        paths.add(path)
    for file in ("build.ninja", "Makefile", "sol", "sol.exe", "sol_cli_io_helper"):
        if (build / file).is_file():
            paths.add(build / file)
    for test in inventory:
        check(budget)
        for arg in test.get("command", []):
            path = Path(arg)
            if path.is_absolute() and path.is_file():
                paths.add(path)
    # Linked shared dependencies are not necessarily visible in CTest argv.
    for key, value in config.items():
        if ("LIBRARY" in key or key.endswith("COMPILER")) and Path(value).is_file():
            paths.add(Path(value))
    artifacts = {str(path.resolve()): sha(path, budget) for path in sorted(paths)}
    manifests = {}
    represented = build / ("sol_wasm_represented_tests.exe" if os.name == "nt" else "sol_wasm_represented_tests")
    if represented.is_file():
        for option in ("--manifest", "--list-shards"):
            check(budget)
            data = captured([str(represented), option], cwd=build,
                            budget=budget, environment=environment)
            manifests[option] = hashlib.sha256(data).hexdigest()
    return dict(source=source_pins(budget, environment), build=str(build), configuration=config,
                runtime_environment=environment_identity(environment),
                effective_child_environment=environment_identity(child_environment(environment)),
                artifacts_sha256=artifacts, manifests_sha256=manifests, inventory=inventory)


def target(name):
    # Inventory policy from audit-inventory.md, not a relaxed timeout policy.
    integration = name in INTEGRATION_NAMES or name.startswith(("mir", "wasm", "progress_"))
    return 5.0 if integration else 1.0


def save(path, receipt):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    with open(temporary, "x") as stream:
        json.dump(receipt, stream, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


class Delivery:
    def __init__(self, raw, events, channel, start, live):
        self.raw, self.events, self.channel, self.start, self.live = raw, events, channel, start, live
        self.last = start
        self.gap = 0.0

    def write(self, data):
        now = time.monotonic()
        self.gap = max(self.gap, now - self.last)
        self.last = now
        self.raw.write(data)
        self.events.write(json.dumps(dict(elapsed=now - self.start, channel=self.channel,
                                          bytes=len(data))) + "\n")
        self.live.write(data)

    def flush(self):
        self.raw.flush()
        self.events.flush()
        self.live.flush()


def audit(options, budget, environment, state):
    started = options._audit_started
    checkpoint = options._receipt_path
    budget.stage = "resume-environment"
    budget.check()
    output = options.output.resolve()
    if output.exists() and options.resume is None:
        raise ValueError("output exists; use --resume explicitly or choose a new receipt")
    if output.exists() and options.resume is not None and output != options.resume.resolve():
        raise ValueError("output exists and is not the explicitly resumed receipt")
    build = options.build_dir.resolve()
    old = None
    if options.resume is not None:
        old = json.loads(options.resume.read_text())
        budget.check()
        if (old.get("version") != 2 or old.get("pins_stable") is not True
                or old.get("pins", {}).get("runtime_environment") != environment_identity(environment)):
            raise ValueError("resume rejected: environment/schema/pins differ or unstable")
    budget.stage = "discovery"
    ctest = shutil.which("ctest", path=environment.get("PATH", os.defpath))
    if ctest is None:
        raise ValueError("ctest not found")
    discovered = json.loads(captured([ctest, "--test-dir", str(build), "--show-only=json-v1"],
                                    budget=budget, environment=environment))
    budget.check()
    inventory = discovered["tests"]
    names = [test["name"] for test in inventory]
    if len(names) != len(set(names)):
        raise ValueError("duplicate configured test names")
    selected = options.names if options.names is not None else names
    if len(selected) != len(set(selected)):
        raise ValueError("duplicate selected names would overwrite per-test logs")
    if set(selected) - set(names):
        raise ValueError("unknown exact names: " + repr(sorted(set(selected) - set(names))))
    receipt = dict(version=2, classification="NOT_ACCEPTANCE", status="INCOMPLETE", complete=False,
                   inventory_known=True, resume_verified=False, pins_stable=False,
                   pins=None, runs=[], tests={name: dict(status=("NOT_RUN_SUITE_BUDGET" if name in selected
                                                               else "NOT_RUN_SELECTION"), attempts=[],
                        target_seconds=target(name)) for name in names},
                   speed_targets=dict(unit_seconds=1, integration_seconds=5, suite_seconds=60),
                   note="Logging is not a speed fix. Cutoffs do not establish expected outcomes.")
    state["receipt"] = receipt
    save(checkpoint, receipt)
    budget.stage = "initial-pins"
    identity = pins(build, inventory, ctest, budget, environment)
    budget.check()
    receipt["pins"] = identity
    if old is not None:
        if old.get("pins") != identity:
            raise ValueError("resume rejected: source/configuration/binary/manifest/inventory pins differ or unstable")
        receipt = old
        receipt["resumed_from"] = dict(path=str(options.resume.resolve()), sha256=sha(options.resume, budget))
        receipt["resume_verified"] = True
        state["receipt"] = receipt
        if options.names is None:
            selected = [name for name in names if receipt["tests"][name]["status"].startswith("NOT_RUN")]
    run_id = uuid.uuid4().hex
    log_dir = output.parent / (output.stem + "-" + run_id)
    log_dir.mkdir(parents=True)
    cycle = dict(id=run_id, selected=selected, suite_budget_seconds=options.suite_seconds,
                 per_test_seconds=options.per_test_seconds, argv=sys.argv, cwd=str(Path.cwd()),
                 logs=str(log_dir), start_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    receipt["runs"].append(cycle)
    for name in selected:
        receipt["tests"][name]["status"] = "NOT_RUN_SUITE_BUDGET"
    receipt["pins_stable"] = False
    save(checkpoint, receipt)
    # Fixed tiny workload; measure launch/wrapper overhead separately from product
    # timings. This is NOT subtracted from product target comparisons.
    tiny = [sys.executable, "-c", "pass"]
    budget.stage = "overhead"
    overhead = {}
    before = time.monotonic()
    # Fixed immutable tiny child: no descendants. The containing worker's owned
    # group is also bounded by the parent deadline if this syscall ever stalls.
    subprocess.run(tiny, check=True, timeout=min(5, budget.remaining()), env=environment)
    budget.check()
    overhead["direct_seconds"] = time.monotonic() - before
    with supervisor.run(tiny, name="audit-wrapper-overhead", timeout=min(5, budget.remaining()),
                        env=environment) as result:
        budget.check()
        if result.returncode:
            raise BudgetExpired("overhead")
        overhead["supervised_seconds"] = result.elapsed
    overhead["difference_seconds"] = overhead["supervised_seconds"] - overhead["direct_seconds"]
    cycle["wrapper_overhead"] = overhead
    for index, name in enumerate(selected):
        budget.stage = "probe:" + name
        budget.check()
        properties = {prop["name"]: prop["value"] for prop in inventory[names.index(name)].get("properties", [])}
        # CTest can return zero without executing a disabled/skipped child. Do
        # not turn that into PASS. Such policies need a richer CTest result API;
        # retain the name explicitly instead of guessing from console strings.
        if properties.get("DISABLED") or any(key.startswith("SKIP_") for key in properties):
            receipt["tests"][name]["status"] = ("NOT_RUN_DISABLED" if properties.get("DISABLED")
                                               else "NOT_RUN_SKIP_POLICY")
            save(checkpoint, receipt)
            continue
        # Reserve existing owned cleanup (<=3s) and receipt/pin finalization. The
        # suite budget includes discovery and overhead, not just child execution.
        remaining = budget.remaining(reserve=min(1.0, options.suite_seconds / 10))
        if remaining <= 1.0:
            break
        probe_budget = min(options.per_test_seconds, remaining)
        # The per-probe budget covers owned cleanup too, not just active work.
        # Small synthetic cutoffs retain a positive window; product probes keep
        # the full existing <=3s cleanup reserve inside their <=30s wall budget.
        cleanup_reserve = min(3.0, probe_budget / 4.0)
        timeout = probe_budget - cleanup_reserve
        argv = [ctest, "--test-dir", str(build), "--verbose", "--output-on-failure",
                "--no-tests=error", "-j", "1", "-R", "^" + re.escape(name) + "$"]
        print("AUDIT test={}/{} name={} remaining_inventory={} ceiling={:.3f}s NOT_ACCEPTANCE".format(
            index + 1, len(selected), name, sum(not row["attempts"] for row in receipt["tests"].values()),
            timeout), file=sys.stderr, flush=True)
        stem = "{:03d}-".format(names.index(name)) + re.sub(r"[^A-Za-z0-9_.-]", "_", name)
        before = time.monotonic()
        semantic_path = log_dir / (stem + "-observations.jsonl")
        delivery_path = log_dir / (stem + "-deliveries.jsonl")
        raw_path = log_dir / (stem + ".log")
        with open(raw_path, "wb") as raw, open(delivery_path, "w") as deliveries, \
                open(semantic_path, "w") as observations:
            sinks = [Delivery(raw, deliveries, channel, before, sys.stdout.buffer)
                     for channel in ("stdout", "stderr", "monitor")]
            def observe(record):
                observations.write(json.dumps(record) + "\n")
                observations.flush()
            with supervisor.run(argv, name="audit:" + name, timeout=timeout, cwd=ROOT,
                                stdout=sinks[0], stderr=sinks[1], monitor=sinks[2],
                                on_observation=observe, env=environment) as result:
                status = "AUDIT_TIMEOUT" if result.outcome == "TIMEOUT" else \
                    "INTERRUPTED" if result.outcome == "INTERRUPTED" else \
                    "PASS" if result.returncode == 0 else "FAIL"
                attempt = dict(cycle=run_id, argv=argv, cwd=str(ROOT), monotonic_elapsed=result.elapsed,
                               outcome=status, classification="NOT_ACCEPTANCE", exit=result.returncode,
                               cutoff_seconds=timeout, max_delivery_gap=result.max_delivery_gap,
                               probe_budget_seconds=probe_budget, cleanup_reserve_seconds=cleanup_reserve,
                               probe_budget_exceeded=result.elapsed > probe_budget,
                               cutoff_reason=("SUITE_BUDGET" if probe_budget < options.per_test_seconds
                                              else "PER_TEST_BUDGET") if status == "AUDIT_TIMEOUT" else None,
                               observations=result.observations, history_limit=supervisor.HISTORY_RECORDS,
                               complete_observations=str(semantic_path), delivery_log=str(delivery_path),
                               raw_log=str(raw_path), target_seconds=target(name),
                               target_comparison="UNKNOWN_CUTOFF" if status in ("AUDIT_TIMEOUT", "INTERRUPTED")
                                   else "WITHIN" if result.elapsed < target(name) else "OVER",
                               cleanup_error=result.cleanup_error)
                receipt["tests"][name]["attempts"].append(attempt)
                receipt["tests"][name]["status"] = status
                save(checkpoint, receipt)
                if status == "INTERRUPTED":
                    break
    budget.stage = "final-verification"
    budget.check()
    receipt["pins_stable"] = pins(build, inventory, ctest, budget, environment) == identity
    budget.check()
    cycle["elapsed_seconds"] = time.monotonic() - started
    cycle["suite_budget_exceeded"] = cycle["elapsed_seconds"] > options.suite_seconds
    receipt["complete"] = all(row["status"] in ("PASS", "FAIL") for row in receipt["tests"].values())
    receipt["status"] = "COMPLETE_DIAGNOSTIC" if receipt["complete"] else "PARTIAL"
    receipt["all_pass"] = all(row["status"] == "PASS" for row in receipt["tests"].values())
    receipt["counts"] = {status: sum(row["status"] == status for row in receipt["tests"].values())
                         for status in sorted({row["status"] for row in receipt["tests"].values()})}
    save(checkpoint, receipt)
    budget.check()
    print("AUDIT " + receipt["status"] + " NOT_ACCEPTANCE " + json.dumps(receipt["counts"]),
          file=sys.stderr, flush=True)
    return 0 if receipt["all_pass"] and receipt["pins_stable"] else 1


def incomplete(receipt, stage):
    receipt.update(status="INCOMPLETE", complete=False, all_pass=False,
                   pins_stable=False, nonresumable=True, incomplete_stage=stage)


def worker(options):
    budget = Budget(options._audit_deadline)
    environment = dict(os.environ)  # Immutable snapshot used by EVERY child.
    state = {"receipt": json.loads(options._receipt_path.read_text())}
    try:
        return audit(options, budget, environment, state)
    except (BudgetExpired, subprocess.TimeoutExpired):
        incomplete(state["receipt"], budget.stage)
        save(options._receipt_path, state["receipt"])
        return 1
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        incomplete(state["receipt"], budget.stage)
        save(options._receipt_path, state["receipt"])
        print("AUDIT ERROR NOT_ACCEPTANCE: " + str(error), file=sys.stderr, flush=True)
        return 2


def run_audit(options):
    """One invocation deadline also bounds blocked metadata/file I/O.

    The worker checkpoints *only* to a new sidecar until the parent promotes it.
    Reserve 3s inside the requested horizon for owned-tree cleanup (shared
    supervisor's <=0.6s cooperative grace plus bounded reap/drain) and the final
    receipt write. No new phase receives a fresh overall budget. Streaming hash
    checks provide cooperative stops; the owned worker is the backstop for a
    stuck read/stat/fsync or an unexpectedly slow final pin verification.
    """
    deadline = INVOCATION_START + options.suite_seconds
    work_deadline = deadline - 3.0
    output = options.output.resolve()
    if output.exists() and (options.resume is None or output != options.resume.resolve()):
        raise ValueError("output exists and is not the explicitly resumed receipt")
    checkpoint = output.with_name(output.stem + "-" + uuid.uuid4().hex + ".partial.json")
    receipt = dict(version=2, classification="NOT_ACCEPTANCE", status="INCOMPLETE",
                   inventory_known=False, inventory_status="UNKNOWN_DISCOVERY_INCOMPLETE",
                   pins=None, pins_stable=False, resume_verified=False,
                   complete=False, all_pass=False, nonresumable=True, tests={}, runs=[])
    save(checkpoint, receipt)
    argv = [sys.executable, str(Path(__file__).resolve()), *sys.argv[1:],
            "--_audit-worker", "--_audit-started", str(INVOCATION_START),
            "--_audit-deadline", str(work_deadline), "--_receipt-path", str(checkpoint)]
    remaining = work_deadline - time.monotonic()
    if remaining > 0:
        # Frozen full environment, not a filtered whitelist or os.environ read
        # at later spawn sites. The worker fingerprints the actual inherited
        # snapshot after the supervisor's documented depth transformation.
        environment = dict(os.environ)
        with supervisor.run(argv, name="audit-budget", timeout=remaining, env=environment) as result:
            receipt = json.loads(checkpoint.read_text())
            status = result.returncode
            if result.outcome in ("TIMEOUT", "INTERRUPTED") or result.cleanup_error is not None:
                incomplete(receipt, receipt.get("incomplete_stage", "worker-deadline"))
                status = 1
    else:
        status = 1
        incomplete(receipt, "startup")
    receipt["invocation_elapsed_seconds"] = time.monotonic() - INVOCATION_START
    receipt["suite_budget_seconds"] = options.suite_seconds
    receipt["absolute_deadline_monotonic"] = deadline
    receipt["worker_deadline_monotonic"] = work_deadline
    receipt["cleanup_and_receipt_reserve_seconds"] = 3.0
    receipt["suite_budget_exceeded"] = time.monotonic() > deadline
    if receipt["suite_budget_exceeded"]:
        incomplete(receipt, "parent-finalization")
        status = 1
    receipt["nonresumable"] = not receipt.get("pins_stable", False)
    if status == 0 and not (receipt.get("all_pass") and receipt.get("pins_stable")):
        status = 1
    # A failed/unverified resume never overwrites the old PASS/history. Leave
    # its incomplete sidecar explicit and NONRESUMABLE instead.
    commit = status != 2 and (options.resume is None or receipt.get("resume_verified"))
    save(output if commit else checkpoint, receipt)
    print("AUDIT receipt=" + str(output if commit else checkpoint) + " " + receipt["status"],
          file=sys.stderr, flush=True)
    return status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--per-test-seconds", type=float, default=30)
    parser.add_argument("--suite-seconds", type=float, default=120)
    parser.add_argument("--names", nargs="+")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--resume", type=Path)
    parser.add_argument("--_audit-worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--_audit-started", type=float, help=argparse.SUPPRESS)
    parser.add_argument("--_audit-deadline", type=float, help=argparse.SUPPRESS)
    parser.add_argument("--_receipt-path", type=Path, help=argparse.SUPPRESS)
    options = parser.parse_args()
    if not math.isfinite(options.per_test_seconds) or not 0 < options.per_test_seconds <= 30:
        parser.error("audit per-test budget must be >0 and <=30s")
    if not math.isfinite(options.suite_seconds) or options.suite_seconds <= 5:
        parser.error("suite budget must be finite and >5s (includes cleanup reserve)")
    try:
        return worker(options) if options._audit_worker else run_audit(options)
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        print("AUDIT ERROR NOT_ACCEPTANCE: " + str(error), file=sys.stderr, flush=True)
        return 2


if __name__ == "__main__":
    sys.exit(main())
