// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Observes a socket's connection lifecycle events and current status.
/// </summary>
public interface ISocketMonitor : IDisposable, IAsyncDisposable
{
    /// <summary>
    ///     Registers a callback invoked for each monitor event. The callback runs
    ///     on a background dispatch thread.
    /// </summary>
    void OnEvent(Action<MonitorEvent> handler);

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
/// <param name="Value">An event-specific value, such as an error code or a reconnect interval.</param>
/// <param name="RoutingId">The peer routing id, when the event carries one.</param>
/// <param name="LocalAddr">The local endpoint address.</param>
/// <param name="RemoteAddr">The remote endpoint address.</param>
public sealed record MonitorEvent(
    MonitorEventType Event,
    uint Value,
    RoutingId? RoutingId,
    string LocalAddr,
    string RemoteAddr);

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
    ///     Gets the automatic high water mark unit budget bytes.
    /// </summary>
    public ulong AutoHwmUnitBudgetBytes { get; }

    /// <summary>
    ///     Gets the automatic high water mark size cap.
    /// </summary>
    public uint AutoHwmSizeCap { get; }

    /// <summary>
    ///     Gets the automatic high water mark socket message slots.
    /// </summary>
    public ulong AutoHwmSocketMessageSlots { get; }

    /// <summary>
    ///     Gets whether a connection-count bucket applied to this socket.
    /// </summary>
    public bool AutoHwmConnectionBucketEnabled { get; }

    /// <summary>
    ///     Gets the peer count used by the connection bucket planner.
    /// </summary>
    public uint AutoHwmConnectionBucketCount { get; }

    /// <summary>
    ///     Gets the selected connection bucket index.
    /// </summary>
    public uint AutoHwmConnectionBucketIndex { get; }

    /// <summary>
    ///     Gets the selected bucket HWM for a 4 KiB message unit.
    /// </summary>
    public uint AutoHwmConnectionBucketHwm4K { get; }

    /// <summary>
    ///     Gets whether hysteresis retained the previous connection bucket.
    /// </summary>
    public bool AutoHwmConnectionBucketHysteresisRetained { get; }

    /// <summary>
    ///     Gets the automatic high water mark effective message bytes.
    /// </summary>
    public ulong AutoHwmEffectiveMessageBytes { get; }

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
    ///     Gets whether the monitored socket source is in the ready state.
    /// </summary>
    public bool IsReady => SourceKind == MonitorSourceKind.Socket
                           && (StateFlags & MonitorStateFlags.Ready) != 0;
}
