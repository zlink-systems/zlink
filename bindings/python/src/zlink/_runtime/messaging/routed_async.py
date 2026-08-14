# SPDX-License-Identifier: MPL-2.0

"""Exact-target coroutine admission for DEALER and ROUTER operations."""

import asyncio
import ctypes
import errno
import threading
import time
from collections import deque

from ..._native.ffi import (
    ZLINK_DONTWAIT,
    ZLINK_PART_FINAL,
    ZLINK_PART_MORE,
    ZlinkPollerEvent,
    ZlinkRoutedSendReadyEvent,
    ZlinkRoutedSubmitTarget,
    lib,
)
from ...contracts.errors.errors import HandlerError, RequestError, SubmitError
from ...contracts.sockets.codes import (
    HandlerResult,
    RequestResult,
    SocketType,
    SubmitResult,
)
from ..handles.native_support import (
    _REPLY_HANDLER,
    _clone_native_msg,
    _close_multipart,
    _copy_routing_id,
    _raise_result_error,
    _request_result_from_code,
)
from .message_materializer import Message
from .native_parts import _materialize_native_parts


_ROUTED_SEND_WRITABLE = 1
_ROUTED_SEND_TERMINAL = 2
_POLL_COMPLETION = 32
_ETERM = 156384765

_ROUTED_SEND_READY_HANDLER = ctypes.CFUNCTYPE(
    None,
    ctypes.c_void_p,
    ctypes.POINTER(ZlinkRoutedSendReadyEvent),
    ctypes.c_void_p,
)


def _finish_future(future, value, is_error):
    if future.done():
        if not is_error and isinstance(value, list):
            for message in value:
                message.close()
        return
    if is_error:
        future.set_exception(value)
    else:
        future.set_result(value)


def _schedule_future(loop, future, value=None, *, is_error=False):
    try:
        loop.call_soon_threadsafe(_finish_future, future, value, is_error)
    except RuntimeError:
        if not is_error and isinstance(value, list):
            for message in value:
                message.close()


def _terminal_submit_result(native_errno):
    if native_errno in (
        errno.ECANCELED,
        getattr(errno, "ESHUTDOWN", -1),
        _ETERM,
    ):
        return SubmitResult.TERMINATED
    if native_errno in (errno.ENOENT, getattr(errno, "EHOSTUNREACH", -1)):
        return SubmitResult.NOT_FOUND
    return SubmitResult.NOT_CONNECTED


def _close_native_parts(native_parts, start=0):
    for native in native_parts[start:]:
        lib().zlink_msg_close(ctypes.byref(native))


class _OwnedPayload:
    """Stable native snapshot cloned for each complete-record attempt."""

    def __init__(self, payload):
        self._parts = _materialize_native_parts(payload)

    def clone_for_attempt(self):
        clones = []
        try:
            for part in self._parts:
                clones.append(_clone_native_msg(part))
        except Exception:
            _close_native_parts(clones)
            raise
        return clones

    def close(self):
        parts = self._parts
        self._parts = ()
        _close_native_parts(parts)


class _TargetState:
    __slots__ = ("epoch", "operations", "terminal")

    def __init__(self):
        self.epoch = 0
        self.operations = deque()
        self.terminal = None


class _AdmissionTicket:
    __slots__ = (
        "key",
        "loop",
        "state",
        "future",
        "terminal",
        "finished",
    )

    def __init__(self, key, loop):
        self.key = key
        self.loop = loop
        self.state = "queued"
        self.future = None
        self.terminal = None
        self.finished = False


class _RequestCompletion:
    __slots__ = (
        "token",
        "loop",
        "future",
        "lock",
        "accepted",
        "detached",
        "terminal",
    )

    def __init__(self, token, loop):
        self.token = token
        self.loop = loop
        self.future = loop.create_future()
        self.lock = threading.Lock()
        self.accepted = False
        self.detached = False
        self.terminal = False

    def mark_accepted(self):
        with self.lock:
            self.accepted = True

    def detach(self):
        with self.lock:
            self.detached = True
            future = self.future
        if not future.done():
            future.cancel()

    def resolve(self, value=None, error=None):
        with self.lock:
            if self.terminal:
                already_terminal = True
            else:
                already_terminal = False
                self.terminal = True
            detached = self.detached
            future = self.future
        if already_terminal or detached:
            if error is None and isinstance(value, list):
                for message in value:
                    message.close()
            return
        _schedule_future(
            self.loop,
            future,
            error if error is not None else value,
            is_error=error is not None,
        )


class _RequestCompletionProgress:
    """Drive admitted Core request completions, never HWM admission waits."""

    def __init__(self, owner):
        self._owner = owner
        self._lock = threading.Lock()
        self._thread = None
        self._stop = threading.Event()

    def ensure_running(self):
        with self._lock:
            if self._thread is not None and self._thread.is_alive():
                return
            self._stop.clear()
            self._thread = threading.Thread(
                target=self._run,
                name="zlink-request-completion",
                daemon=True,
            )
            thread = self._thread
        thread.start()

    def stop(self):
        self._stop.set()
        with self._lock:
            thread = self._thread
        if thread is None or thread is threading.current_thread():
            return
        thread.join(timeout=1.0)
        if thread.is_alive():
            raise RuntimeError("request completion worker did not stop")

    def _run(self):
        poller = None
        handle = self._owner.native_handle()
        try:
            if not handle:
                return
            poller = lib().zlink_poller_new()
            if not poller:
                self._owner.fail_request_completions()
                return
            rc = lib().zlink_poller_add(
                poller,
                handle,
                None,
                ctypes.c_short(_POLL_COMPLETION),
            )
            if rc != 0:
                self._owner.fail_request_completions()
                return
            events = (ZlinkPollerEvent * 1)()
            error_out = ctypes.c_int()
            while (
                not self._stop.is_set()
                and self._owner.has_request_completions()
            ):
                rc = lib().zlink_poller_wait(
                    poller,
                    events,
                    1,
                    50,
                    ctypes.byref(error_out),
                )
                if rc < 0:
                    self._owner.fail_request_completions()
                    break
        except Exception:
            self._owner.fail_request_completions()
        finally:
            if poller:
                try:
                    lib().zlink_poller_remove(poller, handle)
                finally:
                    poller_ptr = ctypes.c_void_p(poller)
                    lib().zlink_poller_destroy(ctypes.byref(poller_ptr))
            with self._lock:
                if self._thread is threading.current_thread():
                    self._thread = None
            if not self._stop.is_set() and self._owner.has_request_completions():
                self.ensure_running()


class RoutedAdmissionOwner:
    """Socket-local exact-target admission and request-completion owner."""

    def __init__(
        self,
        socket,
        role,
        complete_record_attempt_gate,
        read_send_timeout_ms,
        read_request_timeout_ms,
    ):
        if role not in (SocketType.DEALER, SocketType.ROUTER):
            raise ValueError("routed admission requires DEALER or ROUTER")
        self._socket = socket
        self._role = role
        self._attempt_gate = complete_record_attempt_gate
        self._read_send_timeout_ms = read_send_timeout_ms
        self._read_request_timeout_ms = read_request_timeout_ms
        self._selection_gate = threading.Lock()
        self._state_lock = threading.RLock()
        self._targets = {}
        self._request_completions = {}
        self._next_request_token = 1
        self._closing = False
        self._closed = False
        self._deferred_ready_events = []
        self._reply_handler = _REPLY_HANDLER(self._on_request_reply)
        self._ready_handler = _ROUTED_SEND_READY_HANDLER(self._on_ready)
        self._progress = _RequestCompletionProgress(self)

        rc = lib().zlink_routed_send_ready_handler(
            self.native_handle(), self._ready_handler, None
        )
        if rc != int(HandlerResult.OK):
            _raise_result_error(
                HandlerError,
                HandlerResult,
                rc,
                lib().zlink_errno(),
            )

    def native_handle(self):
        return self._socket._handle

    @staticmethod
    def _target_key(target):
        rid = bytes(target.peer_rid.data[: target.peer_rid.size])
        return (
            rid,
            int(target.transport_pair_id),
            int(target.transport_pair_generation),
        )

    @staticmethod
    def _event_value(event):
        rid = bytes(event.peer_rid.data[: event.peer_rid.size])
        return (
            (
                rid,
                int(event.transport_pair_id),
                int(event.transport_pair_generation),
            ),
            int(event.state),
            int(event.terminal_errno),
        )

    def _on_ready(self, _subject, event_ptr, _userdata):
        if not event_ptr:
            return
        event = self._event_value(event_ptr.contents)
        with self._state_lock:
            if self._closed:
                return
            if self._closing:
                self._deferred_ready_events.append(event)
                return
            self._apply_ready_event_locked(*event)

    def _apply_ready_event_locked(self, key, state_value, terminal_errno):
        state = self._targets.get(key)
        if state is None:
            return
        if state_value == _ROUTED_SEND_TERMINAL:
            terminal = (_terminal_submit_result(terminal_errno), terminal_errno)
            state.terminal = terminal
            for ticket in tuple(state.operations):
                ticket.terminal = terminal
                self._wake_ticket_locked(ticket)
            return
        if state_value != _ROUTED_SEND_WRITABLE:
            return
        state.epoch += 1
        if state.operations:
            ticket = state.operations[0]
            if ticket.state == "waiting":
                ticket.state = "ready"
                self._wake_ticket_locked(ticket)

    def _wake_ticket_locked(self, ticket):
        future = ticket.future
        ticket.future = None
        if future is not None:
            _schedule_future(ticket.loop, future)

    def _resolve_timeouts_and_target(self, router_rid, request_timeout, started):
        with self._selection_gate:
            with self._state_lock:
                if self._closing or self._closed or not self.native_handle():
                    raise SubmitError(SubmitResult.INVALID_STATE, errno.ECANCELED)
            if request_timeout is None:
                timeout_ms = int(self._read_send_timeout_ms())
            elif request_timeout == 0:
                timeout_ms = int(self._read_request_timeout_ms())
                if timeout_ms <= 0:
                    timeout_ms = 5000
            else:
                timeout_ms = int(request_timeout)
            target = ZlinkRoutedSubmitTarget()
            native_rid = None if router_rid is None else _copy_routing_id(router_rid)
            rc = lib().zlink_select_routed_submit_target(
                self.native_handle(),
                None if native_rid is None else ctypes.byref(native_rid),
                ctypes.byref(target),
            )
            if rc != int(SubmitResult.OK):
                _raise_result_error(
                    SubmitError,
                    SubmitResult,
                    rc,
                    lib().zlink_errno(),
                )
        deadline = None if timeout_ms < 0 else started + (timeout_ms / 1000.0)
        return target, deadline, timeout_ms

    def _register(self, target, loop):
        key = self._target_key(target)
        ticket = _AdmissionTicket(key, loop)
        with self._state_lock:
            if self._closing or self._closed:
                raise SubmitError(SubmitResult.INVALID_STATE, errno.ECANCELED)
            state = self._targets.get(key)
            if state is None:
                state = _TargetState()
                self._targets[key] = state
            if state.terminal is not None:
                result, native_errno = state.terminal
                raise SubmitError(result, native_errno)
            state.operations.append(ticket)
            ticket.state = "ready" if len(state.operations) == 1 else "queued"
        return ticket

    def _try_begin(self, ticket):
        with self._state_lock:
            if ticket.terminal is not None:
                result, native_errno = ticket.terminal
                raise SubmitError(result, native_errno)
            if ticket.finished:
                raise SubmitError(SubmitResult.INVALID_STATE, 0)
            state = self._targets.get(ticket.key)
            if state is None or not state.operations:
                raise SubmitError(SubmitResult.INVALID_STATE, 0)
            if self._closing:
                ticket.state = "waiting"
                return None
            if state.operations[0] is ticket and ticket.state == "ready":
                ticket.state = "attempting"
                return state.epoch
            if ticket.future is None:
                ticket.future = ticket.loop.create_future()
            return None

    async def _wait_ready(self, ticket, deadline, timeout_error):
        with self._state_lock:
            if ticket.terminal is not None:
                result, native_errno = ticket.terminal
                raise SubmitError(result, native_errno)
            if ticket.future is None:
                ticket.future = ticket.loop.create_future()
            future = ticket.future
        if deadline is None:
            await asyncio.shield(future)
            return
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise timeout_error
        try:
            await asyncio.wait_for(asyncio.shield(future), remaining)
        except asyncio.TimeoutError:
            raise timeout_error

    def _backpressured(self, ticket, observed_epoch):
        with self._state_lock:
            if ticket.terminal is not None:
                result, native_errno = ticket.terminal
                raise SubmitError(result, native_errno)
            state = self._targets.get(ticket.key)
            if state is None:
                raise SubmitError(SubmitResult.INVALID_STATE, 0)
            ticket.state = "ready" if state.epoch != observed_epoch else "waiting"

    def _finish(self, ticket):
        with self._state_lock:
            terminal = ticket.terminal
            state = self._targets.get(ticket.key)
            was_head = bool(state and state.operations and state.operations[0] is ticket)
            if state is not None:
                try:
                    state.operations.remove(ticket)
                except ValueError:
                    pass
                if not state.operations:
                    self._targets.pop(ticket.key, None)
                elif was_head and state.terminal is None:
                    next_ticket = state.operations[0]
                    next_ticket.state = "ready"
                    self._wake_ticket_locked(next_ticket)
            ticket.finished = True
            future = ticket.future
            ticket.future = None
            if future is not None:
                future.cancel()
            return terminal

    def _cancel(self, ticket):
        if not ticket.finished:
            self._finish(ticket)

    def _native_attempt(self, attempt):
        with self._attempt_gate:
            with self._state_lock:
                if self._closing or self._closed or not self.native_handle():
                    return int(SubmitResult.TERMINATED), errno.ECANCELED
            return attempt(self.native_handle())

    def run_sync_outbound_attempt(self, attempt):
        rc, native_errno = self._native_attempt(attempt)
        if rc != int(SubmitResult.OK):
            _raise_result_error(
                SubmitError,
                SubmitResult,
                rc,
                native_errno,
            )

    @staticmethod
    def _submit_parts(native_parts, submit_part):
        part_count = len(native_parts)
        for index, native in enumerate(native_parts):
            flag = ZLINK_PART_FINAL if index == part_count - 1 else ZLINK_PART_MORE
            rc = submit_part(ctypes.byref(native), flag, index == part_count - 1)
            if rc != int(SubmitResult.OK):
                native_errno = lib().zlink_errno()
                _close_native_parts(native_parts, index + 1)
                return int(rc), native_errno
        return int(SubmitResult.OK), 0

    def _attempt_send(self, target, payload):
        native_parts = payload.clone_for_attempt()

        def attempt(handle):
            if self._role == SocketType.DEALER:
                return self._submit_parts(
                    native_parts,
                    lambda part, flag, _final: lib().zlink_dealer_send_transport_pair_part(
                        handle,
                        ctypes.byref(target),
                        part,
                        ZLINK_DONTWAIT,
                        flag,
                    ),
                )
            return self._submit_parts(
                native_parts,
                lambda part, flag, _final: lib().zlink_send_part_transport_pair(
                    handle,
                    ctypes.byref(target.peer_rid),
                    target.transport_pair_id,
                    target.transport_pair_generation,
                    part,
                    ZLINK_DONTWAIT,
                    flag,
                ),
            )

        return self._native_attempt(attempt)

    async def submit_send(self, router_rid, payload):
        started = time.monotonic()
        loop = asyncio.get_running_loop()
        owned_payload = _OwnedPayload(payload)
        ticket = None
        try:
            target, deadline, timeout_ms = self._resolve_timeouts_and_target(
                router_rid, request_timeout=None, started=started
            )
            ticket = self._register(target, loop)
            attempted = False
            timeout_error = SubmitError(
                SubmitResult.BACKPRESSURED, errno.ETIMEDOUT
            )
            attempt_before_expiry = ticket.state == "ready" and timeout_ms == 0
            while True:
                if (
                    deadline is not None
                    and time.monotonic() >= deadline
                    and (attempted or not attempt_before_expiry)
                ):
                    self._finish(ticket)
                    raise timeout_error
                observed_epoch = self._try_begin(ticket)
                if observed_epoch is None:
                    await self._wait_ready(ticket, deadline, timeout_error)
                    continue
                rc, native_errno = self._attempt_send(target, owned_payload)
                attempted = True
                if rc == int(SubmitResult.OK):
                    terminal = self._finish(ticket)
                    if terminal is not None:
                        result, terminal_errno = terminal
                        raise SubmitError(result, terminal_errno)
                    return None
                if rc != int(SubmitResult.BACKPRESSURED):
                    self._finish(ticket)
                    raise SubmitError(rc, native_errno)
                if deadline is not None and time.monotonic() >= deadline:
                    self._finish(ticket)
                    raise timeout_error
                self._backpressured(ticket, observed_epoch)
        except asyncio.CancelledError:
            if ticket is not None:
                self._cancel(ticket)
            raise
        finally:
            if ticket is not None and not ticket.finished:
                self._cancel(ticket)
            owned_payload.close()

    def _new_request_completion(self, loop):
        with self._state_lock:
            token = self._next_request_token
            self._next_request_token += 1
        return _RequestCompletion(token, loop)

    def _arm_request(self, completion):
        with self._state_lock:
            self._request_completions[completion.token] = completion

    def _disarm_request(self, completion):
        with self._state_lock:
            current = self._request_completions.get(completion.token)
            if current is completion and not completion.accepted:
                self._request_completions.pop(completion.token, None)

    def _accept_request(self, completion):
        completion.mark_accepted()
        with self._state_lock:
            active = self._request_completions.get(completion.token) is completion
        if active:
            self._progress.ensure_running()

    def _attempt_request(self, target, payload, timeout_ms, completion):
        native_parts = payload.clone_for_attempt()
        self._arm_request(completion)

        def attempt(handle):
            if self._role == SocketType.DEALER:
                return self._submit_parts(
                    native_parts,
                    lambda part, flag, final: lib().zlink_dealer_request_transport_pair_part(
                        handle,
                        ctypes.byref(target),
                        part,
                        ZLINK_DONTWAIT,
                        flag,
                        timeout_ms if final else 0,
                        self._reply_handler if final else None,
                        ctypes.c_void_p(completion.token) if final else None,
                    ),
                )
            return self._submit_parts(
                native_parts,
                lambda part, flag, final: lib().zlink_router_request_transport_pair_part(
                    handle,
                    ctypes.byref(target.peer_rid),
                    target.transport_pair_id,
                    target.transport_pair_generation,
                    part,
                    ZLINK_DONTWAIT,
                    flag,
                    timeout_ms if final else 0,
                    self._reply_handler if final else None,
                    ctypes.c_void_p(completion.token) if final else None,
                ),
            )

        result = self._native_attempt(attempt)
        if result[0] != int(SubmitResult.OK):
            self._disarm_request(completion)
        return result

    async def submit_request(self, router_rid, payload, timeout_ms):
        started = time.monotonic()
        loop = asyncio.get_running_loop()
        owned_payload = _OwnedPayload(payload)
        completion = self._new_request_completion(loop)
        ticket = None
        accepted = False
        try:
            target, deadline, _ = self._resolve_timeouts_and_target(
                router_rid,
                request_timeout=timeout_ms,
                started=started,
            )
            ticket = self._register(target, loop)
            timeout_error = RequestError(RequestResult.TIMED_OUT, errno.ETIMEDOUT)
            while True:
                if deadline is not None and time.monotonic() >= deadline:
                    self._finish(ticket)
                    raise timeout_error
                observed_epoch = self._try_begin(ticket)
                if observed_epoch is None:
                    await self._wait_ready(ticket, deadline, timeout_error)
                    continue
                remaining_ms = 0
                if deadline is not None:
                    remaining_ms = max(
                        1,
                        min(
                            0xFFFFFFFF,
                            int((deadline - time.monotonic()) * 1000),
                        ),
                    )
                rc, native_errno = self._attempt_request(
                    target, owned_payload, remaining_ms, completion
                )
                if rc == int(SubmitResult.OK):
                    self._accept_request(completion)
                    accepted = True
                    terminal = self._finish(ticket)
                    if terminal is not None:
                        completion.detach()
                        result, terminal_errno = terminal
                        raise SubmitError(result, terminal_errno)
                    break
                if rc != int(SubmitResult.BACKPRESSURED):
                    self._finish(ticket)
                    raise SubmitError(rc, native_errno)
                if deadline is not None and time.monotonic() >= deadline:
                    self._finish(ticket)
                    raise timeout_error
                self._backpressured(ticket, observed_epoch)
            return await completion.future
        except asyncio.CancelledError:
            if ticket is not None and not ticket.finished:
                self._cancel(ticket)
            if accepted:
                completion.detach()
            else:
                self._disarm_request(completion)
            raise
        finally:
            if ticket is not None and not ticket.finished:
                self._cancel(ticket)
            if not accepted:
                self._disarm_request(completion)
            owned_payload.close()

    def _on_request_reply(self, result_code, parts, part_count, userdata):
        token = ctypes.cast(userdata, ctypes.c_void_p).value
        with self._state_lock:
            completion = self._request_completions.pop(token, None)
        if completion is None:
            _close_multipart(parts, int(part_count))
            return
        result = _request_result_from_code(int(result_code))
        if result != RequestResult.OK:
            _close_multipart(parts, int(part_count))
            completion.resolve(error=RequestError(result, 0))
            return
        messages = []
        try:
            for index in range(int(part_count)):
                message = Message.__new__(Message)
                message._msg = _clone_native_msg(parts[index])
                message._valid = True
                message._keepalive = None
                messages.append(message)
        except Exception:
            for message in messages:
                message.close()
            completion.resolve(
                error=RequestError(RequestResult.INTERNAL_ERROR, errno.EIO)
            )
        else:
            completion.resolve(value=messages)
        finally:
            _close_multipart(parts, int(part_count))

    def has_request_completions(self):
        with self._state_lock:
            return any(
                completion.accepted
                for completion in self._request_completions.values()
            )

    def fail_request_completions(self):
        with self._state_lock:
            failed = [
                completion
                for completion in self._request_completions.values()
                if completion.accepted
            ]
            for completion in failed:
                self._request_completions.pop(completion.token, None)
        for completion in failed:
            completion.resolve(
                error=RequestError(RequestResult.INTERNAL_ERROR, errno.EIO)
            )

    def begin_close(self):
        with self._state_lock:
            if self._closed:
                return
            self._closing = True
        self._progress.stop()
        with self._selection_gate:
            pass
        with self._attempt_gate:
            pass

    def rollback_close(self):
        with self._state_lock:
            self._closing = False
            deferred = self._deferred_ready_events
            self._deferred_ready_events = []
            for event in deferred:
                self._apply_ready_event_locked(*event)
            for state in self._targets.values():
                if state.operations and state.terminal is None:
                    ticket = state.operations[0]
                    if ticket.state == "waiting":
                        ticket.state = "ready"
                        self._wake_ticket_locked(ticket)
        if self.has_request_completions():
            self._progress.ensure_running()

    def finish_close(self):
        with self._state_lock:
            self._closed = True
            self._closing = False
            self._deferred_ready_events = []
            tickets = [
                ticket
                for state in self._targets.values()
                for ticket in state.operations
            ]
            self._targets.clear()
            completions = list(self._request_completions.values())
            self._request_completions.clear()
            for ticket in tickets:
                ticket.finished = True
                ticket.terminal = (SubmitResult.TERMINATED, errno.ECANCELED)
                self._wake_ticket_locked(ticket)
        for completion in completions:
            completion.resolve(
                error=RequestError(RequestResult.TERMINATED, errno.ECANCELED)
            )


__all__ = ["RoutedAdmissionOwner"]
