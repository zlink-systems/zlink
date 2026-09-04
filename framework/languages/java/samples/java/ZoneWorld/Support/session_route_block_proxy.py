#!/usr/bin/env python3
"""Transparent ZMP proxy that blocks command 44 for the ZoneWorld B8 lane."""

import argparse
import os
import socket
import socketserver
import threading

ZMP_HEADER_SIZE = 8
ZMP_REQUEST_SEQUENCE_SIZE = 8
ZMP_FLAG_MORE = 0x01
ZMP_REQUEST_REPLY_KINDS = frozenset((0x01, 0x02, 0x03))
WIRE_MAGIC = bytes((90, 77))
SESSION_RELOCATION_ROUTE = 44

class FrameParser:
    def __init__(self): self.buffer = bytearray()
    def feed(self, data):
        self.buffer.extend(data); frames = []
        while len(self.buffer) >= ZMP_HEADER_SIZE:
            if self.buffer[0] != 0x5A or self.buffer[1] != 0x01:
                raise ValueError("unexpected ZMP frame header")
            flags = self.buffer[2]; kind = self.buffer[3]
            size = int.from_bytes(self.buffer[4:8], "big")
            header_size = ZMP_HEADER_SIZE
            if kind in ZMP_REQUEST_REPLY_KINDS:
                header_size += ZMP_REQUEST_SEQUENCE_SIZE
            total = header_size + size
            if len(self.buffer) < total: break
            raw = bytes(self.buffer[:total]); del self.buffer[:total]
            frames.append((raw, flags, raw[header_size:]))
        return frames

def is_command_44(body):
    return len(body) >= 5 and body[:2] == WIRE_MAGIC and body[3] == SESSION_RELOCATION_ROUTE

def pump(source, sink, direction, arm_file):
    parser = FrameParser(); message = bytearray(); blocked = False
    try:
        while True:
            data = source.recv(65536)
            if not data: break
            for raw, flags, body in parser.feed(data):
                message.extend(raw)
                blocked = blocked or (os.path.exists(arm_file) and is_command_44(body))
                if flags & ZMP_FLAG_MORE: continue
                if blocked: print(f"blocked-command-44 direction={direction}", flush=True)
                else: sink.sendall(message)
                message.clear(); blocked = False
    except (OSError, ValueError) as error:
        print(f"proxy-pump-ended direction={direction} error={error!r}", flush=True)
    finally:
        try: sink.shutdown(socket.SHUT_WR)
        except OSError: pass

class Proxy(socketserver.ThreadingTCPServer):
    allow_reuse_address = True; daemon_threads = True
    def __init__(self, listen, target, arm_file):
        self.target = target; self.arm_file = arm_file
        super().__init__(listen, Handler)

class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        proxy = self.server; upstream = socket.create_connection(proxy.target, timeout=10)
        upstream.settimeout(None); self.request.settimeout(None)
        print(f"proxy-connection listen={proxy.server_address} target={proxy.target}", flush=True)
        downstream = threading.Thread(target=pump,
            args=(upstream, self.request, "gateway-to-peer", proxy.arm_file), daemon=True)
        downstream.start(); pump(self.request, upstream, "peer-to-gateway", proxy.arm_file)
        downstream.join(timeout=5); upstream.close()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen-host", required=True); parser.add_argument("--listen-port", type=int, required=True)
    parser.add_argument("--target-host", required=True); parser.add_argument("--target-port", type=int, required=True)
    parser.add_argument("--arm-file", required=True); args = parser.parse_args()
    with Proxy((args.listen_host, args.listen_port), (args.target_host, args.target_port), args.arm_file) as proxy:
        print(f"proxy-ready listen={args.listen_host}:{args.listen_port} target={args.target_host}:{args.target_port}", flush=True)
        proxy.serve_forever()

if __name__ == "__main__": main()
