#!/usr/bin/env python3
"""Bounded test-tool regressions. No compiler matrices or fuzz workloads."""
import argparse
import hashlib
import importlib.util
import io
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

import run_with_progress as supervisor

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OPTIONS = None
SPEC = importlib.util.spec_from_file_location("audit_tests", ROOT / "tests/audit_tests.py")
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


class IntegrationTests(unittest.TestCase):
    def invoke(self, argv, **kwargs):
        with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
            with supervisor.run(argv, name=self._testMethodName, timeout=12, capture=True,
                                stdout=out, stderr=err, monitor=err, **kwargs) as result:
                return result.returncode, result.stdout.read(), result.stderr.read()

    def test_stress_stubs_and_hash_json(self):
        def probe(mode):
            with tempfile.TemporaryDirectory(prefix="sol-stress-proof-") as temporary:
                root = Path(temporary)
                stub = root / "stub sol"
                stub.write_text("#!" + sys.executable + "\n" + (HERE / "stress_stub.py").read_text())
                stub.chmod(0o755)
                baseline = json.loads((ROOT / "tests/stress/baselines.json").read_text())
                if mode == "timeout":
                    for value in baseline.values():
                        value["max_seconds"] = 1.0  # Temporary stub policy, never real baseline.
                baseline_path = root / "baseline.json"
                baseline_path.write_text(json.dumps(baseline))
                argv = [sys.executable, str(ROOT / "tests/stress/run_stress.py"),
                        "--baseline", str(baseline_path), "--work-dir", str(root / "generated")]
                if mode == "hashes":
                    argv += ["--print-hashes"]
                else:
                    argv += ["--sol", str(stub)]
                status, out, err = self.invoke(argv, scope=scope,
                    env=dict(os.environ, SOL_STRESS_STUB=mode))
                if mode == "hashes":
                    self.assertEqual(json.loads(out), {name: value["sha256"] for name, value in baseline.items()})
                    self.assertEqual(status, 0)
                elif mode == "success":
                    self.assertEqual(status, 0)
                    self.assertIn(b"package:", out)
                else:
                    self.assertNotEqual(status, 0)
                self.assertIn(b"generate-generic-chain", err)
                if mode != "hashes":
                    self.assertIn(b"stub-live-stdout", out, (mode, err))
                    self.assertIn(b"stub-live-stderr", err, mode)
                if mode == "timeout":
                    self.assertIn(b'"event":"TIMEOUT"', err)
                if mode == "wrong":
                    self.assertIn(b"expected", err)
        with supervisor.cancellation_scope() as scope, ThreadPoolExecutor(max_workers=5) as pool:
            list(pool.map(probe, ("success", "wrong", "failure", "timeout", "hashes")))

    @unittest.skipIf(os.name == "nt", "POSIX stub shebang and local CTest; Windows unvalidated")
    def test_audit_inventory_budget_resume_and_stale_rejection(self):
        # A blocked-metadata horizon runs alongside (not after) the normal tiny
        # resume probes, keeping the entire integration group's <=5s target.
        with supervisor.cancellation_scope() as scope, ThreadPoolExecutor(max_workers=2) as pool:
            normal = pool.submit(self.audit_resume, scope)
            blocked = pool.submit(self.audit_blocked_metadata, scope)
            normal.result()
            blocked.result()

    def audit_resume(self, scope):
        with tempfile.TemporaryDirectory(prefix="sol-audit-proof-") as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            build = root / "build"
            project = '''cmake_minimum_required(VERSION 3.17)
project(AuditProof NONE)
enable_testing()
add_test(NAME tiny.wait COMMAND "PYTHON" "CHILD" grandchild)
add_test(NAME tiny.pass COMMAND "PYTHON" "CHILD" success)
add_test(NAME tiny.fail COMMAND "PYTHON" "CHILD" nonzero)
add_test(NAME tiny.disabled COMMAND "PYTHON" "CHILD" success)
set_tests_properties(tiny.disabled PROPERTIES DISABLED TRUE)
'''.replace("PYTHON", sys.executable).replace("CHILD", str(HERE / "support_child.py"))
            (source / "CMakeLists.txt").write_text(project)
            status, _, _ = self.invoke([OPTIONS.cmake, "-S", str(source), "-B", str(build)], scope=scope)
            self.assertEqual(status, 0)
            output = root / "audit.json"
            argv = [sys.executable, str(ROOT / "tests/audit_tests.py"), "--build-dir", str(build),
                    "--output", str(output), "--suite-seconds", "6.1", "--per-test-seconds", "2"]
            secret = "not-for-receipts-" + os.urandom(16).hex()
            environment = dict(os.environ, AUDIT_SECRET_FIXTURE=secret, AUDIT_CUSTOM_FIXTURE="original")
            status, _, err = self.invoke(argv, scope=scope, env=environment)
            self.assertEqual(status, 1, err)
            first = json.loads(output.read_text())
            self.assertEqual(set(first["tests"]), {"tiny.wait", "tiny.pass", "tiny.fail", "tiny.disabled"})
            self.assertEqual(first["tests"]["tiny.wait"]["status"], "AUDIT_TIMEOUT")
            self.assertTrue(any(row["status"] == "NOT_RUN_SUITE_BUDGET" for row in first["tests"].values()))
            self.assertFalse(first["complete"])
            self.assertTrue(first["pins_stable"])
            resume = argv[:-4] + ["--suite-seconds", "8", "--per-test-seconds", "0.5",
                                  "--resume", str(output), "--names", "tiny.pass", "tiny.fail", "tiny.disabled"]
            status, _, err = self.invoke(resume, scope=scope, env=environment)
            self.assertEqual(status, 1, err)
            second = json.loads(output.read_text())
            self.assertEqual(second["tests"]["tiny.pass"]["status"], "PASS")
            self.assertEqual(second["tests"]["tiny.fail"]["status"], "FAIL")
            self.assertEqual(second["tests"]["tiny.disabled"]["status"], "NOT_RUN_DISABLED")
            self.assertEqual(second["tests"]["tiny.disabled"]["attempts"], [])
            self.assertEqual(len(second["runs"]), 2)
            self.assertEqual(second["tests"]["tiny.wait"], first["tests"]["tiny.wait"])
            self.assertEqual(second["pins"]["runtime_environment"]["values_sha256"]["AUDIT_SECRET_FIXTURE"],
                             hashlib.sha256(secret.encode()).hexdigest())
            inherited = AUDIT.child_environment(AUDIT.child_environment(environment))
            self.assertEqual(second["pins"]["runtime_environment"], AUDIT.environment_identity(inherited))
            self.assertEqual(second["pins"]["effective_child_environment"],
                             AUDIT.environment_identity(AUDIT.child_environment(inherited)))
            # Every mutation must reject BEFORE PATH lookup/discovery or any
            # reused PASS is committed. No raw secret values in any receipt.
            before = output.read_bytes()
            def changed(key):
                changed_env = dict(environment)
                changed_env[key] = "/invalid-audit-tool" if key == "PATH" else "different-audit-value"
                status, _, err = self.invoke(resume, scope=scope, env=changed_env)
                self.assertEqual(status, 2, (key, err))
                self.assertIn(b"resume rejected", err)
                self.assertEqual(output.read_bytes(), before)
            with ThreadPoolExecutor(max_workers=4) as pool:
                list(pool.map(changed, ("CC", "CFLAGS", "PATH", "AUDIT_CUSTOM_FIXTURE")))
            for receipt_path in root.glob("*.json"):
                self.assertNotIn(secret, receipt_path.read_text())
            cache = build / "CMakeCache.txt"
            cache.write_text(cache.read_text() + "\nSOL_PROOF_PIN:STRING=changed\n")
            before = output.read_bytes()
            status, _, err = self.invoke(resume, scope=scope, env=environment)
            self.assertEqual(status, 2)
            self.assertIn(b"resume rejected", err)
            self.assertEqual(output.read_bytes(), before)

    def audit_blocked_metadata(self, scope):
        with tempfile.TemporaryDirectory(prefix="sol-audit-deadline-") as temporary:
            root = Path(temporary)
            # Only this synthetic wrapper stalls. No production test/tool hook.
            wrapper = root / "blocked.py"
            wrapper.write_text("import importlib.util, sys, time\n"
                "s=importlib.util.spec_from_file_location('audit_tests', " + repr(str(ROOT / "tests/audit_tests.py")) + ")\n"
                "a=importlib.util.module_from_spec(s); s.loader.exec_module(a)\n"
                "a.__file__=__file__\n"
                "a.captured=lambda *args, **kwargs: time.sleep(20)\n"
                "sys.exit(a.main())\n")
            output = root / "bounded.json"
            before = time.monotonic()
            status, _, err = self.invoke([sys.executable, str(wrapper), "--build-dir", str(root),
                "--output", str(output), "--suite-seconds", "6.1"], scope=scope)
            elapsed = time.monotonic() - before
            self.assertEqual(status, 1, err)
            self.assertLess(elapsed, 6.1)
            receipt = json.loads(output.read_text())
            self.assertEqual(receipt["status"], "INCOMPLETE")
            self.assertFalse(receipt["inventory_known"])
            self.assertEqual(receipt["inventory_status"], "UNKNOWN_DISCOVERY_INCOMPLETE")
            self.assertFalse(receipt["pins_stable"])
            self.assertTrue(receipt["nonresumable"])
            print("audit-hard-deadline-proof elapsed={:.3f}s budget=6.1s inventory=UNKNOWN".format(elapsed), flush=True)

    def test_audit_absolute_budget_phases(self):
        # Deterministic clock: six exhaustion sites without six real sleeps.
        # The separate blocked-process proof above exercises the hard backstop.
        from types import SimpleNamespace
        from contextlib import contextmanager
        @contextmanager
        def metadata_run(argv, **kwargs):
            self.assertAlmostEqual(kwargs["timeout"], 0.25)
            self.assertEqual(kwargs["env"], {"CC": "explicit-snapshot"})
            yield SimpleNamespace(returncode=0, outcome="PASS", stdout=io.BytesIO(b"metadata"))
        with mock.patch.object(AUDIT.time, "monotonic", return_value=10), \
             mock.patch.object(AUDIT.supervisor, "run", side_effect=metadata_run):
            self.assertEqual(AUDIT.captured(["synthetic"], budget=AUDIT.Budget(10.25),
                environment={"CC": "explicit-snapshot"}), b"metadata")
        for stage in ("discovery", "manifests", "overhead", "slow-pins", "final-verification", "probe-grace"):
            with self.subTest(stage=stage), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / "CMakeCache.txt").write_text("")
                (root / "sol_wasm_represented_tests").touch()
                checkpoint = root / "checkpoint.json"
                AUDIT.save(checkpoint, dict(inventory_known=False, pins_stable=False, tests={}, runs=[]))
                clock = [0.0]
                options = SimpleNamespace(_audit_started=0, _audit_deadline=5,
                    _receipt_path=checkpoint, output=root / "receipt.json", resume=None,
                    build_dir=root, names=None, suite_seconds=8, per_test_seconds=2)
                calls = []
                identity = dict(proof="same")
                def captured(argv, **kwargs):
                    calls.append((kwargs["budget"].stage, list(argv)))
                    kwargs["budget"].check()
                    if "--show-only=json-v1" in argv:
                        if stage == "discovery":
                            clock[0] = 6
                        return json.dumps(dict(tests=[dict(name="tiny.pass", command=[], properties=[])])).encode()
                    # Cumulative manifest work crosses the SAME deadline, not
                    # a new per-manifest allowance. Nothing afterwards starts.
                    if stage == "manifests":
                        clock[0] += 3
                        kwargs["budget"].check()
                    return b"manifest"
                pin_calls = []
                def pinning(*args):
                    pin_calls.append(args)
                    if stage == "slow-pins":
                        clock[0] = 6
                        args[3].check()
                    if stage == "final-verification" and len(pin_calls) == 2:
                        clock[0] = 6
                    return identity
                @contextmanager
                def run(argv, **kwargs):
                    if kwargs["name"].startswith("audit:") and stage == "probe-grace":
                        # Active deadline plus unexpected cooperative cleanup
                        # consumes the remaining horizon; final pins cannot pass.
                        clock[0] += kwargs["timeout"] + 5
                    yield SimpleNamespace(returncode=0, outcome="PASS", elapsed=0.01,
                        observations=[], max_delivery_gap=0, cleanup_error=None)
                def direct(*args, **kwargs):
                    self.assertLessEqual(kwargs["timeout"], 5 - clock[0])
                    if stage == "overhead":
                        clock[0] = 6
                with mock.patch.object(AUDIT.time, "monotonic", side_effect=lambda: clock[0]), \
                     mock.patch.object(AUDIT.shutil, "which", return_value=sys.executable), \
                     mock.patch.object(AUDIT, "captured", side_effect=captured), \
                     mock.patch.object(AUDIT.subprocess, "run", side_effect=direct), \
                     mock.patch.object(AUDIT.supervisor, "run", side_effect=run), \
                     mock.patch.object(AUDIT, "source_pins", return_value={}):
                    if stage == "manifests":
                        status = AUDIT.worker(options)
                    else:
                        with mock.patch.object(AUDIT, "pins", side_effect=pinning):
                            status = AUDIT.worker(options)
                result = json.loads(checkpoint.read_text())
                self.assertEqual(status, 1)
                self.assertEqual(result["status"], "INCOMPLETE")
                self.assertFalse(result["pins_stable"])
                self.assertFalse(result["all_pass"])
                self.assertTrue(result["nonresumable"])
                if stage == "discovery":
                    self.assertFalse(result["inventory_known"])
                    self.assertEqual(pin_calls, [])
                if stage != "final-verification":
                    self.assertLessEqual(len(pin_calls), 1)
        # Streaming reads are checked before AND after each chunk, not just
        # before starting one huge file. No full-source scan needed for proof.
        clock = [0.0]
        class SlowStream(io.BytesIO):
            def read(self, size):
                clock[0] += 0.6
                return super().read(size)
        stream = SlowStream(b"x" * (3 * 1024 * 1024))
        with mock.patch.object(AUDIT.time, "monotonic", side_effect=lambda: clock[0]), \
             mock.patch("builtins.open", return_value=stream):
            with self.assertRaises(AUDIT.BudgetExpired):
                AUDIT.sha("synthetic-slow-file", AUDIT.Budget(1.0))
        self.assertEqual(clock[0], 1.2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
