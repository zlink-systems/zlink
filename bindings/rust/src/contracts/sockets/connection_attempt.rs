// SPDX-License-Identifier: MPL-2.0

/// State of one outbound connection registration.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum ConnectionAttemptState {
    /// No success or failure result yet.
    Connecting,
    /// The registration's Core connection is ready.
    Connected,
    /// No ready connection exists and the latest attempt failed.
    AttemptFailed,
}

/// Why a connection attempt failed.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum ConnectionFailureKind {
    None,
    Refused,
    EndpointMissing,
    TimedOut,
    Unreachable,
    NameResolution,
    AuthFailed,
    ProtocolFailed,
    ConnectionLost,
    Other,
}

/// Where in the connect sequence the failure happened.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum ConnectionFailureStage {
    None,
    Resolve,
    Transport,
    Tls,
    WebSocket,
    CoreHandshake,
    Established,
}

/// The most recent failure of a registration, kept after recovery.
///
/// `normalized_errno` and `disconnect_reason` are diagnostics.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct ConnectionFailure {
    pub kind: ConnectionFailureKind,
    pub stage: ConnectionFailureStage,
    pub normalized_errno: i32,
    pub disconnect_reason: u32,
}

/// One row of a connection-attempts snapshot, one per outbound registration.
///
/// `intent_id`, `revision` and `last_failure_id` are opaque equality tokens;
/// `last_failure_id` is 0 when there is no failure history.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ConnectionAttempt {
    pub endpoint: String,
    pub intent_id: u64,
    pub revision: u64,
    pub last_failure_id: u64,
    pub state: ConnectionAttemptState,
    pub last_failure: ConnectionFailure,
}
