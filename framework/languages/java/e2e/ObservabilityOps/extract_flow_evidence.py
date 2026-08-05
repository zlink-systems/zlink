#!/usr/bin/env python3
"""Extract Config 11 A1/A2 verifier input from real process logs."""
import json
import pathlib
import re
import sys

LOG_DIR = pathlib.Path(sys.argv[1])
OUT_DIR = pathlib.Path(sys.argv[2])
SELECTOR = sys.argv[3]
FLOW = re.compile(r"\bflow=([0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12})\b")

def lines(name):
    path = LOG_DIR / name
    return path.read_text(encoding="utf-8", errors="replace").splitlines() if path.is_file() else []

def flow_of(line):
    match = FLOW.search(line)
    return match.group(1) if match else None

def require(rows, message):
    if not rows:
        raise SystemExit(message)
    return rows

def write(scenario, rows, extra=None):
    flows = []
    for sequence, (phase, line) in enumerate(rows, 1):
        flows.append({
            "flow": flow_of(line),
            "origin": re.search(r"\borigin=([^ ]+)", line).group(1).lower() if "origin=" in line else "inbound",
            "label": re.search(r"\blabel=([^ ]+)", line).group(1) if "label=" in line else phase,
            "phase": phase,
            "outcome": (
                re.search(r"\boutcome=([^ ]+)", line).group(1).lower()
                if "outcome=" in line
                else ("error" if phase == "stream_error" and "kind=4" in line else "sent")
            ),
            "sequence": sequence,
            "source": line,
        })
    path = OUT_DIR / f"{scenario}.json"
    document = {"scenario": scenario, "flows": flows}
    if extra:
        document.update(extra)
    path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(f"{scenario} evidence={path} flow={flows[0]['flow']} rows={len(flows)}")

if SELECTOR in ("all", "OBS-A1"):
    connector = require([line for line in lines("a1-client.stderr.log") if "connector write-start" in line and "name=ActorPushAwaitReq" in line and flow_of(line)], "OBS-A1 connector outbound log missing")[-1]
    flow = flow_of(connector)
    session = [line for line in lines("session-flow.log") if f"flow={flow}" in line]
    play = [line for line in lines("play-a-flow.log") if f"flow={flow}" in line]
    inbound = require([line for line in session if "outcome=RECEIVED" in line and "surface=STREAM_SESSION" in line], "OBS-A1 stream inbound log missing")[0]
    relay = require([line for line in session if "outcome=SENT" in line and "surface=SPOT_ACTOR" in line], "OBS-A1 actual actor relay log missing")[0]
    dispatch = require([line for line in play if "outcome=RECEIVED" in line and "surface=SPOT_ACTOR" in line], "OBS-A1 actor Spot dispatch log missing")[0]
    write("OBS-A1", [("connector_outbound", connector), ("stream_inbound", inbound), ("actor_relay", relay), ("spot_dispatch", dispatch)])

if SELECTOR in ("all", "OBS-A2"):
    received = require([
        line for line in lines("session-flow.log")
        if "outcome=RECEIVED" in line
        and "surface=STREAM_SESSION" in line
        and "packet=ObservabilityMissingPacket" in line
        and flow_of(line)
    ], "OBS-A2 received log missing")[-1]
    flow = flow_of(received)
    session = [line for line in lines("session-flow.log") if f"flow={flow}" in line]
    error = require([
        line for line in session
        if "outcome=ERROR" in line
        and "surface=STREAM_SESSION" in line
        and "packet=ObservabilityMissingPacket" in line
    ], "OBS-A2 server dispatch error log missing")[0]
    write("OBS-A2", [("stream_inbound", received), ("stream_error", error)])

if SELECTOR in ("all", "OBS-A3"):
    connector = require([
        line for line in lines("a3-client.stderr.log")
        if "connector write-start" in line and "name=ActorPushAwaitReq" in line and flow_of(line)
    ], "OBS-A3 entry log missing")[-1]
    flow = flow_of(connector)
    downstream = require([
        line for line in lines("play-a-flow.log")
        if f"flow={flow}" in line and "surface=SPOT_ACTOR" in line and "outcome=RECEIVED" in line
    ], "OBS-A3 downstream log after tracing-off node missing")[0]
    if any(f"flow={flow}" in line and "message flow" in line
           for line in lines("off-node.stdout.log") + lines("off-node.stderr.log")):
        raise SystemExit("OBS-A3 tracing-off node emitted a flow log")
    write("OBS-A3", [("entry", connector), ("after_off_node", downstream)])

if SELECTOR in ("all", "OBS-A4"):
    play_a = lines("play-a-flow.log")
    play_b = lines("play-b-flow.log")
    published = require([
        line for line in play_a
        if "outcome=SENT" in line and "kind=PUBLISH" in line
        and "packet=ObservabilityFanoutEvent" in line and "origin=TIMER" in line
        and flow_of(line)
    ], "OBS-A4 timer fanout publish log missing")[-1]
    publish_flow = flow_of(published)
    subscriber_a = require([
        line for line in play_a
        if f"flow={publish_flow}" in line and "outcome=RECEIVED" in line
        and "kind=PUBLISH" in line and "packet=ObservabilityFanoutEvent" in line
    ], "OBS-A4 subscriber play-a log missing")[0]
    subscriber_b = require([
        line for line in play_b
        if f"flow={publish_flow}" in line and "outcome=RECEIVED" in line
        and "kind=PUBLISH" in line and "packet=ObservabilityFanoutEvent" in line
    ], "OBS-A4 subscriber play-b log missing")[0]
    timer_root = require([
        line for line in play_a
        if "origin=TIMER" in line and flow_of(line) and flow_of(line) != publish_flow
    ], "OBS-A4 independent timer root log missing")[-1]
    write("OBS-A4", [
        ("publish", published),
        ("subscriber", subscriber_a),
        ("subscriber", subscriber_b),
        ("timer_root", timer_root),
    ], {"publishFlow": publish_flow})

def load_metrics(*names):
    merged = {}
    for name in names:
        path = LOG_DIR / name
        if not path.is_file():
            continue
        for row in json.loads(path.read_text(encoding="utf-8")):
            if not row.get("name", "").startswith("zlink."):
                continue
            key = (row["name"], tuple(sorted(row.get("tags", {}).items())))
            if key not in merged:
                merged[key] = dict(row)
            else:
                merged[key]["value"] = merged[key].get("value", 0) + row.get("value", 0)
                merged[key]["count"] = merged[key].get("count", 0) + row.get("count", 0)
    return list(merged.values())

if SELECTOR in ("all", "OBS-B1"):
    metrics = load_metrics("session-metrics.json", "connector-metrics.json")
    require([row for row in metrics if row["name"] == "zlink.stream.reconnects"], "OBS-B1 reconnect meter missing")
    write_path = OUT_DIR / "OBS-B1.json"
    write_path.write_text(json.dumps({"scenario": "OBS-B1", "metrics": metrics}, indent=2) + "\n", encoding="utf-8")

if SELECTOR in ("all", "OBS-B2"):
    metrics = load_metrics("play-a-metrics.json", "play-b-metrics.json")
    require([row for row in metrics if row["name"] == "zlink.actor.transfers"], "OBS-B2 actor transfer meter missing")
    write_path = OUT_DIR / "OBS-B2.json"
    write_path.write_text(json.dumps({"scenario": "OBS-B2", "metrics": metrics}, indent=2) + "\n", encoding="utf-8")

if SELECTOR in ("all", "OBS-B3"):
    metrics = load_metrics("play-a-metrics.json", "play-b-metrics.json")
    require([row for row in metrics if row["name"] == "zlink.location.owner_lease.renew.lateness"], "OBS-B3 owner lease meter missing")
    write_path = OUT_DIR / "OBS-B3.json"
    write_path.write_text(json.dumps({"scenario": "OBS-B3", "metrics": metrics, "dropObservable": False}, indent=2) + "\n", encoding="utf-8")

if SELECTOR in ("all", "OBS-B4"):
    result_path = LOG_DIR / "reader-free-result.json"
    require([result_path] if result_path.is_file() else [], "OBS-B4 reader-free result missing")
    result = json.loads(result_path.read_text(encoding="utf-8"))
    metric_series = load_metrics("reader-free-metrics.json")
    write_path = OUT_DIR / "OBS-B4.json"
    write_path.write_text(json.dumps({
        "scenario": "OBS-B4",
        "messagingAccurate": result.get("messagingAccurate", False),
        "metricSeriesStored": len(metric_series),
        "trafficEvents": result.get("trafficEvents", 0),
    }, indent=2) + "\n", encoding="utf-8")

if SELECTOR in ("all", "OBS-C4"):
    status = json.loads((LOG_DIR / "c4-drain-status.json").read_text(encoding="utf-8"))
    connector = json.loads((LOG_DIR / "c4-connector-result.json").read_text(encoding="utf-8"))
    trace = "\n".join(lines("c4-client.stderr.log"))
    require([trace] if "session-closing version=1 reason=server_drain" in trace else [],
            "OBS-C4 session-closing v1 trace missing")
    events = [{"state": event["state"].lower(), "timestamp": event["timestamp"]}
              for event in status.get("events", [])]
    write_path = OUT_DIR / "OBS-C4.json"
    write_path.write_text(json.dumps({
        "scenario": "OBS-C4",
        "drainEvents": events,
        "result": status.get("result", ""),
        "reason": status.get("reason", ""),
        "closeReason": connector.get("closeReason", ""),
        "sessionClosingVersion": 1,
        "metrics": load_metrics("c4-metrics.json"),
    }, indent=2) + "\n", encoding="utf-8")

if SELECTOR in ("all", "OBS-C1"):
    before = json.loads((LOG_DIR / "c1-before.json").read_text(encoding="utf-8"))
    during = json.loads((LOG_DIR / "c1-during.json").read_text(encoding="utf-8"))
    existing = "observability-ops client scenario=OBS-C1 result=passed" in "\n".join(
        lines("c1-existing.stdout.log"))
    before_renewed = before.get("locationStatus", {}).get("ownerLeaseRenewedAt", "")
    during_renewed = during.get("locationStatus", {}).get("ownerLeaseRenewedAt", "")
    events = [{"state": event["state"].lower(), "timestamp": event["timestamp"]}
              for event in during.get("events", [])]
    write_path = OUT_DIR / "OBS-C1.json"
    write_path.write_text(json.dumps({
        "scenario": "OBS-C1",
        "readyAfterDrain": during.get("ready", True),
        "existingRequestSucceeded": existing,
        "leaseRenewedWhileDraining": bool(before_renewed and during_renewed != before_renewed),
        "peerRows": during.get("peerRows", []),
        "drainEvents": events,
    }, indent=2) + "\n", encoding="utf-8")

if SELECTOR in ("all", "OBS-C2"):
    output = "\n".join(lines("c2-client.stdout.log"))
    metrics = load_metrics("c2-metrics.json")
    handed_off = any(row["name"] == "zlink.drain.actors.handed_off"
                     and row.get("value", 0) >= 1 for row in metrics)
    write_path = OUT_DIR / "OBS-C2.json"
    write_path.write_text(json.dumps({
        "scenario": "OBS-C2",
        "locationTakenOver": handed_off,
        "boundPushContinued": "bound-push=true" in output,
        "pendingRequestsCompleted": "pending-completed=true" in output,
        "metrics": metrics,
    }, indent=2) + "\n", encoding="utf-8")

if SELECTOR in ("all", "OBS-C3"):
    drained = json.loads((LOG_DIR / "c3-fixed-terminal.json").read_text(encoding="utf-8"))
    read_output = "\n".join(lines("c3-read.stdout.log"))
    metrics = load_metrics("c3-fixed-metrics.json")
    (OUT_DIR / "OBS-C3.json").write_text(json.dumps({
        "scenario": "OBS-C3",
        "fixedDrainCompleted": drained.get("result") == "drained",
        "ownerReleasedAndRecreated": drained.get("result") == "drained" and "node=play-b" in read_output,
        "replayRestoredState": "value=state-v1" in read_output and "replayed=true" in read_output,
        "metrics": metrics,
    }, indent=2) + "\n", encoding="utf-8")

if SELECTOR in ("all", "OBS-C5"):
    serving = json.loads((LOG_DIR / "c5-serving-terminal.json").read_text(encoding="utf-8"))
    zero = json.loads((LOG_DIR / "c5-zero-terminal.json").read_text(encoding="utf-8"))
    target_during = json.loads((LOG_DIR / "c5-zero-play-b-during.json").read_text(encoding="utf-8"))
    zero_metrics = load_metrics("c5-zero-metrics.json")
    handed_off = any(row.get("name") == "zlink.drain.actors.handed_off"
                     and row.get("value", 0) > 0 for row in zero_metrics)
    (OUT_DIR / "OBS-C5.json").write_text(json.dumps({
        "scenario": "OBS-C5",
        "rolloutResult": serving.get("result"),
        "rolloutForceStopping": any(event.get("state", "").lower() == "force_stopping"
                                     for event in serving.get("events", [])),
        "zeroTargetHeldAtSource": not handed_off and not target_during.get("ready", True)
            and any(row.get("nodeRid") == "play-b" and row.get("draining")
                    for row in target_during.get("peerRows", [])),
        "zeroTargetResult": zero.get("result"),
        "zeroTargetReason": zero.get("reason"),
    }, indent=2) + "\n", encoding="utf-8")
