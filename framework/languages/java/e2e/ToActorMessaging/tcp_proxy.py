#!/usr/bin/env python3
"""Forward one actor mesh endpoint until the runner deliberately stops it."""

import argparse
import select
import signal
import socket
import threading


stop_requested = threading.Event()
listener = None


def stop(_signum, _frame):
    stop_requested.set()
    if listener is not None:
        try:
            listener.close()
        except OSError:
            pass


def forward(client, upstream_host, upstream_port):
    upstream = None
    try:
        upstream = socket.create_connection((upstream_host, upstream_port), timeout=2)
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen-host", required=True)
    parser.add_argument("--listen-port", required=True, type=int)
    parser.add_argument("--upstream-host", required=True)
    parser.add_argument("--upstream-port", required=True, type=int)
    options = parser.parse_args()

    global listener
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((options.listen_host, options.listen_port))
    listener.listen(16)
    listener.settimeout(0.5)
    print("proxy-ready", flush=True)
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


if __name__ == "__main__":
    main()
