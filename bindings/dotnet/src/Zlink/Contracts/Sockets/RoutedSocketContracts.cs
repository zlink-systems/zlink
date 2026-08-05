// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Receives an opaque bounded control record from a peer's existing
///     completion connection.
/// </summary>
/// <remarks>
///     The callback owns every message in <paramref name="parts" /> and must
///     dispose each message exactly once.
/// </remarks>
public delegate void CompletionControlHandler(
    RoutingId sourceRoutingId,
    IReadOnlyList<Message> parts);

/// <summary>
///     Socket contract for routed message sockets.
/// </summary>
public interface IRoutedMessageSocket : ISocket
{
    /// <summary>
    ///     Start a multipart send operation addressed to <paramref name="routingId" />.
    /// </summary>
    SendOperation Send(RoutingId routingId);

    /// <summary>
    ///     Receive a routed message into caller-provided envelope storage.
    /// </summary>
    /// <param name="result">Reusable storage overwritten on success.</param>
    /// <param name="flags">Receive behavior flags.</param>
    /// <returns>
    ///     true on success; false when <see cref="RecvFlags.DontWait" /> is set
    ///     and no message is available.
    /// </returns>
    bool Recv(Received result, RecvFlags flags = RecvFlags.None);

    /// <summary>
    ///     Register a callback invoked when the socket can accept more sends.
    /// </summary>
    void OnSendReady(Action handler);
}

/// <summary>
///     Routed message socket that also supports connect and bind operations.
/// </summary>
public interface IConnectableRoutedMessageSocket : IRoutedMessageSocket,
    IConnectableSocket
{
}

/// <summary>
///     ROUTER socket contract.
/// </summary>
public interface IRouterSocket : IConnectableRoutedMessageSocket
{
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
    ///     Start a request operation addressed to a peer routing id.
    /// </summary>
    RequestOperation Request(RoutingId peerRid);

    /// <summary>
    ///     Start a reply operation addressed to a peer request.
    /// </summary>
    ReplyOperation Reply(RoutingId rid, ulong requestSeq);

    /// <summary>
    ///     Tries to submit an opaque multipart record on the peer's existing
    ///     completion connection.
    /// </summary>
    /// <remarks>
    ///     The method does not consume <paramref name="parts" />. Core assigns no
    ///     command meaning to the payload. A false result means completion-lane
    ///     back-pressure; other failures throw <see cref="ZlinkSubmitException" />.
    /// </remarks>
    bool TrySendCompletionControl(
        RoutingId peerRid,
        IReadOnlyList<Message> parts);

    /// <summary>
    ///     Installs or replaces the callback for opaque completion-control
    ///     records. The callback is independent from application receive.
    /// </summary>
    void OnCompletionControl(CompletionControlHandler handler);
}
