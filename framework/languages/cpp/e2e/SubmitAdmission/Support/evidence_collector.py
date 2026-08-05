#!/usr/bin/env python3
"""Independent process evidence store for Config 13 source-lifecycle scenarios."""

import argparse
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse


class EvidenceStore:
    def __init__(self):
        self.lock = threading.Lock()
        self.operations = {}

    def append(self, evidence):
        operation_id = evidence.get("operationId")
        if not isinstance(operation_id, str) or not operation_id:
            raise ValueError("operationId is required")
        with self.lock:
            self.operations.setdefault(operation_id, []).append(evidence)

    def get(self, operation_id):
        with self.lock:
            events = list(self.operations.get(operation_id, []))
        return {"operationId": operation_id, "events": events, "eventCount": len(events)}


class Handler(BaseHTTPRequestHandler):
    store = None

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
        parsed = urlparse(self.path)
        if parsed.path == "/health":
            self.reply(200, {"status": "ready"})
            return
        if parsed.path == "/evidence":
            operation_id = parse_qs(parsed.query).get("operationId", [""])[0]
            if not operation_id:
                self.reply(400, {"error": "operationId is required"})
                return
            self.reply(200, self.store.get(operation_id))
            return
        self.reply(404, {"error": "not found"})

    def do_POST(self):
        if self.path != "/evidence":
            self.reply(404, {"error": "not found"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            evidence = json.loads(self.rfile.read(length).decode("utf-8"))
            self.store.append(evidence)
            self.reply(202, {"accepted": True, "operationId": evidence["operationId"]})
        except (ValueError, json.JSONDecodeError) as error:
            self.reply(400, {"error": str(error)})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen", required=True)
    arguments = parser.parse_args()
    endpoint = arguments.listen.replace("http://", "", 1)
    host, port = endpoint.rsplit(":", 1)
    Handler.store = EvidenceStore()
    ThreadingHTTPServer((host, int(port)), Handler).serve_forever()


if __name__ == "__main__":
    main()
