# SPDX-License-Identifier: Elastic-2.0
"""Drive the actors through the shared file protocol and check provider/API evidence."""

import json
import os
from pathlib import Path
import select
import signal
import sys
import time
from urllib.parse import unquote


def expect(actual, expected, context):
    if actual != expected:
        raise ValueError(f"{context}: expected {expected!r}, got {actual!r}")


class Host:
    def __init__(self, root, run_id, role, control):
        self.prefix = root / f"{run_id}.{role}"
        self.events = Path(f"{self.prefix}.events")
        self.command = control / f"{run_id}.{role}.command"
        self.run_id = run_id
        self.sequence = 0
        self.role = role

    def read(self):
        # A host can be appending the next line while this file is read.
        paths = (self.events, Path(f"{self.prefix}.wire"))
        lines = [line for path in paths if path.exists()
                 for line in path.read_text(encoding="utf-8").split("\n")[:-1]]
        prefix = f"actor-destroy|run={self.run_id}|"
        return [{key: unquote(value) for key, value in
                 (field.split("=", 1) for field in line[len(prefix):].split("|"))}
                for line in lines if line.startswith(prefix)]

    def wait(self, name, *, before=None, **fields):
        limit = time.monotonic() + 90
        while time.monotonic() < limit:
            events = self.read()
            for event in events:
                if event["event"] == "error":
                    raise ValueError(f"{self.role}: {event['message']}")
            for event in events:
                if event["event"] == name and all(event.get(k) == v for k, v in fields.items()):
                    return event
            if before is not None:
                host, terminal, match = before
                for row in events if host is self else host.read():
                    if row["event"] == "error":
                        raise ValueError(f"{host.role}: {row['message']}")
                    if row["event"] == terminal and all(row.get(k) == v for k, v in match.items()):
                        details = " ".join(f"{key}={value.splitlines()[0]}"
                                           for key, value in row.items() if key != "event" and value)
                        raise ValueError(f"{host.role}: {terminal} before {name}: {details}")
            time.sleep(0.02)
        raise ValueError(f"{self.role}: timed out waiting for {name} {fields}")

    def send(self, command, *args):
        self.sequence += 1
        temporary = self.command.with_suffix(".next")
        temporary.write_text("\t".join((self.run_id, str(self.sequence), command, *args)) + "\n",
                             encoding="utf-8")
        temporary.replace(self.command)
        self.wait("ack", seq=str(self.sequence))

    def result(self, command, event, label, **fields):
        self.send(command, label)
        value = self.wait(event, label=label)
        for key, expected in fields.items():
            expect(value.get(key), expected, f"{self.role} {label} {key}")
        return value

    def count(self, event, expected, *, after=0, **fields):
        actual = sum(row["event"] == event and int(row["timeUs"]) > after
                     and all(row.get(k) == v for k, v in fields.items())
                     for row in self.read())
        expect(actual, expected, f"{self.role} count {event}")


def snapshot(host, label):
    return {key: value for key, value in host.result("authority", "authority", label).items()
            if key not in ("event", "timeUs", "label")}


def order(fixture, *hosts):
    # The probe events identify the request by label in all four hosts.
    probe_events = {
        ("probeReturned", "pending-cas"): "pendingCasProbeReturned",
        ("probeHandled", "retry"): "retryProbeHandled",
        ("probeReturned", "post-commit"): "postCommitProbeReturned",
    }
    observed = {}
    for host in hosts:
        for row in host.read():
            name = probe_events.get((row["event"], row.get("label")), row["event"])
            timestamp = int(row["timeUs"])
            observed[name] = min(observed.get(name, timestamp), timestamp)
    for earlier, later in fixture["requiredOrder"]:
        left, right = observed.get(earlier), observed.get(later)
        if left is None or right is None or left >= right:
            raise ValueError(f"requiredOrder: {earlier} must precede {later}")


def verify_ready(row, owner, run_id, fixture):
    expect(row["read"], "Found", "Ready authority")
    expect(row["owner"], f"actor-destroy-{owner}", "Ready owner")
    expect(row["actorId"], run_id, "Ready ActorId")
    expect(row["actorType"], "cross-lang-actor-destroy-type", "Ready Actor type")
    expect(row["spotKind"], "entry", "Ready Spot kind")
    if not row["spot"]:
        raise ValueError("Ready Actor has no Entry Spot")
    expected = fixture["wire"]["actorAuthorityPayload"]["Ready"]
    expect(row["state"], str(expected["object.state"]), "Ready state")
    expect(row["operationKind"], str(expected["operationKind"]), "Ready operation")


def verify_closing(row, ready, fixture):
    state = fixture["wire"]["actorAuthorityPayload"]["Closing"]
    expected = dict(ready, state=str(state["object.state"]), operationKind=str(state["operationKind"]))
    if row.get("version") == ready["version"]:
        raise ValueError("Closing must publish a new StoreVersion")
    expect(row, dict(expected, version=row.get("version")), "Closing authority")


def verify_new(caller, ready, fixture, expected_kind):
    result = caller.wait("getOrCreateReturned")
    expect(result["kind"], expected_kind, "GetOrCreate result")
    generation = result["generation"]
    if int(generation) <= int(ready["generation"]):
        raise ValueError(f"recreation must advance object generation: {ready['generation']} -> {generation}")
    replacement = snapshot(caller, "replacement")
    verify_ready(replacement, "caller", caller.run_id, fixture)
    expect(replacement["generation"], generation, "new authority generation")
    caller.result("find", "found", "replacement", generation=generation)
    caller.count("newFactoryStarted", 1)
    return replacement


def wire(host, ready, run_id, sent, *, after=0):
    all_rows = host.read()
    rows = [row for row in all_rows if int(row["timeUs"]) > after]
    expect(sum(row["event"] == "wireObserved" for row in all_rows), 1,
           f"{run_id} {host.role} transport observation")
    commands = [row for row in rows if row["event"] == "command27Sent"]
    expect(len(commands), sent, "command 27 sends")
    expect(sum(row["event"] == "command27Reply" for row in rows), 0, "command 27 replies")
    for command in commands:
        for key in ("generation", "owner", "nodeGeneration", "ownerGeneration", "leaseGeneration"):
            expect(command[key], ready[key], f"command 27 {key}")
        expect(command["actorId"], run_id, "command 27 actor ID")
        expect(command["flags"], "0", "command 27 flags")
        expect(command["transportKind"], "0", "command 27 transport kind")
        expect(command["payloadFrames"], "0", "command 27 payload frames")
        if int(command["correlation"]) == 0:
            raise ValueError("command 27 correlation is zero")


def verify_release(caller, closing, expected, terminal, **terminal_fields):
    release = caller.wait("releaseCommitted", before=(caller, terminal, terminal_fields))
    for key, expected in dict(version=closing["version"], invalidLease="true",
                              descriptorCondition="true", capacityCondition="true",
                              membershipRemoved=str(expected["oldMembershipRemoved"]).lower(),
                              capacityDelta=str(expected["oldActiveActorCapacityDelta"]),
                              relocationPolicyCondition=str(expected["requiresRelocationPolicy"]).lower()).items():
        expect(release.get(key), expected, f"release batch {key}")
    caller.count("releaseCommitted", 1)


def verify_held_admission(caller, started, returned, owner_rid):
    events = caller.read()
    closed = {row["connection"]: int(row["timeUs"]) for row in events
              if row["event"] == "admissionDisconnected"}
    if not any(int(row["timeUs"]) <= started and closed.get(row["connection"], returned + 1) > returned
               for row in events if row["event"] == "admissionHeld"
               and row["sender"] == owner_rid and row["action"] == "hold"
               and row["reason"] == "admissionCheck"):
        raise ValueError("no connected route held A's ADMIT through the Destroy call")


def stop_owner(owner):
    pid = int(owner.wait("hostReady")["pid"])
    if pid <= 1:
        raise ValueError(f"invalid owner PID {pid}")
    # Keep the process identity and confirm exit before sampling its still-live lease.
    with os.fdopen(os.pidfd_open(pid), "rb") as process:
        signal.pidfd_send_signal(process.fileno(), signal.SIGKILL)
        waited = select.poll()
        waited.register(process, select.POLLIN)
        if not waited.poll(5000):
            raise ValueError(f"owner process {pid} did not exit")


def run(fixture, root, run_id, scenario, control, caller_language):
    owner = Host(root, run_id, "owner", control)
    caller = Host(root, run_id, "caller", control)
    case = dict(zip("abcdef", fixture["scenarios"]))
    case["admissionCheck"] = fixture["admissionCheck"]
    steps = [step["expect"] for step in case[scenario]["steps"]]
    owner.send("placement", "100")
    caller.send("placement", "0")
    owner.send("peer-placement", "0")
    owner.send("create")
    created = owner.wait("created")
    ready = snapshot(owner, "initial")
    verify_ready(ready, "owner", run_id, fixture)
    expect(created["generation"], ready["generation"], "initial generation")
    caller.result("find", "found", "initial", generation=ready["generation"])

    if scenario == "admissionCheck":
        caller.wait("admissionHeld")
        writes = [sum(row["event"] == "authorityWritten" for row in host.read())
                  for host in (owner, caller)]
        started = time.time_ns() // 1000
        result = caller.result("destroy", "destroyReturned", "admission",
                               kind=steps[0]["kind"])
        verify_held_admission(caller, started, int(result["timeUs"]), ready["owner"])
        expect(snapshot(caller, "after-admission-destroy"), ready, "admission authority")
        if caller_language == "node":
            # The Node host observes the binding call as well as transmitted frames.
            caller.count("submissionObserverReady", 1)
            caller.count("command27Submitted", steps[0]["command27Submissions"])
        for host, before in zip((owner, caller), writes):
            host.count("authorityWritten", before)
        caller.send("allow-admission")
        caller.wait("admissionAllowed")
        caller.send("peer")
        # An immediate return is compared with the same API deadline used by the host.
        if int(result["timeUs"]) >= int(caller.wait("destroyDeadline")["timeUs"]):
            raise ValueError("admission rejection reached the request deadline")
    elif scenario == "e":
        owner.send("hold-cas")
        owner.send("destroy", "fail")
        owner.wait("closingCasHeld", before=(owner, "failedDestroyReturned", {"label": "fail"}))
        expect(snapshot(owner, "cas-held"), ready, "held CAS authority")
        owner.count("closingCommitted", steps[0]["closingCommitsByA"])
        owner.result("probe", "probeReturned", "pending-cas", kind=steps[1]["kind"])
        owner.count("probeHandled", steps[1]["probeHandlerCallsForRequest"], label="pending-cas")
        owner.send("fail-cas")
        failed = owner.wait("failedDestroyReturned", label="fail")
        expect(failed["kind"], steps[2]["kind"], "failed Destroy result")
        expect(failed.get("reason"), steps[2]["errorSource"], "Closing CAS failure source")
        expect(snapshot(owner, "cas-failed"), ready, "failed CAS authority")
        owner.count("closingCommitted", steps[2]["closingCommitsByA"])
        retry_probe = owner.result("probe", "probeReturned", "retry")
        expect(retry_probe["kind"] == steps[3]["kind"], steps[1]["retryable"],
               "probe retry after Closing CAS failure")
        expect(retry_probe["generation"], ready["generation"], "retry probe generation")
        owner.count("probeHandled", steps[3]["probeHandlerCallsForRequest"], label="retry")
        owner.send("hold-delete")
        owner.send("destroy", "retry")
        owner.wait("ownerDeleteHeld", before=(owner, "destroyReturned", {"label": "retry"}))
        verify_closing(snapshot(owner, "cas-retry"), ready, fixture)
        owner.result("probe", "probeReturned", "post-commit", kind=steps[5]["kind"])
        owner.count("probeHandled", 0, label="post-commit")
        owner.send("resume-delete")
        retry = owner.wait("destroyReturned", label="retry")
        expect(retry["kind"], str(steps[6]["value"]).lower(), "retry Destroy result")
        expect(snapshot(owner, "deleted"), {"read": "Missing"}, "retry Delete")
        owner.count("closingCommitted", steps[4]["closingCommitsByA"])
        owner.count("ownerDeleteCommitted", steps[6]["ownerDeleteCommitsByA"])
    else:
        caller.send("peer")
        owner.send("peer")
        if scenario in ("a", "d"):
            owner.send("placement", "0")
            caller.send("placement", "100")
            caller.send("peer-placement", "0")
        owner.send("hold-delete")
        caller.send("destroy", "remote")
        owner.wait("ownerDeleteHeld", before=(caller, "destroyReturned", {"label": "remote"}))
        closing = snapshot(caller, "closing")
        verify_closing(closing, ready, fixture)
        owner.count("closingCommitted", 1)
        if scenario in ("a", "b"):
            caller.result("lease", "lease", "closing", kind=case[scenario]["given"]["ownerLease"])
        if scenario == "a":
            caller.result("find", "found", "closing", generation="none")
            caller.send("get-or-create")
            caller.wait("createObservedClosing", before=(caller, "getOrCreateReturned", {}))
            caller.send("pending-create")
            expect(caller.wait("createPending")["value"], "true", "GetOrCreate while Closing")
            caller.count("newFactoryStarted", 0)
        if scenario == "b":
            owner.send("destroy", "duplicate")
            owner.result("pending-destroy", "ownerDuplicatePending", "duplicate", value="true")
        if scenario in ("a", "b"):
            caller.result("pending-destroy", "destroyPending", "remote", value="true")
            owner.send("resume-delete")
            remote = caller.wait("destroyReturned", label="remote")
            returned = steps[4 if scenario == "a" else 1]
            key = "destroy" if scenario == "a" else "remoteDestroy"
            expect(remote["kind"], str(returned["value"][key]).lower(), "remote Destroy result")
            owner.count("ownerDeleteCommitted", returned["ownerDeleteCommitsByA"])
            caller.count("releaseCommitted", 0)
        if scenario == "a":
            replacement = verify_new(caller, ready, fixture, steps[4]["value"]["getOrCreate"]["kind"])
            order(case["a"], owner, caller)
            wire(caller, ready, run_id, steps[4]["command27Sends"])
            print(f"{run_id}: PASS", flush=True)
            try:
                stale_begin = verify_stale(owner, caller, replacement, case["c"])
            except (ValueError, OSError, KeyError) as error:
                raise ValueError(f"c: {error}") from error
        if scenario == "b":
            duplicate = owner.wait("ownerDuplicateReturned", label="duplicate")
            expect(duplicate["kind"], str(steps[1]["value"]["ownerDuplicateDestroy"]).lower(),
                   "duplicate Destroy result")
            expect(snapshot(caller, "deleted"), {"read": "Missing"}, "duplicate Delete")
            owner.count("cleanup", steps[1]["cleanupCallsByA"])
        if scenario == "d":
            remote = caller.wait("destroyReturned", label="remote")
            expect(remote["kind"], steps[0]["kind"], "live-owner Destroy result")
            caller.result("lease", "lease", "before-exit", kind="valid")
            stop_owner(owner)
            caller.result("lease", "lease", "after-exit", kind="valid")
            expect(snapshot(caller, "after-exit"), closing, "authority after exit")
            caller.send("wait-lease-expired")
            caller.wait("ownerLeaseExpired")
            caller.result("find", "found", "expired", generation="none")
            expect(snapshot(caller, "expired"), closing, "Find preserves expired authority")
            caller.count("releaseCommitted", 0)
            caller.send("get-or-create")
            verify_release(caller, closing, steps[4]["releaseBatch"], "getOrCreateReturned")
            verify_new(caller, ready, fixture, steps[5]["value"]["getOrCreate"]["kind"])
            owner.count("ownerDeleteCommitted", 0)
        if scenario == "f":
            caller.wait("callerObservedClosingWithValidLease")
            caller.result("pending-destroy", "destroyPending", "remote", value="true")
            caller.count("releaseCommitted", 0)
            owner.send("end-lease")
            verify_release(caller, closing, steps[1]["releaseBatch"],
                           "destroyReturned", label="remote")
            owner.count("ownerLeaseEnded", 1)
            caller.count("ownerLeaseEnded", 0)
            remote = caller.wait("destroyReturned", label="remote")
            expect(remote["kind"], str(steps[2]["value"]).lower(), "ended-owner Destroy result")
            expect(snapshot(caller, "released"), {"read": "Missing"}, "release removes authority")
            owner.count("ownerDeleteCommitted", 0)

    if scenario != "d":
        owner.send("finish")
    caller.send("finish")
    if scenario == "a":
        try:
            for host, step in zip((caller, owner), case["c"]["steps"][1:3]):
                wire(host, replacement, run_id, step["expect"]["command27Sends"],
                     after=stale_begin)
        except (ValueError, OSError, KeyError) as error:
            raise ValueError(f"c: {error}") from error
    else:
        order(case[scenario], owner, caller)
    if scenario not in ("a", "e"):
        wire(caller, ready, run_id, steps[0].get("command27Sends", 1))
        if scenario == "b":
            owner.count("command27Sent", 0)
    print(f"{run_id}{' c' if scenario == 'a' else ''}: PASS", flush=True)


def verify_stale(owner, caller, replacement, case):
    owner.result("find", "found", "replacement", generation=replacement["generation"])
    owner.send("peer")
    started = time.time_ns() // 1000
    for host, step in zip((caller, owner), case["steps"][1:3]):
        host.result("destroy", "destroyReturned", "stale", kind=step["expect"]["kind"])
    caller.result("find", "found", "after-stale", generation=replacement["generation"])
    caller.result("probe", "probeReturned", "after-stale", kind="return",
                  generation=replacement["generation"])
    caller.count("probeHandled", 1, label="after-stale")
    expect(snapshot(caller, "after-stale"), replacement, "stale Destroy authority")
    for host, step in zip((caller, owner), case["steps"][1:3]):
        host.count("authorityWritten", step["expect"]["authorityWrites"], after=started)
    return started


if __name__ == "__main__":
    fixture_path, root, run_id, scenario, control, caller_language = sys.argv[1:]
    try:
        run(json.loads(Path(fixture_path).read_text(encoding="utf-8")), Path(root),
            run_id, scenario, Path(control), caller_language)
    except ValueError as error:
        print(f"{run_id}: {error}", file=sys.stderr)
        sys.exit(2)
    except (OSError, KeyError) as error:
        sys.exit(f"{run_id}: {error}")
