#!/usr/bin/env python3
"""Build, test and remove CMake executable shards on a Ninja sanitizer build."""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
import shutil
import subprocess
import sys
import threading
import time
import xml.etree.ElementTree as ET
from pathlib import Path


def load_summary():
    spec = importlib.util.spec_from_file_location(
        "sanitizer_summary", Path(__file__).with_name("sanitizer-summary.py")
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


summary = load_summary()


def inside(build: Path, path: str) -> Path:
    resolved = (build / path).resolve()
    if not resolved.is_relative_to(build) or resolved == build:
        raise ValueError(f"Path is outside the build tree: {resolved}")
    return resolved


def read_targets(build: Path) -> list[dict]:
    reply = build / ".cmake/api/v1/reply"
    index = json.loads(sorted(reply.glob("index-*.json"))[-1].read_text())
    model = json.loads(
        (reply / index["reply"]["codemodel-v2"]["jsonFile"]).read_text()
    )
    configurations = model["configurations"]
    if len(configurations) != 1:
        raise ValueError("Only a single-configuration Ninja build is supported.")
    return [
        json.loads((reply / target["jsonFile"]).read_text())
        for target in configurations[0]["targets"]
    ]


def discover(build: Path, targets: list[dict], label: str) -> list[dict]:
    # CTest omits commands for executables which have not been linked. Empty
    # artifact placeholders allow show-only discovery; they are never executed.
    placeholders = []
    try:
        for target in targets:
            if target["type"] != "EXECUTABLE":
                continue
            for artifact in target.get("artifacts", []):
                path = inside(build, artifact["path"])
                path.parent.mkdir(parents=True, exist_ok=True)
                try:
                    with path.open("xb"):
                        pass
                except FileExistsError:
                    continue
                placeholders.append(path)
                path.chmod(0o755)
        command = ["ctest", "--test-dir", str(build), "--show-only=json-v1", "-L", label]
        tests = json.loads(subprocess.check_output(command, text=True))["tests"]
    finally:
        for path in placeholders:
            path.unlink()
    if not tests or len({test["name"] for test in tests}) != len(tests):
        raise ValueError("CTest selection is empty or contains duplicate names.")
    for test in tests:
        if not test.get("command"):
            raise ValueError(f"CTest command cannot be resolved: {test['name']}")
        properties = {p["name"]: p["value"] for p in test.get("properties", [])}
        if any(key.startswith("FIXTURES_") for key in properties):
            raise ValueError(f"CTest fixtures require a shard policy: {test['name']}")
        if properties.get("DISABLED"):
            raise ValueError(f"Selected test is disabled: {test['name']}")
    return tests


def make_shards(build: Path, targets: list[dict], tests: list[dict], size: int) -> list[dict]:
    executables = {t["id"]: t for t in targets if t["type"] == "EXECUTABLE"}
    artifacts = {
        key: [inside(build, a["path"]).as_posix() for a in t.get("artifacts", [])]
        for key, t in executables.items()
    }
    groups = []
    for test in tests:
        command = "\n".join(test["command"]).replace("\\", "/")
        required = {
            key for key, paths in artifacts.items()
            if any(re.search(r"(?<![\w./+-])" + re.escape(path) + r"(?![\w./+-])", command)
                   for path in paths)
        }
        # An executable dependency must remain available for all tests using it.
        pending = list(required)
        while pending:
            for dependency in executables[pending.pop()].get("dependencies", []):
                key = dependency["id"]
                if key in executables and key not in required:
                    required.add(key)
                    pending.append(key)
        group = {"tests": [test["name"]], "targets": required}
        for previous in groups[:]:
            if previous["targets"] & group["targets"]:
                group["tests"].extend(previous["tests"])
                group["targets"].update(previous["targets"])
                groups.remove(previous)
        groups.append(group)
    shards = []
    for group in groups:
        if not shards or len(shards[-1]["targets"] | group["targets"]) > size:
            shards.append({"tests": [], "targets": set()})
        shards[-1]["tests"].extend(group["tests"])
        shards[-1]["targets"].update(group["targets"])
    return shards


def disk(stage: str, build: Path) -> None:
    print(f"DISK {stage}", flush=True)
    subprocess.run(["df", "-h", "/", str(Path.cwd()), str(build)], check=True)
    if stage == "after-libraries":
        subprocess.run(["du", "-sh", str(build)], check=True)


def build_targets(build: Path, names: list[str], parallel: int, log: Path) -> int:
    if not names:
        log.write_text("No executable targets in this shard.\n", encoding="utf-8")
        return 0
    command = ["cmake", "--build", str(build), "--parallel", str(parallel),
               "--target", *names, "--", "-k", "0"]
    stopped = threading.Event()
    samples = []

    def sample_disk():
        while True:
            usage = shutil.disk_usage(build)
            samples.append({"time": time.time(), "used_bytes": usage.used,
                            "free_bytes": usage.free})
            if stopped.wait(5):
                break

    monitor = threading.Thread(target=sample_disk, daemon=True)
    monitor.start()
    try:
        with log.open("w", encoding="utf-8") as output:
            return subprocess.run(command, stdout=output, stderr=subprocess.STDOUT).returncode
    finally:
        stopped.set()
        monitor.join()
        usage = shutil.disk_usage(build)
        samples.append({"time": time.time(), "used_bytes": usage.used, "free_bytes": usage.free})
        log.with_suffix(".disk.json").write_text(json.dumps(samples, indent=2) + "\n",
                                                encoding="utf-8")
        print(f"BUILD DISK_PEAK {log.name} used_bytes={max(s['used_bytes'] for s in samples)} "
              f"min_free_bytes={min(s['free_bytes'] for s in samples)} interval_seconds=5",
              flush=True)


def remove_artifacts(build: Path, targets: list[dict]) -> int:
    removed = 0
    for target in targets:
        paths = [inside(build, a["path"]) for a in target.get("artifacts", [])]
        objects = inside(build, f"{target['paths']['build']}/CMakeFiles/{target['name']}.dir")
        paths.extend(objects.rglob("*.o"))
        for path in paths:
            if path.is_file():
                removed += path.stat().st_size
                path.unlink()
    return removed


def coverage(junit: Path, expected: list[str]) -> list[str]:
    try:
        actual = [case.get("name") for case in ET.parse(junit).getroot().iter("testcase")]
    except (OSError, ET.ParseError) as error:
        return [f"Could not verify CTest coverage: {error}"]
    if sorted(actual) == sorted(expected):
        return []
    return [f"Expected each selected test once. Expected: {expected}; observed: {actual}"]


def failed_outputs(log: Path) -> list[str]:
    return re.findall(r"^FAILED: (.+)$", log.read_text(errors="replace"), re.M)


def run(args, results: list[dict]) -> None:
    build = args.build_dir.resolve()
    targets = read_targets(build)
    by_id = {t["id"]: t for t in targets}
    tests = discover(build, targets, args.label)
    shards = make_shards(build, targets, tests, args.shard_size)
    plan = [{"tests": s["tests"], "targets": sorted(by_id[k]["name"] for k in s["targets"])}
            for s in shards]
    (args.log_dir / f"{args.suite}.plan.json").write_text(
        json.dumps(plan, indent=2) + "\n", encoding="utf-8"
    )
    print(f"SHARD PLAN {args.suite}: tests={len(tests)} shards={len(shards)} "
          f"target_limit={args.shard_size}", flush=True)
    disk("before-libraries", build)
    libraries = [t["name"] for t in targets if t["type"] in {
        "STATIC_LIBRARY", "SHARED_LIBRARY", "MODULE_LIBRARY", "OBJECT_LIBRARY"
    }]
    library_log = args.log_dir / f"{args.suite}.libraries.build.log"
    code = build_targets(build, libraries, args.parallel, library_log)
    if code:
        results.append({"test": f"{args.suite}::build::libraries", "returncode": code,
                        "assertions": failed_outputs(library_log) or
                        ["See the libraries build log for failed targets."]})
    disk("after-libraries", build)
    for index, shard in enumerate(shards, 1):
        stem = f"{args.suite}.shard-{index:03d}"
        selected = [by_id[key] for key in sorted(shard["targets"])]
        print(f"SHARD {index}/{len(shards)} tests={len(shard['tests'])} "
              f"targets={[t['name'] for t in selected]}", flush=True)
        disk(f"{stem} before-build", build)
        try:
            build_log = args.log_dir / f"{stem}.build.log"
            code = build_targets(build, [t["name"] for t in selected], args.parallel, build_log)
            if code:
                failed = failed_outputs(build_log)
                results.append({"test": f"{args.suite}::build::shard-{index:03d}",
                                "returncode": code,
                                "assertions": failed or ["Build failed; see shard build log."]})
            disk(f"{stem} after-build", build)
            # Group every use of one executable together so deletion never
            # causes a later test to relink it. Anchors avoid prefix matches.
            regex = "^(" + "|".join(re.escape(n) for n in shard["tests"]) + ")$"
            results.extend(summary.run_suite(
                args.suite, build, args.label, args.log_dir, test_regex=regex, log_name=stem
            ))
            errors = coverage(args.log_dir / f"{stem}.junit.xml", shard["tests"])
            print(f"SHARD COVERAGE {stem} expected={len(shard['tests'])} "
                  f"exactly_once={not errors}", flush=True)
            if errors:
                results.append({"test": f"{args.suite}::coverage::shard-{index:03d}",
                                "returncode": 1, "assertions": errors})
        finally:
            removed = remove_artifacts(build, selected)
            print(f"SHARD CLEANUP {stem} removed_bytes={removed}", flush=True)
            disk(f"{stem} after-cleanup", build)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", default="framework")
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--label", default="^(unit|contract)$")
    parser.add_argument("--shard-size", type=int, default=10)
    parser.add_argument("--parallel", type=int, default=2)
    parser.add_argument("--log-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.shard_size < 1 or args.parallel < 1:
        parser.error("shard-size and parallel must be positive")
    args.log_dir.mkdir(parents=True, exist_ok=True)
    results = []
    try:
        run(args, results)
    except Exception as error:
        results.append({"test": f"{args.suite}::shard-runner", "returncode": 1,
                        "assertions": [f"{type(error).__name__}: {error}"]})
        print(f"SHARD ERROR {error}", file=sys.stderr)
    report = summary.summarize(results)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"SHARD SUMMARY {args.output}: total={report['total']} "
          f"failed={len(report['failed'])}", flush=True)
    return 1 if report["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
