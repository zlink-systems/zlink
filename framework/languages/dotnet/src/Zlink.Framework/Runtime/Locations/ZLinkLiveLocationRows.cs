namespace Zlink.Framework.Runtime.Locations;

internal sealed class ZLinkLiveLocationRows(ZLinkOwnerLeaseTracker leaseTracker)
{
    public async ValueTask<(TRow? Row, bool LiveRowPresent)> ResolveWithPresenceAsync<TRow>(
        TRow? row,
        Func<TRow, string> ownerOf,
        CancellationToken cancellationToken
    )
        where TRow : class
    {
        if (row is null)
            return (null, false);

        if (
            !await leaseTracker
                .IsOwnerLiveAsync(ownerOf(row), cancellationToken)
                .ConfigureAwait(false)
        )
        {
            if (Diagnostics.ZLinkFrameworkDebugLog.SpotDiscoveryEnabled)
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"live_row_rejected reason=owner_not_live owner={ownerOf(row)}"
                );
            return (null, false);
        }

        return (row, true);
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
                if (Diagnostics.ZLinkFrameworkDebugLog.SpotDiscoveryEnabled)
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
