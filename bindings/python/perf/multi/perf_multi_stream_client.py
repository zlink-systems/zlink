import argparse
import base64
import errno
import hashlib
import os
import selectors
import socket
import ssl
import struct
import sys
import time
from dataclasses import dataclass, field

from perf_multi_common import (
    HEADER_SIZE,
    benchmark_run_id,
    is_active_message,
    latency_ns_from_message,
    print_result_lines,
    resolve_multi_connect_ready_timeout_ms,
    resolve_multi_recv_timeout_ms,
    result_metrics,
    stamp_payload,
)


WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def _parse_args(argv):
    parser = argparse.ArgumentParser(prog="perf_multi_stream_client.py")
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--transport", default="tcp")
    parser.add_argument("--duration", type=float, default=5.0)
    parser.add_argument("--msg-size", type=int, default=64)
    parser.add_argument("--clients", type=int, default=10000)
    args = parser.parse_args(argv)
    if args.duration <= 0 or args.msg_size < HEADER_SIZE or args.clients <= 0:
        raise SystemExit("invalid perf arguments")
    args.transport = args.transport.lower()
    return args


def _parse_endpoint(endpoint):
    scheme, rest = endpoint.split("://", 1)
    host, port_text = rest.rsplit(":", 1)
    return scheme.lower(), host, int(port_text)


def _stream_packet(payload):
    return b"\x00\x00" + len(payload).to_bytes(4, "big") + payload


def _parse_stream_frames(buffer):
    frames = []
    offset = 0
    while len(buffer) - offset >= 6:
        header_size = int.from_bytes(buffer[offset : offset + 2], "big")
        body_size = int.from_bytes(buffer[offset + 2 : offset + 6], "big")
        frame_end = offset + 6 + header_size + body_size
        if len(buffer) < frame_end:
            break
        body_offset = offset + 6 + header_size
        frames.append(bytes(buffer[body_offset:frame_end]))
        offset = frame_end
    if offset:
        del buffer[:offset]
    return frames


def _websocket_frame(payload):
    mask = os.urandom(4)
    length = len(payload)
    if length < 126:
        header = bytes([0x82, 0x80 | length])
    elif length < 65536:
        header = bytes([0x82, 0x80 | 126]) + struct.pack("!H", length)
    else:
        header = bytes([0x82, 0x80 | 127]) + struct.pack("!Q", length)
    masked = bytes(payload[index] ^ mask[index % 4] for index in range(length))
    return header + mask + masked


def _parse_websocket_frames(buffer):
    frames = []
    offset = 0
    while len(buffer) - offset >= 2:
        first = buffer[offset]
        second = buffer[offset + 1]
        fin = bool(first & 0x80)
        opcode = first & 0x0F
        masked = bool(second & 0x80)
        length = second & 0x7F
        cursor = offset + 2
        if length == 126:
            if len(buffer) - cursor < 2:
                break
            length = struct.unpack("!H", buffer[cursor : cursor + 2])[0]
            cursor += 2
        elif length == 127:
            if len(buffer) - cursor < 8:
                break
            length = struct.unpack("!Q", buffer[cursor : cursor + 8])[0]
            cursor += 8
        if masked:
            if len(buffer) - cursor < 4:
                break
            mask = buffer[cursor : cursor + 4]
            cursor += 4
        else:
            mask = None
        frame_end = cursor + length
        if len(buffer) < frame_end:
            break
        payload = bytes(buffer[cursor:frame_end])
        if mask is not None:
            payload = bytes(
                payload[index] ^ mask[index % 4] for index in range(len(payload))
            )
        if not fin:
            raise RuntimeError("fragmented websocket frame is not supported")
        if opcode == 0x8:
            raise RuntimeError("websocket connection closed by peer")
        if opcode == 0x2:
            frames.append(payload)
        offset = frame_end
    if offset:
        del buffer[:offset]
    return frames


def _build_ws_handshake(host, port):
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    request = (
        f"GET / HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    ).encode("ascii")
    accept = base64.b64encode(
        hashlib.sha1(f"{key}{WS_GUID}".encode("ascii")).digest()
    ).decode("ascii")
    return request, accept


def _perform_ws_handshake(sock, host, port):
    request, expected_accept = _build_ws_handshake(host, port)
    sock.sendall(request)
    response = bytearray()
    while b"\r\n\r\n" not in response:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("websocket handshake closed before response")
        response.extend(chunk)
    header_text = response.decode("latin-1")
    if " 101 " not in header_text:
        raise RuntimeError(f"websocket handshake failed: {header_text.splitlines()[0]}")
    accept_line = next(
        (
            line.split(":", 1)[1].strip()
            for line in header_text.split("\r\n")
            if line.lower().startswith("sec-websocket-accept:")
        ),
        "",
    )
    if accept_line != expected_accept:
        raise RuntimeError("websocket handshake accept mismatch")


def _wrap_tls(sock, host):
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    return context.wrap_socket(sock, server_hostname=host)


def _send_buffer(sock, buffer):
    while buffer:
        try:
            sent = sock.send(buffer)
        except (BlockingIOError, ssl.SSLWantWriteError):
            return False
        if sent <= 0:
            raise RuntimeError("stream send returned 0")
        del buffer[:sent]
    return True


def _recv_into_buffer(sock, buffer):
    try:
        chunk = sock.recv(65536)
    except (BlockingIOError, ssl.SSLWantReadError):
        return False
    if not chunk:
        raise RuntimeError("stream peer closed connection")
    buffer.extend(chunk)
    return True


@dataclass
class StreamClientState:
    sock: socket.socket
    transport: str
    payload: bytearray
    send_buffer: bytearray = field(default_factory=bytearray)
    recv_buffer: bytearray = field(default_factory=bytearray)
    waiting_reply: bool = False
    send_pending: bool = True

    def queue_request(self, *, run_id, seq):
        body = stamp_payload(self.payload, phase=1, run_id=run_id, seq=seq)
        frame = _stream_packet(bytes(body))
        if self.transport in {"ws", "wss"}:
            frame = _websocket_frame(frame)
        self.send_buffer.extend(frame)

    def ready_frames(self):
        if self.transport in {"ws", "wss"}:
            payloads = _parse_websocket_frames(self.recv_buffer)
            bodies = []
            for payload in payloads:
                inner = bytearray(payload)
                bodies.extend(_parse_stream_frames(inner))
            return bodies
        return _parse_stream_frames(self.recv_buffer)


def _open_stream_socket(transport, host, port, timeout_s):
    base = socket.create_connection((host, port), timeout=timeout_s)
    if transport in {"tls", "wss"}:
        base = _wrap_tls(base, host)
    if transport in {"ws", "wss"}:
        _perform_ws_handshake(base, host, port)
    base.setblocking(False)
    return base


def main(argv=None):
    # PERF_MULTI_TEST_POLICY: the MULTI_STREAM client role MUST be the shared
    # C binary bindings/c/perf/common/streamclient (the measured surface is
    # the Python STREAM server). run_benchmarks.py spawns that binary; this
    # hand-rolled Python client is disabled to prevent measuring the wrong
    # surface.
    raise SystemExit(
        "perf_multi_stream_client.py is disabled: the MULTI_STREAM client "
        "must be the shared C perf_stream_client binary "
        "(bindings/c/perf/common/streamclient). run_benchmarks.py spawns it "
        "automatically."
    )
    args = _parse_args(argv or sys.argv[1:])  # noqa: F841  (unreachable)
    transport, host, port = _parse_endpoint(args.endpoint)
    if transport != args.transport:
        raise SystemExit("endpoint transport must match --transport")

    run_id = benchmark_run_id()
    connect_timeout_s = resolve_multi_connect_ready_timeout_ms() / 1000.0
    recv_timeout_s = resolve_multi_recv_timeout_ms() / 1000.0
    selector = selectors.DefaultSelector()
    states = []
    latencies = []
    seq = 0

    try:
        for _index in range(args.clients):
            sock = _open_stream_socket(args.transport, host, port, connect_timeout_s)
            state = StreamClientState(
                sock=sock,
                transport=args.transport,
                payload=bytearray(args.msg_size),
            )
            states.append(state)
            selector.register(sock, selectors.EVENT_READ | selectors.EVENT_WRITE, state)
    except Exception:
        for state in states:
            state.sock.close()
        raise

    connect_ok = len(states)
    if connect_ok != args.clients:
        raise RuntimeError(
            f"stream connect mismatch: connect_ok={connect_ok} target_clients={args.clients}"
        )

    active_deadline = time.perf_counter() + args.duration
    drain_deadline = active_deadline + max(recv_timeout_s, 0.2)
    while time.perf_counter() < drain_deadline:
        now = time.perf_counter()
        if now >= active_deadline:
            for state in states:
                state.send_pending = False
        events = selector.select(timeout=0.05)
        if not events and now >= active_deadline and not any(
            state.waiting_reply or state.send_buffer for state in states
        ):
            break
        for key, mask in events:
            state = key.data
            if mask & selectors.EVENT_WRITE:
                if now < active_deadline and not state.waiting_reply and state.send_pending:
                    seq += 1
                    state.queue_request(run_id=run_id, seq=seq)
                    state.waiting_reply = True
                    state.send_pending = False
                if state.send_buffer:
                    _send_buffer(state.sock, state.send_buffer)
            if mask & selectors.EVENT_READ:
                _recv_into_buffer(state.sock, state.recv_buffer)
                for body in state.ready_frames():
                    state.waiting_reply = False
                    if now < active_deadline:
                        state.send_pending = True
                    if not is_active_message(
                        body,
                        expected_msg_size=args.msg_size,
                        run_id=run_id,
                    ):
                        continue
                    latency = latency_ns_from_message(body)
                    if latency is not None:
                        latencies.append(latency / 2.0)

    for key in list(selector.get_map().values()):
        selector.unregister(key.fileobj)
    selector.close()
    for state in states:
        state.sock.close()

    if not latencies:
        raise RuntimeError("multi stream benchmark did not receive any active reply")
    metrics = result_metrics(
        count=len(latencies),
        msg_size=args.msg_size,
        elapsed_s=args.duration,
        latencies_ns=latencies,
        bandwidth_multiplier=2.0,
    )
    print_result_lines("MULTI_STREAM", args.transport, args.msg_size, metrics)


if __name__ == "__main__":
    main()
