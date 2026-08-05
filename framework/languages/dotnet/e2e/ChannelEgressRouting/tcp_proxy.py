#!/usr/bin/env python3
"""Forward one TCP endpoint until the runner deliberately stops the proxy."""

from __future__ import annotations

import argparse
import select
import signal
import socket
import sys
import threading


stop_requested = threading.Event()
listener: socket.socket | None = None


def stop(_signum: int, _frame: object) -> None:
    stop_requested.set()
    if listener is not None:
        try:
            listener.close()
        except OSError:
            pass


def forward(client: socket.socket, upstream_host: str, upstream_port: int) -> None:
    upstream: socket.socket | None = None
    try:
        upstream = socket.create_connection((upstream_host, upstream_port), timeout=2)
        client.setblocking(False)
        upstream.setblocking(False)
        sockets = (client, upstream)
        while not stop_requested.is_set():
            readable, _, _ = select.select(sockets, [], [], 0.5)
            for source in readable:
                payload = source.recv(64 * 1024)
                if not payload:
                    return
                destination = upstream if source is client else client
                destination.sendall(payload)
    except (ConnectionError, OSError):
        return
    finally:
        try:
            client.close()
        except OSError:
            pass
        if upstream is not None:
            try:
                upstream.close()
            except OSError:
                pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen-host", required=True)
    parser.add_argument("--listen-port", required=True, type=int)
    parser.add_argument("--upstream-host", required=True)
    parser.add_argument("--upstream-port", required=True, type=int)
    return parser.parse_args()


def main() -> int:
    global listener
    options = parse_args()
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((options.listen_host, options.listen_port))
    listener.listen(16)
    listener.settimeout(0.5)
    print(
        f"proxy ready listen={options.listen_host}:{options.listen_port} "
        f"upstream={options.upstream_host}:{options.upstream_port}",
        flush=True,
    )
    try:
        while not stop_requested.is_set():
            try:
                client, _ = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                if stop_requested.is_set():
                    break
                raise
            threading.Thread(
                target=forward,
                args=(client, options.upstream_host, options.upstream_port),
                daemon=True,
            ).start()
    finally:
        listener.close()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
