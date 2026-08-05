namespace Zlink.Framework.Runtime.Locations;

internal sealed class ZLinkSpotMeshLocationResolver
{
    private readonly IReadOnlyList<string> _meshNames;
    private readonly ZLinkStoreLocationResolvers _rows;

    internal ZLinkSpotMeshLocationResolver(
        ZLinkFrameworkRegistration registration,
        ZLinkStoreLocationResolvers rows)
    {
        _rows = rows;
        _meshNames = registration.SpotNodes.Values
            .Select(static node => node.SpotMeshChannelName ?? node.SpotNodeName)
            .Distinct(StringComparer.Ordinal)
            .ToArray();
    }

    internal async ValueTask<ZLinkActorLocation?> ResolveActorAsync(
        string actorId,
        CancellationToken cancellationToken)
    {
        var (row, _) = await ResolveActorWithPresenceAsync(actorId, cancellationToken)
            .ConfigureAwait(false);
        return row;
    }

    /// <summary>See ZLinkStoreLocationResolvers.ResolveActorRowWithPresenceAsync.</summary>
    internal async ValueTask<(ZLinkActorLocation? Row, bool RowPresent)>
        ResolveActorWithPresenceAsync(
            string actorId,
            CancellationToken cancellationToken)
    {
        var present = false;
        foreach (var meshName in _meshNames)
        {
            var (row, rowPresent) = await _rows.ResolveActorRowWithPresenceAsync(
                    new ZLinkActorLocationKey(actorId),
                    cancellationToken)
                .ConfigureAwait(false);
            if (row is not null) return (row.ToPublic(), true);
            present |= rowPresent;
        }

        return (null, present);
    }

    internal async ValueTask<ZLinkSpotLocation?> ResolveAsync(
        string spotId,
        CancellationToken cancellationToken)
    {
        foreach (var meshName in _meshNames)
        {
            var row = await _rows.ResolveSpotRowAsync(
                    new ZLinkSpotLocationKey(spotId),
                    cancellationToken)
                .ConfigureAwait(false);
            if (row is not null) return row.ToPublic();
        }

        return null;
    }
}
