#!/usr/bin/env python3
"""Small TCP proxy that can blackhole only client-to-target traffic."""

from __future__ import annotations

import argparse
import http.server
import socket
import socketserver
import threading


class ProxyState:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.block_client_to_target = False
        self.accepted = 0
        self.dropped_bytes = 0

    def blocked(self) -> bool:
        with self.lock:
            return self.block_client_to_target

    def set_blocked(self, value: bool) -> None:
        with self.lock:
            self.block_client_to_target = value

    def record_connection(self) -> None:
        with self.lock:
            self.accepted += 1

    def record_drop(self, count: int) -> None:
        with self.lock:
            self.dropped_bytes += count

    def snapshot(self) -> tuple[bool, int, int]:
        with self.lock:
            return self.block_client_to_target, self.accepted, self.dropped_bytes


def pump(source: socket.socket, target: socket.socket, state: ProxyState, drop: bool) -> None:
    try:
        while True:
            data = source.recv(65536)
            if not data:
                break
            if not (drop and state.blocked()):
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
        proxy.state.record_connection()
        upstream = socket.create_connection(proxy.target, timeout=5)
        upstream.settimeout(None)
        self.request.settimeout(None)
        threads = [
            threading.Thread(
                target=pump,
                args=(self.request, upstream, proxy.state, True),
                daemon=True,
            ),
            threading.Thread(
                target=pump,
                args=(upstream, self.request, proxy.state, False),
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
            payload = f'{{"blocked":{str(blocked).lower()},"accepted":{accepted},"droppedBytes":{dropped}}}'.encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(payload)
            return
        self.send_error(404)

    def do_POST(self) -> None:  # noqa: N802
        if self.path == "/block/client-to-target":
            self.state.set_blocked(True)
        elif self.path == "/unblock":
            self.state.set_blocked(False)
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
    args = parser.parse_args()
    state = ProxyState()
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
