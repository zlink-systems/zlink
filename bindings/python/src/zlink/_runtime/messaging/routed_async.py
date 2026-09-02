# SPDX-License-Identifier: MPL-2.0

"""Socket-local pull completion ownership for Core 0.16.0 operations."""

import asyncio
import ctypes
import errno
import threading

from ..._native.ffi import (
    ZLINK_COMPLETION_REQUEST,
    ZLINK_COMPLETION_SEND,
    ZLINK_DONTWAIT,
    ZLINK_PART_FINAL,
    ZLINK_PART_MORE,
    ZLINK_SEND_ADMITTED,
    ZlinkCompletion,
    ZlinkPollerEvent,
    lib,
)
from ...contracts.errors.codes import ConfigResult
from ...contracts.errors.errors import (
    ConfigError,
    RecvError,
    RequestError,
    SubmitError,
)
from ...contracts.sockets.codes import RecvResult, RequestResult, SubmitResult
from ..handles.native_support import (
    _clone_native_msg,
    _copy_routing_id,
    _raise_result_error,
    _request_result_from_code,
)
from .message_materializer import Message
from .native_parts import _materialize_native_parts


def _close_messages(messages):
    if not isinstance(messages, list):
        return
    for message in messages:
        try:
            message.close()
        except Exception:
            pass


def _request_errno(result):
    return {
        RequestResult.TIMED_OUT: errno.ETIMEDOUT,
        RequestResult.NOT_FOUND: errno.ENOENT,
        RequestResult.TERMINATED: getattr(errno, "ESHUTDOWN", errno.ECANCELED),
        RequestResult.PROTOCOL_ERROR: errno.EPROTO,
        RequestResult.REJECTED: errno.EACCES,
        RequestResult.CONFLICT: getattr(errno, "ESTALE", errno.EIO),
        RequestResult.BUSY: errno.EBUSY,
        RequestResult.NOT_CONNECTED: errno.ENOTCONN,
        RequestResult.INVALID_ARGUMENT: errno.EINVAL,
        RequestResult.INVALID_STATE: errno.EBUSY,
        RequestResult.NOT_SUPPORTED: errno.ENOTSUP,
        RequestResult.BACKPRESSURED: errno.EAGAIN,
    }.get(result, errno.EIO)


class _CompletionEntry:
    """Join native submit publication with one captured completion."""

    __slots__ = (
        "kind",
        "loop",
        "future",
        "condition",
        "_published",
        "_captured",
        "_settled",
        "_detached",
        "_value",
        "_error",
        "completion_id",
    )

    def __init__(self, kind, loop=None):
        self.kind = kind
        self.loop = loop
        self.future = None if loop is None else loop.create_future()
        self.condition = threading.Condition()
        self._published = False
        self._captured = False
        self._settled = False
        self._detached = False
        self._value = None
        self._error = None
        self.completion_id = 0

    @property
    def context(self):
        return id(self)

    def publish(self, completion_id):
        with self.condition:
            self.completion_id = int(completion_id)
            self._published = True
            deliver = self._settle_if_joined_locked()
        self._deliver(deliver)

    def fail_submit(self):
        with self.condition:
            self._published = True
            self._captured = True
            self._settled = True
            self.condition.notify_all()

    def capture_inline_send(self):
        with self.condition:
            self._captured = True
            deliver = self._settle_if_joined_locked()
        self._deliver(deliver)

    def capture(self, completion):
        value = None
        failure = None
        try:
            if self.kind == ZLINK_COMPLETION_SEND:
                if (
                    int(completion.kind) != ZLINK_COMPLETION_SEND
                    or int(completion.send_result) != ZLINK_SEND_ADMITTED
                ):
                    native_errno = int(completion.send_terminal_errno) or errno.EIO
                    failure = SubmitError(SubmitResult.NOT_ADMITTED, native_errno)
            elif int(completion.kind) != ZLINK_COMPLETION_REQUEST:
                failure = RequestError(RequestResult.INTERNAL_ERROR, errno.EPROTO)
            else:
                result = _request_result_from_code(int(completion.request_result))
                if result != RequestResult.OK:
                    failure = RequestError(result, _request_errno(result))
                else:
                    value = []
                    try:
                        for index in range(int(completion.reply_part_count)):
                            message = Message.__new__(Message)
                            message._msg = _clone_native_msg(completion.reply_parts[index])
                            message._valid = True
                            message._keepalive = None
                            value.append(message)
                    except BaseException:
                        _close_messages(value)
                        value = None
                        failure = RequestError(RequestResult.INTERNAL_ERROR, errno.EIO)
        finally:
            lib().zlink_completion_close(ctypes.byref(completion))

        with self.condition:
            if self._captured:
                _close_messages(value)
                return
            self._value = value
            self._error = failure
            self._captured = True
            deliver = self._settle_if_joined_locked()
        self._deliver(deliver)

    def shutdown(self):
        native_errno = getattr(errno, "ESHUTDOWN", errno.ECANCELED)
        error = (
            SubmitError(SubmitResult.TERMINATED, native_errno)
            if self.kind == ZLINK_COMPLETION_SEND
            else RequestError(RequestResult.TERMINATED, native_errno)
        )
        with self.condition:
            if self._settled:
                return
            self._published = True
            self._captured = True
            self._error = error
            deliver = self._settle_if_joined_locked()
        self._deliver(deliver)

    def _settle_if_joined_locked(self):
        if not self._published or not self._captured or self._settled:
            return None
        self._settled = True
        self.condition.notify_all()
        return self._value, self._error, self._detached

    def _deliver(self, deliver):
        if deliver is None or self.future is None:
            return
        value, error, detached = deliver
        if detached:
            _close_messages(value)
            return

        def finish():
            with self.condition:
                detached_now = self._detached
            if detached_now or self.future.done():
                _close_messages(value)
                return
            if error is not None:
                self.future.set_exception(error)
            else:
                self.future.set_result(value)

        if getattr(self.loop, "_thread_id", None) == threading.get_ident():
            finish()
            return
        try:
            self.loop.call_soon_threadsafe(finish)
        except RuntimeError:
            _close_messages(value)

    def detach(self):
        value = None
        with self.condition:
            self._detached = True
            if self.future is not None and self.future.done() and not self.future.cancelled():
                try:
                    if self.future.exception() is None:
                        value = self.future.result()
                except BaseException:
                    pass
        _close_messages(value)

    async def wait_async(self):
        try:
            return await asyncio.shield(self.future)
        except asyncio.CancelledError:
            self.detach()
            raise

    def wait_request(self):
        with self.condition:
            while not self._settled:
                self.condition.wait()
            if self._error is not None:
                raise self._error
            value = self._value
            self._value = None
            return value

    def wait_settled(self):
        with self.condition:
            while not self._settled:
                self.condition.wait()


class CompletionOwner:
    """Own one socket completion queue and its provisional registry."""

    def __init__(self, socket):
        self._socket = socket
        self._lock = threading.RLock()
        self._entries = {}
        self._public_owner = None
        self._runtime_poller = None
        self._runtime_thread = None
        self._runtime_stop = False
        self._shutdown = False

    def _register(self, entry):
        with self._lock:
            if self._shutdown:
                raise SubmitError(
                    SubmitResult.INVALID_STATE,
                    getattr(errno, "ESHUTDOWN", errno.ECANCELED),
                )
            self._entries[entry.context] = entry
            if self._public_owner is None:
                self._start_runtime_owner_locked()

    def _unregister(self, entry):
        with self._lock:
            self._entries.pop(entry.context, None)

    def _start_runtime_owner_locked(self):
        if self._runtime_poller or self._runtime_thread or self._shutdown:
            return
        poller = lib().zlink_poller_new()
        if not poller:
            raise OSError(lib().zlink_errno(), "zlink_poller_new failed")
        rc = lib().zlink_poller_add(poller, self._socket._handle, None, 32)
        if rc != int(ConfigResult.OK):
            native_errno = lib().zlink_errno()
            handle = ctypes.c_void_p(poller)
            lib().zlink_poller_destroy(ctypes.byref(handle))
            _raise_result_error(ConfigError, ConfigResult, rc, native_errno)
        self._runtime_poller = poller
        self._runtime_stop = False
        thread = threading.Thread(
            target=self._runtime_loop,
            name="zlink-completion-drain",
            daemon=True,
        )
        self._runtime_thread = thread
        thread.start()

    def _detach_runtime_owner_locked(self):
        self._runtime_stop = True
        thread = self._runtime_thread
        poller = self._runtime_poller
        self._runtime_thread = None
        self._runtime_poller = None
        return thread, poller

    @staticmethod
    def _finish_runtime_owner(thread, poller):
        if thread is not None and thread is not threading.current_thread():
            thread.join()
        if poller:
            handle = ctypes.c_void_p(poller)
            lib().zlink_poller_destroy(ctypes.byref(handle))

    def _runtime_loop(self):
        while True:
            with self._lock:
                if self._runtime_stop or self._shutdown:
                    return
                poller = self._runtime_poller
            native_event = ZlinkPollerEvent()
            error_out = ctypes.c_int()
            rc = lib().zlink_poller_wait(
                poller, ctypes.byref(native_event), 1, 25, ctypes.byref(error_out)
            )
            if rc > 0:
                try:
                    self.drain(wait_for_publish=False)
                except Exception:
                    return
            elif rc < 0 and lib().zlink_errno() not in (errno.EINTR, errno.EAGAIN):
                return

    def transfer_to_public(self, poller_owner):
        with self._lock:
            if self._shutdown:
                raise ConfigError(
                    ConfigResult.INVALID_STATE,
                    getattr(errno, "ESHUTDOWN", errno.ECANCELED),
                )
            if self._public_owner is not None and self._public_owner is not poller_owner:
                raise ConfigError(ConfigResult.INVALID_STATE, errno.EBUSY)
            if self._public_owner is poller_owner:
                return
            self._public_owner = poller_owner
            thread, poller = self._detach_runtime_owner_locked()
        self._finish_runtime_owner(thread, poller)

    def transfer_to_runtime(self, poller_owner):
        with self._lock:
            if self._public_owner is not poller_owner:
                return
            self._public_owner = None
            if self._entries and not self._shutdown:
                self._start_runtime_owner_locked()

    def drain(self, wait_for_publish):
        processed = 0
        while True:
            completion = ZlinkCompletion()
            completion.struct_size = ctypes.sizeof(ZlinkCompletion)
            rc = lib().zlink_completion_recv(
                self._socket._handle,
                ctypes.byref(completion),
                ZLINK_DONTWAIT,
            )
            if rc == int(RecvResult.NO_DATA):
                break
            if rc != int(RecvResult.OK):
                _raise_result_error(RecvError, RecvResult, rc, lib().zlink_errno())
            context = int(completion.user_context or 0)
            with self._lock:
                entry = self._entries.get(context)
            if entry is None:
                lib().zlink_completion_close(ctypes.byref(completion))
            else:
                entry.capture(completion)
                if wait_for_publish:
                    entry.wait_settled()
                self._unregister(entry)
            processed += 1
        return processed

    @staticmethod
    def _close_unsubmitted(native_parts, start=0):
        for native in native_parts[start:]:
            lib().zlink_msg_close(ctypes.byref(native))

    def _submit_parts(self, target, native_parts, flags, entry=None, timeout_ms=None):
        native_rid = None if target is None else _copy_routing_id(target)
        completion_id = ctypes.c_uint64(0)
        part_count = len(native_parts)
        for index, native in enumerate(native_parts):
            final = index == part_count - 1
            part_flag = ZLINK_PART_FINAL if final else ZLINK_PART_MORE
            context = ctypes.c_void_p(entry.context) if final and entry is not None else None
            completion_out = ctypes.byref(completion_id) if final and entry is not None else None
            if timeout_ms is not None:
                rc = lib().zlink_request_part(
                    self._socket._handle,
                    None if native_rid is None else ctypes.byref(native_rid),
                    ctypes.byref(native),
                    flags,
                    part_flag,
                    int(timeout_ms) if final else 0,
                    context,
                    completion_out,
                )
            elif native_rid is None:
                rc = lib().zlink_send_part(
                    self._socket._handle,
                    ctypes.byref(native),
                    flags,
                    part_flag,
                    context,
                    completion_out,
                )
            else:
                rc = lib().zlink_send_part_rid(
                    self._socket._handle,
                    ctypes.byref(native_rid),
                    ctypes.byref(native),
                    flags,
                    part_flag,
                    context,
                    completion_out,
                )
            if rc != int(SubmitResult.OK):
                native_errno = lib().zlink_errno()
                self._close_unsubmitted(native_parts, index)
                return int(rc), native_errno, 0
        return int(SubmitResult.OK), 0, int(completion_id.value)

    async def submit_send(self, target, payload):
        native_parts = _materialize_native_parts(payload)
        entry = _CompletionEntry(ZLINK_COMPLETION_SEND, asyncio.get_running_loop())
        self._register(entry)
        rc, native_errno, completion_id = self._submit_parts(
            target, native_parts, ZLINK_DONTWAIT, entry
        )
        if rc != int(SubmitResult.OK):
            entry.fail_submit()
            self._unregister(entry)
            _raise_result_error(SubmitError, SubmitResult, rc, native_errno)
        entry.publish(completion_id)
        if completion_id == 0:
            entry.capture_inline_send()
            self._unregister(entry)
        await entry.wait_async()

    def submit_send_sync(self, target, payload):
        native_parts = _materialize_native_parts(payload)
        rc, native_errno, _ = self._submit_parts(target, native_parts, 0)
        if rc != int(SubmitResult.OK):
            _raise_result_error(SubmitError, SubmitResult, rc, native_errno)

    async def submit_request(self, target, payload, timeout_ms):
        native_parts = _materialize_native_parts(payload)
        entry = _CompletionEntry(ZLINK_COMPLETION_REQUEST, asyncio.get_running_loop())
        self._register(entry)
        rc, native_errno, completion_id = self._submit_parts(
            target, native_parts, ZLINK_DONTWAIT, entry, int(timeout_ms)
        )
        if rc != int(SubmitResult.OK):
            entry.fail_submit()
            self._unregister(entry)
            _raise_result_error(SubmitError, SubmitResult, rc, native_errno)
        if completion_id == 0:
            entry.fail_submit()
            self._unregister(entry)
            raise SubmitError(SubmitResult.INTERNAL_ERROR, errno.EPROTO)
        entry.publish(completion_id)
        return await entry.wait_async()

    def submit_request_sync(self, target, payload, timeout_ms):
        native_parts = _materialize_native_parts(payload)
        entry = _CompletionEntry(ZLINK_COMPLETION_REQUEST)
        self._register(entry)
        rc, native_errno, completion_id = self._submit_parts(
            target, native_parts, 0, entry, int(timeout_ms)
        )
        if rc != int(SubmitResult.OK):
            entry.fail_submit()
            self._unregister(entry)
            _raise_result_error(SubmitError, SubmitResult, rc, native_errno)
        if completion_id == 0:
            entry.fail_submit()
            self._unregister(entry)
            raise SubmitError(SubmitResult.INTERNAL_ERROR, errno.EPROTO)
        entry.publish(completion_id)
        return entry.wait_request()

    def shutdown(self):
        with self._lock:
            if self._shutdown:
                return
            self._shutdown = True
            thread, poller = self._detach_runtime_owner_locked()
            entries = list(self._entries.values())
            self._entries.clear()
        self._finish_runtime_owner(thread, poller)
        for entry in entries:
            entry.shutdown()


__all__ = ["CompletionOwner"]
