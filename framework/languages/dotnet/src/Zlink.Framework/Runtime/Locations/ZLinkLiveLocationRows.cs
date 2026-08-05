namespace Zlink.Framework.Runtime.Locations;

internal sealed class ZLinkLiveLocationRows(ZLinkOwnerLeaseTracker leaseTracker)
{
    public async ValueTask<TRow?> ResolveAsync<TRow>(
        TRow? row,
        Func<TRow, string> ownerOf,
        Func<TRow, bool> acceptObserved,
        CancellationToken cancellationToken)
        where TRow : class
    {
        var (resolved, _) = await ResolveWithPresenceAsync(
                row,
                ownerOf,
                acceptObserved,
                cancellationToken)
            .ConfigureAwait(false);
        return resolved;
    }

    public async ValueTask<(TRow? Row, bool LiveRowPresent)> ResolveWithPresenceAsync<TRow>(
        TRow? row,
        Func<TRow, string> ownerOf,
        Func<TRow, bool> acceptObserved,
        CancellationToken cancellationToken)
        where TRow : class
    {
        if (row is null) return (null, false);

        // Liveness gates the observation: a dead-owner row must never record a
        // generation floor, or the successor incarnation's fresh row (whose
        // axes legitimately restart) would be rejected as a lagging replica.
        if (!await leaseTracker.IsOwnerLiveAsync(ownerOf(row), cancellationToken).ConfigureAwait(false))
        {
            //  Both rejections below return a null row, and the caller cannot
            //  tell a dead owner from a lagging replica without being told.
            Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                $"live_row_rejected reason=owner_not_live owner={ownerOf(row)}");
            return (null, false);
        }

        var accepted = acceptObserved(row);
        if (!accepted)
            Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                $"live_row_rejected reason=stale_observation owner={ownerOf(row)}");
        return (accepted ? row : null, true);
    }

    public async ValueTask<IReadOnlyList<TRow>> FilterAsync<TRow>(
        IReadOnlyList<TRow> rows,
        Func<TRow, string> ownerOf,
        Func<TRow, bool> acceptObserved,
        CancellationToken cancellationToken,
        Func<TRow, long>? ownerLeaseGenerationOf = null)
    {
        var live = new List<TRow>(rows.Count);
        foreach (var row in rows)
        {
            var ownerLive = ownerLeaseGenerationOf is null
                ? await leaseTracker.IsOwnerLiveAsync(
                        ownerOf(row),
                        cancellationToken)
                    .ConfigureAwait(false)
                : await leaseTracker.IsOwnerTokenLiveAsync(
                        new ZLinkLocationOwnerToken(
                            ownerOf(row),
                            ownerLeaseGenerationOf(row)),
                        cancellationToken)
                    .ConfigureAwait(false);
            if (!ownerLive)
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"live_row_filter rejected=owner_not_live owner={ownerOf(row)}");
                continue;
            }

            if (acceptObserved(row))
                live.Add(row);
            else
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"live_row_filter rejected=stale_observation owner={ownerOf(row)}");
        }

        return live;
    }
}
