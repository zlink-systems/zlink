#!/usr/bin/env python3
"""Run connector examples against one isolated server on Linux."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
from queue import Empty, Queue
import signal
import subprocess
from threading import Thread

BUNDLE = Path(__file__).resolve().parents[2] / "framework/templates/connector-examples"
CLIENTS = {
    "typescript": [
        ["npm", "ci"],
        ["npm", "run", "build:browser"],
        ["npm", "run", "e2e"],
    ],
    "csharp": [
        ["dotnet", "build", "-m:2"],
        ["dotnet", "run", "--no-build"],
    ],
    "java": [["./gradlew", "--no-daemon", "--max-workers=2", "run"]],
}


def read_server_output(
    server: subprocess.Popen[str], ready: Queue[str | None],
) -> None:
    assert server.stdout is not None
    for line in server.stdout:
        print(line, end="", flush=True)
        if line.startswith("READY "):
            ready.put(line.removeprefix("READY ").strip())
    ready.put(None)


def run(languages: list[str]) -> None:
    ready: Queue[str | None] = Queue()
    with subprocess.Popen(
        ["./run-server.sh"],
        cwd=BUNDLE / "server",
        env={**os.environ, "CONNECTOR_EXAMPLE_BIND": "ws://127.0.0.1:0"},
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        encoding="utf-8",
        errors="replace",
        start_new_session=True,
    ) as server:
        reader = Thread(target=read_server_output, args=(server, ready))
        reader.start()
        try:
            try:
                endpoint = ready.get(timeout=120)
            except Empty as error:
                raise RuntimeError("Server did not report READY within 120 seconds") from error
            if endpoint is None:
                raise RuntimeError(f"Server exited before READY (exit {server.wait()})")
            env = {**os.environ, "CONNECTOR_EXAMPLE_ENDPOINT": endpoint}
            for language in languages:
                print(f"Running {language} against {endpoint}", flush=True)
                for command in CLIENTS[language]:
                    subprocess.run(command, cwd=BUNDLE / "client" / language, env=env, check=True)
        finally:
            if server.poll() is None:
                try:
                    os.killpg(server.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            try:
                status = server.wait(timeout=20)
            except subprocess.TimeoutExpired:
                os.killpg(server.pid, signal.SIGKILL)
                server.wait()
                raise
            finally:
                reader.join()
        if status:
            raise RuntimeError(f"Server exited with status {status}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("language", nargs="*", metavar="LANGUAGE", help=", ".join(CLIENTS))
    args = parser.parse_args()
    unknown = set(args.language) - CLIENTS.keys()
    if unknown:
        parser.error(f"Unknown client(s): {', '.join(sorted(unknown))}")
    run(args.language or list(CLIENTS))


if __name__ == "__main__":
    main()
