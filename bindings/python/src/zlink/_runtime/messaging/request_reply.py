# SPDX-License-Identifier: MPL-2.0

import ctypes
import errno
import threading
import traceback

from ...contracts.errors.errors import SubmitError
from ...contracts.sockets.codes import RequestResult, SubmitResult
from ..handles.native_support import (
    _REPLY_HANDLER,
    _clone_native_msg,
    _copy_routing_id,
    _report_unhandled_callback_exception,
    _request_result_from_code,
    _raise_result_error,
    _routing_id_bytes,
)
from .message_materializer import Message
from .native_parts import _materialize_native_parts
from ..sockets.socket_base import _enter_callback, _leave_callback
from ..._native.ffi import ZlinkPollerEvent, lib


_ERRNO_ENOTSUP = getattr(errno, "ENOTSUP", getattr(errno, "EOPNOTSUPP", 95))


def _ensure_reply_flags_supported(flags):
    if int(flags) != 0:
        raise SubmitError(SubmitResult.NOT_SUPPORTED, _ERRNO_ENOTSUP)


def _timeout_to_ms(timeout):
    if timeout in (None, 0):
        return 0
    return max(1, int(float(timeout) * 1000))


_clone_payload = _materialize_native_parts


def _message_list_from_parts(parts_ptr, part_count):
    messages = []
    for index in range(int(part_count)):
        msg = Message.__new__(Message)
        msg._msg = _clone_native_msg(parts_ptr[index])
        msg._valid = True
        msg._keepalive = None
        messages.append(msg)
    return messages


class _PendingRequest:
    def __init__(self, *, callback=None):
        self.callback = callback

    def resolve(self, result, received):
        if self.callback is None:
            return

        try:
            _enter_callback()
            try:
                self.callback(result, received if result == RequestResult.OK else [])
            finally:
                _leave_callback()
        except Exception:
            _report_unhandled_callback_exception(self.callback)


_POLL_COMPLETION = 32  # ZLINK_POLLCOMPLETION


class _RequestProgressPump:
    """Per-handle background worker that drains request completions.

    Uses the canonical poller-based progress model:
    a dedicated zlink_poller is registered with the handle under the
    ZLINK_POLLCOMPLETION flag. zlink_poller_wait() blocks until reply
    completions are available and drains them internally; the worker only
    needs to wake the wait loop when handles complete.
    """

    def __init__(self, handle_getter, is_active, on_failure):
        self._handle_getter = handle_getter
        self._is_active = is_active
        self._on_failure = on_failure
        self._lock = threading.Lock()
        self._thread = None
        self._stop_event = threading.Event()

    def ensure_running(self):
        with self._lock:
            if self._thread is not None and self._thread.is_alive():
                return
            self._stop_event.clear()
            self._thread = threading.Thread(
                target=self._run,
                name="zlink-request-progress",
                daemon=True,
            )
            self._thread.start()

    def stop(self):
        """Stop the poller before its owning socket is closed.

        A close retry may restart the worker when Core rejects the close with
        ``BUSY``. A worker that cannot join is treated as a lifecycle error so
        the native handle is never closed while its poller still references it.
        """

        self._stop_event.set()
        with self._lock:
            thread = self._thread
        if thread is None or thread is threading.current_thread():
            return
        thread.join(timeout=1.0)
        if thread.is_alive():
            raise RuntimeError("request progress worker did not stop")

    def _run(self):
        try:
            handle = self._handle_getter()
            if not handle:
                return
            poller = lib().zlink_poller_new()
            if not poller:
                self._notify_failure()
                return
            poller_added = False
            try:
                rc = lib().zlink_poller_add(
                    poller,
                    handle,
                    None,
                    ctypes.c_short(_POLL_COMPLETION),
                )
                if rc != 0:
                    self._notify_failure()
                    return
                poller_added = True
                events = (ZlinkPollerEvent * 1)()
                error_out = ctypes.c_int()
                # Use a finite wait timeout so the worker can observe
                # _is_active() turning false (e.g. when the owning socket
                # closes and cancels its pending requests).
                while not self._stop_event.is_set() and self._is_active():
                    try:
                        wait_rc = lib().zlink_poller_wait(
                            poller, events, 1, 50, ctypes.byref(error_out)
                        )
                        if wait_rc < 0:
                            self._notify_failure()
                            break
                    except Exception:
                        self._notify_failure()
                        break
            finally:
                if poller_added:
                    lib().zlink_poller_remove(poller, handle)
                handle_ptr = ctypes.c_void_p(poller)
                lib().zlink_poller_destroy(ctypes.byref(handle_ptr))
        finally:
            with self._lock:
                if self._thread is threading.current_thread():
                    self._thread = None

    def _notify_failure(self):
        if self._stop_event.is_set() or not self._is_active():
            return
        try:
            self._on_failure()
        except Exception:
            traceback.print_exc()
