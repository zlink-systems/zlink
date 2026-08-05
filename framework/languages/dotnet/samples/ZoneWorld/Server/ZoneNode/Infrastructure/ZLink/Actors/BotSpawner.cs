using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using ZoneWorld.Server.Configuration;
using ZoneWorld.Server.ZoneNode.Application.Node;
using ZoneWorld.Server.ZoneNode.Application.Zone;
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
    ZoneNodeSettings settings,
    ILogger<ZoneNodeBootstrap> logger) : IHostedService
{
    public async Task StartAsync(CancellationToken cancellationToken)
    {
        await RestoreMaintenanceAsync(cancellationToken);

        var zones = ZoneTopology.ZonesOf(maintenance.OwnNodeId);
        foreach (var zoneId in zones)
        {
            var result = await spots.GetOrCreate(zoneId, ZoneWorldNames.ZoneSpotType)
                .InMesh(ZoneWorldNames.MeshName)
                .Request(ZLinkMessage.Empty)
                .Async(cancellationToken);
            if (result.State is ZLinkSpotCreateState.Rejected)
                throw new InvalidOperationException(
                    $"Zone Spot creation was rejected. zone={zoneId}");
            logger.LogInformation("zone ensured. zone={ZoneId}", zoneId);
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
            "bot spawned. bot={PlayerId}, zone={ZoneId}, dir=({DirX},{DirY})",
            route.PlayerId,
            entered.ZoneId,
            route.DirX,
            route.DirY);
    }
}
