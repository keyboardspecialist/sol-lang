"""Two imported worker supervisors, with explicit main-thread cancellation owner.

Synthetic orphan regression only. PID files identify ONLY this fixture's owned
processes. Parent tests never search names or enumerate unrelated processes.
"""
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import signal
import sys
import time

import run_with_progress as supervisor


directory = Path(sys.argv[1])
directory.mkdir(exist_ok=True)
(directory / 'owner.json').write_text(json.dumps(dict(owner=os.getpid(), supervisor=os.getppid())))


def worker(index):
    command = [sys.executable, str(Path(__file__).with_name('support_child.py')),
               'tree', str(directory / ('tree-' + str(index) + '.json'))]
    with supervisor.run(command, name='worker-' + str(index), timeout=10,
                        scope=scope) as result:
        return result.returncode


with supervisor.cancellation_scope() as scope, ThreadPoolExecutor(max_workers=2) as pool:
    futures = [pool.submit(worker, index) for index in range(2)]
    while not scope.cancelled and not all((directory / ('tree-' + str(i) + '.json')).exists()
                                         for i in range(2)):
        time.sleep(0.005)
    if not scope.cancelled:
        print('WORKERS_READY', flush=True)
    results = [future.result() for future in futures]
if scope.signal_number:
    signal.signal(scope.signal_number, signal.SIG_DFL)
    os.kill(os.getpid(), scope.signal_number)
sys.exit(0 if all(status == 0 for status in results) else 1)
