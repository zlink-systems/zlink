using Systems.Zlink.Stream.Connector.Contracts;
using System.Text;
using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Client;

/// <summary>
/// The self-check from §11. Every assertion here is observable on the wire, which is why
/// no browser is involved: a browser would only add rendering and timing between the
/// server's behaviour and the verdict.
/// </summary>
public static class Scenarios
{
    /// <summary>
    /// Scenarios the client drives end to end. Running "all" runs these.
    /// </summary>
    public static IReadOnlyDictionary<string, Func<ClientOptions, CancellationToken, ValueTask>> All =>
        new Dictionary<string, Func<ClientOptions, CancellationToken, ValueTask>>(StringComparer.OrdinalIgnoreCase)
        {
            ["ZW-A1"] = A1EnterAndMove,
            ["ZW-A2"] = A2RejectionOrder,
            ["ZW-A3"] = A3SameZonePlayers,
            ["ZW-A4"] = A4DiagonalCrossing,
            ["ZW-A5"] = A5SameZonePositionUpdate,
            ["ZW-B1"] = B1BorderSync,
            ["ZW-B2"] = B2CrossNodeRelocation,
            ["ZW-B3"] = B3IntraNodeZoneChange,
            ["ZW-B5"] = B5ActorGenerationPreserved,
            ["ZW-B6"] = B6MessageFollow,
            ["ZW-C1"] = C1WatchNodes,
            ["ZW-C4"] = C4SpotEventReported,
            ["ZW-D1"] = D1AnnounceAllNodes,
            ["ZW-E1"] = E1TargetedMaintenance,
            ["ZW-E2"] = E2MaintainedNodeKeepsMoving,
            ["ZW-E3"] = E3LeavingMaintainedNode,
            ["ZW-E4"] = E4NodeDiagnostics,
            ["ZW-E6"] = E6MaintenanceBlocksEntry,
            ["ZW-F1"] = F1BotsPresent,
            ["ZW-F3"] = F3NoPushToBots,
            ["ZW-F4"] = F4BotReversesOnRejection
        };

    /// <summary>
    /// Scenarios that need the runner to stop or restart a node while they watch. They are
    /// addressed by id, never by "all": the runner has to disrupt the topology around them, so
    /// running them blind would just make them time out.
    /// </summary>
    public static IReadOnlyDictionary<string, Func<ClientOptions, CancellationToken, ValueTask>> RunnerDriven =>
        new Dictionary<string, Func<ClientOptions, CancellationToken, ValueTask>>(StringComparer.OrdinalIgnoreCase)
        {
            ["ZW-B4"] = B4BorderSnapshotExpiry,
            ["ZW-C2"] = C2NodeShutdown,
            ["ZW-C3"] = C3NodeDisconnected,
            ["ZW-E5-arm"] = E5Arm,
            ["ZW-E5"] = E5MaintenanceRestored,
            ["ZW-G2"] = G2ReverseStartedNodeOperations
        };

    // --- Track A: entry and movement ----------------------------------------

    private static async ValueTask A1EnterAndMove(ClientOptions options, CancellationToken ct)
    {
        await using var player = await GameClient.ConnectAsync(options, Unique("a1"), ct);
        var join = await player.JoinWorldAsync(ct);

        ZlinkStreamAssert.Ensure(object.Equals(ZoneIds.NorthWest, join.ZoneId), "a new player spawns in zone-nw");
        ZlinkStreamAssert.Ensure(object.Equals(ZoneWorldSpec.SpawnX, join.X), "the spawn coordinate is fixed");
        ZlinkStreamAssert.Ensure(object.Equals(ZoneWorldSpec.SpawnY, join.Y), "the spawn coordinate is fixed");

        var targetX = join.X + 3;
        var targetY = join.Y + 2;
        var waiting = player.Connector.WaitFor<ZoneStateNotify>()
            .Where(message => message.Payload.Players.Any(p =>
                p.PlayerId == player.PlayerId && p.X == targetX && p.Y == targetY))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async(ct);
        await player.MoveAsync(targetX, targetY);
        var state = (await waiting).Payload;
        player.Position = (targetX, targetY);

        ZlinkStreamAssert.Ensure(object.Equals(ZoneIds.NorthWest, state.ZoneId), "the move stayed inside zone-nw");
    }

    /// <summary>
    /// A move can break several rules at once, and every language must name the same one.
    /// This target is out of range *and* further than the step cap; the fixed order says
    /// OutOfRange wins (§2.2).
    /// </summary>
    private static async ValueTask A2RejectionOrder(ClientOptions options, CancellationToken ct)
    {
        await using var player = await GameClient.ConnectAsync(options, Unique("a2"), ct);
        var join = await player.JoinWorldAsync(ct);

        var waiting = player.Connector.WaitFor<MoveRejectedNotify>()
            .Timeout(TimeSpan.FromSeconds(15))
            .Async(ct);
        await player.MoveAsync(-40, join.Y);
        var rejected = (await waiting).Payload;

        ZlinkStreamAssert.Ensure(object.Equals(MoveRejectReasons.OutOfRange, rejected.Reason), "OutOfRange is checked before TooFar");
        ZlinkStreamAssert.Ensure(object.Equals(join.X, rejected.X), "a refused move leaves the coordinate untouched");
        ZlinkStreamAssert.Ensure(object.Equals(join.Y, rejected.Y), "a refused move leaves the coordinate untouched");
    }

    private static async ValueTask A3SameZonePlayers(ClientOptions options, CancellationToken ct)
    {
        var firstId = Unique("a3-b");
        var secondId = Unique("a3-a");

        await using var first = await GameClient.ConnectAsync(options, firstId, ct);
        await first.JoinWorldAsync(ct);
        await using var second = await GameClient.ConnectAsync(options, secondId, ct);
        await second.JoinWorldAsync(ct);

        // Each client sees the other — the canonical says so of both, not of one (§11 ZW-A3).
        foreach (var (client, other) in new[] { (first, secondId), (second, firstId) })
        {
            var state = (await client.Connector.WaitFor<ZoneStateNotify>()
                .Where(message => message.Payload.Players.Any(p => p.PlayerId == firstId)
                                  && message.Payload.Players.Any(p => p.PlayerId == secondId))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(ct)).Payload;

            ZlinkStreamAssert.Ensure(
                state.Players.Any(p => p.PlayerId == other),
                "both clients are in the same zone, so each is in the other's Players");

            // The ordering rule covers the whole list, bots included: it is what makes every
            // language produce the same list from the same world.
            var ids = state.Players.Select(p => p.PlayerId).ToArray();
            ZlinkStreamAssert.Ensure((
                ids.OrderBy(id => id, StringComparer.Ordinal)).SequenceEqual(
                ids),
                "Players is ordered by PlayerId as UTF-8 bytes");
        }
    }

    /// <summary>A step to (50,50) from (49,49) crosses both boundaries at once and would land
    /// in a zone that shares no edge with the current one, so it is refused (§2.2).</summary>
    private static async ValueTask A4DiagonalCrossing(ClientOptions options, CancellationToken ct)
    {
        await using var player = await GameClient.ConnectAsync(options, Unique("a4"), ct);
        await player.JoinWorldAsync(ct);
        foreach (var step in player.PlanWalkWithinZone(49, 49))
        {
            var arrived = player.Connector.WaitFor<ZoneStateNotify>()
                .Where(message => message.Payload.Players.Any(p =>
                    p.PlayerId == player.PlayerId && p.X == step.X && p.Y == step.Y))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(ct);
            await player.MoveAsync(step.X, step.Y);
            await arrived;
            player.Position = step;
        }

        var waiting = player.Connector.WaitFor<MoveRejectedNotify>()
            .Timeout(TimeSpan.FromSeconds(15))
            .Async(ct);
        await player.MoveAsync(50, 50);
        var rejected = (await waiting).Payload;

        ZlinkStreamAssert.Ensure(object.Equals(MoveRejectReasons.DiagonalCrossing, rejected.Reason), "a move may not cross both boundaries");
        ZlinkStreamAssert.Ensure(object.Equals(49, rejected.X), "a refused move leaves the coordinate untouched");
        ZlinkStreamAssert.Ensure(object.Equals(49, rejected.Y), "a refused move leaves the coordinate untouched");
    }

    private static async ValueTask A5SameZonePositionUpdate(ClientOptions options, CancellationToken ct)
    {
        await using var player = await GameClient.ConnectAsync(options, Unique("a5"), ct);
        var join = await player.JoinWorldAsync(ct);

        var targetX = join.X + 4;
        var waiting = player.Connector.WaitFor<ZoneStateNotify>()
            .Where(message => message.Payload.Players.Any(p =>
                p.PlayerId == player.PlayerId && p.X == targetX && p.Y == join.Y))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async(ct);
        await player.MoveAsync(targetX, join.Y);
        var state = (await waiting).Payload;
        player.Position = (targetX, join.Y);

        var me = player.Me(state);
        ZlinkStreamAssert.Ensure(object.Equals(join.X + 4, me.X), "the zone spot's copy follows the actor's coordinate");
        ZlinkStreamAssert.Ensure(object.Equals(ZoneIds.NorthWest, me.ZoneId), "the zone did not change");
    }

    // --- Track B: borders and relocation -------------------------------------

    /// <summary>
    /// A player inside the border band is visible from the zone across that edge — and only
    /// that one. The diagonal zone shares no edge, so it never sees them (§4.1).
    /// </summary>
    private static async ValueTask B1BorderSync(ClientOptions options, CancellationToken ct)
    {
        var westId = Unique("b1-w");
        var eastId = Unique("b1-e");
        var diagonalId = Unique("b1-d");

        await using var west = await GameClient.ConnectAsync(options, westId, ct);
        await west.JoinWorldAsync(ct);
        await using var east = await GameClient.ConnectAsync(options, eastId, ct);
        await east.JoinWorldAsync(ct);
        await using var diagonal = await GameClient.ConnectAsync(options, diagonalId, ct);
        await diagonal.JoinWorldAsync(ct);

        // The eastern player crosses into zone-ne, which shares zone-nw's X edge. The diagonal
        // player goes to zone-se, which shares no edge with zone-nw at all. The western one
        // stands in zone-nw's band, close enough to be visible across an edge it shares.
        foreach (var (client, route) in new[]
                 {
                     (east, new[]
                     {
                         (X: 48, Y: 25, ChangesZone: false),
                         (X: 52, Y: 25, ChangesZone: true),
                         (X: 55, Y: 25, ChangesZone: false)
                     }),
                     (diagonal, new[]
                     {
                         (X: 48, Y: 48, ChangesZone: false),
                         (X: 52, Y: 48, ChangesZone: true),
                         (X: 55, Y: 48, ChangesZone: false),
                         (X: 55, Y: 52, ChangesZone: true),
                         (X: 55, Y: 55, ChangesZone: false)
                     })
                 })
        {
            foreach (var target in route)
            {
                if (target.ChangesZone)
                {
                    var changed = client.Connector.WaitFor<ZoneChangedNotify>()
                        .Where(message => message.Payload.PlayerId == client.PlayerId)
                        .Timeout(TimeSpan.FromSeconds(15))
                        .Async(ct);
                    await client.MoveAsync(target.X, target.Y);
                    await changed;
                    client.Position = (target.X, target.Y);
                    continue;
                }

                foreach (var step in client.PlanWalkWithinZone(target.X, target.Y))
                {
                    var arrived = client.Connector.WaitFor<ZoneStateNotify>()
                        .Where(message => message.Payload.Players.Any(p =>
                            p.PlayerId == client.PlayerId && p.X == step.X && p.Y == step.Y))
                        .Timeout(TimeSpan.FromSeconds(15))
                        .Async(ct);
                    await client.MoveAsync(step.X, step.Y);
                    await arrived;
                    client.Position = step;
                }
            }
        }
        Console.WriteLine("scenario ZW-B1 checkpoint=remote-players-positioned");

        // Arm the cross-border observation immediately before the western player enters the
        // border band. Starting this timeout before the two setup walks would spend most of
        // its observation budget on unrelated movement.
        var borderVisible = east.Connector.WaitFor<ZoneStateNotify>()
            .Where(message => message.Payload.ZoneId == ZoneIds.NorthEast
                              && message.Payload.Players.Any(p => p.PlayerId == westId))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async(ct);
        foreach (var step in west.PlanWalkWithinZone(45, 45))
        {
            var arrived = west.Connector.WaitFor<ZoneStateNotify>()
                .Where(message => message.Payload.Players.Any(p =>
                    p.PlayerId == west.PlayerId && p.X == step.X && p.Y == step.Y))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(ct);
            await west.MoveAsync(step.X, step.Y);
            await arrived;
            west.Position = step;
        }
        Console.WriteLine("scenario ZW-B1 checkpoint=west-in-border-band");

        var seenFromEast = (await borderVisible).Payload;
        Console.WriteLine("scenario ZW-B1 checkpoint=east-observed-west");

        var neighbour = seenFromEast.Players.First(p => p.PlayerId == westId);
        ZlinkStreamAssert.Ensure(object.Equals(ZoneIds.NorthWest, neighbour.ZoneId), "the neighbour is reported with its own zone");
        ZlinkStreamAssert.Ensure(neighbour.X >= 40, "only players inside the band cross the border");

        // The negative control: zone-se shares no edge with zone-nw, so the same player must
        // never appear there — not once, over a run of ticks (§4.1). Only zone-se's own ticks
        // count; the walk across the map leaves stragglers from the zones it passed through.
        for (var tick = 0; tick < BorderObservationTicks; tick++)
        {
            var state = (await diagonal.Connector.WaitFor<ZoneStateNotify>()
                .Where(message => message.Payload.ZoneId == ZoneIds.SouthEast
                                  && message.Payload.Players.Any(p => p.PlayerId == diagonalId))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(ct)).Payload;
            ZlinkStreamAssert.Ensure(
                state.Players.All(p => p.PlayerId != westId),
                "a zone that shares no edge never sees the player across the diagonal");
        }
        Console.WriteLine("scenario ZW-B1 checkpoint=diagonal-exclusion-observed");
    }

    /// <summary>Long enough for a border snapshot to have arrived and expired twice over, so
    /// "never appears" is an observation rather than a coincidence of timing.</summary>
    private const int BorderObservationTicks = ZoneWorldSpec.BorderSnapshotExpiryTicks * 2;

    /// <summary>
    /// Selects adjacent zones with different current owners, crosses their shared
    /// boundary, and proves that the client connection remains usable (ZW-B2).
    /// </summary>
    private static async ValueTask B2CrossNodeRelocation(ClientOptions options, CancellationToken ct)
    {
        var playerId = Unique("b2");
        await using var probes = await RelocationProbeClient.ConnectAsync(
            options.GatewayEndpoint,
            ct);
        var pair = await probes.SelectPairAsync(ct);
        ZlinkStreamAssert.Ensure(
            pair.Error is null,
            "a release run requires adjacent Zone Spots with different current owners");

        await using (var player = await GameClient.ConnectAsync(options, playerId, ct))
        {
            var initialState = player.Connector.WaitFor<ZoneStateNotify>()
                .Where(message => message.Payload.Players.Any(p => p.PlayerId == playerId))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(ct);
            await player.JoinWorldAsync(ct);
            await initialState;
            var source = ZoneCenter(pair.SourceZoneId);
            var target = ZoneCenter(pair.TargetZoneId);
            await MoveToAsync(player, source.X, source.Y, ct);
            var before = await probes.FindActorAsync(playerId, ct);
            ZlinkStreamAssert.Ensure(
                before.Error is null && before.OwnerNodeRid == pair.SourceOwnerNodeRid,
                "the selected source zone owns the actor before relocation");

            await MoveToAsync(player, target.X, target.Y, ct);
            var after = await probes.FindActorAsync(playerId, ct);
            ZlinkStreamAssert.Ensure(
                after.Error is null
                && after.OwnerNodeRid == pair.TargetOwnerNodeRid
                && after.OwnerNodeRid != before.OwnerNodeRid,
                "the actor moved to the selected adjacent zone's different owner");

            // The same connection keeps working: the bound session followed the actor.
            var continuedX = target.X + (target.X < ZoneWorldSpec.ZoneSplit ? 1 : -1);
            var stateWait = player.Connector.WaitFor<ZoneStateNotify>()
                .Where(message => message.Payload.Players.Any(p =>
                    p.PlayerId == player.PlayerId && p.X == continuedX && p.Y == target.Y))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(ct);
            await player.MoveAsync(continuedX, target.Y);
            var state = (await stateWait).Payload;
            player.Position = (continuedX, target.Y);
            ZlinkStreamAssert.Ensure(
                object.Equals(pair.TargetZoneId, state.ZoneId),
                "the client keeps playing on the new owner");
        }

        // Global player identity outlives the connection and its original owner.
        await using var rejoined = await GameClient.ConnectAsync(options, playerId, ct);
        var resumed = await rejoined.JoinWorldAsync(ct);
        ZlinkStreamAssert.Ensure(
            object.Equals(pair.TargetZoneId, resumed.ZoneId),
            "rejoin keeps the relocated actor's zone");
    }

    private static async ValueTask B5ActorGenerationPreserved(
        ClientOptions options,
        CancellationToken ct)
    {
        var playerId = Unique("b5");
        await using var probes = await RelocationProbeClient.ConnectAsync(
            options.GatewayEndpoint,
            ct);
        var pair = await probes.SelectPairAsync(ct);
        ZlinkStreamAssert.Ensure(
            pair.Error is null,
            "a release run requires adjacent Zone Spots with different current owners");

        await using var player = await GameClient.ConnectAsync(options, playerId, ct);
        var initialState = player.Connector.WaitFor<ZoneStateNotify>()
            .Where(message => message.Payload.Players.Any(p => p.PlayerId == playerId))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async(ct);
        await player.JoinWorldAsync(ct);
        await initialState;
        var source = ZoneCenter(pair.SourceZoneId);
        var target = ZoneCenter(pair.TargetZoneId);
        await MoveToAsync(player, source.X, source.Y, ct);
        var before = await probes.FindActorAsync(playerId, ct);
        await MoveToAsync(player, target.X, target.Y, ct);
        var after = await probes.FindActorAsync(playerId, ct);

        ZlinkStreamAssert.Ensure(
            before.Error is null && after.Error is null,
            "the operational probe resolves the actor on both sides of relocation");
        ZlinkStreamAssert.Ensure(
            before.ObjectGeneration == after.ObjectGeneration,
            "relocation preserves ObjectGeneration");
        ZlinkStreamAssert.Ensure(
            before.OwnerNodeRid != after.OwnerNodeRid,
            "relocation changes the current owner");
    }

    /// <summary>
    /// Primes the Gateway's normal public Actor client before relocation, then submits a one-way
    /// probe and a request through that same client immediately after the owner changes. The
    /// bounded route cache makes both calls enter the previous owner, where Message Follow must
    /// deliver them to the committed target without application retry or route reconstruction.
    /// </summary>
    private static async ValueTask B6MessageFollow(ClientOptions options, CancellationToken ct)
    {
        var playerId = Unique("b6");
        await using var probes = await RelocationProbeClient.ConnectAsync(
            options.GatewayEndpoint,
            ct);
        var pair = await probes.SelectPairAsync(ct);
        ZlinkStreamAssert.Ensure(
            pair.Error is null,
            "a release run requires adjacent Zone Spots with different current owners");

        await using var player = await GameClient.ConnectAsync(options, playerId, ct);
        var initialState = player.Connector.WaitFor<ZoneStateNotify>()
            .Where(message => message.Payload.Players.Any(p => p.PlayerId == playerId))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async(ct);
        await player.JoinWorldAsync(ct);
        await initialState;

        var source = ZoneCenter(pair.SourceZoneId);
        var target = ZoneCenter(pair.TargetZoneId);
        await MoveToAsync(player, source.X, source.Y, ct);
        var before = await probes.FindActorAsync(playerId, ct);
        ZlinkStreamAssert.Ensure(
            before.Error is null && before.OwnerNodeRid == pair.SourceOwnerNodeRid,
            "the selected source zone owns the actor before Message Follow is primed");

        var primed = await probes.PrimeMessageFollowRouteAsync(playerId, ct);
        ZlinkStreamAssert.Ensure(
            primed.ProbeId.StartsWith("prime-", StringComparison.Ordinal)
            && primed.Payload.AsSpan().SequenceEqual("route-prime"u8),
            "the public Actor request can prime the source route without changing its payload");

        await MoveToAsync(player, target.X, target.Y, ct);
        var after = await probes.FindActorAsync(playerId, ct);
        ZlinkStreamAssert.Ensure(
            after.Error is null
            && after.OwnerNodeRid == pair.TargetOwnerNodeRid
            && after.OwnerNodeRid != before.OwnerNodeRid
            && after.ObjectGeneration == before.ObjectGeneration,
            "the probe actor moved to the target owner without changing its generation");

        var oneWayId = $"one-way-{Unique("b6")}";
        var requestId = $"request-{Unique("b6")}";
        var oneWayPayload = Encoding.UTF8.GetBytes("one-way-payload");
        var requestPayload = Encoding.UTF8.GetBytes("request-payload");
        var oneWay = probes.SendMessageFollowProbeAsync(playerId, oneWayId, oneWayPayload);
        var request = probes.RequestMessageFollowProbeAsync(
            playerId,
            requestId,
            requestPayload,
            ct);

        await oneWay;
        var reply = await request;
        ZlinkStreamAssert.Ensure(
            reply.ProbeId == requestId
            && reply.Payload.AsSpan().SequenceEqual(requestPayload),
            "the followed request preserves its payload and reply correlation");
        Console.WriteLine(
            $"message-follow-probe completed actor={playerId} one_way={oneWayId} request={requestId} "
            + $"generation={after.ObjectGeneration}");
    }

    private static (int X, int Y) ZoneCenter(string zoneId) => zoneId switch
    {
        ZoneIds.NorthWest => (25, 25),
        ZoneIds.NorthEast => (75, 25),
        ZoneIds.SouthWest => (25, 75),
        ZoneIds.SouthEast => (75, 75),
        _ => throw new ScenarioFailure($"Unknown ZoneId '{zoneId}'.")
    };

    private static async ValueTask MoveToAsync(
        GameClient player,
        int targetX,
        int targetY,
        CancellationToken cancellationToken)
    {
        while (player.Position.X != targetX || player.Position.Y != targetY)
        {
            // Move one axis at a time so one command cannot cross two boundaries.
            var nextX = player.Position.X != targetX
                ? player.Position.X + Math.Clamp(
                    targetX - player.Position.X,
                    -ZoneWorldSpec.MaxStepPerAxis,
                    ZoneWorldSpec.MaxStepPerAxis)
                : player.Position.X;
            var nextY = player.Position.X == targetX
                ? player.Position.Y + Math.Clamp(
                    targetY - player.Position.Y,
                    -ZoneWorldSpec.MaxStepPerAxis,
                    ZoneWorldSpec.MaxStepPerAxis)
                : player.Position.Y;
            var oldZone = ZoneWorldSpec.ZoneOf(player.Position.X, player.Position.Y);
            var newZone = ZoneWorldSpec.ZoneOf(nextX, nextY);

            if (!string.Equals(oldZone, newZone, StringComparison.Ordinal))
            {
                var changed = player.Connector.WaitFor<ZoneChangedNotify>()
                    .Where(message => message.Payload.PlayerId == player.PlayerId
                                      && message.Payload.ZoneId == newZone)
                    .Timeout(TimeSpan.FromSeconds(15))
                    .Async(cancellationToken);
                await player.MoveAsync(nextX, nextY);
                await changed;
            }
            else
            {
                var arrived = player.Connector.WaitFor<ZoneStateNotify>()
                    .Where(message => message.Payload.Players.Any(candidate =>
                        candidate.PlayerId == player.PlayerId
                        && candidate.X == nextX
                        && candidate.Y == nextY))
                    .Timeout(TimeSpan.FromSeconds(15))
                    .Async(cancellationToken);
                await player.MoveAsync(nextX, nextY);
                await arrived;
            }

            player.Position = (nextX, nextY);
        }
    }

    /// <summary>The Y boundary stays inside one node, so no relocation happens (§2.6).</summary>
    private static async ValueTask B3IntraNodeZoneChange(ClientOptions options, CancellationToken ct)
    {
        await using var player = await GameClient.ConnectAsync(options, Unique("b3"), ct);
        await player.JoinWorldAsync(ct);
        foreach (var step in player.PlanWalkWithinZone(25, 48))
        {
            var arrived = player.Connector.WaitFor<ZoneStateNotify>()
                .Where(message => message.Payload.Players.Any(p =>
                    p.PlayerId == player.PlayerId && p.X == step.X && p.Y == step.Y))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(ct);
            await player.MoveAsync(step.X, step.Y);
            await arrived;
            player.Position = step;
        }

        var changedWait = player.Connector.WaitFor<ZoneChangedNotify>()
            .Timeout(TimeSpan.FromSeconds(15))
            .Async(ct);
        await player.MoveAsync(25, 52);
        var changed = (await changedWait).Payload;
        player.Position = (25, 52);

        ZlinkStreamAssert.Ensure(object.Equals(ZoneIds.SouthWest, changed.ZoneId), "the Y boundary leads into zone-sw");
    }

    // --- Track C: observing the nodes ----------------------------------------

    private static async ValueTask C1WatchNodes(ClientOptions options, CancellationToken ct)
    {
        await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);
        var firstReady = (await ops.Connector.WaitFor<NodeStatusNotify>()
            .Where(message => message.Payload.Registered)
            .Timeout(TimeSpan.FromSeconds(40))
            .Async(ct)).Payload;
        ZlinkStreamAssert.Ensure(firstReady.Registered, "a runtime-observed node is registered");

        // Registration and connection are two different observations — the location runtime
        // reports one, the socket events the other — and the console has to show both (§8.1).
        var nodes = await ops.WatchNodesAsync(ct);
        ZlinkStreamAssert.Ensure(nodes.Nodes.Count(node => node.Registered && node.Connected) >= 2,
            "the console observes at least two ready ZoneNodes");
        foreach (var nodeId in nodes.Nodes.Where(node => node.Registered).Select(node => node.NodeId))
        {
            var node = nodes.Nodes.FirstOrDefault(n => n.NodeId == nodeId);
            ZlinkStreamAssert.Ensure(node is not null, $"the console knows about {nodeId}");
            ZlinkStreamAssert.Ensure(node!.Registered, $"{nodeId} is registered");
            ZlinkStreamAssert.Ensure(node.Connected, $"{nodeId} is connected");
        }
    }

    /// <summary>
    /// A zone spot's timer handler fails. The owning node receives the provider-neutral timer
    /// failure event and reports it to Ops (§8.1). The console sees it as an alert.
    /// </summary>
    private static async ValueTask C4SpotEventReported(ClientOptions options, CancellationToken ct)
    {
        await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);

        // Arm the wait before asking to watch: the alert may already have happened, and the
        // reply to WatchNodesReq is what replays it.
        var waiting = ops.Connector.WaitFor<NodeAlertNotify>()
            .Where(message => message.Payload.Kind == NodeAlertKinds.TimerHandlerFailed)
            .Timeout(TimeSpan.FromSeconds(40))
            .Async(ct);
        await ops.WatchNodesAsync(ct);
        var alert = (await waiting).Payload;

        // The fault is injected into one node only (the runner gives zone-node-1 the failing
        // zone), so the alert has to name that node. An alert from anywhere else would mean the
        // report carries no identity, which is the whole point of routing it through the node.
        ZlinkStreamAssert.Ensure(object.Equals(NodeAlertKinds.TimerHandlerFailed, alert.Kind), "the node reports its own spot event");
        var observed = await ops.WatchNodesAsync(ct);
        ZlinkStreamAssert.Ensure(observed.Nodes.Any(node => node.NodeId == alert.NodeId),
            "the alert names a node from the current runtime snapshot");
    }

    // --- Track D: announcing to every node -----------------------------------

    /// <summary>
    /// One announcement leaves Ops without a node list and comes out of every node's fanout
    /// subscriber. What the client can judge is the part that reaches it: an announcement it
    /// receives is never a duplicate. Whether both nodes' subscribers and every zone spot got
    /// it is judged from the server logs by the runner, because no client can see another
    /// node's subscriber. Delivery to a player is best-effort, so a player that misses one is
    /// not a failure (§8.2, §11 ZW-D1).
    /// </summary>
    private static async ValueTask D1AnnounceAllNodes(ClientOptions options, CancellationToken ct)
    {
        await using var player = await GameClient.ConnectAsync(options, Unique("d1"), ct);
        await player.JoinWorldAsync(ct);
        await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);

        // Best-effort delivery permits zero messages. A typed timeout is the only accepted
        // absence result; any transport, decode, or cancellation failure still fails the sample.
        var firstDelivery = player.Connector.WaitFor<WorldAnnounceNotify>()
            .Timeout(AnnouncementSettleTicks)
            .Async(ct);
        var published = await ops.AnnounceAsync("server maintenance starts in 10 minutes", ct);
        ZlinkStreamAssert.Ensure(published.AnnouncementId.Length > 0, "the publish is answered with an id");

        try
        {
            var received = (await firstDelivery).Payload;
            ZlinkStreamAssert.Ensure(
                received.AnnouncementId == published.AnnouncementId,
                "the player receives the announcement that Ops published");
            await player.Connector.ExpectNone<WorldAnnounceNotify>()
                .Within(AnnouncementSettleTicks)
                .Async(ct);
        }
        catch (TimeoutException)
        {
            // Zero deliveries are valid for this best-effort push.
        }
    }

    /// <summary>How long to keep listening after a publish before deciding what arrived. Long
    /// enough for the fanout to have reached both nodes and their zone spots.</summary>
    private static readonly TimeSpan AnnouncementSettleTicks = TimeSpan.FromSeconds(3);

    // --- Track E: maintenance and diagnostics --------------------------------

    /// <summary>
    /// Maintenance names one node. The other node keeps taking players, which is what makes
    /// this a targeted call rather than a broadcast (§8.4).
    /// </summary>
    private static async ValueTask E1TargetedMaintenance(ClientOptions options, CancellationToken ct)
    {
        await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);
        var observed = await ops.WatchNodesAsync(ct);
        var targetNodeId = observed.Nodes.Single(node => node.Zones.Contains(ZoneIds.NorthEast)).NodeId;
        foreach (var nodeId in observed.Nodes.Where(node => node.Registered).Select(node => node.NodeId))
        {
            var resetObserved = ops.Connector.WaitFor<NodeStatusNotify>()
                .Where(message => message.Payload.NodeId == nodeId && !message.Payload.Maintenance)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async(ct);
            var reset = await ops.SetMaintenanceAsync(nodeId, enabled: false, ct);
            if (reset.Error is null) await resetObserved;
            ZlinkStreamAssert.Ensure(!reset.Enabled, $"{nodeId} starts outside maintenance");
        }
        await using var player = await GameClient.ConnectAsync(options, Unique("e1"), ct);
        await player.JoinWorldAsync(ct);
        foreach (var step in player.PlanWalkWithinZone(48, 25))
        {
            var arrived = player.Connector.WaitFor<ZoneStateNotify>()
                .Where(message => message.Payload.Players.Any(p =>
                    p.PlayerId == player.PlayerId && p.X == step.X && p.Y == step.Y))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(ct);
            await player.MoveAsync(step.X, step.Y);
            await arrived;
            player.Position = step;
        }

        var enabledObserved = ops.Connector.WaitFor<NodeStatusNotify>()
            .Where(message => message.Payload.NodeId == targetNodeId && message.Payload.Maintenance)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        var applied = await ops.SetMaintenanceAsync(targetNodeId, enabled: true, ct);
        if (applied.Error is null) await enabledObserved;
        try
        {
            ZlinkStreamAssert.Ensure(object.Equals(targetNodeId, applied.NodeId), "maintenance applies to the observed owner");
            ZlinkStreamAssert.Ensure(applied.Zones.Contains(ZoneIds.NorthEast),
                "maintenance response reports the target zone's current owner");

            // Entering zone-ne is refused; the coordinate does not move.
            var rejectedWait = player.Connector.WaitFor<MoveRejectedNotify>()
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(ct);
            await player.MoveAsync(52, 25);
            var rejected = (await rejectedWait).Payload;
            ZlinkStreamAssert.Ensure(object.Equals(MoveRejectReasons.ZoneMaintenance, rejected.Reason), "the maintained node refuses arrivals");
            ZlinkStreamAssert.Ensure(object.Equals(48, rejected.X), "a refused move leaves the coordinate untouched");

            // If the same current owner also hosts zone-se, its admission must be rejected too.
            foreach (var step in player.PlanWalkWithinZone(48, 48))
            {
                var arrived = player.Connector.WaitFor<ZoneStateNotify>()
                    .Where(message => message.Payload.Players.Any(p =>
                        p.PlayerId == player.PlayerId && p.X == step.X && p.Y == step.Y))
                    .Timeout(TimeSpan.FromSeconds(15))
                    .Async(ct);
                await player.MoveAsync(step.X, step.Y);
                await arrived;
                player.Position = step;
            }
            var southChanged = player.Connector.WaitFor<ZoneChangedNotify>()
                .Where(message => message.Payload.PlayerId == player.PlayerId)
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(ct);
            await player.MoveAsync(48, 52);
            await southChanged;
            player.Position = (48, 52);
            foreach (var step in player.PlanWalkWithinZone(48, 55))
            {
                var arrived = player.Connector.WaitFor<ZoneStateNotify>()
                    .Where(message => message.Payload.Players.Any(p =>
                        p.PlayerId == player.PlayerId && p.X == step.X && p.Y == step.Y))
                    .Timeout(TimeSpan.FromSeconds(15))
                    .Async(ct);
                await player.MoveAsync(step.X, step.Y);
                await arrived;
                player.Position = step;
            }
            if (applied.Zones.Contains(ZoneIds.SouthEast))
            {
                var refusedSouthWait = player.Connector.WaitFor<MoveRejectedNotify>()
                    .Timeout(TimeSpan.FromSeconds(15))
                    .Async(ct);
                await player.MoveAsync(52, 55);
                var refusedSouth = (await refusedSouthWait).Payload;
                ZlinkStreamAssert.Ensure(object.Equals(MoveRejectReasons.ZoneMaintenance, refusedSouth.Reason),
                    "another zone on the observed owner refuses arrivals too");
                ZlinkStreamAssert.Ensure(object.Equals(48, refusedSouth.X), "a refused move leaves the coordinate untouched");
            }

            // zone-node-1 is untouched: a move inside it still works.
            var westMove = player.Connector.WaitFor<ZoneStateNotify>()
                .Where(message => message.Payload.Players.Any(p =>
                    p.PlayerId == player.PlayerId && p.X == 45 && p.Y == 55))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(ct);
            await player.MoveAsync(45, 55);
            await westMove;
            player.Position = (45, 55);
        }
        finally
        {
            var disabledObserved = ops.Connector.WaitFor<NodeStatusNotify>()
                .Where(message => message.Payload.NodeId == targetNodeId && !message.Payload.Maintenance)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async(ct);
            var disabled = await ops.SetMaintenanceAsync(targetNodeId, enabled: false, ct);
            if (disabled.Error is null) await disabledObserved;
        }
    }

    /// <summary>Maintenance stops arrivals, not the players already there (§2.3).</summary>
    private static async ValueTask E2MaintainedNodeKeepsMoving(ClientOptions options, CancellationToken ct)
    {
        await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);
        var observed = await ops.WatchNodesAsync(ct);
        var targetNodeId = observed.Nodes.Single(node => node.Zones.Contains(ZoneIds.NorthWest)).NodeId;
        foreach (var nodeId in observed.Nodes.Where(node => node.Registered).Select(node => node.NodeId))
        {
            var resetObserved = ops.Connector.WaitFor<NodeStatusNotify>()
                .Where(message => message.Payload.NodeId == nodeId && !message.Payload.Maintenance)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async(ct);
            var reset = await ops.SetMaintenanceAsync(nodeId, enabled: false, ct);
            if (reset.Error is null) await resetObserved;
            ZlinkStreamAssert.Ensure(!reset.Enabled, $"{nodeId} starts outside maintenance");
        }
        await using var player = await GameClient.ConnectAsync(options, Unique("e2"), ct);
        var initialState = player.Connector.WaitFor<ZoneStateNotify>()
            .Timeout(TimeSpan.FromSeconds(15))
            .Async(ct);
        await player.JoinWorldAsync(ct);
        await initialState;

        var enabledObserved = ops.Connector.WaitFor<NodeStatusNotify>()
            .Where(message => message.Payload.NodeId == targetNodeId && message.Payload.Maintenance)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        var enabled = await ops.SetMaintenanceAsync(targetNodeId, enabled: true, ct);
        if (enabled.Error is null) await enabledObserved;
        try
        {
            // Moving inside the maintained node's zone.
            var moved = player.Connector.WaitFor<ZoneStateNotify>()
                .Where(message => message.Payload.Players.Any(p =>
                    p.PlayerId == player.PlayerId && p.X == 30 && p.Y == 30))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(ct);
            await player.MoveAsync(30, 30);
            await moved;
            player.Position = (30, 30);

            // And across a zone boundary inside the same node.
            foreach (var step in player.PlanWalkWithinZone(30, 48))
            {
                var arrived = player.Connector.WaitFor<ZoneStateNotify>()
                    .Where(message => message.Payload.Players.Any(p =>
                        p.PlayerId == player.PlayerId && p.X == step.X && p.Y == step.Y))
                    .Timeout(TimeSpan.FromSeconds(15))
                    .Async(ct);
                await player.MoveAsync(step.X, step.Y);
                await arrived;
                player.Position = step;
            }
            var changedWait = player.Connector.WaitFor<ZoneChangedNotify>()
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(ct);
            await player.MoveAsync(30, 52);
            var changed = (await changedWait).Payload;
            player.Position = (30, 52);
            ZlinkStreamAssert.Ensure(object.Equals(ZoneIds.SouthWest, changed.ZoneId), "the logical zone change completed");
        }
        finally
        {
            var disabledObserved = ops.Connector.WaitFor<NodeStatusNotify>()
                .Where(message => message.Payload.NodeId == targetNodeId && !message.Payload.Maintenance)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async(ct);
            var disabled = await ops.SetMaintenanceAsync(targetNodeId, enabled: false, ct);
            if (disabled.Error is null) await disabledObserved;
        }
    }

    /// <summary>Leaving a maintained node for a healthy one is allowed (§2.3).</summary>
    private static async ValueTask E3LeavingMaintainedNode(ClientOptions options, CancellationToken ct)
    {
        await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);
        var observed = await ops.WatchNodesAsync(ct);
        var sourceNodeId = observed.Nodes.Single(node => node.Zones.Contains(ZoneIds.NorthWest)).NodeId;
        foreach (var nodeId in observed.Nodes.Where(node => node.Registered).Select(node => node.NodeId))
        {
            var resetObserved = ops.Connector.WaitFor<NodeStatusNotify>()
                .Where(message => message.Payload.NodeId == nodeId && !message.Payload.Maintenance)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async(ct);
            var reset = await ops.SetMaintenanceAsync(nodeId, enabled: false, ct);
            if (reset.Error is null) await resetObserved;
            ZlinkStreamAssert.Ensure(!reset.Enabled, $"{nodeId} starts outside maintenance");
        }
        await using var player = await GameClient.ConnectAsync(options, Unique("e3"), ct);
        await player.JoinWorldAsync(ct);
        foreach (var step in player.PlanWalkWithinZone(48, 25))
        {
            var arrived = player.Connector.WaitFor<ZoneStateNotify>()
                .Where(message => message.Payload.Players.Any(p =>
                    p.PlayerId == player.PlayerId && p.X == step.X && p.Y == step.Y))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(ct);
            await player.MoveAsync(step.X, step.Y);
            await arrived;
            player.Position = step;
        }

        var enabledObserved = ops.Connector.WaitFor<NodeStatusNotify>()
            .Where(message => message.Payload.NodeId == sourceNodeId && message.Payload.Maintenance)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        var enabled = await ops.SetMaintenanceAsync(sourceNodeId, enabled: true, ct);
        if (enabled.Error is null) await enabledObserved;
        try
        {
            var changedWait = player.Connector.WaitFor<ZoneChangedNotify>()
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(ct);
            await player.MoveAsync(52, 25);
            var changed = (await changedWait).Payload;
            player.Position = (52, 25);
            ZlinkStreamAssert.Ensure(object.Equals(ZoneIds.NorthEast, changed.ZoneId), "the logical zone change completed");
        }
        finally
        {
            var disabledObserved = ops.Connector.WaitFor<NodeStatusNotify>()
                .Where(message => message.Payload.NodeId == sourceNodeId && !message.Payload.Maintenance)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async(ct);
            var disabled = await ops.SetMaintenanceAsync(sourceNodeId, enabled: false, ct);
            if (disabled.Error is null) await disabledObserved;
        }
    }

    private static async ValueTask E4NodeDiagnostics(ClientOptions options, CancellationToken ct)
    {
        await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);
        var observed = await ops.WatchNodesAsync(ct);
        var targetNodeId = observed.Nodes.First(node => node.Registered).NodeId;
        var diagnostics = await ops.DiagnoseAsync(targetNodeId, ct);

        ZlinkStreamAssert.Ensure(object.Equals(targetNodeId, diagnostics.NodeId),
            "diagnostics come back from the runtime-observed node");
        ZlinkStreamAssert.Ensure(diagnostics.PlayerCount >= 0, "the node reports how many players it holds");
    }

    private static async ValueTask G2ReverseStartedNodeOperations(ClientOptions options, CancellationToken ct)
    {
        await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);
        var observed = await ops.WatchNodesAsync(ct);
        var targetNodeId = observed.Nodes.First(node => node.Registered && node.Connected).NodeId;
        foreach (var nodeId in observed.Nodes.Where(node => node.Registered).Select(node => node.NodeId))
        {
            var resetObserved = ops.Connector.WaitFor<NodeStatusNotify>()
                .Where(message => message.Payload.NodeId == nodeId && !message.Payload.Maintenance)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async(ct);
            var reset = await ops.SetMaintenanceAsync(nodeId, enabled: false, ct);
            if (reset.Error is null) await resetObserved;
            ZlinkStreamAssert.Ensure(!reset.Enabled, $"{nodeId} starts outside maintenance");
        }

        var enabledObserved = ops.Connector.WaitFor<NodeStatusNotify>()
            .Where(message => message.Payload.NodeId == targetNodeId && message.Payload.Maintenance)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        var applied = await ops.SetMaintenanceAsync(targetNodeId, enabled: true, ct);
        if (applied.Error is null) await enabledObserved;
        try
        {
            ZlinkStreamAssert.Ensure(applied.Error is null, "reverse-started zone-node-2 accepted maintenance");
            ZlinkStreamAssert.Ensure(object.Equals(targetNodeId, applied.NodeId),
                "reverse-started node keeps the runtime-observed application identity");

            var diagnostics = await ops.DiagnoseAsync(targetNodeId, ct);
            ZlinkStreamAssert.Ensure(diagnostics.Error is null, "reverse-started node answered diagnostics");
            ZlinkStreamAssert.Ensure(object.Equals(targetNodeId, diagnostics.NodeId),
                "diagnostics preserve the application NodeId instead of the allocated routing id");
        }
        finally
        {
            var disabledObserved = ops.Connector.WaitFor<NodeStatusNotify>()
                .Where(message => message.Payload.NodeId == targetNodeId && !message.Payload.Maintenance)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async(ct);
            var disabled = await ops.SetMaintenanceAsync(targetNodeId, enabled: false, ct);
            if (disabled.Error is null) await disabledObserved;
        }
    }

    /// <summary>A brand-new entry into a maintained node is refused (§2.3).</summary>
    private static async ValueTask E6MaintenanceBlocksEntry(ClientOptions options, CancellationToken ct)
    {
        await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);
        var observed = await ops.WatchNodesAsync(ct);
        var spawnOwnerNodeId = observed.Nodes.Single(node => node.Zones.Contains(ZoneIds.NorthWest)).NodeId;
        foreach (var nodeId in observed.Nodes.Where(node => node.Registered).Select(node => node.NodeId))
        {
            var resetObserved = ops.Connector.WaitFor<NodeStatusNotify>()
                .Where(message => message.Payload.NodeId == nodeId && !message.Payload.Maintenance)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async(ct);
            var reset = await ops.SetMaintenanceAsync(nodeId, enabled: false, ct);
            if (reset.Error is null) await resetObserved;
            ZlinkStreamAssert.Ensure(!reset.Enabled, $"{nodeId} starts outside maintenance");
        }
        var enabledObserved = ops.Connector.WaitFor<NodeStatusNotify>()
            .Where(message => message.Payload.NodeId == spawnOwnerNodeId && message.Payload.Maintenance)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        var enabled = await ops.SetMaintenanceAsync(spawnOwnerNodeId, enabled: true, ct);
        if (enabled.Error is null) await enabledObserved;
        try
        {
            await using var player = await GameClient.ConnectAsync(options, Unique("e6"), ct);
            var join = await player.JoinWorldAsync(ct);
            ZlinkStreamAssert.Ensure(object.Equals(MoveRejectReasons.ZoneMaintenance, join.Error), "the spawn node refuses a new entry");
        }
        finally
        {
            var disabledObserved = ops.Connector.WaitFor<NodeStatusNotify>()
                .Where(message => message.Payload.NodeId == spawnOwnerNodeId && !message.Payload.Maintenance)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async(ct);
            var disabled = await ops.SetMaintenanceAsync(spawnOwnerNodeId, enabled: false, ct);
            if (disabled.Error is null) await disabledObserved;
        }
    }

    /// <summary>
    /// A node that goes away is not there to answer a request, so the console learns about it
    /// from the location runtime rather than by polling (§8.1). The runner stops zone-node-2
    /// while this scenario is watching.
    /// </summary>
    private static async ValueTask C2NodeShutdown(ClientOptions options, CancellationToken ct)
    {
        await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);
        var nodes = await ops.WatchNodesAsync(ct);
        var targetNodeId = nodes.Nodes
            .Where(node => node.Registered)
            .OrderBy(node => node.NodeId, StringComparer.Ordinal)
            .Last().NodeId;

        // The node has to be registered before its going away means anything. "Not registered"
        // is also the state of a node the console has never heard of, and waiting for that
        // would pass before the runner had done anything.
        ZlinkStreamAssert.Ensure(
            nodes.Nodes.Any(n => n.NodeId == targetNodeId && n.Registered),
            "the runtime-selected node is registered before the runner stops it");
        var goneWait = ops.Connector.WaitFor<NodeStatusNotify>()
            .Where(message => message.Payload.NodeId == targetNodeId && !message.Payload.Registered)
            .Timeout(TimeSpan.FromSeconds(40))
            .Async(ct);
        Console.WriteLine("scenario ZW-C2 armed");

        var gone = (await goneWait).Payload;

        ZlinkStreamAssert.Ensure(!gone.Registered, "a stopped node stops being registered");
    }

    /// <summary>
    /// The node's connection to Ops drops. This is a socket event, not a location one: the node
    /// may still be registered while its link is gone (§8.1). The runner stops zone-node-2 while
    /// this scenario is watching.
    /// </summary>
    private static async ValueTask C3NodeDisconnected(ClientOptions options, CancellationToken ct)
    {
        await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);
        var nodes = await ops.WatchNodesAsync(ct);
        var targetNodeId = nodes.Nodes
            .Where(node => node.Connected)
            .OrderBy(node => node.NodeId, StringComparer.Ordinal)
            .Last().NodeId;

        // As in ZW-C2: a link that was never up cannot drop, so the connected state has to be
        // established before the drop is observed — otherwise the default state passes the test.
        ZlinkStreamAssert.Ensure(
            nodes.Nodes.Any(n => n.NodeId == targetNodeId && n.Connected),
            "the runtime-selected node's link is up before the runner stops it");
        var droppedWait = ops.Connector.WaitFor<NodeStatusNotify>()
            .Where(message => message.Payload.NodeId == targetNodeId && !message.Payload.Connected)
            .Timeout(TimeSpan.FromSeconds(40))
            .Async(ct);
        Console.WriteLine("scenario ZW-C3 armed");

        var dropped = (await droppedWait).Payload;

        ZlinkStreamAssert.Ensure(!dropped.Connected, "a node whose link drops is reported as disconnected");
    }

    /// <summary>
    /// The adjacent zone's node stops. Its border snapshots stop arriving, and after three
    /// ticks the players it was reporting are dropped rather than left frozen on screen (§2.4).
    /// The runner stops zone-node-2 while this scenario is watching.
    /// </summary>
    private static async ValueTask B4BorderSnapshotExpiry(ClientOptions options, CancellationToken ct)
    {
        var westId = Unique("b4-w");
        var eastId = Unique("b4-e");

        await using var west = await GameClient.ConnectAsync(options, westId, ct);
        await west.JoinWorldAsync(ct);
        await using var east = await GameClient.ConnectAsync(options, eastId, ct);
        await east.JoinWorldAsync(ct);

        foreach (var step in west.PlanWalkWithinZone(45, 25))
        {
            var arrived = west.Connector.WaitFor<ZoneStateNotify>()
                .Where(message => message.Payload.Players.Any(p =>
                    p.PlayerId == west.PlayerId && p.X == step.X && p.Y == step.Y))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(ct);
            await west.MoveAsync(step.X, step.Y);
            await arrived;
            west.Position = step;
        }
        Console.WriteLine("scenario ZW-B4 checkpoint=west-in-border-band");

        // Prepare the cross-border observation only after the western setup walk. The east
        // relocation below is the trigger; unrelated setup must not consume its timeout budget.
        var eastVisible = west.Connector.WaitFor<ZoneStateNotify>()
            .Where(message => message.Payload.Players.Any(p =>
                p.PlayerId == eastId && p.ZoneId == ZoneIds.NorthEast))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async(ct);

        // The eastern player stands in zone-ne's band, so the western one can see it.
        foreach (var step in east.PlanWalkWithinZone(48, 25))
        {
            var arrived = east.Connector.WaitFor<ZoneStateNotify>()
                .Where(message => message.Payload.Players.Any(p =>
                    p.PlayerId == east.PlayerId && p.X == step.X && p.Y == step.Y))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(ct);
            await east.MoveAsync(step.X, step.Y);
            await arrived;
            east.Position = step;
        }
        var eastChanged = east.Connector.WaitFor<ZoneChangedNotify>()
            .Where(message => message.Payload.PlayerId == east.PlayerId)
            .Timeout(TimeSpan.FromSeconds(15))
            .Async(ct);
        await east.MoveAsync(52, 25);
        await eastChanged;
        east.Position = (52, 25);
        Console.WriteLine("scenario ZW-B4 checkpoint=east-in-zone-ne");

        // Wait until east is visible *as a zone-ne player*, not merely visible. Right after the
        // relocation there is a tick or two where east is still in zone-nw's own copy — the spot
        // holds a copy of a coordinate the actor owns (§2.1), and the copy lags the actor by a
        // turn. "Visible" is satisfied by that stale copy, and a test that starts from there is
        // watching the wrong player: it would then see the copy disappear and call it expiry.
        // The snapshot from the *other node* is the thing whose expiry this scenario is about.
        await eastVisible;

        var expiredWait = west.Connector.WaitFor<ZoneStateNotify>()
            .Where(message => message.Payload.ZoneId == ZoneIds.NorthWest
                              && message.Payload.Players.All(p =>
                                  p.PlayerId != eastId && p.ZoneId != ZoneIds.NorthEast))
            .Timeout(TimeSpan.FromSeconds(60))
            .Async(ct);
        Console.WriteLine("scenario ZW-B4 armed");

        // zone-node-2 goes away — the runner takes it away, and it waits long enough first for
        // the cross-node walk above to have finished. The wait here has to outlast that, or it
        // gives up before the thing it is watching for has been arranged.
        //
        // Expiry has to be waited for as *both* facts at once: east is gone, and the zone it was
        // in is gone with it — a snapshot is replaced whole, never merged (§2.4). Neither alone
        // is sound. "east is gone" is briefly true of the wrong thing: right after the relocation
        // the source zone still holds its copy of east (§2.1 — the spot's copy lags the actor by
        // a turn), so east is on screen as a zone-nw player while zone-ne has yet to report it.
        // And "zone-ne is gone" is true before zone-ne's first snapshot ever lands. Requiring
        // both leaves only the state this scenario is about.
        //
        // Only zone-ne goes. zone-nw's other neighbour, zone-sw, is on zone-node-1 and is still
        // publishing, so its band players legitimately stay in view (§4.1).
        var expired = (await expiredWait).Payload;

        ZlinkStreamAssert.Ensure(
            expired.Players.All(p => p.PlayerId != eastId),
            "a stopped node's players expire out of the neighbour's view");
    }

    /// <summary>Puts zone-node-2 into maintenance so the runner can restart it (§8.4, ZW-E5).</summary>
    private static async ValueTask E5Arm(ClientOptions options, CancellationToken ct)
    {
        await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);
        var observed = await ops.WatchNodesAsync(ct);
        var targetNodeId = observed.Nodes
            .Where(node => node.Registered && node.Connected)
            .OrderBy(node => node.NodeId, StringComparer.Ordinal)
            .Last().NodeId;
        var enabledObserved = ops.Connector.WaitFor<NodeStatusNotify>()
            .Where(message => message.Payload.NodeId == targetNodeId && message.Payload.Maintenance)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        var applied = await ops.SetMaintenanceAsync(targetNodeId, enabled: true, ct);
        if (applied.Error is null) await enabledObserved;
        // The desired state is committed before Ops tries the owner-consistent channel. A
        // restarted node can be between transport connections here; NodeUnavailable is still
        // a successful setup for this scenario because the restart below must read the stored
        // value. E5 then proves that it did.
        ZlinkStreamAssert.Ensure(
            applied.NodeId == targetNodeId,
            "maintenance targets the runtime-selected node");
        ZlinkStreamAssert.Ensure(applied.Enabled, "maintenance desired state is enabled before the restart");
        ZlinkStreamAssert.Ensure(
            applied.Error is null or ZoneWorldErrors.NodeUnavailable,
            "maintenance records desired state even while its target reconnects");
    }

    /// <summary>
    /// Maintenance is desired state, not a message: the node reads it back from the store when
    /// it starts, so a restart does not quietly reopen a node the operator closed (§8.4).
    /// The runner restarts zone-node-2 between E5-arm and this scenario.
    /// </summary>
    private static async ValueTask E5MaintenanceRestored(ClientOptions options, CancellationToken ct)
    {
        await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);
        var observed = await ops.WatchNodesAsync(ct);
        var targetNodeId = observed.Nodes
            .Where(node => node.Registered)
            .OrderBy(node => node.NodeId, StringComparer.Ordinal)
            .Last().NodeId;
        try
        {
            var diagnostics = await ops.DiagnoseAsync(targetNodeId, ct);
            ZlinkStreamAssert.Ensure(diagnostics.Maintenance, "the restarted node came up still under maintenance");
        }
        finally
        {
            var disabledObserved = ops.Connector.WaitFor<NodeStatusNotify>()
                .Where(message => message.Payload.NodeId == targetNodeId && !message.Payload.Maintenance)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async(ct);
            var disabled = await ops.SetMaintenanceAsync(targetNodeId, enabled: false, ct);
            if (disabled.Error is null) await disabledObserved;
        }
    }

    // --- Track F: bots --------------------------------------------------------

    /// <summary>
    /// The world runs bots with no client attached, and they move on their own (§2.7). A client
    /// sees the bots of its own zone plus whichever are standing in an adjacent zone's border
    /// band — never the whole population of eight, because a client only ever receives one
    /// zone's view. The count of eight is judged by the runner from the server logs; what is
    /// asserted here is what a client can actually see.
    /// </summary>
    private static async ValueTask F1BotsPresent(ClientOptions options, CancellationToken ct)
    {
        await using var player = await GameClient.ConnectAsync(options, Unique("f1"), ct);
        await player.JoinWorldAsync(ct);

        var first = (await player.Connector.WaitFor<ZoneStateNotify>()
            .Where(message => message.Payload.Players.Count(p => p.IsBot) >= ZoneWorldSpec.BotsPerZone)
            .Timeout(TimeSpan.FromSeconds(15))
            .Async(ct)).Payload;
        var bots = first.Players.Where(p => p.IsBot).ToDictionary(p => p.PlayerId, StringComparer.Ordinal);
        ZlinkStreamAssert.Ensure(
            bots.Count >= ZoneWorldSpec.BotsPerZone,
            "the spawn zone's own bots are in the world with no client attached");

        // Both of the zone's bots move: one patrols X, the other Y, and each has to be walking.
        var stillStanding = new HashSet<string>(bots.Keys, StringComparer.Ordinal);
        while (stillStanding.Count > 0)
        {
            var state = (await player.Connector.WaitFor<ZoneStateNotify>()
                .Where(message => message.Payload.Players.Any(p => p.PlayerId == player.PlayerId))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(ct)).Payload;
            foreach (var bot in state.Players.Where(p => p.IsBot))
            {
                if (!bots.TryGetValue(bot.PlayerId, out var before)) continue;
                if (bot.X != before.X || bot.Y != before.Y) stillStanding.Remove(bot.PlayerId);
            }
        }
    }

    /// <summary>
    /// A bot has no bound session, so nothing is ever pushed to it. What the canonical asks for
    /// is the *absence* of a push attempt (§11 ZW-F3), and no client can observe an absence on
    /// another actor — so the runner judges it from the server logs, where a push to an unbound
    /// actor would leave an error. This scenario supplies the traffic that would provoke one:
    /// it puts bots in the world alongside a human, publishes an announcement the zone spots
    /// have to deliver, and drives a tick loop, all of which walk the push path.
    /// </summary>
    private static async ValueTask F3NoPushToBots(ClientOptions options, CancellationToken ct)
    {
        await using var player = await GameClient.ConnectAsync(options, Unique("f3"), ct);
        await player.JoinWorldAsync(ct);
        await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);

        await ops.AnnounceAsync("bots receive nothing", ct);

        // A rejected move is the other push (§2.2), and a bot must not be sent one either.
        var rejected = player.Connector.WaitFor<MoveRejectedNotify>()
            .Timeout(TimeSpan.FromSeconds(15))
            .Async(ct);
        await player.MoveAsync(-40, player.Position.Y);
        await rejected;

        var state = (await player.Connector.WaitFor<ZoneStateNotify>()
            .Where(message => message.Payload.Players.Any(p => p.PlayerId == player.PlayerId))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async(ct)).Payload;
        ZlinkStreamAssert.Ensure(state.Players.Any(p => p.IsBot), "the bots are in the world alongside the human");
    }

    /// <summary>
    /// A rejected move turns a bot around (§2.7). The destination node is selected from the
    /// bot that is actually at an X boundary. This keeps the assertion valid after earlier
    /// relocation scenarios have changed which side owns each fixed bot.
    /// </summary>
    private static async ValueTask F4BotReversesOnRejection(ClientOptions options, CancellationToken ct)
    {
        await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);
        var observed = await ops.WatchNodesAsync(ct);
        foreach (var nodeId in observed.Nodes.Where(node => node.Registered).Select(node => node.NodeId))
        {
            var resetObserved = ops.Connector.WaitFor<NodeStatusNotify>()
                .Where(message => message.Payload.NodeId == nodeId && !message.Payload.Maintenance)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async(ct);
            var reset = await ops.SetMaintenanceAsync(nodeId, enabled: false, ct);
            if (reset.Error is null) await resetObserved;
            ZlinkStreamAssert.Ensure(!reset.Enabled, $"{nodeId} starts outside maintenance");
        }
        await using var player = await GameClient.ConnectAsync(options, Unique("f4"), ct);
        await player.JoinWorldAsync(ct);

        var boundary = (await player.Connector.WaitFor<ZoneStateNotify>()
            .Where(message => FindAboutToCross(message.Payload) is not null)
            .Timeout(TimeSpan.FromSeconds(30))
            .Async(ct)).Payload;
        var botAtBoundary = FindAboutToCross(boundary)
                            ?? throw new ScenarioFailure("the boundary observation lost its bot");
        var sourceZone = botAtBoundary.ZoneId;
        var targetZone = string.Equals(sourceZone, ZoneIds.NorthWest, StringComparison.Ordinal)
            ? ZoneIds.NorthEast
            : ZoneIds.NorthWest;
        var targetNodeId = observed.Nodes
            .Single(node => node.Zones.Contains(targetZone, StringComparer.Ordinal))
            .NodeId;

        var enabledObserved = ops.Connector.WaitFor<NodeStatusNotify>()
            .Where(message => message.Payload.NodeId == targetNodeId && message.Payload.Maintenance)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        var enabled = await ops.SetMaintenanceAsync(targetNodeId, enabled: true, ct);
        if (enabled.Error is null) await enabledObserved;
        try
        {
            // The next X step enters the maintained destination. A rejected entry reverses
            // the direction, so the same bot must move away from the boundary afterwards.
            var botId = botAtBoundary.PlayerId;
            var peak = botAtBoundary.X;

            // It is refused at the boundary and walks back the way it came.
            var reversed = (await player.Connector.WaitFor<ZoneStateNotify>()
                .Where(message =>
                {
                    var x = BotX(message.Payload, botId);
                    if (x is null) return false;
                    if (string.Equals(sourceZone, ZoneIds.NorthWest, StringComparison.Ordinal))
                    {
                        if (x > peak) peak = x.Value;
                        return x < peak;
                    }

                    if (x < peak) peak = x.Value;
                    return x > peak;
                })
                .Timeout(TimeSpan.FromSeconds(30))
                .Async(ct)).Payload;

            ZlinkStreamAssert.Ensure(
                string.Equals(sourceZone, ZoneIds.NorthWest, StringComparison.Ordinal)
                    ? BotX(reversed, botId) < peak
                    : BotX(reversed, botId) > peak,
                "a bot refused entry to a node under maintenance turns around");
        }
        finally
        {
            var disabledObserved = ops.Connector.WaitFor<NodeStatusNotify>()
                .Where(message => message.Payload.NodeId == targetNodeId && !message.Payload.Maintenance)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async(ct);
            var disabled = await ops.SetMaintenanceAsync(targetNodeId, enabled: false, ct);
            if (disabled.Error is null) await disabledObserved;
        }
    }

    private static int? BotX(ZoneStateNotify state, string botId) =>
        state.Players.FirstOrDefault(p => p.PlayerId == botId)?.X;

    /// <summary>
    /// An X-patrolling bot (its id ends in "-x", §2.7) whose next step crosses either side of
    /// the vertical boundary. The destination node is maintained after this observation.
    /// </summary>
    private static PlayerView? FindAboutToCross(ZoneStateNotify state) =>
        state.Players.FirstOrDefault(p =>
            p.IsBot
            && p.PlayerId.EndsWith("-x", StringComparison.Ordinal)
            && ((p.ZoneId == ZoneIds.NorthWest
                 && p.X + ZoneWorldSpec.BotStep >= ZoneWorldSpec.ZoneSplit)
                || (p.ZoneId == ZoneIds.NorthEast
                    && p.X - ZoneWorldSpec.BotStep < ZoneWorldSpec.ZoneSplit)));

    private static string Unique(string prefix) =>
        $"{prefix}-{Guid.NewGuid().ToString("n")[..6]}";
}
