using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Runtime.Host;

/// <summary>
/// Recovery fence validation for standalone Actor relocations. No
/// different-target transition exists in this contract version:
/// TargetAttemptGeneration distinguishes same-target retries only, and a
/// committed relocation whose target died stays parked until the exact target
/// returns. This class only answers whether another live runtime still owns
/// recovery and whether a snapshot names the current target attempt.
/// </summary>
internal sealed class ZLinkStandaloneActorRelocationTakeoverCoordinator(
    ZLinkFrameworkRuntime runtime,
    ZLinkFrameworkRegistration registration)
{
    internal async ValueTask<bool> HasLiveRemoteRecoveryOwnerAsync(
        ZLinkCanonicalRelocationAuthorityProjection projection,
        string meshName,
        CancellationToken cancellationToken)
    {
        var store = registration.Locations.ResolveStore()
                    ?? throw new ZLinkConfigurationException(
                        "Location Store is not registered.");
        var descriptors = await store.ListAllMeshNodesAsync(
                meshName,
                cancellationToken)
            .ConfigureAwait(false);

        if (projection.TargetAttemptGeneration != 0
            && await IsLiveRemoteFenceAsync(
                    projection.State.TargetNodeRid,
                    projection.State.TargetNodeGeneration,
                    projection.TargetOwnerId,
                    projection.TargetOwnerLeaseGeneration,
                    descriptors,
                    store,
                    cancellationToken)
                .ConfigureAwait(false))
            return true;

        // Once owner publication has committed, only the exact target attempt
        // keeps recovery remote. A 'false' here for a committed phase means
        // the caller must do nothing (park until the exact target returns);
        // pre-commit phases fall through to the source coordinator fence,
        // where 'false' permits the precommit abort.
        if (projection.Phase >= 4)
            return false;

        return await IsLiveRemoteFenceAsync(
                projection.State.CoordinatorNodeRid,
                projection.State.CoordinatorNodeGeneration,
                projection.State.CoordinatorOwnerId,
                projection.State.CoordinatorLeaseGeneration,
                descriptors,
                store,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal static bool IsCurrentAttempt(
        ZLinkAuthoritySnapshot snapshot,
        Guid relocationId,
        ulong targetAttemptGeneration,
        ZLinkActorAuthorityPayload targetAuthority)
    {
        if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                snapshot.Payload.Span,
                out var current))
            return false;
        Span<byte> id = stackalloc byte[16];
        relocationId.TryWriteBytes(id, bigEndian: true, out _);
        var high = System.Buffers.Binary.BinaryPrimitives
            .ReadUInt64BigEndian(id);
        var low = System.Buffers.Binary.BinaryPrimitives
            .ReadUInt64BigEndian(id[8..]);
        return current.RelocationHigh == high
               && current.RelocationLow == low
               && current.TargetAttemptGeneration == targetAttemptGeneration
               && StringComparer.Ordinal.Equals(
                   current.State.TargetNodeRid,
                   targetAuthority.NodeRid.ToHex())
               && current.State.TargetNodeGeneration
               == targetAuthority.NodeGeneration
               && StringComparer.Ordinal.Equals(
                   current.TargetOwnerId,
                   targetAuthority.OwnerId)
               && current.TargetOwnerLeaseGeneration
               == targetAuthority.OwnerLeaseGeneration
               && StringComparer.Ordinal.Equals(
                   snapshot.OwnerId,
                   targetAuthority.OwnerId)
               && snapshot.OwnerLeaseGeneration
               == checked((long)targetAuthority.OwnerLeaseGeneration);
    }

    private async ValueTask<bool> IsLiveRemoteFenceAsync(
        string nodeRid,
        ulong nodeGeneration,
        string ownerId,
        ulong ownerLeaseGeneration,
        IReadOnlyList<ZLinkMeshNodeDescriptor> descriptors,
        IZLinkLocationRepository store,
        CancellationToken cancellationToken)
    {
        if (nodeGeneration == 0
            || ownerLeaseGeneration == 0
            || ownerLeaseGeneration > long.MaxValue
            || string.IsNullOrWhiteSpace(nodeRid)
            || string.IsNullOrWhiteSpace(ownerId))
            throw DataLost(
                "Standalone Actor recovery owner fence is malformed.");

        RoutingId rid;
        try
        {
            rid = RoutingId.FromHex(nodeRid);
        }
        catch (Exception exception) when (exception is ArgumentException
                                          or FormatException)
        {
            throw DataLost(
                "Standalone Actor recovery owner RID is malformed.");
        }

        var local = runtime.TryGetSpotNodeRuntime(rid);
        if (local is not null
            && local.Node.MeshStatus().LifecycleGeneration == nodeGeneration)
            return false;

        var lease = await store.ReadOwnerLeaseAsync(ownerId, cancellationToken)
            .ConfigureAwait(false);
        if (lease is not ZLinkOwnerLeaseReadResult.Found found
            || !StringComparer.Ordinal.Equals(found.Token.OwnerId, ownerId)
            || found.Token.LeaseGeneration
               != checked((long)ownerLeaseGeneration)
            || found.LeaseExpiresAt <= found.StoreNow)
            return false;

        return descriptors.Any(descriptor =>
            descriptor.Rid == rid
            && descriptor.LifecycleGeneration == nodeGeneration
            && StringComparer.Ordinal.Equals(descriptor.OwnerId, ownerId)
            && descriptor.LeaseGeneration
               == checked((long)ownerLeaseGeneration));
    }

    private static ZLinkFrameworkException DataLost(string message) =>
        new(
            ZLinkFrameworkErrorKind.DataLost,
            message,
            retryAdvice: ZLinkRetryAdvice.DoNotRetry);
}
