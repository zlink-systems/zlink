use std::time::Duration;

use crate::error::ConfigError;
use crate::internal::SocketStorage;
use crate::message::{Message, RoutingId};
use crate::socket_contracts::DealerSocket;

/// Flags that modify send behavior.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SendFlags(u32);

impl SendFlags {
    /// Default behavior: block until the message can be queued.
    pub const NONE: Self = Self(0);
    /// Do not block; report back-pressure instead of waiting when the send
    /// would block.
    pub const DONT_WAIT: Self = Self(0x0001);

    /// Returns the raw flag bits.
    pub const fn bits(self) -> u32 {
        self.0
    }
}

impl Default for SendFlags {
    fn default() -> Self {
        Self::NONE
    }
}

/// Flags that modify receive behavior.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RecvFlags(u32);

impl RecvFlags {
    /// Default behavior: block until a message is available.
    pub const NONE: Self = Self(0);
    /// Do not block; return without a message when none is available.
    pub const DONT_WAIT: Self = Self(0x0001);

    /// Returns the raw flag bits.
    pub const fn bits(self) -> u32 {
        self.0
    }
}

impl Default for RecvFlags {
    fn default() -> Self {
        Self::NONE
    }
}

/// Determines how a socket reacts to a peer that reuses an existing routing id.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum RidDuplicatePolicy {
    /// Reject the new peer and keep the existing route.
    Reject = 0,
    /// Hand the routing id to the new peer, dropping the previous holder.
    Handover = 1,
}

/// Determines whether a failed submit is retried.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum SubmitRetryMode {
    /// Never retry; a failed submit fails immediately.
    Off = 0,
    /// Retry when the submit fails locally, such as under back-pressure.
    LocalFailure = 1,
}

/// Typed facade over the socket options shared by every socket type.
pub struct CommonSocketOptions<'a> {
    inner: &'a SocketStorage,
}

impl<'a> CommonSocketOptions<'a> {
    pub(crate) fn new(inner: &'a SocketStorage) -> Self {
        Self { inner }
    }

    /// Sets how long `close` waits to deliver still-queued messages.
    pub fn set_linger(&self, d: Duration) -> Result<(), ConfigError> {
        self.inner.set_linger(d)
    }
    /// Returns the configured linger duration.
    pub fn linger(&self) -> Result<Duration, ConfigError> {
        self.inner.linger()
    }
    /// Sets the maximum outbound bytes queued before the socket applies
    /// back-pressure; 0 means no limit.
    pub fn set_send_high_water_mark(&self, value: u64) -> Result<(), ConfigError> {
        self.inner.set_send_high_water_mark(value)
    }
    /// Returns the configured send high-water mark.
    pub fn send_high_water_mark(&self) -> Result<u64, ConfigError> {
        self.inner.send_high_water_mark()
    }
    /// Sets the maximum inbound bytes queued before the socket applies
    /// back-pressure; 0 means no limit.
    pub fn set_receive_high_water_mark(&self, value: u64) -> Result<(), ConfigError> {
        self.inner.set_receive_high_water_mark(value)
    }
    /// Returns the configured receive high-water mark.
    pub fn receive_high_water_mark(&self) -> Result<u64, ConfigError> {
        self.inner.receive_high_water_mark()
    }
    /// Sets how long a blocking send waits to enqueue a message before failing.
    pub fn set_send_timeout(&self, d: Duration) -> Result<(), ConfigError> {
        self.inner.set_send_timeout(d)
    }
    /// Returns the configured send timeout.
    pub fn send_timeout(&self) -> Result<Duration, ConfigError> {
        self.inner.send_timeout()
    }
    /// Sets how long a blocking receive waits for a message before failing.
    pub fn set_receive_timeout(&self, d: Duration) -> Result<(), ConfigError> {
        self.inner.set_receive_timeout(d)
    }
    /// Returns the configured receive timeout.
    pub fn receive_timeout(&self) -> Result<Duration, ConfigError> {
        self.inner.receive_timeout()
    }
    /// Sets whether messages are queued only to fully established connections;
    /// when disabled they may also queue to pending connections.
    pub fn set_immediate(&self, enabled: bool) -> Result<(), ConfigError> {
        self.inner.set_immediate(enabled)
    }
    /// Returns whether immediate mode is enabled.
    pub fn immediate(&self) -> Result<bool, ConfigError> {
        self.inner.immediate()
    }
    /// Sets how the socket reacts when a connecting peer presents a routing id
    /// already in use; see [`RidDuplicatePolicy`].
    pub fn set_rid_duplicate_policy(&self, value: RidDuplicatePolicy) -> Result<(), ConfigError> {
        self.inner.set_rid_duplicate_policy(value as i32)
    }
    /// Returns the configured routing-id duplicate policy.
    pub fn rid_duplicate_policy(&self) -> Result<RidDuplicatePolicy, ConfigError> {
        let raw = self.inner.rid_duplicate_policy()?;
        Ok(if raw == RidDuplicatePolicy::Handover as i32 {
            RidDuplicatePolicy::Handover
        } else {
            RidDuplicatePolicy::Reject
        })
    }
    /// Sets the time limit for a single connection attempt.
    pub fn set_connect_timeout(&self, d: Duration) -> Result<(), ConfigError> {
        self.inner.set_connect_timeout(d)
    }
    /// Returns the configured connect timeout.
    pub fn connect_timeout(&self) -> Result<Duration, ConfigError> {
        self.inner.connect_timeout()
    }
    /// Sets whether IPv6 connections are enabled.
    pub fn set_ipv6(&self, enabled: bool) -> Result<(), ConfigError> {
        self.inner.set_ipv6(enabled)
    }
    /// Returns whether IPv6 is enabled.
    pub fn ipv6(&self) -> Result<bool, ConfigError> {
        self.inner.ipv6()
    }
    /// Sets whether Nagle's algorithm is disabled (`TCP_NODELAY`) so small
    /// messages are sent without buffering delay.
    pub fn set_tcp_no_delay(&self, enabled: bool) -> Result<(), ConfigError> {
        self.inner.set_tcp_no_delay(enabled)
    }
    /// Returns whether `TCP_NODELAY` is enabled.
    pub fn tcp_no_delay(&self) -> Result<bool, ConfigError> {
        self.inner.tcp_no_delay()
    }
    /// Sets whether TCP keep-alive probes are enabled.
    pub fn set_tcp_keepalive(&self, enabled: bool) -> Result<(), ConfigError> {
        self.inner.set_tcp_keepalive(enabled)
    }
    /// Returns whether TCP keep-alive is enabled.
    pub fn tcp_keepalive(&self) -> Result<bool, ConfigError> {
        self.inner.tcp_keepalive()
    }
    /// Sets the maximum inbound message size in bytes; -1 means no limit.
    pub fn set_max_message_size(&self, bytes: i64) -> Result<(), ConfigError> {
        self.inner.set_max_message_size(bytes)
    }
    /// Returns the configured maximum inbound message size in bytes.
    pub fn max_message_size(&self) -> Result<i64, ConfigError> {
        self.inner.max_message_size()
    }
    /// Sets the maximum length of the queue of pending inbound connections on a
    /// bound endpoint.
    pub fn set_backlog(&self, value: i32) -> Result<(), ConfigError> {
        self.inner.set_backlog(value)
    }
    /// Returns the configured connection backlog.
    pub fn backlog(&self) -> Result<i32, ConfigError> {
        self.inner.backlog()
    }
    /// Sets the base delay before reconnecting a broken connection.
    pub fn set_reconnect_interval(&self, d: Duration) -> Result<(), ConfigError> {
        self.inner.set_reconnect_interval(d)
    }
    /// Returns the configured base reconnect interval.
    pub fn reconnect_interval(&self) -> Result<Duration, ConfigError> {
        self.inner.reconnect_interval()
    }
    /// Sets the maximum delay between reconnection attempts when the reconnect
    /// interval backs off exponentially.
    pub fn set_reconnect_interval_max(&self, d: Duration) -> Result<(), ConfigError> {
        self.inner.set_reconnect_interval_max(d)
    }
    /// Returns the configured maximum reconnect interval.
    pub fn reconnect_interval_max(&self) -> Result<Duration, ConfigError> {
        self.inner.reconnect_interval_max()
    }
    /// Sets whether a submit that fails locally (such as under back-pressure) is
    /// retried; see [`SubmitRetryMode`].
    pub fn set_submit_retry_mode(&self, value: SubmitRetryMode) -> Result<(), ConfigError> {
        self.inner.set_submit_retry_mode(value as i32)
    }
    /// Returns the configured submit retry mode.
    pub fn submit_retry_mode(&self) -> Result<SubmitRetryMode, ConfigError> {
        Ok(match self.inner.submit_retry_mode()? {
            1 => SubmitRetryMode::LocalFailure,
            _ => SubmitRetryMode::Off,
        })
    }
    /// Sets the total time budget for submit retries when retrying is enabled.
    pub fn set_submit_retry_timeout(&self, d: Duration) -> Result<(), ConfigError> {
        self.inner.set_submit_retry_timeout(d)
    }
    /// Returns the configured submit retry timeout.
    pub fn submit_retry_timeout(&self) -> Result<Duration, ConfigError> {
        self.inner.submit_retry_timeout()
    }
    /// Sets the maximum number of submit retry attempts when retrying is
    /// enabled.
    pub fn set_submit_retry_attempts(&self, value: i32) -> Result<(), ConfigError> {
        self.inner.set_submit_retry_attempts(value)
    }
    /// Returns the configured submit retry attempt count.
    pub fn submit_retry_attempts(&self) -> Result<i32, ConfigError> {
        self.inner.submit_retry_attempts()
    }
}

/// Typed facade over ROUTER-specific socket options.
pub struct RouterSocketOptions<'a> {
    inner: &'a SocketStorage,
}

impl<'a> RouterSocketOptions<'a> {
    pub(crate) fn new(inner: &'a SocketStorage) -> Self {
        Self { inner }
    }
    /// Sets whether sending to an unknown route raises an error instead of
    /// silently dropping the message.
    pub fn set_mandatory(&self, enabled: bool) -> Result<(), ConfigError> {
        self.inner.set_mandatory(enabled)
    }
    /// Sets whether to send an empty probe message on connect so the peer
    /// immediately learns this socket's routing id.
    pub fn set_probe(&self, enabled: bool) -> Result<(), ConfigError> {
        self.inner.set_probe(enabled)
    }
    /// Assigns `id` as the routing id of the next connection this socket
    /// initiates, instead of letting the peer choose one.
    pub fn set_connect_routing_id(&self, id: &RoutingId) -> Result<(), ConfigError> {
        self.inner.set_connect_routing_id(id)
    }
    /// Returns this peer's relative load-balancing weight (0-100).
    pub fn weight(&self) -> Result<u32, ConfigError> {
        self.inner.weight()
    }
    /// Sets this peer's relative load-balancing weight (0-100); higher values
    /// receive a larger share of round-robined sends.
    pub fn set_weight(&self, value: u32) -> Result<(), ConfigError> {
        self.inner.set_weight(value)
    }
    /// Returns how long a request issued by this ROUTER waits for a reply.
    pub fn request_timeout(&self) -> Result<Duration, ConfigError> {
        self.inner.request_timeout()
    }
    /// Sets how long a request issued by this ROUTER waits for a reply before
    /// reporting a timeout.
    pub fn set_request_timeout(&self, value: Duration) -> Result<(), ConfigError> {
        self.inner.set_request_timeout(value)
    }
}

/// Typed facade over DEALER-specific socket options.
pub struct DealerSocketOptions<'a> {
    inner: &'a DealerSocket,
}

impl<'a> DealerSocketOptions<'a> {
    pub(crate) fn new(inner: &'a DealerSocket) -> Self {
        Self { inner }
    }
    /// Sets whether to send an empty probe message on connect so the peer
    /// immediately learns this socket's routing id.
    pub fn set_probe(&self, enabled: bool) -> Result<(), ConfigError> {
        self.inner.set_probe(enabled)
    }
    /// Returns this peer's relative load-balancing weight (0-100).
    pub fn weight(&self) -> Result<u32, ConfigError> {
        self.inner.weight()
    }
    /// Sets this peer's relative load-balancing weight (0-100); higher values
    /// receive a larger share of round-robined sends.
    pub fn set_weight(&self, value: u32) -> Result<(), ConfigError> {
        self.inner.set_weight(value)
    }
    /// Sets how long a request issued by this DEALER waits for a reply before
    /// reporting a timeout.
    pub fn set_request_timeout(&self, value: Duration) -> Result<(), ConfigError> {
        self.inner.set_request_timeout(value)
    }
}

/// Typed facade over STREAM-specific socket options.
pub struct StreamSocketOptions<'a> {
    inner: &'a SocketStorage,
}

impl<'a> StreamSocketOptions<'a> {
    pub(crate) fn new(inner: &'a SocketStorage) -> Self {
        Self { inner }
    }
    /// Sets whether peer connect and disconnect events are delivered to the
    /// application as messages.
    pub fn set_notify(&self, enabled: bool) -> Result<(), ConfigError> {
        self.inner.set_notify(enabled)
    }
    /// Returns whether stream connect/disconnect notifications are enabled.
    pub fn notify(&self) -> Result<bool, ConfigError> {
        self.inner.notify()
    }
}

/// Typed facade over PUB/XPUB-specific socket options.
pub struct PubSocketOptions<'a> {
    inner: &'a SocketStorage,
}

impl<'a> PubSocketOptions<'a> {
    pub(crate) fn new(inner: &'a SocketStorage) -> Self {
        Self { inner }
    }

    /// Sets whether every subscription message is delivered to the application,
    /// including duplicates, rather than only the first per topic.
    pub fn set_verbose(&self, enabled: bool) -> Result<(), ConfigError> {
        self.inner.set_verbose(enabled)
    }
    /// Sets whether every subscription and unsubscription message is delivered,
    /// including duplicates.
    pub fn set_verboser(&self, enabled: bool) -> Result<(), ConfigError> {
        self.inner.set_verboser(enabled)
    }
    /// Sets whether a send blocked by back-pressure raises an error instead of
    /// silently dropping the message.
    pub fn set_no_drop(&self, enabled: bool) -> Result<(), ConfigError> {
        self.inner.set_no_drop(enabled)
    }
    /// Sets whether subscriptions are handled manually via [`approve_subscribe`]
    /// and [`reject_subscribe`] instead of being accepted automatically.
    ///
    /// [`approve_subscribe`]: Self::approve_subscribe
    /// [`reject_subscribe`]: Self::reject_subscribe
    pub fn set_manual(&self, enabled: bool) -> Result<(), ConfigError> {
        self.inner.set_manual(enabled)
    }
    /// Returns whether manual-with-last-value subscription handling is enabled.
    pub fn manual_last_value(&self) -> Result<bool, ConfigError> {
        self.inner.manual_last_value()
    }
    /// Sets manual subscription handling that also replays the last cached
    /// message per topic to a newly accepted subscriber.
    pub fn set_manual_last_value(&self, enabled: bool) -> Result<(), ConfigError> {
        self.inner.set_manual_last_value(enabled)
    }
    /// Returns the welcome message sent to new subscribers. The caller owns the
    /// returned message.
    pub fn welcome_message(&self) -> Result<Message, ConfigError> {
        self.inner.welcome_message()
    }
    /// Sets a message automatically sent to each newly connected subscriber.
    pub fn set_welcome_message(&self, message: &Message) -> Result<(), ConfigError> {
        self.inner.set_welcome_message(message)
    }
    /// Accepts a pending subscription from `routing_id`. Requires manual mode.
    pub fn approve_subscribe(&self, routing_id: &RoutingId) -> Result<(), ConfigError> {
        self.inner.approve_subscribe(routing_id)
    }
    /// Declines a pending subscription from `routing_id`. Requires manual mode.
    pub fn reject_subscribe(&self, routing_id: &RoutingId) -> Result<(), ConfigError> {
        self.inner.reject_subscribe(routing_id)
    }
    /// Returns the number of distinct topics currently subscribed across
    /// connected subscribers.
    pub fn topics_count(&self) -> Result<i32, ConfigError> {
        self.inner.pub_topics_count()
    }
}

/// Typed facade over SUB-specific socket options.
pub struct SubSocketOptions<'a> {
    inner: &'a SocketStorage,
}

impl<'a> SubSocketOptions<'a> {
    pub(crate) fn new(inner: &'a SocketStorage) -> Self {
        Self { inner }
    }

    /// Returns the number of active subscriptions on this socket.
    pub fn topics_count(&self) -> Result<i32, ConfigError> {
        self.inner.sub_topics_count()
    }
}
