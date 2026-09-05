# SPDX-License-Identifier: MPL-2.0

import ctypes
import errno

from ...contracts.sockets.codes import RecvResult, SocketType, SubmitResult
from ...contracts.core.routing_id import RoutingId
from ...contracts.messaging.received import (
    _reply_token_from_native,
    _reply_token_owner_matches,
    _reply_token_value,
)
from ..options.option_mapping import (
    create_pub_socket_options,
    create_router_socket_options,
)
from ..._native.ffi import ZlinkMsg, ZlinkRoutingId, lib
from ..handles.native_support import (
    _copy_routing_id,
    _decode_topic_text,
    _ReceivedPartsOwner,
    _raise_result_error,
    _routing_id_bytes,
    _validated_routing_id_bytes,
    _validated_c_string_value,
)
from ...contracts.errors.errors import (
    ConnectError,
    RecvError,
    SubmitError,
)
from ...contracts.errors.codes import ConnectResult
from ..messaging.message_materializer import (
    Message,
    ReceivedMessage,
    SubscriptionEvent,
)
from ..messaging.request_reply import _clone_payload, _timeout_to_ms
from ..messaging.routed_async import CompletionOwner
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
    _StreamOptionSocket,
    _SubscriberOptionSocket,
    _SubscriberSocket,
    _close_native_parts,
    _native_extension,
    _part_flag,
    _submit_parts,
)


_NO_PAYLOAD = object()


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
def _native_publisher_send_op(socket, topic):
    if _native_publisher_send_op_func is None:
        return None
    return _NativePublisherSendOp(socket, topic)


class _PublishOpBase:
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

class _ManagedSendOp:
    """Unified send builder that captures its optional target at creation."""

    __slots__ = ("_socket", "_routing_id", "_payload", "_parts", "_submitted")

    def __init__(self, socket, routing_id=None):
        self._socket = socket
        self._routing_id = (
            None if routing_id is None else _validated_routing_id_bytes(routing_id)
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
        return self._socket._completion_owner.submit_send(self._routing_id, payload)

    def submit_sync(self) -> None:
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        self._payload_or_raise()
        self._submitted = True
        payload = self._parts if self._parts is not None else self._payload
        self._socket._completion_owner.submit_send_sync(self._routing_id, payload)


class _PublisherSendOp(_PublishOpBase):
    __slots__ = ("_topic",)

    def __init__(self, socket, topic):
        super().__init__(socket)
        self._topic = topic

    def submit(self):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        payload = self._payload_or_raise()
        self._submitted = True
        topic_bytes = _validated_c_string_value(self._topic, field="topic")
        bridged = self._socket._publish_payload_via_native_bridge(
            topic_bytes,
            payload,
            self._flags,
        )
        if bridged is not None:
            if not bridged:
                raise SubmitError(SubmitResult.BACKPRESSURED, errno.EAGAIN)
            return None
        native_parts = _clone_payload(payload)
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
        return None


class _NativePublisherSendOp(_PublishOpBase):
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
        submitted = native.submit()
        if not submitted:
            raise SubmitError(SubmitResult.BACKPRESSURED, errno.EAGAIN)
        return None


class _RequestOp:
    """Builder exposing awaitable and blocking request terminals."""

    __slots__ = (
        "_op_submit",
        "_op_submit_sync",
        "_parts",
        "_timeout",
        "_submitted",
    )

    def __init__(self, op_submit, op_submit_sync):
        self._op_submit = op_submit
        self._op_submit_sync = op_submit_sync
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

    def submit_sync(self):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        if not self._parts:
            raise SubmitError(SubmitResult.INVALID_ARGUMENT, 0)
        self._submitted = True
        return self._op_submit_sync(
            self._parts,
            _timeout_to_ms(self._timeout),
        )


class _ReplyOp:
    """Own the fluent state for one raw ROUTER reply."""

    __slots__ = ("_op_callback", "_parts", "_submitted")

    def __init__(self, op_callback):
        self._op_callback = op_callback
        self._parts = []
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

    def submit(self):
        if self._submitted:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        if not self._parts:
            raise SubmitError(SubmitResult.INVALID_ARGUMENT, 0)
        self._submitted = True
        return self._op_callback(self._parts)


class _CompletionSocket:
    """Lifecycle glue for one socket-local pull completion owner."""

    def _init_completion_socket(self):
        self._completion_owner = CompletionOwner(self)

    def close(self):
        owner = getattr(self, "_completion_owner", None)
        if owner is not None:
            owner.shutdown()
        super().close()
        if owner is not None:
            owner.finish_shutdown()


class PairSocket(_CompletionSocket, _EndpointSocket, _MessageSocket):
    _socket_type_value = SocketType.PAIR

    def __init__(self, context):
        super().__init__(context)
        self._init_completion_socket()

    def send(self):
        return _ManagedSendOp(self)


class DealerSocket(
    _CompletionSocket,
    _EndpointSocket,
    _DealerOptionSocket,
    _RoutingIdSocket,
    _MessageSocket,
):
    _socket_type_value = SocketType.DEALER

    def __init__(self, context):
        super().__init__(context)
        self._init_completion_socket()

    def send(self):
        return _ManagedSendOp(self)

    def request(self):
        return _RequestOp(
            lambda parts, timeout_ms: self._completion_owner.submit_request(
                None, parts, timeout_ms
            ),
            lambda parts, timeout_ms: (
                self._completion_owner.submit_request_sync(None, parts, timeout_ms)
            ),
        )

class RouterSocket(
    _CompletionSocket,
    _EndpointSocket,
    _RouterOptionSocket,
    _RoutingIdSocket,
    _RoutedMessageSocket,
):
    _socket_type_value = SocketType.ROUTER

    def __init__(self, context):
        super().__init__(context)
        self._reply_owner = object()
        self._init_completion_socket()

    @property
    def router_options(self):
        return create_router_socket_options(self)

    def send(self, routing_id):
        return _ManagedSendOp(self, routing_id)

    def request(self, peer_rid):
        return _RequestOp(
            lambda parts, timeout_ms: self._completion_owner.submit_request(
                peer_rid, parts, timeout_ms
            ),
            lambda parts, timeout_ms: (
                self._completion_owner.submit_request_sync(peer_rid, parts, timeout_ms)
            ),
        )

    def reply(self, routing_id, token):
        if not _reply_token_owner_matches(token, self._reply_owner):
            raise SubmitError(SubmitResult.INVALID_ARGUMENT, errno.EINVAL)
        return _ReplyOp(lambda parts: self._reply_payload(routing_id, token, parts))

    def _reply_payload(self, routing_id, token, payload):
        native_parts = _clone_payload(payload)
        native_rid = _copy_routing_id(routing_id)
        rc, native_errno = _submit_parts(
            native_parts,
            lambda part_ptr, part_flag: lib().zlink_reply_part(
                self._handle,
                ctypes.byref(native_rid),
                ctypes.c_uint64(_reply_token_value(token)),
                part_ptr,
                part_flag,
            ),
        )
        if rc != int(SubmitResult.OK):
            _raise_result_error(SubmitError, SubmitResult, rc, native_errno)

    def _replace_router_received(self, received, owner, routing_id, token_value):
        received._replace(
            owner,
            routing_id=routing_id,
            reply_token=(
                _reply_token_from_native(self._reply_owner, token_value)
                if token_value != 0
                else None
            ),
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
        if _native_extension is not None:
            return _native_extension.router_recv_into(
                self._socket_handle.handle, int(flags), received,
                ReceivedMessage, RoutingId, self, _reply_token_from_native,
            )
        if _native_router_recv_owner_func is not None:
            result = _native_router_recv_owner_func(
                int(self._socket_handle.handle), int(flags)
            )
            if result is False:
                return False
            rc, err, routing, token_value, owner = result
            if int(rc) != 0:
                _raise_result_error(RecvError, RecvResult, rc, err)
            routing_id = RoutingId.from_(routing) if routing is not None else None
            self._replace_router_received(
                received, owner, routing_id, int(token_value)
            )
            return True
        try:
            source_rid = ctypes.POINTER(ZlinkRoutingId)()
            token_value = ctypes.c_uint64()
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
                        ctypes.byref(token_value),
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
            int(token_value.value),
        )
        return True

class StreamSocket(
    _CompletionSocket,
    _BindSocket,
    _StreamOptionSocket,
    _RoutingIdSocket,
    _RoutedMessageSocket,
):
    _socket_type_value = SocketType.STREAM

    def __init__(self, context):
        super().__init__(context)
        self._init_completion_socket()

    def send(self, routing_id):
        return _ManagedSendOp(self, routing_id)

    def disconnect_rid(self, peer_rid):
        native = _copy_routing_id(peer_rid)
        rc = lib().zlink_disconnect_rid(self._handle, ctypes.byref(native))
        if rc != 0:
            _raise_result_error(ConnectError, ConnectResult, rc, lib().zlink_errno())

    def recv_packet_into(self, out, *, flags=0):
        if out is None or not hasattr(out, "_begin_receive"):
            raise TypeError("out must be a StreamPacket")
        out._begin_receive()
        claimed = True
        source_rid = ctypes.POINTER(ZlinkRoutingId)()
        header_native = ZlinkMsg()
        body_native = ZlinkMsg()
        header_valid = False
        body_valid = False
        try:
            rc = lib().zlink_msg_init(ctypes.byref(header_native))
            if rc != 0:
                _raise_result_error(RecvError, RecvResult, rc, lib().zlink_errno())
            header_valid = True
            rc = lib().zlink_msg_init(ctypes.byref(body_native))
            if rc != 0:
                _raise_result_error(RecvError, RecvResult, rc, lib().zlink_errno())
            body_valid = True
            rc = lib().zlink_stream_recv_packet(
                self._handle,
                ctypes.byref(source_rid),
                ctypes.byref(header_native),
                ctypes.byref(body_native),
                int(flags),
            )
            if rc == int(RecvResult.NO_DATA):
                return False
            if rc != int(RecvResult.OK):
                _raise_result_error(RecvError, RecvResult, rc, lib().zlink_errno())
            if not source_rid or source_rid.contents.size == 0:
                raise RecvError(RecvResult.INTERNAL_ERROR, errno.EPROTO)

            routing_id = RoutingId.from_(_routing_id_bytes(source_rid.contents))
            header = Message.__new__(Message)
            header._msg = header_native
            header._valid = True
            header._keepalive = None
            body = Message.__new__(Message)
            body._msg = body_native
            body._valid = True
            body._keepalive = None
            header_valid = False
            body_valid = False
            out._finish_receive(routing_id, header, body)
            claimed = False
            return True
        finally:
            if header_valid:
                lib().zlink_msg_close(ctypes.byref(header_native))
            if body_valid:
                lib().zlink_msg_close(ctypes.byref(body_native))
            if claimed:
                out._finish_receive()


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
