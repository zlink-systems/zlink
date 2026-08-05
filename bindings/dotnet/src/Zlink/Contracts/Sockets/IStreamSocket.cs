// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     STREAM callback for framed packet dispatch.
///     Message ownership is transferred to the callback.
///     The callback must dispose each message exactly once.
/// </summary>
public delegate void StreamPacketHandler(RoutingId routingId, Message header,
    Message body);

/// <summary>
///     Contract for a STREAM socket: exchanges framed packets with raw TCP peers.
/// </summary>
public interface IStreamSocket : IRoutedMessageSocket
{
    /// <summary>
    ///     Gets the STREAM-specific typed options facade.
    /// </summary>
    new StreamSocketOptions Options { get; }

    /// <summary>
    ///     Registers the handler invoked for each inbound framed packet. The
    ///     handler runs on a background dispatch thread and owns its messages (see
    ///     <see cref="StreamPacketHandler" />).
    /// </summary>
    void OnPacket(StreamPacketHandler handler);

    /// <summary>
    ///     Receives one raw STREAM part and its source routing id. The returned
    ///     message is owned by the caller and must be disposed. The first
    ///     receive operation fixes this socket to receive mode; it cannot be
    ///     combined with packet callback registration.
    /// </summary>
    bool RecvPart(
        out RoutingId? sourceRoutingId,
        out Message? part,
        out bool hasMore,
        RecvFlags flags = RecvFlags.None);

    /// <summary>
    ///     Disconnects the peer identified by <paramref name="peerRid" />.
    /// </summary>
    void DisconnectRid(RoutingId peerRid);
}
