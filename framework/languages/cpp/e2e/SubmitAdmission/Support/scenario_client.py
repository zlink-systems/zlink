#!/usr/bin/env python3
"""HTTP-only driver for C++ Config 13 process scenarios."""

import argparse
import concurrent.futures
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid


def request_json(method, url, body=None, timeout=1.0):
    data = None if body is None else json.dumps(body).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=data,
        method=method,
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def message(marker, payload_bytes=32768):
    return {
        "operationId": f"{marker}-{uuid.uuid4().hex}",
        "sequence": 1,
        "payload": "x" * payload_bytes,
    }


def assert_submit(response, expected="Submitted"):
    if response.get("status") != expected:
        raise RuntimeError(f"expected {expected}, received {response}")
    if response.get("publicInvocationCount") != 1 or response.get("terminalCount") != 1:
        raise RuntimeError(f"submit counts are not exactly one: {response}")


class Driver:
    def __init__(self, arguments):
        self.arguments = arguments
        with open(arguments.socket_buffer_manifest, encoding="utf-8") as source:
            manifest = json.load(source)
        self.socket_buffers = manifest.get(sys.platform)
        if self.socket_buffers is None:
            raise RuntimeError(f"socket buffer manifest has no {sys.platform} entry")

    def record(self, scenario, response):
        with open(self.arguments.evidence, "a", encoding="utf-8") as output:
            output.write(json.dumps({"scenarioId": scenario, **response}, sort_keys=True) + "\n")

    def submit_node(self, target_rid, payload):
        return request_json(
            "POST",
            f"{self.arguments.caller_url}/submit/node",
            {"targetRid": target_rid, "message": payload},
            timeout=2.0,
        )

    @staticmethod
    def assert_pending(future, label):
        time.sleep(0.075)
        if future.done():
            raise RuntimeError(
                f"{label} did not remain pending until the receiver capacity changed: "
                f"{future.result()}"
            )

    def submit_channel(self, payload):
        return request_json(
            "POST", f"{self.arguments.caller_url}/submit/channel", payload
        )

    def submit_client_server(self, payload):
        return request_json(
            "POST",
            f"{self.arguments.client_server_caller_url}/submit/client-server",
            payload,
        )

    def evidence(self, base_url, operation_id, path="/evidence"):
        query = urllib.parse.urlencode({"operationId": operation_id})
        return request_json("GET", f"{base_url}{path}?{query}")

    def wait_evidence(self, base_url, operation_id, predicate, path="/evidence"):
        deadline = time.monotonic() + 3.0
        last = None
        while time.monotonic() < deadline:
            last = self.evidence(base_url, operation_id, path)
            if predicate(last):
                return last
            time.sleep(0.025)
        raise RuntimeError(f"evidence timeout for {operation_id}: {last}")

    def wait_select_one_evidence(self, operation_id):
        deadline = time.monotonic() + 3.0
        last = []
        while time.monotonic() < deadline:
            last = [
                self.evidence(url, operation_id)
                for url in self.arguments.client_server_target_url
            ]
            completed = sum(value.get("handlerCompletedCount", 0) for value in last)
            if completed == 1:
                return last
            if completed > 1:
                raise RuntimeError(
                    f"ClientServer operation reached more than one target: {last}"
                )
            time.sleep(0.025)
        raise RuntimeError(f"ClientServer evidence timeout for {operation_id}: {last}")

    def wait_stream_delivery(self, operation_id):
        return self.wait_evidence(
            self.arguments.stream_peer_url,
            operation_id,
            lambda value: value.get("receivedCount") == 1,
            path="/evidence/stream",
        )

    def assert_receiver_gate(self, status):
        if status.get("bytesReadAfterClose") != 0:
            raise RuntimeError(f"ReceiverGate crossed its read boundary: {status}")
        expected = self.socket_buffers
        if status.get("socketBufferRequestBytes") != expected["requestBytes"]:
            raise RuntimeError(f"ReceiverGate request buffer changed: {status}")
        connections = status.get("connections", [])
        if not connections:
            raise RuntimeError(f"ReceiverGate did not record a connection: {status}")
        for connection in connections:
            if (
                connection.get("frontendSend") != expected["sendBytes"]
                or connection.get("backendSend") != expected["sendBytes"]
                or connection.get("frontendReceive") != expected["receiveBytes"]
                or connection.get("backendReceive") != expected["receiveBytes"]
            ):
                raise RuntimeError(
                    f"ReceiverGate socket buffer differs from manifest: {connection}"
                )

    def exercise_session_actor_path(self, node_url, node_rid, marker):
        actor_id = f"{marker}-{uuid.uuid4().hex}"
        ensure_operation = f"{marker}-ensure-{uuid.uuid4().hex}"
        target = request_json(
            "POST",
            f"{node_url}/actors/ensure",
            {
                "operationId": ensure_operation,
                "actorId": actor_id,
                "nodeRid": "",
                "generation": 0,
            },
            timeout=4.0,
        )
        if (
            target.get("nodeRid") != node_rid
            or not isinstance(target.get("generation"), int)
            or target["generation"] <= 0
        ):
            raise RuntimeError(f"invalid Actor target generation: {target}")
        bound = request_json(
            "POST", f"{self.arguments.stream_peer_url}/actors/bind", target, timeout=4.0
        )
        if bound != target:
            raise RuntimeError(f"Actor bind changed target identity: {target} -> {bound}")
        relay_message = message(f"{marker}-relay")
        relay = request_json(
            "POST",
            f"{self.arguments.stream_peer_url}/actors/relay",
            {"actorId": actor_id, "message": relay_message},
            timeout=4.0,
        )
        assert_submit(relay)
        actor_evidence = self.wait_evidence(
            node_url,
            relay_message["operationId"],
            lambda value: value.get("handlerCompletedCount") == 1,
            path="/evidence/actor",
        )
        bound_message = message(f"{marker}-bound-session")
        bound_submit = request_json(
            "POST",
            f"{node_url}/submit/bound-session",
            {"actorId": actor_id, "message": bound_message},
            timeout=4.0,
        )
        assert_submit(bound_submit)
        bound_delivery = self.wait_stream_delivery(bound_message["operationId"])
        return {
            "target": target,
            "bound": bound,
            "relay": relay,
            "relayEvidence": actor_evidence,
            "boundSession": bound_submit,
            "boundSessionPeerEvidence": bound_delivery,
        }

    def run(self, scenario):
        if scenario == "SA-E2E-01":
            remote = message("fast-remote")
            remote_result = self.submit_node(self.arguments.target_rid, remote)
            assert_submit(remote_result)
            channel = message("fast-channel")
            channel_result = self.submit_channel(channel)
            assert_submit(channel_result)
            client_server = message("fast-client-server")
            client_server_result = self.submit_client_server(client_server)
            assert_submit(client_server_result)
            client_server_evidence = self.wait_select_one_evidence(
                client_server["operationId"]
            )
            stream = message("fast-stream")
            stream_result = request_json(
                "POST", f"{self.arguments.stream_gateway_url}/submit/stream", stream
            )
            assert_submit(stream_result)
            stream_evidence = self.wait_stream_delivery(stream["operationId"])
            reply_race = message("reply-token-race")
            reply_observed = request_json(
                "POST",
                f"{self.arguments.stream_peer_url}/request/reply-race",
                reply_race,
                timeout=4.0,
            )
            if reply_observed.get("status") != "ReplyObserved":
                raise RuntimeError(f"STREAM reply was not observed: {reply_observed}")
            reply_race_evidence = self.wait_evidence(
                self.arguments.stream_gateway_url,
                reply_race["operationId"],
                lambda value: len(value.get("terminals", [])) == 2,
                path="/evidence/stream",
            )
            terminals = reply_race_evidence["terminals"]
            if terminals.count("Submitted") != 1 or sum(
                value.startswith("Exceptional:") for value in terminals
            ) != 1:
                raise RuntimeError(
                    f"STREAM reply token did not select one winner: {reply_race_evidence}"
                )
            sequential = message("reply-token-sequential")
            sequential_reply = request_json(
                "POST",
                f"{self.arguments.stream_peer_url}/request/reply-sequential",
                sequential,
                timeout=4.0,
            )
            if sequential_reply.get("status") != "ReplyObserved":
                raise RuntimeError(f"STREAM sequential reply was not observed: {sequential_reply}")
            sequential_evidence = self.wait_evidence(
                self.arguments.stream_gateway_url,
                sequential["operationId"],
                lambda value: len(value.get("terminals", [])) == 2,
                path="/evidence/stream",
            )
            if sequential_evidence["terminals"].count("Submitted") != 1 or sum(
                value.startswith("Exceptional:")
                for value in sequential_evidence["terminals"]
            ) != 1:
                raise RuntimeError(
                    f"STREAM duplicate terminator changed the first result: {sequential_evidence}"
                )
            no_token = message("reply-token-absent")
            request_json(
                "POST",
                f"{self.arguments.stream_peer_url}/send/no-reply-token",
                no_token,
            )
            no_token_evidence = self.wait_evidence(
                self.arguments.stream_gateway_url,
                no_token["operationId"],
                lambda value: len(value.get("terminals", [])) == 1,
                path="/evidence/stream",
            )
            if not no_token_evidence["terminals"][0].startswith("Exceptional:"):
                raise RuntimeError(f"STREAM send packet exposed a reply token: {no_token_evidence}")
            local_session_actor = self.exercise_session_actor_path(
                self.arguments.stream_gateway_url,
                self.arguments.stream_gateway_rid,
                "session-actor-local",
            )
            remote_session_actor = self.exercise_session_actor_path(
                self.arguments.actor_target_url,
                self.arguments.actor_target_rid,
                "session-actor-remote",
            )
            gate_status = request_json("GET", f"{self.arguments.receiver_gate_url}/status")
            stream_gate_status = request_json(
                "GET", f"{self.arguments.stream_gate_url}/status"
            )
            self.assert_receiver_gate(gate_status)
            self.assert_receiver_gate(stream_gate_status)
            self.record(
                scenario,
                {
                    "node": remote_result,
                    "channel": channel_result,
                    "clientServer": client_server_result,
                    "clientServerTargetEvidence": client_server_evidence,
                    "stream": stream_result,
                    "streamPeerEvidence": stream_evidence,
                    "replyTokenRaceFixture": reply_race_evidence,
                    "replyTokenSequentialFixture": sequential_evidence,
                    "replyTokenAbsentFixture": no_token_evidence,
                    "sessionActorLocalFastPath": local_session_actor,
                    "sessionActorRemoteFastPath": remote_session_actor,
                    "receiverGate": gate_status,
                    "streamReceiverGate": stream_gate_status,
                },
            )
        elif scenario == "SA-E2E-05":
            missing = message("missing-target")
            result = self.submit_node("submit-admission-missing", missing)
            assert_submit(result, "TargetNotFound")
            self.record(scenario, {"missing": result})
        elif scenario == "SA-E2E-08":
            local = message("local-node")
            remote = message("remote-node")
            local_result = self.submit_node(self.arguments.caller_rid, local)
            remote_result = self.submit_node(self.arguments.target_rid, remote)
            assert_submit(local_result)
            assert_submit(remote_result)
            local_evidence = self.wait_evidence(
                self.arguments.caller_url,
                local["operationId"],
                lambda value: value.get("handlerCompletedCount") == 1,
            )
            remote_evidence = self.wait_evidence(
                self.arguments.target_url,
                remote["operationId"],
                lambda value: value.get("handlerCompletedCount") == 1,
            )

            object_topology_before = request_json(
                "GET",
                f"{self.arguments.caller_url}/ready?"
                + urllib.parse.urlencode(
                    {"targetRid": self.arguments.object_client_rid}
                ),
            )
            object_client = message("object-client-node")
            object_client_result = self.submit_node(
                self.arguments.object_client_rid, object_client
            )
            assert_submit(object_client_result, "TargetNotFound")
            object_topology_after = request_json(
                "GET",
                f"{self.arguments.caller_url}/ready?"
                + urllib.parse.urlencode(
                    {"targetRid": self.arguments.object_client_rid}
                ),
            )
            if (
                object_topology_before.get("ready") is not True
                or object_topology_after.get("ready") is not True
                or object_topology_before.get("peerCount")
                != object_topology_after.get("peerCount")
                or object_topology_before.get("generation")
                != object_topology_after.get("generation")
            ):
                raise RuntimeError(
                    "Object Client Node direct changed the existing Ready topology: "
                    f"{object_topology_before} -> {object_topology_after}"
                )

            request_json("POST", f"{self.arguments.caller_url}/gate/close")
            local_delayed_started = time.monotonic_ns()
            local_fills = []
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
                local_delayed = None
                local_delayed_future = None
                for index in range(1, 65):
                    candidate = message(f"local-delayed-{index}")
                    candidate_future = executor.submit(
                        self.submit_node, self.arguments.caller_rid, candidate
                    )
                    time.sleep(0.075)
                    if not candidate_future.done():
                        local_delayed = candidate
                        local_delayed_future = candidate_future
                        break
                    assert_submit(candidate_future.result())
                    local_fills.append(candidate)
                if local_delayed_future is None:
                    request_json(
                        "POST", f"{self.arguments.caller_url}/gate/open"
                    )
                    raise RuntimeError(
                        "local receiver gate did not exhaust the configured HWM "
                        "within 64 submissions"
                    )
                request_json("POST", f"{self.arguments.caller_url}/gate/open")
                local_delayed_result = local_delayed_future.result(timeout=2.0)
            local_delayed_completed = time.monotonic_ns()
            assert_submit(local_delayed_result)
            local_delayed_evidence = None
            for payload in (*local_fills, local_delayed):
                observed = self.wait_evidence(
                    self.arguments.caller_url,
                    payload["operationId"],
                    lambda value: value.get("handlerCompletedCount") == 1,
                )
                if payload is local_delayed:
                    local_delayed_evidence = observed

            request_json("POST", f"{self.arguments.caller_url}/gate/close")
            local_timeout_fills = [
                message(f"local-timeout-fill-{index}")
                for index in range(1, len(local_fills) + 1)
            ]
            local_timeout = message("local-timeout")
            for payload in local_timeout_fills:
                assert_submit(
                    self.submit_node(self.arguments.caller_rid, payload)
                )
            local_timeout_started = time.monotonic_ns()
            local_timeout_result = self.submit_node(
                self.arguments.caller_rid, local_timeout
            )
            local_timeout_completed = time.monotonic_ns()
            assert_submit(local_timeout_result, "DeadlineExceeded")
            local_timeout_before_release = self.evidence(
                self.arguments.caller_url, local_timeout["operationId"]
            )
            if (
                local_timeout_before_release.get("handlerEnteredCount") != 0
                or local_timeout_before_release.get("handlerCompletedCount") != 0
            ):
                raise RuntimeError(
                    "local timed-out operation entered the application handler: "
                    f"{local_timeout_before_release}"
            )
            request_json("POST", f"{self.arguments.caller_url}/gate/open")
            for payload in local_timeout_fills:
                self.wait_evidence(
                    self.arguments.caller_url,
                    payload["operationId"],
                    lambda value: value.get("handlerCompletedCount") == 1,
                )
            time.sleep(0.05)
            local_timeout_after_release = self.evidence(
                self.arguments.caller_url, local_timeout["operationId"]
            )
            if (
                local_timeout_after_release.get("handlerEnteredCount") != 0
                or local_timeout_after_release.get("handlerCompletedCount") != 0
            ):
                raise RuntimeError(
                    "local timed-out operation was admitted after capacity returned: "
                    f"{local_timeout_after_release}"
                )

            request_json("POST", f"{self.arguments.receiver_gate_url}/close")
            remote_delayed_started = time.monotonic_ns()
            remote_fills = []
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
                remote_delayed = None
                remote_delayed_future = None
                for index in range(1, 65):
                    candidate = message(f"remote-delayed-{index}")
                    candidate_future = executor.submit(
                        self.submit_node, self.arguments.target_rid, candidate
                    )
                    time.sleep(0.075)
                    if not candidate_future.done():
                        remote_delayed = candidate
                        remote_delayed_future = candidate_future
                        break
                    assert_submit(candidate_future.result())
                    remote_fills.append(candidate)
                if remote_delayed_future is None:
                    request_json(
                        "POST", f"{self.arguments.receiver_gate_url}/open"
                    )
                    raise RuntimeError(
                        "remote receiver gate did not exhaust the configured HWM "
                        "within 64 submissions"
                    )
                remote_gate_before_release = request_json(
                    "GET", f"{self.arguments.receiver_gate_url}/status"
                )
                request_json("POST", f"{self.arguments.receiver_gate_url}/open")
                remote_delayed_result = remote_delayed_future.result(timeout=2.0)
            remote_delayed_completed = time.monotonic_ns()
            assert_submit(remote_delayed_result)
            remote_delayed_evidence = None
            for payload in (*remote_fills, remote_delayed):
                observed = self.wait_evidence(
                    self.arguments.target_url,
                    payload["operationId"],
                    lambda value: value.get("handlerCompletedCount") == 1,
                )
                if payload is remote_delayed:
                    remote_delayed_evidence = observed

            request_json("POST", f"{self.arguments.receiver_gate_url}/close")
            remote_timeout_fills = [
                message(f"remote-timeout-fill-{index}")
                for index in range(1, len(remote_fills) + 1)
            ]
            remote_timeout = message("remote-timeout")
            for payload in remote_timeout_fills:
                assert_submit(self.submit_node(self.arguments.target_rid, payload))
            remote_timeout_started = time.monotonic_ns()
            remote_timeout_result = self.submit_node(
                self.arguments.target_rid, remote_timeout
            )
            remote_timeout_completed = time.monotonic_ns()
            assert_submit(remote_timeout_result, "DeadlineExceeded")
            remote_timeout_before_release = self.evidence(
                self.arguments.target_url, remote_timeout["operationId"]
            )
            request_json("POST", f"{self.arguments.receiver_gate_url}/open")
            for payload in remote_timeout_fills:
                self.wait_evidence(
                    self.arguments.target_url,
                    payload["operationId"],
                    lambda value: value.get("handlerCompletedCount") == 1,
                )
            time.sleep(0.05)
            remote_timeout_after_release = self.evidence(
                self.arguments.target_url, remote_timeout["operationId"]
            )
            if (
                remote_timeout_before_release.get("handlerEnteredCount") != 0
                or remote_timeout_before_release.get("handlerCompletedCount") != 0
                or remote_timeout_after_release.get("handlerEnteredCount") != 0
                or remote_timeout_after_release.get("handlerCompletedCount") != 0
            ):
                raise RuntimeError(
                    "remote timed-out operation reached the handler: "
                    f"{remote_timeout_before_release} -> "
                    f"{remote_timeout_after_release}"
                )
            self.record(
                scenario,
                {
                    "immediate": {
                        "local": local_result,
                        "remote": remote_result,
                        "localEvidence": local_evidence,
                        "remoteEvidence": remote_evidence,
                    },
                    "delayed": {
                        "local": local_delayed_result,
                        "remote": remote_delayed_result,
                        "localEvidence": local_delayed_evidence,
                        "remoteEvidence": remote_delayed_evidence,
                        "localElapsedMs": (
                            local_delayed_completed - local_delayed_started
                        )
                        / 1_000_000,
                        "remoteElapsedMs": (
                            remote_delayed_completed - remote_delayed_started
                        )
                        / 1_000_000,
                        "localQueueFillCount": len(local_fills),
                        "remoteQueueFillCount": len(remote_fills),
                        "remoteGateBeforeRelease": remote_gate_before_release,
                    },
                    "deadline": {
                        "local": local_timeout_result,
                        "remote": remote_timeout_result,
                        "localElapsedMs": (
                            local_timeout_completed - local_timeout_started
                        )
                        / 1_000_000,
                        "remoteElapsedMs": (
                            remote_timeout_completed - remote_timeout_started
                        )
                        / 1_000_000,
                        "localQueueFillCount": len(local_timeout_fills),
                        "remoteQueueFillCount": len(remote_timeout_fills),
                        "localBeforeRelease": local_timeout_before_release,
                        "localAfterRelease": local_timeout_after_release,
                        "remoteBeforeRelease": remote_timeout_before_release,
                        "remoteAfterRelease": remote_timeout_after_release,
                    },
                    "objectClient": {
                        "submit": object_client_result,
                        "topologyBefore": object_topology_before,
                        "topologyAfter": object_topology_after,
                    },
                },
            )
        elif scenario == "SA-E2E-09":
            payload = message("channel-name")
            result = self.submit_channel(payload)
            assert_submit(result)
            target_evidence = self.wait_evidence(
                self.arguments.target_url,
                payload["operationId"],
                lambda value: value.get("handlerCompletedCount") == 1,
            )
            self.record(scenario, {"submit": result, "targetEvidence": target_evidence})
        elif scenario == "SA-E2E-14":
            payload = message("fanout-zero")
            result = request_json(
                "POST", f"{self.arguments.publisher_url}/submit/fanout", payload
            )
            assert_submit(result)
            self.record(
                scenario,
                {"submit": result, "subscriberSnapshotCount": 0, "lateDeliveryCount": 0},
            )
        elif scenario == "SA-E2E-20":
            request_json("POST", f"{self.arguments.target_url}/gate/close")
            payload = message("handler-gate")
            result = self.submit_node(self.arguments.target_rid, payload)
            assert_submit(result)
            before = self.wait_evidence(
                self.arguments.target_url,
                payload["operationId"],
                lambda value: value.get("handlerEnteredCount") == 1,
            )
            if before.get("handlerCompletedCount") != 0:
                raise RuntimeError(f"handler completed before gate release: {before}")
            request_json("POST", f"{self.arguments.target_url}/gate/open")
            after = self.wait_evidence(
                self.arguments.target_url,
                payload["operationId"],
                lambda value: value.get("handlerCompletedCount") == 1,
            )
            self.record(scenario, {"submit": result, "before": before, "after": after})
        else:
            raise RuntimeError(
                f"{scenario} is not implemented by the C++ Config 13 runner; "
                "see feature-map.ko.md"
            )
        print(f"{scenario} PASS")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--caller-url", required=True)
    parser.add_argument("--target-url", required=True)
    parser.add_argument("--publisher-url", required=True)
    parser.add_argument("--caller-rid", required=True)
    parser.add_argument("--target-rid", required=True)
    parser.add_argument("--object-client-rid", required=True)
    parser.add_argument("--client-server-caller-url", required=True)
    parser.add_argument(
        "--client-server-target-url", action="append", required=True
    )
    parser.add_argument("--receiver-gate-url", required=True)
    parser.add_argument("--stream-gateway-url", required=True)
    parser.add_argument("--stream-peer-url", required=True)
    parser.add_argument("--stream-gate-url", required=True)
    parser.add_argument("--actor-target-url", required=True)
    parser.add_argument("--stream-gateway-rid", required=True)
    parser.add_argument("--actor-target-rid", required=True)
    parser.add_argument("--collector-url", required=True)
    parser.add_argument("--socket-buffer-manifest", required=True)
    parser.add_argument("--evidence", required=True)
    parser.add_argument("scenarios", nargs="+")
    arguments = parser.parse_args()
    driver = Driver(arguments)
    for scenario in arguments.scenarios:
        driver.run(scenario)


if __name__ == "__main__":
    main()
