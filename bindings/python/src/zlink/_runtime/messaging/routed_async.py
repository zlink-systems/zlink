# SPDX-License-Identifier: MPL-2.0

"""Core `send_complete`-driven async send and reply-driven request completion.

Zero binding-owned threads, queues, or retry. HWM-managed **send** (PAIR
send, DEALER/ROUTER routed send) is admitted by ``zlink_send_async`` and
completed by the single ``zlink_send_complete_handler`` installed on the
socket — inline when Core admits immediately, or later from whatever
context Core chooses to dispatch the callback from (its own async mailbox
thread, a deadline thread, or a ``ZLINK_POLLCOMPLETION`` poller). This
module never retries a backpressured attempt and never waits on a
binding-owned queue or timer: the per-operation deadline is the Core-side
``zlink_send_async_options_t.timeout_ms`` field, exactly as
``_runtime/eventing/timer.py`` hands timing entirely to a Core timer
handle.

**request** completion is purely the Core reply callback resolving the
awaitable it was armed with; there is no admission ticket and no
binding-owned polling thread pumping a completion poller.
"""

import asyncio
import ctypes
import errno
import threading

from ..._native.ffi import (
    ZLINK_DONTWAIT,
    ZLINK_PART_FINAL,
    ZLINK_PART_MORE,
    ZLINK_SEND_ADMITTED,
    ZLINK_SEND_TERMINAL,
    ZLINK_SEND_TIMED_OUT,
    ZlinkMsg,
    ZlinkRoutedSubmitTarget,
    ZlinkSendAsyncOptions,
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
    _SEND_COMPLETE_HANDLER,
    _clone_native_msg,
    _close_multipart,
    _copy_routing_id,
    _raise_result_error,
    _request_result_from_code,
)
from .message_materializer import Message
from .native_parts import _materialize_native_parts

_ETERM = 156384765


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


class _SendOperation:
    """One in-flight `zlink_send_async` anchor, keyed by `userdata` token.

    Once `zlink_send_async` returns `ZLINK_SUBMIT_OK`, message ownership has
    transferred to Core for the whole lifetime of the operation, including
    every completion result (`ADMITTED`/`TIMED_OUT`/`TERMINAL`) — this
    anchor never closes native parts itself.
    """

    __slots__ = ("token", "loop", "future", "op_id")

    def __init__(self, token, loop):
        self.token = token
        self.loop = loop
        self.future = loop.create_future()
        self.op_id = None


class SendCompletionOwner:
    """Owns one socket's `zlink_send_complete_handler` and pending anchors.

    Mirrors the C++ reference (`socket_callback_state_t`): one completion
    handler per socket, a pending-operation table keyed by an opaque token
    carried through Core in `zlink_send_async_options_t.userdata` and
    returned unchanged in `zlink_send_complete_event_t.userdata`. Using our
    own token instead of the Core-assigned `op_id` for correlation is
    required because Core may invoke the completion inline, before
    `zlink_send_async` has returned the `op_id` to us.
    """

    def __init__(self, socket):
        self._socket = socket
        self._lock = threading.Lock()
        self._pending = {}
        self._next_token = 1
        self._handler = _SEND_COMPLETE_HANDLER(self._on_complete)
        rc = lib().zlink_send_complete_handler(
            self.native_handle(), self._handler, None
        )
        if rc != int(HandlerResult.OK):
            _raise_result_error(
                HandlerError, HandlerResult, rc, lib().zlink_errno()
            )

    def native_handle(self):
        return self._socket._handle

    def drain_pending(self):
        with self._lock:
            pending = list(self._pending.values())
            self._pending.clear()
        return pending

    def _on_complete(self, _subject, event_ptr, _userdata):
        if not event_ptr:
            return
        event = event_ptr.contents
        token = ctypes.cast(event.userdata, ctypes.c_void_p).value
        with self._lock:
            op = self._pending.pop(token, None) if token is not None else None
        if op is None:
            return
        result = int(event.result)
        if result == ZLINK_SEND_ADMITTED:
            _schedule_future(op.loop, op.future, None)
            return
        if result == ZLINK_SEND_TIMED_OUT:
            error = SubmitError(SubmitResult.BACKPRESSURED, errno.ETIMEDOUT)
        else:
            native_errno = int(event.terminal_errno)
            error = SubmitError(_terminal_submit_result(native_errno), native_errno)
        _schedule_future(op.loop, op.future, error, is_error=True)

    async def submit(self, payload, *, target=None, timeout_ms=0):
        loop = asyncio.get_running_loop()
        # `_materialize_native_parts` hands back independently owned struct
        # values; copy their bytes into one contiguous array for the single
        # `zlink_send_async` record. Past this point the array — not the
        # source list — is the live owner (see `_SendOperation`).
        source_parts = _materialize_native_parts(payload)
        part_count = len(source_parts)
        parts_array = (ZlinkMsg * part_count)(*source_parts)

        with self._lock:
            token = self._next_token
            self._next_token += 1
            op = _SendOperation(token, loop)
            self._pending[token] = op

        options = ZlinkSendAsyncOptions(
            struct_size=ctypes.sizeof(ZlinkSendAsyncOptions),
            timeout_ms=int(timeout_ms) if timeout_ms else 0,
            userdata=ctypes.c_void_p(token),
            target=ctypes.pointer(target) if target is not None else None,
        )
        op_id_out = ctypes.c_uint64(0)
        rc = lib().zlink_send_async(
            self.native_handle(),
            parts_array,
            part_count,
            ctypes.byref(options),
            ctypes.byref(op_id_out),
        )
        if rc != int(SubmitResult.OK):
            with self._lock:
                self._pending.pop(token, None)
            native_errno = lib().zlink_errno()
            _close_native_parts(parts_array)
            _raise_result_error(SubmitError, SubmitResult, rc, native_errno)

        if op_id_out.value == 0:
            # Core completed synchronously and deliberately emits no callback.
            with self._lock:
                self._pending.pop(token, None)
            return None

        with self._lock:
            pending_op = self._pending.get(token)
            if pending_op is not None:
                pending_op.op_id = op_id_out.value

        try:
            await op.future
        except asyncio.CancelledError:
            with self._lock:
                still_pending = self._pending.get(token) is op
            if still_pending and op.op_id:
                lib().zlink_send_async_cancel(self.native_handle(), op.op_id)
            raise
        return None


class _RequestCompletion:
    """One armed request awaiting its Core reply callback."""

    __slots__ = ("token", "loop", "future")

    def __init__(self, token, loop):
        self.token = token
        self.loop = loop
        self.future = loop.create_future()

    def resolve(self, value=None, error=None):
        if self.future.done():
            if error is None and isinstance(value, list):
                for message in value:
                    message.close()
            return
        _schedule_future(
            self.loop,
            self.future,
            error if error is not None else value,
            is_error=error is not None,
        )


class RoutedSendOwner:
    """Socket-local HWM-managed send (via `zlink_send_async`) and
    Core-reply-driven request completion. Owns no thread, queue, or retry
    policy of its own.
    """

    def __init__(
        self,
        socket,
        role,
        read_request_timeout_ms,
    ):
        self._socket = socket
        self._role = role
        self._read_request_timeout_ms = read_request_timeout_ms
        self._state_lock = threading.RLock()
        self._request_completions = {}
        self._next_request_token = 1
        self._reply_handler = _REPLY_HANDLER(self._on_request_reply)
        self._send_completion = SendCompletionOwner(socket)

    def native_handle(self):
        return self._socket._handle

    # -- HWM-managed send (PAIR send, DEALER/ROUTER routed send) --------

    def _select_target(self, router_rid):
        target = ZlinkRoutedSubmitTarget()
        native_rid = None if router_rid is None else _copy_routing_id(router_rid)
        rc = lib().zlink_select_routed_submit_target(
            self.native_handle(),
            None if native_rid is None else ctypes.byref(native_rid),
            ctypes.byref(target),
        )
        if rc != int(SubmitResult.OK):
            _raise_result_error(
                SubmitError, SubmitResult, rc, lib().zlink_errno()
            )
        return target

    async def submit_send(self, router_rid, payload):
        # ROUTER requires an exact target snapshot for the given routing id.
        # DEALER always passes `target=None` here — Core commits one
        # weighted selection at submit time (`zlink_send_async_options_t`
        # doc, core/include/zlink/socket/api.h).
        target = self._select_target(router_rid) if self._role == SocketType.ROUTER else None
        # `zlink_send_async_options_t.timeout_ms` is a per-operation Core
        # deadline, unrelated to `ZLINK_OPT_SNDTIMEO`. 0 means no deadline:
        # Core's own
        # pending-queue bound (`ZLINK_OPT_SEND_PENDING_MAX_MSGS`/`_BYTES`)
        # governs backpressure rejection, not a binding-owned timer.
        await self._send_completion.submit(payload, target=target, timeout_ms=0)

    def run_sync_outbound_attempt(self, attempt):
        handle = self.native_handle()
        if not handle:
            _raise_result_error(
                SubmitError,
                SubmitResult,
                SubmitResult.TERMINATED,
                errno.ECANCELED,
            )
        rc, native_errno = attempt(handle)
        if rc != int(SubmitResult.OK):
            _raise_result_error(SubmitError, SubmitResult, rc, native_errno)

    @staticmethod
    def _submit_parts(native_parts, submit_part):
        part_count = len(native_parts)
        for index, native in enumerate(native_parts):
            flag = ZLINK_PART_FINAL if index == part_count - 1 else ZLINK_PART_MORE
            rc = submit_part(ctypes.byref(native), flag, index == part_count - 1)
            if rc != int(SubmitResult.OK):
                native_errno = lib().zlink_errno()
                # Core consumes the attempted native part on ordinary
                # rejection. The binding owns these materialized copies and
                # closes the now-empty attempted slot together with every
                # later part that was not attempted.
                _close_native_parts(native_parts, index)
                return int(rc), native_errno
        return int(SubmitResult.OK), 0

    # -- request: submitted once, completed purely by the Core reply
    # callback. No admission ticket, no retry on backpressure. ----------

    def _new_request_completion(self, loop):
        with self._state_lock:
            token = self._next_request_token
            self._next_request_token += 1
        completion = _RequestCompletion(token, loop)
        with self._state_lock:
            self._request_completions[token] = completion
        return completion

    def _attempt_request(self, target, native_parts, timeout_ms, completion):
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

        handle = self.native_handle()
        if not handle:
            return int(SubmitResult.TERMINATED), errno.ECANCELED
        return attempt(handle)

    async def submit_request(self, router_rid, payload, timeout_ms):
        loop = asyncio.get_running_loop()
        native_parts = _materialize_native_parts(payload)
        target = self._select_target(router_rid)
        if timeout_ms == 0:
            timeout_ms = int(self._read_request_timeout_ms())
            if timeout_ms <= 0:
                timeout_ms = 5000
        completion = self._new_request_completion(loop)
        try:
            rc, native_errno = self._attempt_request(
                target, native_parts, timeout_ms, completion
            )
        except Exception:
            with self._state_lock:
                self._request_completions.pop(completion.token, None)
            raise
        if rc != int(SubmitResult.OK):
            with self._state_lock:
                self._request_completions.pop(completion.token, None)
            _raise_result_error(SubmitError, SubmitResult, rc, native_errno)
        return await completion.future

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

    # -- lifecycle --------------------------------------------------------

    def finish_close(self):
        with self._state_lock:
            completions = list(self._request_completions.values())
            self._request_completions.clear()
        pending_sends = self._send_completion.drain_pending()
        for completion in completions:
            completion.resolve(
                error=RequestError(RequestResult.TERMINATED, errno.ECANCELED)
            )
        # Each pending send already transferred message ownership to Core at
        # submit time (see `_SendOperation`); only the awaitable needs a
        # terminal result here.
        for op in pending_sends:
            _schedule_future(
                op.loop,
                op.future,
                SubmitError(SubmitResult.TERMINATED, errno.ECANCELED),
                is_error=True,
            )


__all__ = ["RoutedSendOwner", "SendCompletionOwner"]
