// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Connection lifecycle events that a monitor can be subscribed to. Combine as
///     flags to select which events are delivered.
/// </summary>
[Flags]
public enum SocketEvent
{
    /// <summary>
    ///     A connection to a peer was established.
    /// </summary>
    Connected = 0x0001,

    /// <summary>
    ///     An asynchronous connect is still in progress.
    /// </summary>
    ConnectDelayed = 0x0002,

    /// <summary>
    ///     A failed connect will be retried after a delay.
    /// </summary>
    ConnectRetried = 0x0004,

    /// <summary>
    ///     The socket began listening on a bound endpoint.
    /// </summary>
    Listening = 0x0008,

    /// <summary>
    ///     Binding to an endpoint failed.
    /// </summary>
    BindFailed = 0x0010,

    /// <summary>
    ///     An inbound connection was accepted.
    /// </summary>
    Accepted = 0x0020,

    /// <summary>
    ///     Accepting an inbound connection failed.
    /// </summary>
    AcceptFailed = 0x0040,

    /// <summary>
    ///     A connection was closed.
    /// </summary>
    Closed = 0x0080,

    /// <summary>
    ///     Closing a connection failed.
    /// </summary>
    CloseFailed = 0x0100,

    /// <summary>
    ///     A peer disconnected.
    /// </summary>
    Disconnected = 0x0200,

    /// <summary>
    ///     Monitoring of the socket has stopped.
    /// </summary>
    MonitorStopped = 0x0400,

    /// <summary>
    ///     The connection handshake failed without further detail.
    /// </summary>
    HandshakeFailedNoDetail = 0x0800,

    /// <summary>
    ///     The connection completed its handshake and is ready for traffic.
    /// </summary>
    ConnectionReady = 0x1000,

    /// <summary>
    ///     The handshake failed due to a protocol error.
    /// </summary>
    HandshakeFailedProtocol = 0x2000,

    /// <summary>
    ///     The handshake failed authentication.
    /// </summary>
    HandshakeFailedAuth = 0x4000,

    /// <summary>
    ///     A peer's load-balancing weight changed.
    /// </summary>
    PeerWeightChanged = 0x8000,

    /// <summary>
    ///     A remote PAUSE was first applied to an application pipe over the
    ///     paired DEALER/ROUTER completion lane. <c>Value</c> carries the
    ///     flow epoch.
    /// </summary>
    SendFlowPaused = 0x10000,

    /// <summary>
    ///     A remote RUNNING cleared the remote-pause cause for a pipe.
    ///     <c>Value</c> carries the flow epoch; check
    ///     <see cref="MonitorEventFlags.SendFlowWritable" /> in
    ///     <see cref="MonitorEvent.Flags" /> for whether the pipe is
    ///     actually writable now.
    /// </summary>
    SendFlowResumed = 0x20000,

    /// <summary>
    ///     A stale or duplicate completion-lane flow-state frame was
    ///     rejected. <see cref="MonitorEvent.Flags" /> disambiguates the
    ///     stale reason and <see cref="MonitorEvent.Value" /> carries the
    ///     corresponding rejected field.
    /// </summary>
    FlowStateStale = 0x40000,

    /// <summary>
    ///     Every event; subscribes the monitor to all of the above.
    /// </summary>
    All = 0x7FFFF
}

/// <summary>
///     The kind of a delivered <see cref="MonitorEvent" />; mirrors the lifecycle
///     events of <see cref="SocketEvent" />.
/// </summary>
public enum MonitorEventType
{
    /// <summary>
    ///     A connection to a peer was established.
    /// </summary>
    Connected = 0x0001,

    /// <summary>
    ///     An asynchronous connect is still in progress.
    /// </summary>
    ConnectDelayed = 0x0002,

    /// <summary>
    ///     A failed connect will be retried after a delay.
    /// </summary>
    ConnectRetried = 0x0004,

    /// <summary>
    ///     The socket began listening on a bound endpoint.
    /// </summary>
    Listening = 0x0008,

    /// <summary>
    ///     Binding to an endpoint failed.
    /// </summary>
    BindFailed = 0x0010,

    /// <summary>
    ///     An inbound connection was accepted.
    /// </summary>
    Accepted = 0x0020,

    /// <summary>
    ///     Accepting an inbound connection failed.
    /// </summary>
    AcceptFailed = 0x0040,

    /// <summary>
    ///     A connection was closed.
    /// </summary>
    Closed = 0x0080,

    /// <summary>
    ///     Closing a connection failed.
    /// </summary>
    CloseFailed = 0x0100,

    /// <summary>
    ///     A peer disconnected.
    /// </summary>
    Disconnected = 0x0200,

    /// <summary>
    ///     Monitoring of the socket has stopped.
    /// </summary>
    MonitorStopped = 0x0400,

    /// <summary>
    ///     The connection handshake failed without further detail.
    /// </summary>
    HandshakeFailedNoDetail = 0x0800,

    /// <summary>
    ///     The connection completed its handshake and is ready for traffic.
    /// </summary>
    ConnectionReady = 0x1000,

    /// <summary>
    ///     The handshake failed due to a protocol error.
    /// </summary>
    HandshakeFailedProtocol = 0x2000,

    /// <summary>
    ///     The handshake failed authentication.
    /// </summary>
    HandshakeFailedAuth = 0x4000,

    /// <summary>
    ///     A peer's load-balancing weight changed.
    /// </summary>
    PeerWeightChanged = 0x8000,

    /// <summary>
    ///     A remote PAUSE was first applied to an application pipe over the
    ///     paired DEALER/ROUTER completion lane. <see cref="MonitorEvent.Value" />
    ///     carries the flow epoch.
    /// </summary>
    SendFlowPaused = 0x10000,

    /// <summary>
    ///     A remote RUNNING cleared the remote-pause cause for a pipe.
    ///     <see cref="MonitorEvent.Value" /> carries the flow epoch; check
    ///     <see cref="MonitorEventFlags.SendFlowWritable" /> in
    ///     <see cref="MonitorEvent.Flags" /> for whether the pipe is
    ///     actually writable now.
    /// </summary>
    SendFlowResumed = 0x20000,

    /// <summary>
    ///     A stale or duplicate completion-lane flow-state frame was
    ///     rejected. <see cref="MonitorEvent.Flags" /> disambiguates the
    ///     stale reason and <see cref="MonitorEvent.Value" /> carries the
    ///     corresponding rejected field.
    /// </summary>
    FlowStateStale = 0x40000
}

/// <summary>
///     Bits that can appear in <see cref="MonitorEvent.Flags" />. Mirrors
///     <c>ZLINK_MONITOR_EVENT_FLAG_*</c> in the C ABI. Combine as flags.
/// </summary>
[Flags]
public enum MonitorEventFlags : uint
{
    /// <summary>
    ///     No event-specific flags are set.
    /// </summary>
    None = 0,

    /// <summary>
    ///     Set on a <see cref="MonitorEventType.ConnectionReady" /> event
    ///     that moves a connection from not-ready to ready.
    /// </summary>
    ConnectionReadyEdge = 1u << 0,

    /// <summary>
    ///     Set on <see cref="MonitorEventType.SendFlowResumed" /> when
    ///     clearing the remote pause left the pipe actually writable. Clear
    ///     when another cause (byte high-water-mark, transport wait, or
    ///     termination) still blocks it.
    /// </summary>
    SendFlowWritable = 1u << 1,

    /// <summary>
    ///     Set on <see cref="MonitorEventType.FlowStateStale" /> when the
    ///     frame named a different connection generation.
    ///     <see cref="MonitorEvent.Value" /> then carries the received
    ///     generation, and <see cref="MonitorEvent.ConnectionId" />
    ///     carries the current one.
    /// </summary>
    FlowStateStaleGeneration = 1u << 2,

    /// <summary>
    ///     Set on <see cref="MonitorEventType.FlowStateStale" /> when the
    ///     epoch did not advance inside the current generation.
    ///     <see cref="MonitorEvent.Value" /> then carries the received
    ///     epoch; the current epoch is the one reported by the preceding
    ///     <see cref="MonitorEventType.SendFlowPaused" /> or
    ///     <see cref="MonitorEventType.SendFlowResumed" /> event for the
    ///     same pair.
    /// </summary>
    FlowStateStaleEpoch = 1u << 3
}

/// <summary>
///     Identifies what a monitored source is.
/// </summary>
public enum MonitorSourceKind
{
    /// <summary>
    ///     A plain socket.
    /// </summary>
    Socket = 1
}

/// <summary>
///     State bits reported by a monitor status snapshot.
/// </summary>
[Flags]
public enum MonitorStateFlags : uint
{
    /// <summary>
    ///     No monitor state bits are set.
    /// </summary>
    None = 0,

    /// <summary>
    ///     The monitored source is ready for traffic.
    /// </summary>
    Ready = 1u << 0,

    /// <summary>
    ///     The monitored source has a ready bound endpoint.
    /// </summary>
    BoundReady = 1u << 1,

    /// <summary>
    ///     The monitored source is closed.
    /// </summary>
    Closed = 1u << 3
}

/// <summary>
///     Detail bits describing which monitor status fields are populated.
/// </summary>
[Flags]
public enum MonitorStatusDetailFlags : uint
{
    /// <summary>
    ///     No detail fields are marked as populated.
    /// </summary>
    None = 0,

    /// <summary>
    ///     Send pending message telemetry is populated.
    /// </summary>
    SendPendingMessages = 1u << 1,

    /// <summary>
    ///     Receive pending message telemetry is populated.
    /// </summary>
    ReceivePendingMessages = 1u << 2,

    /// <summary>
    ///     Automatic high-water-mark budget telemetry is populated.
    /// </summary>
    AutoHwmBudget = 1u << 3,

    /// <summary>
    ///     Automatic high-water-mark buffer telemetry is populated.
    /// </summary>
    AutoHwmBuffers = 1u << 4,

    /// <summary>
    ///     <see cref="MonitorStatus.FlowPausedConnections" /> and the other
    ///     flow-state fields are populated (paired DEALER/ROUTER sockets
    ///     only).
    /// </summary>
    FlowState = 1u << 5
}

/// <summary>
///     Reason for the last automatic high-water-mark recalculation.
/// </summary>
public enum AutoHwmRecalcReason : uint
{
    /// <summary>
    ///     No recalculation has been recorded.
    /// </summary>
    None = 0,

    /// <summary>
    ///     The initial plan was calculated.
    /// </summary>
    Initial = 1,

    /// <summary>
    ///     The socket role changed.
    /// </summary>
    RoleChange = 2,

    /// <summary>
    ///     The policy was enabled or disabled.
    /// </summary>
    PolicyToggle = 3,

    /// <summary>
    ///     The plan was refreshed.
    /// </summary>
    Refresh = 4,

    /// <summary>
    ///     A shrink was deferred to avoid abrupt queue reduction.
    /// </summary>
    DeferredShrink = 5
}

/// <summary>
///     Identifies what kind of source a poll event came from.
/// </summary>
public enum PollSourceKind
{
    /// <summary>
    ///     A socket.
    /// </summary>
    Socket = 1,

    /// <summary>
    ///     A raw file descriptor.
    /// </summary>
    Fd = 2,

    /// <summary>
    ///     A timer.
    /// </summary>
    Timer = 3
}

/// <summary>
///     Readiness conditions a poll source can be watched for or report. Combine as
///     flags.
/// </summary>
public enum PollEventFlags
{
    /// <summary>
    ///     No events.
    /// </summary>
    None = 0,

    /// <summary>
    ///     Readable: a receive will not block.
    /// </summary>
    PollIn = 1,

    /// <summary>
    ///     Writable: a send will not block.
    /// </summary>
    PollOut = 2,

    /// <summary>
    ///     An error condition occurred on the source.
    /// </summary>
    PollErr = 4,

    /// <summary>
    ///     Priority/out-of-band data is available.
    /// </summary>
    PollPri = 8,

    /// <summary>
    ///     An asynchronous operation completed.
    /// </summary>
    PollCompletion = 32
}
