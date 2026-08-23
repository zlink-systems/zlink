using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using ZoneWorld.Server.Configuration;
using ZoneWorld.Server.ZoneNode.Application.Node;
using ZoneWorld.Server.ZoneNode.Application.Zone;
using ZoneWorld.Server.ZoneNode.Domain.ZoneWorld;
using ZoneWorld.Server.ZoneNode.Infrastructure.ZLink.Spots;
using ZoneWorld.Server.ZoneNode.Ports;
using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.ZoneNode.Infrastructure.ZLink.Actors;

/// <summary>
/// Brings the node up: restores the maintenance state the operator left behind, creates
/// the zone spots this node hosts, and spawns their bots.
///
/// The bots are why the world keeps moving with no client attached, and the four that
/// patrol across the X boundary keep a cross-node actor relocation happening continuously
/// (§2.7, ZW-F2).
/// </summary>
internal sealed class ZoneNodeBootstrap(
    IZLinkSpotManager spots,
    IZLinkActorManager directory,
    IZLinkActorClient actors,
    IMaintenanceStorePort store,
    NodeMaintenancePolicy maintenance,
    NodePlayerCensus census,
    ZoneNodeSettings settings,
    ILogger<ZoneNodeBootstrap> logger) : IHostedService
{
    private const int StartupRetryAttempts = 120;
    private static readonly TimeSpan StartupRetryDelay = TimeSpan.FromMilliseconds(250);

    public async Task StartAsync(CancellationToken cancellationToken)
    {
        await RestoreMaintenanceAsync(cancellationToken);

        // Every eligible process requests the same four global ZoneIds. Placement capacity,
        // not NodeId, distributes two Spot owners to each process. A process that fills its
        // local capacity keeps retrying until the other eligible process is ready and owns the
        // remaining objects.
        var locallyClaimed = new HashSet<string>(StringComparer.Ordinal);
        for (var attempt = 0; census.ZoneIds.Count != 2; attempt++)
        {
            var claimed = ClaimedZones(locallyClaimed);
            var claimOrder = new List<string>();
            foreach (var zoneId in claimed)
                foreach (var adjacent in World.AdjacentZones(zoneId))
                    if (!claimed.Contains(adjacent) && !claimOrder.Contains(adjacent))
                        claimOrder.Add(adjacent);
            foreach (var zoneId in ZoneTopology.Zones)
                if (!claimed.Contains(zoneId) && !claimOrder.Contains(zoneId))
                    claimOrder.Add(zoneId);

            // GetOrCreate can initially observe an object that is still registered to the
            // process the runner just crashed. Re-issuing the canonical create operation is
            // what lets this replacement claim the Zone after that owner expires; merely
            // waiting on the local census would never trigger a new placement decision.
            foreach (var zoneId in claimOrder)
            {
                if (await EnsureZoneAsync(zoneId, cancellationToken))
                    locallyClaimed.Add(zoneId);
                if (!ClaimedZones(locallyClaimed).SequenceEqual(claimed)) break;
            }

            if (settings.AllowEmptyZoneSet
                && census.ZoneIds.Count == 0
                && attempt >= 8)
                break;
            if (attempt + 1 >= StartupRetryAttempts)
                throw new InvalidOperationException(
                    $"Zone Spot capacity did not settle at two local owners. node={maintenance.OwnNodeId}; "
                    + $"zones={string.Join(',', census.ZoneIds)}");
            await Task.Delay(StartupRetryDelay, cancellationToken);
        }
        var zones = census.ZoneIds;

        // A crash replacement must not claim the dead Ready owner's Zone objects. It still
        // advertises the Actor factory and can accept brand-new objects; the G4 runner probes
        // that exact boundary through the replacement RID.
        if (settings.AllowEmptyZoneSet && zones.Count == 0)
        {
            logger.LogInformation(
                "topology=ready node={NodeId} zones= crash_replacement=True",
                maintenance.OwnNodeId);
            return;
        }

        if (!settings.DisableBots)
            foreach (var route in zones.SelectMany(BotPatrolPolicy.RoutesOf))
                await SpawnBotAsync(route, cancellationToken);

        logger.LogInformation(
            "topology=ready node={NodeId} zones={Zones}",
            maintenance.OwnNodeId,
            string.Join(',', zones));
    }

    public Task StopAsync(CancellationToken cancellationToken) => Task.CompletedTask;

    private IReadOnlyList<string> ClaimedZones(IReadOnlySet<string> locallyClaimed) =>
        census.ZoneIds
            .Concat(locallyClaimed)
            .Distinct(StringComparer.Ordinal)
            .OrderBy(value => value, StringComparer.Ordinal)
            .ToArray();

    private async Task<bool> EnsureZoneAsync(string zoneId, CancellationToken cancellationToken)
    {
        for (var attempt = 0; ; attempt++)
        {
            try
            {
                var result = await spots.GetOrCreate(zoneId, ZoneWorldNames.ZoneSpotType)
                    .InMesh(ZoneWorldNames.MeshName)
                    .Request(ZLinkMessage.Empty)
                    .Async(cancellationToken);
                if (result.State is not ZLinkSpotCreateState.Rejected)
                {
                    logger.LogInformation("zone ensured. zone={ZoneId}", zoneId);
                    return result.State is ZLinkSpotCreateState.Created;
                }
            }
            catch (ZLinkFrameworkException exception)
                when (exception.Kind is (ZLinkFrameworkErrorKind.Unavailable
                          or ZLinkFrameworkErrorKind.DeadlineExceeded)
                      && attempt + 1 < StartupRetryAttempts)
            {
            }

            if (attempt + 1 >= StartupRetryAttempts)
                throw new InvalidOperationException(
                    $"Zone Spot creation did not settle. zone={zoneId}");
            await Task.Delay(StartupRetryDelay, cancellationToken);
        }
    }

    /// <summary>
    /// Reads the desired state for every node, not just this one. This node has to judge
    /// moves that leave for another node, so it needs that node's state too (§2.3). The
    /// fanout keeps it current from here on.
    /// </summary>
    private async Task RestoreMaintenanceAsync(CancellationToken cancellationToken)
    {
        var desired = await store.ReadAllAsync(cancellationToken);
        foreach (var (nodeId, enabled) in desired) maintenance.Apply(nodeId, enabled);

        logger.LogInformation(
            "maintenance restored. node={NodeId}, own={Own}, known={Known}",
            maintenance.OwnNodeId,
            maintenance.IsOwnNodeUnderMaintenance,
            desired.Count);
    }

    private async Task SpawnBotAsync(BotRoute route, CancellationToken cancellationToken)
    {
        for (var attempt = 0; ; attempt++)
        {
            try
            {
                await SpawnBotCoreAsync(route, cancellationToken);
                return;
            }
            catch (ZLinkFrameworkException exception)
                when (exception.Kind is (ZLinkFrameworkErrorKind.Unavailable
                          or ZLinkFrameworkErrorKind.DeadlineExceeded)
                      && attempt + 1 < StartupRetryAttempts)
            {
                await Task.Delay(StartupRetryDelay, cancellationToken);
            }
        }
    }

    private async Task SpawnBotCoreAsync(
        BotRoute route,
        CancellationToken cancellationToken)
    {
        // A bot outlives the node that first requested it. GetOrCreate resolves the global
        // ActorId and joins a concurrent claim instead of doing a separate check-before-create.
        // A placement or Store failure remains visible so the node cannot report a complete
        // topology while a required bot is missing.
        var result = await directory
            .GetOrCreate(route.PlayerId, ZoneWorldNames.PlayerActorType)
            .InMesh(ZoneWorldNames.MeshName)
            .Request(ZLinkMessage.Empty)
            .Async(cancellationToken);

        if (result is ZLinkActorCreateResult.Existing)
        {
            logger.LogInformation(
                "bot already exists. bot={PlayerId}",
                route.PlayerId);
            return;
        }
        if (result is not ZLinkActorCreateResult.Created created)
            throw new InvalidOperationException("Bot Actor creation was rejected.");

        var entered = await actors
            .RequestToActor(
                created.Actor.ActorId,
                new EnterWorldReq(route.X, route.Y, IsBot: true, route.DirX, route.DirY))
            .Async<EnterWorldRes>(cancellationToken);

        logger.LogInformation(
            "bot spawned. bot={PlayerId}, zone={ZoneId}, start=({X},{Y}), dir=({DirX},{DirY})",
            route.PlayerId,
            entered.ZoneId,
            route.X,
            route.Y,
            route.DirX,
            route.DirY);
    }
}
