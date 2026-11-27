using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;
using Zlink.NetworkSync.Simulation;

namespace Zlink.NetworkSync.Server;

/// <summary>Relays the network sync one-way packets to an application-selected bound Actor.</summary>
public static class NetworkSyncSession
{
    internal const string InputPacket = "NetworkSyncInputV1";
    internal const string PingPacket = nameof(NetworkSyncPingV1);
    private static readonly int MaxInputBytes = new SimulationSettings().MaxInputBytes;

    /// <summary>
    /// Returns true for packets owned by network sync, including packets rejected at the ingress.
    /// The caller selects the authenticated, current binding from its Session context.
    /// </summary>
    public static async ValueTask<bool> TryRelayAsync(
        ZLinkSessionDispatchContext dispatch, ZLinkMessage payload, IZLinkSessionActor? actor)
    {
        if (dispatch.PacketName is not (InputPacket or PingPacket))
            return false;

        if (actor is null || dispatch.CanReply)
            return true;

        // The typed Spot handler does not receive the original byte length.
        if (dispatch.PacketName == InputPacket)
        {
            var length = payload.Decode<ReadOnlyMemory<byte>>().Length;
            if (length == 0 || length > MaxInputBytes)
                return true;
        }

        await actor.RelayAsync(payload).ConfigureAwait(false);
        return true;
    }
}
