#!/usr/bin/env python3
"""Kill a process group as soon as an appended file marker is observable."""

from __future__ import annotations

import argparse
import ctypes
import errno
import os
import select
import signal
import struct
import time


IN_MODIFY = 0x00000002
EVENT_HEADER = struct.Struct("iIII")


def kill_process_group(pid: int) -> None:
    os.killpg(os.getpgid(pid), signal.SIGKILL)


def read_from_offset(path: str, offset: int) -> tuple[int, str]:
    with open(path, "r", encoding="utf-8") as stream:
        stream.seek(0, os.SEEK_END)
        length = stream.tell()
        if length < offset:
            offset = 0
        stream.seek(offset)
        return stream.tell(), stream.read()


def poll_for_marker(
    pid: int,
    path: str,
    marker: str,
    offset: int,
    deadline: float,
) -> None:
    while time.monotonic() < deadline:
        try:
            offset, appended = read_from_offset(path, offset)
        except FileNotFoundError:
            time.sleep(0.01)
            continue
        if marker in appended:
            kill_process_group(pid)
            return
        time.sleep(0.01)
    raise SystemExit(f"marker was not observed: {marker}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--path", required=True)
    parser.add_argument("--marker", required=True)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()

    deadline = time.monotonic() + args.timeout
    while not os.path.exists(args.path):
        if time.monotonic() >= deadline:
            raise SystemExit("marker file was not created")
        time.sleep(0.01)

    libc = ctypes.CDLL("libc.so.6", use_errno=True)
    descriptor = libc.inotify_init1(os.O_CLOEXEC)
    if descriptor < 0:
        raise OSError(ctypes.get_errno(), "inotify_init1 failed")
    try:
        path = os.fsencode(args.path)
        watch = libc.inotify_add_watch(descriptor, path, IN_MODIFY)
        if watch < 0:
            error = ctypes.get_errno()
            if error not in (errno.ENOSPC, errno.EMFILE, errno.ENOSYS):
                raise OSError(error, "inotify_add_watch failed")
            # Editors can consume the process-wide inotify watch budget. The
            # marker is still a regular file, so bounded polling preserves
            # the process-level fault injection without requiring a private
            # runtime hook or a system-wide watch-limit change.
            os.close(descriptor)
            descriptor = -1
            with open(args.path, "r", encoding="utf-8") as stream:
                existing = stream.read()
                offset = stream.tell()
            if args.marker in existing:
                kill_process_group(args.pid)
                return
            poll_for_marker(
                args.pid,
                args.path,
                args.marker,
                offset,
                deadline)
            return
        # Install the watch before reading existing content. This covers both
        # a marker written before startup and one appended during the read.
        with open(args.path, "r", encoding="utf-8") as stream:
            existing = stream.read()
            offset = stream.tell()
        if args.marker in existing:
            kill_process_group(args.pid)
            return
        while time.monotonic() < deadline:
            timeout = max(0, deadline - time.monotonic())
            readable, _, _ = select.select([descriptor], [], [], timeout)
            if not readable:
                break
            events = os.read(descriptor, 4096)
            cursor = 0
            while cursor + EVENT_HEADER.size <= len(events):
                _, _, _, name_length = EVENT_HEADER.unpack_from(
                    events, cursor)
                cursor += EVENT_HEADER.size + name_length
            with open(args.path, "r", encoding="utf-8") as stream:
                stream.seek(offset)
                appended = stream.read()
                offset = stream.tell()
            if args.marker not in appended:
                continue
            kill_process_group(args.pid)
            return
    finally:
        if descriptor >= 0:
            os.close(descriptor)
    raise SystemExit(f"marker was not observed: {args.marker}")


if __name__ == "__main__":
    main()
