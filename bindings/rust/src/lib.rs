// SPDX-License-Identifier: MPL-2.0

//! Rust bindings for the zlink messaging library.
//!
//! This crate wraps the zlink C API (`zlink.h`) with an idiomatic, safe Rust
//! surface following the bindings API policy:
//!
//! - **Builder-based** public send, publish, request, and reply operations.
//! - **Multipart-only** public send/receive surface.
//! - **Typed options** per socket type – no raw option bags.
//! - **Ownership** via RAII: [`Message`] drop calls `zlink_msg_close`;
//!   `send` consumes messages and suppresses drop.
//! - **Message diagnostics** via [`Message::ref_count`].
//! - **Domain objects**: [`Received`], [`TopicMessage`], [`SubscriptionEvent`],
//!   [`SendResult`], [`RoutingId`].
//! - **Socket capability isolation**: each socket type exposes only its own
//!   capabilities (e.g. `PubSocket` has no `recv`).

mod contracts;
mod internal;
mod native;
mod runtime;

// -- Public re-exports -------------------------------------------------------

pub use contracts::core::context::{
    AutoHwmProfile, AutoHwmRecalcReason, Context, ContextOptions, CoreHwmBudgetSnapshot,
};
pub use contracts::core::routing_id::RoutingId;
pub use contracts::core::utilities::{AtomicCounter, Stopwatch, Thread};
pub use contracts::errors::errors::{
    BindError, CloseError, ConfigError, ConnectError, HandlerError, RecvError, RequestError,
    SubmitError, ZlinkError,
};
pub use contracts::errors::results::{
    BindResult, CloseResult, ConfigResult, ConnectResult, HandlerResult, RecvResult, RequestResult,
    SubmitResult,
};
pub use contracts::eventing::monitor::{
    MONITOR_EVENT_ALL, MONITOR_EVENT_CONNECTION_READY, MonitorEvent, MonitorEventFlags,
    MonitorEventType, MonitorSourceKind, MonitorStatus, Monitorable, SocketMonitor,
    SocketMonitorEventMask, SocketMonitorOpenOptions,
};
pub use contracts::eventing::poller::{
    POLLCOMPLETION, POLLCONNECT, POLLIN, POLLOUT, POLLROUTE, PollEvent, PollItem, PollSourceKind,
    Pollable, Poller,
};
pub use contracts::eventing::timer::Timer;
pub use contracts::messaging::message::Message;
pub use contracts::messaging::operation_contracts::{CompletionKind, SendResult};
pub use contracts::messaging::operations::{
    Empty, PublishOp, Ready, ReplyOp, RequestOp, RequestSubmission, SendOp, SendSubmission,
};
pub use contracts::messaging::received::{Received, ReplyToken};
pub use contracts::messaging::subscription_event::SubscriptionEvent;
pub use contracts::messaging::topic_message::TopicMessage;
pub use contracts::sockets::connection_attempt::{
    ConnectionAttempt, ConnectionAttemptState, ConnectionFailure, ConnectionFailureKind,
    ConnectionFailureStage,
};
pub use contracts::sockets::dealer_socket::DealerSocket;
pub use contracts::sockets::pair_socket::PairSocket;
pub use contracts::sockets::pubsub_sockets::{PubSocket, SubSocket, XPubSocket, XSubSocket};
pub use contracts::sockets::router_socket::{RouterRoute, RouterSocket};
pub use contracts::sockets::socket_options::{
    CommonSocketOptions, DealerSocketOptions, PubSocketOptions, ReceiveFlowState, RecvFlags,
    RidDuplicatePolicy, RouterSocketOptions, SendFlags, StreamRecvMode, StreamSocketOptions,
    SubSocketOptions, SubmitRetryMode,
};
pub use contracts::sockets::stream_socket::{StreamPacket, StreamSocket};

pub use runtime::core::runtime::{multipart_close, proxy, sleep};
pub use runtime::eventing::poller::poll;
pub use runtime::handles::ctx::{has, strerror, version};
