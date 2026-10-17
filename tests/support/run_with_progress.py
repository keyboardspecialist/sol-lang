#!/usr/bin/env python3
"""Live, bounded-memory test supervision; import run() for file-backed capture.

No shell, PTY, output rewriting, estimated work or ETA. See tests/README.md for
process ownership, platform limits, and CTest's independent buffering behavior.
"""

import argparse
from collections import deque
from contextlib import contextmanager
import ctypes
from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import queue
import signal
import subprocess
import sys
import tempfile
import threading
import time


PREFIX = b"SOL_TEST_PROGRESS\t"
CHUNK_BYTES = 4096
QUEUE_CHUNKS = 32
MAX_RECORD_BYTES = 8192
HISTORY_RECORDS = 128
RECORD_FIELDS = {b"event", b"test", b"case", b"phase", b"unit", b"completed", b"total"}
RECORD_FIELDS |= {b"fixture", b"ordinal_kind", b"ordinal", b"ordinal_total"}
RECORD_FIELDS |= {b"hook", b"hook_after"}
RECORD_FIELDS.add(b"outer_phase")
_main_scope = None


class _CancellationScope:
    def __init__(self):
        self.signal_number = 0
        self.closing = False
        self.active = True
        self.runs = 0
        self.condition = threading.Condition()

    @property
    def cancelled(self):
        return self.signal_number != 0 or self.closing

    def handle_signal(self, number, _frame):
        # Python invokes this on the main thread. Do NOT acquire a lock or call
        # Event.set(): a signal can interrupt code already holding that lock.
        self.signal_number = number

    def register(self):
        with self.condition:
            if not self.active or self.closing:
                raise RuntimeError("cancellation scope is no longer accepting runs")
            self.runs += 1

    def unregister(self):
        with self.condition:
            self.runs -= 1
            self.condition.notify_all()

    def close(self):
        with self.condition:
            self.closing = True
            deadline = time.monotonic() + 3.0
            while self.runs:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise RuntimeError("supervisor workers did not finish owned cleanup")
                self.condition.wait(min(0.02, remaining))
            self.active = False


@contextmanager
def cancellation_scope():
    """Main-thread owner for concurrent imported runs (pass scope= to workers).

    Use ``with cancellation_scope() as scope, ThreadPoolExecutor(...) as pool``
    so the scope outlives its workers. Signals only set a scalar cancellation
    flag; each run cooperatively cleans its own group/job. Scope exit requests
    cancellation and waits for registered runs before restoring signal handlers.
    Worker calls without an active explicit scope are rejected BEFORE spawning.
    Sinks/observers must return promptly; arbitrary blocking Python callbacks are
    not interruptible. Windows job/signal behavior still requires platform proof.
    """
    global _main_scope
    if threading.current_thread() is not threading.main_thread():
        raise RuntimeError("cancellation_scope must be entered on the main thread")
    if _main_scope is not None:
        yield _main_scope
        return
    scope = _CancellationScope()
    previous = {}
    try:
        numbers = [signal.SIGINT, signal.SIGTERM]
        if os.name == "nt":
            numbers.append(signal.SIGBREAK)
        for number in numbers:
            previous[number] = signal.signal(number, scope.handle_signal)
        _main_scope = scope
        yield scope
    finally:
        try:
            scope.close()
        finally:
            _main_scope = None
            for number, handler in previous.items():
                signal.signal(number, handler)


@dataclass
class Result:
    outcome: str
    returncode: int
    elapsed: float
    max_delivery_gap: float
    observations: list
    stdout: object = None
    stderr: object = None
    interrupt_signal: int = 0
    error: str = None
    cleanup_error: str = None

    def close(self):
        for stream in (self.stdout, self.stderr):
            if stream is not None:
                stream.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def _write(stream, data):
    if stream is not None:
        stream.write(data)
        stream.flush()


class _WindowsJob:
    """Gate a Python launcher until it belongs to a kill-on-close Job Object.

    Assigning an already-running target has a descendant-escape race. Only the
    tiny gate launcher runs before assignment, and it cannot spawn before GO.
    Windows 8+ nested jobs are required when an outer supervisor/job exists.
    """

    def __init__(self):
        from ctypes import wintypes as w

        class BasicLimit(ctypes.Structure):
            _fields_ = [("process_time", ctypes.c_int64),
                        ("job_time", ctypes.c_int64), ("flags", w.DWORD),
                        ("min_working_set", ctypes.c_size_t),
                        ("max_working_set", ctypes.c_size_t),
                        ("active_processes", w.DWORD),
                        ("affinity", ctypes.c_size_t),
                        ("priority", w.DWORD), ("scheduling", w.DWORD)]

        class IoCounters(ctypes.Structure):
            _fields_ = [(name, ctypes.c_uint64) for name in
                        ("read_ops", "write_ops", "other_ops", "read_bytes",
                         "write_bytes", "other_bytes")]

        class ExtendedLimit(ctypes.Structure):
            _fields_ = [("basic", BasicLimit), ("io", IoCounters),
                        ("process_memory", ctypes.c_size_t),
                        ("job_memory", ctypes.c_size_t),
                        ("peak_process_memory", ctypes.c_size_t),
                        ("peak_job_memory", ctypes.c_size_t)]

        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, w.LPCWSTR]
        self.kernel.CreateJobObjectW.restype = w.HANDLE
        self.kernel.SetInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int,
                                                       ctypes.c_void_p, w.DWORD]
        self.kernel.SetInformationJobObject.restype = w.BOOL
        self.kernel.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
        self.kernel.AssignProcessToJobObject.restype = w.BOOL
        self.kernel.TerminateJobObject.argtypes = [w.HANDLE, w.UINT]
        self.kernel.TerminateJobObject.restype = w.BOOL
        self.kernel.CloseHandle.argtypes = [w.HANDLE]
        self.kernel.CloseHandle.restype = w.BOOL
        self.handle = self.kernel.CreateJobObjectW(None, None)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        limits = ExtendedLimit()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.kernel.SetInformationJobObject(
                self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            error = ctypes.WinError(ctypes.get_last_error())
            self.close()
            raise error

    def assign(self, process):
        if not self.kernel.AssignProcessToJobObject(self.handle, int(process._handle)):
            raise ctypes.WinError(ctypes.get_last_error())

    def terminate(self):
        if not self.kernel.TerminateJobObject(self.handle, 1):
            raise ctypes.WinError(ctypes.get_last_error())

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def _gate_child(command):
    # Internal Windows-only launcher. Target stdin is noninteractive, just as
    # CTest's. This pipe is exclusively an ownership handshake, never user data.
    if sys.stdin.buffer.read(1) != b"G":
        return 125
    try:
        return subprocess.call(command, stdin=subprocess.DEVNULL)
    except OSError as error:
        print(str(error), file=sys.stderr, flush=True)
        return 127


def _reader(pipe, stream, events, abort):
    def send(data):
        while not abort.is_set():
            try:
                events.put((stream, data), timeout=0.02)
                return
            except queue.Full:
                pass

    try:
        while True:
            data = os.read(pipe.fileno(), CHUNK_BYTES)
            if not data:
                break
            send(data)
            if abort.is_set():
                break
    finally:
        pipe.close()
        send(None)


def _records(pending, data):
    """Bounded line parsing only; forwarding itself is byte-exact and immediate."""
    records = []
    for part in data.splitlines(keepends=True):
        if pending is not None:
            pending += part
            if len(pending) > MAX_RECORD_BYTES:
                pending = None  # Ignore oversized line until its newline.
        if part.endswith(b"\n"):
            record_line = pending
            if record_line is not None:
                label, sep, content = record_line.partition(b": ")
                if sep and label.isdigit():
                    record_line = content  # CTest --verbose; forwarding stays exact.
            if record_line is not None and record_line.startswith(PREFIX):
                fields = {}
                for field in record_line[len(PREFIX):].strip().split(b"\t"):
                    key, sep, value = field.partition(b"=")
                    if sep and key in RECORD_FIELDS:
                        fields[key.decode("ascii", "replace")] = value.decode(
                            "ascii", "replace")
                if "phase" in fields and all(
                        fields[key].isascii() and fields[key].isdecimal()
                        for key in ("completed", "total") if key in fields):
                    records.append(fields)
            pending = b""
    return pending, records


def run(command, *, name, timeout=None, cwd=None, env=None, capture=False,
        stdout=None, stderr=None, monitor=None, cancel=None, on_observation=None,
        scope=None):
    """Run argv, stream bytes, optionally spool exact output to temporary files.

    stdout/stderr/monitor accept binary writable streams (None means sys streams).
    Capture is disk-backed, never an unbounded bytes/string. Result owns files;
    callers must close it. on_observation receives bounded semantic records with
    monotonic elapsed seconds; history retains only the last HISTORY_RECORDS.
    Main-thread calls automatically own a cancellation scope. Worker-thread
    calls MUST pass scope= from a main-thread cancellation_scope() context.
    Nested CLI supervisors each own a distinct child group/job, and receive a
    graceful cancellation window before their containing group is escalated.
    """
    if not command:
        raise ValueError("empty command")
    if timeout is not None and (not math.isfinite(timeout) or timeout <= 0):
        raise ValueError("timeout must be finite and positive")
    if scope is None:
        if threading.current_thread() is not threading.main_thread():
            raise RuntimeError("worker run requires an explicit main-thread cancellation_scope")
        with cancellation_scope() as owned_scope:
            return run(command, name=name, timeout=timeout, cwd=cwd, env=env,
                       capture=capture, stdout=stdout, stderr=stderr, monitor=monitor,
                       cancel=cancel, on_observation=on_observation, scope=owned_scope)
    scope.register()
    try:
        return _run(command, name=name, timeout=timeout, cwd=cwd, env=env,
                    capture=capture, stdout=stdout, stderr=stderr, monitor=monitor,
                    cancel=cancel, on_observation=on_observation, scope=scope)
    finally:
        scope.unregister()


def _run(command, *, name, timeout, cwd, env, capture, stdout, stderr, monitor,
         cancel, on_observation, scope):
    command = [os.fspath(arg) for arg in command]
    stdout = sys.stdout.buffer if stdout is None else stdout
    stderr = sys.stderr.buffer if stderr is None else stderr
    monitor = stderr if monitor is None else monitor
    start = time.monotonic()
    last_display = start
    phase_start = start
    phase = {"phase": "command", "inner_progress": "unavailable"}
    history = deque(maxlen=HISTORY_RECORDS)
    gap = 0.0
    cancel = cancel if cancel is not None else threading.Event()
    output_files = [None, None]
    process = None
    job = None
    readers = []
    reader_abort = threading.Event()
    failure = None
    cleanup_failure = None
    broken_streams = set()
    events = queue.Queue(maxsize=QUEUE_CHUNKS)
    stopping = None
    outcome = "FAIL"
    returncode = 127
    pending = [b"", b""]
    # Nested cleanup gets a strictly shorter grace period than its parent.
    child_env = dict(os.environ if env is None else env)
    try:
        depth = min(100, max(0, int(child_env.get("SOL_TEST_MONITOR_DEPTH", "0"))))
    except ValueError:
        depth = 0
    child_env["SOL_TEST_MONITOR_DEPTH"] = str(depth + 1)
    grace = 0.6 / (depth + 1)

    def forward(stream, data):
        try:
            _write(stream, data)
        except BaseException:
            broken_streams.add(id(stream))
            raise

    def display(event, **fields):
        nonlocal last_display, gap
        now = time.monotonic()
        gap = max(gap, now - last_display)
        record = dict(event=event, name=name, elapsed=round(now - start, 6), **fields)
        forward(monitor, b"SOL_TEST_MONITOR\t" + json.dumps(
            record, ensure_ascii=True, separators=(",", ":")).encode() + b"\n")
        last_display = now

    def stop(force=False):
        if process is None:
            return
        if os.name == "nt":
            job.terminate()
            if force and process.poll() is None:
                # Also handles failure to assign the still-gated launcher.
                process.kill()
        else:
            try:
                os.killpg(process.pid, signal.SIGKILL if force else signal.SIGTERM)
            except ProcessLookupError:
                pass
            except PermissionError:
                if process.poll() is None:
                    raise
                # Reaping a just-exited Darwin leader can remove the zombie-only
                # group. Retry only the exact private group we already own.
                try:
                    os.killpg(process.pid, signal.SIGKILL if force else signal.SIGTERM)
                except ProcessLookupError:
                    pass

    def group_alive():
        if os.name == "nt":
            return process.poll() is None
        try:
            os.killpg(process.pid, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            # Conservatively still present. Darwin can report EPERM while the
            # last member transitions to a zombie; poll/reap and retry rather
            # than aborting cleanup or assuming that the group disappeared.
            return True

    def discard(timeout=0.02):
        # No output sink, capture file, semantic observer, or display call belongs
        # in cleanup: the original failure may be in ANY of those callbacks.
        try:
            events.get(timeout=timeout)
        except queue.Empty:
            pass

    def cleanup():
        nonlocal stopping
        if process is None:
            return
        if stopping is None:
            stopping = time.monotonic()
            stop()
        # Even on exceptions give nested monitors a cooperative cancellation
        # window BEFORE killing their containing group. Drain/discard so full
        # child pipes cannot prevent their cleanup/final output from completing.
        deadline = stopping + grace
        while time.monotonic() < deadline:
            process.poll()
            if not group_alive():
                break
            discard(min(0.02, max(0.0, deadline - time.monotonic())))
        if group_alive():
            stop(force=True)
        process.wait(timeout=1.0)
        drain_deadline = time.monotonic() + 1.0
        while any(reader.is_alive() for reader in readers) and time.monotonic() < drain_deadline:
            discard()
        reader_abort.set()
        for reader in readers:
            reader.join(timeout=0.05)
        if any(reader.is_alive() for reader in readers):
            raise OSError("owned cleanup did not close child output pipes")
        for pipe in (process.stdout, process.stderr):
            if pipe is not None:
                pipe.close()

    try:
        display("start", phase="command", argv=command,
                cwd=os.fspath(cwd) if cwd is not None else os.getcwd())
        if capture:
            output_files = [tempfile.TemporaryFile(), tempfile.TemporaryFile()]
        if os.name == "nt":
            job = _WindowsJob()
            spawn = [sys.executable, str(Path(__file__).resolve()), "--_owned-child", *command]
            process = subprocess.Popen(spawn, cwd=cwd, env=child_env,
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, bufsize=0,
                                       creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
            job.assign(process)
            process.stdin.write(b"G")
            process.stdin.close()
        else:
            process = subprocess.Popen(command, cwd=cwd, env=child_env,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       start_new_session=True, bufsize=0)
        for index, pipe in enumerate((process.stdout, process.stderr)):
            reader = threading.Thread(target=_reader, args=(pipe, index, events, reader_abort),
                                      daemon=True)
            reader.start()
            readers.append(reader)
        eof = set()
        forced = False
        while len(eof) < 2 or process.poll() is None:
            now = time.monotonic()
            if stopping is None:
                if cancel.is_set() or scope.cancelled:
                    outcome = "INTERRUPTED"
                    stopping = now
                    stop()
                elif timeout is not None and now - start >= timeout:
                    outcome = "TIMEOUT"
                    stopping = now
                    stop()
                elif process.poll() is not None:
                    # Even a successful leader must not leave descendants holding
                    # pipes or running silently after its final status.
                    stopping = now
                    stop()
            elif not forced and now - stopping >= grace:
                stop(force=True)
                forced = True
            try:
                index, data = events.get(timeout=0.02)
            except queue.Empty:
                data = b""
                index = 0
            if data is None:
                eof.add(index)
            elif data:
                forward(output_files[index], data)
                forward((stdout, stderr)[index], data)
                delivered = time.monotonic()
                gap = max(gap, delivered - last_display)
                last_display = delivered
                pending[index], records = _records(pending[index], data)
                for fields in records:
                    if fields.get("event") != "units" or any(
                            fields.get(key) != phase.get(key)
                            for key in ("phase", "case", "unit")):
                        phase_start = delivered
                    phase = fields
                    observation = dict(elapsed=delivered - start, **fields)
                    history.append(observation)
                    if on_observation is not None:
                        on_observation(observation)
            if time.monotonic() - last_display >= 1.0:
                display("still_running", phase=phase,
                        phase_elapsed=round(time.monotonic() - phase_start, 6),
                        note="liveness only; completed count unchanged")
        for reader in readers:
            reader.join()
        returncode = process.wait()
        if outcome not in ("TIMEOUT", "INTERRUPTED"):
            outcome = "PASS" if returncode == 0 else "FAIL"
    except OSError as error:
        failure = error
        if outcome not in ("TIMEOUT", "INTERRUPTED"):
            outcome = "FAIL"
            returncode = 127
    finally:
        original_exception = sys.exc_info()[1]
        try:
            cleanup()
        except BaseException as cleanup_error:
            cleanup_failure = type(cleanup_error).__name__ + ": " + str(cleanup_error)
            tb = cleanup_error.__traceback__
            while tb.tb_next is not None:
                tb = tb.tb_next
            cleanup_failure += " at " + tb.tb_frame.f_code.co_name + ":" + str(tb.tb_lineno)
            # A secondary cleanup error must never replace the original callback
            # exception or turn an I/O FAIL into another classification.
            if original_exception is None and failure is None:
                raise
            if hasattr(original_exception or failure, "add_note"):
                (original_exception or failure).add_note("cleanup: " + str(cleanup_error))
        finally:
            if job is not None:
                job.close()
        if original_exception is not None:
            for stream in output_files:
                if stream is not None:
                    stream.close()
    # A signal may arrive while the last output/cleanup is finishing, after the
    # polling loop's last cancellation check. Do not report PASS in that race.
    if failure is None and outcome != "TIMEOUT" and (scope.cancelled or cancel.is_set()):
        outcome = "INTERRUPTED"
    if outcome == "TIMEOUT":
        returncode = 124
    elif outcome == "INTERRUPTED":
        returncode = -(scope.signal_number or signal.SIGTERM)
    try:
        if id(monitor) not in broken_streams:
            if failure is not None:
                display("spawn_or_io_error", error=str(failure))
            display(outcome, child_status=process.returncode if process is not None else None,
                    status=returncode)
    except OSError as error:
        failure = failure or error
        if outcome not in ("TIMEOUT", "INTERRUPTED"):
            outcome, returncode = "FAIL", 127
    except BaseException:
        for stream in output_files:
            if stream is not None:
                stream.close()
        if failure is None:
            raise
    for stream in output_files:
        if stream is not None:
            stream.seek(0)
    return Result(outcome, returncode, time.monotonic() - start, gap,
                  list(history), *output_files, interrupt_signal=scope.signal_number,
                  error=None if failure is None else type(failure).__name__ + ": " + str(failure),
                  cleanup_error=cleanup_failure)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv and argv[0] == "--_owned-child":
        return _gate_child(argv[1:])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", required=True)
    parser.add_argument("--timeout", type=float)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("a command is required after --")
    if args.timeout is not None and (not math.isfinite(args.timeout) or args.timeout <= 0):
        parser.error("--timeout must be finite and positive")
    with run(command, name=args.name, timeout=args.timeout) as result:
        status = result.returncode
    if status < 0 and os.name != "nt":
        # Preserve signal termination, not just a shell's conventional number.
        signal.signal(-status, signal.SIG_DFL)
        os.kill(os.getpid(), -status)
    return status


if __name__ == "__main__":
    sys.exit(main())
