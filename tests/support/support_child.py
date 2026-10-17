"""Tiny deterministic subprocess fixtures; no compiler work or production data."""

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time


mode = sys.argv[1]
if mode == "success":
    print("ok", flush=True)
elif mode == "nonzero":
    sys.exit(7)
elif mode == "signal":
    os.kill(os.getpid(), signal.SIGTERM)
elif mode == "stderr":
    print("stderr-only", file=sys.stderr, flush=True)
elif mode == "partial":
    os.write(1, b"partial-without-newline")
    time.sleep(1.22)
    os.write(1, b"-done\n")
elif mode == "quiet":
    time.sleep(1.22)
elif mode == "phase":
    os.write(2, b"SOL_TEST_PROGRESS\tevent=phase\tphase=fixture-build"
                b"\tcompleted=3\ttotal=7\tunit=fixture\n")
    time.sleep(1.22)
elif mode == "history":
    for completed in range(256):
        print("SOL_TEST_PROGRESS\tevent=units\tphase=count-fixtures\tcompleted="
              + str(completed) + "\tunit=fixture", file=sys.stderr, flush=True)
elif mode == "dual":
    def write(fd, value):
        for _ in range(4096):
            os.write(fd, value * 4096)
    writer = threading.Thread(target=write, args=(2, b"e"))
    writer.start()
    write(1, b"o")
    writer.join()
elif mode == "context":
    print(json.dumps(dict(argv=sys.argv[2:], cwd=os.getcwd(),
                          env=os.environ.get("SOL_PROGRESS_FIXTURE"))), flush=True)
elif mode == "grandchild":
    time.sleep(20)
elif mode == "tree":
    grandchild = subprocess.Popen([sys.executable, __file__, "grandchild"])

    def reap(_number, _frame):
        # The supervisor signals the WHOLE group. Reap our child to make proof of
        # tree cleanup independent of the host's orphan/zombie reaper latency.
        if os.name == "nt":
            grandchild.terminate()
        grandchild.wait(timeout=2)
        sys.exit(0)

    signal.signal(signal.SIGTERM, reap)
    if len(sys.argv) > 2:
        Path(sys.argv[2]).write_text(json.dumps(dict(parent=os.getpid(),
            grandchild=grandchild.pid, supervisor=os.getppid())))
    print(json.dumps(dict(parent=os.getpid(), grandchild=grandchild.pid)), flush=True)
    print("SOL_TEST_PROGRESS\tevent=phase\tphase=owned-tree-ready\tcompleted=0\tunit=tree",
          file=sys.stderr, flush=True)
    time.sleep(20)
elif mode == "ignore-term":
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    print("ready", flush=True)
    time.sleep(20)
else:
    raise SystemExit("unknown fixture mode: " + mode)
