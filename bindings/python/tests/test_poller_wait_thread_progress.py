# SPDX-License-Identifier: MPL-2.0

from concurrent.futures import ThreadPoolExecutor
import socket
import threading

import zlink


def test_poller_wait_allows_python_thread_to_make_fd_ready():
    read, write = socket.socketpair()
    waiting = threading.Event()

    def make_ready():
        assert waiting.wait(1)
        write.sendall(b"x")

    with read, write, zlink.create_poller() as poller, ThreadPoolExecutor(1) as worker:
        poller.add_fd(read.fileno(), zlink.PollEventFlag.POLLIN, 7)
        events = zlink.create_poll_events(1)
        ready = worker.submit(make_ready)
        waiting.set()
        assert poller.wait(events, 1000) == 1
        assert events.event(0).slot == 7
        assert read.recv(1) == b"x"
        ready.result()
