# SPDX-License-Identifier: MPL-2.0

from .contracts import (
    Context,
    ContextOptions,
    CommonSocketOptions,
    DealerSocketOptions,
    StreamSocketOptions,
    SubSocketOptions,
    PubSocketOptions,
    RouterSocketOptions,
    PairSocket,
    DealerSocket,
    RouterSocket,
    StreamSocket,
    PubSocket,
    SubSocket,
    XPubSocket,
    XSubSocket,
    Message,
    Received,
    TopicMessage,
    RoutingId,
    SubscriptionEvent,
    AtomicCounter,
    Stopwatch,
    Thread,
    Timer,
    Poller,
    MonitorStatus,
    MonitorEvent,
    MonitorSocket,
    SendOp,
    RequestOp,
    RequestCallbackOp,
    ReplyOp,
    ZlinkError,
    SubmitError,
    RequestError,
    RecvError,
    HandlerError,
    CloseError,
    BindError,
    ConnectError,
    ConfigError,
    SocketType,
    ContextOption,
    AutoHwmProfile,
    AutoHwmRecalcReason,
    SendFlags,
    RecvFlags,
    SubmitResult,
    RequestResult,
    RecvResult,
    HandlerResult,
    CloseResult,
    BindResult,
    ConnectResult,
    ConfigResult,
    RidDuplicatePolicy,
    SubmitRetryMode,
    ErrorCode,
    ProtocolError,
    MonitorEventMask,
    DisconnectReason,
    PollEventFlag,
    PollSourceKind,
    PollEvent,
    PollEvents,
)
from ._runtime.core.context import create_context
from ._runtime.core import zlink as _core_runtime
from ._runtime.eventing.poller import create_poll_events, create_poller
from ._runtime.eventing.timer import create_timer
from ._runtime.eventing.monitor import open_socket_monitor as _runtime_open_socket_monitor
from ._runtime.messaging import message_materializer as _messaging_runtime
from ._runtime.options import option_mapping as _socket_options_runtime
from ._runtime.sockets import socket_base_impl as _socket_runtime
from . import contracts as _contracts_projection
from .contracts import messaging as _messaging_contracts_projection
from .contracts.messaging import message as _message_contract_module

version = _core_runtime.version
strerror = _core_runtime.strerror
has = _core_runtime.has
proxy = _core_runtime.proxy
proxy_steerable = _core_runtime.proxy_steerable
sleep = _core_runtime.sleep
multipart_close = _core_runtime.multipart_close

Message = _messaging_runtime.Message
_contracts_projection.Message = Message
_messaging_contracts_projection.Message = Message
_message_contract_module.Message = Message
allocate_message = _messaging_runtime.message_allocate
create_received = _messaging_runtime.create_received
create_topic_message = _messaging_runtime.create_topic_message
create_subscription_event = _messaging_runtime.create_subscription_event
create_stopwatch = _core_runtime.create_stopwatch
create_thread = _core_runtime.create_thread
create_atomic_counter = _core_runtime.create_atomic_counter

create_pair_socket = _socket_runtime.create_pair_socket
create_dealer_socket = _socket_runtime.create_dealer_socket
create_router_socket = _socket_runtime.create_router_socket
create_stream_socket = _socket_runtime.create_stream_socket
create_pub_socket = _socket_runtime.create_pub_socket
create_sub_socket = _socket_runtime.create_sub_socket
create_xpub_socket = _socket_runtime.create_xpub_socket
create_xsub_socket = _socket_runtime.create_xsub_socket
create_common_socket_options = _socket_options_runtime.create_common_socket_options
create_dealer_socket_options = _socket_options_runtime.create_dealer_socket_options
create_stream_socket_options = _socket_options_runtime.create_stream_socket_options
create_sub_socket_options = _socket_options_runtime.create_sub_socket_options
create_pub_socket_options = _socket_options_runtime.create_pub_socket_options
create_router_socket_options = _socket_options_runtime.create_router_socket_options

__all__ = [
    "version",
    "strerror",
    "has",
    "proxy",
    "proxy_steerable",
    "sleep",
    "multipart_close",
    "create_context",
    "Context",
    "ContextOptions",
    "CommonSocketOptions",
    "create_common_socket_options",
    "DealerSocketOptions",
    "create_dealer_socket_options",
    "StreamSocketOptions",
    "create_stream_socket_options",
    "SubSocketOptions",
    "create_sub_socket_options",
    "PubSocketOptions",
    "create_pub_socket_options",
    "RouterSocketOptions",
    "create_router_socket_options",
    "PairSocket",
    "create_pair_socket",
    "DealerSocket",
    "create_dealer_socket",
    "RouterSocket",
    "create_router_socket",
    "StreamSocket",
    "create_stream_socket",
    "PubSocket",
    "create_pub_socket",
    "SubSocket",
    "create_sub_socket",
    "XPubSocket",
    "create_xpub_socket",
    "XSubSocket",
    "create_xsub_socket",
    "Message",
    "allocate_message",
    "Received",
    "create_received",
    "TopicMessage",
    "create_topic_message",
    "RoutingId",
    "SubscriptionEvent",
    "create_subscription_event",
    "AtomicCounter",
    "create_atomic_counter",
    "Stopwatch",
    "create_stopwatch",
    "Thread",
    "create_thread",
    "Timer",
    "create_timer",
    "Poller",
    "create_poller",
    "MonitorStatus",
    "MonitorEvent",
    "MonitorSocket",
    "SendOp",
    "RequestOp",
    "RequestCallbackOp",
    "ReplyOp",
    "ZlinkError",
    "SubmitError",
    "RequestError",
    "RecvError",
    "HandlerError",
    "CloseError",
    "BindError",
    "ConnectError",
    "ConfigError",
    "SocketType",
    "ContextOption",
    "AutoHwmProfile",
    "AutoHwmRecalcReason",
    "SendFlags",
    "RecvFlags",
    "SubmitResult",
    "RequestResult",
    "RecvResult",
    "HandlerResult",
    "CloseResult",
    "BindResult",
    "ConnectResult",
    "ConfigResult",
    "RidDuplicatePolicy",
    "SubmitRetryMode",
    "ErrorCode",
    "ProtocolError",
    "MonitorEventMask",
    "DisconnectReason",
    "PollEventFlag",
    "PollSourceKind",
    "PollEvent",
    "PollEvents",
    "create_poll_events",
]
