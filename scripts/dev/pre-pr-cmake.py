#!/usr/bin/env python3
"""Timestamp local CMake work while leaving slot acquisition to the host wrapper."""

import os
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ci"))
from framework_timing import event, timed


def main():
    cmake = os.environ["ZLINK_PRE_PR_CMAKE"]
    args = sys.argv[1:]
    if args[:1] == ["--preset"]:
        with timed("Framework C++ configure"):
            return subprocess.call([cmake, *args])
    if args[:1] != ["--build"]:
        os.execv(cmake, [cmake, *args])
    label = "Framework C++ build" if args[1:2] == ["--preset"] else f"CMake build ({Path.cwd()})"
    if Path(cmake).resolve() != (Path.home() / "zlink-locks/bin/cmake").resolve():
        with timed(label):
            return subprocess.call([cmake, *args])

    # The slot wrapper starts the real CMake only after acquiring a slot.
    # Verbose CMake prints its working directory before starting the build tool.
    pos = args.index("--") if "--" in args else len(args)
    args.insert(pos, "--verbose")
    waiting = time.monotonic()
    event("C++ build slot", "wait-start")
    started = None
    with subprocess.Popen(["stdbuf", "-oL", cmake, *args], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT) as process:
        for line in process.stdout:
            if started is None and line.startswith(b"Change Dir:"):
                event("C++ build slot", "acquired", waiting)
                started = time.monotonic()
                event(label, "start")
            sys.stdout.buffer.write(line)
            sys.stdout.buffer.flush()
    if started is None:
        event("C++ build slot", "wait-end", waiting)
    else:
        event(label, "end", started)
    return process.returncode


if __name__ == "__main__":
    sys.exit(main())
