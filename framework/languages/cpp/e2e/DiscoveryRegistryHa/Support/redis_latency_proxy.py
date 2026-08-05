#!/usr/bin/env python3

import argparse
import http.server
import json
import socket
import socketserver
import threading
import time


class LatencyState:
    def __init__(self):
        self._delay_ms = 0
        self._lock = threading.Lock()

    def set_delay(self, delay_ms):
        with self._lock:
            self._delay_ms = max(0, min(int(delay_ms), 5000))

    def delay_seconds(self):
        with self._lock:
            return self._delay_ms / 1000.0


class ProxyHandler(socketserver.BaseRequestHandler):
    def handle(self):
        upstream = socket.create_connection(self.server.upstream_address, timeout=5)
        upstream.settimeout(None)

        def forward_requests():
            try:
                while data := self.request.recv(65536):
                    upstream.sendall(data)
            except OSError:
                pass
            finally:
                try:
                    upstream.shutdown(socket.SHUT_WR)
                except OSError:
                    pass

        request_thread = threading.Thread(target=forward_requests, daemon=True)
        request_thread.start()
        try:
            while data := upstream.recv(65536):
                delay = self.server.latency_state.delay_seconds()
                if delay > 0:
                    time.sleep(delay)
                self.request.sendall(data)
        except OSError:
            pass
        finally:
            upstream.close()
            try:
                self.request.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


class ThreadingProxyServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class AdminHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/health":
            self.send_error(404)
            return
        self._reply({"status": "ok"})

    def do_POST(self):
        if self.path != "/delay":
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            body = json.loads(self.rfile.read(length) or b"{}")
            self.server.latency_state.set_delay(body["milliseconds"])
        except (KeyError, TypeError, ValueError, json.JSONDecodeError):
            self.send_error(400)
            return
        self._reply({"status": "ok"})

    def log_message(self, _format, *_args):
        return

    def _reply(self, value):
        body = json.dumps(value).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class ThreadingAdminServer(http.server.ThreadingHTTPServer):
    daemon_threads = True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen-port", type=int, required=True)
    parser.add_argument("--admin-port", type=int, required=True)
    parser.add_argument("--upstream-host", required=True)
    parser.add_argument("--upstream-port", type=int, required=True)
    args = parser.parse_args()

    state = LatencyState()
    proxy = ThreadingProxyServer(("127.0.0.1", args.listen_port), ProxyHandler)
    proxy.upstream_address = (args.upstream_host, args.upstream_port)
    proxy.latency_state = state
    admin = ThreadingAdminServer(("127.0.0.1", args.admin_port), AdminHandler)
    admin.latency_state = state

    admin_thread = threading.Thread(target=admin.serve_forever, daemon=True)
    admin_thread.start()
    try:
        proxy.serve_forever()
    finally:
        proxy.shutdown()
        admin.shutdown()
        proxy.server_close()
        admin.server_close()


if __name__ == "__main__":
    main()
