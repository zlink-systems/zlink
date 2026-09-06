"""Canonical histogram and owner aggregation; no averaging of process percentiles."""
from __future__ import annotations

import copy
import json
import math
from pathlib import Path

BOUNDS = json.loads((Path(__file__).resolve().parents[1] / "ZLink.Framework.Perf.Shared/histogram-bounds.json").read_text())
OUTCOMES = ("sent", "completed", "settleCompleted", "failed", "timeout", "cancelled", "unresolved")
MAX_U64 = 18446744073709551615


def u64(value: str) -> int:
    if not isinstance(value, str) or not value.isascii() or not value.isdecimal() or str(int(value)) != value:
        raise ValueError("SchemaMismatch: noncanonical U64")
    integer = int(value)
    if integer > MAX_U64:
        raise ValueError("SchemaMismatch: U64 overflow")
    return integer


def count_text(value: int) -> str:
    if not 0 <= value <= MAX_U64:
        raise ValueError("SchemaMismatch: aggregate count overflow")
    return str(value)


def histogram_merge(values: list[dict]) -> dict:
    if not values:
        raise ValueError("CollectionFailure: no histogram owners")
    result = {"unit": "ms", "ticksUnit": "ns", "bounds": BOUNDS, "counts": ["0"] * len(BOUNDS),
              "overflow": "0", "count": "0", "sumNs": "0", "maxNs": None,
              "percentileMethod": "nearest-rank-bucket-upper-bound"}
    for value in values:
        if any(value[key] != result[key] for key in ("unit", "ticksUnit", "bounds", "percentileMethod")):
            raise ValueError("SchemaMismatch: histogram units, bounds or percentile method differ")
        if len(value["counts"]) != len(BOUNDS) or sum(map(u64, value["counts"])) + u64(value["overflow"]) != u64(value["count"]):
            raise ValueError("SchemaMismatch: histogram count does not reconcile")
        if not isinstance(value["sumNs"], str) or not value["sumNs"].isascii() or not value["sumNs"].isdecimal() or str(int(value["sumNs"])) != value["sumNs"]:
            raise ValueError("SchemaMismatch: sumNs is not arbitrary-precision decimal text")
        result["counts"] = [count_text(u64(a) + u64(b)) for a, b in zip(result["counts"], value["counts"])]
        for key in ("count", "overflow"):
            result[key] = count_text(u64(result[key]) + u64(value[key]))
        result["sumNs"] = str(int(result["sumNs"]) + int(value["sumNs"]))
        if value["maxNs"] is not None:
            result["maxNs"] = str(max(u64(value["maxNs"]), u64(result["maxNs"] or "0")))
        elif u64(value["count"]):
            raise ValueError("SchemaMismatch: nonempty histogram has null max")
    return result


def export_latency(histogram: dict, prefix: str, histogram_key: str, metrics: dict, reasons: dict) -> None:
    count = u64(histogram["count"])
    for suffix in ("meanMs", "p50Ms", "p95Ms", "p99Ms", "maxMs"):
        pointer = "/metrics/" + prefix + "." + suffix
        reasons.pop(pointer, None)
        value = None
        if count:
            if suffix == "meanMs":
                value = int(histogram["sumNs"]) / count / 1e6
            elif suffix == "maxMs":
                value = u64(histogram["maxNs"]) / 1e6
            else:
                rank = (int(suffix[1:3]) * count + 99) // 100
                cumulative = 0
                for bound, bucket in zip(BOUNDS, histogram["counts"]):
                    cumulative += u64(bucket)
                    if cumulative >= rank:
                        value = bound
                        break
        metrics[prefix + "." + suffix] = value
        if value is None:
            reasons[pointer] = {"code": "NO_SAMPLES" if not count else "HISTOGRAM_OVERFLOW",
                                "reason": "No successful samples." if not count else "Nearest rank is above the final bucket.",
                                "owner": "perf/README.ko.md §15.3"}
            if count:
                reasons[pointer]["lowerBoundMs"] = 1024
    pointer = "/histograms/" + histogram_key + "/maxNs"
    reasons.pop(pointer, None)
    if not count:
        reasons[pointer] = {"code": "NO_SAMPLES", "reason": "No successful samples."}


def write_json(path: Path, value: object) -> None:
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, ensure_ascii=False, allow_nan=False)
        stream.write("\n")


def aggregate(cell: Path, config: dict, client_files: list[str], server_files: list[str], issues: list[dict]) -> dict:
    owners = client_files if config["scenario"] == "session-echo-only" else ["server-channel-0.json"]
    originals = {}
    templates = {}
    for name in client_files + server_files:
        try:
            value = json.loads((cell / name).read_text())
            if value["schemaVersion"] != 2 or any(value[key] != config[key] for key in ("runId", "cellId", "configHash")):
                raise ValueError("SchemaMismatch: original identity differs")
            templates[name] = value
            if value["resetSeq"] != "1" or value["phase"] != "complete":
                raise ValueError("PhaseMismatch: original has no completed measured reset epoch")
            seconds = value["window"]["measuredSeconds"]
            if not isinstance(seconds, (float, int)) or not math.isfinite(seconds) or seconds <= 0:
                raise ValueError("SchemaMismatch: no finite positive owner window")
            if not math.isclose(seconds, (int(value["window"]["endTicks"]) - int(value["window"]["startTicks"])) / 1e9, rel_tol=0, abs_tol=1e-9):
                raise ValueError("SchemaMismatch: owner monotonic window disagrees with seconds")
            for group in ("metrics", "histograms"):
                for key, item in value[group].items():
                    if item is None and not value["nullReasons"].get(f"/{group}/{key}", {}).get("reason"):
                        raise ValueError("SchemaMismatch: null has no reason")
            originals[name] = value
        except (OSError, KeyError, ValueError, TypeError) as error:
            issues.append({"code": "SchemaMismatch" if str(error).startswith("SchemaMismatch:") else "CollectionFailure",
                           "message": str(error), "sourceFile": name})
    selected = [originals[name] for name in owners if name in originals]
    for name, value in templates.items():
        if value["resetSeq"] != "1" and any(value["metrics"][key] for key in ("errors.byKind", "errors.harness", "errors.language")):
            observed_errors = {key: value["metrics"][key] for key in ("errors.byKind", "errors.harness", "errors.language")}
            issues.append({"code": "PreMeasurementFailure", "message": "Setup/warmup errors: " + json.dumps(observed_errors, sort_keys=True),
                           "sourceFile": name, "resetSeq": value["resetSeq"], "errorCounts": observed_errors})
    if len(selected) != len(owners):
        issues.append({"code": "CollectionFailure", "message": "Missing primary owner original.", "sourceFile": ",".join(owners)})
    template = selected[0] if selected else next(iter(templates.values()), {})
    metrics = copy.deepcopy(template.get("metrics", {}))
    histograms = copy.deepcopy(template.get("histograms", {}))
    reasons = {key: copy.deepcopy(value) for key, value in (selected[0]["nullReasons"].items() if selected else [])
               if key.startswith(("/metrics/", "/histograms/"))}
    if not selected:
        for group, values in (("metrics", metrics), ("histograms", histograms)):
            for key in values:
                if group == "metrics" and key.startswith("errors."):
                    continue
                values[key] = None
                reasons[f"/{group}/{key}"] = {"code": "PHASE_NOT_STARTED", "reason": "No completed measured owner window is available."}
    if selected:
        try:
            for owner in selected:
                counts = {key: u64(owner["metrics"]["messages." + key]) for key in OUTCOMES}
                if counts["sent"] != sum(value for key, value in counts.items() if key != "sent"):
                    raise ValueError("SchemaMismatch: echo cohort does not reconcile")
                for kind, key in (("latencyMs", "completed"), ("settleLatencyMs", "settleCompleted")):
                    if u64(owner["histograms"][kind]["count"]) != counts[key]:
                        raise ValueError("SchemaMismatch: successful count and histogram differ")
            for key in OUTCOMES:
                metrics["messages." + key] = count_text(sum(u64(value["metrics"]["messages." + key]) for value in selected))
            for key in ("latencyMs", "settleLatencyMs"):
                histograms[key] = histogram_merge([value["histograms"][key] for value in selected])
                export_latency(histograms[key], "latency" if key == "latencyMs" else "settle.latency", key, metrics, reasons)
            metrics["throughput.kops"] = sum(u64(value["metrics"]["messages.completed"]) / value["window"]["measuredSeconds"] / 1000 for value in selected)
            participants = [value for name, value in originals.items() if name in server_files or name in owners]
            for direction in ("request", "send", "reply", "event"):
                for group in ("applicationMessages", "applicationPayloadBytes"):
                    key = group + "." + direction
                    metrics[key] = count_text(sum(u64(value["metrics"][key]) for value in participants))
            for key in ("throughput.messagesPerSec", "throughput.megabytesPerSec"):
                metrics[key] = sum(value["metrics"][key] for value in participants)
            for family in ("errors.byKind", "errors.harness", "errors.language"):
                combined = {}
                for value in selected:
                    for key, count in value["metrics"][family].items():
                        combined[key] = count_text(u64(combined.get(key, "0")) + u64(count))
                metrics[family] = combined
            if config["scenario"] == "session-echo-only":
                for key in ("requested", "connected", "failed"):
                    metrics["connections." + key] = count_text(sum(u64(value["metrics"]["connections." + key]) for value in selected))
            if len(selected) > 1:
                metrics["load.inflight.max"] = None
                reasons["/metrics/load.inflight.max"] = {"code": "MULTIPLE_OWNERS", "reason": "Separate process maxima have no verified simultaneous global observation; see owner originals."}
            for name, original in originals.items():
                if any(original["metrics"][key] for key in ("errors.byKind", "errors.harness", "errors.language")):
                    issues.append({"code": "PublicOrApplicationFailure", "message": "See original error namespaces and firstErrors evidence.", "sourceFile": name})
            for key in ("failed", "timeout", "cancelled", "unresolved"):
                if u64(metrics["messages." + key]):
                    issues.append({"code": "EchoOutcomeFailure", "message": key + "=" + metrics["messages." + key], "sourceFile": ",".join(owners)})
            for key in ("process.cpuPercent", "process.rssMb", "process.allocatedMb", "gc.gen0", "gc.gen1", "gc.gen2"):
                metrics[key] = None
                reasons["/metrics/" + key] = {"code": "MULTIPLE_OWNERS", "reason": "Resource observations belong to individual processes; see processes and originals."}
        except (KeyError, TypeError, ValueError, OverflowError) as error:
            issues.append({"code": "CounterOverflow" if "overflow" in str(error) else "SchemaMismatch",
                           "message": str(error), "sourceFile": ",".join(owners)})
    seconds = selected[0]["window"]["measuredSeconds"] if len(selected) == 1 else None
    if seconds is None:
        reasons["/measuredSeconds"] = {"code": "MULTIPLE_OWNERS" if len(selected) > 1 else "COLLECTION_FAILED",
                                     "reason": "A single primary owner window is not available."}
    reasons["/aggregation/fanoutDeliveryRateMethod"] = {"code": "NOT_APPLICABLE", "reason": "Request baseline has no fanout."}
    status = ("unsupported" if any(issue["code"] == "PublicContractMismatch" for issue in issues) else
              "invalid" if any(issue["code"] in ("InvalidSetup", "SchemaMismatch") for issue in issues) else "failed" if issues else "valid")
    if not issues and not u64(metrics.get("messages.completed", "0")):
        status = "invalid"
        issues.append({"code": "NoCompletedEcho", "message": "No window echo success.", "sourceFile": ",".join(owners)})
    result = {
        "schemaVersion": 2, **{key: config[key] for key in ("runId", "cellId", "configHash", "scenario")},
        "language": "dotnet", "configFile": "config.json", "endpointsFile": "endpoints.json",
        "status": status, "baselineEligible": status == "valid" and config.get("diagnostics", "Off") == "Off", "reasons": issues,
        "metricOwners": owners, "ownerWindows": {name: originals[name]["window"] for name in owners if name in originals},
        "measuredSeconds": seconds, "aggregation": {"rateMethod": "sum-owner-rates" if len(owners) > 1 else "single-owner",
            "applicationRateMethod": "sum-role-rates", "fanoutDeliveryRateMethod": None},
        "metrics": metrics, "histograms": histograms, "nullReasons": reasons,
        "clients": client_files, "servers": server_files,
        "processes": [{"sourceFile": name, "pid": value["provenance"]["pid"], "clock": value["clock"],
                       "resources": {key: value["metrics"][key] for key in value["metrics"] if key.startswith(("process.", "gc."))},
                       "publicStatusFile": name} for name, value in originals.items()],
    }
    write_json(cell / "result.json", result)
    summary = {key: result[key] for key in ("schemaVersion", "runId", "cellId", "scenario", "status", "baselineEligible", "reasons", "metricOwners", "ownerWindows")}
    summary.update({"payloadSize": config["workload"]["payloadSize"], "topology": config["topology"],
                    "diagnostics": config.get("diagnostics", "Off"),
                    "metrics": {key: value for key, value in metrics.items() if key.startswith(("throughput.", "latency.", "settle.latency.", "messages.", "errors.", "connections."))},
                    "processes": [{"sourceFile": p["sourceFile"], "pid": p["pid"], "resources": p["resources"]} for p in result["processes"]],
                    "limitations": ["Percentiles are nearest-rank bucket upper-bound estimates, not exact observations.",
                        "Same-host loopback includes CPU competition. Process CPU is one-core=100%.",
                        "Unobservable internal metrics and serialized byte sizes remain null with reasons in result.json."],
                    "nullReasons": {key: value for key, value in reasons.items() if key.startswith(("/metrics/latency.", "/metrics/settle.latency."))}})
    write_json(cell / "summary.json", summary)
    (cell / "summary.txt").write_text(f"{config['scenario']} payload={config['workload']['payloadSize']} topology={config['topology']} status={status} baselineEligible={result['baselineEligible']} KOPS={metrics.get('throughput.kops')} p50/p95/p99={metrics.get('latency.p50Ms')}/{metrics.get('latency.p95Ms')}/{metrics.get('latency.p99Ms')} ms\n", encoding="utf-8")
    return result
