// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Observes a socket's connection lifecycle events and current status.
/// </summary>
public interface ISocketMonitor : IDisposable, IAsyncDisposable
{
    /// <summary>
    ///     Receives the next monitor event, or null when none is pending and
    ///     <see cref="RecvFlags.DontWait" /> is set.
    /// </summary>
    MonitorEvent? Recv(RecvFlags flags = RecvFlags.None);

    /// <summary>
    ///     Returns a snapshot of the monitored socket's current status.
    /// </summary>
    MonitorStatus Status();

    /// <summary>
    ///     Closes the monitor and releases its resources.
    /// </summary>
    void Close();
}

/// <summary>
///     A single socket connection-lifecycle event reported by a monitor.
/// </summary>
/// <param name="Event">The kind of lifecycle event.</param>
/// <param name="Value">
///     An event-specific value, such as an error code, a reconnect
///     interval, or (for the flow events) a flow epoch or a rejected
///     generation/epoch — see <see cref="MonitorEventType.SendFlowPaused" />,
///     <see cref="MonitorEventType.SendFlowResumed" />, and
///     <see cref="MonitorEventType.FlowStateStale" />. Carries the full
///     64-bit native value.
/// </param>
/// <param name="RoutingId">The peer routing id, when the event carries one.</param>
/// <param name="LocalAddr">The local endpoint address.</param>
/// <param name="RemoteAddr">The remote endpoint address.</param>
/// <param name="ConnectionId">The native connection identifier.</param>
/// <param name="TransportLane">The transport lane that emitted the event.</param>
/// <param name="Flags">Event-specific flags; see <see cref="MonitorEventFlags" />.</param>
public sealed record MonitorEvent(
    MonitorEventType Event,
    ulong Value,
    RoutingId? RoutingId,
    string LocalAddr,
    string RemoteAddr,
    ulong ConnectionId,
    uint TransportLane,
    MonitorEventFlags Flags);

/// <summary>
///     A snapshot of a socket's monitored state and auto-high-water-mark telemetry.
/// </summary>
public sealed partial class MonitorStatus
{
    /// <summary>
    ///     Gets the native monitor snapshot ABI version.
    /// </summary>
    public uint AbiVersion { get; }

    /// <summary>
    ///     Gets the native snapshot structure size in bytes.
    /// </summary>
    public uint StructSize { get; }

    /// <summary>
    ///     Gets the source kind.
    /// </summary>
    public MonitorSourceKind SourceKind { get; }

    /// <summary>
    ///     Gets the state flags.
    /// </summary>
    public MonitorStateFlags StateFlags { get; }

    /// <summary>
    ///     Gets the detail flags.
    /// </summary>
    public MonitorStatusDetailFlags DetailFlags { get; }

    /// <summary>
    ///     Gets the send pending message count.
    /// </summary>
    public ulong SndPendingMsgs { get; }

    /// <summary>
    ///     Gets the receive pending message count.
    /// </summary>
    public ulong RcvPendingMsgs { get; }

    /// <summary>
    ///     Gets the send pending byte count.
    /// </summary>
    public ulong SndPendingBytes { get; }

    /// <summary>
    ///     Gets the receive pending byte count.
    /// </summary>
    public ulong RcvPendingBytes { get; }

    /// <summary>
    ///     Gets the automatic high water mark enabled.
    /// </summary>
    public bool AutoHwmEnabled { get; }

    /// <summary>
    ///     Gets the automatic high water mark profile.
    /// </summary>
    public AutoHwmProfile AutoHwmProfile { get; }

    /// <summary>
    ///     Gets the automatic high water mark role.
    /// </summary>
    public uint AutoHwmRole { get; }

    /// <summary>
    ///     Gets the automatic high water mark policy class.
    /// </summary>
    public uint AutoHwmPolicyClass { get; }

    /// <summary>
    ///     Gets the send HWM selected by the current automatic plan, in
    ///     accounted bytes.
    /// </summary>
    public ulong AutoHwmPlannedSendHighWaterMarkBytes { get; }

    /// <summary>
    ///     Gets the receive HWM selected by the current automatic plan, in
    ///     accounted bytes.
    /// </summary>
    public ulong AutoHwmPlannedReceiveHighWaterMarkBytes { get; }

    /// <summary>
    ///     Gets the send HWM currently applied to the socket, in accounted
    ///     bytes.
    /// </summary>
    public ulong AutoHwmAppliedSendHighWaterMarkBytes { get; }

    /// <summary>
    ///     Gets the receive HWM currently applied to the socket, in accounted
    ///     bytes.
    /// </summary>
    public ulong AutoHwmAppliedReceiveHighWaterMarkBytes { get; }

    /// <summary>
    ///     Gets the automatic high water mark effective sndbuf.
    /// </summary>
    public int AutoHwmEffectiveSndbuf { get; }

    /// <summary>
    ///     Gets the automatic high water mark effective rcvbuf.
    /// </summary>
    public int AutoHwmEffectiveRcvbuf { get; }

    /// <summary>
    ///     Gets the automatic high water mark last recalc ms.
    /// </summary>
    public ulong AutoHwmLastRecalcMs { get; }

    /// <summary>
    ///     Gets the automatic high water mark last recalc reason.
    /// </summary>
    public AutoHwmRecalcReason AutoHwmLastRecalcReason { get; }

    /// <summary>
    ///     Gets the automatic high water mark send blocked ratio ppm.
    /// </summary>
    public uint AutoHwmSendBlockedRatioPpm { get; }

    /// <summary>
    ///     Gets the target send HWM while a shrink is deferred, in accounted
    ///     bytes.
    /// </summary>
    public ulong AutoHwmDeferredSendHighWaterMarkBytes { get; }

    /// <summary>
    ///     Gets the target receive HWM while a shrink is deferred, in accounted
    ///     bytes.
    /// </summary>
    public ulong AutoHwmDeferredReceiveHighWaterMarkBytes { get; }

    /// <summary>
    ///     Gets whether the deferred send HWM value is valid.
    /// </summary>
    public bool AutoHwmDeferredSendHighWaterMarkValid { get; }

    /// <summary>
    ///     Gets whether the deferred receive HWM value is valid.
    /// </summary>
    public bool AutoHwmDeferredReceiveHighWaterMarkValid { get; }

    /// <summary>
    ///     Gets the bytes retained by outbound pipe directions.
    /// </summary>
    public ulong SendBytesInFlight { get; }

    /// <summary>
    ///     Gets the bytes retained by inbound pipe directions.
    /// </summary>
    public ulong ReceiveBytesInFlight { get; }

    /// <summary>
    ///     Gets the minimum accounted charge for one Core frame.
    /// </summary>
    public ulong MinimumCoreMessageChargeBytes { get; }

    /// <summary>
    ///     Gets the number of messages admitted by the empty-pipe oversize rule.
    /// </summary>
    public ulong OversizeMessageAdmissionCount { get; }

    /// <summary>
    ///     Gets the largest accounted message admitted by the empty-pipe
    ///     oversize rule.
    /// </summary>
    public ulong OversizeMessageAdmissionMaxBytes { get; }

    /// <summary>
    ///     Gets the current count of application pipes this socket sees as
    ///     remote-PAUSED for affected Application pipes.
    /// </summary>
    public ulong FlowPausedConnections { get; }

    /// <summary>
    ///     Gets the total number of PAUSED transitions actually applied
    ///     (never a stale or duplicate frame).
    /// </summary>
    public ulong FlowPauseAppliedTotal { get; }

    /// <summary>
    ///     Gets the total number of RUNNING transitions actually applied
    ///     (never a stale or duplicate frame).
    /// </summary>
    public ulong FlowResumeAppliedTotal { get; }

    /// <summary>
    ///     Gets the total number of stale or duplicate flow-state frames
    ///     ignored.
    /// </summary>
    public ulong FlowStateStaleTotal { get; }

    /// <summary>
    ///     Gets the duration of the most recently completed PAUSED interval,
    ///     in milliseconds. Zero if no PAUSED interval has completed yet.
    /// </summary>
    public ulong FlowPauseDurationMs { get; }

    /// <summary>
    ///     Gets whether the monitored socket source is in the ready state.
    /// </summary>
    public bool IsReady => SourceKind == MonitorSourceKind.Socket
                           && (StateFlags & MonitorStateFlags.Ready) != 0;
}
