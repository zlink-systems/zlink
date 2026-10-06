"""Role config and endpoint-manifest entries for every role process of a cell (§5.1, §6.3).

The scenario table (`scenarios.py`) says which roles a cell starts and what each listens on; this module only turns
that into files. It knows nothing about how a language launches a role.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Callable

from scenarios import ROLE_KINDS, Cell, Listen, RoleUse, instances


@dataclass
class PlannedRole:
    name: str  # server-<kind>-<instance>: log, process and original file stem (§15.1)
    executable: str
    config_file: str
    config: dict
    manifest: dict
    ports: list[int]
    source: bool


def endpoint_key(listen: Listen, topology: str | None) -> str | None:
    """The transport endpoint key a listener publishes, or None when the topology excludes it."""
    if listen.when is not None and listen.when != topology:
        return None
    return {"topology": "mesh" if topology == "routemesh" else "clientserver"}.get(listen.key, listen.key)


def object_ids(cell: Cell, values: dict, config_hash: str) -> dict:
    """SpotIds and ActorIds are generated per cell after the hash is fixed (§15.1); the hash keeps only the rules."""
    stem = config_hash[:12]
    actors = values.get("connections") or values.get("logical_streams") or 0
    return {"spot": [f"perf-spot-{stem}-{i}" for i in range(values.get("spot_count") or 0)],
            "actor": [f"perf-actor-{stem}-{i}" for i in range(actors)]}


def awaits_remote_targets(scenario, role: RoleUse, mode: str) -> bool:
    """A send/send source on an object-client role is the only Server of its return ChannelName (§10.4, §10.10):
    its own channel has no remote target to wait for. Every other role waits for the targets of its channel."""
    return not (role.source and role.object_role == "ObjectClient" and mode == "send-send" and scenario.channel)


def plan_roles(cell: Cell, values: dict, common: dict, stream_scheme: str, reserve: Callable[[], int]) -> list[PlannedRole]:
    """`common` carries what every role config shares: runId, cellId, configHash, workload, worker, store,
    diagnostics(name -> dict|None), provenance. `reserve` returns a fresh OS-reserved loopback port."""
    scenario = cell.scenario
    ids = object_ids(cell, values, common["configHash"])
    uses = [(role, index) for role in scenario.roles for index in instances(role, cell.subscriber_count)]
    endpoints, admin, trigger = {}, {}, {}
    for role, index in uses:
        key = (role.kind, index)
        admin[key], trigger[key] = reserve(), reserve()
        endpoints[key] = {name: f"{stream_scheme if name == 'stream' else 'tcp'}://127.0.0.1:{reserve()}" for name in
                          (endpoint_key(listen, cell.topology) for listen in role.listens) if name}
    target = next(((r.kind, i) for r, i in uses if not r.source), None)
    mesh_name = "perf-mesh" if any("mesh" in found for found in endpoints.values()) else None
    channel_name = "perf-" + common["runId"] + "-" + common["configHash"][:12] if scenario.channel else None
    planned = []
    for role, index in uses:
        key = (role.kind, index)
        listeners = endpoints[key]
        peer = next(iter(endpoints[target].values()), None) if role.peer and target else None
        name = f"server-{role.kind}-{index}"
        config = {**{k: common[k] for k in ("runId", "cellId", "configHash", "language")},
                  "role": role.kind, "roleInstance": index, "scenario": scenario.name, "mode": cell.mode,
                  "terminal": cell.terminal, "topology": cell.topology, "channelName": channel_name, "meshName": mesh_name,
                  "transportEndpoints": listeners, "peerEndpoint": peer,
                  "metricsUrl": f"http://127.0.0.1:{admin[key]}",
                  "applicationTriggerUrl": f"http://127.0.0.1:{trigger[key]}/app/perf/start", "source": role.source,
                  "objectRole": role.object_role, "awaitRemoteTargets": awaits_remote_targets(scenario, role, cell.mode), "store": common["store"],
                  "spotIds": ids["spot"] if role.objects == "spot" else [],
                  "actorIds": ids["actor"] if role.objects == "actor" else [],
                  "spotCount": cell.spot_count, "subscriberCount": cell.subscriber_count, "worker": common["worker"],
                  "executionMode": scenario.execution, "workload": common["workload"],
                  "diagnostics": common["diagnostics"](name),
                  "provenance": {**common["provenance"], "processKey": name,
                                 **({"fanout": {"noDrop": True}} if role.kind == "publisher" else {})}}
        manifest = {"role": role.kind, "roleInstance": index, "streamEndpoint": listeners.get("stream"),
                    "applicationTriggerUrl": config["applicationTriggerUrl"],
                    "metrics": {"transport": "http", "baseUrl": config["metricsUrl"]},
                    "transportEndpoints": {**listeners, **({"peer": peer} if peer else {})},
                    "spotIds": config["spotIds"], "actorIds": config["actorIds"]}
        ports = [admin[key], trigger[key], *(int(url.rsplit(":", 1)[1]) for url in listeners.values())]
        planned.append(PlannedRole(name, ROLE_KINDS[role.kind], f"role-configs/{role.kind}-{index}.json",
                                   config, manifest, ports, role.source))
    return planned
