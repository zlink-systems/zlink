# SPDX-License-Identifier: MPL-2.0

import ctypes
import errno as _errno
import time as _time
from typing import Optional

from ...contracts.errors.codes import (
    BindResult,
    CloseResult,
    ConfigResult,
    ConnectResult,
)
from ...contracts.eventing.codes import MonitorEventMask
from ...contracts.sockets.codes import (
    ReceiveFlowState,
    RecvResult,
    SocketType,
    SubmitResult,
)
from ..._native.ffi import ZLINK_PART_FINAL, ZLINK_PART_MORE, ZlinkMsg, ZlinkRoutingId, lib
from ..buffers.payload_buffers import (
    _bool_bytes,
    _int32_bytes,
    _int64_bytes,
    _uint64_bytes,
    _read_int32,
    _read_int64,
    _read_uint64,
)
from ..options.option_mapping import (
    create_common_socket_options,
    create_dealer_socket_options,
    create_stream_socket_options,
    create_sub_socket_options,
)
from ...contracts.core.routing_id import RoutingId
from ...contracts.errors.errors import (
    BindError,
    CloseError,
    ConfigError,
    ConnectError,
    RecvError,
    SubmitError,
)
from ..messaging.message_materializer import (
    Message,
    Received,
    ReceivedMessage,
    TopicMessage,
)
from ..messaging.native_parts import _payload_parts
from ..handles.native_support import (
    _BytesReceivedPartsOwner,
    _ReceivedPartsOwner,
    _as_bytes_view,
    _copy_routing_id,
    _recv_native_parts,
    _raise_result_error,
    _routing_id_bytes,
    _validated_c_string_text,
    _validated_c_string_value,
    _validated_routing_id_bytes,
    _send_buffer,
)

try:
    from ..._native import _zlink_native as _native_extension
except ImportError:  # pragma: no cover - exercised when extension is not built.
    _native_extension = None


_native_recv_owner = (
    getattr(_native_extension, "recv_owner", None)
    if _native_extension is not None
    else None
)
_native_subscribe_owner = (
    getattr(_native_extension, "subscribe_owner", None)
    if _native_extension is not None
    else None
)


def _native_socket_type(sock_type):
    # Core 0.9.0 owns the numeric enum values. Do not reinterpret values from an
    # older binding surface before passing them to the raw socket factory.
    return int(sock_type)


def _socket_type_name(socket_type):
    try:
        return SocketType(int(socket_type)).name
    except ValueError:
        return str(int(socket_type))


def _close_native_parts(native_parts, start=0):
    for native in native_parts[start:]:
        lib().zlink_msg_close(ctypes.byref(native))


def _part_flag(part_index, part_count):
    return ZLINK_PART_FINAL if part_index == part_count - 1 else ZLINK_PART_MORE


def _submit_parts(native_parts, submit_part):
    """Submit `native_parts` one by one through `submit_part(part_ptr, part_flag)`.

    Returns ``(rc, errno)``. On failure, releases the remaining parts.
    """
    part_count = len(native_parts)
    for index, native in enumerate(native_parts):
        rc = submit_part(ctypes.byref(native), _part_flag(index, part_count))
        if rc != 0:
            err = lib().zlink_errno()
            _close_native_parts(native_parts, index)
            return rc, err
    return 0, 0


class _SocketHandle:
    def __init__(self, handle, own):
        self.handle = handle
        self.own = own

    def close(self):
        if not self.handle:
            return
        handle = self.handle
        if not self.own:
            self.handle = None
            return
        rc = lib().zlink_close(handle)
        if rc != 0:
            _raise_result_error(CloseError, CloseResult, rc, lib().zlink_errno())
        self.handle = None

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc, tb):
        _close_owned_resource(self.close)


_RESOURCE_CLOSE_RETRY_INTERVAL_SECONDS = 0.001
_RESOURCE_CLOSE_RETRY_TIMEOUT_SECONDS = 1.0


def _close_owned_resource(close):
    """Retry a binding-owned resource while native ownership is transiently busy."""
    deadline = _time.monotonic() + _RESOURCE_CLOSE_RETRY_TIMEOUT_SECONDS
    while True:
        try:
            close()
            return
        except CloseError as exc:
            if (
                exc.result != CloseResult.BUSY
                or exc.native_errno != _errno.EBUSY
                or _time.monotonic() >= deadline
            ):
                raise
            _time.sleep(_RESOURCE_CLOSE_RETRY_INTERVAL_SECONDS)


class _BaseSocket:
    _socket_type_value = None
    _OPTION_ROUTE_MISS = object()
    _OPTION_SET_ROUTES = ()
    _OPTION_GET_ROUTES = ()

    def __init__(self, context, sock_type=None):
        socket_type = self._resolve_socket_type(sock_type)
        handle = lib().zlink_socket(context._handle, socket_type)
        if not handle:
            _raise_result_error(ConfigError, ConfigResult, 701, lib().zlink_errno())
        self._init_from_native_handle(handle, own=True, socket_type=socket_type)

    @classmethod
    def _resolve_socket_type(cls, sock_type=None):
        resolved = cls._socket_type_value if sock_type is None else sock_type
        if resolved is None:
            raise TypeError("sock_type is required")
        return _native_socket_type(resolved)

    def _init_from_native_handle(self, handle, *, own, socket_type):
        self._socket_handle = _SocketHandle(handle, own)
        self._socket_type = socket_type
        self._options = create_common_socket_options(self)

    @property
    def options(self):
        return self._options

    @property
    def _handle(self):
        return self._socket_handle.handle

    @_handle.setter
    def _handle(self, value):
        if hasattr(self, "_socket_handle"):
            self._socket_handle.handle = value
        else:
            self._socket_handle = _SocketHandle(value, False)

    @classmethod
    def _from_handle(cls, handle, own=False):
        obj = cls.__new__(cls)
        obj._init_from_native_handle(
            handle,
            own=own,
            socket_type=cls._resolve_socket_type(None),
        )
        return obj

    def _publish_payload_via_native_bridge(self, topic_bytes, payload, flags):
        if _native_extension is None:
            return None
        if any(isinstance(part, Message) for part in _payload_parts(payload)):
            return None
        result = _native_extension.publish_parts(
            int(self._socket_handle.handle), topic_bytes, payload, int(flags)
        )
        rc, err = result
        if int(rc) != 0:
            _raise_result_error(SubmitError, SubmitResult, rc, err)
        return True

    def _recv_parts_via_native_bridge(self, flags):
        if _native_extension is None:
            return None
        if _native_recv_owner is not None:
            result = _native_recv_owner(int(self._socket_handle.handle), int(flags))
            if result is False:
                return False
            if result is None:
                return None
            rc, err, routing, owner = result
            if int(rc) != 0:
                _raise_result_error(RecvError, RecvResult, rc, err)
            routing_id = RoutingId.from_(routing) if routing is not None else None
            return routing_id, owner

        result = _native_extension.recv_parts(
            int(self._socket_handle.handle), int(flags)
        )
        if result is None:
            return None
        rc, err, routing, parts = result
        if int(rc) != 0:
            _raise_result_error(RecvError, RecvResult, rc, err)
        routing_id = RoutingId.from_(routing) if routing is not None else None
        return routing_id, _BytesReceivedPartsOwner._from_trusted_bytes_tuple(parts)

    def _recv_owner_via_native_bridge(self, flags):
        if _native_recv_owner is None:
            return None
        result = _native_recv_owner(int(self._socket_handle.handle), int(flags))
        if result is False:
            return False
        if result is None:
            return None
        rc, err, _routing, owner = result
        if int(rc) != 0:
            _raise_result_error(RecvError, RecvResult, rc, err)
        return owner

    def _set_raw_option(self, setter, option, value):
        ptr, size, keepalive = _send_buffer(value)
        rc = setter(
            self._handle,
            int(option),
            ctypes.c_void_p(
                ptr if isinstance(ptr, int) else ctypes.cast(ptr, ctypes.c_void_p).value or 0
            ),
            size,
        )
        _ = keepalive
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())

    def _get_raw_option(self, getter, option, size):
        buf = ctypes.create_string_buffer(size)
        out_size = ctypes.c_size_t(size)
        rc = getter(self._handle, int(option), buf, ctypes.byref(out_size))
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())
        return buf.raw[: out_size.value]

    def _unsupported_capability(self, capability):
        actual = _socket_type_name(self._socket_type)
        raise TypeError(f"{actual} sockets do not support {capability}")

    def _option_route_matches(self, route_key, option):
        if len(route_key) == 1:
            return int(option) == route_key[0]
        return route_key[0] <= int(option) < route_key[1]

    def _dispatch_option_route(self, option, value, routes):
        for route_key, capability, required_type, action, args_factory in routes:
            if not self._option_route_matches(route_key, option):
                continue
            if not isinstance(self, required_type):
                self._unsupported_capability(capability)
            return action(self, *args_factory(int(option), value))
        return self._OPTION_ROUTE_MISS

    def _set_routing_id_raw(self, routing_id):
        topic_bytes = _validated_routing_id_bytes(routing_id)
        rc = lib().zlink_set_routing_id(
            self._handle,
            ctypes.c_char_p(topic_bytes),
            len(topic_bytes),
        )
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())

    def _get_routing_id_raw(self):
        rid = ZlinkRoutingId()
        rc = lib().zlink_get_routing_id(self._handle, ctypes.byref(rid))
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())
        return _routing_id_bytes(rid)

    def _send_result(self, native_result):
        if int(native_result) < 0:
            _raise_result_error(SubmitError, SubmitResult, SubmitResult.INTERNAL_ERROR, lib().zlink_errno())
        return SubmitResult(int(native_result))

    def _set_option(self, option: int, value):
        if self._dispatch_option_route(option, value, self._OPTION_SET_ROUTES) is not self._OPTION_ROUTE_MISS:
            return
        self._set_raw_option(lib().zlink_set_option, option, value)

    def _get_option(self, option: int, size: int = 256):
        routed = self._dispatch_option_route(option, size, self._OPTION_GET_ROUTES)
        if routed is not self._OPTION_ROUTE_MISS:
            return routed
        return self._get_raw_option(lib().zlink_get_option, option, size)

    def _set_common_int_option(self, option: int, value):
        self._set_raw_option(lib().zlink_set_option, option, _int32_bytes(value))

    def _get_common_int_option(self, option: int):
        return _read_int32(self._get_raw_option(lib().zlink_get_option, option, 4))

    def _set_common_uint64_option(self, option: int, value):
        self._set_raw_option(lib().zlink_set_option, option, _uint64_bytes(value))

    def _get_common_uint64_option(self, option: int):
        return _read_uint64(self._get_raw_option(lib().zlink_get_option, option, 8))

    def _set_common_bool_option(self, option: int, value):
        self._set_raw_option(lib().zlink_set_option, option, _bool_bytes(value))

    def _get_common_bool_option(self, option: int):
        return bool(self._get_common_int_option(option))

    def _set_pub_bool_option(self, option: int, value):
        self._set_pub_option(option, _bool_bytes(value))

    def _get_pub_bool_option(self, option: int):
        return bool(_read_int32(self._get_pub_option(option, 4)))

    def monitor_open(self, events=MonitorEventMask.ALL, monitor_hwm_bytes=0):
        from ..eventing.monitor import open_socket_monitor

        return open_socket_monitor(self, events, monitor_hwm_bytes)

    def close(self):
        self._socket_handle.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        _close_owned_resource(self.close)


class _Socket(_BaseSocket):
    _dispatch = {}

    def __new__(cls, context, sock_type=None):
        if cls is _Socket:
            target_cls = cls._dispatch.get(_native_socket_type(sock_type))
            if target_cls is None:
                raise ValueError(f"unsupported socket type: {sock_type!r}")
            return super().__new__(target_cls)
        return super().__new__(cls)

    @classmethod
    def _register_socket_type(cls, sock_type, socket_cls):
        cls._dispatch[_native_socket_type(sock_type)] = socket_cls

    def set_tls_server(self, cert: str, key: str, require_client_cert: bool = False):
        rc = lib().zlink_set_tls_server(
            self._handle,
            _validated_c_string_text(cert, field="cert"),
            _validated_c_string_text(key, field="key"),
            int(require_client_cert),
        )
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())

    def set_tls_client(
        self, ca_cert: Optional[str], hostname: Optional[str], trust_system: bool = False
    ):
        ca_value = (
            None
            if ca_cert is None
            else _validated_c_string_text(ca_cert, field="ca_cert")
        )
        host_value = (
            None
            if hostname is None
            else _validated_c_string_text(hostname, field="hostname")
        )
        rc = lib().zlink_set_tls_client(
            self._handle, ca_value, host_value, int(trust_system)
        )
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())

    def set_receive_flow_state(self, state: ReceiveFlowState):
        """Set this DEALER/ROUTER socket's local receive-flow state. Control
        uses the Application connection for count-1 peers and the Completion
        connection for count-2 ROUTER-ROUTER peers.

        RUNNING/PAUSED is an absolute state, not a counter: repeating the
        current state succeeds and resynchronises nothing new. Only
        DEALER/ROUTER sockets support this state; every other socket type
        (PAIR, the PUB/SUB family, and
        STREAM) raises :class:`ConfigError` with
        :attr:`ConfigResult.NOT_SUPPORTED` and keeps its existing byte HWM
        and transport backpressure unchanged.
        """
        rc = lib().zlink_socket_set_receive_flow_state(self._handle, int(state))
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())


class _BindSocket(_Socket):
    def bind(self, endpoint: str):
        rc = lib().zlink_bind(
            self._handle,
            _validated_c_string_text(endpoint, field="endpoint", max_length=255),
        )
        if rc != 0:
            _raise_result_error(BindError, BindResult, rc, lib().zlink_errno())

    def unbind(self, endpoint: str):
        rc = lib().zlink_unbind(
            self._handle,
            _validated_c_string_text(endpoint, field="endpoint", max_length=255),
        )
        if rc != 0:
            _raise_result_error(ConnectError, ConnectResult, rc, lib().zlink_errno())


class _ConnectSocket(_Socket):
    def connect(self, endpoint: str):
        rc = lib().zlink_connect(
            self._handle,
            _validated_c_string_text(endpoint, field="endpoint", max_length=255),
        )
        if rc != 0:
            _raise_result_error(ConnectError, ConnectResult, rc, lib().zlink_errno())

    def disconnect(self, endpoint: str):
        rc = lib().zlink_disconnect(
            self._handle,
            _validated_c_string_text(endpoint, field="endpoint", max_length=255),
        )
        if rc != 0:
            _raise_result_error(ConnectError, ConnectResult, rc, lib().zlink_errno())

    def disconnect_rid(self, peer_rid):
        native = _copy_routing_id(peer_rid)
        rc = lib().zlink_disconnect_rid(self._handle, ctypes.byref(native))
        if rc != 0:
            _raise_result_error(ConnectError, ConnectResult, rc, lib().zlink_errno())


class _EndpointSocket(_BindSocket, _ConnectSocket):
    pass


class _RoutingIdSocket(_Socket):
    def set_routing_id(self, routing_id):
        self._set_routing_id_raw(routing_id)

    def get_routing_id(self):
        return self._get_routing_id_raw()


class _DealerOptionSocket(_Socket):
    def _set_dealer_option(self, option, value):
        self._set_raw_option(lib().zlink_set_dealer_option, option, value)

    def _get_dealer_option(self, option, size=4):
        return self._get_raw_option(lib().zlink_get_dealer_option, option, size)

    @property
    def dealer_options(self):
        return create_dealer_socket_options(self)


class _RouterOptionSocket(_Socket):
    def _set_router_option(self, option, value):
        self._set_raw_option(lib().zlink_set_router_option, option, value)

    def _get_router_option(self, option, size: int = 256):
        return self._get_raw_option(lib().zlink_get_router_option, option, size)

    def _set_router_bool_option(self, option, value):
        self._set_router_option(option, _bool_bytes(value))

    def _get_router_bool_option(self, option):
        return bool(_read_int32(self._get_router_option(option, ctypes.sizeof(ctypes.c_int32))))

    def _set_router_int_option(self, option, value):
        self._set_router_option(option, _int32_bytes(value))

    def _get_router_int_option(self, option):
        return _read_int32(self._get_router_option(option, ctypes.sizeof(ctypes.c_int32)))

    def _set_router_bytes_option(self, option, value):
        self._set_router_option(option, bytes(_as_bytes_view(value)))

    def _get_router_bytes_option(self, option, size: int = 256):
        return self._get_router_option(option, size)


class _StreamOptionSocket(_Socket):
    def _set_stream_option(self, option, value):
        self._set_raw_option(lib().zlink_set_stream_option, option, value)

    def _get_stream_option(self, option, size: int = 256):
        return self._get_raw_option(lib().zlink_get_stream_option, option, size)

    @property
    def stream_options(self):
        return create_stream_socket_options(self)


class _PublisherOptionSocket(_Socket):
    def _set_pub_option(self, option, value):
        self._set_raw_option(lib().zlink_set_pub_option, option, value)

    def _get_pub_option(self, option, size: int = 256):
        return self._get_raw_option(lib().zlink_get_pub_option, option, size)


class _SubscriberOptionSocket(_Socket):
    def _set_sub_option(self, option, value):
        self._set_raw_option(lib().zlink_set_sub_option, option, value)

    def _get_sub_option(self, option, size: int = 256):
        return self._get_raw_option(lib().zlink_get_sub_option, option, size)

    @property
    def sub_options(self):
        return create_sub_socket_options(self)


class _MessageSocket(_Socket):
    def recv_into(self, received, *, flags=0):
        """Receives into a caller-provided ``Received`` object.

        Pass a long-lived :py:class:`Received` as the first positional
        argument and the binding refills its internal state in place each
        successful call. The public receive contract is the caller-provided
        storage contract described in ``bindings/doc/spec/README.md``.

        :param received: Caller-provided :py:class:`Received` storage.
        :returns: ``True`` on success or ``False`` when ``flags`` includes
            ``DONTWAIT`` and no data is available.
        """
        if received is None:
            raise TypeError("received must be a Received")
        if _native_extension is not None:
            return _native_extension.recv_into(
                self._socket_handle.handle, int(flags), received,
                ReceivedMessage, RoutingId,
            )
        try:
            bridged = self._recv_parts_via_native_bridge(flags)
            if bridged is False:
                return False
            if bridged is None:
                routing, owner = _recv_native_parts(self._handle, flags)
            else:
                routing, owner = bridged
        except RecvError as ex:
            if (int(flags) & 1) and ex.result == RecvResult.NO_DATA:
                return False
            raise
        received._replace(owner, routing)
        return True

class _RoutedMessageSocket(_MessageSocket):
    pass


class _PublisherSocket(_Socket):
    pass


class _SubscriberSocket(_Socket):
    def _subscribe_parts_via_native_bridge(self, flags):
        if _native_subscribe_owner is not None:
            result = _native_subscribe_owner(int(self._socket_handle.handle), int(flags))
            if result is False:
                return False
            if result is None:
                return None
            rc, err, routing, topic_raw, owner = result
            if int(rc) != 0:
                _raise_result_error(RecvError, RecvResult, rc, err)
            routing_id = RoutingId.from_(routing) if routing is not None else None
            return topic_raw, owner, routing_id
        if _native_extension is None:
            return None
        result = _native_extension.subscribe_parts(int(self._handle), int(flags))
        if result is None:
            return None
        rc, err, routing, topic_raw, parts = result
        if int(rc) != 0:
            _raise_result_error(RecvError, RecvResult, rc, err)
        routing_id = RoutingId.from_(routing) if routing is not None else None
        return (
            topic_raw,
            _BytesReceivedPartsOwner._from_trusted_bytes_tuple(parts),
            routing_id,
        )

    def _subscribe_parts_owner(self, flags):
        bridged = self._subscribe_parts_via_native_bridge(flags)
        if bridged is False:
            return False
        if bridged is not None:
            return bridged

        routing_id = ctypes.POINTER(ZlinkRoutingId)()
        topic_buf = ctypes.create_string_buffer(256)
        topic_len = ctypes.c_size_t()
        parts_array = (ZlinkMsg * 1)()
        has_more = ctypes.c_int()
        rc = lib().zlink_subscribe_part(
            self._handle,
            ctypes.byref(routing_id),
            topic_buf,
            len(topic_buf),
            ctypes.byref(topic_len),
            ctypes.byref(parts_array[0]),
            ctypes.byref(has_more),
            int(flags),
        )
        if rc != 0:
            _raise_result_error(RecvError, RecvResult, rc, lib().zlink_errno())
        first_topic_raw = bytes(topic_buf.raw[: topic_len.value])

        if has_more.value == 0:
            routing = _routing_id_bytes(routing_id.contents) if routing_id else None
            return first_topic_raw, _ReceivedPartsOwner(parts_array, 1), routing

        parts = [parts_array[0]]
        recv_flags = 1
        try:
            while True:
                native_part = ZlinkMsg()
                rc = lib().zlink_subscribe_part(
                    self._handle,
                    ctypes.byref(routing_id),
                    topic_buf,
                    len(topic_buf),
                    ctypes.byref(topic_len),
                    ctypes.byref(native_part),
                    ctypes.byref(has_more),
                    recv_flags,
                )
                if rc != 0:
                    _raise_result_error(RecvError, RecvResult, rc, lib().zlink_errno())
                parts.append(native_part)
                if has_more.value == 0:
                    break
        except Exception:
            _close_native_parts(parts)
            raise

        part_count = len(parts)
        final_array = (ZlinkMsg * part_count)()
        for index, native_part in enumerate(parts):
            final_array[index] = native_part
        routing = _routing_id_bytes(routing_id.contents) if routing_id else None
        return first_topic_raw, _ReceivedPartsOwner(final_array, part_count), routing

    def _subscribe_once(self, flags):
        result = self._subscribe_parts_owner(flags)
        if result is False:
            raise RecvError(RecvResult.NO_DATA, 0)
        topic_raw, owner, routing = result
        return TopicMessage(topic_raw.decode("utf-8", errors="replace"), owner, routing)

    def _subscribe_allocated(self, *, flags=0):
        try:
            return self._subscribe_once(flags)
        except RecvError as ex:
            if (int(flags) & 1) and ex.result == RecvResult.NO_DATA:
                return None
            raise

    def subscribe_into(self, topic_message, *, flags=0):
        if topic_message is None or not hasattr(topic_message, "_replace"):
            raise TypeError("topic_message must be a TopicMessage")
        if _native_extension is not None:
            return _native_extension.subscribe_into(
                self._socket_handle.handle, int(flags), topic_message,
                ReceivedMessage, RoutingId,
            )
        try:
            result = self._subscribe_parts_owner(flags)
            if result is False:
                return False
            topic_raw, owner, routing = result
        except RecvError as ex:
            if (int(flags) & 1) and ex.result == RecvResult.NO_DATA:
                return False
            raise
        topic_message._replace(owner, topic_raw=topic_raw, routing_id=routing)
        return True

    def set_subscription(self, topic):
        topic_bytes = _validated_c_string_value(topic, field="subscription")
        rc = lib().zlink_set_subscription(self._handle, topic_bytes)
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())

    def unset_subscription(self, topic):
        topic_bytes = _validated_c_string_value(topic, field="subscription")
        rc = lib().zlink_unset_subscription(self._handle, topic_bytes)
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())

    def subscription_at(self, index):
        if index < 0:
            raise ConfigError(ConfigResult.INVALID_ARGUMENT, _errno.EINVAL)
        size = ctypes.c_size_t(0)
        is_pattern = ctypes.c_int(0)
        rc = lib().zlink_subscription_at(
            self._handle,
            ctypes.c_size_t(index),
            None,
            ctypes.byref(size),
            ctypes.byref(is_pattern),
        )
        if rc != 0 and size.value == 0:
            if ConfigResult(rc) == ConfigResult.NOT_FOUND:
                return None
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())
        if size.value == 0:
            rc = lib().zlink_subscription_at(
                self._handle,
                ctypes.c_size_t(index),
                None,
                ctypes.byref(size),
                ctypes.byref(is_pattern),
            )
            if rc != 0:
                if ConfigResult(rc) == ConfigResult.NOT_FOUND:
                    return None
                _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())
            return ("", bool(is_pattern.value))
        buffer = ctypes.create_string_buffer(size.value)
        rc = lib().zlink_subscription_at(
            self._handle,
            ctypes.c_size_t(index),
            buffer,
            ctypes.byref(size),
            ctypes.byref(is_pattern),
        )
        if rc != 0:
            if ConfigResult(rc) == ConfigResult.NOT_FOUND:
                return None
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())
        return (buffer.raw[:size.value].decode("utf-8"), bool(is_pattern.value))

_BaseSocket._OPTION_SET_ROUTES = (
    ((5,), "routing IDs", _RoutingIdSocket,
     lambda socket, value: socket.set_routing_id(value),
     lambda option, value: (value,)),
    ((6,), "subscriptions", _SubscriberSocket,
     lambda socket, value: socket.set_subscription(value),
     lambda option, value: (value,)),
    ((7,), "subscriptions", _SubscriberSocket,
     lambda socket, value: socket.unset_subscription(value),
     lambda option, value: (value,)),
    ((40,), "publisher options", _PublisherOptionSocket,
     lambda socket, option, value: socket._set_pub_option(option, value),
     lambda option, value: (0x3301, value)),
    ((0x3100, 0x3200), "router options", _RouterOptionSocket,
     lambda socket, option, value: socket._set_router_option(option, value),
     lambda option, value: (option, value)),
    ((0x3200, 0x3300), "dealer options", _DealerOptionSocket,
     lambda socket, option, value: socket._set_dealer_option(option, value),
     lambda option, value: (option, value)),
    ((0x3300, 0x3400), "publisher options", _PublisherOptionSocket,
     lambda socket, option, value: socket._set_pub_option(option, value),
     lambda option, value: (option, value)),
    ((0x3400, 0x3500), "subscriber options", _SubscriberOptionSocket,
     lambda socket, option, value: socket._set_sub_option(option, value),
     lambda option, value: (option, value)),
    ((0x3500, 0x3600), "stream options", _StreamOptionSocket,
     lambda socket, option, value: socket._set_stream_option(option, value),
     lambda option, value: (option, value)),
)

_BaseSocket._OPTION_GET_ROUTES = (
    ((5,), "routing IDs", _RoutingIdSocket,
     lambda socket: socket.get_routing_id(),
     lambda option, size: ()),
    ((0x3100, 0x3200), "router options", _RouterOptionSocket,
     lambda socket, option, size: socket._get_router_option(option, size),
     lambda option, size: (option, size)),
    ((0x3300, 0x3400), "publisher options", _PublisherOptionSocket,
     lambda socket, option, size: socket._get_pub_option(option, size),
     lambda option, size: (option, size)),
    ((0x3400, 0x3500), "subscriber options", _SubscriberOptionSocket,
     lambda socket, option, size: socket._get_sub_option(option, size),
     lambda option, size: (option, size)),
    ((0x3500, 0x3600), "stream options", _StreamOptionSocket,
     lambda socket, option, size: socket._get_stream_option(option, size),
     lambda option, size: (option, size)),
)
