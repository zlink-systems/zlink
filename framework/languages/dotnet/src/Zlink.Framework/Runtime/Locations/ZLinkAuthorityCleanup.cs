namespace Zlink.Framework.Runtime.Locations;

/// <summary>Location runtime §5.1: one eligibility decision for discarded authority.</summary>
internal static class ZLinkAuthorityCleanup
{
    internal static async ValueTask<bool> CleanupDiscardedAsync(
        IZLinkLocationRepository repository,
        Func<ZLinkAuthoritySnapshot, CancellationToken, ValueTask<bool>> release,
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot discarded,
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken
    )
    {
        var read = await repository
            .ReadAuthorityAsync(key, cancellationToken)
            .ConfigureAwait(false);
        if (
            read is not ZLinkAuthorityReadResult.Found found
            || !SameIncarnation(found.Snapshot, discarded)
        )
            return true;
        var current = found.Snapshot;
        if (current.Allocation.State != ZLinkPlacementAllocationState.Active)
            return false;
        var spotId = RequireSteadyReady(current);
        if (owner.OwnerId != discarded.OwnerId)
            return false;
        if (spotId is not null)
        {
            ZLinkAuthorityScanCursor? cursor = null;
            do
            {
                var scan = await repository
                    .ListAuthoritiesAsync(
                        ZLinkAuthorityKeyCodec.Prefix(ZLinkAuthorityKeyKind.Actor),
                        cursor,
                        ZLinkPageRequestPolicy.MaximumPageSize,
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                if (scan is ZLinkAuthorityScanResult.ScanExpired)
                {
                    cursor = null;
                    continue;
                }
                var page = ((ZLinkAuthorityScanResult.Page)scan).Value;
                foreach (var item in page.Items)
                    if (
                        ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                            item.Snapshot.Payload.Span,
                            out var member
                        )
                        && member.CurrentSpotKind == ZLinkSpotKind.User
                        && member.CurrentSpotId == spotId
                        && member.CurrentSpotGeneration == current.ObjectGeneration
                    )
                        return false;
                cursor = page.NextCursor;
                if (cursor is null)
                    break;
            } while (true);
        }
        var lease = await repository
            .ReadOwnerLeaseAsync(current.OwnerId, cancellationToken)
            .ConfigureAwait(false);
        if (
            lease is ZLinkOwnerLeaseReadResult.Found live
            && live.Token.LeaseGeneration == current.OwnerLeaseGeneration
            && live.LeaseExpiresAt > live.StoreNow
        )
        {
            if (live.Token != owner)
                return false;
            return await repository
                    .CompareExchangeAuthorityAsync(
                        key,
                        current.StoreVersion,
                        new ZLinkAuthorityMutation.Delete(),
                        cancellationToken
                    )
                    .ConfigureAwait(false) is ZLinkAuthorityCompareExchangeResult.Deleted;
        }
        return await release(current, cancellationToken).ConfigureAwait(false);
    }

    internal static string? RequireSteadyReady(ZLinkAuthoritySnapshot snapshot)
    {
        if (snapshot.Allocation.ObjectKind == ZLinkPlacementObjectKind.UserSpot)
        {
            if (
                !ZLinkUserSpotAuthorityPayloadCodec.TryDecode(snapshot.Payload.Span, out var spot)
                || spot.State != ZLinkUserSpotAuthorityState.Ready
            )
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "User Spot is not steady Ready."
                );
            return spot.SpotId;
        }
        if (
            snapshot.Allocation.ObjectKind == ZLinkPlacementObjectKind.InstanceSpot
            && (
                !ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                    snapshot.Payload.Span,
                    out var instance
                )
                || instance.State != ZLinkInstanceSpotAuthorityState.Ready
                || instance.ActivationRecovery is not null
            )
        )
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "Instance Spot is not steady Ready."
            );
        if (
            snapshot.Allocation.ObjectKind == ZLinkPlacementObjectKind.Actor
            && !ZLinkActorAuthorityPayloadCodec.IsSteadyReady(snapshot.Payload.Span)
        )
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "Actor is not steady Ready.",
                ZLinkRetryAdvice.RetryAfterStateChange
            );
        return null;
    }

    private static bool SameIncarnation(
        ZLinkAuthoritySnapshot current,
        ZLinkAuthoritySnapshot discarded
    ) =>
        current.OwnerId == discarded.OwnerId
        && current.ObjectGeneration == discarded.ObjectGeneration
        && current.AuthorityOwnerGeneration == discarded.AuthorityOwnerGeneration
        && current.Allocation == discarded.Allocation;
}
