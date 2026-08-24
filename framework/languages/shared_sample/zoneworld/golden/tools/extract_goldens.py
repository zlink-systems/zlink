#!/usr/bin/env python3
"""Build and verify the normalized ZoneWorld golden package from captured .NET artifacts."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
from typing import Any


SCHEMA_VERSION = "zoneworld-golden/v1"

COMPLETION_BARS = {
    "typed-json-wire-vectors": "decoder/encoder exact match plus live client receives same semantic payload.",
    "world-state-transcript": "same ordered transcript at specified steps.",
    "relocation-transcript": "all correlations and generation predicates pass; no source re-resolve/retry evidence.",
    "stage-flow-event-ledger": "expected ordered partial order and exactly-once handlers.",
    "store-snapshots": "same key namespaces/separation and state transition invariants, including both relocation-prefix absence assertions.",
    "ops-fanout-transcript": "identical observable states and duplicate/absence assertions.",
    "bot-transcript": "normalized sequence and negative evidence.",
    "lifecycle-transcript": "exact replacement and Unavailable boundaries.",
    "browser-transcript": "headless test and API/headless transcript both green.",
}


def artifact(path: str, selector: str) -> dict[str, str]:
    return {"path": path, "selector": selector}


def record(
    scenario: str,
    producer: str,
    consumer: str,
    payload: Any,
    raw: list[dict[str, str]],
    oracle: dict[str, Any],
) -> dict[str, Any]:
    return {
        "scenario_id": scenario,
        "producer_role": producer,
        "consumer_role": consumer,
        "normalized_payload": payload,
        "raw_artifacts": raw,
        "typed_or_state_oracle": oracle,
    }


def wire_records() -> list[dict[str, Any]]:
    client = "raw/full-reference/logs/client.log"
    zone = "raw/full-reference/logs/zone-node-2.log"
    gateway = "raw/full-reference/logs/gateway.log"
    ops = "raw/full-reference/logs/ops.log"

    def wire(scenario: str, producer: str, consumer: str, packet: str, payload_json: str,
             path: str, selector: str, direction: str = "send") -> dict[str, Any]:
        return record(
            scenario, producer, consumer,
            {"packet": packet, "kind": direction, "codec": "Json", "json": payload_json},
            [artifact(path, selector)],
            {"kind": "typed-wire", "assert": "exact UTF-8 JSON field names, values, nulls, and optional-field presence"},
        )

    return [
        wire("ZW-A1", "client", "gateway", "JoinWorldReq",
             '{"playerId":"<PLAYER:ZW-A1>"}', gateway, "packet=JoinWorldReq"),
        wire("ZW-A1", "gateway", "client", "JoinWorldRes",
             '{"playerId":"<PLAYER:ZW-A1>","zoneId":"zone-nw","x":25,"y":25,"error":null}',
             client, "packet=JoinWorldRes"),
        wire("ZW-A3", "client", "player-actor", "MoveMsg", '{"x":101,"y":25}', zone, "packet=MoveMsg"),
        wire("ZW-A3", "player-actor", "client", "MoveRejectedNotify",
             '{"reason":"OutOfRange","x":25,"y":25}', client, "packet=MoveRejectedNotify"),
        wire("ZW-A3", "player-actor", "client", "MoveRejectedNotify",
             '{"reason":"TooFar","x":25,"y":25}', client, "packet=MoveRejectedNotify"),
        wire("ZW-A3", "player-actor", "client", "MoveRejectedNotify",
             '{"reason":"DiagonalCrossing","x":49,"y":49}', client, "packet=MoveRejectedNotify"),
        wire("ZW-A2", "zone-spot", "client", "ZoneStateNotify",
             '{"zoneId":"zone-nw","tick":"<TICK:1>","players":[{"playerId":"<PLAYER:ZW-A2>","x":28,"y":27,"zoneId":"zone-nw","isBot":false}]}',
             client, "player=a2- packet=ZoneStateNotify"),
        wire("ZW-B2", "zone-spot", "client", "ZoneChangedNotify",
             '{"playerId":"<PLAYER:ZW-B2>","zoneId":"zone-ne"}', client, "packet=ZoneChangedNotify"),
        wire("ZW-B1", "zone-spot", "adjacent-zone-spot", "ZoneBorderEvent",
             '{"fromZoneId":"zone-nw","toZoneId":"zone-ne","tick":"<TICK:2>","players":[{"playerId":"<PLAYER:ZW-B1-WEST>","x":48,"y":25,"zoneId":"zone-nw","isBot":false}]}',
             zone, "packet=ZoneBorderEvent"),
        wire("ZW-C1", "ops-client", "ops", "WatchNodesReq", '{}', ops, "packet=WatchNodesReq", "request"),
        wire("ZW-C1", "ops", "ops-client", "WatchNodesRes",
             '{"nodes":[{"nodeId":"zone-node-1","registered":true,"connected":true,"maintenance":false,"zones":["zone-nw","zone-sw"],"playerCount":<COUNT:1>}]}',
             ops, "packet=WatchNodesRes", "reply"),
        wire("ZW-C4", "ops", "ops-client", "NodeAlertNotify",
             '{"nodeId":"zone-node-1","kind":"TimerHandlerFailed","detail":"spot=zone-nw; timer=zone-tick-zone-nw; detail=injected tick failure for ZW-C4. zone=zone-nw","occurredAt":"<TIMESTAMP:1>"}',
             ops, "kind=TimerHandlerFailed"),
        wire("ZW-D1", "ops-client", "ops", "AnnounceWorldReq",
             '{"text":"server maintenance starts in 10 minutes"}', ops, "packet=AnnounceWorldReq", "request"),
        wire("ZW-D1", "ops", "ops-client", "AnnounceWorldRes",
             '{"announcementId":"<ANNOUNCEMENT_ID:1>"}', client, "scenario ZW-D1 passed", "reply"),
        wire("ZW-D1", "zone-spot", "client", "WorldAnnounceNotify",
             '{"announcementId":"<ANNOUNCEMENT_ID:1>","text":"server maintenance starts in 10 minutes"}',
             client, "packet=WorldAnnounceNotify"),
        wire("ZW-E1", "ops-client", "ops", "SetMaintenanceReq",
             '{"nodeId":"zone-node-2","enabled":true}', ops, "packet=SetMaintenanceReq", "request"),
        wire("ZW-E1", "ops", "ops-client", "SetMaintenanceRes",
             '{"nodeId":"zone-node-2","enabled":true,"zones":["zone-ne","zone-se"],"error":null}',
             client, "scenario ZW-E1 passed", "reply"),
        wire("ZW-E6", "ops-client", "ops", "NodeDiagnosticsReq",
             '{"nodeId":"zone-node-2"}', ops, "packet=NodeDiagnosticsReq", "request"),
        wire("ZW-E6", "zone-node", "ops-client", "NodeDiagnosticsRes",
             '{"nodeId":"zone-node-2","zones":["zone-ne","zone-se"],"playerCount":<COUNT:2>,"maintenance":true,"error":null}',
             client, "scenario ZW-E6 passed", "reply"),
        wire("ZW-B6", "gateway-probe", "source-old-route", "MessageFollowProbeReq",
             '{"actorId":"<ACTOR:ZW-B6>","probeId":"<PROBE:ZW-B6>","payload":"cmVxdWVzdC1wYXlsb2Fk"}',
             gateway, "packet=MessageFollowProbeReq actor=b6-", "request"),
        wire("ZW-B6", "target-actor", "gateway-probe", "MessageFollowProbeRes",
             '{"probeId":"<PROBE:ZW-B6>","payload":"cmVxdWVzdC1wYXlsb2Fk"}',
             client, "message-follow-request completed", "reply"),
        wire("ZW-B5", "gateway-probe", "source-old-route", "MessageFollowProbeMsg",
             '{"actorId":"<ACTOR:ZW-B5>","probeId":"<PROBE:ZW-B5>","payload":"b25lLXdheS1wYXlsb2Fk"}',
             gateway, "packet=MessageFollowProbeMsg actor=b5-", "send"),
    ]


def world_records() -> list[dict[str, Any]]:
    client = "raw/full-reference/logs/client.log"
    return [
        record("ZW-A1", "gateway", "client", {"step": "spawn", "position": [25, 25], "zone": "zone-nw"},
               [artifact(client, "scenario ZW-A1 passed")], {"kind": "typed-state", "wire_record": "JoinWorldRes"}),
        record("ZW-A2", "player-actor", "zone-spot", {"step": "legal-move", "position": [28, 27]},
               [artifact(client, "scenario ZW-A2 passed")], {"kind": "typed-state", "wire_record": "ZoneStateNotify"}),
        record("ZW-A3", "move-policy", "player-actor", {"step": "rejection-precedence", "ordered": ["OutOfRange", "TooFar", "DiagonalCrossing"]},
               [artifact(client, "packet=MoveRejectedNotify")], {"kind": "typed-state", "wire_record": "MoveRejectedNotify"}),
        record("ZW-A4", "zone-spot", "client", {"step": "visibility-order", "sort": "UTF-8 ordinal by playerId", "duplicates": 0},
               [artifact(client, "scenario ZW-A4 passed")], {"kind": "typed-state", "assert": "players array is sorted and unique"}),
        record("ZW-A5", "player-actor", "zone-spot", {"step": "legal-step-boundary", "max_delta_per_axis": 5, "accepted": True},
               [artifact(client, "scenario ZW-A5 passed")], {"kind": "typed-state", "assert": "authoritative position advances"}),
        record("ZW-B1", "zone-nw", "zone-ne", {"step": "adjacent-border", "west_x": 48, "observed": True, "diagonal_observed": False},
               [artifact(client, "checkpoint=east-observed-west"), artifact(client, "checkpoint=diagonal-exclusion-observed")],
               {"kind": "typed-state", "wire_record": "ZoneBorderEvent", "assert": "adjacent only"}),
        record("ZW-B3", "player-actor", "same-owner-zone-spot", {"step": "local-zone-change", "zone_before": "zone-nw", "zone_after": "zone-sw", "owner_changed": False, "object_generation_changed": False},
               [artifact(client, "scenario ZW-B3 passed")], {"kind": "typed-state", "assert": "ZoneChangedNotify and ZoneStateNotify advance zone without relocation"}),
        record("ZW-B4", "zone-border-cache", "client", {"step": "expiry", "stale_tick": "ignored", "missing_after_ticks": 3},
               [artifact(client, "scenario ZW-B4 passed")], {"kind": "typed-state", "assert": "remote player absent from ZoneStateNotify after three ticks"}),
    ]


def relocation_records() -> list[dict[str, Any]]:
    client = "raw/full-reference/logs/client.log"
    gateway = "raw/full-reference/logs/gateway.log"
    zone = "raw/full-reference/logs/zone-node-2.log"
    return [
        record("ZW-B2", "source-player-actor", "target-zone-spot",
               {"actor_id": "<ACTOR:ZW-B2>", "before_owner": "<RID:SOURCE>", "after_owner": "<RID:TARGET>", "object_generation_before": "<OBJECT_GENERATION:1>", "object_generation_after": "<OBJECT_GENERATION:1>", "owner_changed": True},
               [artifact(client, "scenario ZW-B2 passed"), artifact(zone, "relocation_session_route commit=present actor=b2-")],
               {"kind": "typed-state", "wire_record": "ActorLocationProbeRes"}),
        record("ZW-B3", "target-player-actor", "target-zone-spot", {"same_zone_move": True, "relocation_count": 0},
               [artifact(client, "scenario ZW-B3 passed")], {"kind": "typed-state", "assert": "owner RID and generation unchanged"}),
        record("ZW-B5", "gateway-probe", "target-player-actor", {"route": "primed source route", "kind": "one-way", "payload_utf8": "one-way-payload", "target_handler_count": 1, "source_follow_relay_count": 1},
               [artifact(client, "message-follow-one-way completed"), artifact(gateway, "packet=MessageFollowProbeMsg actor=b5-")],
               {"kind": "typed-wire", "wire_record": "MessageFollowProbeMsg"}),
        record("ZW-B6", "gateway-probe", "target-player-actor", {"route": "primed source route", "kind": "request", "payload_utf8": "request-payload", "reply_payload_equal": True, "target_handler_count": 1, "source_follow_relay_count": 1},
               [artifact(client, "message-follow-request completed"), artifact(gateway, "packet=MessageFollowProbeReq actor=b6-")],
               {"kind": "typed-wire", "wire_record": "MessageFollowProbeReq/Res"}),
        record("ZW-B6", "source-old-route", "caller", {"missing_route_terminal": "Unavailable", "source_reresolve_count": 0, "source_retry_count": 0},
               [artifact(gateway, "message_follow_relay")], {"kind": "typed-state", "assert": "single terminal outcome; no second application dispatch"}),
        record("ZW-B7", "player-actor", "bound-session", {"path": ["zone-nw", "zone-ne", "zone-nw"], "actor_id": "<ACTOR:ZW-B7>", "object_generation": "<OBJECT_GENERATION:2>", "binding_session_id": "<SESSION_ID:1>", "binding_preserved": True},
               [artifact(client, "scenario ZW-B7 passed")], {"kind": "typed-state", "assert": "same player receives ZoneChanged and ZoneState after both crossings"}),
        record("ZW-B8", "target-player-actor", "gateway", {"target_commit": True, "command_44_dropped": True, "disconnect": "ProtocolError", "rebind_same_actor": True},
               [artifact("raw/b8-reference/logs/client.log", "scenario ZW-B8 passed"), artifact("raw/b8-reference/logs/session-route-proxy-gateway.log", "blocked-command-44")],
               {"kind": "typed-state", "assert": "reconnect JoinWorldRes reports target zone for same actor"}),
    ]


def flow_records() -> list[dict[str, Any]]:
    zone1 = "raw/full-reference/logs/zone-node-1.log"
    zone2 = "raw/full-reference/logs/zone-node-2.log"
    gateway = "raw/full-reference/logs/gateway.log"
    ops = "raw/full-reference/logs/ops.log"
    events = [
        ("ZW-B2", "source-zone", "target-zone", "join-sent", {"event_id": "zlink.message_flow", "phase": "sent", "packet": "JoinSpot", "flow": "<FLOW:JOIN>", "corr": "<CORR:JOIN>", "outcome": "succeeded"}, zone1, "packet=JoinSpot"),
        ("ZW-B2", "target-zone", "source-zone", "target-ready", {"event": "canonical_ready_sent", "relocation": "<RELOCATION_ID:1>", "attempt": 1}, zone2, "canonical_ready_sent"),
        ("ZW-B2", "target-zone", "location-store", "authority-cutover", {"event": "relocation_session_route", "commit": "present", "actor": "<ACTOR:ZW-B2>"}, zone2, "relocation_session_route commit=present actor=b2-"),
        ("ZW-B2", "target-zone", "actor-runtime", "restore", {"object_generation": "<OBJECT_GENERATION:1>", "owner": "<RID:TARGET>", "restored_application_state": True}, zone2, "actor_state_native_bound actor=b2-"),
        ("ZW-B5", "source-old-route", "target-actor", "follow-relay-one-way", {"event_id": "zlink.message_flow", "packet": "MessageFollowProbeMsg", "flow": "<FLOW:FOLLOW_ONE_WAY>", "corr": "<CORR:FOLLOW_ONE_WAY>", "outcome": "succeeded"}, gateway, "packet=MessageFollowProbeMsg actor=b5-"),
        ("ZW-B6", "source-old-route", "target-actor", "follow-relay-request", {"event_id": "zlink.message_flow", "packet": "MessageFollowProbeReq", "flow": "<FLOW:FOLLOW_REQUEST>", "corr": "<CORR:FOLLOW_REQUEST>", "outcome": "succeeded"}, gateway, "packet=MessageFollowProbeReq actor=b6-"),
        ("ZW-B6", "target-actor", "gateway-probe", "reply-received", {"event_id": "zlink.message_flow", "phase": "reply_received", "packet": "MessageFollowProbeReq", "flow": "<FLOW:FOLLOW_REQUEST>", "corr": "<CORR:FOLLOW_REQUEST>", "outcome": "succeeded"}, gateway, "phase=reply_received surface=actor kind=request"),
        ("ZW-C4", "zone-timer", "ops", "timer-failure-report", {"event_id": "zlink.message_flow", "packet": "ReportSpotEventMsg", "kind": "TimerHandlerFailed", "trigger_outcome": "error", "error_type": "InvalidOperationException", "report_flow_outcome": "succeeded"}, ops, "kind=TimerHandlerFailed"),
    ]
    return [record(s, p, c, {"stage": stage, **payload}, [artifact(path, selector)],
                   {"kind": "typed-state", "assert": "event payload/ordering is paired with the named scenario wire or state record"})
            for s, p, c, stage, payload, path, selector in events]


def store_records() -> list[dict[str, Any]]:
    monitor = "raw/redis-b2-reference/redis-monitor.log"
    pending_monitor = "raw/redis-pending-follow-reference/redis-monitor.log"
    pending_stdout = "raw/redis-pending-follow-reference/pending-follow.stdout.log"
    e5_monitor = "raw/redis-e5-reference/redis-monitor.log"
    return [
        record("ZW-B2", "location-store", "reference-observer", {"point": "before", "key": "authority\\0actor\\0<ACTOR:ZW-B2>", "exists": False},
               [artifact(monitor, "first authority\\x00actor\\x00b2- put")], {"kind": "store-state", "assert": "key missing before creation reservation"}),
        record("ZW-B2", "source-owner", "location-store", {"point": "source-authority", "namespace": "{zlink-location-v3}:opaque", "record_version": 1, "allocation_state": "active", "object_generation": "<OBJECT_GENERATION:1>", "authority_owner_generation": "<OWNER_GENERATION:1>", "owner_rid": "<RID:SOURCE>"},
               [artifact(monitor, "authority\\x00actor\\x00b2- allocation state active source")], {"kind": "store-state", "assert": "opaque record and decoded JSON agree"}),
        record("ZW-B2", "target-owner", "location-store", {"point": "target-authority", "namespace": "{zlink-location-v3}:opaque", "record_version": 1, "allocation_state": "active", "object_generation": "<OBJECT_GENERATION:1>", "authority_owner_generation": "<OWNER_GENERATION:2>", "owner_rid": "<RID:TARGET>", "owner_changed": True},
               [artifact(monitor, "authority\\x00actor\\x00b2- target owner put")], {"kind": "store-state", "assert": "object generation stable; authority owner generation and RID change"}),
        record("ZW-B2", "relocation-runtime", "relocation-store", {"point": "clean-cross-owner-relocation", "namespace_pattern": "<PREFIX>:relocation:*", "redis_access_count": 0, "key_count": 0, "assertion": "absent"},
               [artifact(monitor, "whole-artifact absence scan: literal relocation: occurs zero times")], {"kind": "negative-store-state", "assert": "typed ActorLocationProbeRes proves source-to-target authority change while the Relocation Store remains untouched"}),
        record("ZW-B6-pending-cutover", "source-memory-relay", "eight-request-callers", {"point": "pending-requests-through-cutover", "request_count": 8, "reply_count": 8, "payload_and_correlation_preserved": True, "owner_changed": True, "object_generation_stable": True, "relocation_store_access_count": 0, "relocation_store_key_count": 0},
               [artifact(pending_stdout, "scenario ZW-B6-pending-cutover passed ... requests=8"), artifact(pending_monitor, "whole-artifact absence scan: literal relocation: occurs zero times")], {"kind": "negative-store-state", "assert": "typed request replies complete after cutover through the spec-28 in-memory relay window with no Relocation Store access"}),
        record("ZW-E5", "ops", "maintenance-store", {"point": "before-restart", "key": "<PREFIX>:maintenance", "row": {"zone-node-2": True}},
               [artifact(e5_monitor, '"HSET" "<PREFIX>:maintenance" "zone-node-2" "1"')], {"kind": "store-state", "assert": "typed SetMaintenanceRes enabled=true and hash row=1"}),
        record("ZW-E5", "replacement-zone-node-2", "maintenance-store", {"point": "after-restart", "key": "<PREFIX>:maintenance", "row": {"zone-node-2": True}, "restored": True},
               [artifact("raw/redis-e5-reference/logs/zone-node-2.log", "maintenance restored. node=zone-node-2, own=True")], {"kind": "store-state", "assert": "typed diagnostics maintenance=true after restart; same hash row remains"}),
    ]


def ops_records() -> list[dict[str, Any]]:
    client = "raw/full-reference/logs/client.log"
    ops = "raw/full-reference/logs/ops.log"
    runner = "raw/full-reference/logs/runner.log"
    return [
        record("ZW-C1", "zone-node", "ops-client", {"registered": True, "connected": True, "states_are_distinct": True}, [artifact(client, "scenario ZW-C1 passed")], {"kind": "typed-wire", "wire_record": "WatchNodesRes"}),
        record("ZW-C2", "zone-node-2", "ops-client", {"shutdown": "graceful", "registered": False, "connected": False}, [artifact(client, "scenario ZW-C2 passed")], {"kind": "typed-wire", "wire_record": "NodeStatusNotify"}),
        record("ZW-C3", "zone-node-2", "ops-client", {"shutdown": "crash", "connected": False, "registered_after_ttl": False}, [artifact(client, "scenario ZW-C3 passed")], {"kind": "typed-wire", "wire_record": "NodeStatusNotify"}),
        record("ZW-C4", "zone-node-1", "ops-client", {"kind": "TimerHandlerFailed", "node": "zone-node-1", "delivery_count": 1}, [artifact(ops, "kind=TimerHandlerFailed")], {"kind": "typed-wire", "wire_record": "NodeAlertNotify"}),
        record("ZW-D1", "ops", "all-zone-spots", {"announcement_id": "<ANNOUNCEMENT_ID:1>", "client_duplicates": 0, "subscriber_delivery_per_node": 1, "spot_delivery_per_zone": 1}, [artifact(client, "scenario ZW-D1 passed"), artifact(runner, "scenario ZW-D1-subscribers passed"), artifact(runner, "scenario ZW-D1-spots passed")], {"kind": "typed-wire", "wire_record": "AnnounceWorldRes/WorldAnnounceNotify"}),
        record("ZW-D2", "ops", "subscriber-only-zone-node-3", {"publisher_node_list": None, "received": True, "hosted_zones": 0}, [artifact(runner, "scenario ZW-D2 passed")], {"kind": "typed-state", "assert": "typed WorldAnnounceEvent reaches unknown subscriber"}),
        record("ZW-E1", "ops-client", "target-node", {"target": "zone-node-2", "enabled": True, "non_target_unchanged": True}, [artifact(client, "scenario ZW-E1 passed")], {"kind": "typed-wire", "wire_record": "SetMaintenanceRes"}),
        record("ZW-E2", "source-player", "maintenance-target", {"cross_node_entry": "ZoneMaintenance"}, [artifact(client, "scenario ZW-E2 passed")], {"kind": "typed-wire", "wire_record": "MoveRejectedNotify"}),
        record("ZW-E3", "player", "same-zone-spot", {"same_zone_move_allowed": True, "maintenance": True}, [artifact(client, "scenario ZW-E3 passed")], {"kind": "typed-state", "assert": "ZoneStateNotify advances position"}),
        record("ZW-E4", "player", "different-zone-same-owner", {"cross_zone_entry": "ZoneMaintenance"}, [artifact(client, "scenario ZW-E4 passed")], {"kind": "typed-wire", "wire_record": "MoveRejectedNotify"}),
        record("ZW-E5", "replacement-node", "ops-client", {"desired_state_persisted": True, "maintenance_after_restart": True}, [artifact(client, "scenario ZW-E5 passed")], {"kind": "store-state", "store_record": "after-restart"}),
        record("ZW-E6", "target-node", "ops-client", {"diagnostics_reachable": True, "maintenance": True, "error": None}, [artifact(client, "scenario ZW-E6 passed")], {"kind": "typed-wire", "wire_record": "NodeDiagnosticsRes"}),
    ]


def bot_records() -> list[dict[str, Any]]:
    zone1 = "raw/full-reference/logs/zone-node-1.log"
    zone2 = "raw/full-reference/logs/zone-node-2.log"
    runner = "raw/full-reference/logs/runner.log"
    roster = [
        ["bot-nw-x", "zone-nw", 10, 15, 1, 0], ["bot-nw-y", "zone-nw", 15, 10, 0, 1],
        ["bot-ne-x", "zone-ne", 90, 15, -1, 0], ["bot-ne-y", "zone-ne", 85, 10, 0, 1],
        ["bot-sw-x", "zone-sw", 10, 85, 1, 0], ["bot-sw-y", "zone-sw", 15, 90, 0, -1],
        ["bot-se-x", "zone-se", 90, 85, -1, 0], ["bot-se-y", "zone-se", 85, 90, 0, -1],
    ]
    return [
        record("ZW-F1", "bot-spawner", "world", {"count": 8, "roster": roster}, [artifact(runner, "scenario ZW-F1-population passed")], {"kind": "typed-state", "assert": "eight distinct actor ids with fixed initial state"}),
        record("ZW-F2", "source-bot-actor", "target-zone-spot", {"actor": "<BOT:X_AXIS_WITNESS>", "initial_entry": False, "seen_on_distinct_owners": True}, [artifact(zone1, "bot=True, initial=False"), artifact(zone2, "same bot id, bot=True, initial=False")], {"kind": "store-state", "assert": "same actor authority changes RID"}),
        record("ZW-F3", "zone-spot", "bot-actor", {"bound_session": False, "push_attempt_count": 0}, [artifact(runner, "scenario ZW-F4-no-push passed")], {"kind": "typed-state", "assert": "bot excluded from DeliverZoneStateMsg/DeliverWorldAnnounceMsg recipients"}),
        record("ZW-F4", "maintenance-target", "bot-actor", {"rejection": "ZoneMaintenance", "direction_before": "<DIRECTION:1>", "direction_after": "negate(<DIRECTION:1>)", "later_move_away": True}, [artifact("raw/full-reference/logs/client.log", "scenario ZW-F4 passed")], {"kind": "typed-state", "assert": "bot position reverses in subsequent ZoneState"}),
    ]


def lifecycle_records() -> list[dict[str, Any]]:
    full_ops = "raw/full-reference/logs/ops.log"
    g4_client = "raw/g4-reference/logs/client.log"
    return [
        record("ZW-G1", "zone-node-1/2", "ops", {"rid_shape": "^zn-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$", "distinct": True}, [artifact("raw/full-reference/logs/routing-id-self-check.log", "scenario ZW-G1 passed")], {"kind": "typed-state", "assert": "two observed RIDs match regex and differ"}),
        record("ZW-G2", "zone-node-2", "ops", {"start_order": ["zone-node-2", "zone-node-1"], "canonical_rid": True, "topology_ready": True}, [artifact("raw/full-reference/logs/routing-id-self-check.log", "scenario ZW-G2-rid passed"), artifact("raw/full-reference/logs/client.log", "scenario ZW-G2 passed")], {"kind": "typed-state", "assert": "typed WatchNodesRes reports both nodes"}),
        record("ZW-G3", "replacement-zone-node-2", "ops", {"stop": "normal", "node_id_before": "zone-node-2", "node_id_after": "zone-node-2", "rid_before": "<RID:G3_OLD>", "rid_after": "<RID:G3_NEW>", "rid_changed": True, "fresh_join": True}, [artifact(full_ops, "node status observed. node=zone-node-2, rid=zn-"), artifact("raw/full-reference/logs/routing-id-self-check.log", "scenario ZW-G3 passed")], {"kind": "typed-wire", "wire_record": "JoinWorldRes"}),
        record("ZW-G4", "ready-target", "source-caller", {"stop": "crash", "inflight_terminal": "Unavailable", "automatic_failover": False}, [artifact(g4_client, "packet=CrashRelocationProbeRes"), artifact(g4_client, "scenario ZW-G4 passed")], {"kind": "typed-wire", "assert": "CrashRelocationProbeRes.error == Unavailable"}),
        record("ZW-G4", "replacement-zone-node-2", "gateway-probe", {"node_id": "zone-node-2", "rid_before": "<RID:G4_OLD>", "rid_after": "<RID:G4_NEW>", "rid_changed": True, "fresh_actor_only": True}, [artifact(g4_client, "scenario ZW-G4-fresh owner=")], {"kind": "typed-wire", "assert": "FreshActorProbeRes owner equals replacement RID"}),
        record("ZW-G5", "zone-node-config", "runner", {"fixed_routing_id_occurrences": 0}, [artifact("raw/full-reference/logs/routing-id-self-check.log", "scenario ZW-G5 passed")], {"kind": "typed-state", "assert": "observed RIDs are generated and source/config scan is empty"}),
    ]


def browser_records() -> list[dict[str, Any]]:
    output = "raw/extraction-full/runner.stdout.log"
    return [
        record("ZW-B2", "browser-game", "gateway", {"supplied_endpoints": ["gateway", "ops"], "join_visible": True, "cross_owner_move_visible": True, "authoritative_state_preserved": True}, [artifact(output, "one browser socket keeps authoritative state across an Ops-observed owner boundary")], {"kind": "typed-wire", "assert": "JoinWorldRes + ZoneChangedNotify + ZoneStateNotify"}),
        record("ZW-E1", "browser-ops", "ops", {"maintenance_applied": True, "diagnostics_visible": True}, [artifact(output, "operations page applies owner-targeted maintenance and diagnostics")], {"kind": "typed-wire", "assert": "SetMaintenanceRes + NodeDiagnosticsRes"}),
        record("ZW-C3", "zone-node-2", "browser-ops", {"node_loss_push_visible": True, "replacement_connected": True}, [artifact(output, "operations page receives node loss through server push")], {"kind": "typed-wire", "assert": "NodeStatusNotify connected=false then true"}),
        record("browser", "playwright", "reference-gate", {"tests": 3, "passed": 3, "failed": 0, "headless": True}, [artifact(output, "3 passed")], {"kind": "typed-state", "assert": "API/headless transcript scenario markers also pass in same lane"}),
    ]


SET_BUILDERS = {
    "typed-json-wire-vectors": wire_records,
    "world-state-transcript": world_records,
    "relocation-transcript": relocation_records,
    "stage-flow-event-ledger": flow_records,
    "store-snapshots": store_records,
    "ops-fanout-transcript": ops_records,
    "bot-transcript": bot_records,
    "lifecycle-transcript": lifecycle_records,
    "browser-transcript": browser_records,
}

SET_DESCRIPTIONS = {
    "typed-json-wire-vectors": "Canonical request, reply, push, border, Ops, maintenance, diagnostics, and Follow JSON vectors.",
    "world-state-transcript": "A1-A5/B1/B3/B4 ordered world-state and border-cache semantics.",
    "relocation-transcript": "B2/B5/B6/B7/B8 identity, binding, Follow, cutover, and terminal boundaries.",
    "stage-flow-event-ledger": "Normalized join, ready, restore, cutover, Follow, dispatch, and C4 failure events.",
    "store-snapshots": "Location authority, Maintenance persistence, and both negative Relocation Store assertions.",
    "ops-fanout-transcript": "C/D/E runtime observation, fanout, maintenance, admission, and persistence semantics.",
    "bot-transcript": "F1-F4 fixed roster, relocation witness, no-push, and reversal semantics.",
    "lifecycle-transcript": "G1-G5 generated RID, normal replacement, crash terminal, and fresh-owner semantics.",
    "browser-transcript": "Mandatory three-test Playwright lane paired with API/headless typed assertions.",
}


REQUIRED_MARKERS = [
    *(f"scenario ZW-A{i} passed" for i in range(1, 6)),
    *(f"scenario ZW-B{i} passed" for i in (1, 2, 3, 5, 6, 7)),
    "scenario ZW-C1 passed", "scenario ZW-C4 passed", "scenario ZW-D1 passed",
    *(f"scenario ZW-E{i} passed" for i in (1, 2, 3, 4, 6)),
    *(f"scenario ZW-F{i} passed" for i in (1, 3, 4)),
]

FULL_PHASE_MARKERS = [
    "zoneworld-relocation=completed",
    "zoneworld-border-sync=completed",
    "zoneworld-ops-observe=completed",
    "zoneworld-ops-announce=completed",
    "zoneworld-ops-maintenance=completed",
    "zoneworld=completed",
]

BROWSER_MARKERS = [
    "one browser socket keeps authoritative state across an Ops-observed owner boundary",
    "operations page applies owner-targeted maintenance and diagnostics",
    "operations page receives node loss through server push",
    "3 passed",
]

REQUIRED_LIVE_PACKET_FIELDS = {
    "JoinWorldRes": ("playerId", "zoneId", "x", "y", "error"),
    "MoveRejectedNotify": ("reason", "x", "y"),
    "ZoneStateNotify": ("zoneId", "tick", "players"),
    "ZoneChangedNotify": ("playerId", "zoneId"),
    "WorldAnnounceNotify": ("announcementId", "text"),
    "CrashRelocationProbeRes": ("error",),
}


def observed_wire_semantics(*paths: Path) -> dict[str, Any]:
    observed_fields: dict[str, set[tuple[str, ...]]] = {}
    rejection_reasons: set[str] = set()
    crash_errors: set[str] = set()
    pattern = re.compile(r"packet=([^ ]+) .*preview_b64=([^\r\n ]*)")
    for path in paths:
        text = path.read_text(errors="replace")
        for match in pattern.finditer(text):
            packet, encoded = match.groups()
            if not encoded:
                continue
            try:
                payload = json.loads(base64.b64decode(encoded))
            except (ValueError, json.JSONDecodeError):
                continue
            if not isinstance(payload, dict):
                continue
            observed_fields.setdefault(packet, set()).add(tuple(payload))
            if packet == "MoveRejectedNotify" and isinstance(payload.get("reason"), str):
                rejection_reasons.add(payload["reason"])
            if packet == "CrashRelocationProbeRes" and isinstance(payload.get("error"), str):
                crash_errors.add(payload["error"])
    return {
        "fields": {name: sorted(values) for name, values in observed_fields.items()},
        "rejection_reasons": sorted(rejection_reasons),
        "crash_errors": sorted(crash_errors),
    }


def validate_full_capture(root: Path, capture_name: str) -> dict[str, Any]:
    capture = root / f"raw/{capture_name}"
    metadata = json.loads((capture / "capture-metadata.json").read_text())
    if metadata["exit_code"] != 0:
        raise ValueError(f"{capture_name} reference lane exit code was {metadata['exit_code']}")
    output = (capture / "runner.stdout.log").read_text(errors="replace")
    missing = [marker for marker in FULL_PHASE_MARKERS + BROWSER_MARKERS if marker not in output]
    if missing:
        raise ValueError(f"{capture_name} missing full-lane evidence: {missing}")
    if "scenario " in output and " FAILED:" in output:
        raise ValueError(f"{capture_name} contains a scenario failure")
    semantics = observed_wire_semantics(
        capture / "runner.stdout.log",
        capture / "main/logs/client.log",
    )
    fields = semantics["fields"]
    for packet, expected in REQUIRED_LIVE_PACKET_FIELDS.items():
        if expected not in fields.get(packet, []):
            raise ValueError(
                f"{capture_name} missing live {packet} field order {expected}; "
                f"observed={fields.get(packet, [])}")
    required_rejections = {"OutOfRange", "TooFar", "DiagonalCrossing", "ZoneMaintenance"}
    if not required_rejections <= set(semantics["rejection_reasons"]):
        raise ValueError(f"{capture_name} rejection semantics drifted: {semantics['rejection_reasons']}")
    if semantics["crash_errors"] != ["Unavailable"]:
        raise ValueError(f"{capture_name} crash terminal semantics drifted: {semantics['crash_errors']}")
    return semantics


def validate_raw(root: Path) -> None:
    client = (root / "raw/full-reference/logs/client.log").read_text(errors="replace")
    runner = (root / "raw/full-reference/logs/runner.log").read_text(errors="replace")
    combined = client + "\n" + runner
    missing = [marker for marker in REQUIRED_MARKERS if marker not in combined]
    if missing:
        raise ValueError(f"missing full-reference markers: {missing}")
    for marker in ("scenario ZW-G4 passed", "scenario ZW-B8 passed"):
        if marker not in runner:
            raise ValueError(f"missing child marker in full reference: {marker}")

    observed: dict[str, dict[str, Any]] = {}
    pattern = re.compile(r"packet=([^ ]+) .*preview_b64=([^\r\n]*)")
    for match in pattern.finditer(client):
        packet, encoded = match.groups()
        if packet.startswith("$zlink.") or not encoded or packet in observed:
            continue
        try:
            observed[packet] = json.loads(base64.b64decode(encoded))
        except (ValueError, json.JSONDecodeError):
            continue
    required_packets = {"JoinWorldRes", "MoveRejectedNotify", "ZoneStateNotify", "ZoneChangedNotify"}
    if not required_packets <= observed.keys():
        raise ValueError(f"missing observed typed packets: {sorted(required_packets - observed.keys())}")
    if list(observed["JoinWorldRes"]) != ["playerId", "zoneId", "x", "y", "error"]:
        raise ValueError("JoinWorldRes wire field order/shape drifted")
    if list(observed["ZoneStateNotify"]) != ["zoneId", "tick", "players"]:
        raise ValueError("ZoneStateNotify wire field order/shape drifted")

    # Final ZoneWorld ruling: this app-admission-only sample has no spec-30 host-maintenance
    # move path and no Instance Spot. Both a clean B2 and eight pending requests through
    # cutover therefore prove the negative contract: no Relocation Store access at all.
    for label in ("redis-b2-reference", "redis-pending-follow-reference"):
        redis_monitor = (root / f"raw/{label}/redis-monitor.log").read_text(
            errors="replace")
        if "relocation:" in redis_monitor:
            raise ValueError(
                f"STOP: {label} unexpectedly accessed the ZoneWorld Relocation Store prefix")
    pending = (root / "raw/redis-pending-follow-reference/pending-follow.stdout.log").read_text(
        errors="replace")
    if ("scenario ZW-B6-pending-cutover passed" not in pending
            or "requests=8" not in pending):
        raise ValueError("pending-through-cutover probe did not preserve all eight replies")

    validate_full_capture(root, "extraction-full")


def write_package(root: Path) -> None:
    validate_raw(root)
    sets_dir = root / "sets"
    sets_dir.mkdir(exist_ok=True)
    manifest_sets = []
    for name, builder in SET_BUILDERS.items():
        path = sets_dir / f"{name}.json"
        payload = {"schema_version": SCHEMA_VERSION, "golden_set": name, "records": builder()}
        encoded = json.dumps(payload, indent=2, ensure_ascii=False) + "\n"
        path.write_text(encoded, encoding="utf-8")
        manifest_sets.append({
            "name": name,
            "path": f"sets/{path.name}",
            "record_count": len(payload["records"]),
            "sha256": hashlib.sha256(encoded.encode()).hexdigest(),
            "port_completion_bar": COMPLETION_BARS[name],
        })
    manifest = {
        "schema_version": SCHEMA_VERSION,
        "package_version": "1.0.0",
        "authority": ".NET ZoneWorld reference implementation",
        "record_required_fields": ["scenario_id", "producer_role", "consumer_role", "normalized_payload", "raw_artifacts", "typed_or_state_oracle"],
        "sets": manifest_sets,
        "raw_artifact_roots": [
            "raw/full-reference",
            "raw/g4-reference",
            "raw/b8-reference",
            "raw/redis-b2-reference",
            "raw/redis-e5-reference",
            "raw/redis-pending-follow-reference",
            "raw/extraction-full",
            "raw/verification-full",
        ],
        "normalization": {
            "dynamic_values": "tagged placeholders; never drop the containing assertion",
            "stable_semantics": "zone/node names, field names, null/optional presence, error kinds, ordering, counts, and predicates remain literal",
            "placeholder_classes": [
                "RID", "UUID", "ACTOR", "PLAYER", "SESSION_ID", "OPID", "PROBE",
                "FLOW", "CORR", "PORT", "TIMESTAMP", "TICK", "COUNT", "DIRECTION",
                "OWNER_GENERATION", "OBJECT_GENERATION", "RELOCATION_ID", "SHA256",
            ],
            "widened_not_dropped": [
                "periodic tick values",
                "runtime population counts sampled while bots move",
                "process-assigned identities and endpoints",
                "flow/correlation and lifecycle tokens",
                "wall-clock timestamps",
            ],
        },
        "verification": {
            "reference_capture": "raw/extraction-full",
            "post_extraction_capture": "raw/verification-full",
            "semantic_comparison": "phase/browser markers, typed packet field order, rejection reasons, and crash Unavailable terminal",
        },
    }
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    rows = "\n".join(
        f"| `{name}` | {SET_DESCRIPTIONS[name]} | {COMPLETION_BARS[name]} |"
        for name in SET_BUILDERS)
    readme = f"""# ZoneWorld .NET golden package

Schema version: `{SCHEMA_VERSION}`  
Package version: `1.0.0`  
Authority: the .NET ZoneWorld reference implementation.

## Record schema and provenance

Every record contains `scenario_id`, `producer_role`, `consumer_role`,
`normalized_payload`, `raw_artifacts`, and `typed_or_state_oracle`. A raw artifact entry names
the archived file and the selector or absence scan used to derive the record. A log string is
never the sole oracle: every record pairs it with a typed wire, typed state, or store-state
assertion.

## Normalization rules

- Preserve field names and order, null and optional-field presence, literal error kinds,
  scenario ordering, exact counts, absence assertions, and relationship predicates.
- Replace only process-assigned or timing-dependent values with stable tagged placeholders.
  Placeholder classes are `<RID:n>`, `<UUID:n>`, `<ACTOR:n>`, `<PLAYER:n>`,
  `<SESSION_ID:n>`, `<OPID:n>`, `<PROBE:n>`, `<FLOW:n>`, `<CORR:n>`, `<PORT:n>`,
  `<TIMESTAMP:n>`, `<TICK:n>`, `<COUNT:n>`, generation/token tags, and equivalent named tags.
- Reuse the same tag wherever the same value participates in a correlation. Never erase a
  containing field or assertion merely because its value is nondeterministic.
- Runtime bot positions and periodic ticks may vary. The package retains their roster,
  ordering, counts, directions, owner-change predicates, and before/after relationships; only
  the sampled tick/count/value receives a tagged placeholder where needed.
- Redis endpoints, key prefixes, RIDs, lease/lifecycle tokens, and timestamps are masked while
  namespaces, row separation, record version, allocation state, and generation relationships
  remain literal.

ZoneWorld is spec-30-exempt and has no Instance Spot. Consequently the two required Relocation
Store fixtures are negative: clean ZW-B2 and the eight-request pending-through-cutover probe
both complete with zero `relocation:`-prefix access. Positive Relocation Store fixtures belong
to the stage-9 spec-30/Instance Spot lanes.

## Nine sets and port completion bars

The completion-bar text below is copied verbatim from recon §5.

| Golden set | Contents | Port completion bar |
|---|---|---|
{rows}

## Re-extraction and verification

Run a traced full lane into a new empty raw directory with `tools/capture_full_lane.py`, then
run `tools/extract_goldens.py`. After extraction, capture the full lane again as
`raw/verification-full` and run `tools/extract_goldens.py --verify`. Verification checks set
hashes and record shape, the complete phase/browser bar, live typed JSON field order, all four
move-rejection reasons, the G4 `Unavailable` terminal, and both zero-access store assertions.
The capture tools remove only the ZoneWorld `/tmp/tmp.*` run directories that they archive.
"""
    (root / "README.md").write_text(readme, encoding="utf-8")


def verify_package(root: Path) -> None:
    validate_raw(root)
    reference_semantics = validate_full_capture(root, "extraction-full")
    verification_semantics = validate_full_capture(root, "verification-full")
    if reference_semantics != verification_semantics:
        raise ValueError(
            "post-extraction semantic lane drifted from the extraction reference: "
            f"reference={reference_semantics}, verification={verification_semantics}")
    manifest = json.loads((root / "manifest.json").read_text())
    if manifest["schema_version"] != SCHEMA_VERSION:
        raise ValueError("manifest schema version mismatch")
    for item in manifest["sets"]:
        path = root / item["path"]
        encoded = path.read_bytes()
        if hashlib.sha256(encoded).hexdigest() != item["sha256"]:
            raise ValueError(f"checksum mismatch: {path}")
        payload = json.loads(encoded)
        if len(payload["records"]) != item["record_count"]:
            raise ValueError(f"record count mismatch: {path}")
        for rec in payload["records"]:
            missing = set(manifest["record_required_fields"]) - rec.keys()
            if missing:
                raise ValueError(f"record missing {missing}: {path}")
            if not rec["typed_or_state_oracle"]:
                raise ValueError(f"log-only oracle forbidden: {path}")
            for raw in rec["raw_artifacts"]:
                artifact_path = root / raw["path"]
                if not artifact_path.is_file():
                    raise ValueError(f"raw artifact is missing: {artifact_path}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    if args.verify:
        verify_package(root)
    else:
        write_package(root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
