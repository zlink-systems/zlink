// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Socket contract for routed message sockets.
/// </summary>
public interface IRoutedMessageSocket : IReceivingMessageSocket
{
    /// <summary>
    ///     Start a multipart send operation addressed to <paramref name="routingId" />.
    ///     Independent send builders may be submitted concurrently on the same
    ///     socket. Do not share one builder or one message between concurrent
    ///     operations.
    /// </summary>
    SendOperation Send(RoutingId routingId);
}

/// <summary>
///     Receive/connect role for routed sockets. The concrete routed role owns its
///     send terminal.
/// </summary>
public interface IConnectableRoutedMessageSocket : IReceivingMessageSocket,
    IConnectableSocket
{
}

/// <summary>
///     ROUTER socket contract.
/// </summary>
public interface IRouterSocket : IConnectableRoutedMessageSocket
{
    /// <summary>
    ///     Begins an exact-target ROUTER send. Its <c>Async</c> terminal completes
    ///     immediately on admission; after backpressure it waits for the matching
    ///     WRITABLE token and retries the retained packet.
    /// </summary>
    SendOperation Send(RoutingId routingId);

    /// <summary>
    ///     Gets ROUTER-specific socket options.
    /// </summary>
    new RouterSocketOptions Options { get; }

    /// <summary>
    ///     Set the routing id used by this ROUTER socket.
    /// </summary>
    void SetRoutingId(RoutingId routingId);

    /// <summary>
    ///     Get the routing id used by this ROUTER socket.
    /// </summary>
    RoutingId GetRoutingId();

    /// <summary>
    ///     Start a request operation addressed to a peer routing id. Independent
    ///     request builders may be submitted concurrently on the same socket. Do
    ///     not share one builder or one message between concurrent operations.
    /// </summary>
    RequestOperation Request(RoutingId peerRid);

    /// <summary>
    ///     Start a reply operation addressed to a peer request. Independent reply
    ///     builders may be submitted concurrently on the same socket. Do not share
    ///     one builder or one message between concurrent operations.
    /// </summary>
    ReplyOperation Reply(RoutingId rid, ReplyToken replyToken);

}
