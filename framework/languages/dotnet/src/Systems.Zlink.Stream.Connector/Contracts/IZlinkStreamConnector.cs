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

    /// <summary>Gets the options used by this connector.</summary>
    ZlinkStreamConnectorOptions Options { get; }

    /// <summary>
    ///     Gets the number of messages waiting for manual dispatch.
    /// </summary>
    int PendingDispatchCount { get; }

    IReadOnlyList<IZlinkStreamActor> Actors { get; }

    IZlinkStreamActor? Actor(string actorId);

    IDisposable OnActorBound(Func<IZlinkStreamActor, CancellationToken, ValueTask> handler);

    IDisposable OnActorUnbound(Func<IZlinkStreamActor, CancellationToken, ValueTask> handler);

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
        Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask> handler
    );

    IDisposable OnRequestSending(Action<ZlinkStreamRequestSendingContext> handler);

    IDisposable OnReplyReceived(
        Func<ZlinkStreamReplyReceivedContext, CancellationToken, ValueTask> handler
    );

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
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> handler
    );

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

public interface IZlinkStreamActor
{
    string ActorId { get; }

    bool IsBound { get; }

    IZlinkStreamSendCall Send(ZlinkStreamEncodedPayload payload);

    IZlinkStreamRequestCall Request(ZlinkStreamEncodedPayload payload);

    IDisposable On(
        string name,
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> handler
    );
}
