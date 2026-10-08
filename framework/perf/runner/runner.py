#!/usr/bin/env python3
"""Language-independent perf runner: phases, collection, aggregation. Measured calls live in each language's role processes."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import resource
import select
import signal
import socket
import subprocess
import sys
import time
import urllib.error
from urllib.parse import urlsplit
import urllib.request
import uuid

from environment import ROOT, collect, digest
from launchers import Launcher, declared_framework_version, launcher
from results import aggregate, null_reason, write_json
from roles import plan_roles
from scenarios import (BY_NAME, CLIENT, EXECUTABLES, MODE_VALUES, OPTIONS, PAYLOADS, ROLE_KINDS, TERMINAL_VALUES,
                       TOPOLOGY_VALUES, Cell, check, expand, owner_files, selected, values)
from store import RunStore, check_available


class UnsupportedCellError(RuntimeError):
    """A required public observation contradicts its contract; measured load must not start."""

    def __init__(self, message: str, source_file: str):
        super().__init__(message)
        self.source_file = source_file


class InvalidSetupError(RuntimeError):
    """The aggregate connector pool does not meet the specified preparation criterion."""


def positive_int(text: str) -> int:
    value = int(text)
    if value <= 0 or value > 2147483647:
        raise argparse.ArgumentTypeError("must be a positive int32")
    return value


def positive_number(text: str) -> float:
    value = float(text)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("must be finite and positive")
    return value


def options(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Common perf runner: every standard §10 scenario and the §11 baselines.")
    parser.add_argument("operation", choices=("single", "matrix", "diagnostic"))
    parser.add_argument("--language", required=True)
    parser.add_argument("--perf-dir", required=True, type=Path, help="the language perf folder holding the role projects and perf-results")
    parser.add_argument("--scenario", choices=tuple(BY_NAME))
    # Numeric and path options come from the option table; which scenario consumes each is decided in scenarios.check().
    for key, option in OPTIONS.items():
        parser.add_argument("--" + key.replace("_", "-"), dest=key, type=positive_int if option.kind == "int" else Path)
    parser.add_argument("--duration-seconds", type=positive_number, default=30)
    parser.add_argument("--warmup-seconds", type=positive_number, default=5)
    parser.add_argument("--payload-size", type=int, choices=PAYLOADS)
    parser.add_argument("--payload-sizes")
    parser.add_argument("--mode", choices=MODE_VALUES)
    parser.add_argument("--terminal", choices=TERMINAL_VALUES)
    parser.add_argument("--channel-topology", choices=TOPOLOGY_VALUES)
    parser.add_argument("--codec", choices=("json",), default="json")
    parser.add_argument("--package-source", choices=("published", "local"), default="published")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--run-id", default=time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()) + "-" + uuid.uuid4().hex[:10])
    args = parser.parse_args(argv)
    if not args.run_id or any(not (c.isascii() and (c.isalnum() or c in "_-")) for c in args.run_id):
        parser.error("--run-id must match [A-Za-z0-9_-]+")
    matrix = args.operation == "matrix"
    if not matrix and args.scenario is None:
        parser.error("run_single.sh requires --scenario")
    if not matrix and args.payload_sizes is not None:
        parser.error("--payload-sizes is consumed only by run_perf.sh")
    if matrix and args.payload_size is not None:
        parser.error("run_perf.sh requires --payload-sizes instead of --payload-size")
    problem = check(args, matrix)
    if problem:
        parser.error(problem)
    for scenario in selected(args):
        consumed = values(scenario, args)
        if consumed["client_count"] > consumed.get("connections", consumed["client_count"]):
            parser.error("client-count must not exceed connections")
    if args.payload_sizes is not None:
        try:
            payloads = [int(part) for part in args.payload_sizes.split(",")]
        except ValueError:
            parser.error("invalid payload matrix")
        if not payloads or len(set(payloads)) != len(payloads) or any(p not in PAYLOADS for p in payloads):
            parser.error("payload sizes must be distinct members of 1024,4096")
        args.payloads = payloads
    else:
        args.payloads = list(PAYLOADS)
    args.perf_dir = args.perf_dir.resolve()
    args.output = (args.output or args.perf_dir / "perf-results" / args.run_id).resolve()
    return args


def agreed_core_version(role_versions: dict, declared: str | None) -> str:
    """Every server role reports the Core it actually loaded; all reports and earlier cells must agree."""
    observed = set(role_versions.values()) | ({declared} - {None})
    if None in role_versions.values() or len(observed) != 1:
        raise RuntimeError(f"Core version differs between roles or is unreported: {role_versions}; declared {declared}")
    return observed.pop()


def agreed_framework_version(observed: str | None, declared: str | None) -> str:
    """The launcher-observed framework package must match the language's fixed release declaration."""
    if not observed or not declared or observed != declared:
        raise RuntimeError(f"Framework version differs from or is missing against the declaration: observed {observed}; declared {declared}")
    return observed


def role_executables(args: argparse.Namespace) -> list[str]:
    """The executables the selected scenarios start, in table order: the Client and each needed role."""
    needed = {CLIENT} | {ROLE_KINDS[role.kind] for scenario in selected(args) for role in scenario.roles}
    return [name for name in EXECUTABLES if name in needed]


def build(args: argparse.Namespace, executables: list[str]) -> None:
    logs = args.output / "logs"
    logs.mkdir()
    build_environment = os.environ | {"ZLINK_PERF_FRAMEWORK_VERSION": declared_framework_version(args.language),
                                      "ZLINK_PERF_PACKAGE_SOURCE": args.package_source}
    for role in executables:
        path = logs / ("build-" + role + ".log")
        with path.open("x") as log:
            result = subprocess.run(launcher(args.language).build(args.perf_dir, role), cwd=ROOT, stdout=log,
                                    stderr=subprocess.STDOUT, env=build_environment)
        if result.returncode:
            raise RuntimeError(f"Release build failed; {path}")


def preflight(args: argparse.Namespace, environment: dict) -> None:
    scenarios = selected(args)
    consumed = [values(scenario, args) for scenario in scenarios]
    if any(scenario.store for scenario in scenarios):
        check_available()
    soft, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
    low, high = map(int, environment["ephemeralPortRange"].split())
    for scenario, v in zip(scenarios, consumed):
        if scenario.driver != "clients":
            continue
        if soft != resource.RLIM_INFINITY and soft < v["connections"] + 256:
            raise ValueError(f"FD limit {soft} is below the Session server's {v['connections'] + 256} required descriptors")
        if v["connections"] + 32 > high - low + 1:
            raise ValueError("Insufficient ephemeral ports for the requested connector pool")
        if int(environment["listenBacklog"]) < min(v["connect_concurrency"], v["connections"]):
            raise ValueError("OS listen backlog is below requested simultaneous connector setup")
    if environment["effectiveProcessorCount"] < 1:
        raise ValueError("No effective processor is available")
    available = int(environment["memoryAvailable"].split()[1]) * 1024
    if environment["memoryLimit"] not in (None, "max") and environment.get("memoryCurrent") is not None:
        available = min(available, int(environment["memoryLimit"]) - int(environment["memoryCurrent"]))
    # A necessary lower bound from the harness's sequence and task-reference arrays, not an estimate of Core queues.
    if any(available <= 16 * (v.get("connections") or v["logical_streams"]) for v in consumed):
        raise ValueError("Available memory cannot hold even the required harness sequence/task-reference arrays")


class OwnedProcesses:
    def __init__(self, cell: Path, redis_container_id: str | None):
        self.cell = cell
        self.redis_container_id = redis_container_id
        self.processes: list[tuple[str, subprocess.Popen]] = []
        self.logs = []
        self.reservations: list[socket.socket] = []

    def reserve(self) -> int:
        reserved = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        reserved.bind(("127.0.0.1", 0))
        self.reservations.append(reserved)
        return reserved.getsockname()[1]

    def release(self, ports: list[int]) -> None:
        for reserved in list(self.reservations):
            if reserved.getsockname()[1] in ports:
                reserved.close()
                self.reservations.remove(reserved)

    def start(self, name: str, command: list[str], ports: list[int], client: bool = False) -> subprocess.Popen:
        self.release(ports)
        log = (self.cell / "logs" / (name + ".log")).open("x")
        self.logs.append(log)
        process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.PIPE if client else subprocess.DEVNULL,
                                   stdout=subprocess.PIPE if client else log, stderr=log, text=False, close_fds=True)
        self.processes.append((name, process))
        write_json(self.cell / "tmp" / (name + "-process.json"), {"pid": process.pid, "command": command})
        return process

    def check(self) -> None:
        for name, process in self.processes:
            if process.poll() is not None:
                raise RuntimeError(f"Owned process {name} PID {process.pid} exited {process.returncode}; logs/{name}.log")

    def cleanup(self) -> None:
        for reserved in self.reservations:
            reserved.close()
        self.reservations.clear()
        # Popen handles are the only process authority. Never search by process name or prefix.
        for _, process in reversed(self.processes):
            if process.poll() is None:
                process.terminate()
        for _, process in reversed(self.processes):
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        write_json(self.cell / "cleanup.json", {"ownedProcesses": [{"name": name, "pid": process.pid,
                    "exitCode": process.returncode, "terminated": process.poll() is not None} for name, process in self.processes],
                    "redisContainerId": self.redis_container_id,
                    "redisReason": "Run-owned Docker Redis; removed by this container ID when the run ends (store.json)."
                    if self.redis_container_id else "This scenario needs no Store."})
        for log in self.logs:
            log.close()


class ClientControl:
    def __init__(self, process: subprocess.Popen, log: Path):
        self.process = process
        self.log = log.open("ab")
        self.buffer = b""

    # A control message is either the one "prepared" report (key "snapshot") or a command reply (key "response").
    def receive(self, seconds: float, key: str) -> dict:
        deadline = time.monotonic() + seconds
        while True:
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                self.log.write(line + b"\n")
                self.log.flush()
                try:
                    value = json.loads(line)
                except json.JSONDecodeError:
                    continue  # Diagnostic text is preserved; only typed control JSON is evidence.
                if not isinstance(value, dict) or "ok" not in value or key not in value:
                    continue
                if not value["ok"]:
                    raise RuntimeError("Client control failure: " + json.dumps(value))
                return value
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("Client JSON control acknowledgement exceeded its configured bound")
            readable, _, _ = select.select([self.process.stdout], [], [], remaining)
            if not readable:
                raise TimeoutError("Client JSON control acknowledgement timed out")
            data = os.read(self.process.stdout.fileno(), 65536)
            if not data:
                raise RuntimeError("Client control pipe closed before acknowledgement")
            self.buffer += data

    def send(self, command: str, request: dict | None = None) -> None:
        value = {"command": command}
        if request is not None:
            value["request"] = request
        self.process.stdin.write((json.dumps(value) + "\n").encode())
        self.process.stdin.flush()

    def call(self, command: str, request: dict | None, seconds: float) -> object:
        self.send(command, request)
        value = self.receive(seconds, "response")["response"]
        if isinstance(value, dict) and (value.get("accepted") is False or value.get("ok") is False):
            raise RuntimeError("Phase acknowledgement rejected: " + json.dumps(value))
        return value

    def close(self) -> None:
        if self.process.poll() is None:
            self.send("stop")
            self.process.wait(timeout=5)
        self.log.close()


HTTP = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def get_json(url: str, timeout: float) -> dict:
    with HTTP.open(url, timeout=timeout) as response:
        return json.load(response)


class AdminConflict(RuntimeError):
    """HTTP 409 from a role admin endpoint."""


def post_json(url: str, body: dict, timeout: float) -> dict:
    request = urllib.request.Request(url, data=json.dumps(body).encode("utf-8"),
                                     headers={"Content-Type": "application/json"}, method="POST")
    try:
        with HTTP.open(request, timeout=timeout) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        body = error.read().decode("utf-8", errors="replace")
        raise (AdminConflict if error.code == 409 else RuntimeError)(f"Admin HTTP {error.code}: {body}") from error


def post_until_accepted(url: str, body: dict, workload: dict, deadline: float) -> dict:
    """POST a phase trigger or reset until the role accepts it. Only the role can tell whether its previous work has
    drained (§4.1); until the deadline a 409 means not yet."""
    while True:
        try:
            return post_json(url, body, workload["adminTimeoutMs"] / 1000)
        except AdminConflict:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.02)


def reset_ack_matches(acknowledgement: object, request: dict) -> bool:
    return isinstance(acknowledgement, dict) and acknowledgement.get("ok") is True and all(
        acknowledgement.get(key) == request[key] for key in ("runId", "cellId", "resetSeq"))


def trigger_ack_matches(acknowledgement: object, request: dict, config_hash: str) -> bool:
    return isinstance(acknowledgement, dict) and acknowledgement.get("accepted") is True and all(
        acknowledgement.get(key) == request[key] for key in ("runId", "cellId", "resetSeq", "phase")) and \
        acknowledgement.get("configHash") == config_hash


def stage_ready(ready: dict, level: bool | str) -> bool:
    """level False = infrastructure, "objects" = infrastructure and objects, True = every stage (§16.1)."""
    if level == "objects":
        return ready["infrastructureReady"] and ready["objectsReady"]
    return ready["ready" if level else "infrastructureReady"]


def wait_ready(owned: OwnedProcesses, roles: list[dict], full: bool | str, cell: Path, stage: str, language: Launcher,
               workload: dict) -> list[dict]:
    deadline = time.monotonic() + workload["setupTimeoutMs"] / 1000
    admin_timeout = workload["adminTimeoutMs"] / 1000
    observed = {}
    pending = list(roles)
    while pending:
        owned.check()
        for role in list(pending):
            key = role["role"] + "-" + str(role["roleInstance"])
            try:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                ready = get_json(role["metrics"]["baseUrl"] + "/perf/ready",
                                 min(admin_timeout, remaining))
                observed[key] = ready
                if stage_ready(ready, full):
                    pending.remove(role)
                elif any("failed" in reason.lower() for reason in ready["reasons"]):
                    raise RuntimeError("Preparation failed: " + json.dumps(ready))
            except urllib.error.HTTPError as error:
                raise RuntimeError(f"Admin readiness HTTP {error.code}: {error.read().decode()}") from error
            except (urllib.error.URLError, ConnectionError, TimeoutError) as error:
                # Listener startup is observed until the setup deadline; no workload retry or propagation sleep.
                observed.setdefault(key, {"url": role["metrics"]["baseUrl"] + "/perf/ready",
                                          "errorType": type(error).__name__, "message": str(error)})
        if time.monotonic() >= deadline:
            write_json(cell / "tmp" / (stage + "-readiness-failed.json"), observed)
            for ready in observed.values():
                status_evidence = next((entry["observedValue"] for entry in ready.get("evidence", []) if entry.get("kind") == "publicStatus"), {})
                channel = status_evidence.get("clientServer", {})
                if (channel.get("localRole") == "Server" and channel.get("readyTargetCount", 0) > 0 and
                        not channel.get("isReady") and ready.get("consumersReady") and status_evidence.get("host", {}).get("isReady")):
                    raise UnsupportedCellError(
                        "ClientServer Server reports Degraded/IsReady=false despite Serving, a Ready target and a typed probe reply. "
                        "Required public status: " + language.clientserver_interface + "; readiness meaning: "
                        "framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md:194. " +
                        language.clientserver_gate, language.clientserver_interface)
            raise TimeoutError(f"Public readiness evidence did not converge inside setupTimeoutMs={workload['setupTimeoutMs']}")
    write_json(cell / "tmp" / (stage + "-readiness.json"), observed)
    return list(observed.values())


def publish_listener_endpoints(role: dict, ready: dict) -> None:
    """Replace wildcard listener addresses with the role's public bound endpoints."""
    configured = role["transportEndpoints"]
    event = next(
        (
            item
            for item in ready.get("evidence", [])
            if item.get("kind") == "boundTransportEndpoints"
        ),
        None,
    )
    actual = event.get("observedValue") if event is not None else {}
    identity = f"{role['role']}-{role['roleInstance']}"
    if not isinstance(actual, dict) or set(actual) != set(configured):
        raise RuntimeError(f"Role {identity} reported an incomplete transport endpoint map.")
    for key, endpoint in actual.items():
        if not isinstance(endpoint, str):
            raise RuntimeError(f"Role {identity} reported a non-text {key} endpoint.")
        try:
            port = urlsplit(endpoint).port
        except ValueError as error:
            raise RuntimeError(f"Role {identity} reported an invalid {key} endpoint.") from error
        if port is None or port == 0:
            raise RuntimeError(f"Role {identity} did not report the bound {key} endpoint.")
    role["transportEndpoints"] = actual
    role["streamEndpoint"] = actual.get("stream")


def wait_roles_complete(owned: OwnedProcesses, roles: list[dict], workload: dict, deadline: float, stage: str) -> None:
    """Poll each role until its phase is complete, sharing the phase drain deadline (§4.1)."""
    pending = list(roles)
    admin_timeout = workload["adminTimeoutMs"] / 1000
    observed = {}
    while pending:
        owned.check()
        for role in list(pending):
            name = f"server-{role['role']}-{role['roleInstance']}.json"
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            snapshot = get_json(role["metrics"]["baseUrl"] + "/perf/stats", min(admin_timeout, remaining))
            observed[name] = snapshot
            if snapshot.get("phase") == "complete":
                pending.remove(role)
        if pending and time.monotonic() >= deadline:
            write_json(owned.cell / "tmp" / (stage + "-completion-failed.json"), observed)
            raise TimeoutError(f"Role {stage} did not complete before its deadline; see tmp/{stage}-completion-failed.json")
        if pending:
            time.sleep(0.02)


def comparison(args: argparse.Namespace, cell: Cell, env: dict) -> tuple[dict, str]:
    """The §15.1 comparison input; every part comes from the scenario table and the options it consumes."""
    scenario, v = cell.scenario, values(cell.scenario, args)
    cs = scenario.driver == "clients"
    workload = {"payloadSize": cell.payload, "durationSeconds": args.duration_seconds, "warmupSeconds": args.warmup_seconds,
                "connections": v.get("connections"),
                "logicalStreams": v.get("logical_streams"), "clientCount": v["client_count"],
                "connectConcurrency": v.get("connect_concurrency"),
                "drainTimeoutMs": 30000, "setupTimeoutMs": 30000, "adminTimeoutMs": 5000,
                "socketSendTimeoutMs": math.ceil((args.warmup_seconds + args.duration_seconds) * 1000) + 30000}
    pool = v.get("worker_pool_size")
    comparable = {"language": args.language, "scenario": scenario.name, "mode": cell.mode, "terminal": cell.terminal,
                  "topology": cell.topology, "discovery": scenario.discovery, "objectRole": scenario.object_roles,
                  "executionMode": scenario.execution,
                  "spotMapping": {"count": cell.spot_count, "rule": "streamId mod spotCount"} if cell.spot_count else None,
                  "actorMapping": None if not scenario.uses(objects="actor") else
                  "one Actor per connector ID" if cs else "one ActorId per logical stream",
                  "packageSource": args.package_source,
                  "subscriberCount": cell.subscriber_count,
                  "worker": {"algorithm": "xorshift32-v1", "taskMillis": v["worker_task_millis"], "minThreads": pool, "maxThreads": pool,
                             "idleTimeoutMs": 60000} if scenario.worker else None,
                  "splitRule": "q=N/P,r=N%P,count=q+(i<r),first=i*q+min(i,r)" if cs else "one source; stream IDs 0..N-1",
                  "workload": workload, "serializer": env["serializer"],
                  "cpu": {key: env[key] for key in ("cpuModel", "effectiveProcessorCount", "cpuQuota", "cpuset", "cpuAffinity")},
                  "memoryLimit": env["memoryLimit"], "runtimeOptions": env["runtimeOptions"],
                  "runtimeSettings": env.get("runtimeSettings", {}), "installedRuntimes": env.get("installedRuntimes"),
                  "workloadHash": None, "repetition": None}
    if scenario.name == "pubsub-fanout-echo":
        comparable["publisherChannel"] = {"noDrop": True}
    comparable["diagnostics"] = "Normal" if args.operation == "diagnostic" else "Off"
    exact = json.dumps(comparable, separators=(",", ":"), sort_keys=True, ensure_ascii=False)
    return comparable, exact


def cell_run(args: argparse.Namespace, cell_spec: Cell, env: dict, store: RunStore) -> dict:
    scenario, language = cell_spec.scenario, launcher(args.language)
    comparable, exact = comparison(args, cell_spec, env)
    config_hash = hashlib.sha256(exact.encode("utf-8")).hexdigest()
    cell_id = cell_spec.cell_id(config_hash)
    cell = args.output / cell_id
    cell.mkdir(parents=True, exist_ok=False)
    for folder in ("logs", "tmp", "role-configs"):
        (cell / folder).mkdir()
    config = {"schemaVersion": 2, "runId": args.run_id, "cellId": cell_id, "configHash": config_hash,
              "configHashInputUtf8": exact, **comparable, "environmentFile": "../../../env.json"}
    write_json(cell / "config.json", config)
    owned = OwnedProcesses(cell, store.container_id if scenario.store else None)
    clients: list[ClientControl] = []
    client_files = [f"client-{i}.json" for i in range(config["workload"]["clientCount"])]
    owners = owner_files(scenario, client_files, cell_spec.subscriber_count)
    server_files = []
    roles = []
    issues = []
    interrupted = False
    try:
        common = {"runId": args.run_id, "cellId": cell_id, "configHash": config_hash, "language": args.language,
                  "workload": config["workload"],
                  "worker": comparable["worker"], "store": store.config(config_hash) if scenario.store else None,
                  "diagnostics": lambda name: {"level": "Normal", "flowFile": str(cell / "logs" / ("message-flow-" + name.removeprefix("server-") + ".log"))}
                  if args.operation == "diagnostic" else None,
                  "provenance": {"environmentFile": str(args.output / "env.json"), "buildMode": "Release",
                                 "loadedArtifactsFile": "loaded-artifacts.json", "commit": env["commit"], "serializer": env["serializer"],
                                 "listenerReservation": "control HTTP ports are OS-reserved; Framework listeners bind wildcard ports"}}
        planned = plan_roles(cell_spec, values(scenario, args), common, language.stream_scheme, owned.reserve)
        manifests = {}
        for role in planned:
            server_files.append(role.name + ".json")
            manifest = {**role.manifest, "configFile": role.config_file}
            manifests[role.name] = manifest
            roles.append(manifest)

        def start_role(role):
            write_json(cell / role.config_file, role.config)
            owned.start(
                role.name,
                [
                    *language.command(args.perf_dir, role.executable),
                    "--config",
                    str(cell / role.config_file),
                ],
                role.ports,
            )

        peer_targets = {role.peer_target for role in planned if role.peer_target is not None}
        for target in planned:
            if target.name not in peer_targets:
                continue
            start_role(target)
            target_manifest = manifests[target.name]
            target_ready = wait_ready(
                owned,
                [target_manifest],
                False,
                cell,
                "peer-" + target.name,
                language,
                config["workload"],
            )[0]
            publish_listener_endpoints(target_manifest, target_ready)
            for dependent in planned:
                if dependent.peer_target != target.name:
                    continue
                endpoint = target_manifest["transportEndpoints"].get(dependent.peer_endpoint_key)
                if endpoint is None:
                    raise RuntimeError(
                        f"Role {target.name} did not publish peer listener "
                        f"{dependent.peer_endpoint_key}."
                    )
                dependent.config["peerEndpoint"] = endpoint

        for role in planned:
            if role.name not in peer_targets:
                start_role(role)
        manifest = {"runId": args.run_id, "cellId": cell_id, "configHash": config_hash, "language": args.language,
                    "workload": config["workload"],
                    "roles": roles, "provenance": {"environmentFile": str(args.output / "env.json"), "buildMode": "Release",
                                                  "loadedArtifactsFile": "loaded-artifacts.json",
                                                  "commit": env["commit"], "serializer": env["serializer"]}}
        readiness = wait_ready(owned, roles, False, cell, "infrastructure", language, config["workload"])
        readiness_by_role = {(item["role"], item["roleInstance"]): item for item in readiness}
        for role in roles:
            identity = (role["role"], role["roleInstance"])
            publish_listener_endpoints(role, readiness_by_role[identity])
        write_json(cell / "endpoints.json", manifest)
        for index in range(config["workload"]["clientCount"]):
            name = f"client-{index}"
            process = owned.start(name, [*language.command(args.perf_dir, CLIENT), "--endpoint-config", str(cell / "endpoints.json"),
                                        "--client-index", str(index)], [], client=True)
            client = ClientControl(process, cell / "logs" / (name + "-control.log"))
            clients.append(client)
        setup_snapshots = []
        for index, client in enumerate(clients):
            prepared = client.receive(config["workload"]["setupTimeoutMs"] / 1000, "snapshot")
            write_json(cell / "tmp" / f"client-{index}-setup.json", prepared)
            setup_snapshots.append(prepared["snapshot"])
        if scenario.driver == "clients":
            connected = sum(int(snapshot["metrics"]["connections.connected"]) for snapshot in setup_snapshots)
            requested = sum(int(snapshot["metrics"]["connections.requested"]) for snapshot in setup_snapshots)
            if requested != config["workload"]["connections"] or connected * 100 < requested * 99:
                raise InvalidSetupError(f"Global connector preparation {connected}/{requested} is below 99%; see tmp/client-*-setup.json")
        # §16.1: warmup starts after infrastructure and objects; consumersReady (probe echo, PS marker) is the measured barrier.
        wait_ready(owned, roles, "objects", cell, "probe", language, config["workload"])
        for phase, reset_seq in (("warmup", "0"), ("measured", "1")):
            if phase == "measured":
                request = {"runId": args.run_id, "cellId": cell_id, "resetSeq": reset_seq}
                role_acks = []
                reset_deadline = time.monotonic() + config["workload"]["setupTimeoutMs"] / 1000
                for role in roles:
                    ack = post_until_accepted(role["metrics"]["baseUrl"] + "/perf/reset", request, config["workload"],
                                              reset_deadline)
                    role_acks.append({"role": role["role"], "roleInstance": role["roleInstance"], "acknowledgement": ack})
                client_acks = [client.call("reset", request, config["workload"]["adminTimeoutMs"] / 1000)
                               for client in clients]
                reset_evidence = {"roles": role_acks, "clients": client_acks}
                write_json(cell / "tmp" / "reset-barrier.json", reset_evidence)
                if any(not reset_ack_matches(entry["acknowledgement"], request) for entry in role_acks) or \
                        any(not reset_ack_matches(ack, request) for ack in client_acks):
                    raise RuntimeError("resetSeq barrier did not converge")
                wait_ready(owned, roles, True, cell, "measured", language, config["workload"])
            trigger = {"runId": args.run_id, "cellId": cell_id, "resetSeq": reset_seq, "phase": phase}
            barrier = []
            role_acks = []
            trigger_deadline = time.monotonic() + config["workload"]["setupTimeoutMs"] / 1000
            for role in roles:
                sent = time.monotonic_ns()
                ack = post_until_accepted(role["applicationTriggerUrl"], trigger, config["workload"], trigger_deadline)
                entry = {"participant": f"role {role['role']}-{role['roleInstance']}", "sentTicks": str(sent),
                         "ackTicks": str(time.monotonic_ns()), "acknowledgement": ack}
                barrier.append(entry)
                role_acks.append(ack)
            for index, client in enumerate(clients):
                sent = time.monotonic_ns()
                ack = client.call("start", trigger, config["workload"]["adminTimeoutMs"] / 1000)
                barrier.append({"participant": f"client-{index}", "sentTicks": str(sent), "ackTicks": str(time.monotonic_ns()), "acknowledgement": ack})
            write_json(cell / "tmp" / (phase + "-start-barrier.json"), {"clockDomainId": f"coordinator-{os.getpid()}",
                       "clockSource": "time.monotonic_ns", "observedStartSkewBoundNs": str(int(barrier[-1]["ackTicks"]) - int(barrier[0]["sentTicks"])),
                       "exactCrossProcessStartSkewNs": None, "nullReasons": {"/exactCrossProcessStartSkewNs": null_reason(
                           "CLOCK_DOMAIN_UNVERIFIED", "Process clock epochs are not asserted to be shared.")}, "participants": barrier})
            if any(not trigger_ack_matches(ack, trigger, config_hash) for ack in role_acks):
                raise RuntimeError("Role phase trigger was rejected; see the start barrier evidence")
            duration = config["workload"]["warmupSeconds" if phase == "warmup" else "durationSeconds"]
            for client in clients:
                client.send("wait")
            # §4.1, §5.2: a phase completes when its work has drained; the drain limit is drainTimeoutMs.
            completion_deadline = time.monotonic() + duration + config["workload"]["drainTimeoutMs"] / 1000
            for client in clients:
                remaining = completion_deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(f"Client phase did not complete inside drainTimeoutMs={config['workload']['drainTimeoutMs']}")
                acknowledgement = client.receive(remaining, "response")["response"]
                if not acknowledgement.get("ok") or acknowledgement.get("phase") != "complete":
                    raise RuntimeError("Client phase did not complete successfully; collect its firstErrors evidence")
            wait_roles_complete(owned, roles, config["workload"], completion_deadline, phase)
            for role in roles:
                filename = f"server-{role['role']}-{role['roleInstance']}.json"
                snapshot = get_json(role["metrics"]["baseUrl"] + "/perf/stats?final=1",
                                    config["workload"]["adminTimeoutMs"] / 1000)
                write_json(cell / ("tmp/warmup-" + filename if phase == "warmup" else filename), snapshot)
                if phase == "warmup" and any(snapshot["metrics"][key] for key in ("errors.byKind", "errors.harness", "errors.language")):
                    raise RuntimeError("Warmup failed; " + filename)
            for index, client in enumerate(clients):
                snapshot = client.call("stats", None, config["workload"]["adminTimeoutMs"] / 1000)
                write_json(cell / (f"tmp/warmup-client-{index}.json" if phase == "warmup" else f"client-{index}.json"), snapshot)
        loaded = []
        for name, process in owned.processes:
            paths = sorted({line.split()[-1] for line in Path(f"/proc/{process.pid}/maps").read_text().splitlines()
                            if "/" in line and any(token in line for token in language.loaded_artifact_markers)})
            loaded.append({"process": name, "pid": process.pid, "artifacts": [{"actualLoadPath": path,
                           "sha256": digest(Path(path))} for path in paths if Path(path).is_file()]})
        # The role processes report the version of the libzlink they actually loaded; every role must agree.
        role_versions = {name: json.loads((cell / name).read_text())["provenance"].get("coreVersion") for name in server_files}
        for entry in loaded:
            entry["coreVersion"] = role_versions.get(entry["process"] + ".json")
        write_json(cell / "loaded-artifacts.json", loaded)
        env["coreVersion"] = agreed_core_version(role_versions, env["coreVersion"])
    except (Exception, KeyboardInterrupt) as error:
        interrupted = isinstance(error, KeyboardInterrupt)
        issues.append({"code": "PublicContractMismatch" if isinstance(error, UnsupportedCellError) else "InvalidSetup" if isinstance(error, InvalidSetupError) else "CollectionFailure",
                       "message": type(error).__name__ + ": " + str(error),
                       "sourceFile": error.source_file if isinstance(error, UnsupportedCellError) else "logs/"})
        write_json(cell / "failure.json", issues)
        for role in roles:
            filename = cell / f"server-{role['role']}-{role['roleInstance']}.json"
            if not filename.exists():
                try:
                    write_json(filename, get_json(role["metrics"]["baseUrl"] + "/perf/stats",
                                                  config["workload"]["adminTimeoutMs"] / 1000))
                except (OSError, ValueError) as collection_error:
                    issues.append({"code": "CollectionFailure", "message": str(collection_error), "sourceFile": filename.name})
        for index, client in enumerate(clients):
            filename = cell / f"client-{index}.json"
            if not filename.exists():
                try:
                    write_json(filename, client.call("stats", None, config["workload"]["adminTimeoutMs"] / 1000))
                except (OSError, ValueError, RuntimeError, TimeoutError) as collection_error:
                    issues.append({"code": "CollectionFailure", "message": str(collection_error), "sourceFile": filename.name})
    finally:
        for client in clients:
            try:
                client.close()
            except (BrokenPipeError, OSError, subprocess.TimeoutExpired) as cleanup_error:
                issues.append({"code": "CollectionFailure", "message": "Client shutdown: " + str(cleanup_error), "sourceFile": "cleanup.json"})
        owned.cleanup()
    result = aggregate(cell, config, client_files, server_files, issues, owners, scenario.aggregation)
    print(f"cell={cell_id} status={result['status']} result={cell / 'result.json'}", flush=True)
    if interrupted:
        raise KeyboardInterrupt("run interrupted after the cell result was written")
    return result


def run_exit_code(results: list[dict], store: RunStore) -> int:
    store_removed = store.removal is None or store.removal.get("removed") is True
    return 0 if store_removed and all(result["status"] == "valid" for result in results) else 1


def main(argv: list[str]) -> int:
    args = options(argv)
    if args.output.exists():
        raise FileExistsError("Refusing to overwrite an existing run root: " + str(args.output))
    args.output.mkdir(parents=True)
    executables = role_executables(args)
    env = collect(args.language, args.perf_dir, executables, require_versions=False)
    preflight(args, env)
    print("run_root=" + str(args.output), flush=True)
    build(args, executables)
    local_dotnet = args.language == "dotnet" and args.package_source == "local"
    env = collect(args.language, args.perf_dir, executables, require_versions=not local_dotnet)
    env["packageSource"] = args.package_source
    if args.package_source == "published":
        env["frameworkVersion"] = agreed_framework_version(env["frameworkVersion"], env["declaredFrameworkVersion"])
    elif local_dotnet:
        if not env["bindingVersion"]:
            raise ValueError("The .NET local package build did not report its resolved Zlink binding version")
        env["frameworkVersion"] = agreed_framework_version(
            env["frameworkVersion"] or env["declaredFrameworkVersion"], env["declaredFrameworkVersion"])
    if args.package_source == "published" and not env["packages"]:
        raise ValueError("No restored Zlink package in the role outputs; perf must reference published packages")
    write_json(args.output / "env.json", env)
    store = RunStore(args.run_id, args.output)
    results = []
    try:
        for cell in expand(args, args.operation == "matrix"):
            if cell.scenario.store:
                store.acquire()  # a Store cell runs only against the run's own Redis; failing to start it fails the run
            results.append(cell_run(args, cell, env, store))
            # A failed cell remains a failed cell. Matrix progression does not resubmit its measured operations.
    finally:
        store.release()
    (args.output / "env.json").write_text(json.dumps(env, indent=2, ensure_ascii=False) + "\n")  # now carries the observed coreVersion
    write_json(args.output / "index.json", {"schemaVersion": 2, "runId": args.run_id,
                "cells": [{"cellId": r["cellId"],
                "resultFile": r["cellId"] + "/result.json", "status": r["status"]} for r in results]})
    (args.output / "summary.txt").write_text("".join((args.output / r["cellId"] / "summary.txt").read_text() for r in results))
    return run_exit_code(results, store)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda signum, frame: (_ for _ in ()).throw(KeyboardInterrupt("coordinator terminated")))
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (OSError, ValueError, RuntimeError) as error:
        print(type(error).__name__ + ": " + str(error), file=sys.stderr)
        raise SystemExit(2)
