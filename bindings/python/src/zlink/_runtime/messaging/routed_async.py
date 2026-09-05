# SPDX-License-Identifier: MPL-2.0

"""Socket-local pull completion ownership for Core 0.17.0 operations."""

import asyncio
import ctypes
import errno
import socket as _socket
import threading

from ..._native.ffi import (
    ZLINK_COMPLETION_REQUEST,
    ZLINK_COMPLETION_SEND,
    ZLINK_COMPLETION_WRITABLE,
    ZLINK_DONTWAIT,
    ZLINK_PART_FINAL,
    ZLINK_PART_MORE,
    ZLINK_SEND_ADMITTED,
    ZLINK_SEND_TERMINAL,
    ZlinkCompletion,
    ZlinkPollerEvent,
    lib,
)
from ...contracts.errors.codes import ConfigResult, ErrorCode
from ...contracts.errors.errors import (
    ConfigError,
    RecvError,
    RequestError,
    SubmitError,
)
from ...contracts.eventing.codes import PollEventFlag, PollSourceKind
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
        "_native_wait",
        "_captured_completion_id",
        "_captured_context",
        "completion_id",
    )

    def __init__(self, kind, loop=None, *, condition=None):
        self.kind = kind
        self.loop = loop
        # SEND needs an awaitable only after admission actually parks.
        self.future = (
            loop.create_future()
            if loop is not None and kind != ZLINK_COMPLETION_SEND
            else None
        )
        self.condition = threading.Condition() if condition is None else condition
        self._published = False
        self._captured = False
        self._settled = False
        self._detached = False
        self._value = None
        self._error = None
        self._native_wait = False
        self._captured_completion_id = 0
        self._captured_context = 0
        self.completion_id = 0

    @property
    def context(self):
        return id(self)

    def publish(self, completion_id):
        discarded = None
        with self.condition:
            if self._settled:
                return False
            self.completion_id = int(completion_id)
            self._published = True
            self._native_wait = self.completion_id != 0
            if self.kind == ZLINK_COMPLETION_REQUEST and self._captured:
                token_matches = self._captured_completion_id == self.completion_id
                if (
                    not token_matches or self._captured_context != self.context
                ):
                    self._error = RequestError(
                        RequestResult.INTERNAL_ERROR,
                        errno.EPROTO,
                    )
                    discarded = self._value
                    self._value = None
                if token_matches:
                    self._native_wait = False
            deliver = self._settle_if_joined_locked()
        _close_messages(discarded)
        self._deliver(deliver)
        return True

    def fail_submit(self):
        with self.condition:
            if self._settled:
                return
            self._published = True
            self._captured = True
            self._settled = True
            self._native_wait = False
            self.condition.notify_all()
            if self.future is not None and not self.future.done():
                self.future.cancel()

    def succeed_send(self):
        with self.condition:
            if self._settled:
                return
            self._published = True
            self._captured = True
            self._native_wait = False
            deliver = self._settle_if_joined_locked()
        self._deliver(deliver)

    def fail(self, error):
        with self.condition:
            if self._settled:
                return
            self._published = True
            self._captured = True
            self._error = error
            deliver = self._settle_if_joined_locked()
        self._deliver(deliver)

    def await_writable(self, completion_id):
        with self.condition:
            if self._settled:
                return False
            self.completion_id = int(completion_id)
            self._published = True
            self._captured = False
            self._native_wait = True
            return True

    def consume_writable(self, completion_id):
        with self.condition:
            if (
                not self._published
                or self.completion_id != int(completion_id)
            ):
                return False
            self.completion_id = 0
            self._published = False
            self._native_wait = False
            return True

    def retire_native(self, completion_id):
        with self.condition:
            if not self._native_wait or self.completion_id != int(completion_id):
                return False
            self.completion_id = 0
            self._native_wait = False
            return True

    def capture(self, completion):
        value = None
        failure = None
        completion_id = int(completion.completion_id)
        context = int(completion.user_context or 0)
        with self.condition:
            already_captured = self._captured
            correlation_invalid = self._published and (
                completion_id != self.completion_id or context != self.context
            )
        try:
            if already_captured:
                pass
            elif correlation_invalid:
                failure = RequestError(RequestResult.INTERNAL_ERROR, errno.EPROTO)
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
            self._captured_completion_id = completion_id
            self._captured_context = context
            if self._published:
                token_matches = completion_id == self.completion_id
                if not token_matches or context != self.context:
                    failure = RequestError(
                        RequestResult.INTERNAL_ERROR,
                        errno.EPROTO,
                    )
                    discarded = self._value
                    self._value = None
                else:
                    discarded = None
                if token_matches:
                    self._native_wait = False
            else:
                discarded = None
            self._error = failure
            self._captured = True
            deliver = self._settle_if_joined_locked()
        _close_messages(discarded)
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
            self._native_wait = False
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
        with self.condition:
            if self.future is None:
                if self._settled:
                    if self._error is not None:
                        raise self._error
                    return self._value
                self.future = self.loop.create_future()
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

    @property
    def settled(self):
        with self.condition:
            return self._settled

    @property
    def waiting_native(self):
        with self.condition:
            return self._native_wait

    @property
    def releasable(self):
        with self.condition:
            return self._settled and not self._native_wait


class _SendEntry(_CompletionEntry):
    """One managed SEND packet across any number of WRITABLE retries."""

    __slots__ = ("target", "payload")

    def __init__(self, loop, target, native_parts, *, condition=None):
        super().__init__(ZLINK_COMPLETION_SEND, loop, condition=condition)
        self.target = None if target is None else bytes(target)
        self.payload = native_parts

    def clone_payload(self):
        """Clone the retained zlink_msg values for one consuming submit call."""

        clones = []
        with self.condition:
            if self.payload is None:
                return None
            try:
                for native in self.payload:
                    clones.append(_clone_native_msg(native))
            except BaseException:
                for clone in clones:
                    lib().zlink_msg_close(ctypes.byref(clone))
                raise
        return clones

    def _release_payload(self):
        with self.condition:
            native_parts = self.payload
            self.payload = None
            self.target = None
        if native_parts is not None:
            for native in native_parts:
                lib().zlink_msg_close(ctypes.byref(native))

    def succeed_send(self):
        self._release_payload()
        super().succeed_send()

    def fail(self, error):
        self._release_payload()
        super().fail(error)

    def shutdown(self):
        self._release_payload()
        super().shutdown()


class _RequestEntry(_CompletionEntry):
    """One REQUEST while it waits for admission and then its reply."""

    __slots__ = ("target", "payload", "timeout_ms")

    def __init__(self, loop, timeout_ms):
        super().__init__(ZLINK_COMPLETION_REQUEST, loop)
        self.target = None
        self.payload = None
        self.timeout_ms = int(timeout_ms)

    def retain_retry(self, target, payload):
        """Snapshot a refused request for its next consuming submit call."""

        native_parts = _materialize_native_parts(payload)
        try:
            retained_target = None if target is None else bytes(target)
        except BaseException:
            for native in native_parts:
                lib().zlink_msg_close(ctypes.byref(native))
            raise
        with self.condition:
            if self._settled:
                retain = False
            else:
                self.target = retained_target
                self.payload = native_parts
                retain = True
        if not retain:
            for native in native_parts:
                lib().zlink_msg_close(ctypes.byref(native))
        return retain

    def clone_payload(self):
        clones = []
        with self.condition:
            if self.payload is None:
                return None
            try:
                for native in self.payload:
                    clones.append(_clone_native_msg(native))
            except BaseException:
                for clone in clones:
                    lib().zlink_msg_close(ctypes.byref(clone))
                raise
        return clones

    def _release_payload(self):
        with self.condition:
            native_parts = self.payload
            self.payload = None
            self.target = None
        if native_parts is not None:
            for native in native_parts:
                lib().zlink_msg_close(ctypes.byref(native))

    @property
    def waiting_admission(self):
        with self.condition:
            return self.payload is not None

    def publish_request(self, completion_id):
        self._release_payload()
        return super().publish(completion_id)

    def fail(self, error):
        self._release_payload()
        super().fail(error)

    def shutdown(self):
        waiting_admission = self.waiting_admission
        self._release_payload()
        if waiting_admission:
            _CompletionEntry.fail(
                self,
                SubmitError(
                    SubmitResult.TERMINATED,
                    getattr(errno, "ESHUTDOWN", errno.ECANCELED),
                ),
            )
            return
        super().shutdown()


class _DrainResult:
    __slots__ = ("total_count", "request_count")

    def __init__(self, total_count=0, request_count=0):
        self.total_count = int(total_count)
        self.request_count = int(request_count)


class CompletionOwner:
    """Own one socket completion queue and its provisional registry."""

    def __init__(self, socket):
        self._socket = socket
        self._lock = threading.RLock()
        self._state_changed = threading.Condition(self._lock)
        self._drain_lock = threading.Lock()
        self._poll_wait_lock = threading.Lock()
        self._entries = {}
        self._entries_by_id = {}
        self._public_owner = None
        self._runtime_poller = None
        self._runtime_loop = None
        self._runtime_handle = None
        self._wake_reader = None
        self._wake_writer = None
        self._sync_waiters = 0
        self._closing_entries = []
        self._shutdown = False

    def _register(self, entry, *, schedule=False):
        with self._lock:
            if self._shutdown:
                raise SubmitError(
                    SubmitResult.INVALID_STATE,
                    getattr(errno, "ESHUTDOWN", errno.ECANCELED),
                )
            self._entries[entry.context] = entry
            if schedule:
                self._schedule_runtime_owner_locked(entry.loop)

    def _unregister(self, entry):
        with self._lock:
            self._entries.pop(entry.context, None)
            completion_id = int(entry.completion_id)
            if self._entries_by_id.get(completion_id) is entry:
                self._entries_by_id.pop(completion_id, None)
            if not self._entries:
                self._cancel_runtime_callback_locked()
            self._state_changed.notify_all()

    def _track_native_wait_locked(self, entry):
        completion_id = int(entry.completion_id)
        if completion_id == 0:
            return False
        previous = self._entries_by_id.get(completion_id)
        if previous is not None and previous is not entry:
            entry.fail(SubmitError(SubmitResult.INTERNAL_ERROR, errno.EPROTO))
            return False
        self._entries_by_id[completion_id] = entry
        return True

    def _release_native_wait_locked(self, entry, completion_id):
        completion_id = int(completion_id)
        if self._entries_by_id.get(completion_id) is entry:
            self._entries_by_id.pop(completion_id, None)

    def _select_runtime_loop_locked(self, preferred=None):
        candidates = [preferred]
        candidates.extend(entry.loop for entry in self._entries.values())
        for loop in candidates:
            if loop is not None and not loop.is_closed() and loop.is_running():
                return loop
        return None

    def _schedule_runtime_owner_locked(self, preferred=None):
        if self._runtime_handle is not None:
            thread = self._runtime_handle
            loop = self._runtime_loop
            if (
                thread.is_alive()
                and loop is not None
                and not loop.is_closed()
                and loop.is_running()
            ):
                return
            self._cancel_runtime_callback_locked()
        if (
            self._public_owner is not None
            or self._sync_waiters != 0
            or self._shutdown
            or not self._entries
        ):
            return
        loop = self._select_runtime_loop_locked(preferred)
        if loop is None:
            return
        try:
            self._ensure_runtime_poller_locked()
        except Exception:
            self._fail_runtime_wait(lib().zlink_errno())
            return
        self._runtime_loop = loop
        try:
            thread = threading.Thread(
                target=self._runtime_wait_loop,
                name="zlink-python-completion",
                daemon=True,
            )
            self._runtime_handle = thread
            thread.start()
        except RuntimeError:
            self._runtime_loop = None
            self._runtime_handle = None

    def _cancel_runtime_callback_locked(self):
        self._runtime_handle = None
        self._runtime_loop = None
        self._signal_runtime_wait_locked()

    def _ensure_wake_pair_locked(self):
        if self._wake_reader is not None:
            return
        reader, writer = _socket.socketpair()
        reader.setblocking(False)
        writer.setblocking(False)
        self._wake_reader = reader
        self._wake_writer = writer

    def _signal_runtime_wait_locked(self):
        writer = self._wake_writer
        if writer is None:
            return
        try:
            writer.send(b"\0")
        except (BlockingIOError, OSError):
            pass

    def _clear_runtime_wake(self):
        with self._lock:
            reader = self._wake_reader
        if reader is None:
            return
        while True:
            try:
                if not reader.recv(256):
                    return
            except BlockingIOError:
                return
            except OSError:
                return

    def _ensure_runtime_poller_locked(self):
        if self._runtime_poller is not None:
            return
        poller = lib().zlink_poller_new()
        if not poller:
            raise OSError(lib().zlink_errno(), "zlink_poller_new failed")
        self._ensure_wake_pair_locked()
        # Every WRITABLE record also holds POLLCOMPLETION level-ready. Watching
        # only that bit prevents an unrelated socket-wide POLLOUT state from
        # waking a synchronous REQUEST waiter before its completion exists.
        events = int(PollEventFlag.POLLCOMPLETION)
        rc = lib().zlink_poller_add(poller, self._socket._handle, None, events)
        if rc != int(ConfigResult.OK):
            native_errno = lib().zlink_errno()
            handle = ctypes.c_void_p(poller)
            lib().zlink_poller_destroy(ctypes.byref(handle))
            _raise_result_error(ConfigError, ConfigResult, rc, native_errno)
        rc = lib().zlink_poller_add_fd(
            poller,
            self._wake_reader.fileno(),
            None,
            int(PollEventFlag.POLLIN),
        )
        if rc != int(ConfigResult.OK):
            native_errno = lib().zlink_errno()
            handle = ctypes.c_void_p(poller)
            lib().zlink_poller_destroy(ctypes.byref(handle))
            _raise_result_error(ConfigError, ConfigResult, rc, native_errno)
        self._runtime_poller = poller

    def _stop_runtime_owner_locked(self):
        self._cancel_runtime_callback_locked()
        poller = self._runtime_poller
        self._runtime_poller = None
        if poller:
            handle = ctypes.c_void_p(poller)
            lib().zlink_poller_destroy(ctypes.byref(handle))

    def _close_wake_pair_locked(self):
        reader = self._wake_reader
        writer = self._wake_writer
        self._wake_reader = None
        self._wake_writer = None
        for endpoint in (reader, writer):
            if endpoint is not None:
                try:
                    endpoint.close()
                except OSError:
                    pass

    def _fail_runtime_wait(self, native_errno):
        with self._lock:
            entries = list(self._entries.values())
        for entry in entries:
            if entry.kind == ZLINK_COMPLETION_SEND or (
                isinstance(entry, _RequestEntry) and entry.waiting_admission
            ):
                entry.fail(
                    SubmitError(
                        SubmitResult.INTERNAL_ERROR,
                        int(native_errno) or errno.EIO,
                    )
                )
            else:
                entry.fail(
                    RequestError(
                        RequestResult.INTERNAL_ERROR,
                        int(native_errno) or errno.EIO,
                    )
                )
            if entry.releasable:
                self._unregister(entry)
        with self._lock:
            self._state_changed.notify_all()

    def _runtime_wait_loop(self):
        """Block on Core progress; the wake FD handles ownership changes."""

        current = threading.current_thread()
        while True:
            with self._lock:
                if (
                    self._runtime_handle is not current
                    or self._shutdown
                    or self._public_owner is not None
                    or self._sync_waiters != 0
                    or not self._entries
                ):
                    return
            with self._poll_wait_lock:
                with self._lock:
                    if (
                        self._runtime_handle is not current
                        or self._shutdown
                        or self._public_owner is not None
                        or self._sync_waiters != 0
                        or not self._entries
                    ):
                        return
                    poller = self._runtime_poller
                native_event = ZlinkPollerEvent()
                error_out = ctypes.c_int()
                rc = lib().zlink_poller_wait(
                    poller,
                    ctypes.byref(native_event),
                    1,
                    -1,
                    ctypes.byref(error_out),
                )
                native_errno = lib().zlink_errno() if rc < 0 else 0
            with self._lock:
                self._state_changed.notify_all()
            if rc > 0 and int(native_event.source_kind) == int(PollSourceKind.FD):
                self._clear_runtime_wake()
                continue
            if rc > 0:
                try:
                    self.drain()
                except Exception:
                    self._fail_runtime_wait(lib().zlink_errno())
                    return
                continue
            if rc < 0 and native_errno not in (errno.EINTR, errno.EAGAIN):
                self._fail_runtime_wait(native_errno)
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
            self._cancel_runtime_callback_locked()
            self._signal_runtime_wait_locked()
            self._state_changed.notify_all()
        with self._poll_wait_lock:
            with self._lock:
                self._stop_runtime_owner_locked()

    def transfer_to_runtime(self, poller_owner):
        with self._lock:
            if self._public_owner is not poller_owner:
                return
            self._public_owner = None
            self._state_changed.notify_all()
            if self._entries and not self._shutdown:
                self._schedule_runtime_owner_locked()

    def has_managed_writable_wait(self):
        with self._lock:
            return any(
                (
                    isinstance(entry, _SendEntry)
                    or (
                        isinstance(entry, _RequestEntry)
                        and entry.waiting_admission
                    )
                )
                and entry.completion_id != 0
                and entry.waiting_native
                for entry in self._entries.values()
            )

    @staticmethod
    def _completion_target(completion):
        size = int(completion.peer_rid.size)
        if size == 0:
            return None
        return bytes(completion.peer_rid.data[:size])

    @staticmethod
    def _submit_error(result, native_errno):
        try:
            typed_result = SubmitResult(int(result))
        except ValueError:
            typed_result = int(result)
        return SubmitError(typed_result, int(native_errno))

    @staticmethod
    def _terminal_submit_error(native_errno):
        native_errno = int(native_errno) or errno.EIO
        if native_errno == errno.ENOENT:
            result = SubmitResult.NOT_FOUND
        elif native_errno in (
            errno.ECANCELED,
            getattr(errno, "ESHUTDOWN", errno.ECANCELED),
            int(ErrorCode.ETERM),
        ):
            result = SubmitResult.TERMINATED
        else:
            result = SubmitResult.NOT_ADMITTED
        return SubmitError(result, native_errno)

    def _capture_writable(self, entry, completion):
        completion_id = int(completion.completion_id)
        context = int(completion.user_context or 0)
        kind = int(completion.kind)
        target = self._completion_target(completion)
        send_result = int(completion.send_result)
        terminal_errno = int(completion.send_terminal_errno)
        lib().zlink_completion_close(ctypes.byref(completion))

        token_matches = completion_id == entry.completion_id
        if token_matches:
            with self._lock:
                self._release_native_wait_locked(entry, completion_id)
        token_consumed = token_matches and entry.consume_writable(completion_id)
        if not token_consumed:
            entry.fail(SubmitError(SubmitResult.INTERNAL_ERROR, errno.EPROTO))
            return False
        if entry.settled:
            return False
        if (
            kind != ZLINK_COMPLETION_WRITABLE
            or context != entry.context
            or target != entry.target
        ):
            entry.fail(SubmitError(SubmitResult.INTERNAL_ERROR, errno.EPROTO))
            return False
        if send_result == ZLINK_SEND_TERMINAL:
            entry.fail(self._terminal_submit_error(terminal_errno))
            return False
        if send_result != ZLINK_SEND_ADMITTED or terminal_errno != 0:
            entry.fail(SubmitError(SubmitResult.INTERNAL_ERROR, errno.EPROTO))
            return False
        return True

    def _dispatch_retry(self, entry):
        if entry.settled:
            return
        attempt = (
            self._attempt_request
            if isinstance(entry, _RequestEntry)
            else self._attempt_send
        )
        loop = entry.loop
        if loop is None or getattr(loop, "_thread_id", None) == threading.get_ident():
            attempt(entry)
            return
        try:
            loop.call_soon_threadsafe(attempt, entry)
        except RuntimeError as error:
            entry.fail(error)
            self._unregister(entry)

    def drain(self, caller=None):
        with self._drain_lock:
            return self._drain(caller)

    def _drain(self, caller=None):
        processed = 0
        request_count = 0
        retry_entries = []
        while True:
            with self._lock:
                if self._public_owner is not None and self._public_owner is not caller:
                    break
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
            completion_kind = int(completion.kind)
            completion_id = int(completion.completion_id)
            context = int(completion.user_context or 0)
            with self._lock:
                entry = self._entries_by_id.get(completion_id)
                if entry is None:
                    entry = self._entries.get(context)
            if entry is None:
                lib().zlink_completion_close(ctypes.byref(completion))
            elif entry.settled and entry.waiting_native:
                lib().zlink_completion_close(ctypes.byref(completion))
                if entry.retire_native(completion_id):
                    with self._lock:
                        self._release_native_wait_locked(entry, completion_id)
                    self._unregister(entry)
                if completion_kind == ZLINK_COMPLETION_REQUEST:
                    request_count += 1
            elif completion_kind == ZLINK_COMPLETION_WRITABLE and isinstance(
                entry, (_SendEntry, _RequestEntry)
            ):
                if self._capture_writable(entry, completion):
                    retry_entries.append(entry)
                if entry.releasable:
                    self._unregister(entry)
            else:
                with self._lock:
                    if completion_id == entry.completion_id:
                        self._release_native_wait_locked(entry, completion_id)
                entry.capture(completion)
                if completion_kind == ZLINK_COMPLETION_REQUEST:
                    request_count += 1
                if entry.releasable:
                    self._unregister(entry)
            processed += 1
            with self._lock:
                self._state_changed.notify_all()
        for entry in retry_entries:
            self._dispatch_retry(entry)
        return _DrainResult(processed, request_count)

    @staticmethod
    def _close_unsubmitted(native_parts, start=0):
        for native in native_parts[start:]:
            lib().zlink_msg_close(ctypes.byref(native))

    def _submit_parts(self, target, native_parts, flags, entry=None, timeout_ms=None):
        try:
            native_rid = None if target is None else _copy_routing_id(target)
        except BaseException:
            self._close_unsubmitted(native_parts)
            raise
        completion_id = ctypes.c_uint64(0)
        part_count = len(native_parts)
        for index, native in enumerate(native_parts):
            final = index == part_count - 1
            part_flag = ZLINK_PART_FINAL if final else ZLINK_PART_MORE
            context = ctypes.c_void_p(entry.context) if final and entry is not None else None
            completion_out = ctypes.byref(completion_id) if final and entry is not None else None
            try:
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
            except BaseException:
                self._close_unsubmitted(native_parts, index)
                raise
            if rc != int(SubmitResult.OK):
                native_errno = lib().zlink_errno()
                self._close_unsubmitted(native_parts, index)
                return int(rc), native_errno, int(completion_id.value)
        return int(SubmitResult.OK), 0, int(completion_id.value)

    def _attempt_send(self, entry):
        with self._lock:
            if self._shutdown or entry.settled:
                return
        try:
            native_parts = entry.clone_payload()
        except Exception as error:
            with self._lock:
                if self._shutdown or entry.settled:
                    return
            entry.fail(error)
            self._unregister(entry)
            return
        if native_parts is None:
            return
        with self._lock:
            if self._shutdown or entry.settled:
                self._close_unsubmitted(native_parts)
                return
            try:
                rc, native_errno, completion_id = self._submit_parts(
                    entry.target, native_parts, ZLINK_DONTWAIT, entry
                )
            except BaseException as error:
                entry.fail(error)
                self._unregister(entry)
                return
            # Admission and token publication share the drain owner's lock.
            # A successful SEND never enters either completion registry.
            if completion_id != 0:
                self._entries[entry.context] = entry
            if rc == int(SubmitResult.OK):
                if completion_id != 0:
                    entry.await_writable(completion_id)
                    self._track_native_wait_locked(entry)
                    entry.fail(SubmitError(SubmitResult.INTERNAL_ERROR, errno.EPROTO))
                else:
                    entry.succeed_send()
            elif rc == int(SubmitResult.BACKPRESSURED):
                if completion_id != 0:
                    entry.await_writable(completion_id)
                    self._track_native_wait_locked(entry)
                if native_errno != errno.EAGAIN or completion_id == 0:
                    entry.fail(SubmitError(SubmitResult.INTERNAL_ERROR, errno.EPROTO))
            else:
                if completion_id != 0:
                    entry.await_writable(completion_id)
                    self._track_native_wait_locked(entry)
                entry.fail(self._submit_error(rc, native_errno))

            if entry.releasable:
                if self._entries.get(entry.context) is entry:
                    self._unregister(entry)
            else:
                self._schedule_runtime_owner_locked(entry.loop)

    def _attempt_request(self, entry):
        with self._lock:
            if self._shutdown or entry.settled:
                return
        try:
            native_parts = entry.clone_payload()
        except Exception as error:
            with self._lock:
                if self._shutdown or entry.settled:
                    return
            entry.fail(error)
            self._unregister(entry)
            return
        if native_parts is None:
            return
        with self._lock:
            if self._shutdown or entry.settled:
                self._close_unsubmitted(native_parts)
                return
            try:
                rc, native_errno, completion_id = self._submit_parts(
                    entry.target,
                    native_parts,
                    ZLINK_DONTWAIT,
                    entry,
                    entry.timeout_ms,
                )
            except BaseException as error:
                entry.fail(error)
                self._unregister(entry)
                return
            if rc == int(SubmitResult.OK):
                if completion_id == 0:
                    entry.fail(SubmitError(SubmitResult.INTERNAL_ERROR, errno.EPROTO))
                else:
                    entry.publish_request(completion_id)
                    self._track_native_wait_locked(entry)
            elif rc == int(SubmitResult.BACKPRESSURED):
                if completion_id != 0:
                    entry.await_writable(completion_id)
                    self._track_native_wait_locked(entry)
                if native_errno != errno.EAGAIN or completion_id == 0:
                    entry.fail(SubmitError(SubmitResult.INTERNAL_ERROR, errno.EPROTO))
            else:
                if completion_id != 0:
                    entry.await_writable(completion_id)
                    self._track_native_wait_locked(entry)
                entry.fail(self._submit_error(rc, native_errno))

            if entry.releasable:
                self._unregister(entry)
            else:
                self._schedule_runtime_owner_locked(entry.loop)

    async def submit_send(self, target, payload):
        native_parts = _materialize_native_parts(payload)
        entry = _SendEntry(
            asyncio.get_running_loop(),
            target,
            native_parts,
            condition=self._state_changed,
        )
        with self._lock:
            if self._shutdown:
                entry._release_payload()
                raise SubmitError(
                    SubmitResult.INVALID_STATE,
                    getattr(errno, "ESHUTDOWN", errno.ECANCELED),
                )
            self._attempt_send(entry)
        await entry.wait_async()

    def submit_send_sync(self, target, payload):
        native_parts = _materialize_native_parts(payload)
        rc, native_errno, completion_id = self._submit_parts(target, native_parts, 0)
        if rc != int(SubmitResult.OK):
            _raise_result_error(SubmitError, SubmitResult, rc, native_errno)
        if completion_id != 0:
            raise SubmitError(SubmitResult.INTERNAL_ERROR, errno.EPROTO)

    def _finish_request_submit(self, entry, completion_id, *, schedule):
        if not entry.publish(completion_id):
            return
        with self._lock:
            if self._shutdown or self._entries.get(entry.context) is not entry:
                return
            if entry.releasable:
                self._unregister(entry)
                return
            self._track_native_wait_locked(entry)
            if schedule:
                self._schedule_runtime_owner_locked(entry.loop)

    async def submit_request(self, target, payload, timeout_ms):
        native_parts = _materialize_native_parts(payload)
        entry = _RequestEntry(asyncio.get_running_loop(), timeout_ms)
        with self._lock:
            if self._shutdown:
                self._close_unsubmitted(native_parts)
                raise SubmitError(
                    SubmitResult.INVALID_STATE,
                    getattr(errno, "ESHUTDOWN", errno.ECANCELED),
                )
            self._entries[entry.context] = entry
            try:
                rc, native_errno, completion_id = self._submit_parts(
                    target, native_parts, ZLINK_DONTWAIT, entry, int(timeout_ms)
                )
            except BaseException:
                entry.fail_submit()
                self._unregister(entry)
                raise
            if rc == int(SubmitResult.OK):
                if completion_id == 0:
                    entry.fail_submit()
                    self._unregister(entry)
                    raise SubmitError(SubmitResult.INTERNAL_ERROR, errno.EPROTO)
                self._finish_request_submit(entry, completion_id, schedule=True)
            elif rc == int(SubmitResult.BACKPRESSURED):
                if completion_id != 0:
                    entry.await_writable(completion_id)
                    self._track_native_wait_locked(entry)
                if native_errno != errno.EAGAIN or completion_id == 0:
                    entry.fail(SubmitError(SubmitResult.INTERNAL_ERROR, errno.EPROTO))
                else:
                    try:
                        entry.retain_retry(target, payload)
                    except BaseException as error:
                        entry.fail(error)
                if entry.releasable:
                    self._unregister(entry)
                else:
                    self._schedule_runtime_owner_locked(entry.loop)
            else:
                if completion_id != 0:
                    entry.await_writable(completion_id)
                    self._track_native_wait_locked(entry)
                    entry.fail(self._submit_error(rc, native_errno))
                    self._schedule_runtime_owner_locked(entry.loop)
                else:
                    entry.fail_submit()
                    self._unregister(entry)
                    _raise_result_error(SubmitError, SubmitResult, rc, native_errno)
        return await entry.wait_async()

    def submit_request_sync(self, target, payload, timeout_ms):
        native_parts = _materialize_native_parts(payload)
        entry = _CompletionEntry(ZLINK_COMPLETION_REQUEST)
        self._register(entry)
        try:
            rc, native_errno, completion_id = self._submit_parts(
                target, native_parts, 0, entry, int(timeout_ms)
            )
        except BaseException:
            entry.fail_submit()
            self._unregister(entry)
            raise
        if rc != int(SubmitResult.OK):
            entry.fail_submit()
            self._unregister(entry)
            _raise_result_error(SubmitError, SubmitResult, rc, native_errno)
        if completion_id == 0:
            entry.fail_submit()
            self._unregister(entry)
            raise SubmitError(SubmitResult.INTERNAL_ERROR, errno.EPROTO)
        self._finish_request_submit(entry, completion_id, schedule=False)
        return self._wait_request(entry)

    def _wait_request(self, entry):
        with self._lock:
            self._sync_waiters += 1
            self._cancel_runtime_callback_locked()
        try:
            while not entry.settled:
                with self._state_changed:
                    while (
                        self._public_owner is not None
                        and not self._shutdown
                        and not entry.settled
                    ):
                        self._state_changed.wait()
                    if self._shutdown or entry.settled:
                        continue
                    if not self._poll_wait_lock.acquire(blocking=False):
                        self._state_changed.wait()
                        continue

                native_event = ZlinkPollerEvent()
                error_out = ctypes.c_int()
                try:
                    with self._lock:
                        if (
                            self._shutdown
                            or self._public_owner is not None
                            or entry.settled
                        ):
                            continue
                        self._ensure_runtime_poller_locked()
                        poller = self._runtime_poller
                    rc = lib().zlink_poller_wait(
                        poller,
                        ctypes.byref(native_event),
                        1,
                        -1,
                        ctypes.byref(error_out),
                    )
                    if rc > 0:
                        if int(native_event.source_kind) == int(PollSourceKind.FD):
                            self._clear_runtime_wake()
                        else:
                            self.drain()
                    elif rc < 0 and lib().zlink_errno() != errno.EINTR:
                        self._fail_runtime_wait(lib().zlink_errno())
                except Exception:
                    self._fail_runtime_wait(lib().zlink_errno())
                finally:
                    self._poll_wait_lock.release()
                    with self._lock:
                        self._state_changed.notify_all()
        finally:
            with self._lock:
                self._sync_waiters -= 1
                self._state_changed.notify_all()
                self._schedule_runtime_owner_locked()
        return entry.wait_request()

    def shutdown(self):
        with self._lock:
            if self._shutdown:
                return
            self._shutdown = True
            self._public_owner = None
            self._cancel_runtime_callback_locked()
            self._signal_runtime_wait_locked()
            entries = list(self._entries.values())
            self._closing_entries = entries
            self._entries.clear()
            self._entries_by_id.clear()
            self._state_changed.notify_all()
        for entry in entries:
            entry.shutdown()
        with self._lock:
            self._state_changed.notify_all()
        with self._poll_wait_lock:
            with self._lock:
                self._stop_runtime_owner_locked()
                self._close_wake_pair_locked()

    def finish_shutdown(self):
        """Release native user-context roots after the socket is discarded."""

        with self._lock:
            self._closing_entries.clear()


__all__ = ["CompletionOwner"]
