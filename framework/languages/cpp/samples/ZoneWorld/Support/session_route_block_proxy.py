#!/usr/bin/env python3
"""Transparent ZMP proxy that drops command 44 while the B8 arm file exists."""

from __future__ import annotations

import argparse
import os
import socket
import socketserver
import threading
from pathlib import Path

ZMP_MAGIC = 0x5A
ZMP_VERSION = 0x01
ZMP_HEADER_SIZE = 8
ZMP_FLAG_MORE = 0x01
WIRE_MAGIC = bytes((90, 77))
SESSION_RELOCATION_ROUTE = 44


class FrameParser:
    def __init__(self) -> None:
        self._buffer = bytearray()

    def feed(self, data: bytes) -> list[tuple[bytes, int, bytes]]:
        self._buffer.extend(data)
        frames: list[tuple[bytes, int, bytes]] = []
        while len(self._buffer) >= ZMP_HEADER_SIZE:
            if self._buffer[0] != ZMP_MAGIC or self._buffer[1] != ZMP_VERSION:
                raise ValueError("unexpected ZMP frame header")
            flags = self._buffer[2]
            size = int.from_bytes(self._buffer[4:8], "big")
            total = ZMP_HEADER_SIZE + size
            if len(self._buffer) < total:
                break
            raw = bytes(self._buffer[:total])
            del self._buffer[:total]
            frames.append((raw, flags, raw[ZMP_HEADER_SIZE:]))
        return frames


def command_44_identity(body: bytes) -> tuple[str, int, int, int] | None:
    if len(body) < 5 or body[:2] != WIRE_MAGIC or body[3] != SESSION_RELOCATION_ROUTE:
        return None

    offset = 5 + 16

    def skip_text8() -> bytes:
        nonlocal offset
        size = body[offset]
        offset += 1
        value = body[offset : offset + size]
        offset += size
        return value

    def skip_bytes8() -> None:
        skip_text8()

    def skip_text16() -> None:
        nonlocal offset
        size = int.from_bytes(body[offset : offset + 2], "big")
        offset += 2 + size

    try:
        skip_text8()
        offset += 8
        skip_bytes8()
        offset += 8
        skip_text16()
        offset += 1
        actor = skip_text8().decode("utf-8", errors="replace")
        offset += 8
        skip_bytes8()
        offset += 8
        skip_text8()
        offset += 8
        skip_bytes8()
        offset += 8
        action = body[offset]
        offset += 1
        route_size = int.from_bytes(body[offset : offset + 2], "big")
        offset += 2
        if action == 1 and route_size >= 16:
            previous = int.from_bytes(body[offset : offset + 8], "big")
            target = int.from_bytes(body[offset + 8 : offset + 16], "big")
        else:
            previous = 0
            target = 0
        return actor, action, previous, target
    except (IndexError, ValueError):
        return None


def pump(source: socket.socket, sink: socket.socket, direction: str, arm_file: str) -> None:
    parser = FrameParser()
    message = bytearray()
    message_frames = 0
    command_44: tuple[str, int, int, int] | None = None
    try:
        while True:
            data = source.recv(65536)
            if not data:
                break
            for raw, flags, body in parser.feed(data):
                message.extend(raw)
                message_frames += 1
                identity = command_44_identity(body)
                if identity is not None:
                    command_44 = identity
                if flags & ZMP_FLAG_MORE:
                    continue
                blocked = (
                    message_frames == 1
                    and os.path.exists(arm_file)
                    and command_44 is not None
                    and command_44[1] == 1
                )
                if blocked:
                    actor = command_44[0]
                    Path(f"{arm_file}.blocked").touch()
                    print(
                        f"blocked-command-44 direction={direction} "
                        f"actor={actor} action=commit "
                        f"previous-authority={command_44[2]} "
                        f"target-authority={command_44[3]}",
                        flush=True,
                    )
                else:
                    sink.sendall(message)
                message.clear()
                message_frames = 0
                command_44 = None
    except (OSError, ValueError) as error:
        print(f"proxy-pump-ended direction={direction} error={error!r}", flush=True)
    finally:
        try:
            sink.shutdown(socket.SHUT_WR)
        except OSError:
            pass


class Proxy(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, listen: tuple[str, int], target: tuple[str, int], arm_file: str):
        self.target = target
        self.arm_file = arm_file
        super().__init__(listen, Handler)


class Handler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        proxy = self.server
        assert isinstance(proxy, Proxy)
        try:
            upstream = socket.create_connection(proxy.target, timeout=10)
        except OSError:
            return
        upstream.settimeout(None)
        self.request.settimeout(None)
        downstream = threading.Thread(
            target=pump,
            args=(upstream, self.request, "gateway-to-peer", proxy.arm_file),
            daemon=True,
        )
        downstream.start()
        pump(self.request, upstream, "peer-to-gateway", proxy.arm_file)
        downstream.join(timeout=5)
        upstream.close()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen-host", required=True)
    parser.add_argument("--listen-port", required=True, type=int)
    parser.add_argument("--target-host", required=True)
    parser.add_argument("--target-port", required=True, type=int)
    parser.add_argument("--arm-file", required=True)
    args = parser.parse_args()
    with Proxy(
        (args.listen_host, args.listen_port),
        (args.target_host, args.target_port),
        args.arm_file,
    ) as proxy:
        print(
            f"proxy-ready listen={args.listen_host}:{args.listen_port} "
            f"target={args.target_host}:{args.target_port}",
            flush=True,
        )
        proxy.serve_forever()


if __name__ == "__main__":
    main()
