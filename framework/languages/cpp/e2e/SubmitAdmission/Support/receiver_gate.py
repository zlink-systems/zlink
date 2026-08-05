#!/usr/bin/env python3
"""Runner-owned TCP gate for Config 13 receiver admission scenarios."""

import argparse
import json
import select
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


BUFFER_BYTES = 4096


def split_endpoint(endpoint):
    if not endpoint.startswith("tcp://"):
        raise ValueError(f"TCP endpoint is required: {endpoint}")
    host, port = endpoint[6:].rsplit(":", 1)
    return host, int(port)


class GateState:
    def __init__(self):
        self.lock = threading.Lock()
        self.open = True
        self.closed_at_ns = None
        self.opened_at_ns = time.time_ns()
        self.bytes_forwarded = 0
        self.bytes_read_after_close = 0
        self.connection_generation = 0
        self.socket_buffers = []

    def close(self):
        with self.lock:
            self.open = False
            self.closed_at_ns = time.time_ns()

    def reopen(self):
        with self.lock:
            self.open = True
            self.opened_at_ns = time.time_ns()

    def register_connection(self, frontend, backend):
        with self.lock:
            self.connection_generation += 1
            self.socket_buffers.append(
                {
                    "generation": self.connection_generation,
                    "frontendSend": frontend.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF),
                    "frontendReceive": frontend.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF),
                    "backendSend": backend.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF),
                    "backendReceive": backend.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF),
                }
            )

    def status(self):
        with self.lock:
            return {
                "open": self.open,
                "gateClosedAt": self.closed_at_ns,
                "gateOpenedAt": self.opened_at_ns,
                "bytesForwarded": self.bytes_forwarded,
                "bytesReadAfterClose": self.bytes_read_after_close,
                "connectionGeneration": self.connection_generation,
                "socketBufferRequestBytes": BUFFER_BYTES,
                "connections": list(self.socket_buffers),
            }


def configure_socket(value):
    value.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, BUFFER_BYTES)
    value.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, BUFFER_BYTES)


def forward_uncontrolled(source, destination, stopped):
    try:
        while not stopped.is_set():
            data = source.recv(BUFFER_BYTES)
            if not data:
                return
            destination.sendall(data)
    except OSError:
        return
    finally:
        stopped.set()


def forward_with_gate(source, destination, state, stopped):
    try:
        source.setblocking(False)
        while not stopped.is_set():
            readable, _, _ = select.select([source], [], [], 0.05)
            if not readable:
                continue
            # close() takes the same lock. Once its HTTP response is returned,
            # this section cannot read another payload byte until reopen().
            with state.lock:
                if not state.open:
                    continue
                try:
                    data = source.recv(BUFFER_BYTES)
                except BlockingIOError:
                    continue
                if not data:
                    return
                state.bytes_forwarded += len(data)
            destination.sendall(data)
    except OSError:
        return
    finally:
        stopped.set()


def serve_connection(frontend, backend_address, state, blocked_direction):
    backend = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        configure_socket(frontend)
        configure_socket(backend)
        backend.connect(backend_address)
        state.register_connection(frontend, backend)
        stopped = threading.Event()
        if blocked_direction == "frontend-to-backend":
            reverse = threading.Thread(
                target=forward_uncontrolled,
                args=(backend, frontend, stopped),
                daemon=True,
            )
            reverse.start()
            forward_with_gate(frontend, backend, state, stopped)
        else:
            forward = threading.Thread(
                target=forward_uncontrolled,
                args=(frontend, backend, stopped),
                daemon=True,
            )
            forward.start()
            forward_with_gate(backend, frontend, state, stopped)
    finally:
        try:
            frontend.close()
        finally:
            backend.close()


class ControlHandler(BaseHTTPRequestHandler):
    state = None

    def log_message(self, _format, *_args):
        return

    def reply(self, status, payload):
        body = json.dumps(payload, sort_keys=True).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            self.reply(200, {"status": "ready"})
        elif self.path == "/status":
            self.reply(200, self.state.status())
        else:
            self.reply(404, {"error": "not found"})

    def do_POST(self):
        if self.path == "/close":
            self.state.close()
            self.reply(200, self.state.status())
        elif self.path == "/open":
            self.state.reopen()
            self.reply(200, self.state.status())
        else:
            self.reply(404, {"error": "not found"})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen", required=True)
    parser.add_argument("--backend", required=True)
    parser.add_argument("--control", required=True)
    parser.add_argument(
        "--blocked-direction",
        choices=("frontend-to-backend", "backend-to-frontend"),
        default="frontend-to-backend",
    )
    arguments = parser.parse_args()

    listen_address = split_endpoint(arguments.listen)
    backend_address = split_endpoint(arguments.backend)
    control_address = split_endpoint(arguments.control.replace("http://", "tcp://", 1))
    state = GateState()
    ControlHandler.state = state
    control = ThreadingHTTPServer(control_address, ControlHandler)
    threading.Thread(target=control.serve_forever, daemon=True).start()

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    configure_socket(listener)
    listener.bind(listen_address)
    listener.listen()
    try:
        while True:
            frontend, _ = listener.accept()
            threading.Thread(
                target=serve_connection,
                args=(frontend, backend_address, state, arguments.blocked_direction),
                daemon=True,
            ).start()
    finally:
        listener.close()
        control.shutdown()


if __name__ == "__main__":
    main()
