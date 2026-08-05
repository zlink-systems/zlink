// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Contract for sockets that exchange multipart messages over a direct
///     connection.
/// </summary>
public interface IMessageSocket : IConnectableSocket
{
    /// <summary>
    ///     Begins a multipart send: add parts on the returned builder, then submit.
    ///     The parts are consumed on a successful submit (see
    ///     <see cref="SendOperation" /> for the ownership contract).
    /// </summary>
    SendOperation Send();

    /// <summary>
    ///     Receive a message into caller-provided envelope storage.
    /// </summary>
    /// <param name="result">Reusable storage overwritten on success.</param>
    /// <param name="flags">Receive behavior flags.</param>
    /// <returns>
    ///     true on success; false when <see cref="RecvFlags.DontWait" /> is set
    ///     and no message is available.
    /// </returns>
    bool Recv(Received result, RecvFlags flags = RecvFlags.None);

    /// <summary>
    ///     Registers a callback invoked when the socket can accept more sends after
    ///     back-pressure. The callback runs on a background dispatch thread.
    /// </summary>
    void OnSendReady(Action handler);
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
public interface IDealerSocket : IMessageSocket
{
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
    ///     <see cref="SendOperation" /> for the ownership contract).
    /// </summary>
    RequestOperation Request();
}
