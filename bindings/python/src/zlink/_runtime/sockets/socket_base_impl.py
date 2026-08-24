# SPDX-License-Identifier: MPL-2.0

import ctypes

from ...contracts.sockets.codes import SocketType
from ...contracts.core.routing_id import RoutingId
from ..options.option_mapping import (
    create_pub_socket_options,
    create_router_socket_options,
)
from ..buffers.payload_buffers import _read_int32
from ..._native.ffi import ZlinkMsg, ZlinkRoutingId, lib
from ..handles.native_support import (
    _copy_routing_id,
    _decode_topic_text,
    _ReceivedPartsOwner,
    _raise_result_error,
    _report_unhandled_callback_exception,
    _routing_id_bytes,
    _validated_routing_id_bytes,
    _validated_c_string_value,
)
from ...contracts.errors.errors import (
    ConnectError,
    HandlerError,
    RecvError,
    SubmitError,
)
from ...contracts.errors.codes import ConnectResult
from ...contracts.sockets.codes import HandlerResult, RecvResult, SubmitResult
from ..messaging.message_materializer import (
    Message,
    ReceivedMessage,
    SubscriptionEvent,
)
from ..messaging.request_reply import (
    _clone_payload,
    _ensure_reply_flags_supported,
    _timeout_to_ms,
)
from ..messaging.routed_async import RoutedSendOwner, SendCompletionOwner
from .socket_base import (
    _BindSocket,
    _DealerOptionSocket,
    _EndpointSocket,
    _MessageSocket,
    _PublisherOptionSocket,
    _PublisherSocket,
    _RoutingIdSocket,
    _RoutedMessageSocket,
    _RouterOptionSocket,
    _Socket,
    _STREAM_PACKET_HANDLER,
    _StreamOptionSocket,
    _SubscriberOptionSocket,
    _SubscriberSocket,
    _close_native_parts,
    _clone_received_owner,
    _in_callback,
    _native_extension,
    _part_flag,
    _submit_parts,
)


_NO_PAYLOAD = object()


_native_socket_send_op_func = (
    getattr(_native_extension, "socket_send_op", None)
    if _native_extension is not None
    else None
)
_native_routed_send_op_func = (
    getattr(_native_extension, "routed_send_op", None)
    if _native_extension is not None
    else None
)
_native_publisher_send_op_func = (
    getattr(_native_extension, "publisher_send_op", None)
    if _native_extension is not None
    else None
)
_native_router_recv_owner_func = (
    getattr(_native_extension, "router_recv_owner", None)
    if _native_extension is not None
    else None
)


def _native_socket_send_op(socket):
    if _native_socket_send_op_func is None or _in_callback():
        return None
    return _NativeBuilderSendOp(socket)


def _native_routed_send_op(socket, routing_id):
    if _native_routed_send_op_func is None or _in_callback():
        return None
    if isinstance(routing_id, bytes):
        routing_id_bytes = routing_id
    else:
        routing_id_bytes = _validated_routing_id_bytes(routing_id)
    return _NativeBuilderSendOp(socket, routing_id_bytes)


def _native_publisher_send_op(socket, topic):
    if _native_publisher_send_op_func is None or _in_callback():
        return None
    return _NativePublisherSendOp(socket, topic)


class _SocketSendOp:
    __slots__ = ("_socket", "_payload", "_parts", "_flags", "_submitted")

    def __init__(self, socket):
        self._socket = socket
        self._payload = _NO_PAYLOAD
        self._parts = None
        self._flags = 0
        self._submitted = False

    def message(self, payload):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        if self._parts is not None:
            self._parts.append(payload)
        elif self._payload is _NO_PAYLOAD:
            self._payload = payload
        else:
            self._parts = [self._payload, payload]
            self._payload = _NO_PAYLOAD
        return self

    def messages(self, *payloads):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        if not payloads:
            return self
        if self._parts is not None:
            self._parts.extend(payloads)
        elif self._payload is _NO_PAYLOAD:
            if len(payloads) == 1:
                self._payload = payloads[0]
            else:
                self._parts = list(payloads)
        else:
            self._parts = [self._payload, *payloads]
            self._payload = _NO_PAYLOAD
        return self

    def flags(self, flags):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        self._flags = int(flags)
        return self

    def _payload_or_raise(self):
        if self._parts is not None:
            if not self._parts:
                raise SubmitError(SubmitResult.INVALID_ARGUMENT, 0)
            return self._parts
        if self._payload is _NO_PAYLOAD:
            raise SubmitError(SubmitResult.INVALID_ARGUMENT, 0)
        return self._payload

    def submit(self):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        payload = self._payload_or_raise()
        self._submitted = True
        try:
            bridged = self._socket._send_payload_via_native_bridge(
                payload, self._flags
            )
            if bridged is not None:
                return bridged
            self._socket._send_native_parts(
                self._socket._native_parts_from_payload(payload),
                self._flags,
            )
            return True
        except SubmitError as ex:
            if (self._flags & 1) and ex.result == SubmitResult.BACKPRESSURED:
                return False
            raise


class _RoutedSocketSendOp(_SocketSendOp):
    """Synchronous routed builder retained for STREAM only."""

    __slots__ = ("_routing_id",)

    def __init__(self, socket, routing_id):
        super().__init__(socket)
        self._routing_id = _validated_routing_id_bytes(routing_id)

    def submit(self):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        payload = self._payload_or_raise()
        self._submitted = True
        try:
            bridged = self._socket._send_routed_payload_bytes_via_native_bridge(
                self._routing_id,
                payload,
                self._flags,
            )
            if bridged is not None:
                return bridged
            self._socket._send_native_parts_to_routing_id(
                self._routing_id,
                self._socket._native_parts_from_payload(payload),
                self._flags,
            )
            return True
        except SubmitError as ex:
            if (self._flags & 1) and ex.result == SubmitResult.BACKPRESSURED:
                return False
            raise


class _ManagedSendOp:
    """Canonical coroutine builder for PAIR's HWM-managed send.

    Admission is `zlink_send_async`; completion is the single
    `zlink_send_complete_handler` the socket installs at construction (see
    `RoutedSendOwner`/`SendCompletionOwner` in `_runtime/messaging/routed_async.py`).
    """

    __slots__ = ("_socket", "_payload", "_parts", "_submitted")

    def __init__(self, socket):
        self._socket = socket
        self._payload = _NO_PAYLOAD
        self._parts = None
        self._submitted = False

    def message(self, payload):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        if self._parts is not None:
            self._parts.append(payload)
        elif self._payload is _NO_PAYLOAD:
            self._payload = payload
        else:
            self._parts = [self._payload, payload]
            self._payload = _NO_PAYLOAD
        return self

    def messages(self, *payloads):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        if not payloads:
            return self
        if self._parts is not None:
            self._parts.extend(payloads)
        elif self._payload is _NO_PAYLOAD:
            if len(payloads) == 1:
                self._payload = payloads[0]
            else:
                self._parts = list(payloads)
        else:
            self._parts = [self._payload, *payloads]
            self._payload = _NO_PAYLOAD
        return self

    def _payload_or_raise(self):
        if self._parts is not None:
            if not self._parts:
                raise SubmitError(SubmitResult.INVALID_ARGUMENT, 0)
            return self._parts
        if self._payload is _NO_PAYLOAD:
            raise SubmitError(SubmitResult.INVALID_ARGUMENT, 0)
        return self._payload

    def submit(self):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        payload = self._payload_or_raise()
        self._submitted = True
        return self._socket._send_completion.submit(payload)


class _ManagedRoutedSendOp:
    """Canonical coroutine builder for DEALER/ROUTER HWM admission."""

    __slots__ = ("_socket", "_routing_id", "_payload", "_parts", "_submitted")

    def __init__(self, socket, routing_id=None):
        self._socket = socket
        self._routing_id = (
            None
            if routing_id is None
            else _validated_routing_id_bytes(routing_id)
        )
        self._payload = _NO_PAYLOAD
        self._parts = None
        self._submitted = False

    def message(self, payload):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        if self._parts is not None:
            self._parts.append(payload)
        elif self._payload is _NO_PAYLOAD:
            self._payload = payload
        else:
            self._parts = [self._payload, payload]
            self._payload = _NO_PAYLOAD
        return self

    def messages(self, *payloads):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        if not payloads:
            return self
        if self._parts is not None:
            self._parts.extend(payloads)
        elif self._payload is _NO_PAYLOAD:
            if len(payloads) == 1:
                self._payload = payloads[0]
            else:
                self._parts = list(payloads)
        else:
            self._parts = [self._payload, *payloads]
            self._payload = _NO_PAYLOAD
        return self

    def _payload_or_raise(self):
        if self._parts is not None:
            if not self._parts:
                raise SubmitError(SubmitResult.INVALID_ARGUMENT, 0)
            return self._parts
        if self._payload is _NO_PAYLOAD:
            raise SubmitError(SubmitResult.INVALID_ARGUMENT, 0)
        return self._payload

    def submit(self):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        payload = self._payload_or_raise()
        self._submitted = True
        return self._socket._routed_admission.submit_send(
            self._routing_id, payload
        )


class _PublisherSendOp(_SocketSendOp):
    __slots__ = ("_topic",)

    def __init__(self, socket, topic):
        super().__init__(socket)
        self._topic = topic

    def submit(self):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        payload = self._payload_or_raise()
        self._submitted = True
        try:
            topic_bytes = _validated_c_string_value(self._topic, field="topic")
            bridged = self._socket._publish_payload_via_native_bridge(
                topic_bytes,
                payload,
                self._flags,
            )
            if bridged is not None:
                return bridged
            native_parts = self._socket._native_parts_from_payload(payload)
            part_count = len(native_parts)
            for index, native in enumerate(native_parts):
                rc = lib().zlink_publish_part(
                    self._socket._handle,
                    topic_bytes,
                    ctypes.byref(native),
                    int(self._flags),
                    _part_flag(index, part_count),
                )
                if rc != 0:
                    err = lib().zlink_errno()
                    _close_native_parts(native_parts, index)
                    _raise_result_error(SubmitError, SubmitResult, rc, err)
            return True
        except SubmitError as ex:
            if (self._flags & 1) and ex.result == SubmitResult.BACKPRESSURED:
                return False
            raise


class _NativeBuilderSendOp(_SocketSendOp):
    """Delay native builder selection until payload types are known.

    The C builder is the fast path for buffer-protocol parts. A public
    ``Message`` is a native-owned value rather than a Python buffer, so it is
    submitted through the shared Python materializer instead of leaking that
    implementation distinction as a ``TypeError``.
    """

    __slots__ = (
        "_handle",
        "_routing_id",
    )

    def __init__(self, socket, routing_id=None):
        super().__init__(socket)
        self._handle = int(socket._socket_handle.handle)
        self._routing_id = routing_id

    def submit(self):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        payload = self._payload_or_raise()
        self._submitted = True
        has_native_message = (
            isinstance(payload, Message)
            if self._parts is None
            else any(isinstance(part, Message) for part in self._parts)
        )
        if has_native_message:
            if self._routing_id is None:
                fallback = _SocketSendOp(self._socket)
            else:
                fallback = _RoutedSocketSendOp(self._socket, self._routing_id)
            if self._parts is not None:
                fallback.messages(*self._parts)
            else:
                fallback.message(self._payload)
            fallback.flags(self._flags)
            return fallback.submit()

        if self._routing_id is None:
            native = _native_socket_send_op_func(self._handle)
        else:
            native = _native_routed_send_op_func(self._handle, self._routing_id)
        if self._parts is not None:
            native.messages(*self._parts)
        else:
            native.message(self._payload)
        native.flags(self._flags)
        return native.submit()


class _NativePublisherSendOp(_SocketSendOp):
    """Use the native publisher builder without per-send factory closures."""

    __slots__ = ("_handle", "_topic")

    def __init__(self, socket, topic):
        super().__init__(socket)
        self._handle = int(socket._socket_handle.handle)
        self._topic = topic

    def submit(self):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        payload = self._payload_or_raise()
        self._submitted = True
        has_native_message = (
            isinstance(payload, Message)
            if self._parts is None
            else any(isinstance(part, Message) for part in self._parts)
        )
        if has_native_message:
            fallback = _PublisherSendOp(self._socket, self._topic)
            if self._parts is not None:
                fallback.messages(*self._parts)
            else:
                fallback.message(payload)
            fallback.flags(self._flags)
            return fallback.submit()

        native = _native_publisher_send_op_func(self._handle, self._topic)
        if self._parts is not None:
            native.messages(*self._parts)
        else:
            native.message(payload)
        native.flags(self._flags)
        return native.submit()


class _RequestOp:
    """Canonical coroutine builder for one routed request."""

    __slots__ = ("_op_submit", "_parts", "_timeout", "_submitted")

    def __init__(self, op_submit):
        self._op_submit = op_submit
        self._parts = []
        self._timeout = 0
        self._submitted = False

    def message(self, payload):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        self._parts.append(payload)
        return self

    def messages(self, *payloads):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        self._parts.extend(payloads)
        return self

    def timeout(self, timeout):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        self._timeout = timeout
        return self

    def submit(self):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        if not self._parts:
            raise SubmitError(SubmitResult.INVALID_ARGUMENT, 0)
        self._submitted = True
        return self._op_submit(
            self._parts,
            _timeout_to_ms(self._timeout),
        )


class _ReplyOp:
    """Own the fluent state for one raw ROUTER reply."""

    __slots__ = ("_op_callback", "_parts", "_flags", "_submitted")

    def __init__(self, op_callback):
        self._op_callback = op_callback
        self._parts = []
        self._flags = 0
        self._submitted = False

    def message(self, payload):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        self._parts.append(payload)
        return self

    def messages(self, *payloads):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        self._parts.extend(payloads)
        return self

    def flags(self, flags):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        self._flags = int(flags)
        return self

    def submit(self):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        if not self._parts:
            raise SubmitError(SubmitResult.INVALID_ARGUMENT, 0)
        self._submitted = True
        return self._op_callback(self._parts, self._flags)


class _RoutedAsyncSocket:
    """Lifecycle glue for the socket-local routed coroutine owner."""

    def _init_routed_async_socket(self, role):
        if role == SocketType.DEALER:
            read_request_timeout = lambda: self.dealer_options.request_timeout_ms
        else:
            read_request_timeout = lambda: self.router_options.request_timeout_ms
        try:
            admission = RoutedSendOwner(
                self,
                role,
                self._outbound_record_attempt_gate,
                read_request_timeout,
            )
        except Exception:
            super().close()
            raise
        self._routed_admission = admission

    def close(self):
        admission = getattr(self, "_routed_admission", None)
        if admission is None:
            return super().close()
        try:
            admission.begin_close()
            super().close()
        except Exception:
            admission.rollback_close()
            raise
        admission.finish_close()


class PairSocket(_EndpointSocket, _MessageSocket):
    _socket_type_value = SocketType.PAIR

    def __init__(self, context):
        super().__init__(context)
        try:
            self._send_completion = SendCompletionOwner(self)
        except Exception:
            super().close()
            raise

    def send(self):
        return _ManagedSendOp(self)


class DealerSocket(
    _RoutedAsyncSocket,
    _EndpointSocket,
    _DealerOptionSocket,
    _RoutingIdSocket,
    _MessageSocket,
):
    _socket_type_value = SocketType.DEALER

    def __init__(self, context):
        super().__init__(context)
        self._init_routed_async_socket(SocketType.DEALER)

    def send(self):
        return _ManagedRoutedSendOp(self)

    def request(self):
        return _RequestOp(
            lambda parts, timeout_ms: self._routed_admission.submit_request(
                None, parts, timeout_ms
            )
        )

class RouterSocket(
    _RoutedAsyncSocket,
    _EndpointSocket,
    _RouterOptionSocket,
    _RoutingIdSocket,
    _RoutedMessageSocket,
):
    _socket_type_value = SocketType.ROUTER

    def __init__(self, context):
        super().__init__(context)
        self._init_routed_async_socket(SocketType.ROUTER)

    @property
    def router_options(self):
        return create_router_socket_options(self)

    def send(self, routing_id):
        return _ManagedRoutedSendOp(self, routing_id)

    def request(self, peer_rid):
        return _RequestOp(
            lambda parts, timeout_ms: self._routed_admission.submit_request(
                peer_rid, parts, timeout_ms
            )
        )

    def reply(self, routing_id, request_seq):
        return _ReplyOp(
            lambda parts, op_flags: self._reply_payload(
                routing_id, request_seq, parts, flags=op_flags
            )
        )

    def _reply_payload(self, routing_id, request_seq, payload, *, flags=0):
        _ensure_reply_flags_supported(flags)
        native_parts = _clone_payload(payload)
        native_rid = _copy_routing_id(routing_id)
        attempted = False

        def submit(_handle):
            nonlocal attempted
            attempted = True
            return _submit_parts(
                native_parts,
                lambda part_ptr, part_flag: lib().zlink_router_reply_part(
                    self._handle,
                    ctypes.byref(native_rid),
                    ctypes.c_uint64(request_seq),
                    part_ptr,
                    part_flag,
                ),
            )

        try:
            self._routed_admission.run_sync_outbound_attempt(submit)
        except Exception:
            if not attempted:
                _close_native_parts(native_parts)
            raise

    def _replace_router_received(self, received, owner, routing_id, request_seq):
        received._replace(
            owner,
            routing_id=routing_id,
            request_seq=request_seq if request_seq != 0 else None,
            router_socket=self,
        )

    def recv_into(self, received, *, flags=0):
        """Receives a routed message into a caller-provided ``Received`` object.

        Pass a long-lived :py:class:`Received` as the first positional
        argument and the binding refills its internal state in place each
        successful call. The public receive contract is the caller-provided
        storage contract described in ``bindings/doc/spec/README.md``.

        :param received: Caller-provided :py:class:`Received` storage.
        :returns: ``True`` on success or ``False`` when DONTWAIT finds no data.
        """
        if received is None:
            raise TypeError("received must be a Received")
        if _native_router_recv_owner_func is not None and not _in_callback():
            result = _native_router_recv_owner_func(
                int(self._socket_handle.handle), int(flags)
            )
            if result is False:
                return False
            rc, err, routing, request_seq, owner = result
            if int(rc) != 0:
                _raise_result_error(RecvError, RecvResult, rc, err)
            routing_id = RoutingId.from_(routing) if routing is not None else None
            self._replace_router_received(
                received, owner, routing_id, int(request_seq)
            )
            return True
        try:
            source_rid = ctypes.POINTER(ZlinkRoutingId)()
            request_seq = ctypes.c_uint64()
            native_parts = []
            has_more = ctypes.c_int()
            recv_flags = int(flags)
            try:
                while True:
                    native_part = ZlinkMsg()
                    rc = lib().zlink_msg_init(ctypes.byref(native_part))
                    if rc != 0:
                        _raise_result_error(RecvError, RecvResult, rc, lib().zlink_errno())
                    rc = lib().zlink_router_recv_part(
                        self._handle,
                        ctypes.byref(source_rid),
                        ctypes.byref(request_seq),
                        ctypes.byref(native_part),
                        ctypes.byref(has_more),
                        recv_flags,
                    )
                    if rc != 0:
                        lib().zlink_msg_close(ctypes.byref(native_part))
                        _raise_result_error(RecvError, RecvResult, rc, lib().zlink_errno())
                    native_parts.append(native_part)
                    if has_more.value == 0:
                        break
                    recv_flags = 1
            except Exception:
                _close_native_parts(native_parts)
                raise
        except RecvError as ex:
            if int(flags) & 1 and ex.result == RecvResult.NO_DATA:
                return False
            raise
        routing_id = _routing_id_bytes(source_rid.contents) if source_rid else None
        parts_array = (ZlinkMsg * len(native_parts))()
        for index, native_part in enumerate(native_parts):
            parts_array[index] = native_part
        self._replace_router_received(
            received,
            _ReceivedPartsOwner(parts_array, len(native_parts)),
            routing_id,
            int(request_seq.value),
        )
        return True

class StreamSocket(
    _BindSocket,
    _StreamOptionSocket,
    _RoutingIdSocket,
    _RoutedMessageSocket,
):
    _socket_type_value = SocketType.STREAM

    def __init__(self, context):
        super().__init__(context)

    def send(self, routing_id):
        return _native_routed_send_op(self, routing_id) or _RoutedSocketSendOp(
            self, routing_id
        )

    def disconnect_rid(self, peer_rid):
        native = _copy_routing_id(peer_rid)
        rc = lib().zlink_disconnect_rid(self._handle, ctypes.byref(native))
        if rc != 0:
            _raise_result_error(ConnectError, ConnectResult, rc, lib().zlink_errno())

    def on_packet(self, handler):
        if handler is None:
            raise ValueError("handler must not be None")
        if self._recv_handler is not None or self._packet_handler is not None:
            raise RuntimeError("handler is already attached")

        self._packet_handler = handler
        dispatcher = self._dispatcher

        def _invoke(routing_id, header, body):
            try:
                handler(routing_id, header, body)
            except Exception:
                _report_unhandled_callback_exception(handler)
            finally:
                try:
                    header.close()
                finally:
                    body.close()

        def _callback(_stream, source_rid_ptr, header_ptr, body_ptr, _):
            try:
                routing_id = None
                if source_rid_ptr:
                    routing_id = _routing_id_bytes(source_rid_ptr.contents)
                header_owner = _clone_received_owner(header_ptr, 1)
                try:
                    body_owner = _clone_received_owner(body_ptr, 1)
                except Exception:
                    header_owner.close()
                    raise
                header = ReceivedMessage._from_owner(header_owner, 0)
                body = ReceivedMessage._from_owner(body_owner, 0)
            except Exception:
                _report_unhandled_callback_exception(handler)
                return
            task = lambda routing_id=routing_id, header=header, body=body: _invoke(
                routing_id, header, body
            )
            if not dispatcher.submit(task):
                header.close()
                body.close()

        callback = _STREAM_PACKET_HANDLER(_callback)
        rc = lib().zlink_stream_packet_handler(self._handle, callback, None)
        if rc != 0:
            self._packet_handler = None
            _raise_result_error(HandlerError, HandlerResult, rc, lib().zlink_errno())
        self._packet_handler_cb = callback


class PubSocket(
    _EndpointSocket,
    _PublisherOptionSocket,
    _PublisherSocket,
):
    _socket_type_value = SocketType.PUB

    @property
    def pub_options(self):
        return create_pub_socket_options(self)

    def publish(self, topic):
        native = _native_publisher_send_op(self, topic)
        if native is not None:
            return native
        return _PublisherSendOp(self, topic)


class SubSocket(
    _EndpointSocket,
    _SubscriberOptionSocket,
    _SubscriberSocket,
):
    _socket_type_value = SocketType.SUB


class XPubSocket(
    _EndpointSocket,
    _PublisherOptionSocket,
    _PublisherSocket,
):
    _socket_type_value = SocketType.XPUB

    @property
    def pub_options(self):
        return create_pub_socket_options(self)

    def publish(self, topic):
        native = _native_publisher_send_op(self, topic)
        if native is not None:
            return native
        return _PublisherSendOp(self, topic)

    def _subscription_event(self, flags):
        routing_id = ctypes.POINTER(ZlinkRoutingId)()
        subscribed = ctypes.c_int()
        topic_buf = ctypes.create_string_buffer(256)
        topic_len = ctypes.c_size_t()
        rc = lib().zlink_xpub_recv_part(
            self._handle,
            ctypes.byref(routing_id),
            ctypes.byref(subscribed),
            topic_buf,
            len(topic_buf),
            ctypes.byref(topic_len),
            int(flags),
        )
        if rc != 0:
            _raise_result_error(RecvError, RecvResult, rc, lib().zlink_errno())
        return SubscriptionEvent(
            routing_id=_routing_id_bytes(routing_id.contents) if routing_id else None,
            topic=_decode_topic_text(topic_buf.raw[: topic_len.value]),
            subscribed=bool(subscribed.value),
        )

    def _receive_subscription_event(self, *, flags=0):
        return self._subscription_event(flags)

    def receive_subscription_event_into(self, event, *, flags=0):
        if event is None or not hasattr(event, "_adopt_from"):
            raise TypeError("event must be a SubscriptionEvent")
        try:
            fresh = self._receive_subscription_event(flags=flags)
        except RecvError as ex:
            if (int(flags) & 1) and ex.result == RecvResult.NO_DATA:
                return False
            raise
        event._adopt_from(fresh)
        return True


class XSubSocket(
    _EndpointSocket,
    _SubscriberOptionSocket,
    _SubscriberSocket,
):
    _socket_type_value = SocketType.XSUB


for _public_type in (
    PairSocket,
    DealerSocket,
    RouterSocket,
    StreamSocket,
    PubSocket,
    SubSocket,
    XPubSocket,
    XSubSocket,
):
    _public_type.__module__ = "zlink.contracts.sockets.socket"


def create_pair_socket(context):
    return PairSocket(context)


def create_dealer_socket(context):
    return DealerSocket(context)


def create_router_socket(context):
    return RouterSocket(context)


def create_stream_socket(context):
    return StreamSocket(context)


def create_pub_socket(context):
    return PubSocket(context)


def create_sub_socket(context):
    return SubSocket(context)


def create_xpub_socket(context):
    return XPubSocket(context)


def create_xsub_socket(context):
    return XSubSocket(context)


for _socket_type, _socket_cls in (
    (SocketType.PAIR, PairSocket),
    (SocketType.DEALER, DealerSocket),
    (SocketType.ROUTER, RouterSocket),
    (SocketType.STREAM, StreamSocket),
    (SocketType.PUB, PubSocket),
    (SocketType.SUB, SubSocket),
    (SocketType.XPUB, XPubSocket),
    (SocketType.XSUB, XSubSocket),
):
    _Socket._register_socket_type(_socket_type, _socket_cls)
