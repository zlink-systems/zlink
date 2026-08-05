using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Runtime.Locations;

internal sealed class ZLinkSpotLocationLifecycle(
    ZLinkLocationRuntime runtime)
{
    private readonly object _gate = new();
    private readonly Dictionary<string, TrackedSpot> _spots =
        new(StringComparer.Ordinal);

    internal async ValueTask<ZLinkLocationWriteStatus> ClaimAsync(
        string meshName,
        string spotId,
        ulong spotGeneration,
        string? spotType,
        RoutingId nodeRid,
        ulong nodeGeneration,
        ZLinkSpotKind spotKind,
        ulong authorityOwnerGeneration,
        Func<CancellationToken, ValueTask>? deactivate,
        CancellationToken cancellationToken = default)
    {
        runtime.EnsureOwnerAdmissionOpen();
        if (spotKind == ZLinkSpotKind.Entry)
        {
            Track(
                spotId,
                spotGeneration,
                storeVersion: null,
                authorityOwnerGeneration,
                meshName,
                spotType,
                nodeRid,
                nodeGeneration,
                spotKind,
                deactivate);
            return ZLinkLocationWriteStatus.Stored;
        }

        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId);
        var read = await runtime.Store.ReadAuthorityAsync(key, cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found)
            return ZLinkLocationWriteStatus.RejectedConflict;

        var snapshot = found.Snapshot;
        if (!MatchesReadySpot(
                snapshot,
                meshName,
                spotId,
                spotType,
                nodeRid,
                nodeGeneration,
                spotKind)
            || authorityOwnerGeneration != 0
               && snapshot.AuthorityOwnerGeneration != authorityOwnerGeneration
            || snapshot.OwnerId != runtime.OwnerToken.OwnerId
            || snapshot.OwnerLeaseGeneration
               != runtime.OwnerToken.LeaseGeneration)
            return ZLinkLocationWriteStatus.RejectedConflict;

        Track(
            spotId,
            snapshot.ObjectGeneration,
            snapshot.StoreVersion,
            snapshot.AuthorityOwnerGeneration,
            meshName,
            spotType,
            nodeRid,
            nodeGeneration,
            spotKind,
            deactivate);
        return ZLinkLocationWriteStatus.Stored;
    }

    internal bool TryGetTrackedGeneration(string spotId, out ulong generation)
    {
        lock (_gate)
        {
            if (_spots.TryGetValue(spotId, out var tracked))
            {
                generation = tracked.SpotGeneration;
                return true;
            }
        }

        generation = 0;
        return false;
    }

    internal async ValueTask<ZLinkLocationWriteStatus> TrackRelocatedAsync(
        string meshName,
        string spotId,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        string stableType,
        RoutingId nodeRid,
        ulong nodeGeneration,
        ZLinkSpotKind spotKind,
        Func<CancellationToken, ValueTask>? deactivate,
        CancellationToken cancellationToken)
    {
        runtime.EnsureOwnerAdmissionOpen();
        var read = await runtime.Store.ReadAuthorityAsync(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId),
                cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found)
            return ZLinkLocationWriteStatus.RejectedConflict;
        var snapshot = found.Snapshot;
        var expectedKind = spotKind == ZLinkSpotKind.Instance
            ? ZLinkPlacementObjectKind.InstanceSpot
            : ZLinkPlacementObjectKind.UserSpot;
        if (snapshot.ObjectGeneration != objectGeneration
            || snapshot.AuthorityOwnerGeneration
               != authorityOwnerGeneration
            || snapshot.OwnerId != runtime.OwnerToken.OwnerId
            || snapshot.OwnerLeaseGeneration
               != runtime.OwnerToken.LeaseGeneration
            || snapshot.Allocation.State
               != ZLinkPlacementAllocationState.Active
            || snapshot.Allocation.ObjectKind != expectedKind
            || snapshot.Allocation.StableType != stableType
            || snapshot.Allocation.Descriptor.MeshName != meshName
            || snapshot.Allocation.Descriptor.Rid != nodeRid
            || snapshot.Allocation.DescriptorLifecycleGeneration
               != nodeGeneration)
            return ZLinkLocationWriteStatus.RejectedConflict;

        Track(
            spotId,
            objectGeneration,
            snapshot.StoreVersion,
            authorityOwnerGeneration,
            meshName,
            stableType,
            nodeRid,
            nodeGeneration,
            spotKind,
            deactivate);
        return ZLinkLocationWriteStatus.Stored;
    }

    internal void ForgetRelocated(string spotId, ulong objectGeneration)
    {
        lock (_gate)
        {
            if (_spots.TryGetValue(spotId, out var tracked)
                && tracked.SpotGeneration == objectGeneration)
                _spots.Remove(spotId);
        }
    }

    internal async ValueTask ReleaseAsync(
        string meshName,
        string spotId,
        CancellationToken cancellationToken = default)
    {
        _ = meshName;
        TrackedSpot? tracked;
        lock (_gate)
        {
            if (!_spots.TryGetValue(spotId, out tracked))
                return;
        }

        if (tracked.StoreVersion is { } expectedVersion)
        {
            var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId);
            var result = await runtime.Store.CompareExchangeAuthorityAsync(
                    key,
                    expectedVersion,
                    new ZLinkAuthorityMutation.Delete(),
                    cancellationToken)
                .ConfigureAwait(false);
            if (result is ZLinkAuthorityCompareExchangeResult.Conflict(
                    ZLinkAuthorityReadResult.Found current)
                && current.Snapshot.OwnerId == runtime.OwnerToken.OwnerId
                && current.Snapshot.OwnerLeaseGeneration
                   == runtime.OwnerToken.LeaseGeneration
                && current.Snapshot.ObjectGeneration
                   == tracked.SpotGeneration
                && current.Snapshot.AuthorityOwnerGeneration
                   == tracked.AuthorityOwnerGeneration
                && MatchesTrackedSpot(current.Snapshot, spotId, tracked))
            {
                result = await runtime.Store.CompareExchangeAuthorityAsync(
                        key,
                        current.Snapshot.StoreVersion,
                        new ZLinkAuthorityMutation.Delete(),
                        cancellationToken)
                    .ConfigureAwait(false);
            }

            if (result is ZLinkAuthorityCompareExchangeResult.Conflict(
                    ZLinkAuthorityReadResult.Found))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    $"Spot '{spotId}' authority is owned by another runtime.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            if (result is ZLinkAuthorityCompareExchangeResult.GenerationExhausted)
                throw new ZLinkAuthorityGenerationExhaustedException(
                    $"removing Spot '{spotId}' authority");
        }

        lock (_gate)
        {
            if (_spots.TryGetValue(spotId, out var current)
                && ReferenceEquals(current, tracked))
                _spots.Remove(spotId);
        }
    }

    internal Func<CancellationToken, ValueTask>? TakeOwnershipLostDeactivation(
        string canonicalKey)
    {
        var spotId = TryDecodeCanonicalSpotId(canonicalKey);
        if (spotId is null) return null;
        lock (_gate)
        {
            if (!_spots.Remove(spotId, out var spot))
                return null;
            return spot.Deactivate;
        }
    }

    internal void ResetGeneration()
    {
        lock (_gate) _spots.Clear();
    }

    private void Track(
        string spotId,
        ulong spotGeneration,
        string? storeVersion,
        ulong authorityOwnerGeneration,
        string meshName,
        string? spotType,
        RoutingId nodeRid,
        ulong nodeGeneration,
        ZLinkSpotKind spotKind,
        Func<CancellationToken, ValueTask>? deactivate)
    {
        lock (_gate)
        {
            _spots[spotId] = new TrackedSpot(
                spotGeneration,
                storeVersion,
                authorityOwnerGeneration,
                meshName,
                spotType,
                nodeRid,
                nodeGeneration,
                spotKind,
                deactivate);
        }
    }

    private static bool MatchesReadySpot(
        ZLinkAuthoritySnapshot snapshot,
        string meshName,
        string spotId,
        string? spotType,
        RoutingId nodeRid,
        ulong nodeGeneration,
        ZLinkSpotKind spotKind)
    {
        if (spotKind == ZLinkSpotKind.User
            && ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var user))
            return user.State == ZLinkUserSpotAuthorityState.Ready
                   && user.SpotId == spotId
                   && user.MeshName == meshName
                   && user.NodeRid == nodeRid
                   && user.NodeGeneration == nodeGeneration
                   && (spotType is null || user.StableType == spotType);

        return spotKind == ZLinkSpotKind.Instance
               && ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                   snapshot.Payload.Span,
                   out var instance)
               && instance.State == ZLinkInstanceSpotAuthorityState.Ready
               && instance.SpotId == spotId
               && instance.MeshName == meshName
               && instance.NodeRid == nodeRid
               && instance.NodeGeneration == nodeGeneration
               && (spotType is null || instance.StableType == spotType);
    }

    private static string? TryDecodeCanonicalSpotId(string canonicalKey)
    {
        return ZLinkUserSpotAuthorityPayloadCodec.TryGetSpotId(
            new ZLinkAuthorityKey(canonicalKey),
            out var spotId)
            ? spotId
            : null;
    }

    private static bool MatchesTrackedSpot(
        ZLinkAuthoritySnapshot snapshot,
        string spotId,
        TrackedSpot tracked)
    {
        return (ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                    snapshot.Payload.Span,
                    out var user)
                && user.SpotId == spotId
                && tracked.SpotKind == ZLinkSpotKind.User
                && user.MeshName == tracked.MeshName
                && user.StableType == tracked.SpotType
                && user.NodeRid == tracked.NodeRid
                && user.NodeGeneration == tracked.NodeGeneration)
               || (ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                       snapshot.Payload.Span,
                       out var instance)
                   && instance.SpotId == spotId
                   && tracked.SpotKind == ZLinkSpotKind.Instance
                   && instance.MeshName == tracked.MeshName
                   && instance.StableType == tracked.SpotType
                   && instance.NodeRid == tracked.NodeRid
                   && instance.NodeGeneration == tracked.NodeGeneration);
    }

    private sealed record TrackedSpot(
        ulong SpotGeneration,
        string? StoreVersion,
        ulong AuthorityOwnerGeneration,
        string MeshName,
        string? SpotType,
        RoutingId NodeRid,
        ulong NodeGeneration,
        ZLinkSpotKind SpotKind,
        Func<CancellationToken, ValueTask>? Deactivate);
}
