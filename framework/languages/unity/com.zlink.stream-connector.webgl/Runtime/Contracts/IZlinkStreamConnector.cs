using System;
using System.Threading;
using System.Threading.Tasks;
using Systems.Zlink.Stream.Connector.Contracts.Calls;

namespace Systems.Zlink.Stream.Connector.Contracts
{
    /// <summary>
    ///     Represents a client-side stream connection to a ZLink stream endpoint.
    /// </summary>
    /// <remarks>
    ///     Identical to the native .NET interface of the same name, minus
    ///     <c>SetDiagnosticsLevelAsync</c>; see the package README for the full list of
    ///     differences. <see cref="On" /> is the normal callback path for long-lived push
    ///     handling. <see cref="WaitFor" /> is a deterministic wait path for samples,
    ///     command-line flows, and e2e scenario tests.
    /// </remarks>
    public interface IZlinkStreamConnector : IAsyncDisposable
    {
        /// <summary>Gets whether the connector is currently connected.</summary>
        bool IsConnected { get; }

        /// <summary>Gets the current connection state.</summary>
        ZlinkStreamConnectionState State { get; }

        /// <summary>Gets the options used by this connector.</summary>
        ZlinkStreamConnectorOptions Options { get; }

        /// <summary>Gets the connector's current diagnostics level.</summary>
        ZlinkStreamDiagnosticsLevel DiagnosticsLevel { get; }

        /// <summary>
        ///     Changes the connector's diagnostics level while it keeps running.
        /// </summary>
        /// <exception cref="ZlinkStreamException">
        ///     <paramref name="level" /> is not one of the defined values.
        /// </exception>
        void SetDiagnosticsLevel(ZlinkStreamDiagnosticsLevel level);

        /// <summary>Changes the connector's diagnostics level asynchronously.</summary>
        Task SetDiagnosticsLevelAsync(ZlinkStreamDiagnosticsLevel level);

        /// <summary>Gets the number of messages waiting for manual dispatch.</summary>
        int PendingDispatchCount { get; }

        /// <summary>Starts the stream connection lifecycle operation.</summary>
        IZlinkStreamLifecycleCall Connect { get; }

        /// <summary>Starts the stream close lifecycle operation.</summary>
        IZlinkStreamLifecycleCall Close { get; }

        /// <summary>Starts a manual dispatch lifecycle operation.</summary>
        IZlinkStreamLifecycleCall Dispatch { get; }

        /// <summary>Raised when the endpoint sends an error message.</summary>
        event Func<ZlinkStreamError, CancellationToken, ValueTask> ErrorReceived;

        /// <summary>Raised after the connector becomes disconnected.</summary>
        event Func<ZlinkStreamDisconnected, CancellationToken, ValueTask> Disconnected;

        /// <summary>Raised when the connection state changes.</summary>
        event Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask> ConnectionStateChanged;

        /// <summary>Gets the number of received messages with <paramref name="name" />.</summary>
        int ReceivedCount(string name);

        /// <summary>Starts a send operation for an encoded payload.</summary>
        IZlinkStreamSendCall Send(ZlinkStreamEncodedPayload payload);

        /// <summary>Starts a request operation for an encoded payload.</summary>
        IZlinkStreamRequestCall Request(ZlinkStreamEncodedPayload payload);

        /// <summary>Registers a callback for messages with the given packet name.</summary>
        IDisposable On(
            string name,
            Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> handler);

        /// <summary>Starts a wait operation for the next unread received message with the given packet name.</summary>
        IZlinkStreamWaitCall WaitFor(string name);

        /// <summary>Verifies that no unread message with the given packet name arrives during a configured window.</summary>
        IZlinkStreamExpectNoneCall ExpectNone(string name);

        /// <summary>Waits for messages with the given packet name and verifies their arrival order.</summary>
        IZlinkStreamSequenceCall WaitForSequence(string name);
    }
}
