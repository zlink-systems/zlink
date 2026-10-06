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
    ILogger<ZoneNodeBootstrap> logger
) : IHostedService
{
    private const int StartupRetryAttempts = 120;
    private const int ReplacementReadyAttempts = 8;
    private static readonly TimeSpan StartupRetryDelay = TimeSpan.FromMilliseconds(250);

    public async Task StartAsync(CancellationToken cancellationToken)
    {
        await RestoreMaintenanceAsync(cancellationToken);

        // A replacement claims no Zone. Only the initial cold start claims: a Ready owner
        // failure is not an automatic replacement, so the Zones the previous incarnation owned
        // stay registered to it, and the Zones a graceful stop released belong to the next cold
        // start rather than to the process coming back. Claiming here would also let a
        // replacement claim a Zone, violating the empty zone set a
        // replacement announces — and that is a state this bootstrap cannot leave.
        if (settings.AllowEmptyZoneSet)
        {
            for (var attempt = 0; attempt < ReplacementReadyAttempts; attempt++)
                await Task.Delay(StartupRetryDelay, cancellationToken);
            if (census.ZoneIds.Count != 0)
                throw new InvalidOperationException(
                    "A replacement must reach ready with no Zone of its own. "
                        + $"node={maintenance.OwnNodeId}; zones={string.Join(',', census.ZoneIds)}"
                );
            logger.LogInformation("topology=ready node={NodeId} zones=", maintenance.OwnNodeId);
            return;
        }

        for (var attempt = 0; census.ZoneIds.Count != settings.ZoneCapacity; attempt++)
        {
            var claimed = census.ZoneIds;
            // Re-issuing the canonical create operation every round is what lets this process
            // claim a Zone as soon as capacity frees up; merely waiting on the local census
            // would never trigger a new placement decision.
            foreach (var zoneId in ZoneTopology.Zones)
            {
                await EnsureZoneAsync(zoneId, cancellationToken);
                if (!census.ZoneIds.SequenceEqual(claimed))
                    break;
            }

            if (attempt + 1 >= StartupRetryAttempts)
                throw new InvalidOperationException(
                    $"Zone Spot capacity did not settle. node={maintenance.OwnNodeId}; "
                        + $"zones={string.Join(',', census.ZoneIds)}"
                );
            await Task.Delay(StartupRetryDelay, cancellationToken);
        }
        var zones = census.ZoneIds;

        if (!settings.DisableBots)
            foreach (var route in zones.SelectMany(BotPatrolPolicy.RoutesOf))
                await SpawnBotAsync(route, cancellationToken);

        logger.LogInformation(
            "topology=ready node={NodeId} zones={Zones}",
            maintenance.OwnNodeId,
            string.Join(',', zones)
        );
    }

    public Task StopAsync(CancellationToken cancellationToken) => Task.CompletedTask;

    /// <summary>
    /// One claim attempt for one Zone. The claim loop in <see cref="StartAsync"/> owns the
    /// retry budget (§3, `250 ms` × `120`) and the startup failure, so a Zone this attempt
    /// could not take is simply not claimed: another node may still be starting, or the Zone
    /// belongs to an owner that is gone and whose object Framework never reassigns (§7.5).
    /// Retrying here as well would spend the whole budget on the first such Zone and never let
    /// the claim loop reach its own decision.
    /// </summary>
    private async Task EnsureZoneAsync(string zoneId, CancellationToken cancellationToken)
    {
        try
        {
            var result = await spots
                .GetOrCreate(zoneId, ZoneWorldNames.ZoneSpotType)
                .InMesh(ZoneWorldNames.MeshName)
                .Request(ZLinkMessage.Empty)
                .Async(cancellationToken);
            if (result.State is ZLinkSpotCreateState.Rejected)
                return;
            logger.LogInformation("zone ensured. zone={ZoneId}", zoneId);
        }
        catch (ZLinkFrameworkException exception)
            when (exception.Kind
                    is ZLinkFrameworkErrorKind.Unavailable
                        or ZLinkFrameworkErrorKind.DeadlineExceeded
            )
        {
            return;
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
        foreach (var (nodeId, enabled) in desired)
            maintenance.Apply(nodeId, enabled);

        logger.LogInformation(
            "maintenance restored. node={NodeId}, own={Own}, known={Known}",
            maintenance.OwnNodeId,
            maintenance.IsOwnNodeUnderMaintenance,
            desired.Count
        );
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
                when (exception.Kind
                        is (
                            ZLinkFrameworkErrorKind.Unavailable
                            or ZLinkFrameworkErrorKind.DeadlineExceeded
                        )
                    && attempt + 1 < StartupRetryAttempts
                )
            {
                await Task.Delay(StartupRetryDelay, cancellationToken);
            }
        }
    }

    private async Task SpawnBotCoreAsync(BotRoute route, CancellationToken cancellationToken)
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
            logger.LogInformation("bot already exists. bot={PlayerId}", route.PlayerId);
            return;
        }
        if (result is not ZLinkActorCreateResult.Created created)
            throw new InvalidOperationException("Bot Actor creation was rejected.");

        await actors
            .RequestToActor(
                created.Actor.ActorId,
                new EnterWorldReq(route.X, route.Y, IsBot: true, route.DirX, route.DirY)
            )
            .Async<EnterWorldRes>(cancellationToken);
    }
}
