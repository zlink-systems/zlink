#!/usr/bin/env python3
"""Collect serial Java benchmark windows with final-recording JIT validation.

Each JVM pair first runs 80 short active phases and two full-length active
phases: 30 seconds of active warmup, including the phase control path. Subsequent
windows are selected only by compilation overlap, never by throughput. Snapshot
selections are provisional until both JVMs exit normally.
"""

import argparse
from contextlib import ExitStack
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import time


HERE = Path(__file__).resolve().parent
BENCH = HERE.parent.parent
COMMON = BENCH / "runner_common.sh"
ANALYZER = HERE / "analyze_java_jit.py"
SETTINGS = HERE / "compilation.jfc"
PATTERNS = ("send-saturation", "request-serial", "request-backpressure")
SERVERS = {
    "zlink-java": "rawserver.ZLinkRawBenchServer",
    "zlink-framework-java": "frameworkserver.ZLinkFrameworkBenchServer",
}
GROUP_WINDOWS = 3
ACTIVE_MS = 5000
WARMUP_PLAN = (250,) * 80 + (ACTIVE_MS,) * 2
WARMUP_WINDOWS = len(WARMUP_PLAN)
REQUIRED_CLEAN = 3


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def hashes(classpath):
    result = {}
    for entry in classpath.split(os.pathsep):
        path = Path(entry).resolve(strict=True)
        if path.is_file():
            result[str(path)] = digest(path)
        else:
            for child in sorted(path.rglob("*")):
                if child.is_file():
                    result[str(child)] = digest(child)
    return result


def shell(log, script, *arguments):
    command = ["bash", "-c", 'set -euo pipefail\nsource "$1"\nshift\n' + script,
               "java-jit-phase", str(COMMON), *map(str, arguments)]
    log.write("COMMAND " + json.dumps(command) + "\n")
    log.flush()
    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)


def validate_metric(metric, duration_ms=ACTIVE_MS):
    if (metric.get("contaminated") or metric.get("drain_bound_hit")
            or metric["active_elapsed_ms"] != duration_ms
            or metric["errors"] != 0 or metric["abandoned"] != 0
            or metric["target_stats"]["errors"] != 0):
        raise ValueError("invalid benchmark result: " + json.dumps(metric))


def run_phase(out, log, urls, run_id, cell, pattern, index, duration_ms):
    trigger, source_url, target_url = urls
    # The repository controller returns this POST only after runPhase finishes.
    # There is no source or target stats request while this call is in progress.
    shell(log, 'trigger_phase "$1" "$2" "$3" "$4" 1024 active "$5"',
          trigger, run_id, cell, pattern, duration_ms)
    result = out / "results.json"
    target_file = out / "target-stats.json"
    shell(log,
          'remaining=$(python3 - "$4" "$DRAIN_BOUND_MS" <<\'PY\'\n'
          'import json, math, sys\n'
          'with open(sys.argv[1]) as stream:\n'
          '    cell = json.load(stream)["cells"][0]\n'
          'print(max(0, int(sys.argv[2]) - math.ceil(float(\n'
          '    cell.get("source_drain_ms", cell.get("drain_ms", 0))))))\n'
          'PY\n)\n'
          'settle_and_capture "$1" "$2" "$3" received "$remaining"\n'
          'merge_target_stats "$4" "$3" "$SETTLE_MS" "$SETTLE_BOUND_HIT"\n'
          'verify_request_counts "$4"',
          source_url, target_url, target_file, result)
    metric = json.loads(result.read_text())["cells"][0]
    window = out / f"window-{index:04d}"
    window.mkdir()
    for name in ("results.json", "target-stats.json", "report.txt"):
        path = out / name
        if path.exists():
            path.rename(window / name)
    validate_metric(metric, duration_ms)
    return {"window_index": index, "cell_id": cell, "metric": metric,
            "path": str(window), "warmup": index <= WARMUP_WINDOWS}


def native_evidence(processes, out, expected):
    result = {}
    for role, process in processes.items():
        maps = Path(f"/proc/{process.pid}/maps").read_text()
        lines = [line for line in maps.splitlines() if "libzlink" in line]
        (out / f"{role}-native-mappings.txt").write_text("\n".join(lines) + "\n")
        paths = {Path(line.split()[-1]) for line in lines}
        observed = {str(path): digest(path) for path in paths}
        if not observed or set(observed.values()) != expected:
            raise ValueError(f"{role} native library mismatch: {observed}; expected {expected}")
        result[role] = observed
    save(out / "native-hashes.json", result)


def analyze(args, out, source, target, required, provisional):
    command = ["taskset", "-c", args.analysis_cpus, sys.executable, str(ANALYZER),
               "--source", str(source), "--target", str(target), "--output", str(out),
               "--required-clean", str(required), "--warmup-windows", str(WARMUP_WINDOWS),
               "--minimum-warmup-seconds", "30", "--expected-active-seconds", "5"]
    if provisional:
        command.append("--provisional")
    with out.with_suffix(".log").open("w") as log:
        completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    if completed.returncode not in (0, 2):
        raise RuntimeError(f"JFR analysis failed ({completed.returncode}): {out.with_suffix('.log')}")
    value = json.loads(out.read_text())
    if provisional and value.get("valid"):
        raise ValueError("snapshot analysis cannot be final evidence")
    return value


def expired(args):
    return datetime.now(timezone.utc) >= args.deadline


def validate_windows(summary, windows):
    """Bind every candidate marker, including discarded ones, to its actual result."""
    expected = [window for window in windows if not window["warmup"]]
    candidates = summary["candidate_windows"]
    if summary["candidate_count"] != len(expected) or len(candidates) != len(expected):
        raise ValueError("JFR marker and benchmark result counts differ")
    for evidence, window in zip(candidates, expected):
        metric = window["metric"]
        if (evidence["window_index"] != window["window_index"]
                or evidence["cell_id"] != window["cell_id"]
                or evidence["start_ns"] != metric["active_started_at_ns"]
                or evidence["end_ns"] != metric["active_ended_at_ns"]):
            raise ValueError("JFR marker and benchmark result identity or active boundary mismatch")
    by_index = {evidence["window_index"]: evidence for evidence in candidates}
    for evidence in summary["selected_windows"]:
        if evidence != by_index.get(evidence["window_index"]) or not evidence["clean"]:
            raise ValueError("selected JFR window is not a clean candidate")


def verify_inputs(args, classpath):
    if hashes(args.harness_classpath) != args.harness_hashes:
        raise ValueError("harness classpath changed during collection")
    if {path: digest(Path(path)) for path in args.tool_hashes} != args.tool_hashes:
        raise ValueError("measurement tool or JFR settings changed during collection")
    value = hashes(classpath)
    if any(value.get(path) != sha for path, sha in args.product_hashes.items()
           if path in classpath.split(os.pathsep)):
        raise ValueError("product classpath changed during collection")
    return value


def run_attempt(args, version, commit, classpath, pattern, implementation,
                out, attempt, required, expected_native):
    out.mkdir()
    java = str(Path(args.java_home) / "bin/java")
    jcmd = str(Path(args.java_home) / "bin/jcmd")
    port = args.base_port
    trigger = f"http://127.0.0.1:{port}"
    source_url = f"http://127.0.0.1:{port + 1}"
    endpoint = f"tcp://127.0.0.1:{port + 2}"
    command_endpoint = f"tcp://127.0.0.1:{port + 3}"
    target_url = f"http://127.0.0.1:{port + 4}"
    run_id = f"{args.output.name}-{version}-{pattern}-{implementation}-{attempt}"
    environment = os.environ.copy()
    environment.update(PROFILE_ENABLED="0", BENCH_COMMIT=commit)

    def launch(role):
        return [java, "--enable-native-access=ALL-UNNAMED",
                f"-XX:StartFlightRecording=name=compilation,settings={args.settings},"
                f"filename={out / (role + '.jfr')},dumponexit=true", "-cp", classpath]

    commands = {
        "target": launch("target") + ["systems.zlink.bench.withgrpc." + SERVERS[implementation],
            "--endpoint", endpoint, "--metrics-url", target_url,
            "--command-endpoint", command_endpoint],
        "source": launch("source") + ["systems.zlink.bench.withgrpc.client.BenchClient",
            "--implementation", implementation, "--scenario", pattern, "--payload-size", "1024",
            "--warmup-seconds", "30", "--trigger-url", trigger, "--stats-url", source_url,
            "--target-endpoint", endpoint, "--target-command-endpoint", command_endpoint,
            "--target-stats-url", target_url, "--run-id", run_id,
            "--cell-id", pattern, "--output", str(out)],
    }
    metadata = {
        "version": version, "artifactCommit": commit, "pattern": pattern,
        "implementation": implementation, "attempt": attempt, "startedAt": utc_now(),
        "affinity": sorted(os.sched_getaffinity(0)), "payload": 1024,
        "warmupActiveWindows": WARMUP_WINDOWS, "activeDurationMs": ACTIVE_MS,
        "warmupPlanMs": WARMUP_PLAN, "warmupActiveMs": sum(WARMUP_PLAN),
        "selection": "first clean windows in chronological order, final JFR validation",
        "officialBenchProtocol": False, "classpath": classpath,
        "classpathHashes": verify_inputs(args, classpath), "commands": commands,
        "toolHashes": args.tool_hashes,
        "environment": {key: environment.get(key) for key in
            ("JAVA_HOME", "JAVA_TOOL_OPTIONS", "JDK_JAVA_OPTIONS", "_JAVA_OPTIONS",
             "ZLINK_JAVA_NATIVE_CACHE", "XDG_CACHE_HOME", "ZLINK_CORE_CACHE_DIR")},
        "loadStart": Path("/proc/loadavg").read_text().strip(),
    }
    if metadata["affinity"] != [0, 1, 2, 3]:
        raise ValueError("run under the measurement lock on CPUs 0-3")
    save(out / "metadata.json", metadata)
    processes = {}
    windows = []
    failure = None
    provisional = None
    started = time.monotonic()
    print("START", version, pattern, implementation, attempt, flush=True)
    with ExitStack() as streams:
        log = streams.enter_context((out / "runner.log").open("w"))
        try:
            shell(log, 'check_ports_free "$1" "$2"', port, port + 4)
            for role in ("target", "source"):
                stream = streams.enter_context((out / f"{role}.log").open("w"))
                process = subprocess.Popen(commands[role], env=environment, cwd=BENCH,
                    stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT)
                processes[role] = process
            save(out / "processes.json", {role: {"pid": process.pid,
                 "startTicks": Path(f"/proc/{process.pid}/stat").read_text().split()[21]}
                 for role, process in processes.items()})
            shell(log, 'wait_for_stats "$1" 0\nwait_for_stats "$2" 1', target_url, source_url)
            native_evidence(processes, out, expected_native)
            for role, process in processes.items():
                with (out / f"{role}-vm-command-line.txt").open("w") as vm_log:
                    subprocess.run(["taskset", "-c", args.analysis_cpus, jcmd,
                        str(process.pid), "VM.command_line"], stdout=vm_log,
                        stderr=subprocess.STDOUT, check=True)
            index = 0
            while not expired(args):
                durations = WARMUP_PLAN if index == 0 else (ACTIVE_MS,) * GROUP_WINDOWS
                for duration_ms in durations:
                    if expired(args):
                        break
                    index += 1
                    cell = f"{pattern}-window-{index:04d}"
                    windows.append(run_phase(out, log, (trigger, source_url, target_url),
                                             run_id, cell, pattern, index, duration_ms))
                    save(out / "windows.json", windows)
                    print("WINDOW", version, pattern, implementation, attempt, index, flush=True)
                if index <= WARMUP_WINDOWS:
                    continue
                group = out / f"snapshot-{index:04d}"
                group.mkdir()
                for role, process in processes.items():
                    with (group / f"{role}-dump.log").open("w") as dump_log:
                        subprocess.run(["taskset", "-c", args.analysis_cpus, jcmd,
                            str(process.pid), "JFR.dump", "name=compilation",
                            f"filename={group / (role + '.jfr')}"], stdout=dump_log,
                            stderr=subprocess.STDOUT, check=True)
                provisional = analyze(args, group / "summary.json", group / "source.jfr",
                                      group / "target.jfr", required, True)
                validate_windows(provisional, windows)
                if len(provisional.get("selected_windows", [])) >= required:
                    break
        except BaseException as error:
            failure = repr(error)
            raise
        finally:
            release_errors = []
            # Both endpoints own their cleanup; there are no signals or forced exits.
            for role in ("source", "target"):
                process = processes.get(role)
                if process and process.poll() is None:
                    try:
                        process.stdin.write(b"\n")
                        process.stdin.flush()
                        process.stdin.close()
                    except (BrokenPipeError, OSError) as error:
                        release_errors.append({"role": role, "error": repr(error)})
            exits = {role: process.wait() for role, process in processes.items()}
            status = {"exits": exits, "failure": failure, "releaseErrors": release_errors,
                      "normalStdinReleaseAttempted": True, "endedAt": utc_now(),
                      "elapsedSeconds": time.monotonic() - started,
                      "loadEnd": Path("/proc/loadavg").read_text().strip()}
            save(out / "status.json", status)
    if exits != {"target": 0, "source": 0}:
        raise RuntimeError(f"JVM exit failure: {status}")
    verify_inputs(args, classpath)
    if len(windows) <= WARMUP_WINDOWS:
        summary = {"valid": False, "selected_windows": [],
                   "reason": "deadline reached before warmup and candidate completion"}
        save(out / "summary.json", summary)
        return {"selected": [], "discardedCount": 0, "candidateCount": 0}
    summary = analyze(args, out / "summary.json", out / "source.jfr",
                      out / "target.jfr", required, False)
    validate_windows(summary, windows)
    by_index = {window["window_index"]: window for window in windows}
    selected = []
    for evidence in summary.get("selected_windows", []):
        window = by_index[evidence["window_index"]]
        if window["warmup"] or evidence["cell_id"] != window["cell_id"]:
            raise ValueError("JFR marker and benchmark result identity mismatch")
        selected.append({**window, "jit": evidence, "attempt": attempt,
                         "finalRecording": str(out / "summary.json")})
    save(out / "selected.json", selected)
    print("FINAL", version, pattern, implementation, "clean", len(selected), flush=True)
    return {"selected": selected, "discardedCount": summary["discarded_count"],
            "candidateCount": summary["candidate_count"]}


def run_condition(args, version, pattern, implementation, expected_native):
    out = args.output / version / pattern / implementation
    out.mkdir(parents=True)
    commit = (args.classpath_dir / f"{version}.commit").read_text().strip()
    products = (args.classpath_dir / f"{version}.cp").read_text().strip()
    classpath = args.harness_classpath + os.pathsep + products
    selected = []
    attempt = 0
    discarded = candidates = 0
    while len(selected) < REQUIRED_CLEAN and not expired(args):
        attempt += 1
        result = run_attempt(args, version, commit, classpath, pattern, implementation,
            out / f"attempt-{attempt:03d}", attempt, REQUIRED_CLEAN - len(selected), expected_native)
        selected.extend(result["selected"])
        discarded += result["discardedCount"]
        candidates += result["candidateCount"]
        save(out / "selected.json", selected)
    value = {"version": version, "commit": commit, "pattern": pattern,
             "implementation": implementation, "complete": len(selected) == REQUIRED_CLEAN,
             "selected": selected, "attempts": attempt, "discardedCount": discarded,
             "candidateCount": candidates}
    save(out / "condition.json", value)
    return value


def summarize(args, conditions):
    ratios = []
    for version in args.versions:
        for pattern in args.patterns:
            pair = {c["implementation"]: c for c in conditions
                    if c["version"] == version and c["pattern"] == pattern}
            complete = all(implementation in pair and pair[implementation]["complete"]
                           for implementation in SERVERS)
            values = []
            if complete:
                for binding, framework in zip(pair["zlink-java"]["selected"],
                                             pair["zlink-framework-java"]["selected"]):
                    values.append(framework["metric"]["throughput_per_second"]
                                  / binding["metric"]["throughput_per_second"])
            ratios.append({"version": version, "pattern": pattern, "complete": complete,
                           "ratios": values, "medianRatio": statistics.median(values) if values else None})
    expected = len(args.versions) * len(args.patterns) * len(args.implementations)
    value = {"complete": len(conditions) == expected and all(c["complete"] for c in conditions),
             "conditions": conditions, "ratios": ratios, "updatedAt": utc_now(),
             "discardedCount": sum(c["discardedCount"] for c in conditions),
             "candidateCount": sum(c["candidateCount"] for c in conditions),
             "stopBeforeUtc": args.deadline.isoformat()}
    save(args.output / "summary.json", value)
    return value


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--classpath-dir", type=Path, required=True)
    parser.add_argument("--harness-classpath", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--versions", default="main,pr2354,pr2608")
    parser.add_argument("--patterns", default=",".join(PATTERNS))
    parser.add_argument("--implementations", default=",".join(SERVERS))
    parser.add_argument("--stop-before-utc", required=True,
                        help="ISO UTC deadline; active phases and normal cleanup finish before returning")
    parser.add_argument("--java-home", default=os.environ.get("JAVA_HOME"))
    parser.add_argument("--analysis-cpus", default="4-15")
    parser.add_argument("--settings", type=Path, default=SETTINGS,
                        help="JFR settings, including profile.jfc for allocation/CPU evidence")
    parser.add_argument("--base-port", type=int, default=48500)
    args = parser.parse_args()
    if not args.java_home:
        parser.error("JAVA_HOME or --java-home is required")
    for name, allowed in (("versions", None), ("patterns", PATTERNS),
                          ("implementations", SERVERS)):
        values = getattr(args, name).split(",")
        if any(not value or "/" in value or value in (".", "..") for value in values):
            parser.error(f"invalid {name}")
        if len(set(values)) != len(values) or (allowed and any(value not in allowed for value in values)):
            parser.error(f"invalid or duplicated {name}")
        setattr(args, name, values)
    args.deadline = datetime.fromisoformat(args.stop_before_utc.replace("Z", "+00:00"))
    if args.deadline.tzinfo is None:
        parser.error("--stop-before-utc must include UTC timezone")
    args.output = args.output.resolve()
    args.classpath_dir = args.classpath_dir.resolve()
    args.settings = args.settings.resolve(strict=True)
    return args


def main():
    args = parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    manifest_path = args.classpath_dir / "artifact-manifest.json"
    manifest = json.loads(manifest_path.read_text())
    expected_native = set(manifest["nativeHashes"].values())
    if len(expected_native) != 1:
        raise ValueError("exactly one native library SHA256 is required")
    args.product_hashes = {}
    for version in args.versions:
        commit = (args.classpath_dir / f"{version}.commit").read_text().strip()
        if commit != manifest["versions"][version]:
            raise ValueError(f"commit identity mismatch: {version}")
        entries = (args.classpath_dir / f"{version}.cp").read_text().strip().split(os.pathsep)
        actual = {Path(entry).name: digest(Path(entry)) for entry in entries}
        expected = {name: value["sha256"] for name, value in manifest["jars"][version].items()}
        if len(entries) != len(actual) or actual != expected:
            raise ValueError(f"classpath artifact mismatch: {version}")
        args.product_hashes.update({str(Path(entry).resolve()): actual[Path(entry).name]
                                    for entry in entries})
    args.harness_hashes = hashes(args.harness_classpath)
    args.tool_hashes = {str(path): digest(path) for path in
                       (Path(__file__), COMMON, ANALYZER, args.settings, HERE / "ReadJit.java")}
    shutil.copy2(manifest_path, args.output / "artifact-manifest.json")
    conditions = []
    try:
        for version in args.versions:
            for pattern in args.patterns:
                for implementation in args.implementations:
                    if expired(args):
                        return 2
                    condition = run_condition(args, version, pattern, implementation, expected_native)
                    conditions.append(condition)
                    summarize(args, conditions)
        return 0 if all(condition["complete"] for condition in conditions) else 2
    except Exception as error:
        save(args.output / "failure.json", {"error": repr(error), "at": utc_now()})
        print("ERROR", repr(error), file=sys.stderr, flush=True)
        return 1
    finally:
        summarize(args, conditions)


if __name__ == "__main__":
    raise SystemExit(main())
