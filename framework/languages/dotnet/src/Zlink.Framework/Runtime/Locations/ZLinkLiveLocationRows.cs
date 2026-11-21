namespace Zlink.Framework.Runtime.Locations;

internal sealed class ZLinkLiveLocationRows(ZLinkOwnerLeaseTracker leaseTracker)
{
    public async ValueTask<(
        TRow? Row,
        ZLinkOwnerLeaseTracker.Snapshot? Lease
    )> ResolveWithPresenceAsync<TRow>(
        TRow? row,
        Func<TRow, string> ownerOf,
        CancellationToken cancellationToken
    )
        where TRow : class
    {
        if (row is null)
            return (null, null);

        var owner = ownerOf(row);
        var lease = await leaseTracker
            .ReadLiveSnapshotAsync(owner, cancellationToken)
            .ConfigureAwait(false);
        if (lease is null)
        {
            if (Diagnostics.ZLinkFrameworkDebugLog.SpotDiscoveryEnabled)
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"live_row_rejected reason=owner_not_live owner={owner}"
                );
            return (null, null);
        }

        return (row, lease);
    }

    public async ValueTask<IReadOnlyList<TRow>> FilterAsync<TRow>(
        IReadOnlyList<TRow> rows,
        Func<TRow, string> ownerOf,
        CancellationToken cancellationToken,
        Func<TRow, long>? ownerLeaseGenerationOf = null
    )
    {
        var live = new List<TRow>(rows.Count);
        foreach (var row in rows)
        {
            var ownerLive = ownerLeaseGenerationOf is null
                ? await leaseTracker
                    .IsOwnerLiveAsync(ownerOf(row), cancellationToken)
                    .ConfigureAwait(false)
                : await leaseTracker
                    .IsOwnerTokenLiveAsync(
                        new ZLinkLocationOwnerToken(ownerOf(row), ownerLeaseGenerationOf(row)),
                        cancellationToken
                    )
                    .ConfigureAwait(false);
            if (!ownerLive)
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"live_row_filter rejected=owner_not_live owner={ownerOf(row)}"
                );
                continue;
            }

            live.Add(row);
        }

        return live;
    }
}
