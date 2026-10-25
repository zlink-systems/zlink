#!/usr/bin/env python3
"""Run the public MeshNode request probe with one MeshNode per process."""

import queue
import subprocess
import sys
import threading
import time

RUNTIME_TIMEOUT_SECONDS = 25
PROCESS_TIMEOUT_SECONDS = 5


def main():
    binary = sys.argv[1]
    events = queue.Queue()
    children = {}
    readers = []
    deadline = time.monotonic() + RUNTIME_TIMEOUT_SECONDS

    def start(name, arguments):
        process = subprocess.Popen(
            [binary, *arguments],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        children[name] = process

        def read():
            for line in process.stdout:
                events.put((name, line.rstrip("\r\n")))
            events.put((name, None))

        reader = threading.Thread(target=read)
        reader.start()
        readers.append(reader)
        return process

    def send(name, command):
        process = children[name]
        process.stdin.write(command + "\n")
        process.stdin.flush()

    failed = False
    try:
        start("server", ["--server"])
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("public request processes exceeded their deadline")
            name, line = events.get(timeout=remaining)
            if line is None:
                code = children[name].wait(timeout=PROCESS_TIMEOUT_SECONDS)
                if name == "server":
                    raise RuntimeError(f"server exited before client completion: {code}")
                if code != 0:
                    raise RuntimeError(f"public request client failed: {code}")
                break
            print(f"{name}: {line}", flush=True)
            if name == "server" and line.startswith("B3_SERVER_ENDPOINT="):
                if "client" in children:
                    raise RuntimeError("server published its endpoint more than once")
                start("client", ["--client", line.split("=", 1)[1]])
            elif name == "client" and line == "B3_CLIENT_ARM":
                send("server", "arm")
            elif name == "server" and line == "B3_SERVER_ARMED":
                send("client", "armed")
            elif name == "server" and line == "B3_SERVER_ENTERED":
                send("client", "entered")
            elif name == "client" and line == "B3_CLIENT_RELEASE":
                send("server", "release")
    except (OSError, RuntimeError, TimeoutError, queue.Empty) as error:
        print(str(error) or "public request coordination timed out", file=sys.stderr)
        failed = True
    finally:
        # Stop releases any pending application gate before joining the host.
        # Even a failing client must leave no test process or pipe reader alive.
        server = children.get("server")
        if server is not None and server.poll() is None:
            try:
                send("server", "cleanup")
                send("server", "stop")
                print("server: cooperative cleanup and stop requested", flush=True)
            except (BrokenPipeError, OSError):
                pass
        client = children.get("client")
        if client is not None and client.poll() is None:
            try:
                send("client", "stop")
            except (BrokenPipeError, OSError):
                pass
        for name, process in children.items():
            try:
                process.stdin.close()
            except (BrokenPipeError, OSError) as error:
                failed = True
                print(f"{name}: stdin cleanup failed: {error}", file=sys.stderr)
        for name, process in children.items():
            try:
                code = process.wait(timeout=PROCESS_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                failed = True
                print(f"{name}: cooperative cleanup exceeded its deadline", file=sys.stderr)
                code = process.wait()
            if code != 0:
                failed = True
            print(f"{name}: exit={code}", flush=True)
        for reader in readers:
            reader.join(timeout=PROCESS_TIMEOUT_SECONDS)
            if reader.is_alive():
                failed = True
                print("pipe reader exceeded its cleanup deadline", file=sys.stderr)
                reader.join()
        for name, process in children.items():
            try:
                process.stdout.close()
            except OSError as error:
                failed = True
                print(f"{name}: stdout cleanup failed: {error}", file=sys.stderr)
        print(f"cleanup: children_reaped={len(children)} readers_joined={len(readers)}", flush=True)
        while not events.empty():
            name, line = events.get_nowait()
            if line is not None:
                print(f"{name}: {line}", flush=True)
    if not failed:
        for mode in ("stop", "destroy"):
            try:
                with subprocess.Popen(
                    [binary, f"--classification-{mode}"],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                ) as process:
                    try:
                        output, _ = process.communicate(
                            timeout=max(0, deadline - time.monotonic())
                        )
                    except subprocess.TimeoutExpired:
                        failed = True
                        print(f"classification-{mode}: exceeded the runtime deadline", file=sys.stderr)
                        # The fixture releases its Store latch before joining its host.
                        # Reap that cooperative cleanup without terminating the child.
                        output, _ = process.communicate()
                    for line in output.splitlines():
                        print(f"classification-{mode}: {line}", flush=True)
                    print(f"classification-{mode}: exit={process.returncode}", flush=True)
                    failed |= process.returncode != 0
            except OSError as error:
                failed = True
                print(f"classification-{mode}: {error}", file=sys.stderr)
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
