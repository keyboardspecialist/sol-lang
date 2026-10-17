#!/usr/bin/env python3
"""Fast supervisor/C harness regressions. Synthetic waits run concurrently."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import time
import tracemalloc
import unittest
from unittest import mock

import run_with_progress as supervisor


HERE = Path(__file__).resolve().parent
CHILD = HERE / "support_child.py"
MONITOR = HERE / "run_with_progress.py"
OPTIONS = None


def command(mode):
    return [sys.executable, str(CHILD), mode]


def phase(name):
    # unittest's verbose case label deliberately has no trailing newline.
    print("\nSOL_TEST_PROGRESS\tevent=phase\tphase=" + name
          + "\tcompleted=0\tunit=case", file=sys.stderr, flush=True)


def wait_for(file, needle, process, timeout=1.5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        file.seek(0)
        data = file.read()
        if needle in data:
            return data
        if process.poll() is not None:
            raise AssertionError("child exited before live output: " + repr(data))
        time.sleep(0.01)
    raise AssertionError("missing live output: " + repr(needle))


class ProgressTests(unittest.TestCase):
    def test_ctest_records_and_native_operations(self):
        line = b"3: SOL_TEST_PROGRESS\tevent=phase\tphase=mutate\tfixture=p33\tcompleted=0\ttotal=7\tunit=attempt\n"
        pending, records = supervisor._records(b"", line[:17])
        pending, records = supervisor._records(pending, line[17:])
        self.assertEqual(pending, b"")
        self.assertEqual(records[0]["fixture"], "p33")
        if OPTIONS.child is None:
            return  # Dedicated C checks elsewhere explicitly skip without --child.
        with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
            with supervisor.run([str(OPTIONS.child), "--operations"], name="typed-operations",
                                timeout=2, stdout=out, stderr=err, monitor=err) as result:
                self.assertEqual(result.returncode, 0)
                completed = [r for r in result.observations if r.get("phase") == "mutate-restore"
                             and r.get("event") == "phase_done"]
                self.assertEqual(completed[0]["completed"], "2")
                self.assertEqual(completed[0]["ordinal_total"], "7")

    def setUp(self):
        phase(self._testMethodName)

    def invoke(self, argv, **kwargs):
        # File-backed output avoids accidentally using test code as an unbounded
        # capture path, especially for the oversized-line/high-volume regression.
        with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
            return supervisor.run(argv, name=self._testMethodName, timeout=3,
                                  capture=True, stdout=out, stderr=err, **kwargs)

    def test_fast_success_nonzero_stderr_signal_missing_exec(self):
        for mode, expected in (("success", 0), ("nonzero", 7), ("stderr", 0)):
            with self.invoke(command(mode)) as result:
                self.assertEqual(result.returncode, expected)
                if mode == "stderr":
                    self.assertEqual(result.stderr.read(), b"stderr-only\n")
        with self.invoke([str(HERE / "does not exist")]) as result:
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.outcome, "FAIL")
        if os.name != "nt":
            with self.invoke(command("signal")) as result:
                self.assertEqual(result.returncode, -signal.SIGTERM)
            cli = subprocess.run([sys.executable, str(MONITOR), "--name", "signal",
                                  "--", *command("signal")], capture_output=True,
                                 timeout=2)
            self.assertEqual(cli.returncode, -signal.SIGTERM)
            self.assertIn(b'"event":"FAIL"', cli.stderr)

    def test_parallel_redirected_liveness_and_partial_lines(self):
        def probe(mode):
            arrivals = []

            class Sink:
                def write(self, data):
                    arrivals.append((time.monotonic(), data))

                def flush(self):
                    pass

            sink = Sink()
            start = time.monotonic()
            with supervisor.run(command(mode), name=mode, timeout=2.5,
                                stdout=sink, stderr=sink, monitor=sink, scope=scope) as result:
                self.assertEqual(result.returncode, 0)
                self.assertLessEqual(result.max_delivery_gap, 1.5)
                transcript = b"".join(data for _, data in arrivals)
                self.assertIn(b'"event":"still_running"', transcript)
                self.assertLess(arrivals[0][0] - start, 0.1)
                if mode == "partial":
                    arrival = next(t for t, data in arrivals
                                   if b"partial-without-newline" in data)
                    self.assertLess(arrival - start, 0.5)
                if mode == "phase":
                    self.assertIn(b'"completed":"3"', transcript)
                    self.assertIn(b'"phase":"fixture-build"', transcript)
                    self.assertIn(b"completed count unchanged", transcript)
                print("redirected-proof mode={} elapsed={:.3f}s max_gap={:.3f}s".format(
                    mode, result.elapsed, result.max_delivery_gap), flush=True)

        with supervisor.cancellation_scope() as scope, ThreadPoolExecutor(max_workers=3) as pool:
            list(pool.map(probe, ("quiet", "partial", "phase")))

    def test_simultaneous_high_volume_no_newline_memory_bound(self):
        tracemalloc.start()
        try:
            with self.invoke(command("dual")) as result:
                self.assertEqual(result.returncode, 0)
                for stream, expected in ((result.stdout, b"o"), (result.stderr, b"e")):
                    stream.seek(0, os.SEEK_END)
                    self.assertEqual(stream.tell(), 16 * 1024 * 1024)
                    stream.seek(0)
                    while True:
                        data = stream.read(4096)
                        if not data:
                            break
                        self.assertEqual(data, expected * len(data))
                _, peak = tracemalloc.get_traced_memory()
                self.assertLess(peak, 3 * 1024 * 1024)
                self.assertLessEqual(len(result.observations), supervisor.HISTORY_RECORDS)
                print("bounded-memory forwarded=32MiB python-peak={} bytes".format(peak),
                      flush=True)
        finally:
            tracemalloc.stop()

    def test_argv_environment_cwd_spaces(self):
        with tempfile.TemporaryDirectory(prefix="sol progress space ") as directory:
            env = dict(os.environ, SOL_PROGRESS_FIXTURE="inherited value")
            spaced_child = Path(directory) / "child with spaces.py"
            spaced_child.write_bytes(CHILD.read_bytes())
            with self.invoke([sys.executable, str(spaced_child), "context",
                              "one argument", "", "x; y"],
                             env=env, cwd=directory) as result:
                self.assertEqual(result.returncode, 0)
                context = json.load(result.stdout)
                self.assertEqual(context["argv"], ["one argument", "", "x; y"])
                self.assertEqual(Path(context["cwd"]).resolve(), Path(directory).resolve())
                self.assertEqual(context["env"], "inherited value")

    @unittest.skipIf(os.name == "nt", "POSIX signal/tree proof; Windows Job path unvalidated")
    def test_timeout_cancel_nested_and_force_cleanup(self):
        def probe(kind):
            argv = command("ignore-term" if kind == "force" else "tree")
            if kind == "nested":
                argv = [sys.executable, str(MONITOR), "--name", "inner", "--", *argv]
            cancel = threading.Event()
            with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
                class Sink:
                    def write(self, data):
                        out.write(data)
                        if kind == "cancel" and b"grandchild" in data:
                            cancel.set()

                    def flush(self):
                        out.flush()

                with supervisor.run(argv, name=kind, timeout=0.35 if kind != "cancel" else 3,
                                    stdout=Sink(), stderr=err, scope=scope, cancel=cancel) as result:
                    self.assertEqual(result.returncode, -signal.SIGTERM if kind == "cancel" else 124)
                    self.assertEqual(result.outcome, "INTERRUPTED" if kind == "cancel" else "TIMEOUT")
                    if kind != "force":
                        out.seek(0)
                        pids = json.loads(out.readline())
                        for pid in pids.values():
                            with self.assertRaises(ProcessLookupError):
                                os.kill(pid, 0)

        with supervisor.cancellation_scope() as scope, ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(probe, ("timeout", "cancel", "nested", "force")))

    def test_unowned_worker_rejected_before_spawn(self):
        with ThreadPoolExecutor(max_workers=1) as pool, mock.patch.object(
                supervisor.subprocess, "Popen") as spawn:
            future = pool.submit(supervisor.run, command("success"), name="unowned")
            with self.assertRaisesRegex(RuntimeError, "cancellation_scope"):
                future.result()
            spawn.assert_not_called()

    @unittest.skipIf(os.name == "nt", "POSIX owned PID proof; Windows jobs unvalidated")
    def test_exception_cleanup_nested_tree(self):
        def probe(kind):
            with tempfile.TemporaryDirectory(prefix="sol owned error ") as directory, \
                    tempfile.TemporaryFile() as err, tempfile.TemporaryFile() as out:
                pid_file = Path(directory) / "tree.json"
                argv = [sys.executable, str(MONITOR), "--name", "nested-error", "--",
                        *command("tree"), str(pid_file)]
                original = OSError("original-forward-error") if kind == "output-oserror" else \
                    BrokenPipeError("original-broken-pipe")

                class BrokenSink:
                    calls = 0

                    def write(self, _data):
                        self.calls += 1
                        raise original

                    def flush(self):
                        pass

                sink = BrokenSink() if kind.startswith("output-") else \
                    object() if kind == "missing-write" else out
                observer_error = ValueError("original-observer-error") if kind == "observer" else \
                    OSError("original-observer-oserror")

                def observer(_record):
                    raise observer_error

                before = time.monotonic()
                try:
                    result = supervisor.run(argv, name=kind, timeout=2, scope=scope,
                        stdout=sink, stderr=err, monitor=err, capture=True,
                        on_observation=observer if kind.startswith("observer") else None)
                except ValueError as error:
                    self.assertEqual(kind, "observer")
                    self.assertIs(error, observer_error)
                except AttributeError as error:
                    self.assertEqual(kind, "missing-write")
                    self.assertIn("write", str(error))
                else:
                    with result:
                        self.assertIn(kind, ("output-oserror", "output-brokenpipe", "observer-oserror"))
                        self.assertEqual((result.outcome, result.returncode), ("FAIL", 127))
                        self.assertIsNone(result.cleanup_error)
                        self.assertIn(str(observer_error if kind.startswith("observer") else original),
                                      result.error)
                self.assertLess(time.monotonic() - before, 2)
                if isinstance(sink, BrokenSink):
                    self.assertEqual(sink.calls, 1, "cleanup called the failed sink again")
                pids = json.loads(pid_file.read_text())
                self.assertEqual(set(pids), {"parent", "grandchild", "supervisor"})
                for pid in pids.values():
                    with self.assertRaises(ProcessLookupError):
                        os.kill(pid, 0)
                print("owned-error-proof kind={} pids={} all_gone=true".format(kind, pids), flush=True)

        with supervisor.cancellation_scope() as scope, ThreadPoolExecutor(max_workers=5) as pool:
            list(pool.map(probe, ("output-oserror", "output-brokenpipe", "missing-write",
                                  "observer", "observer-oserror")))

    @unittest.skipIf(os.name == "nt", "POSIX timeout/signals; Windows jobs unvalidated")
    def test_outer_cancellation_of_concurrent_imported_runs(self):
        def probe(kind):
            with tempfile.TemporaryDirectory(prefix="sol worker ownership ") as directory, \
                    tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
                argv = [sys.executable, str(MONITOR), "--name", "worker-outer",
                        "--timeout", "0.35" if kind == "timeout" else "3", "--",
                        sys.executable, str(HERE / "ownership_child.py"), directory]
                sent = False
                pending = b""

                class Sink:
                    def write(self, data):
                        nonlocal sent, pending
                        out.write(data)
                        pending = (pending + data)[-8192:]
                        if kind != "timeout" and not sent and b"WORKERS_READY" in pending:
                            owner = json.loads((Path(directory) / "owner.json").read_text())
                            os.kill(owner["supervisor"], signal.SIGINT if kind == "SIGINT" else signal.SIGTERM)
                            sent = True

                    def flush(self):
                        out.flush()

                with supervisor.run(argv, name=kind, timeout=3, scope=scope,
                                    stdout=Sink(), stderr=err) as result:
                    expected = 124 if kind == "timeout" else \
                        -signal.SIGINT if kind == "SIGINT" else -signal.SIGTERM
                    self.assertEqual(result.returncode, expected)
                    self.assertLess(result.elapsed, 2)
                pid_files = list(Path(directory).glob("*.json"))
                self.assertEqual(len(pid_files), 3, "both workers must actually have spawned")
                pids = {pid for path in pid_files for pid in json.loads(path.read_text()).values()}
                self.assertEqual(len(pids), 6, "outer/helper + two children + two grandchildren")
                for pid in pids:
                    with self.assertRaises(ProcessLookupError):
                        os.kill(pid, 0)
                print("owned-worker-proof kind={} pids={} all_gone=true".format(kind, sorted(pids)),
                      flush=True)

        with supervisor.cancellation_scope() as scope, ThreadPoolExecutor(max_workers=3) as pool:
            list(pool.map(probe, ("timeout", "SIGINT", "SIGTERM")))

    def test_cancel_event_and_validation(self):
        cancel = threading.Event()
        cancel.set()
        with self.invoke(command("quiet"), cancel=cancel) as result:
            self.assertEqual(result.outcome, "INTERRUPTED")
            self.assertNotEqual(result.returncode, 0)
        for timeout in (0, -1, float("nan"), float("inf")):
            with self.assertRaises(ValueError):
                supervisor.run(command("success"), name="bad-timeout", timeout=timeout)

    def test_record_boundaries_and_bounded_history(self):
        pending, records = supervisor._records(b"", b"SOL_TEST_PROG")
        pending, records = supervisor._records(pending,
            b"RESS\tevent=phase\tphase=compile\tcompleted=0\n")
        self.assertEqual(records[0]["phase"], "compile")
        pending, records = supervisor._records(b"", b"x" * (supervisor.MAX_RECORD_BYTES + 1))
        self.assertIsNone(pending)
        pending, records = supervisor._records(pending,
            b"\nSOL_TEST_PROGRESS\tevent=units\tphase=compile\tcompleted=1\n")
        self.assertEqual(records[0]["completed"], "1")
        with self.invoke(command("history")) as result:
            self.assertEqual(result.returncode, 0)
            self.assertEqual(len(result.observations), supervisor.HISTORY_RECORDS)
            self.assertEqual(result.observations[-1]["completed"], "255")

    def test_c_continuation_and_live_direct_stderr(self):
        if OPTIONS.child is None:
            self.skipTest("pass --child BUILD/sol_progress_child for C harness checks")
        for arguments, expected in (([], 0), (["--fail"], 1)):
            with self.invoke([OPTIONS.child, *arguments]) as result:
                errors = result.stderr.read(supervisor.MAX_RECORD_BYTES)
                self.assertEqual(result.returncode, expected, errors)
                self.assertEqual(result.stdout.read(), b"FOLLOWING_CASE_EXECUTED\n")
                self.assertIn(b"event=done_pass" if expected == 0 else b"event=done_fail",
                              errors)
        with tempfile.TemporaryFile() as err:
            process = subprocess.Popen([OPTIONS.child, "--wait"], stdin=subprocess.PIPE,
                                       stdout=subprocess.DEVNULL, stderr=err)
            try:
                wait_for(err, b"phase=wait-for-input", process)
                self.assertIsNone(process.poll(), "phase must arrive before completion")
                process.communicate(b"continue\n", timeout=2)
                self.assertEqual(process.returncode, 0)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_cmake_capture_boundaries(self):
        if OPTIONS.cmake is None:
            self.skipTest("pass --cmake CMAKE for capture boundary checks")
        with tempfile.TemporaryDirectory(prefix="sol capture space ") as directory:
            script = Path(directory) / "capture.cmake"
            script.write_text('''include("{support}")
sol_test_phase("exact-json")
execute_process(COMMAND "{cmake}" -E echo "{{\\"ok\\":true}}"
    OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE status)
if(NOT status EQUAL 0 OR NOT output STREQUAL "{{\\"ok\\":true}}\\n" OR NOT error STREQUAL "")
    message(FATAL_ERROR "captured compiler contract changed")
endif()
sol_test_phase("empty-streams")
execute_process(COMMAND "{cmake}" -E true
    OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE status)
if(NOT status EQUAL 0 OR NOT output STREQUAL "" OR NOT error STREQUAL "")
    message(FATAL_ERROR "empty stream contract changed")
endif()
'''.format(support=(HERE / "test_progress.cmake").as_posix(),
           cmake=Path(OPTIONS.cmake).as_posix()))
            with self.invoke([OPTIONS.cmake, "-P", str(script)]) as result:
                self.assertEqual(result.returncode, 0, result.stderr.read())
                self.assertEqual(result.stdout.read(), b"")
                self.assertEqual(len(result.observations), 2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--child")
    parser.add_argument("--cmake")
    OPTIONS, remaining = parser.parse_known_args()
    if OPTIONS.child is not None:
        OPTIONS.child = str(Path(OPTIONS.child).resolve())
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
