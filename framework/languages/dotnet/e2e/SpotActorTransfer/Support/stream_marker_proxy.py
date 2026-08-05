#!/usr/bin/env python3
"""TCP proxy that can hold a stream at an application-supplied byte marker."""

from __future__ import annotations

import argparse
import json
import socket
import socketserver
import threading
import time
import urllib.parse
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


@dataclass
class Gate:
    marker: bytes
    after_gate_id: str | None = None
    target_flow: "Flow | None" = None
    captured_count: int = 0
    released_count: int = 0
    released: bool = False


class Flow:
    PARTIAL_MARKER_GRACE_SECONDS = 0.05

    def __init__(
        self,
        owner: "GateState",
        destination: socket.socket,
    ) -> None:
        self.owner = owner
        self.destination = destination
        self.pending = bytearray()
        self.offsets: dict[str, int] = {}
        self.closed = False
        self.partial_generation = 0
        self.peer: "Flow | None" = None

    def accept(self, data: bytes) -> None:
        with self.owner.condition:
            self.partial_generation += 1
            self.pending.extend(data)
            self._capture_markers()
            outbound = self._take_forwardable()
            if outbound:
                self.destination.sendall(outbound)
            self._schedule_partial_flush()

    def release(self) -> None:
        with self.owner.condition:
            outbound = self._take_forwardable()
            if outbound:
                self.destination.sendall(outbound)

    def close(self) -> None:
        with self.owner.condition:
            self.partial_generation += 1
            if not self._held_offsets() and self.pending:
                self.destination.sendall(self.pending)
                self.pending.clear()
            self.closed = True
            self.owner.flows.discard(self)
            self.owner.condition.notify_all()

    def _capture_markers(self) -> None:
        for gate_id, gate in self.owner.gates.items():
            if (
                gate.after_gate_id is not None
                and gate.target_flow is self
                and gate_id not in self.offsets
                and self.pending
            ):
                self.offsets[gate_id] = 0
                gate.captured_count += 1
                self.owner.condition.notify_all()
                continue
            if (
                gate.released
                or gate.after_gate_id is not None
                or gate_id in self.offsets
            ):
                continue
            offset = self.pending.find(gate.marker)
            if offset < 0:
                continue
            self.offsets[gate_id] = offset
            gate.captured_count += 1
            self.owner.condition.notify_all()

    def _take_forwardable(self) -> bytes:
        self._capture_markers()
        held_offsets = self._held_offsets()
        if held_offsets:
            count = min(held_offsets)
        else:
            uncaptured = [
                gate.marker
                for gate_id, gate in self.owner.gates.items()
                if (
                    not gate.released
                    and gate.after_gate_id is None
                    and gate_id not in self.offsets
                )
            ]
            retained = self._partial_marker_suffix(uncaptured)
            count = max(0, len(self.pending) - retained)

        if count == 0:
            return b""
        outbound = bytes(self.pending[:count])
        del self.pending[:count]
        for gate_id, offset in list(self.offsets.items()):
            gate = self.owner.gates[gate_id]
            if gate.released and offset < count:
                gate.released_count += 1
                del self.offsets[gate_id]
                self.owner.condition.notify_all()
            else:
                self.offsets[gate_id] = offset - count
        return outbound

    def _held_offsets(self) -> list[int]:
        return [
            offset
            for gate_id, offset in self.offsets.items()
            if not self.owner.gates[gate_id].released
        ]

    def _schedule_partial_flush(self) -> None:
        if self.closed or self._held_offsets() or not self.pending:
            return
        generation = self.partial_generation
        timer = threading.Timer(
            self.PARTIAL_MARKER_GRACE_SECONDS,
            self._flush_unmatched_partial,
            args=(generation,),
        )
        timer.daemon = True
        timer.start()

    def _flush_unmatched_partial(self, generation: int) -> None:
        with self.owner.condition:
            if (
                self.closed
                or generation != self.partial_generation
                or self._held_offsets()
                or not self.pending
            ):
                return
            outbound = bytes(self.pending)
            self.pending.clear()
            self.destination.sendall(outbound)

    def _partial_marker_suffix(self, markers: list[bytes]) -> int:
        retained = 0
        for marker in markers:
            limit = min(len(self.pending), len(marker) - 1)
            for size in range(limit, retained, -1):
                if self.pending[-size:] == marker[:size]:
                    retained = size
                    break
        return retained


class GateState:
    def __init__(self) -> None:
        self.condition = threading.Condition()
        self.gates: dict[str, Gate] = {}
        self.flows: set[Flow] = set()

    def arm(
        self,
        gate_id: str,
        marker: str,
        after_gate_id: str | None,
    ) -> dict[str, object]:
        encoded = marker.encode("utf-8")
        if not gate_id or (not encoded and not after_gate_id):
            raise ValueError(
                "gateId and either marker or afterGateId are required")
        with self.condition:
            if gate_id in self.gates:
                raise ValueError(f"gate '{gate_id}' is already armed")
            if after_gate_id and after_gate_id not in self.gates:
                raise ValueError(
                    f"parent gate '{after_gate_id}' is not armed")
            self.gates[gate_id] = Gate(
                encoded,
                after_gate_id=after_gate_id,
            )
            return self.snapshot(gate_id)

    def release(self, gate_id: str) -> dict[str, object]:
        with self.condition:
            gate = self._gate(gate_id)
            gate.released = True
            for child in self.gates.values():
                if (
                    child.after_gate_id == gate_id
                    and child.target_flow is None
                ):
                    parent_flow = next(
                        (
                            flow
                            for flow in self.flows
                            if gate_id in flow.offsets
                        ),
                        None,
                    )
                    if parent_flow is not None:
                        child.target_flow = parent_flow.peer
            flows = list(self.flows)
        for flow in flows:
            flow.release()
        with self.condition:
            return self.snapshot(gate_id)

    def wait(self, gate_id: str, timeout_seconds: float) -> dict[str, object]:
        deadline = time.monotonic() + timeout_seconds
        with self.condition:
            gate = self._gate(gate_id)
            while gate.captured_count == 0:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                self.condition.wait(remaining)
            return self.snapshot(gate_id)

    def snapshot(self, gate_id: str) -> dict[str, object]:
        gate = self._gate(gate_id)
        return {
            "gateId": gate_id,
            "afterGateId": gate.after_gate_id,
            "capturedCount": gate.captured_count,
            "releasedCount": gate.released_count,
            "released": gate.released,
        }

    def _gate(self, gate_id: str) -> Gate:
        try:
            return self.gates[gate_id]
        except KeyError as error:
            raise ValueError(f"gate '{gate_id}' is not armed") from error


class ProxyHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        server = self.server
        assert isinstance(server, ProxyServer)
        upstream = socket.create_connection(server.upstream, timeout=5)
        upstream.settimeout(None)
        self.request.settimeout(None)
        left_to_right = Flow(server.state, upstream)
        right_to_left = Flow(server.state, self.request)
        left_to_right.peer = right_to_left
        right_to_left.peer = left_to_right
        with server.state.condition:
            server.state.flows.update((left_to_right, right_to_left))

        def pump(source: socket.socket, flow: Flow) -> None:
            try:
                while data := source.recv(65536):
                    flow.accept(data)
            except (ConnectionError, OSError):
                pass
            finally:
                flow.close()
                try:
                    flow.destination.shutdown(socket.SHUT_WR)
                except OSError:
                    pass

        reverse = threading.Thread(
            target=pump, args=(upstream, right_to_left), daemon=True)
        reverse.start()
        pump(self.request, left_to_right)
        reverse.join(timeout=5)
        upstream.close()


class ProxyServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(
        self,
        address: tuple[str, int],
        upstream: tuple[str, int],
        state: GateState,
    ) -> None:
        self.upstream = upstream
        self.state = state
        super().__init__(address, ProxyHandler)


class AdminHandler(BaseHTTPRequestHandler):
    server_version = "ZLinkStreamMarkerProxy/1"

    def do_GET(self) -> None:
        try:
            path, query = self._request_parts()
            if path == "/health":
                self._reply(200, {"status": "ready"})
                return
            if path == "/snapshot":
                self._reply(200, self._state().snapshot(self._required(query, "gateId")))
                return
            self._reply(404, {"error": "not found"})
        except ValueError as error:
            self._reply(400, {"error": str(error)})

    def do_POST(self) -> None:
        try:
            path, query = self._request_parts()
            if path == "/arm":
                body = self._json_body()
                result = self._state().arm(
                    str(body.get("gateId", "")),
                    str(body.get("marker", "")),
                    (
                        str(body["afterGateId"])
                        if body.get("afterGateId")
                        else None
                    ),
                )
            elif path == "/wait":
                result = self._state().wait(
                    self._required(query, "gateId"),
                    float(query.get("timeoutSeconds", ["10"])[0]),
                )
            elif path == "/release":
                result = self._state().release(
                    self._required(query, "gateId"))
            else:
                self._reply(404, {"error": "not found"})
                return
            self._reply(200, result)
        except (ValueError, json.JSONDecodeError) as error:
            self._reply(400, {"error": str(error)})

    def log_message(self, format: str, *args: object) -> None:
        return

    def _state(self) -> GateState:
        server = self.server
        assert isinstance(server, AdminServer)
        return server.state

    def _request_parts(self) -> tuple[str, dict[str, list[str]]]:
        parsed = urllib.parse.urlparse(self.path)
        return parsed.path, urllib.parse.parse_qs(parsed.query)

    def _required(
        self, query: dict[str, list[str]], name: str
    ) -> str:
        value = query.get(name, [""])[0]
        if not value:
            raise ValueError(f"{name} is required")
        return value

    def _json_body(self) -> dict[str, object]:
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            body = bytearray()
            while True:
                size = int(self.rfile.readline().split(b";", 1)[0], 16)
                if size == 0:
                    while self.rfile.readline() not in (b"\r\n", b"\n", b""):
                        pass
                    break
                body.extend(self.rfile.read(size))
                self.rfile.read(2)
            return json.loads(body)
        length = int(self.headers.get("Content-Length", "0"))
        return json.loads(self.rfile.read(length))

    def _reply(self, status: int, value: object) -> None:
        encoded = json.dumps(value).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)


class AdminServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address: tuple[str, int], state: GateState) -> None:
        self.state = state
        super().__init__(address, AdminHandler)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen-host", default="127.0.0.1")
    parser.add_argument("--listen-port", type=int, required=True)
    parser.add_argument("--upstream-host", required=True)
    parser.add_argument("--upstream-port", type=int, required=True)
    parser.add_argument("--admin-host", default="127.0.0.1")
    parser.add_argument("--admin-port", type=int, required=True)
    args = parser.parse_args()

    state = GateState()
    proxy = ProxyServer(
        (args.listen_host, args.listen_port),
        (args.upstream_host, args.upstream_port),
        state,
    )
    admin = AdminServer((args.admin_host, args.admin_port), state)
    proxy_thread = threading.Thread(target=proxy.serve_forever, daemon=True)
    proxy_thread.start()
    print(
        f"ready proxy={args.listen_host}:{args.listen_port} "
        f"upstream={args.upstream_host}:{args.upstream_port} "
        f"admin={args.admin_host}:{args.admin_port}",
        flush=True,
    )
    try:
        admin.serve_forever()
    finally:
        proxy.shutdown()
        proxy.server_close()
        admin.server_close()


if __name__ == "__main__":
    main()
