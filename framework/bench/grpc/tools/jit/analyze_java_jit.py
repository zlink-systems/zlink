#!/usr/bin/env python3
"""Select chronological compilation-free windows from source and target JFR evidence.

Final validity requires the caller to have joined both JVMs successfully and to
provide their final recordings. Live dumps can only produce provisional results.
"""

import argparse
import json
import os
import re
import subprocess
import tempfile
from collections import defaultdict
from decimal import Decimal, InvalidOperation
from pathlib import Path


HERE = Path(__file__).resolve().parent


class JitAnalysisError(ValueError):
    """Missing or inconsistent evidence cannot certify a measurement."""


def integer(row, key, minimum=0):
    value = row.get(key)
    if type(value) is not int or not minimum <= value <= 2**63 - 1:
        raise JitAnalysisError(f"Invalid {key}: {value!r}")
    return value


def seconds_ns(value):
    try:
        nanos = Decimal(str(value)) * 1_000_000_000
        if not nanos.is_finite() or nanos < 0 or nanos != int(nanos):
            raise ValueError
        return int(nanos)
    except (InvalidOperation, ValueError, OverflowError) as error:
        raise JitAnalysisError(f"Invalid duration in seconds: {value!r}") from error


def parse_recording(events, role):
    groups = defaultdict(list)
    count = 0
    ended = False
    for event in events:
        if not isinstance(event, dict) or ended:
            raise JitAnalysisError(f"{role}: invalid data after reader end")
        kind = event.get("type")
        if kind == "end":
            if integer(event, "records") != count:
                raise JitAnalysisError(f"{role}: incomplete reader output")
            ended = True
            continue
        if kind not in ("anchor", "phase", "compilation", "deoptimization", "data_loss", "setting"):
            raise JitAnalysisError(f"{role}: unknown record type {kind!r}")
        groups[kind].append(event)
        count += 1
    if not ended:
        raise JitAnalysisError(f"{role}: missing reader end; output may be truncated")
    if groups["data_loss"]:
        raise JitAnalysisError(f"{role}: jdk.DataLoss occurred")
    settings = defaultdict(list)
    for event in groups["setting"]:
        settings[(event.get("event"), event.get("name"))].append(event.get("value"))
    for name in ("jdk.Compilation", "jdk.DataLoss"):
        enabled = settings[(name, "enabled")]
        if not enabled or any(value != "true" for value in enabled):
            raise JitAnalysisError(f"{role}: {name} must be enabled throughout the recording")
    thresholds = settings[("jdk.Compilation", "threshold")]
    if not thresholds or any(not isinstance(value, str) or not re.fullmatch(
            r"0(?:\.0+)?\s*(?:ns|us|ms|s|m|h|d)?", value) for value in thresholds):
        raise JitAnalysisError(f"{role}: Compilation threshold must be zero")
    if len(groups["anchor"]) != 1:
        raise JitAnalysisError(f"{role}: exactly one monotonic anchor is required")
    anchor = groups["anchor"][0]
    lower = integer(anchor, "lower_ns", -(2**63))
    upper = integer(anchor, "upper_ns", -(2**63))
    origin = integer(anchor, "timestamp_ns", 1)
    if upper < lower:
        raise JitAnalysisError(f"{role}: reversed anchor bounds")
    ids = set()
    for event in groups["compilation"] + groups["deoptimization"]:
        timestamp = integer(event, "timestamp_ns", 1)
        duration = integer(event, "duration_ns") if event["type"] == "compilation" else 0
        event = event.copy()
        event["start_lower_ns"] = lower + timestamp - origin
        event["end_upper_ns"] = upper + timestamp - origin + duration
        if not -(2**63) <= event["start_lower_ns"] <= event["end_upper_ns"] <= 2**63 - 1:
            raise JitAnalysisError(f"{role}: mapped event is outside signed monotonic time")
        if event["type"] == "compilation":
            compile_id = integer(event, "compile_id")
            if compile_id in ids:
                raise JitAnalysisError(f"{role}: duplicate compilation ID {compile_id}")
            ids.add(compile_id)
            integer(event, "level")
            if not isinstance(event.get("method"), str) or not event["method"]:
                raise JitAnalysisError(f"{role}: missing compilation method")
            if type(event.get("osr")) is not bool or type(event.get("succeeded")) is not bool:
                raise JitAnalysisError(f"{role}: invalid compilation flags")
            groups["mapped_compilations"].append(event)
        else:
            groups["mapped_deoptimizations"].append(event)
    phases = []
    for event in groups["phase"]:
        start = integer(event, "start_ns", -(2**63))
        duration = integer(event, "duration_ns")
        if event.get("phase") is None and event.get("cell_id") is None and start == duration == 0:
            continue  # MeasurementTiming registers the event type before recording its anchor.
        if event.get("phase") not in ("warmup", "active"):
            raise JitAnalysisError(f"{role}: unknown or missing phase name")
        if not isinstance(event.get("cell_id"), str) or not event["cell_id"]:
            raise JitAnalysisError(f"{role}: missing phase cell ID")
        if duration == 0 or start < lower or start + duration > 2**63 - 1:
            raise JitAnalysisError(f"{role}: invalid phase interval")
        phases.append(dict(event, end_ns=start + duration))
    phases.sort(key=lambda phase: phase["start_ns"])
    for previous, current in zip(phases, phases[1:]):
        if current["start_ns"] < previous["end_ns"]:
            raise JitAnalysisError(f"{role}: duplicate or overlapping phase windows")
    return {"anchor_uncertainty_ns": upper - lower,
            "phases": [phase for phase in phases if phase["phase"] == "active"],
            "compilations": groups["mapped_compilations"],
            "deoptimizations": groups["mapped_deoptimizations"]}


def analyze_recordings(source_events, target_events, *, required_clean=3, warmup_windows=6,
                       minimum_warmup_seconds=30, expected_active_seconds=5, provisional=False):
    """Analyze decoded reader records without using throughput to select windows."""
    if type(required_clean) is not int or required_clean < 1:
        raise JitAnalysisError("required_clean must be positive")
    if type(warmup_windows) is not int or warmup_windows < 0:
        raise JitAnalysisError("warmup_windows must be nonnegative")
    minimum_warmup_ns = seconds_ns(minimum_warmup_seconds)
    expected_ns = seconds_ns(expected_active_seconds)
    if expected_ns == 0:
        raise JitAnalysisError("expected_active_seconds must be positive")
    recordings = {"source": parse_recording(source_events, "source"),
                  "target": parse_recording(target_events, "target")}
    phases = recordings["source"]["phases"]
    if len(phases) <= warmup_windows:
        raise JitAnalysisError("Missing active measurement windows after warmup")
    if any(phase["duration_ns"] != expected_ns for phase in phases[warmup_windows:]):
        raise JitAnalysisError("Measurement duration differs from expected_active_seconds")
    warmup_ns = sum(phase["duration_ns"] for phase in phases[:warmup_windows])
    if warmup_ns < minimum_warmup_ns:
        raise JitAnalysisError("Insufficient active warmup duration")
    candidates = []
    for index, phase in enumerate(phases[warmup_windows:], warmup_windows + 1):
        start, end = phase["start_ns"], phase["end_ns"]
        window = {"window_index": index, "candidate_index": index - warmup_windows,
                  "cell_id": phase.get("cell_id", ""), "start_ns": start, "end_ns": end,
                  "roles": {}}
        for role, recording in recordings.items():
            compilations = [event for event in recording["compilations"]
                            if event["start_lower_ns"] < end and event["end_upper_ns"] > start]
            deoptimizations = [event for event in recording["deoptimizations"]
                               if event["start_lower_ns"] < end and event["end_upper_ns"] >= start]
            window["roles"][role] = {
                "count": len(compilations), "duration_ns": sum(e["duration_ns"] for e in compilations),
                "overlap_upper_ns": sum(min(end, e["end_upper_ns"]) - max(start, e["start_lower_ns"])
                                        for e in compilations),
                "anchor_uncertainty_ns": recording["anchor_uncertainty_ns"],
                "compilations": compilations, "deoptimizations": deoptimizations}
        window["clean"] = all(role["count"] == 0 for role in window["roles"].values())
        candidates.append(window)
    clean = [window for window in candidates if window["clean"]]
    selected = clean[:required_clean]
    discarded = [window for window in candidates if not window["clean"]]
    complete = len(selected) == required_clean
    return {"schema_version": 1, "valid": complete and not provisional,
            "complete": complete, "provisional": provisional, "required_clean": required_clean,
            "warmup_windows": warmup_windows, "warmup_duration_ns": warmup_ns,
            "candidate_count": len(candidates), "clean_count": len(clean),
            "discarded_count": len(discarded), "selected_windows": selected,
            "discarded_windows": discarded, "candidate_windows": candidates,
            "recording_totals": {role: {"compilations": len(r["compilations"]),
                                        "deoptimizations": len(r["deoptimizations"])}
                                 for role, r in recordings.items()},
            "finalization_requirement": "Both JVMs exited successfully; recordings belong to this condition.",
            "limitation": "Live dumps omit compilations that have not completed; final recordings must be rechecked."
                          if provisional else "Compilation durations are elapsed time, not CPU time."}


def java_tool(name):
    home = os.environ.get("JAVA_HOME")
    return str(Path(home) / "bin" / name) if home else name


def read_pair(source, target):
    """Decode both final recordings with a streaming Java reader in a temporary directory."""
    for path in (source, target):
        if not Path(path).is_file() or Path(path).stat().st_size == 0:
            raise JitAnalysisError(f"Missing or empty JFR: {path}")
    result = {}
    with tempfile.TemporaryDirectory(prefix="zlink-jit-reader-") as directory:
        command = [java_tool("javac"), "-d", directory, str(HERE / "ReadJit.java")]
        build = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if build.returncode:
            raise JitAnalysisError(f"JFR reader compilation failed: {build.stderr.strip()}")
        for role, path in (("source", source), ("target", target)):
            output = Path(directory) / f"{role}.jsonl"
            with output.open("w", encoding="utf-8") as stream:
                process = subprocess.run([java_tool("java"), "-cp", directory, "ReadJit", str(path)],
                                         stdout=stream, stderr=subprocess.PIPE, text=True)
            if process.returncode:
                raise JitAnalysisError(f"{role}: JFR decoding failed: {process.stderr.strip()}")
            try:
                with output.open(encoding="utf-8") as stream:
                    result[role] = [json.loads(line) for line in stream]
            except (ValueError, UnicodeError) as error:
                raise JitAnalysisError(f"{role}: invalid reader output") from error
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--target", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--required-clean", type=int, default=3)
    parser.add_argument("--warmup-windows", type=int, default=6)
    parser.add_argument("--minimum-warmup-seconds", default="30")
    parser.add_argument("--expected-active-seconds", default="5")
    parser.add_argument("--provisional", action="store_true")
    args = parser.parse_args(argv)
    try:
        evidence = read_pair(args.source, args.target)
        result = analyze_recordings(evidence["source"], evidence["target"],
                                    required_clean=args.required_clean, warmup_windows=args.warmup_windows,
                                    minimum_warmup_seconds=args.minimum_warmup_seconds,
                                    expected_active_seconds=args.expected_active_seconds,
                                    provisional=args.provisional)
        code = 0 if result["complete"] else 2
    except (JitAnalysisError, OSError) as error:
        result = {"schema_version": 1, "valid": False, "complete": False,
                  "provisional": args.provisional, "error": str(error)}
        code = 1
    result["recordings"] = {"source": str(args.source.resolve()), "target": str(args.target.resolve())}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
