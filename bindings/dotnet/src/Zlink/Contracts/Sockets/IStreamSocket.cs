// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Contract for a STREAM socket: exchanges framed packets with raw TCP peers.
/// </summary>
public interface IStreamSocket : IReceivingMessageSocket
{
    /// <summary>
    ///     Begins an exact-target STREAM send whose asynchronous terminal waits
    ///     for Core admission to the selected logical connection.
    /// </summary>
    SendOperation Send(RoutingId routingId);

    /// <summary>
    ///     Gets the STREAM-specific typed options facade.
    /// </summary>
    new StreamSocketOptions Options { get; }

    /// <summary>Receives one decoded packet into reusable output storage.</summary>
    bool RecvPacket(StreamPacket result,
        RecvFlags flags = RecvFlags.None);

    /// <summary>
    ///     Disconnects the peer identified by <paramref name="peerRid" />.
    /// </summary>
    void DisconnectRid(RoutingId peerRid);
}
