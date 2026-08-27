using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Locations;

internal sealed class ZLinkSpotLocationLifecycle(
    ZLinkLocationRuntime runtime)
{
    private readonly ZLinkStateLane _lane = new();
    private readonly Dictionary<ZLinkSpotId, TrackedSpot> _spots = [];

    internal async ValueTask<ZLinkLocationWriteStatus> ClaimAsync(
        ZLinkMeshName meshName,
        ZLinkSpotId spotId,
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
        var lifecycleKind = ZLinkSpotLifecycleKind.FromBoundary(spotKind);
        if (lifecycleKind is ZLinkSpotLifecycleKind.Entry)
        {
            await TrackAsync(
                spotId,
                spotGeneration,
                storeVersion: null,
                authorityOwnerGeneration,
                meshName,
                spotType,
                nodeRid,
                nodeGeneration,
                lifecycleKind,
                deactivate);
            return ZLinkLocationWriteStatus.Stored;
        }

        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId.Value);
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
                lifecycleKind)
            || authorityOwnerGeneration != 0
               && snapshot.AuthorityOwnerGeneration != authorityOwnerGeneration
            || snapshot.OwnerId != runtime.OwnerToken.OwnerId
            || snapshot.OwnerLeaseGeneration
               != runtime.OwnerToken.LeaseGeneration)
            return ZLinkLocationWriteStatus.RejectedConflict;

        await TrackAsync(
            spotId,
            snapshot.ObjectGeneration,
            snapshot.StoreVersion,
            snapshot.AuthorityOwnerGeneration,
            meshName,
            spotType,
            nodeRid,
            nodeGeneration,
            lifecycleKind,
            deactivate);
        return ZLinkLocationWriteStatus.Stored;
    }

    internal ValueTask<ulong?> GetTrackedGenerationAsync(ZLinkSpotId spotId) =>
        _lane.RunAsync(() => _spots.TryGetValue(spotId, out var tracked)
            ? tracked.SpotGeneration
            : (ulong?)null);

    internal bool TryGetTrackedGeneration(ZLinkSpotId spotId, out ulong generation)
    {
        var trackedGeneration = AwaitStateLane(GetTrackedGenerationAsync(spotId));
        generation = trackedGeneration.GetValueOrDefault();
        return trackedGeneration.HasValue;
    }

    internal async ValueTask<ZLinkLocationWriteStatus> TrackRelocatedAsync(
        ZLinkMeshName meshName,
        ZLinkSpotId spotId,
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
        var lifecycleKind =
            ZLinkSpotLifecycleKind.RelocatableFromBoundary(spotKind);
        var read = await runtime.Store.ReadAuthorityAsync(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId.Value),
                cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found)
            return ZLinkLocationWriteStatus.RejectedConflict;
        var snapshot = found.Snapshot;
        var expectedKind = lifecycleKind.PlacementKind!.Value;
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
            || snapshot.Allocation.Descriptor.MeshName != meshName.Value
            || snapshot.Allocation.Descriptor.Rid != nodeRid
            || snapshot.Allocation.DescriptorLifecycleGeneration
               != nodeGeneration)
            return ZLinkLocationWriteStatus.RejectedConflict;

        await TrackAsync(
            spotId,
            objectGeneration,
            snapshot.StoreVersion,
            authorityOwnerGeneration,
            meshName,
            stableType,
            nodeRid,
            nodeGeneration,
            lifecycleKind,
            deactivate);
        return ZLinkLocationWriteStatus.Stored;
    }

    internal ValueTask ForgetRelocatedAsync(
        ZLinkSpotId spotId,
        ulong objectGeneration) =>
        _lane.RunAsync(() =>
        {
            if (_spots.TryGetValue(spotId, out var tracked)
                && tracked.SpotGeneration == objectGeneration)
                _spots.Remove(spotId);
        });

    internal async ValueTask ReleaseAsync(
        ZLinkMeshName meshName,
        ZLinkSpotId spotId,
        CancellationToken cancellationToken = default)
    {
        var tracked = await _lane.RunAsync(() =>
            _spots.TryGetValue(spotId, out var current) ? current : null)
            .ConfigureAwait(false);
        if (tracked is null)
            return;

        if (tracked.StoreVersion is { } expectedVersion)
        {
            var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId.Value);
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
                    $"Spot '{spotId.Value}' authority is owned by another runtime.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            if (result is ZLinkAuthorityCompareExchangeResult.GenerationExhausted)
                throw new ZLinkAuthorityGenerationExhaustedException(
                    $"removing Spot '{spotId.Value}' authority");
        }

        await _lane.RunAsync(() =>
        {
            if (_spots.TryGetValue(spotId, out var current)
                && ReferenceEquals(current, tracked))
                _spots.Remove(spotId);
        }).ConfigureAwait(false);
    }

    internal ValueTask<Func<CancellationToken, ValueTask>?> TakeOwnershipLostDeactivationAsync(
        string canonicalKey)
    {
        var spotId = TryDecodeCanonicalSpotId(canonicalKey);
        if (spotId is null) return ValueTask.FromResult<Func<CancellationToken, ValueTask>?>(null);
        var spotKey = spotId.Value;
        return _lane.RunAsync(() =>
        {
            if (!_spots.Remove(spotKey, out var spot))
                return null;
            return spot.Deactivate;
        });
    }

    internal ValueTask ResetGenerationAsync() => _lane.RunAsync(_spots.Clear);

    private ValueTask TrackAsync(
        ZLinkSpotId spotId,
        ulong spotGeneration,
        string? storeVersion,
        ulong authorityOwnerGeneration,
        ZLinkMeshName meshName,
        string? spotType,
        RoutingId nodeRid,
        ulong nodeGeneration,
        ZLinkSpotLifecycleKind spotKind,
        Func<CancellationToken, ValueTask>? deactivate)
    {
        return _lane.RunAsync(() =>
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
        });
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static bool MatchesReadySpot(
        ZLinkAuthoritySnapshot snapshot,
        ZLinkMeshName meshName,
        ZLinkSpotId spotId,
        string? spotType,
        RoutingId nodeRid,
        ulong nodeGeneration,
        ZLinkSpotLifecycleKind spotKind)
    {
        return spotKind.MatchesReady(
            snapshot,
            meshName,
            spotId,
            spotType,
            nodeRid,
            nodeGeneration);
    }

    private static ZLinkSpotId? TryDecodeCanonicalSpotId(string canonicalKey)
    {
        return ZLinkUserSpotAuthorityPayloadCodec.TryGetSpotId(
            new ZLinkAuthorityKey(canonicalKey),
            out var spotId)
            ? ZLinkSpotId.FromBoundary(spotId, nameof(spotId))
            : null;
    }

    private static bool MatchesTrackedSpot(
        ZLinkAuthoritySnapshot snapshot,
        ZLinkSpotId spotId,
        TrackedSpot tracked)
    {
        return tracked.SpotKind.MatchesTracked(
            snapshot,
            spotId,
            tracked.MeshName,
            tracked.SpotType,
            tracked.NodeRid,
            tracked.NodeGeneration);
    }

    private sealed record TrackedSpot(
        ulong SpotGeneration,
        string? StoreVersion,
        ulong AuthorityOwnerGeneration,
        ZLinkMeshName MeshName,
        string? SpotType,
        RoutingId NodeRid,
        ulong NodeGeneration,
        ZLinkSpotLifecycleKind SpotKind,
        Func<CancellationToken, ValueTask>? Deactivate);
}
