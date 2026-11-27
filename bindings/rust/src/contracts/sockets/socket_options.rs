use crate::contracts::sockets::dealer_socket::DealerSocket;
use crate::internal::SocketStorage;

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
    /// Retry selected local connection failures. DONTWAIT backpressure is
    /// reported as EAGAIN and is not covered by this Core option.
    LocalFailure = 1,
}

/// Receive-flow state for DEALER/ROUTER sockets. Control uses the Application
/// connection for count-1 peers and the Completion connection for count-2
/// ROUTER-ROUTER peers.
/// (core-byte-hwm-flow-control-plan.ko.md §5). RUNNING and PAUSED are an
/// absolute socket-wide state, not a counter: repeating the current state
/// succeeds as a no-op. Values match the C ABI (`zlink_receive_flow_state_t`).
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum ReceiveFlowState {
    /// The socket accepts data normally.
    Running = 0,
    /// The socket has signalled its paired peer to stop sending data.
    Paused = 1,
}

/// Selects raw-message or framed-packet STREAM receive semantics.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StreamRecvMode {
    Unspecified = 0,
    Raw = 1,
    Packet = 2,
}

/// Typed facade over the socket options shared by every socket type.
pub struct CommonSocketOptions<'a> {
    pub(crate) inner: &'a SocketStorage,
}

/// Typed facade over ROUTER-specific socket options.
pub struct RouterSocketOptions<'a> {
    pub(crate) inner: &'a SocketStorage,
}

/// Typed facade over DEALER-specific socket options.
pub struct DealerSocketOptions<'a> {
    pub(crate) inner: &'a DealerSocket,
}

/// Typed facade over STREAM-specific socket options.
pub struct StreamSocketOptions<'a> {
    pub(crate) inner: &'a SocketStorage,
}

/// Typed facade over PUB/XPUB-specific socket options.
pub struct PubSocketOptions<'a> {
    pub(crate) inner: &'a SocketStorage,
}

/// Typed facade over SUB-specific socket options.
pub struct SubSocketOptions<'a> {
    pub(crate) inner: &'a SocketStorage,
}
