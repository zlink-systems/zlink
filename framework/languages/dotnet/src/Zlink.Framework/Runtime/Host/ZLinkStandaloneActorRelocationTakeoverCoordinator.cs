using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Runtime.Host;

/// <summary>Checks whether an authority still names the target attempt.</summary>
internal static class ZLinkStandaloneActorRelocationTakeoverCoordinator
{
    internal static bool IsCurrentAttempt(
        ZLinkAuthoritySnapshot snapshot,
        Guid relocationId,
        ulong targetAttemptGeneration,
        ZLinkActorAuthorityPayload targetAuthority
    )
    {
        if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(snapshot, out var current))
            return false;
        Span<byte> id = stackalloc byte[16];
        relocationId.TryWriteBytes(id, bigEndian: true, out _);
        var high = System.Buffers.Binary.BinaryPrimitives.ReadUInt64BigEndian(id);
        var low = System.Buffers.Binary.BinaryPrimitives.ReadUInt64BigEndian(id[8..]);
        return current.RelocationHigh == high
            && current.RelocationLow == low
            && current.TargetAttemptGeneration == targetAttemptGeneration
            && StringComparer.Ordinal.Equals(
                current.State.TargetNodeRid,
                targetAuthority.NodeRid.ToHex()
            )
            && current.State.TargetNodeGeneration == targetAuthority.NodeGeneration
            && StringComparer.Ordinal.Equals(current.TargetOwnerId, targetAuthority.OwnerId)
            && current.TargetOwnerLeaseGeneration == targetAuthority.OwnerLeaseGeneration
            && StringComparer.Ordinal.Equals(snapshot.OwnerId, targetAuthority.OwnerId)
            && snapshot.OwnerLeaseGeneration == checked((long)targetAuthority.OwnerLeaseGeneration);
    }
}
