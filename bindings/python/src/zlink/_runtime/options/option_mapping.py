# SPDX-License-Identifier: MPL-2.0

import ctypes

from ...contracts.core.routing_id import RoutingId
from ...contracts.sockets.codes import RidDuplicatePolicy
from ..native_codes import RouterOption, SocketOption
from ...contracts.messaging.message import Message
from ..._native.ffi import lib
from ..buffers.payload_buffers import (
    _bool_bytes,
    _int32_bytes,
    _int64_bytes,
    _read_int32,
    _read_int64,
)
from ..messaging.message_materializer import message_from


class CommonSocketOptions:
    def __init__(self, socket):
        self._socket = socket

    @property
    def linger_ms(self):
        return self._socket._get_common_int_option(SocketOption.LINGER)

    @linger_ms.setter
    def linger_ms(self, value):
        self._socket._set_common_int_option(SocketOption.LINGER, value)

    @property
    def send_high_water_mark(self):
        return self._socket._get_common_uint64_option(SocketOption.SNDHWM)

    @send_high_water_mark.setter
    def send_high_water_mark(self, value):
        self._socket._set_common_uint64_option(SocketOption.SNDHWM, value)

    @property
    def receive_high_water_mark(self):
        return self._socket._get_common_uint64_option(SocketOption.RCVHWM)

    @receive_high_water_mark.setter
    def receive_high_water_mark(self, value):
        self._socket._set_common_uint64_option(SocketOption.RCVHWM, value)

    @property
    def send_timeout_ms(self):
        return self._socket._get_common_int_option(SocketOption.SNDTIMEO)

    @send_timeout_ms.setter
    def send_timeout_ms(self, value):
        self._socket._set_common_int_option(SocketOption.SNDTIMEO, value)

    @property
    def receive_timeout_ms(self):
        return self._socket._get_common_int_option(SocketOption.RCVTIMEO)

    @receive_timeout_ms.setter
    def receive_timeout_ms(self, value):
        self._socket._set_common_int_option(SocketOption.RCVTIMEO, value)

    @property
    def immediate(self):
        return self._socket._get_common_bool_option(SocketOption.IMMEDIATE)

    @immediate.setter
    def immediate(self, value):
        self._socket._set_common_bool_option(SocketOption.IMMEDIATE, value)

    @property
    def rid_duplicate_policy(self):
        value = self._socket._get_common_int_option(
            SocketOption.RID_DUPLICATE_POLICY
        )
        return RidDuplicatePolicy(value)

    @rid_duplicate_policy.setter
    def rid_duplicate_policy(self, value):
        policy = RidDuplicatePolicy(value)
        self._socket._set_common_int_option(
            SocketOption.RID_DUPLICATE_POLICY, int(policy)
        )

    @property
    def connect_timeout_ms(self):
        return self._socket._get_common_int_option(SocketOption.CONNECT_TIMEOUT)

    @connect_timeout_ms.setter
    def connect_timeout_ms(self, value):
        self._socket._set_common_int_option(SocketOption.CONNECT_TIMEOUT, value)

    @property
    def ipv6(self):
        return self._socket._get_common_bool_option(SocketOption.IPV6)

    @ipv6.setter
    def ipv6(self, value):
        self._socket._set_common_bool_option(SocketOption.IPV6, value)

    @property
    def tcp_no_delay(self):
        return self._socket._get_common_bool_option(SocketOption.TCP_NODELAY)

    @tcp_no_delay.setter
    def tcp_no_delay(self, value):
        self._socket._set_common_bool_option(SocketOption.TCP_NODELAY, value)

    @property
    def tcp_keepalive(self):
        return self._socket._get_common_int_option(SocketOption.TCP_KEEPALIVE)

    @tcp_keepalive.setter
    def tcp_keepalive(self, value):
        self._socket._set_common_int_option(SocketOption.TCP_KEEPALIVE, value)

    @property
    def heartbeat_interval_ms(self):
        return self._socket._get_common_int_option(SocketOption.HEARTBEAT_IVL)

    @heartbeat_interval_ms.setter
    def heartbeat_interval_ms(self, value):
        self._socket._set_common_int_option(SocketOption.HEARTBEAT_IVL, value)

    @property
    def heartbeat_ttl_ms(self):
        return self._socket._get_common_int_option(SocketOption.HEARTBEAT_TTL)

    @heartbeat_ttl_ms.setter
    def heartbeat_ttl_ms(self, value):
        self._socket._set_common_int_option(SocketOption.HEARTBEAT_TTL, value)

    @property
    def heartbeat_timeout_ms(self):
        return self._socket._get_common_int_option(SocketOption.HEARTBEAT_TIMEOUT)

    @heartbeat_timeout_ms.setter
    def heartbeat_timeout_ms(self, value):
        self._socket._set_common_int_option(SocketOption.HEARTBEAT_TIMEOUT, value)

    @property
    def max_message_size(self):
        return _read_int64(self._socket._get_raw_option(
            lib().zlink_get_option,
            SocketOption.MAXMSGSIZE,
            ctypes.sizeof(ctypes.c_int64),
        ))

    @max_message_size.setter
    def max_message_size(self, value):
        self._socket._set_raw_option(
            lib().zlink_set_option,
            SocketOption.MAXMSGSIZE,
            _int64_bytes(value),
        )

    @property
    def backlog(self):
        return self._socket._get_common_int_option(SocketOption.BACKLOG)

    @backlog.setter
    def backlog(self, value):
        self._socket._set_common_int_option(SocketOption.BACKLOG, value)

    @property
    def reconnect_interval_ms(self):
        return self._socket._get_common_int_option(SocketOption.RECONNECT_IVL)

    @reconnect_interval_ms.setter
    def reconnect_interval_ms(self, value):
        self._socket._set_common_int_option(SocketOption.RECONNECT_IVL, value)

    @property
    def reconnect_interval_max_ms(self):
        return self._socket._get_common_int_option(SocketOption.RECONNECT_IVL_MAX)

    @reconnect_interval_max_ms.setter
    def reconnect_interval_max_ms(self, value):
        self._socket._set_common_int_option(SocketOption.RECONNECT_IVL_MAX, value)

    @property
    def submit_retry_mode(self):
        return self._socket._get_common_int_option(SocketOption.SUBMIT_RETRY_MODE)

    @submit_retry_mode.setter
    def submit_retry_mode(self, value):
        self._socket._set_common_int_option(SocketOption.SUBMIT_RETRY_MODE, value)

    @property
    def submit_retry_timeout_ms(self):
        return self._socket._get_common_int_option(SocketOption.SUBMIT_RETRY_TIMEOUT)

    @submit_retry_timeout_ms.setter
    def submit_retry_timeout_ms(self, value):
        self._socket._set_common_int_option(SocketOption.SUBMIT_RETRY_TIMEOUT, value)

    @property
    def submit_retry_attempts(self):
        return self._socket._get_common_int_option(SocketOption.SUBMIT_RETRY_ATTEMPTS)

    @submit_retry_attempts.setter
    def submit_retry_attempts(self, value):
        self._socket._set_common_int_option(SocketOption.SUBMIT_RETRY_ATTEMPTS, value)


class DealerSocketOptions:
    _PROBE = 0x3201
    _REQUEST_TIMEOUT_MS = 0x3202
    _WEIGHT = 0x3203

    def __init__(self, socket):
        self._socket = socket

    @property
    def probe(self):
        return bool(_read_int32(self._socket._get_dealer_option(self._PROBE, 4)))

    @probe.setter
    def probe(self, value):
        self._socket._set_dealer_option(self._PROBE, _bool_bytes(value))

    @property
    def weight(self):
        return _read_int32(self._socket._get_dealer_option(self._WEIGHT, 4))

    @weight.setter
    def weight(self, value):
        self._socket._set_dealer_option(self._WEIGHT, _int32_bytes(value))

    @property
    def request_timeout_ms(self):
        return _read_int32(
            self._socket._get_dealer_option(self._REQUEST_TIMEOUT_MS, 4)
        )

    @request_timeout_ms.setter
    def request_timeout_ms(self, value):
        self._socket._set_dealer_option(self._REQUEST_TIMEOUT_MS, _int32_bytes(value))


class StreamSocketOptions:
    _NOTIFY = 0x3501

    def __init__(self, socket):
        self._socket = socket

    @property
    def notify(self):
        return bool(_read_int32(self._socket._get_stream_option(self._NOTIFY, 4)))

    @notify.setter
    def notify(self, value):
        self._socket._set_stream_option(self._NOTIFY, _bool_bytes(value))


class SubSocketOptions:
    _TOPICS_COUNT = 0x3400

    def __init__(self, socket):
        self._socket = socket

    @property
    def topics_count(self):
        return _read_int32(self._socket._get_sub_option(self._TOPICS_COUNT, 4))


class PubSocketOptions:
    _VERBOSE = 0x3301
    _VERBOSER = 0x3302
    _MANUAL = 0x3303
    _MANUAL_LAST_VALUE = 0x3304
    _NO_DROP = 0x3305
    _WELCOME_MSG = 0x3306
    _TOPICS_COUNT = 0x3307
    _APPROVE_SUBSCRIBE = 0x3308
    _REJECT_SUBSCRIBE = 0x3309

    def __init__(self, socket):
        self._socket = socket

    @property
    def verbose(self):
        return self._socket._get_pub_bool_option(self._VERBOSE)

    @verbose.setter
    def verbose(self, enabled):
        self._socket._set_pub_bool_option(self._VERBOSE, enabled)

    @property
    def verboser(self):
        return self._socket._get_pub_bool_option(self._VERBOSER)

    @verboser.setter
    def verboser(self, enabled):
        self._socket._set_pub_bool_option(self._VERBOSER, enabled)

    @property
    def manual(self):
        return self._socket._get_pub_bool_option(self._MANUAL)

    @manual.setter
    def manual(self, enabled):
        self._socket._set_pub_bool_option(self._MANUAL, enabled)

    @property
    def no_drop(self):
        return self._socket._get_pub_bool_option(self._NO_DROP)

    @no_drop.setter
    def no_drop(self, enabled):
        self._socket._set_pub_bool_option(self._NO_DROP, enabled)

    @property
    def manual_last_value(self):
        return self._socket._get_pub_bool_option(self._MANUAL_LAST_VALUE)

    @manual_last_value.setter
    def manual_last_value(self, enabled):
        self._socket._set_pub_bool_option(self._MANUAL_LAST_VALUE, enabled)

    @property
    def welcome_message(self):
        return message_from(self._socket._get_pub_option(self._WELCOME_MSG))

    @welcome_message.setter
    def welcome_message(self, message):
        if not isinstance(message, Message):
            message = message_from(message)
        self._socket._set_pub_option(self._WELCOME_MSG, message.to_bytes())

    @property
    def topics_count(self):
        return _read_int32(self._socket._get_pub_option(self._TOPICS_COUNT, 4))

    def approve_subscribe(self, routing_id):
        self._socket._set_pub_option(
            self._APPROVE_SUBSCRIBE,
            RoutingId(routing_id).to_bytes(),
        )

    def reject_subscribe(self, routing_id):
        self._socket._set_pub_option(
            self._REJECT_SUBSCRIBE,
            RoutingId(routing_id).to_bytes(),
        )


class RouterSocketOptions:
    # RID_DUPLICATE_POLICY common option values (mirrors ZLINK_RID_DUPLICATE_*)
    _RID_DUPLICATE_POLICY_OPTION = 0x3033
    _RID_DUPLICATE_REJECT = 0
    _RID_DUPLICATE_HANDOVER = 1
    _REQUEST_TIMEOUT_MS = 0x3105

    def __init__(self, socket):
        self._socket = socket

    @property
    def mandatory(self):
        return self._socket._get_router_bool_option(RouterOption.MANDATORY)

    @mandatory.setter
    def mandatory(self, enabled):
        self._socket._set_router_bool_option(RouterOption.MANDATORY, enabled)

    @property
    def handover(self):
        return (
            self._socket._get_common_int_option(self._RID_DUPLICATE_POLICY_OPTION)
            == self._RID_DUPLICATE_HANDOVER
        )

    @handover.setter
    def handover(self, value):
        policy = self._RID_DUPLICATE_HANDOVER if value else self._RID_DUPLICATE_REJECT
        self._socket._set_common_int_option(self._RID_DUPLICATE_POLICY_OPTION, policy)

    @property
    def probe(self):
        return self._socket._get_router_bool_option(RouterOption.PROBE)

    @probe.setter
    def probe(self, enabled):
        self._socket._set_router_bool_option(RouterOption.PROBE, enabled)

    @property
    def connect_routing_id(self):
        cached = getattr(self._socket, "_connect_routing_id_option", None)
        if cached is not None:
            return cached
        return RoutingId(
            self._socket._get_router_bytes_option(RouterOption.CONNECT_ROUTING_ID)
        )

    @connect_routing_id.setter
    def connect_routing_id(self, routing_id):
        typed_routing_id = RoutingId(routing_id)
        self._socket._set_router_bytes_option(
            RouterOption.CONNECT_ROUTING_ID,
            typed_routing_id.to_bytes(),
        )
        self._socket._connect_routing_id_option = typed_routing_id

    @property
    def weight(self):
        return self._socket._get_router_int_option(RouterOption.WEIGHT)

    @weight.setter
    def weight(self, value):
        self._socket._set_router_int_option(RouterOption.WEIGHT, value)

    @property
    def request_timeout_ms(self):
        return self._socket._get_router_int_option(self._REQUEST_TIMEOUT_MS)

    @request_timeout_ms.setter
    def request_timeout_ms(self, value):
        self._socket._set_router_int_option(self._REQUEST_TIMEOUT_MS, value)


for _public_type in (
    CommonSocketOptions,
    DealerSocketOptions,
    StreamSocketOptions,
    SubSocketOptions,
    PubSocketOptions,
    RouterSocketOptions,
):
    _public_type.__module__ = "zlink.contracts.sockets.socket_options"


def create_common_socket_options(socket):
    return CommonSocketOptions(socket)


def create_dealer_socket_options(socket):
    return DealerSocketOptions(socket)


def create_stream_socket_options(socket):
    return StreamSocketOptions(socket)


def create_sub_socket_options(socket):
    return SubSocketOptions(socket)


def create_pub_socket_options(socket):
    return PubSocketOptions(socket)


def create_router_socket_options(socket):
    return RouterSocketOptions(socket)
