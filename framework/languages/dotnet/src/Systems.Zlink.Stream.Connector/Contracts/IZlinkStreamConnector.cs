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
    ///     Gets the options used by this connector.
    /// </summary>
    ZlinkStreamConnectorOptions Options { get; }

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
    ///     Raised when the endpoint sends an error message.
    /// </summary>
    event Func<ZlinkStreamError, CancellationToken, ValueTask>? ErrorReceived;

    /// <summary>
    ///     Raised after the connector becomes disconnected.
    /// </summary>
    event Func<ZlinkStreamDisconnected, CancellationToken, ValueTask>? Disconnected;

    /// <summary>
    ///     Raised when the connection state changes.
    /// </summary>
    event Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask>? ConnectionStateChanged;

    /// <summary>
    ///     Gets the number of received messages with <paramref name="name" />.
    /// </summary>
    /// <remarks>
    ///     The count represents messages still retained in the bounded unread
    ///     history. A wait operation removes the message it consumes. The value is
    ///     intended for diagnostics and scenario assertions rather than production
    ///     flow control.
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
    ///     Registers an observer for immutable inbound frame snapshots.
    /// </summary>
    /// <remarks>
    ///     Observers are for logging, metrics, and tracing. They cannot drop,
    ///     transform, or reply to messages, and they must not call connector
    ///     send, request, wait, or dispatch APIs from the callback. Registration is
    ///     only supported before the connector starts connecting.
    /// </remarks>
    IDisposable ObserveInbound(
        Func<ZlinkStreamInboundObservation, CancellationToken, ValueTask> observer);

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
