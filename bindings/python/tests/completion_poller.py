import threading

import zlink


class CompletionPollerDriver:
    """Drive one public completion owner for async contract tests."""

    def __init__(self, socket):
        self._poller = zlink.create_poller()
        self._events = zlink.create_poll_events(1)
        self._stop = threading.Event()
        self._failure = None
        self._poller.add_socket(
            socket, zlink.PollEventFlag.POLLCOMPLETION, 1
        )
        self._thread = threading.Thread(
            target=self._run,
            name="zlink-test-completion-poller",
        )

    def _run(self):
        try:
            while not self._stop.is_set():
                self._poller.wait(self._events, 50)
        except BaseException as failure:
            self._failure = failure

    def __enter__(self):
        self._thread.start()
        return self

    def __exit__(self, exc_type, exc, tb):
        self._stop.set()
        self._thread.join(2)
        try:
            if self._thread.is_alive():
                raise AssertionError("completion poller thread did not stop")
            if self._failure is not None and exc_type is None:
                raise self._failure
        finally:
            self._poller.close()
