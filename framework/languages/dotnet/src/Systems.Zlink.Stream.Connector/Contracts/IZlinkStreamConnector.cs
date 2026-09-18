namespace Systems.Zlink.Stream.Connector.Contracts;

/// <summary>
///     Represents a client-side stream connection to a ZLink stream endpoint.
/// </summary>
/// <remarks>
///     <see cref="On" /> is the normal callback path for long-lived push handling.
///     <see cref="WaitFor" /> is a
///     deterministic wait path for samples, command-line flows, and e2e scenario
///     tests.
/// </remarks>
public interface IZlinkStreamConnector : IAsyncDisposable
{
    /// <summary>
    ///     Gets whether the connector is currently connected.
    /// </summary>
    bool IsConnected { get; }

    /// <summary>
    ///     Gets the current connection state.
    /// </summary>
    ZlinkStreamConnectionState State { get; }

    /// <summary>
    ///     Gets the reason the connection last ended, or <see langword="null" /> when this
    ///     connector has never lost a connection.
    /// </summary>
    /// <remarks>
    ///     Readable at any time, so code that missed the disconnect event still reads the
    ///     same value (stream-connector spec §6.2, .NET spec §10). A first connect that
    ///     fails also leaves a reason here — <see cref="ZlinkStreamErrorCode.ConnectTimeout" />
    ///     and <see cref="ZlinkStreamErrorCode.TlsValidationFailed" /> record
    ///     <see cref="ZlinkStreamCloseReason.TransportError" />. Reconnecting does not clear
    ///     the value; it keeps the reason of the last close.
    /// </remarks>
    ZlinkStreamCloseReason? CloseReason { get; }

    /// <summary>
    ///     Gets the options used by this connector. <see cref="ZlinkStreamConnectorOptions.DiagnosticsLevel" />
    ///     on this instance always reflects the level most recently applied through
    ///     <see cref="SetDiagnosticsLevel" />.
    /// </summary>
    ZlinkStreamConnectorOptions Options { get; }

    /// <summary>
    ///     Gets the connector's current diagnostics level.
    /// </summary>
    /// <remarks>
    ///     Equivalent to reading <see cref="ZlinkStreamConnectorOptions.DiagnosticsLevel" /> off
    ///     <see cref="Options" />. The connector never re-derives this value mid-processing:
    ///     each processing point (building an outbound frame, dispatching an inbound one)
    ///     reads it exactly once at the start of that operation.
    /// </remarks>
    ZlinkStreamDiagnosticsLevel DiagnosticsLevel { get; }

    /// <summary>
    ///     Atomically changes the connector's diagnostics level while it keeps running,
    ///     without recreating the connector.
    /// </summary>
    /// <remarks>
    ///     The change applies to processing points that start after this call returns;
    ///     frames already built or already being dispatched are not revisited
    ///     (stream-connector spec §13, following server spec §26 §4.1). Request
    ///     correlation ids are kept at every level regardless of this setting. The level
    ///     is stored on <see cref="Options" />, so connectors that were constructed from
    ///     the same <see cref="ZlinkStreamConnectorOptions" /> instance share it.
    /// </remarks>
    /// <exception cref="ZlinkStreamException">
    ///     <paramref name="level" /> is not one of the defined <see cref="ZlinkStreamDiagnosticsLevel" /> values.
    /// </exception>
    /// <exception cref="ObjectDisposedException">The connector has already been disposed.</exception>
    /// <remarks>
    ///     This surface writes the value and returns; it never blocks on an asynchronous
    ///     counterpart, so calling it inside a receive callback creates no cycle in which the
    ///     call waits on its own completion (stream-connector spec §13, .NET spec §12).
    /// </remarks>
    void SetDiagnosticsLevel(ZlinkStreamDiagnosticsLevel level);

    /// <summary>
    ///     Asynchronous counterpart of <see cref="SetDiagnosticsLevel" />, provided for the
    ///     .NET idiom. Both surfaces change the same value; this one does not replace the
    ///     synchronous surface.
    /// </summary>
    Task SetDiagnosticsLevelAsync(ZlinkStreamDiagnosticsLevel level);

    /// <summary>
    ///     Gets the number of messages waiting for manual dispatch.
    /// </summary>
    int PendingDispatchCount { get; }

    /// <summary>
    ///     Starts the stream connection lifecycle operation.
    /// </summary>
    IZlinkStreamLifecycleCall Connect { get; }

    /// <summary>
    ///     Starts the stream close lifecycle operation.
    /// </summary>
    IZlinkStreamLifecycleCall Close { get; }

    /// <summary>
    ///     Starts a manual dispatch lifecycle operation.
    /// </summary>
    IZlinkStreamLifecycleCall Dispatch { get; }

    /// <summary>
    ///     Registers a handler invoked when the endpoint sends an error message.
    /// </summary>
    /// <remarks>
    ///     Connection events are registration methods rather than C# events because an
    ///     <c>event</c> hands back nothing to unsubscribe with (stream-connector spec §7,
    ///     .NET spec §3). Dispose the returned registration to remove the handler;
    ///     disposing it twice is not an error, and a removed handler runs on no later
    ///     dispatch.
    /// </remarks>
    IDisposable OnErrorReceived(Func<ZlinkStreamError, CancellationToken, ValueTask> handler);

    /// <summary>
    ///     Registers a handler invoked after the connector becomes disconnected.
    /// </summary>
    /// <remarks>
    ///     Dispose the returned registration to remove the handler.
    /// </remarks>
    IDisposable OnDisconnected(Func<ZlinkStreamDisconnected, CancellationToken, ValueTask> handler);

    /// <summary>
    ///     Registers a handler invoked when the connection state changes.
    /// </summary>
    /// <remarks>
    ///     Dispose the returned registration to remove the handler.
    /// </remarks>
    IDisposable OnConnectionStateChanged(
        Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask> handler);

    /// <summary>
    ///     Gets the number of messages received with <paramref name="name" /> on the
    ///     current connection.
    /// </summary>
    /// <remarks>
    ///     The value counts arrivals, not retained messages: consuming a message through
    ///     a wait surface or dispatching it to an <see cref="On" /> handler leaves the
    ///     count unchanged, and the count is the same in both dispatch modes because it
    ///     advances when the packet arrives (stream-connector spec §10). The baseline is
    ///     the moment a connection is established — every reconnect starts again at zero.
    ///     The value serves scenario assertions and diagnostics, never flow control.
    /// </remarks>
    int ReceivedCount(string name);

    /// <summary>
    ///     Starts a send operation for an encoded payload.
    /// </summary>
    IZlinkStreamSendCall Send(ZlinkStreamEncodedPayload payload);

    /// <summary>
    ///     Starts a request operation for an encoded payload.
    /// </summary>
    IZlinkStreamRequestCall Request(ZlinkStreamEncodedPayload payload);

    /// <summary>
    ///     Registers a callback for messages with the given packet name.
    /// </summary>
    /// <remarks>
    ///     The returned registration removes the callback when disposed. This is the
    ///     normal API for production push handling.
    /// </remarks>
    IDisposable On(
        string name,
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> handler);

    /// <summary>
    ///     Starts a wait operation for the next unread received message with the given packet name.
    /// </summary>
    /// <remarks>
    ///     The matched message is consumed by this wait operation. Use this API for
    ///     deterministic sample, CLI, or e2e scenario flow; production clients
    ///     should normally use <see cref="On" />.
    /// </remarks>
    IZlinkStreamWaitCall WaitFor(string name);

    /// <summary>
    ///     Verifies that no unread message with the given packet name arrives during a configured window.
    /// </summary>
    IZlinkStreamExpectNoneCall ExpectNone(string name);

    /// <summary>
    ///     Waits for messages with the given packet name and verifies their arrival order.
    /// </summary>
    IZlinkStreamSequenceCall WaitForSequence(string name);
}
