#!/usr/bin/env python3
"""Small TCP proxy that can blackhole only client-to-target traffic."""

from __future__ import annotations

import argparse
import http.server
import json
import socket
import socketserver
import threading


class ProxyState:
    def __init__(
        self,
        block_source_host: str | None = None,
        block_routing_prefix: str | None = None,
    ) -> None:
        self.lock = threading.Lock()
        self.block_source_host = block_source_host
        self.block_routing_prefix = block_routing_prefix.encode() if block_routing_prefix else None
        self.block_client_to_target = False
        self.block_target_to_client = False
        self.accepted = 0
        self.dropped_bytes = 0
        self.routing_matches: set[int] = set()
        self.routing_identified: set[int] = set()

    def blocked(self, client_to_target: bool, source_host: str) -> bool:
        with self.lock:
            if self.block_source_host is not None and source_host != self.block_source_host:
                return False
            return (self.block_client_to_target if client_to_target
                    else self.block_target_to_client)

    def set_blocked(self, value: bool) -> None:
        with self.lock:
            self.block_client_to_target = value

    def set_target_to_client_blocked(self, value: bool) -> None:
        with self.lock:
            self.block_target_to_client = value

    def unblock(self) -> None:
        with self.lock:
            self.block_client_to_target = False
            self.block_target_to_client = False

    def record_connection(self, source: str) -> int:
        with self.lock:
            self.accepted += 1
            index = self.accepted - 1
            return index

    def record_sample(self, index: int, direction: str, data: bytes) -> None:
        with self.lock:
            if index not in self.routing_identified and self.block_routing_prefix is not None:
                if self.block_routing_prefix in data:
                    self.routing_matches.add(index)
                    self.routing_identified.add(index)

    def is_routing_match(self, index: int) -> bool:
        with self.lock:
            return index in self.routing_matches

    def was_routing_identified(self, index: int) -> bool:
        with self.lock:
            return index in self.routing_identified

    def record_drop(self, count: int) -> None:
        with self.lock:
            self.dropped_bytes += count

    def snapshot(self) -> tuple[bool, int, int]:
        with self.lock:
            return (self.block_client_to_target or self.block_target_to_client,
                    self.accepted, self.dropped_bytes)


def pump(source: socket.socket, target: socket.socket, state: ProxyState, client_to_target: bool, source_host: str, connection_index: int) -> None:
    try:
        while True:
            data = source.recv(65536)
            if not data:
                break
            was_identified = state.was_routing_identified(connection_index)
            state.record_sample(connection_index, "clientToTarget" if client_to_target else "targetToClient", data)
            drop = state.blocked(client_to_target, source_host)
            if state.block_routing_prefix is not None:
                # Forward the initial handshake so the connection can be
                # classified by routing identity before blackholing it.
                drop = drop and was_identified and state.is_routing_match(connection_index)
            if not drop:
                target.sendall(data)
            else:
                state.record_drop(len(data))
    except (ConnectionError, OSError):
        pass
    finally:
        for current in (source, target):
            try:
                current.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                current.close()
            except OSError:
                pass


class TcpProxy(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address, target, state: ProxyState):
        self.target = target
        self.state = state
        super().__init__(address, ProxyHandler)


class ProxyHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        proxy: TcpProxy = self.server  # type: ignore[assignment]
        source_host = self.client_address[0]
        connection_index = proxy.state.record_connection(source_host)
        upstream = socket.create_connection(proxy.target, timeout=5)
        upstream.settimeout(None)
        self.request.settimeout(None)
        threads = [
            threading.Thread(
                target=pump,
                args=(self.request, upstream, proxy.state, True, source_host, connection_index),
                daemon=True,
            ),
            threading.Thread(
                target=pump,
                args=(upstream, self.request, proxy.state, False, proxy.target[0], connection_index),
                daemon=True,
            ),
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()


class ControlHandler(http.server.BaseHTTPRequestHandler):
    state: ProxyState

    def do_GET(self) -> None:  # noqa: N802
        if self.path == "/health":
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"ok")
            return
        if self.path == "/stats":
            blocked, accepted, dropped = self.state.snapshot()
            payload = json.dumps({
                "blocked": blocked,
                "accepted": accepted,
                "droppedBytes": dropped,
            }).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(payload)
            return
        self.send_error(404)

    def do_POST(self) -> None:  # noqa: N802
        if self.path == "/block/client-to-target":
            self.state.set_blocked(True)
        elif self.path == "/block/target-to-client":
            self.state.set_target_to_client_blocked(True)
        elif self.path == "/unblock":
            self.state.unblock()
        else:
            self.send_error(404)
            return
        self.send_response(204)
        self.end_headers()

    def log_message(self, format: str, *args) -> None:
        return


class ControlServer(http.server.ThreadingHTTPServer):
    daemon_threads = True


def parse_address(value: str) -> tuple[str, int]:
    host, port = value.rsplit(":", 1)
    return host, int(port)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--control", required=True)
    parser.add_argument("--block-source-host")
    parser.add_argument("--block-routing-prefix")
    args = parser.parse_args()
    state = ProxyState(args.block_source_host, args.block_routing_prefix)
    tcp = TcpProxy(parse_address(args.listen), parse_address(args.target), state)
    ControlHandler.state = state
    control = ControlServer(parse_address(args.control), ControlHandler)
    threads = [
        threading.Thread(target=tcp.serve_forever, daemon=True),
        threading.Thread(target=control.serve_forever, daemon=True),
    ]
    for thread in threads:
        thread.start()
    try:
        for thread in threads:
            thread.join()
    except KeyboardInterrupt:
        pass
    finally:
        tcp.shutdown()
        control.shutdown()
        tcp.server_close()
        control.server_close()


if __name__ == "__main__":
    main()
