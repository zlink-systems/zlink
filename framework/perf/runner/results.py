"""Canonical histogram and owner aggregation; no averaging of process percentiles."""
from __future__ import annotations

import copy
import json
import math
from pathlib import Path

BOUNDS = json.loads((Path(__file__).resolve().parents[1] / "schema/histogram-bounds.json").read_text())
OUTCOMES = ("sent", "completed", "settleCompleted", "failed", "timeout", "cancelled", "unresolved")
MAX_U64 = 18446744073709551615


def null_reason(code: str, reason: str, owner: str | None = None, lower_bound_ms: float | None = None) -> dict:
    return {"code": code, "reason": reason, "owner": owner or "perf/runner", "lowerBoundMs": lower_bound_ms}


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
            reasons[pointer] = null_reason("NO_SAMPLES" if not count else "HISTOGRAM_OVERFLOW",
                                           "No successful samples." if not count else "Nearest rank is above the final bucket.",
                                           "perf/README.ko.md §15.3", 1024 if count else None)
    pointer = "/histograms/" + histogram_key + "/maxNs"
    reasons.pop(pointer, None)
    if not count:
        reasons[pointer] = null_reason("NO_SAMPLES", "No successful samples.")


def write_json(path: Path, value: object) -> None:
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, ensure_ascii=False, allow_nan=False)
        stream.write("\n")


def sequence_ranges(value: object, where: str) -> list[tuple[int, int]]:
    """A §15.4 Range[]: canonical U64 text, both ends inclusive, ascending, maximal (never overlapping or adjacent)."""
    if not isinstance(value, list):
        raise ValueError(f"SchemaMismatch: {where} is not a range list")
    parsed: list[tuple[int, int]] = []
    for item in value:
        first, last = u64(item["first"]), u64(item["last"])
        if first > last or (parsed and first <= parsed[-1][1] + 1):
            raise ValueError(f"SchemaMismatch: {where} ranges are not ascending maximal intervals")
        parsed.append((first, last))
    return parsed


def range_size(ranges: list[tuple[int, int]]) -> int:
    return sum(last - first + 1 for first, last in ranges)


def range_intersection_size(left: list[tuple[int, int]], right: list[tuple[int, int]]) -> int:
    total = i = j = 0
    while i < len(left) and j < len(right):
        low, high = max(left[i][0], right[j][0]), min(left[i][1], right[j][1])
        if low <= high:
            total += high - low + 1
        if left[i][1] < right[j][1]:
            i += 1
        else:
            j += 1
    return total


def read_sequences(cell: Path, name: str, config: dict, keys: tuple[str, ...]) -> tuple[dict, dict]:
    value = json.loads((cell / name).read_text())
    if (any(value.get(key) != config[key] for key in ("runId", "cellId")) or value.get("resetSeq") != "1"
            or value.get("phase") != "measured"):
        raise ValueError(f"SchemaMismatch: {name} identity differs from the measured epoch")
    return value, {key: sequence_ranges(value[key], f"{name}/{key}") for key in keys}


def fanout_aggregate(cell: Path, config: dict, originals: dict, owners: list[str], metrics: dict, histograms: dict, reasons: dict) -> dict:
    """§15.4 PS: intersect each subscriber's first receipts with the publisher's window-success set.
    owners[0] is the publisher original (already the metrics template); the rest are the subscribers."""
    publisher_file, subscriber_files = owners[0], owners[1:]
    publisher = originals[publisher_file]
    _, published = read_sequences(cell, "publisher-sequences.json", config, ("attemptedRanges", "windowSuccessRanges", "settleSuccessRanges"))
    window_success = published["windowSuccessRanges"]
    in_window, in_settle = range_size(window_success), range_size(published["settleSuccessRanges"])
    counts = {key: u64(publisher["metrics"]["messages." + key]) for key in
              ("sent", "published", "publishedInWindow", "settlePublished", "failed", "timeout", "cancelled", "unresolved")}
    if (counts["publishedInWindow"], counts["settlePublished"]) != (in_window, in_settle) or counts["published"] != in_window + in_settle:
        raise ValueError("SchemaMismatch: publisher counters differ from its success ranges")
    if counts["sent"] != range_size(published["attemptedRanges"]):
        raise ValueError("SchemaMismatch: publisher sent differs from its attempted range")
    if counts["sent"] != counts["published"] + counts["failed"] + counts["timeout"] + counts["cancelled"] + counts["unresolved"]:
        raise ValueError("SchemaMismatch: publish cohort does not reconcile")
    if len(subscriber_files) != config["subscriberCount"]:
        raise ValueError("CollectionFailure: subscriber originals differ from the configured subscriber count")
    per_subscriber: dict[str, dict] = {}
    for name in subscriber_files:
        original = originals[name]
        document, received = read_sequences(cell, f"subscriber-{original['roleInstance']}-sequences.json", config, ("windowRanges", "settleRanges"))
        if range_intersection_size(received["windowRanges"], received["settleRanges"]):
            raise ValueError(f"SchemaMismatch: {name} received a sequence as first in both window and settle")
        duplicates = u64(document["duplicateEvents"])
        if duplicates != u64(original["metrics"]["fanout.duplicateEvents"]):
            raise ValueError(f"SchemaMismatch: {name} duplicate count differs between original and sequence file")
        delivered_window = range_intersection_size(received["windowRanges"], window_success)
        delivered_settle = range_intersection_size(received["settleRanges"], window_success)
        received_unique = range_size(received["windowRanges"]) + range_size(received["settleRanges"])
        per_subscriber[name] = {
            "subscriberId": original["roleInstance"], "deliveredInWindow": delivered_window, "settleDelivered": delivered_settle,
            "uniqueDelivered": delivered_window + delivered_settle, "outOfCohortEvents": received_unique - delivered_window - delivered_settle,
            "duplicateEvents": duplicates, "measuredSeconds": original["window"]["measuredSeconds"]}
    total = lambda key: sum(entry[key] for entry in per_subscriber.values())  # noqa: E731
    for key, value in (("subscriberCount", len(per_subscriber)), ("uniqueDelivered", total("uniqueDelivered")),
                       ("deliveredInWindow", total("deliveredInWindow")), ("settleDelivered", total("settleDelivered")),
                       ("duplicateEvents", total("duplicateEvents")), ("outOfCohortEvents", total("outOfCohortEvents"))):
        metrics["fanout." + key] = count_text(value)
        reasons.pop("/metrics/fanout." + key, None)
    metrics["fanout.publishOpsPerSec"] = in_window / publisher["window"]["measuredSeconds"]
    metrics["fanout.deliveryOpsPerSec"] = sum(e["deliveredInWindow"] / e["measuredSeconds"] for e in per_subscriber.values())
    for key in ("publishOpsPerSec", "deliveryOpsPerSec"):
        reasons.pop("/metrics/fanout." + key, None)
    if in_window:
        metrics["fanout.deliveryRatio"] = min(e["uniqueDelivered"] for e in per_subscriber.values()) / in_window
        reasons.pop("/metrics/fanout.deliveryRatio", None)
    else:
        metrics["fanout.deliveryRatio"] = None
        reasons["/metrics/fanout.deliveryRatio"] = null_reason("ZERO_DENOMINATOR", "The publisher has no window-success publish.",
                                                               "perf/README.ko.md §15.4")
    # Delivery latency needs a verified shared clock domain; the subscribers' originals carry the reason (§15.2).
    first = originals[subscriber_files[0]]["nullReasons"]
    for prefix in ("fanout.deliveryLatency", "fanout.settleDeliveryLatency"):
        for suffix in ("meanMs", "p50Ms", "p95Ms", "p99Ms", "maxMs"):
            pointer = "/metrics/" + prefix + "." + suffix
            metrics[prefix + "." + suffix] = None
            reasons[pointer] = first[pointer]
    for key in ("fanoutDeliveryLatencyMs", "fanoutSettleDeliveryLatencyMs"):
        histograms[key] = None
        reasons["/histograms/" + key] = first["/histograms/" + key]
    return per_subscriber


def _received_unique(stats: dict) -> int:
    receipts = stats["runtimeMetrics"]["fanoutReceipts"]["value"]
    return u64(receipts["uniqueInWindow"]) + u64(receipts["uniqueInSettle"])


def _cohort_terminal_echo(stats: dict[str, dict], owners: list[str]) -> tuple[bool, dict]:
    """Echo: the source's own settle already waits for every cohort operation, so its phase complete is the end."""
    return True, {}


def _cohort_terminal_fanout(stats: dict[str, dict], owners: list[str]) -> tuple[bool, dict]:
    """Fanout (§4.1): every subscriber has received as many unique sequences as the publisher's window success count."""
    required = u64(stats[owners[0]]["metrics"]["messages.publishedInWindow"])
    received = {name: _received_unique(stats[name]) for name in owners[1:]}
    return all(count >= required for count in received.values()), {"required": required, "received": received}


# aggregation -> "is every observed cohort member terminal" (§4.1); the scenario table names the aggregation.
COHORT_TERMINAL = {"echo": _cohort_terminal_echo, "fanout-sequences": _cohort_terminal_fanout}


def settle_status(aggregation: str, stats: dict[str, dict], owners: list[str], elapsed: float, bound: float) -> dict:
    """The settle of a measured phase ends when the cohort is terminal or after settleTimeoutMs (§4.1, §5.2)."""
    terminal, progress = COHORT_TERMINAL[aggregation](stats, owners)
    return {"done": terminal or elapsed >= bound, "boundReached": not terminal and elapsed >= bound,
            "elapsedSeconds": elapsed, "boundSeconds": bound, "progress": progress}


def aggregate(cell: Path, config: dict, client_files: list[str], server_files: list[str], issues: list[dict], owners: list[str],
              aggregation: str = "echo") -> dict:
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
    ps = aggregation == "fanout-sequences"
    delivery: dict = {}
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
                reasons[f"/{group}/{key}"] = null_reason("PHASE_NOT_STARTED", "No completed measured owner window is available.")
    if selected:
        try:
            if ps:
                delivery = fanout_aggregate(cell, config, originals, owners, metrics, histograms, reasons)
            else:
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
            if owners == client_files:  # CS: the connector pool belongs to the client owners
                for key in ("requested", "connected", "failed"):
                    metrics["connections." + key] = count_text(sum(u64(value["metrics"]["connections." + key]) for value in selected))
            if len(selected) > 1 and not ps:
                metrics["load.inflight.max"] = None
                reasons["/metrics/load.inflight.max"] = null_reason(
                    "MULTIPLE_OWNERS", "Separate process maxima have no verified simultaneous global observation; see owner originals.")
            for name, original in originals.items():
                if any(original["metrics"][key] for key in ("errors.byKind", "errors.harness", "errors.language")):
                    issues.append({"code": "PublicOrApplicationFailure", "message": "See original error namespaces and firstErrors evidence.", "sourceFile": name})
            for key in ("failed", "timeout", "cancelled", "unresolved"):
                if u64(metrics["messages." + key]):
                    issues.append({"code": "EchoOutcomeFailure", "message": key + "=" + metrics["messages." + key], "sourceFile": ",".join(owners)})
            for key in ("process.cpuPercent", "process.rssMb", "process.allocatedMb", "gc.gen0", "gc.gen1", "gc.gen2"):
                metrics[key] = None
                reasons["/metrics/" + key] = null_reason(
                    "MULTIPLE_OWNERS", "Resource observations belong to individual processes; see processes and originals.")
        except (KeyError, TypeError, ValueError, OverflowError) as error:
            issues.append({"code": "CounterOverflow" if "overflow" in str(error) else "SchemaMismatch",
                           "message": str(error), "sourceFile": ",".join(owners)})
    seconds = selected[0]["window"]["measuredSeconds"] if len(selected) == 1 else None
    if seconds is None:
        reasons["/measuredSeconds"] = null_reason("MULTIPLE_OWNERS" if len(selected) > 1 else "COLLECTION_FAILED",
                                                   "A single primary owner window is not available.")
    if not ps:
        reasons["/aggregation/fanoutDeliveryRateMethod"] = null_reason("NOT_APPLICABLE", "Request baseline has no fanout.")
    status = ("unsupported" if any(issue["code"] == "PublicContractMismatch" for issue in issues) else
              "invalid" if any(issue["code"] in ("InvalidSetup", "SchemaMismatch") for issue in issues) else "failed" if issues else "valid")
    if not issues and not u64(metrics.get("messages.publishedInWindow" if ps else "messages.completed", "0")):
        status = "invalid"
        issues.append({"code": "ZeroDenominator" if ps else "NoCompletedEcho",
                       "message": "No window publish success." if ps else "No window echo success.", "sourceFile": ",".join(owners)})
    # Inputs from language roles can predate lowerBoundMs; every runner result uses the §15.5 reason shape.
    reasons = {key: null_reason(value["code"], value["reason"], value.get("owner"), value.get("lowerBoundMs"))
               for key, value in reasons.items()}
    result = {
        "schemaVersion": 2, **{key: config[key] for key in ("runId", "cellId", "configHash", "scenario")},
        "language": config["language"], "configFile": "config.json", "endpointsFile": "endpoints.json",
        "status": status, "baselineEligible": status == "valid" and config.get("diagnostics", "Off") == "Off", "reasons": issues,
        "metricOwners": owners, "ownerWindows": {name: originals[name]["window"] for name in owners if name in originals},
        "measuredSeconds": seconds, "aggregation": {"rateMethod": "sum-owner-rates" if len(owners) > 1 and not ps else "single-owner",
            "applicationRateMethod": "sum-role-rates", "fanoutDeliveryRateMethod": "sum-subscriber-rates" if ps else None},
        "metrics": metrics, "histograms": histograms, "nullReasons": reasons,
        **({"settle": json.loads((cell / "settle.json").read_text())} if (cell / "settle.json").exists() else {}),
        "clients": client_files, "servers": server_files,
        "processes": [{"sourceFile": name, "pid": value["provenance"]["pid"], "clock": value["clock"],
                       "resources": {key: value["metrics"][key] for key in value["metrics"] if key.startswith(("process.", "gc."))},
                       "publicStatusFile": name, **({"fanoutDelivery": delivery[name]} if name in delivery else {})}
                      for name, value in originals.items()],
    }
    write_json(cell / "result.json", result)
    summary = {key: result[key] for key in ("schemaVersion", "runId", "cellId", "scenario", "status", "baselineEligible", "reasons", "metricOwners", "ownerWindows")}
    summary.update({"payloadSize": config["workload"]["payloadSize"], "topology": config["topology"],
                    "diagnostics": config.get("diagnostics", "Off"),
                    "metrics": {key: value for key, value in metrics.items() if key.startswith(("throughput.", "latency.", "settle.latency.", "messages.", "errors.", "connections.", "fanout."))},
                    "processes": [{"sourceFile": p["sourceFile"], "pid": p["pid"], "resources": p["resources"]} for p in result["processes"]],
                    "limitations": ["Percentiles are nearest-rank bucket upper-bound estimates, not exact observations.",
                        "Same-host loopback includes CPU competition. Process CPU is one-core=100%.",
                        "Unobservable internal metrics and serialized byte sizes remain null with reasons in result.json."],
                    "nullReasons": {key: value for key, value in reasons.items() if key.startswith(("/metrics/latency.", "/metrics/settle.latency."))}})
    write_json(cell / "summary.json", summary)
    head = f"{config['scenario']} payload={config['workload']['payloadSize']} topology={config['topology']} status={status} baselineEligible={result['baselineEligible']}"
    tail = (f"publishOps/s={metrics.get('fanout.publishOpsPerSec')} deliveryOps/s={metrics.get('fanout.deliveryOpsPerSec')} deliveryRatio={metrics.get('fanout.deliveryRatio')}" if ps else
            f"KOPS={metrics.get('throughput.kops')} p50/p95/p99={metrics.get('latency.p50Ms')}/{metrics.get('latency.p95Ms')}/{metrics.get('latency.p99Ms')} ms")
    (cell / "summary.txt").write_text(f"{head} {tail}\n", encoding="utf-8")
    return result
