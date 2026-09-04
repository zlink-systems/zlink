// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Contract for sockets that receive multipart messages and report outbound
///     send readiness.
/// </summary>
public interface IReceivingMessageSocket : ISocket
{
    /// <summary>
    ///     Receive a message into caller-provided envelope storage.
    /// </summary>
    /// <param name="result">Reusable storage overwritten on success.</param>
    /// <param name="flags">Receive behavior flags.</param>
    /// <returns>
    ///     true on success; false when <see cref="RecvFlags.DontWait" /> is set
    ///     and no message is available.
    /// </returns>
    /// <remarks>
    ///     Receive is a single-consumer operation for one socket. The caller must
    ///     not use the same <paramref name="result" /> concurrently or start a
    ///     second receive on this socket until the first receive has returned.
    /// </remarks>
    bool Recv(Received result, RecvFlags flags = RecvFlags.None);

}

/// <summary>
///     Contract for sockets that exchange multipart messages over a direct
///     connection using the synchronous send terminal.
/// </summary>
public interface IMessageSocket : IReceivingMessageSocket, IConnectableSocket
{
    /// <summary>
    ///     Begins a multipart send: add parts on the returned builder, then submit.
    ///     The parts are consumed on a successful submit (see
    ///     <see cref="SendOperation" /> for the ownership contract). Independent
    ///     send builders may be submitted concurrently on the same socket. Do not
    ///     share one builder or one message between concurrent operations.
    /// </summary>
    SendOperation Send();
}

/// <summary>
///     Contract for a PAIR socket: an exclusive one-to-one peering with no routing.
/// </summary>
public interface IPairSocket : IMessageSocket
{
}

/// <summary>
///     Contract for a DEALER socket: load-balances sends across its connected
///     peers and can issue routed requests.
/// </summary>
public interface IDealerSocket : IReceivingMessageSocket, IConnectableSocket
{
    /// <summary>
    ///     Begins a DEALER send. Its <c>Async</c> terminal completes immediately
    ///     on admission; after backpressure it waits for the exact WRITABLE token
    ///     and retries the retained packet without occupying the caller thread.
    /// </summary>
    SendOperation Send();

    /// <summary>
    ///     Gets DEALER-specific socket options.
    /// </summary>
    new DealerSocketOptions Options { get; }

    /// <summary>
    ///     Sets the routing id that identifies this DEALER to its peers. Apply
    ///     before connecting so peers observe it from the first message.
    /// </summary>
    void SetRoutingId(RoutingId routingId);

    /// <summary>
    ///     Gets the routing id that identifies this DEALER to its peers.
    /// </summary>
    RoutingId GetRoutingId();

    /// <summary>
    ///     Begins a request: add parts on the returned builder, then submit and
    ///     await a reply. An awaitable request refused before admission retains
    ///     its packet in the binding and retries only after its WRITABLE token.
    ///     Independent request builders may be submitted concurrently on the
    ///     same socket. Do not share one builder or one message between
    ///     concurrent operations.
    /// </summary>
    RequestOperation Request();
}
