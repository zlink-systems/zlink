using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Spots;
using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.Gateway.Infrastructure.ZLink.Sessions;

/// <summary>
/// Serves runner-only location probes through the Gateway's existing Object Client.
/// NodeRid values are evidence only and are never used for application routing.
/// </summary>
public sealed class RelocationProbeService(
    IZLinkSpotManager spots,
    IZLinkActorManager actors)
{
    private static readonly (string Source, string Target)[] AdjacentPairs =
    [
        (ZoneIds.NorthWest, ZoneIds.NorthEast),
        (ZoneIds.NorthWest, ZoneIds.SouthWest),
        (ZoneIds.NorthEast, ZoneIds.SouthEast),
        (ZoneIds.SouthWest, ZoneIds.SouthEast)
    ];

    public async ValueTask<RelocationPairRes> SelectPairAsync(
        CancellationToken cancellationToken)
    {
        foreach (var (sourceZoneId, targetZoneId) in AdjacentPairs)
        {
            var source = await spots.FindAsync(sourceZoneId, cancellationToken);
            var target = await spots.FindAsync(targetZoneId, cancellationToken);
            if (source is null
                || target is null
                || source.Value.NodeRid.Equals(target.Value.NodeRid))
                continue;

            return new RelocationPairRes(
                sourceZoneId,
                targetZoneId,
                source.Value.NodeRid.ToString(),
                target.Value.NodeRid.ToString());
        }

        return new RelocationPairRes(
            string.Empty,
            string.Empty,
            string.Empty,
            string.Empty,
            "NoCrossNodeAdjacentPair");
    }

    public async ValueTask<ActorLocationProbeRes> FindActorAsync(
        string actorId,
        CancellationToken cancellationToken)
    {
        var actor = await actors.FindAsync(actorId, cancellationToken);
        return actor is null
            ? new ActorLocationProbeRes(actorId, 0, string.Empty, "ActorNotFound")
            : new ActorLocationProbeRes(
                actor.Value.ActorId,
                actor.Value.ObjectGeneration,
                actor.Value.NodeRid.ToString());
    }
}
