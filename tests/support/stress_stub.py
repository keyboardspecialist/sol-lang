"""Synthetic sol replacement body; integration tests add a Python shebang."""
import os
from pathlib import Path
import sys
import time

print("stub-live-stdout", flush=True)
print("stub-live-stderr", file=sys.stderr, flush=True)
mode = os.environ.get("SOL_STRESS_STUB", "success")
if mode == "timeout":
    time.sleep(20)
if mode == "failure":
    sys.exit(7)
if mode == "wrong":
    print("wrong summary", flush=True)
elif Path(sys.argv[-1]).name == "generic-chain.sol":
    print(": 1000 declarations", flush=True)
elif Path(sys.argv[-1]).name == "validated-tests.sol":
    print("250 tests, 250 passed, 0 failed", flush=True)
else:
    print(": 128 files, 128 declarations", flush=True)
