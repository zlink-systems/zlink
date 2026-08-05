# SPDX-License-Identifier: MPL-2.0
"""Shared callback dispatcher.

Replaces per-handler ``threading.Thread`` instances with a single worker
thread that multiplexes every callback for one socket or monitor. The
native side enqueues a 0-arg task and the dispatcher thread invokes it
under the ``_enter_callback``/``_leave_callback`` guard so reentry checks
behave identically to the dedicated-thread form.
"""

from __future__ import annotations

import queue
import sys
import threading
import traceback


class CallbackDispatcher:
    """One worker thread serves callbacks for all handler types of one owner.

    The thread is started lazily on first ``submit`` and stopped on
    ``close``. ``close`` is idempotent and safe to call from any thread
    other than the dispatcher's own worker.
    """

    __slots__ = (
        "_queue",
        "_thread",
        "_name",
        "_closed",
        "_enter",
        "_leave",
        "_state_lock",
    )

    def __init__(self, name, enter_callback, leave_callback):
        self._queue = queue.SimpleQueue()
        self._thread = None
        self._name = name
        self._closed = False
        self._enter = enter_callback
        self._leave = leave_callback
        self._state_lock = threading.Lock()

    def submit(self, task):
        with self._state_lock:
            if self._closed:
                return False
            if self._thread is None:
                self._thread = threading.Thread(
                    target=self._run, name=self._name, daemon=True
                )
                self._thread.start()
            self._queue.put(task)
            return True

    def close(self):
        with self._state_lock:
            if not self._closed:
                self._closed = True
                self._queue.put(None)
            thread = self._thread
        if (
            thread is not None
            and thread.is_alive()
            and thread is not threading.current_thread()
        ):
            thread.join(timeout=1.0)
        with self._state_lock:
            if self._thread is thread and (thread is None or not thread.is_alive()):
                self._thread = None

    @property
    def closed(self):
        return self._closed

    def _run(self):
        enter = self._enter
        leave = self._leave
        get = self._queue.get
        try:
            while True:
                task = get()
                if task is None:
                    return
                entered = False
                try:
                    enter()
                    entered = True
                    task()
                except Exception:
                    # Callback wrappers normally report user exceptions. This
                    # path keeps dispatcher failures visible instead of
                    # silently dropping a task or corrupting callback depth.
                    traceback.print_exc(file=sys.stderr)
                finally:
                    if entered:
                        leave()
        finally:
            with self._state_lock:
                if self._thread is threading.current_thread():
                    self._thread = None
