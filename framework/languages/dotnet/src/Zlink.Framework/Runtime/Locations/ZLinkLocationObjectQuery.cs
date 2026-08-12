using System.Text;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Runtime.Locations;

internal sealed class ZLinkLocationObjectQuery(
    IZLinkLocationRepository store,
    ZLinkOwnerLeaseTracker leaseTracker,
    ZLinkLocationStoreHealth? storeHealth)
{
    private const int MaximumStorePageSize = 256;
    private const int MaximumEncodedPageBytes = 4 * 1024 * 1024;

    internal async ValueTask<ZLinkLocationObjectEntry?> FindActorAsync(
        string actorId,
        CancellationToken cancellationToken)
    {
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        var read = await ReadAsync(
                "actor-location-query-read",
                cancellationToken,
                token => store.ReadAuthorityAsync(key, token))
            .ConfigureAwait(false);
        return read is ZLinkAuthorityReadResult.Found found
            ? await ProjectAsync(actorId, found.Snapshot, cancellationToken)
                .ConfigureAwait(false)
            : null;
    }

    internal async ValueTask<ZLinkLocationObjectEntry?> FindSpotAsync(
        string spotId,
        CancellationToken cancellationToken)
    {
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId);
        var read = await ReadAsync(
                "spot-location-query-read",
                cancellationToken,
                token => store.ReadAuthorityAsync(key, token))
            .ConfigureAwait(false);
        return read is ZLinkAuthorityReadResult.Found found
            ? await ProjectAsync(spotId, found.Snapshot, cancellationToken)
                .ConfigureAwait(false)
            : null;
    }

    internal async ValueTask<ZLinkLocationPage<ZLinkLocationObjectEntry>> ListAsync(
        ZLinkLocationObjectFilter filter,
        ZLinkPageRequest page,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(filter);
        if (!Enum.IsDefined(filter.ObjectKind))
            throw new ArgumentOutOfRangeException(nameof(filter));

        var normalized = ZLinkPageRequestPolicy.Normalize(page);
        ZLinkAuthorityScanCursor? cursor = normalized.ContinuationToken is { } token
            ? new ZLinkAuthorityScanCursor(token)
            : null;
        var prefix = filter.ObjectKind == ZLinkLocationObjectKind.Actor
            ? "zla1:a:"
            : "zla1:s:";
        var scan = await ReadAsync(
                "object-location-query-list",
                cancellationToken,
                token => store.ListAuthoritiesAsync(
                    prefix,
                    cursor,
                    Math.Min(normalized.PageSize, MaximumStorePageSize),
                    token))
            .ConfigureAwait(false);
        if (scan is not ZLinkAuthorityScanResult.Page result)
            throw Unavailable("The object location continuation token expired.");

        var items = new List<ZLinkLocationObjectEntry>(result.Value.Items.Count);
        foreach (var authority in result.Value.Items)
        {
            if (!MatchesKind(authority.Snapshot.Allocation.ObjectKind, filter.ObjectKind))
                continue;
            var globalId = GetGlobalId(authority, filter.ObjectKind);
            var entry = await ProjectAsync(
                    globalId,
                    authority.Snapshot,
                    cancellationToken)
                .ConfigureAwait(false);
            if (entry is not null && Matches(entry, filter)) items.Add(entry);
        }

        var pageResult = new ZLinkLocationPage<ZLinkLocationObjectEntry>(
            items,
            result.Value.NextCursor?.Encoded);
        if (EncodedSizeUpperBound(pageResult) > MaximumEncodedPageBytes)
            throw Unavailable("The encoded object location page exceeds 4 MiB.");
        return pageResult;
    }

    private async ValueTask<ZLinkLocationObjectEntry> ProjectAsync(
        string globalId,
        ZLinkAuthoritySnapshot snapshot,
        CancellationToken cancellationToken)
    {
        var state = ZLinkLocationObjectState.Creating;
        if (snapshot.Allocation.State == ZLinkPlacementAllocationState.Active)
        {
            var remaining = await ReadAsync(
                    "object-location-owner-lease-read",
                    cancellationToken,
                    token => leaseTracker.GetOwnerTokenRemainingAdmissionLifetimeAsync(
                    new ZLinkLocationOwnerToken(
                        snapshot.OwnerId,
                        snapshot.OwnerLeaseGeneration),
                    token))
                .ConfigureAwait(false);
            state = remaining is null
                ? ZLinkLocationObjectState.Unavailable
                : ZLinkLocationObjectState.Ready;
        }

        return new ZLinkLocationObjectEntry(
            globalId,
            snapshot.ObjectGeneration,
            snapshot.Allocation.Descriptor.MeshName,
            snapshot.Allocation.Descriptor.Rid,
            state,
            snapshot.Allocation.StableType);
    }

    private async ValueTask<T> ReadAsync<T>(
        string source,
        CancellationToken cancellationToken,
        Func<CancellationToken, ValueTask<T>> read)
    {
        try
        {
            return await ZLinkLocationStoreRead.ExecuteAsync(
                    storeHealth,
                    source,
                    cancellationToken,
                    read)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception error)
        {
            throw Unavailable("The object location store is unavailable.", error);
        }
    }

    private static string GetGlobalId(
        ZLinkAuthorityEntry authority,
        ZLinkLocationObjectKind kind)
    {
        try
        {
            if (kind == ZLinkLocationObjectKind.Actor)
                return ZLinkAuthorityKeyCodec.DecodeActor(authority.Key);

            var spotId = ZLinkAuthorityKeyCodec.DecodeSpot(authority.Key);
            return ZLinkSpotId.IsValid(spotId)
                ? spotId
                : throw new InvalidDataException(
                    "The authority key contains an invalid spot identifier.");
        }
        catch (InvalidDataException error)
        {
            throw Unavailable(
                "The object location store contains a noncanonical authority key.",
                error);
        }
    }

    private static bool MatchesKind(
        ZLinkPlacementObjectKind stored,
        ZLinkLocationObjectKind requested) => requested switch
    {
        ZLinkLocationObjectKind.Actor => stored == ZLinkPlacementObjectKind.Actor,
        ZLinkLocationObjectKind.UserSpot => stored == ZLinkPlacementObjectKind.UserSpot,
        ZLinkLocationObjectKind.InstanceSpot => stored == ZLinkPlacementObjectKind.InstanceSpot,
        _ => false
    };

    private static bool Matches(
        ZLinkLocationObjectEntry entry,
        ZLinkLocationObjectFilter filter) =>
        (filter.StableType is null
         || string.Equals(entry.StableType, filter.StableType, StringComparison.Ordinal))
        && (filter.MeshName is null
            || string.Equals(entry.MeshName, filter.MeshName, StringComparison.Ordinal));

    private static long EncodedSizeUpperBound(
        ZLinkLocationPage<ZLinkLocationObjectEntry> page)
    {
        const int fixedPageBytes = 256;
        const int fixedEntryBytes = 256;
        var size = fixedPageBytes;
        if (page.ContinuationToken is { } continuationToken)
            size += Encoding.UTF8.GetByteCount(continuationToken) * 6;
        foreach (var entry in page.Items)
        {
            size += fixedEntryBytes;
            size += Encoding.UTF8.GetByteCount(entry.GlobalId) * 6;
            size += Encoding.UTF8.GetByteCount(entry.MeshName) * 6;
            size += Encoding.UTF8.GetByteCount(entry.StableType) * 6;
            size += entry.NodeRid.ToString().Length * 6;
        }
        return size;
    }

    private static ZLinkFrameworkException Unavailable(
        string message,
        Exception? error = null) => new(
        ZLinkFrameworkErrorKind.Unavailable,
        message,
        ZLinkRetryAdvice.RetryAfterBackoff,
        error);
}
