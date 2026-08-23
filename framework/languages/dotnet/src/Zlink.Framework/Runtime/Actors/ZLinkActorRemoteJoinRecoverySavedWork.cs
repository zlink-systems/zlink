using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Runtime.Actors;

/// <summary>Canonical saved-work representation for a pending routed Actor join.</summary>
internal static class ZLinkActorRemoteJoinRecoverySavedWork
{
    internal const string PacketName = "__zlink.actor.routed_join.recovery";

    internal static ZLinkRelocationQueuedJob Create(
        ulong order,
        ZLinkActorRelocationSourceFence source,
        ReadOnlyMemory<byte> encoded)
    {
        _ = ZLinkActorRemoteJoinRecoveryCodec.Decode(
            encoded.Span, out var generatedSource);
        if (!StringComparer.Ordinal.Equals(source.OwnerId, generatedSource.OwnerId)
            || source.OwnerLeaseGeneration
               != generatedSource.OwnerLeaseGeneration
            || source.NodeRid != generatedSource.NodeRid
            || source.NodeGeneration != generatedSource.NodeGeneration)
            throw new InvalidDataException(
                "Actor Join recovery source fence does not match its frozen record.");
        return new ZLinkRelocationQueuedJob(order, encoded);
    }

    internal static bool TryDecode(
        ReadOnlySpan<byte> encoded,
        out ZLinkActorRelocationSourceFence source,
        out ZLinkActorRelocationRecoveryRecord recovery)
    {
        source = default!;
        recovery = default!;
        try
        {
            recovery = ZLinkActorRemoteJoinRecoveryCodec.Decode(
                encoded, out source);
            return true;
        }
        catch (Exception error) when (error is InvalidDataException
                                      or EndOfStreamException
                                      or ArgumentException
                                      or OverflowException)
        {
            return false;
        }
    }
}
