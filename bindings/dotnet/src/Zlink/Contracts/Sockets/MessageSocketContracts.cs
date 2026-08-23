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
    ///     Queue-admission credit returns at dequeue; use
    ///     <see cref="RecvRetained(Received, RecvFlags)" /> only when the result
    ///     lifetime must retain that credit.
    /// </remarks>
    bool Recv(Received result, RecvFlags flags = RecvFlags.None);

    /// <summary>
    ///     Receive a message while retaining its queue-admission credit with the
    ///     supplied envelope.
    /// </summary>
    /// <param name="result">Reusable storage that owns the retained credit.</param>
    /// <param name="flags">Receive behavior flags.</param>
    /// <returns>
    ///     true on success; false when <see cref="RecvFlags.DontWait" /> is set
    ///     and no message is available.
    /// </returns>
    /// <remarks>
    ///     Starting this receive releases any message and retained credit already
    ///     held by <paramref name="result" />. A successful receive holds its
    ///     credit until the result is disposed or reused. An API that transfers
    ///     message ownership out of the result releases the credit before
    ///     returning the messages. Ordinary
    ///     <see cref="Recv(Received, RecvFlags)" /> returns queue credit at
    ///     dequeue.
    /// </remarks>
    bool RecvRetained(Received result, RecvFlags flags = RecvFlags.None);
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
    ///     Begins a DEALER send whose <c>Async</c> terminal waits for exact-target
    ///     Core admission without occupying the caller thread.
    /// </summary>
    RoutedSendOperation Send();

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
    ///     await a reply. Request parts are consumed on a successful submit (see
    ///     <see cref="SendOperation" /> for the ownership contract). Independent
    ///     request builders may be submitted concurrently on the same socket. Do
    ///     not share one builder or one message between concurrent operations.
    /// </summary>
    RequestOperation Request();
}
