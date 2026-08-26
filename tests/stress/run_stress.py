#!/usr/bin/env python3
import argparse
import hashlib
import json
import shutil
import subprocess
import time
from pathlib import Path


def write_generic_chain(root: Path, count: int) -> tuple[Path, str]:
    lines = ["module stress_generic"]
    for index in range(count):
        if index + 1 == count:
            body = "return value"
        else:
            body = f"return f{index + 1}(value)"
        lines.append(f"function f{index}<T>(value: T) -> T {{ {body} }}")
    data = ("\n".join(lines) + "\n").encode()
    path = root / "generic-chain.sol"
    path.write_bytes(data)
    return path, hashlib.sha256(data).hexdigest()


def write_validated_tests(root: Path, count: int) -> tuple[Path, str]:
    lines = [
        "module stress_tests",
        "function value() -> Int64 effects { pure } { return 1 }",
    ]
    lines.extend(f'test "case {index}" {{ value() == 1 }}' for index in range(count))
    data = ("\n".join(lines) + "\n").encode()
    path = root / "validated-tests.sol"
    path.write_bytes(data)
    return path, hashlib.sha256(data).hexdigest()


def write_package(root: Path, count: int) -> tuple[Path, str]:
    package = root / "package"
    package.mkdir()
    digest = hashlib.sha256()
    for index in range(count):
        relative = f"{index:04d}.sol"
        data = (
            f"module stress.m{index}\n"
            f"function value_{index}() -> Int64 {{ return {index} }}\n"
        ).encode()
        (package / relative).write_bytes(data)
        digest.update(relative.encode())
        digest.update(b"\0")
        digest.update(data)
    return package, digest.hexdigest()


def generate(root: Path, baseline: dict) -> dict[str, tuple[Path, str, list[str], str]]:
    root.mkdir(parents=True)
    generic = baseline["generic_chain"]
    tests = baseline["validated_tests"]
    package = baseline["package"]
    generic_path, generic_hash = write_generic_chain(root, generic["count"])
    tests_path, tests_hash = write_validated_tests(root, tests["count"])
    package_path, package_hash = write_package(root, package["count"])
    return {
        "generic_chain": (
            generic_path,
            generic_hash,
            ["check"],
            f': {generic["count"]} declarations',
        ),
        "validated_tests": (
            tests_path,
            tests_hash,
            ["test"],
            f'{tests["count"]} tests, {tests["count"]} passed, 0 failed',
        ),
        "package": (
            package_path,
            package_hash,
            ["check"],
            f': {package["count"]} files, {package["count"]} declarations',
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sol", type=Path)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--print-hashes", action="store_true")
    parser.add_argument("--enforce-time", action="store_true")
    arguments = parser.parse_args()
    baseline = json.loads(arguments.baseline.read_text())
    if arguments.work_dir.exists():
        shutil.rmtree(arguments.work_dir)
    cases = generate(arguments.work_dir, baseline)
    if arguments.print_hashes:
        print(json.dumps({name: case[1] for name, case in cases.items()}, indent=2))
        return 0
    if arguments.sol is None:
        parser.error("--sol is required unless --print-hashes is used")
    environment = {"LC_ALL": "C", "LANG": "C"}
    for name, (path, digest, command, expected) in cases.items():
        expected_digest = baseline[name]["sha256"]
        if digest != expected_digest:
            raise SystemExit(f"{name}: generated SHA-256 {digest} != {expected_digest}")
        started = time.monotonic()
        result = subprocess.run(
            [str(arguments.sol), *command, str(path)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=baseline[name]["max_seconds"] if arguments.enforce_time else None,
            env=environment,
            check=False,
        )
        elapsed = time.monotonic() - started
        if result.returncode != 0 or expected not in result.stdout:
            raise SystemExit(
                f"{name}: failed ({result.returncode})\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
            )
        if arguments.enforce_time and elapsed > baseline[name]["max_seconds"]:
            raise SystemExit(f"{name}: {elapsed:.3f}s exceeded baseline")
        print(f"{name}: {elapsed:.3f}s sha256={digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
