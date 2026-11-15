using System.Text;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Runtime.Locations;

internal sealed class ZLinkLocationObjectQuery(
    IZLinkLocationRepository store,
    ZLinkOwnerLeaseTracker leaseTracker,
    ZLinkLocationStoreHealth? storeHealth
)
{
    private const int MaximumStorePageSize = 256;
    private const int MaximumEncodedPageBytes = 4 * 1024 * 1024;

    internal async ValueTask<ZLinkLocationObjectEntry?> FindActorAsync(
        string actorId,
        CancellationToken cancellationToken
    )
    {
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        var read = await ReadAsync(
                "actor-location-query-read",
                cancellationToken,
                token => store.ReadAuthorityAsync(key, token)
            )
            .ConfigureAwait(false);
        return read is ZLinkAuthorityReadResult.Found found
            ? await ProjectAsync(actorId, found.Snapshot, cancellationToken).ConfigureAwait(false)
            : null;
    }

    internal async ValueTask<ZLinkLocationObjectEntry?> FindSpotAsync(
        string spotId,
        CancellationToken cancellationToken
    )
    {
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId);
        var read = await ReadAsync(
                "spot-location-query-read",
                cancellationToken,
                token => store.ReadAuthorityAsync(key, token)
            )
            .ConfigureAwait(false);
        return read is ZLinkAuthorityReadResult.Found found
            ? await ProjectAsync(spotId, found.Snapshot, cancellationToken).ConfigureAwait(false)
            : null;
    }

    internal async ValueTask<ZLinkLocationPage<ZLinkLocationObjectEntry>> ListAsync(
        ZLinkLocationObjectFilter filter,
        ZLinkPageRequest page,
        CancellationToken cancellationToken
    )
    {
        ArgumentNullException.ThrowIfNull(filter);
        if (!Enum.IsDefined(filter.ObjectKind))
            throw new ArgumentOutOfRangeException(nameof(filter));

        var normalized = ZLinkPageRequestPolicy.Normalize(page);
        ZLinkAuthorityScanCursor? cursor = normalized.ContinuationToken is { } token
            ? new ZLinkAuthorityScanCursor(token)
            : null;
        var prefix = ZLinkAuthorityKeyCodec.Prefix(
            filter.ObjectKind == ZLinkLocationObjectKind.Actor
                ? ZLinkAuthorityKeyKind.Actor
                : ZLinkAuthorityKeyKind.Spot
        );
        var items = new List<ZLinkLocationObjectEntry>(normalized.PageSize);
        long encodedItemsBytes = 0;
        while (items.Count < normalized.PageSize)
        {
            var batchCursor = cursor;
            var scan = await ReadAsync(
                    "object-location-query-list",
                    cancellationToken,
                    token =>
                        store.ListAuthoritiesAsync(
                            prefix,
                            batchCursor,
                            Math.Min(normalized.PageSize - items.Count, MaximumStorePageSize),
                            token
                        )
                )
                .ConfigureAwait(false);
            if (scan is not ZLinkAuthorityScanResult.Page result)
                throw Unavailable("The object location continuation token expired.");

            var index = 0;
            for (; index < result.Value.Items.Count && items.Count < normalized.PageSize; index++)
            {
                var authority = result.Value.Items[index];
                if (!MatchesKind(authority.Snapshot.Allocation.ObjectKind, filter.ObjectKind))
                    continue;
                var globalId = GetGlobalId(authority, filter.ObjectKind);
                var entry = await ProjectAsync(globalId, authority.Snapshot, cancellationToken)
                    .ConfigureAwait(false);
                if (!Matches(entry, filter))
                    continue;
                var entryBytes = EncodedObjectSize(entry);
                var candidateItemsBytes = encodedItemsBytes + entryBytes;
                var continuation = result.Value.NextCursor?.Encoded;
                if (
                    EncodedPageSize(candidateItemsBytes, items.Count + 1, continuation)
                    > MaximumEncodedPageBytes
                )
                {
                    if (items.Count == 0)
                        throw Unavailable(
                            $"The encoded object location entry exceeds {MaximumEncodedPageBytes} bytes."
                        );
                    break;
                }
                items.Add(entry);
                encodedItemsBytes = candidateItemsBytes;
            }

            cursor = result.Value.NextCursor;
            if (index < result.Value.Items.Count)
            {
                if (index == 0)
                    cursor = batchCursor;
                else
                {
                    var consumed = await ReadAsync(
                            "object-location-query-list",
                            cancellationToken,
                            token => store.ListAuthoritiesAsync(prefix, batchCursor, index, token)
                        )
                        .ConfigureAwait(false);
                    if (consumed is not ZLinkAuthorityScanResult.Page consumedPage)
                        throw Unavailable("The object location continuation token expired.");
                    cursor = consumedPage.Value.NextCursor;
                }
                break;
            }
            if (cursor is null)
                break;
        }
        return new ZLinkLocationPage<ZLinkLocationObjectEntry>(items, cursor?.Encoded);
    }

    private async ValueTask<ZLinkLocationObjectEntry> ProjectAsync(
        string globalId,
        ZLinkAuthoritySnapshot snapshot,
        CancellationToken cancellationToken
    )
    {
        var state = ZLinkLocationObjectState.Creating;
        if (snapshot.Allocation.State == ZLinkPlacementAllocationState.Active)
        {
            var remaining = await ReadAsync(
                    "object-location-owner-lease-read",
                    cancellationToken,
                    token =>
                        leaseTracker.GetOwnerTokenRemainingAdmissionLifetimeAsync(
                            new ZLinkLocationOwnerToken(
                                snapshot.OwnerId,
                                snapshot.OwnerLeaseGeneration
                            ),
                            token
                        )
                )
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
            snapshot.Allocation.StableType
        );
    }

    private async ValueTask<T> ReadAsync<T>(
        string source,
        CancellationToken cancellationToken,
        Func<CancellationToken, ValueTask<T>> read
    )
    {
        try
        {
            return await ZLinkLocationStoreRead
                .ExecuteAsync(storeHealth, source, cancellationToken, read)
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

    private static string GetGlobalId(ZLinkAuthorityEntry authority, ZLinkLocationObjectKind kind)
    {
        try
        {
            if (kind == ZLinkLocationObjectKind.Actor)
                return ZLinkAuthorityKeyCodec.DecodeActor(authority.Key);

            var spotId = ZLinkAuthorityKeyCodec.DecodeSpot(authority.Key);
            return ZLinkSpotId.IsValid(spotId)
                ? spotId
                : throw new InvalidDataException(
                    "The authority key contains an invalid spot identifier."
                );
        }
        catch (InvalidDataException error)
        {
            throw Unavailable(
                "The object location store contains a noncanonical authority key.",
                error
            );
        }
    }

    private static bool MatchesKind(
        ZLinkPlacementObjectKind stored,
        ZLinkLocationObjectKind requested
    ) =>
        requested switch
        {
            ZLinkLocationObjectKind.Actor => stored == ZLinkPlacementObjectKind.Actor,
            ZLinkLocationObjectKind.UserSpot => stored == ZLinkPlacementObjectKind.UserSpot,
            ZLinkLocationObjectKind.InstanceSpot => stored == ZLinkPlacementObjectKind.InstanceSpot,
            _ => false,
        };

    private static bool Matches(ZLinkLocationObjectEntry entry, ZLinkLocationObjectFilter filter) =>
        (
            filter.StableType is null
            || string.Equals(entry.StableType, filter.StableType, StringComparison.Ordinal)
        )
        && (
            filter.MeshName is null
            || string.Equals(entry.MeshName, filter.MeshName, StringComparison.Ordinal)
        );

    internal static long EncodedPageSize(
        long encodedItemsBytes,
        int itemCount,
        string? continuationToken
    ) =>
        Encoding.UTF8.GetByteCount("{\"items\":[")
        + encodedItemsBytes
        + Math.Max(0, itemCount - 1)
        + Encoding.UTF8.GetByteCount("],\"continuationToken\":")
        + (continuationToken is null ? 4 : JsonStringSize(continuationToken))
        + 1;

    internal static long EncodedObjectSize(ZLinkLocationObjectEntry entry) =>
        Encoding.UTF8.GetByteCount("{\"globalId\":")
        + JsonStringSize(entry.GlobalId)
        + Encoding.UTF8.GetByteCount(",\"objectGeneration\":")
        + entry.ObjectGeneration.ToString(System.Globalization.CultureInfo.InvariantCulture).Length
        + Encoding.UTF8.GetByteCount(",\"meshName\":")
        + JsonStringSize(entry.MeshName)
        + Encoding.UTF8.GetByteCount(",\"nodeRid\":")
        + JsonStringSize(entry.NodeRid.ToString())
        + Encoding.UTF8.GetByteCount(",\"state\":")
        + JsonStringSize(entry.State.ToString())
        + Encoding.UTF8.GetByteCount(",\"stableType\":")
        + JsonStringSize(entry.StableType)
        + 1;

    private static long JsonStringSize(string value)
    {
        long bytes = Encoding.UTF8.GetByteCount(value) + 2;
        foreach (var character in value)
        {
            if (character is '"' or '\\')
                bytes++;
            else if (character < 0x20)
                bytes += character is '\b' or '\t' or '\n' or '\f' or '\r' ? 1 : 5;
        }
        return bytes;
    }

    private static ZLinkFrameworkException Unavailable(string message, Exception? error = null) =>
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            message,
            ZLinkRetryAdvice.RetryAfterBackoff,
            error
        );
}
