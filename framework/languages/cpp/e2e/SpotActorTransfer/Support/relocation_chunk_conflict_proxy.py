#!/usr/bin/env python3
"""Inject the contracted relocation direct-transfer fault on the mesh wire.

The proxy sits in front of one node's mesh router endpoint and forwards raw
ZMP bytes untouched until it is armed through its control endpoint. Once
armed it fires exactly once: the first `relocationState` (command 52) record
it observes is forwarded unchanged and then re-emitted a second time with the
last byte of the record flipped.

`chunk_data` is the trailing field of the relocationState record (see
service_wire_codec.cpp encode_relocation_control), so flipping the record's
last byte keeps the exact relocation identity -- RelocationId,
targetAttemptGeneration, coordinator fence, object, chunk ordinal and the
declared chunk length -- and changes only the payload bytes. That is the
contract's fault point: the same exact relocation identity arriving a second
time with different bytes while the target's staging already holds the first
chunk.

The proxy never invents framing: it copies the observed ZMP frame bytes
verbatim and only flips one byte in the copy, so the target decodes a
well-formed record and reaches the assembly, not the malformed-wire path.

STATUS -- NOT WIRED INTO run_e2e.sh YET (2026-08-22).  The seam itself was
built and verified end to end: both actor routers bind 127.0.0.2 and
advertise 127.0.0.1 where this proxy listens (the ST-F3A bind/advertise
split), mesh traffic passes through untouched, and a whole scenario
(ST-C1) runs green behind it.  What is missing is the *trigger*: nothing
in this config puts a relocationState(52) record on the wire.  A remote
Actor Join carries the captured state inline as
spot_actor_commit_route_request_t.transfer_state (core_transfer=true, see
mesh_node_runtime.cpp prepare_remote_application_actor_join), and the only
producer of relocationState chunks -- maintenance_runtime.cpp
relocate_send_state_chunks, reached through maintenance->relocate -- has
exactly two callers, mesh_node_runtime.cpp relocate_application_actor and
relocate_application_unit, both invoked only from app.cpp's termination
(drain) relocation.  Observed, not assumed: with both actor routers behind
this proxy, ST-C4's Join, ST-C1, and a manual create-then-shutdown drain
produced zero records of kinds 30/31/34/40/52/53, while the same
connection carried actorJoin(28).  Driving the ST-C4 fault point therefore
needs a relocation shape that uses the canonical chunk wire while the
source node stays alive -- a coordinator/spec call, not a harness one.

Harness wiring this expects, once such a trigger exists:
  --route 127.0.0.1:<A_ROUTER>=127.0.0.2:<A_ROUTER>
  --route 127.0.0.1:<B_ROUTER>=127.0.0.2:<B_ROUTER>
  --control 127.0.0.1:<ADMIN>
with both actor nodes started on the 127.0.0.2 bind plus
routerAdvertiseHost=127.0.0.1, and the source node given a small
relocation_payload_chunk_limit_bytes (the contracted location option, so
one payload spans several chunks and the target's assembly is holding
staged bytes when the divergent duplicate arrives).  The scenario arms it
with POST /arm and reads GET /state (stateFrames/injectedFrames) so a
topology or chunk-limit regression fails loudly instead of degrading the
assertion.
"""

from __future__ import annotations

import argparse
import http.server
import json
import socket
import socketserver
import threading

# core/src/runtime/protocol/zmp_protocol.hpp: every ZMP frame is
# [magic 0x5A][version 0x01][flags][0x00][u32 big-endian body size][body].
ZMP_MAGIC = 0x5A
ZMP_VERSION = 0x01
ZMP_HEADER_SIZE = 8
ZMP_FLAG_MORE = 0x01

# service_wire_constants.hpp: magic = {90, 77}, record prefix is
# {magic0, magic1, wireMajor, command, flags}; command::relocationState = 52.
WIRE_MAGIC = bytes((90, 77))
COMMAND_RELOCATION_STATE = 52
PREFIX_SIZE = 5


class State:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.armed = False
        self.fired = False
        self.connections = 0
        self.state_frames = 0
        self.injected_frames = 0
        self.skipped_frames = 0
        self.kinds: dict[str, int] = {}

    def arm(self) -> None:
        with self.lock:
            self.armed = True

    def observe_state_message(self, injectable: bool) -> bool:
        """Count one relocationState record; return True when it must be duplicated."""
        with self.lock:
            self.state_frames += 1
            if not injectable:
                self.skipped_frames += 1
                return False
            if not self.armed or self.fired:
                return False
            self.fired = True
            self.injected_frames += 1
            return True

    def snapshot(self) -> dict[str, object]:
        with self.lock:
            return {
                "armed": self.armed,
                "fired": self.fired,
                "connections": self.connections,
                "stateFrames": self.state_frames,
                "injectedFrames": self.injected_frames,
                "skippedFrames": self.skipped_frames,
                "kinds": dict(sorted(self.kinds.items())),
            }


def is_relocation_state_record(body: bytes) -> bool:
    return (
        len(body) > PREFIX_SIZE
        and body[0:2] == WIRE_MAGIC
        and body[3] == COMMAND_RELOCATION_STATE
    )


class FrameParser:
    """Split a ZMP byte stream into whole frames without altering them."""

    def __init__(self) -> None:
        self._buffer = bytearray()

    def feed(self, data: bytes) -> list[tuple[bytes, int, bytes]]:
        """Return [(raw_frame_bytes, flags, body)] for every complete frame."""
        out: list[tuple[bytes, int, bytes]] = []
        self._buffer.extend(data)
        while len(self._buffer) >= ZMP_HEADER_SIZE:
            if self._buffer[0] != ZMP_MAGIC or self._buffer[1] != ZMP_VERSION:
                raise ValueError(
                    f"unexpected ZMP frame header {bytes(self._buffer[:8]).hex()}")
            flags = self._buffer[2]
            size = int.from_bytes(self._buffer[4:8], "big")
            total = ZMP_HEADER_SIZE + size
            if len(self._buffer) < total:
                break
            raw = bytes(self._buffer[:total])
            del self._buffer[:total]
            out.append((raw, flags, raw[ZMP_HEADER_SIZE:]))
        return out


def pump_raw(source: socket.socket, sink: socket.socket) -> None:
    try:
        while True:
            data = source.recv(65536)
            if not data:
                break
            sink.sendall(data)
    except OSError:
        pass
    finally:
        try:
            sink.shutdown(socket.SHUT_WR)
        except OSError:
            pass


def pump_inspected(source: socket.socket, sink: socket.socket, state: State, tag: str = "?") -> None:
    parser = FrameParser()
    message = bytearray()
    carries_state_record = False
    try:
        while True:
            data = source.recv(65536)
            if not data:
                break
            for raw, flags, body in parser.feed(data):
                sink.sendall(raw)
                message.extend(raw)
                if len(body) > PREFIX_SIZE and body[0:2] == WIRE_MAGIC:
                    if body[3] in (30, 31, 34, 40, 52, 53):
                        print(f"relocation-record {tag} kind={body[3]} len={len(body)}", flush=True)
                    key = f"{tag}:{body[3]}"
                    with state.lock:
                        state.kinds[key] = state.kinds.get(key, 0) + 1
                if is_relocation_state_record(body):
                    carries_state_record = True
                if flags & ZMP_FLAG_MORE:
                    continue
                if carries_state_record:
                    # The record must be the message's last frame: its
                    # chunk_data is the trailing field, so the message's last
                    # byte is a payload byte and nothing else.
                    injectable = is_relocation_state_record(body)
                    if state.observe_state_message(injectable):
                        # Same frames, same record, one flipped byte at the
                        # very end of chunk_data: exact identity intact,
                        # payload divergent.
                        duplicate = bytearray(message)
                        duplicate[-1] ^= 0x01
                        sink.sendall(bytes(duplicate))
                message.clear()
                carries_state_record = False
    except OSError:
        pass
    except Exception as error:  # noqa: BLE001 - a parse fault must be visible
        print(f"chunk-conflict proxy inspect failed: {error!r}", flush=True)
    finally:
        try:
            sink.shutdown(socket.SHUT_WR)
        except OSError:
            pass


class Proxy(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address: tuple[str, int], target: tuple[str, int], state: State):
        self.target = target
        self.state = state
        super().__init__(address, Handler)


class Handler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        proxy = self.server
        assert isinstance(proxy, Proxy)
        upstream = socket.create_connection(proxy.target, timeout=5)
        upstream.settimeout(None)
        self.request.settimeout(None)
        with proxy.state.lock:
            proxy.state.connections += 1
        print(f"connect from={self.client_address} listen={proxy.server_address} target={proxy.target}", flush=True)
        # Both directions are inspected: mesh peers connect to each other's
        # router, so which of the two connections carries the source-to-target
        # relocation traffic is not fixed by the harness.
        downstream = threading.Thread(
            target=pump_inspected,
            args=(upstream, self.request, proxy.state,
                  f"down{proxy.server_address[1]}"),
            daemon=True)
        downstream.start()
        pump_inspected(self.request, upstream, proxy.state,
                       f"up{proxy.server_address[1]}")
        downstream.join(timeout=5)
        try:
            upstream.close()
        except OSError:
            pass


class Control(http.server.BaseHTTPRequestHandler):
    state: State

    def _send(self, payload: dict[str, object]) -> None:
        body = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        if self.path in ("/health", "/state"):
            self._send(Control.state.snapshot())
            return
        self.send_error(404)

    def do_POST(self) -> None:  # noqa: N802
        if self.path == "/arm":
            Control.state.arm()
            self._send(Control.state.snapshot())
            return
        self.send_error(404)

    def log_message(self, *_: object) -> None:
        return


def split(address: str) -> tuple[str, int]:
    host, port = address.rsplit(":", 1)
    return host, int(port)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--route", action="append", required=True,
                        metavar="LISTEN=TARGET",
                        help="one listen=target router hop; repeatable")
    parser.add_argument("--control", required=True)
    args = parser.parse_args()

    state = State()
    Control.state = state
    control = socketserver.ThreadingTCPServer(split(args.control), Control)
    control.allow_reuse_address = True
    control.daemon_threads = True
    threading.Thread(target=control.serve_forever, daemon=True).start()

    hops = []
    for route in args.route:
        listen, target = route.split("=", 1)
        hops.append(Proxy(split(listen), split(target), state))
    for hop in hops[1:]:
        threading.Thread(target=hop.serve_forever, daemon=True).start()
    hops[0].serve_forever()


if __name__ == "__main__":
    main()
