#!/usr/bin/env python3
"""Hold raw bytes in one direction for a selected ZLink routing identity."""

from __future__ import annotations

import argparse
import http.server
import json
import socket
import socketserver
import threading


class State:
    def __init__(self, routing_identity: str) -> None:
        self.condition = threading.Condition()
        self.routing_identity = routing_identity.encode()
        self.hold = False
        self.connections = 0
        self.selected_connections = 0
        self.held_bytes = 0

    def set_hold(self, value: bool) -> None:
        with self.condition:
            self.hold = value
            self.condition.notify_all()

    def snapshot(self) -> dict[str, object]:
        with self.condition:
            return {
                "hold": self.hold,
                "connections": self.connections,
                "selectedConnections": self.selected_connections,
                "heldBytes": self.held_bytes,
            }


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
        with proxy.state.condition:
            proxy.state.connections += 1

        def client_to_target() -> None:
            selected = False
            buffered = bytearray()
            self.request.settimeout(0.05)
            try:
                while True:
                    try:
                        data = self.request.recv(65536)
                    except socket.timeout:
                        data = None
                    if data == b"":
                        break
                    if data is None:
                        with proxy.state.condition:
                            flush = bool(buffered) and not proxy.state.hold
                        if flush:
                            upstream.sendall(buffered)
                            buffered.clear()
                        continue
                    identified_now = False
                    if not selected and proxy.state.routing_identity in data:
                        selected = True
                        identified_now = True
                        with proxy.state.condition:
                            proxy.state.selected_connections += 1
                    with proxy.state.condition:
                        # The identity is part of the transport handshake. It
                        # must reach the peer before application bytes can be
                        # held on the established connection.
                        if selected and not identified_now and proxy.state.hold:
                            buffered.extend(data)
                            proxy.state.held_bytes += len(data)
                            continue
                        if buffered:
                            upstream.sendall(buffered)
                            buffered.clear()
                        upstream.sendall(data)
                with proxy.state.condition:
                    while selected and proxy.state.hold:
                        proxy.state.condition.wait()
                    if buffered:
                        upstream.sendall(buffered)
            except (ConnectionError, OSError):
                pass

        def target_to_client() -> None:
            try:
                while data := upstream.recv(65536):
                    self.request.sendall(data)
            except (ConnectionError, OSError):
                pass

        threads = [threading.Thread(target=client_to_target, daemon=True),
                   threading.Thread(target=target_to_client, daemon=True)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()


class Control(http.server.BaseHTTPRequestHandler):
    state: State

    def do_GET(self) -> None:  # noqa: N802
        if self.path == "/health":
            self.send_response(200)
            self.end_headers()
            return
        if self.path == "/stats":
            payload = json.dumps(self.state.snapshot()).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(payload)
            return
        self.send_error(404)

    def do_POST(self) -> None:  # noqa: N802
        if self.path == "/hold":
            self.state.set_hold(True)
        elif self.path == "/release":
            self.state.set_hold(False)
        else:
            self.send_error(404)
            return
        payload = json.dumps(self.state.snapshot()).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *_: object) -> None:
        return


def address(value: str) -> tuple[str, int]:
    host, port = value.rsplit(":", 1)
    return host, int(port)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--control", required=True)
    parser.add_argument("--routing-identity", required=True)
    args = parser.parse_args()
    state = State(args.routing_identity)
    proxy = Proxy(address(args.listen), address(args.target), state)
    Control.state = state
    control = http.server.ThreadingHTTPServer(address(args.control), Control)
    threads = [threading.Thread(target=proxy.serve_forever, daemon=True),
               threading.Thread(target=control.serve_forever, daemon=True)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()


if __name__ == "__main__":
    main()
