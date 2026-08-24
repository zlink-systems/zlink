using Systems.Zlink.Stream.Connector.Contracts;
using System.Text;
using ZoneWorld.Server.Configuration;
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
            ["ZW-A1"] = A1DeferredAdmissionEntry,
            ["ZW-A2"] = A2SameZoneMove,
            ["ZW-A3"] = A3RejectionOrder,
            ["ZW-A4"] = A4SameZonePlayers,
            ["ZW-A5"] = A5Utf8OrderingAndOwnZonePrecedence,
            ["ZW-B1"] = B1BorderSync,
            ["ZW-B2"] = B2CrossNodeRelocation,
            ["ZW-B3"] = B3ActorGenerationPreserved,
            ["ZW-B5"] = B5MessageFollowOneWay,
            ["ZW-B6"] = B6MessageFollowRequest,
            ["ZW-B7"] = B7RelocationRoundTrip,
            ["ZW-C1"] = C1WatchNodes,
            ["ZW-C4"] = C4SpotEventReported,
            ["ZW-D1"] = D1AnnounceAllNodes,
            ["ZW-E1"] = E1TargetedMaintenance,
            ["ZW-E2"] = E2MaintenanceBlocksNewEntry,
            ["ZW-E3"] = E3SameZoneMoveAllowed,
            ["ZW-E4"] = E4SameNodeDifferentZoneRejected,
            ["ZW-E6"] = E6NodeDiagnostics,
            ["ZW-F1"] = F1BotsPresent,
            ["ZW-F3"] = F4BotReversesOnRejection,
            ["ZW-F4"] = F3NoPushToBots
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
            ["ZW-B8"] = B8SessionRouteSealTimeoutReconnect,
            ["ZW-C2"] = C2NodeDisconnected,
            ["ZW-C3"] = C3ReportTtlExpired,
            ["ZW-E5-arm"] = E5Arm,
            ["ZW-E5"] = E5MaintenanceRestored,
            ["ZW-G2"] = G2ReverseStartedNodeOperations,
            ["ZW-G4"] = G4CrashEndsCurrentOperationUnavailable,
            ["ZW-G4-fresh"] = G4FreshReplacementAcceptsObject
        };

    // Cross-node observation includes actor relocation and the first target-zone snapshot.
    // This is a harness budget, not a public latency guarantee.
    private static readonly TimeSpan CrossNodeObservationTimeout = TimeSpan.FromSeconds(30);

    // Bot observation includes the first snapshot after world admission. This is a harness
    // budget for a busy full-sample run, not a public latency guarantee.
    private static readonly TimeSpan BotObservationTimeout = TimeSpan.FromSeconds(30);

    // Border observation includes several movement snapshots before the cross-zone assertion.
    // This is a harness budget for a busy full-sample run, not a public latency guarantee.
    private static readonly TimeSpan BorderObservationTimeout = TimeSpan.FromSeconds(30);

    // --- Track A: entry and movement ----------------------------------------

    private static async ValueTask A1DeferredAdmissionEntry(ClientOptions options, CancellationToken ct)
    {
        await using var player = await GameClient.ConnectAsync(options, Unique("a1"), ct);
        var join = await player.JoinWorldAsync(ct);

        ZlinkStreamAssert.Ensure(object.Equals(ZoneIds.NorthWest, join.ZoneId), "a new player spawns in zone-nw");
        ZlinkStreamAssert.Ensure(object.Equals(ZoneWorldSpec.SpawnX, join.X), "the spawn coordinate is fixed");
        ZlinkStreamAssert.Ensure(object.Equals(ZoneWorldSpec.SpawnY, join.Y), "the spawn coordinate is fixed");

        ZlinkStreamAssert.Ensure(join.Error is null, "target zone admission completed before JoinWorldRes");
    }

    /// <summary>
    /// A move can break several rules at once, and every language must name the same one.
    /// This target is out of range *and* further than the step cap; the fixed order says
    /// OutOfRange wins (§2.2).
    /// </summary>
    private static async ValueTask A2SameZoneMove(ClientOptions options, CancellationToken ct)
    {
        await using var player = await GameClient.ConnectAsync(options, Unique("a2"), ct);
        var join = await player.JoinWorldAsync(ct);

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

    private static async ValueTask A3RejectionOrder(ClientOptions options, CancellationToken ct)
    {
        await using var player = await GameClient.ConnectAsync(options, Unique("a3"), ct);
        await player.JoinWorldAsync(ct);

        await ExpectMoveRejectedAsync(player, -1, 25, MoveRejectReasons.OutOfRange, ct);
        await ExpectMoveRejectedAsync(player, 31, 25, MoveRejectReasons.TooFar, ct);
        await MoveToAsync(player, 49, 49, ct);
        await ExpectMoveRejectedAsync(player, 50, 50, MoveRejectReasons.DiagonalCrossing, ct);

        await using var probes = await RelocationProbeClient.ConnectAsync(options.GatewayEndpoint, ct);
        var pair = await probes.SelectPairAsync(ct);
        ZlinkStreamAssert.Ensure(pair.Error is null, "maintenance rejection needs a cross-owner adjacent pair");
        var edge = CrossingCoordinates(pair.SourceZoneId, pair.TargetZoneId);
        await MoveToAsync(player, edge.Source.X, edge.Source.Y, ct);

        await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);
        var nodes = await ops.WatchNodesAsync(ct);
        var targetNodeId = nodes.Nodes.Single(node =>
            node.Zones.Contains(pair.TargetZoneId, StringComparer.Ordinal)).NodeId;
        var enabledObserved = ops.Connector.WaitFor<NodeStatusNotify>()
            .Where(message => message.Payload.NodeId == targetNodeId && message.Payload.Maintenance)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        var enabled = await ops.SetMaintenanceAsync(targetNodeId, enabled: true, ct);
        if (enabled.Error is null) await enabledObserved;
        try
        {
            await ExpectMoveRejectedAsync(
                player,
                edge.Target.X,
                edge.Target.Y,
                MoveRejectReasons.ZoneMaintenance,
                ct);
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

    private static async ValueTask A4SameZonePlayers(ClientOptions options, CancellationToken ct)
    {
        var firstId = Unique("a4-b");
        var secondId = Unique("a4-a");

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

        }
    }

    private static async ValueTask A5Utf8OrderingAndOwnZonePrecedence(
        ClientOptions options,
        CancellationToken ct)
    {
        // UTF-16 ordinal and UTF-8 byte order disagree for U+10000 versus U+E000.
        var firstId = Unique("a5-\uE000");
        var secondId = Unique("a5-\U00010000");
        await using var first = await GameClient.ConnectAsync(options, firstId, ct);
        await first.JoinWorldAsync(ct);
        await using var second = await GameClient.ConnectAsync(options, secondId, ct);
        await second.JoinWorldAsync(ct);

        var state = (await first.Connector.WaitFor<ZoneStateNotify>()
            .Where(message => message.Payload.Players.Any(p => p.PlayerId == firstId)
                              && message.Payload.Players.Any(p => p.PlayerId == secondId))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async(ct)).Payload;
        var ids = state.Players.Select(player => player.PlayerId).ToArray();
        ZlinkStreamAssert.Ensure(
            ids.OrderBy(id => id, Utf8StringComparer.Instance).SequenceEqual(ids),
            "Players is ordered by PlayerId UTF-8 bytes");
        ZlinkStreamAssert.Ensure(
            state.Players.Count(player => player.PlayerId == firstId) == 1
            && state.Players.Single(player => player.PlayerId == firstId).ZoneId == state.ZoneId,
            "the resident value wins over any border copy of the same PlayerId");
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
                        .Timeout(BorderObservationTimeout)
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
                        .Timeout(BorderObservationTimeout)
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
            .Timeout(BorderObservationTimeout)
            .Async(ct);
        foreach (var step in west.PlanWalkWithinZone(45, 45))
        {
            var arrived = west.Connector.WaitFor<ZoneStateNotify>()
                .Where(message => message.Payload.Players.Any(p =>
                    p.PlayerId == west.PlayerId && p.X == step.X && p.Y == step.Y))
                .Timeout(BorderObservationTimeout)
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
                .Timeout(BorderObservationTimeout)
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

    /// <summary>
    /// Repeated relocation round trip (ZW-B7). After the ZW-B2 crossing, the same player walks
    /// back across the same boundary to the node that owned it first. Returning to a
    /// previously-visited node is exactly the case where ObjectGeneration stays unchanged while
    /// the owner and binding fences have advanced, so the assertion is identity continuity: the
    /// same ActorId, the same ObjectGeneration on all three probes, and the settling notifies
    /// arriving over the connection that was bound before either crossing.
    /// </summary>
    private static async ValueTask B7RelocationRoundTrip(ClientOptions options, CancellationToken ct)
    {
        var playerId = Unique("b7");
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

        // First leg (the ZW-B2 family crossing): the player leaves the source owner for the
        // adjacent zone's different owner. Every step is awaited, so the leg is fully settled
        // before the return leg starts.
        await MoveToAsync(player, source.X, source.Y, ct);
        var atSource = await probes.FindActorAsync(playerId, ct);
        ZlinkStreamAssert.Ensure(
            atSource.Error is null && atSource.OwnerNodeRid == pair.SourceOwnerNodeRid,
            "the selected source zone owns the actor before the first crossing");

        await MoveToAsync(player, target.X, target.Y, ct);
        var atTarget = await probes.FindActorAsync(playerId, ct);
        ZlinkStreamAssert.Ensure(
            atTarget.Error is null
            && atTarget.OwnerNodeRid == pair.TargetOwnerNodeRid
            && atTarget.OwnerNodeRid != atSource.OwnerNodeRid,
            "the first crossing moved the actor to the adjacent zone's different owner");

        // Return leg: the same player crosses the same boundary back. MoveToAsync awaits the
        // crossing and hands back the notify, so the message that reported the return can be
        // inspected here. (A second waiter with the same filter would race it for the single
        // notify — the connector's WaitFor consumes what it matches.)
        var returned = await MoveToAsync(player, source.X, source.Y, ct);
        ZlinkStreamAssert.Ensure(
            returned is not null
            && object.Equals(playerId, returned.PlayerId)
            && object.Equals(pair.SourceZoneId, returned.ZoneId),
            "the return crossing names the same actor entering the original zone");

        // Settle with one more awaited move to a coordinate this player has never stood on
        // (the walks above only touch multiples of five). A snapshot naming it can only
        // postdate the return, and it arrives over the same connection that was bound before
        // either crossing — the binding survived both relocations.
        var settleY = source.Y + 3;
        var settledWait = player.Connector.WaitFor<ZoneStateNotify>()
            .Where(message => message.Payload.ZoneId == pair.SourceZoneId
                              && message.Payload.Players.Any(p =>
                                  p.PlayerId == playerId && p.X == source.X && p.Y == settleY))
            .Timeout(CrossNodeObservationTimeout)
            .Async(ct);
        await player.MoveAsync(source.X, settleY);
        var settled = (await settledWait).Payload;
        player.Position = (source.X, settleY);
        ZlinkStreamAssert.Ensure(
            object.Equals(pair.SourceZoneId, settled.ZoneId),
            "the round trip settles in the original zone on the still-bound session");

        // Identity continuity across A→B→A: the original owner holds the actor again, under
        // the same ActorId and the ObjectGeneration it had before the first crossing. Only the
        // owner changed — twice — which is precisely what distinguishes a returning actor from
        // a re-created one.
        var atHome = await probes.FindActorAsync(playerId, ct);
        ZlinkStreamAssert.Ensure(
            atHome.Error is null && atHome.OwnerNodeRid == pair.SourceOwnerNodeRid,
            "the return crossing restored the original owner");
        ZlinkStreamAssert.Ensure(
            object.Equals(atSource.ActorId, atTarget.ActorId)
            && object.Equals(atSource.ActorId, atHome.ActorId),
            "the round trip keeps the same ActorId on every probe");
        ZlinkStreamAssert.Ensure(
            atSource.ObjectGeneration == atTarget.ObjectGeneration
            && atSource.ObjectGeneration == atHome.ObjectGeneration,
            "returning to a previously visited node preserves ObjectGeneration");
    }

    private static async ValueTask B3ActorGenerationPreserved(
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
    private static async ValueTask B5MessageFollowOneWay(ClientOptions options, CancellationToken ct)
    {
        var prepared = await PrepareMessageFollowAsync("b5", options, ct);
        await using var probes = prepared.Probes;
        await using var player = prepared.Player;
        var probeId = $"one-way-{Unique("b5")}";
        var payload = Encoding.UTF8.GetBytes("one-way-payload");
        await probes.SendMessageFollowProbeAsync(prepared.PlayerId, probeId, payload);
        Console.WriteLine(
            $"message-follow-one-way completed actor={prepared.PlayerId} probe={probeId} "
            + $"generation={prepared.Generation}");
    }

    private static async ValueTask B6MessageFollowRequest(ClientOptions options, CancellationToken ct)
    {
        var prepared = await PrepareMessageFollowAsync("b6", options, ct);
        await using var probes = prepared.Probes;
        await using var player = prepared.Player;
        var requestId = $"request-{Unique("b6")}";
        var requestPayload = Encoding.UTF8.GetBytes("request-payload");
        var request = probes.RequestMessageFollowProbeAsync(
            prepared.PlayerId,
            requestId,
            requestPayload,
            ct);

        var reply = await request;
        ZlinkStreamAssert.Ensure(
            reply.ProbeId == requestId
            && reply.Payload.AsSpan().SequenceEqual(requestPayload),
            "the followed request preserves its payload and reply correlation");
        Console.WriteLine(
            $"message-follow-request completed actor={prepared.PlayerId} request={requestId} "
            + $"generation={prepared.Generation}");
    }

    private static async ValueTask<MessageFollowPreparation> PrepareMessageFollowAsync(
        string idPrefix,
        ClientOptions options,
        CancellationToken ct)
    {
        var playerId = Unique(idPrefix);
        var probes = await RelocationProbeClient.ConnectAsync(options.GatewayEndpoint, ct);
        var pair = await probes.SelectPairAsync(ct);
        ZlinkStreamAssert.Ensure(
            pair.Error is null,
            "a release run requires adjacent Zone Spots with different current owners");

        var player = await GameClient.ConnectAsync(options, playerId, ct);
        var initialState = player.Connector.WaitFor<ZoneStateNotify>()
            .Where(message => message.Payload.Players.Any(candidate => candidate.PlayerId == playerId))
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
            "the public Actor request primes the previous-owner route");

        await MoveToAsync(player, target.X, target.Y, ct);
        var after = await probes.FindActorAsync(playerId, ct);
        ZlinkStreamAssert.Ensure(
            after.Error is null
            && after.OwnerNodeRid == pair.TargetOwnerNodeRid
            && after.OwnerNodeRid != before.OwnerNodeRid
            && after.ObjectGeneration == before.ObjectGeneration,
            "the actor moved owners without changing ObjectGeneration");
        return new MessageFollowPreparation(playerId, probes, player, after.ObjectGeneration);
    }

    private sealed record MessageFollowPreparation(
        string PlayerId,
        RelocationProbeClient Probes,
        GameClient Player,
        ulong Generation);

    private static (int X, int Y) ZoneCenter(string zoneId) => zoneId switch
    {
        ZoneIds.NorthWest => (25, 25),
        ZoneIds.NorthEast => (75, 25),
        ZoneIds.SouthWest => (25, 75),
        ZoneIds.SouthEast => (75, 75),
        _ => throw new ScenarioFailure($"Unknown ZoneId '{zoneId}'.")
    };

    private static ((int X, int Y) Source, (int X, int Y) Target) CrossingCoordinates(
        string sourceZoneId,
        string targetZoneId) => (sourceZoneId, targetZoneId) switch
        {
            (ZoneIds.NorthWest, ZoneIds.NorthEast) => ((48, 25), (52, 25)),
            (ZoneIds.NorthWest, ZoneIds.SouthWest) => ((25, 48), (25, 52)),
            (ZoneIds.NorthEast, ZoneIds.SouthEast) => ((75, 48), (75, 52)),
            (ZoneIds.SouthWest, ZoneIds.SouthEast) => ((48, 75), (52, 75)),
            (ZoneIds.NorthEast, ZoneIds.NorthWest) => ((52, 25), (48, 25)),
            (ZoneIds.SouthWest, ZoneIds.NorthWest) => ((25, 52), (25, 48)),
            (ZoneIds.SouthEast, ZoneIds.NorthEast) => ((75, 52), (75, 48)),
            (ZoneIds.SouthEast, ZoneIds.SouthWest) => ((52, 75), (48, 75)),
            _ => throw new ScenarioFailure(
                $"Zones '{sourceZoneId}' and '{targetZoneId}' are not a canonical directed edge.")
        };

    private static readonly (string Source, string Target)[] AdjacentZonePairs =
    [
        (ZoneIds.NorthWest, ZoneIds.NorthEast),
        (ZoneIds.NorthWest, ZoneIds.SouthWest),
        (ZoneIds.NorthEast, ZoneIds.SouthEast),
        (ZoneIds.SouthWest, ZoneIds.SouthEast)
    ];

    private static async ValueTask ExpectMoveRejectedAsync(
        GameClient player,
        int x,
        int y,
        string expectedReason,
        CancellationToken cancellationToken)
    {
        var waiting = player.Connector.WaitFor<MoveRejectedNotify>()
            .Timeout(TimeSpan.FromSeconds(15))
            .Async(cancellationToken);
        await player.MoveAsync(x, y);
        var rejected = (await waiting).Payload;
        ZlinkStreamAssert.Ensure(
            rejected.Reason == expectedReason,
            $"the rejection reason is {expectedReason}");
        ZlinkStreamAssert.Ensure(
            rejected.X == player.Position.X && rejected.Y == player.Position.Y,
            "a refused move leaves the coordinate untouched");
    }

    private sealed class Utf8StringComparer : IComparer<string>
    {
        public static Utf8StringComparer Instance { get; } = new();

        public int Compare(string? left, string? right)
        {
            if (ReferenceEquals(left, right)) return 0;
            if (left is null) return -1;
            if (right is null) return 1;
            return Encoding.UTF8.GetBytes(left).AsSpan()
                .SequenceCompareTo(Encoding.UTF8.GetBytes(right));
        }
    }

    /// <summary>
    /// Walks the player to the target one awaited step at a time. Returns the payload of the
    /// last ZoneChangedNotify the walk consumed, or null when no boundary was crossed — the
    /// notify is consumed here, so a caller that wants to inspect it must take it from the
    /// return value rather than arm a competing waiter.
    /// </summary>
    private static async ValueTask<ZoneChangedNotify?> MoveToAsync(
        GameClient player,
        int targetX,
        int targetY,
        CancellationToken cancellationToken)
    {
        ZoneChangedNotify? lastCrossing = null;
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
                lastCrossing = (await changed).Payload;
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

        return lastCrossing;
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
        var players = new List<GameClient>();
        try
        {
            foreach (var zoneId in ZoneTopology.Zones)
            {
                var player = await GameClient.ConnectAsync(options, Unique($"d1-{zoneId}"), ct);
                players.Add(player);
                await player.JoinWorldAsync(ct);
                var center = ZoneCenter(zoneId);
                await MoveToAsync(player, center.X, center.Y, ct);
            }

            var deliveries = players.Select(player =>
                    player.Connector.WaitFor<WorldAnnounceNotify>()
                        .Timeout(AnnouncementSettleTicks)
                        .Async(ct))
                .ToArray();
            await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);
            var published = await ops.AnnounceAsync("server maintenance starts in 10 minutes", ct);
            ZlinkStreamAssert.Ensure(
                published.AnnouncementId.Length > 0,
                "the publish is answered with an id");

            foreach (var delivery in deliveries)
            {
                var received = (await delivery).Payload;
                ZlinkStreamAssert.Ensure(
                    received.AnnouncementId == published.AnnouncementId,
                    "each zone's game client receives the published AnnouncementId");
            }
            foreach (var player in players)
                await player.Connector.ExpectNone<WorldAnnounceNotify>()
                    .Within(AnnouncementSettleTicks)
                    .Async(ct);
        }
        finally
        {
            foreach (var player in players) await player.DisposeAsync();
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
        observed = await ops.WatchNodesAsync(ct);
        var targetNodeId = observed.Nodes
            .Where(node => node.Registered && node.Connected)
            .OrderBy(node => node.NodeId, StringComparer.Ordinal)
            .Last().NodeId;
        var unaffected = observed.Nodes
            .Where(node => node.NodeId != targetNodeId)
            .ToDictionary(node => node.NodeId, node => node.Maintenance, StringComparer.Ordinal);

        var enabledObserved = ops.Connector.WaitFor<NodeStatusNotify>()
            .Where(message => message.Payload.NodeId == targetNodeId && message.Payload.Maintenance)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        var applied = await ops.SetMaintenanceAsync(targetNodeId, enabled: true, ct);
        if (applied.Error is null) await enabledObserved;
        try
        {
            ZlinkStreamAssert.Ensure(
                applied.Error is null && applied.NodeId == targetNodeId && applied.Enabled,
                "maintenance desired state is stored for the selected NodeId");
            var after = await ops.WatchNodesAsync(ct);
            ZlinkStreamAssert.Ensure(
                after.Nodes.Single(node => node.NodeId == targetNodeId).Maintenance,
                "the selected NodeId alone reports maintenance enabled");
            foreach (var (nodeId, wasEnabled) in unaffected)
                ZlinkStreamAssert.Ensure(
                    after.Nodes.Single(node => node.NodeId == nodeId).Maintenance == wasEnabled,
                    $"maintenance did not change non-target node {nodeId}");
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
    private static async ValueTask E3SameZoneMoveAllowed(ClientOptions options, CancellationToken ct)
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
        await using var player = await GameClient.ConnectAsync(options, Unique("e3"), ct);
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
            // Same-zone movement does not invoke Actor Join and remains allowed.
            var moved = player.Connector.WaitFor<ZoneStateNotify>()
                .Where(message => message.Payload.Players.Any(p =>
                    p.PlayerId == player.PlayerId && p.X == 30 && p.Y == 30))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(ct);
            await player.MoveAsync(30, 30);
            await moved;
            player.Position = (30, 30);
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
        var initialMembership = player.Connector.WaitFor<ZoneStateNotify>()
            .Where(message => message.Payload.Players.Any(candidate =>
                candidate.PlayerId == player.PlayerId))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async(ct);
        await player.JoinWorldAsync(ct);
        await initialMembership;

        await using var probes = await RelocationProbeClient.ConnectAsync(
            options.GatewayEndpoint,
            ct);
        var pair = await probes.SelectPairAsync(ct);
        ZlinkStreamAssert.Ensure(
            pair.Error is null,
            "maintained-source departure requires adjacent zones with different current owners");
        observed = await ops.WatchNodesAsync(ct);
        var sourceNodeId = observed.Nodes
            .Single(node => node.Zones.Contains(pair.SourceZoneId, StringComparer.Ordinal))
            .NodeId;
        var targetNodeId = observed.Nodes
            .Single(node => node.Zones.Contains(pair.TargetZoneId, StringComparer.Ordinal))
            .NodeId;
        ZlinkStreamAssert.Ensure(
            !string.Equals(sourceNodeId, targetNodeId, StringComparison.Ordinal),
            "the observed relocation pair has different application owners");
        var source = ZoneCenter(pair.SourceZoneId);
        await MoveToAsync(player, source.X, source.Y, ct);

        var enabledObserved = ops.Connector.WaitFor<NodeStatusNotify>()
            .Where(message => message.Payload.NodeId == sourceNodeId && message.Payload.Maintenance)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        var enabled = await ops.SetMaintenanceAsync(sourceNodeId, enabled: true, ct);
        ZlinkStreamAssert.Ensure(
            enabled.Error is null && enabled.Enabled,
            "the observed source owner enters maintenance");
        await enabledObserved;
        try
        {
            var target = ZoneCenter(pair.TargetZoneId);
            var changed = await MoveToAsync(player, target.X, target.Y, ct);
            ZlinkStreamAssert.Ensure(
                changed is not null
                && string.Equals(changed.ZoneId, pair.TargetZoneId, StringComparison.Ordinal),
                "the player leaves the maintained source for the healthy observed owner");
        }
        finally
        {
            var disabledObserved = ops.Connector.WaitFor<NodeStatusNotify>()
                .Where(message => message.Payload.NodeId == sourceNodeId && !message.Payload.Maintenance)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async(ct);
            var disabled = await ops.SetMaintenanceAsync(sourceNodeId, enabled: false, ct);
            ZlinkStreamAssert.Ensure(
                disabled.Error is null && !disabled.Enabled,
                "the observed source owner leaves maintenance");
            await disabledObserved;
        }
    }

    private static async ValueTask E4SameNodeDifferentZoneRejected(
        ClientOptions options,
        CancellationToken ct)
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
        }
        observed = await ops.WatchNodesAsync(ct);

        var selected = AdjacentZonePairs
            .Select(pair => new
            {
                Pair = pair,
                Node = observed.Nodes.FirstOrDefault(node =>
                    node.Registered
                    && node.Zones.Contains(pair.Source, StringComparer.Ordinal)
                    && node.Zones.Contains(pair.Target, StringComparer.Ordinal))
            })
            .FirstOrDefault(candidate => candidate.Node is not null)
            ?? throw new ScenarioFailure(
                "ZW-E4 requires two adjacent zones currently owned by the same ZoneNode.");
        var targetNodeId = selected.Node!.NodeId;
        var edge = CrossingCoordinates(selected.Pair.Source, selected.Pair.Target);

        await using var player = await GameClient.ConnectAsync(options, Unique("e4"), ct);
        await player.JoinWorldAsync(ct);
        await MoveToAsync(player, edge.Source.X, edge.Source.Y, ct);

        var enabledObserved = ops.Connector.WaitFor<NodeStatusNotify>()
            .Where(message => message.Payload.NodeId == targetNodeId && message.Payload.Maintenance)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        var enabled = await ops.SetMaintenanceAsync(targetNodeId, enabled: true, ct);
        if (enabled.Error is null) await enabledObserved;
        try
        {
            await ExpectMoveRejectedAsync(
                player,
                edge.Target.X,
                edge.Target.Y,
                MoveRejectReasons.ZoneMaintenance,
                ct);
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

    private static async ValueTask E6NodeDiagnostics(ClientOptions options, CancellationToken ct)
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

    private static async ValueTask G4CrashEndsCurrentOperationUnavailable(
        ClientOptions options,
        CancellationToken ct)
    {
        await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);
        var nodes = await ops.WatchNodesAsync(ct);
        var replacementNode = nodes.Nodes.Single(node => node.NodeId == NodeIds.East);
        await using var probes = await RelocationProbeClient.ConnectAsync(options.GatewayEndpoint, ct);
        var observed = await probes.SelectPairAsync(ct);
        ZlinkStreamAssert.Ensure(observed.Error is null, "G4 requires a cross-owner adjacent pair");

        var pair = replacementNode.Zones.Contains(observed.TargetZoneId, StringComparer.Ordinal)
            ? observed
            : replacementNode.Zones.Contains(observed.SourceZoneId, StringComparer.Ordinal)
                ? new RelocationPairRes(
                    observed.TargetZoneId,
                    observed.SourceZoneId,
                    observed.TargetOwnerNodeRid,
                    observed.SourceOwnerNodeRid)
                : throw new ScenarioFailure("the crash replacement node owns neither probed zone");
        var edge = CrossingCoordinates(pair.SourceZoneId, pair.TargetZoneId);

        await using var player = await GameClient.ConnectAsync(options, Unique("g4-crash"), ct);
        await player.JoinWorldAsync(ct);
        await MoveToAsync(player, edge.Source.X, edge.Source.Y, ct);
        var failed = player.Connector.WaitFor<CrashRelocationProbeRes>()
            .Where(message => message.Payload.Error == "Unavailable")
            .Timeout(TimeSpan.FromSeconds(45))
            .Async(ct);
        await player.Connector
            .Send(new CrashRelocationProbeMsg(edge.Target.X, edge.Target.Y))
            .Async(ct);
        Console.WriteLine($"scenario ZW-G4 armed node={NodeIds.East}");

        var terminal = (await failed).Payload;
        ZlinkStreamAssert.Ensure(
            terminal.Error == "Unavailable",
            "the in-flight operation ends Unavailable instead of auto-failing over");
    }

    private static async ValueTask B8SessionRouteSealTimeoutReconnect(
        ClientOptions options,
        CancellationToken ct)
    {
        await using var probes = await RelocationProbeClient.ConnectAsync(options.GatewayEndpoint, ct);
        var pair = await probes.SelectPairAsync(ct);
        ZlinkStreamAssert.Ensure(pair.Error is null, "B8 requires a cross-owner adjacent pair");
        var edge = CrossingCoordinates(pair.SourceZoneId, pair.TargetZoneId);
        var playerId = Unique("b8-seal");
        await using var player = await GameClient.ConnectAsync(options, playerId, ct);
        await player.JoinWorldAsync(ct);
        await MoveToAsync(player, edge.Source.X, edge.Source.Y, ct);

        var disconnected = new TaskCompletionSource<ZlinkStreamCloseReason>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        player.Connector.Disconnected += (message, _) =>
        {
            disconnected.TrySetResult(message.CloseReason);
            return ValueTask.CompletedTask;
        };
        Console.WriteLine(
            $"scenario ZW-B8 armed actor={playerId} target={pair.TargetZoneId}");
        var armFile = options.FaultArmFile;
        ZlinkStreamAssert.Ensure(!string.IsNullOrWhiteSpace(armFile), "B8 runner arm file is configured");
        for (var attempt = 0; !File.Exists(armFile); attempt++)
        {
            if (attempt >= 200)
                throw new ScenarioFailure("B8 runner did not arm the command-44 fault");
            await Task.Delay(TimeSpan.FromMilliseconds(50), ct);
        }
        await player.MoveAsync(edge.Target.X, edge.Target.Y);
        var closeReason = await disconnected.Task.WaitAsync(TimeSpan.FromSeconds(45), ct);
        Console.WriteLine($"scenario ZW-B8 disconnected reason={closeReason}");

        for (var attempt = 0; File.Exists(armFile); attempt++)
        {
            if (attempt >= 900)
            {
                throw new ScenarioFailure(
                    "ZW-B8 precondition unmet: runner did not prove command-44 interception " +
                    "and target relocation commit.");
            }
            await Task.Delay(TimeSpan.FromMilliseconds(50), ct);
        }

        await player.Connector.Connect.Async(ct);
        var rebound = await player.JoinWorldAsync(ct);
        ZlinkStreamAssert.Ensure(rebound.PlayerId == playerId, "B8 rebind preserves PlayerId");
        ZlinkStreamAssert.Ensure(
            rebound.ZoneId == pair.TargetZoneId,
            "B8 rejoin reaches the already relocated Actor instead of creating a replacement");
    }

    private static async ValueTask G4FreshReplacementAcceptsObject(
        ClientOptions options,
        CancellationToken ct)
    {
        await using var probes = await RelocationProbeClient.ConnectAsync(
            options.GatewayEndpoint,
            ct);
        for (var attempt = 0; attempt < 16; attempt++)
        {
            var created = await probes.CreateFreshActorAsync(Unique("g4-fresh"), ct);
            ZlinkStreamAssert.Ensure(created.Error is null, "fresh Actor creation succeeded");
            ZlinkStreamAssert.Ensure(created.ObjectGeneration > 0, "fresh Actor has an object generation");
            Console.WriteLine(
                $"scenario ZW-G4-fresh owner={created.OwnerNodeRid} actor={created.ActorId}");
        }
    }

    /// <summary>A brand-new entry into a maintained node is refused (§2.3).</summary>
    private static async ValueTask E2MaintenanceBlocksNewEntry(ClientOptions options, CancellationToken ct)
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
            await using var player = await GameClient.ConnectAsync(options, Unique("e2"), ct);
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
    /// Once the last report is older than the 15-second Ops TTL, Registered becomes false.
    /// The runner crashes zone-node-2 while this scenario is watching (§2.2).
    /// </summary>
    private static async ValueTask C3ReportTtlExpired(ClientOptions options, CancellationToken ct)
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
        Console.WriteLine("scenario ZW-C3 armed");

        var gone = (await goneWait).Payload;

        ZlinkStreamAssert.Ensure(!gone.Registered, "a stopped node stops being registered");
    }

    /// <summary>
    /// A normal node shutdown drops its Ops connection. This is a socket event, not a report-TTL
    /// decision: the node may still be registered while its link is gone (§8.1).
    /// </summary>
    private static async ValueTask C2NodeDisconnected(ClientOptions options, CancellationToken ct)
    {
        await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);
        var nodes = await ops.WatchNodesAsync(ct);
        var targetNodeId = nodes.Nodes
            .Where(node => node.Connected)
            .OrderBy(node => node.NodeId, StringComparer.Ordinal)
            .Last().NodeId;

        // A link that was never up cannot drop, so the connected state has to be
        // established before the drop is observed — otherwise the default state passes the test.
        ZlinkStreamAssert.Ensure(
            nodes.Nodes.Any(n => n.NodeId == targetNodeId && n.Connected),
            "the runtime-selected node's link is up before the runner stops it");
        var droppedWait = ops.Connector.WaitFor<NodeStatusNotify>()
            .Where(message => message.Payload.NodeId == targetNodeId && !message.Payload.Connected)
            .Timeout(TimeSpan.FromSeconds(40))
            .Async(ct);
        Console.WriteLine("scenario ZW-C2 armed");

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
        await using var probes = await RelocationProbeClient.ConnectAsync(options.GatewayEndpoint, ct);
        var pair = await probes.SelectPairAsync(ct);
        ZlinkStreamAssert.Ensure(pair.Error is null, "border expiry requires a cross-owner adjacent pair");
        var edge = CrossingCoordinates(pair.SourceZoneId, pair.TargetZoneId);
        var sourceId = Unique("b4-source");
        var targetId = Unique("b4-target");

        await using var source = await GameClient.ConnectAsync(options, sourceId, ct);
        await source.JoinWorldAsync(ct);
        await MoveToAsync(source, edge.Source.X, edge.Source.Y, ct);
        await using var target = await GameClient.ConnectAsync(options, targetId, ct);
        await target.JoinWorldAsync(ct);
        var visible = source.Connector.WaitFor<ZoneStateNotify>()
            .Where(message => message.Payload.ZoneId == pair.SourceZoneId
                              && message.Payload.Players.Any(player =>
                                  player.PlayerId == targetId && player.ZoneId == pair.TargetZoneId))
            .Timeout(CrossNodeObservationTimeout)
            .Async(ct);
        await MoveToAsync(target, edge.Target.X, edge.Target.Y, ct);
        await visible;

        await using var ops = await OpsClient.ConnectAsync(options.OpsEndpoint, ct);
        var nodes = await ops.WatchNodesAsync(ct);
        var targetNodeId = nodes.Nodes.Single(node =>
            node.Zones.Contains(pair.TargetZoneId, StringComparer.Ordinal)).NodeId;
        var expiredWait = source.Connector.WaitFor<ZoneStateNotify>()
            .Where(message => message.Payload.ZoneId == pair.SourceZoneId
                              && message.Payload.Players.All(player => player.PlayerId != targetId))
            .Timeout(TimeSpan.FromSeconds(60))
            .Async(ct);
        Console.WriteLine($"scenario ZW-B4 armed node={targetNodeId}");

        var expired = (await expiredWait).Payload;
        ZlinkStreamAssert.Ensure(
            expired.Players.All(player => player.PlayerId != targetId),
            "the interrupted FromZoneId snapshot expires after three local ticks");
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
            //  Ops answers a diagnostics request it cannot route with
            //  NodeUnavailable and Maintenance=false. A node that just
            //  restarted can still be between transport connections, so that
            //  answer says nothing about the stored desired state - poll until
            //  Ops can reach the node before judging the restore.
            var diagnostics = await ops.DiagnoseAsync(targetNodeId, ct);
            var reachableBy = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(20);
            while (diagnostics.Error is not null
                && DateTimeOffset.UtcNow < reachableBy)
            {
                await Task.Delay(TimeSpan.FromMilliseconds(100), ct);
                diagnostics = await ops.DiagnoseAsync(targetNodeId, ct);
            }
            ZlinkStreamAssert.Ensure(
                diagnostics.Error is null,
                "Ops can reach the restarted node to read its maintenance state");
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
        var firstWaiting = player.Connector.WaitFor<ZoneStateNotify>()
            .Where(message => message.Payload.Players.Any(p =>
                p.IsBot && string.Equals(p.ZoneId, message.Payload.ZoneId, StringComparison.Ordinal)))
            .Timeout(BotObservationTimeout)
            .Async(ct);
        await player.JoinWorldAsync(ct);

        var first = (await firstWaiting).Payload;
        var bots = first.Players
            .Where(p => p.IsBot && string.Equals(p.ZoneId, first.ZoneId, StringComparison.Ordinal))
            .ToDictionary(p => p.PlayerId, StringComparer.Ordinal);
        ZlinkStreamAssert.Ensure(
            bots.Count > 0,
            "the client sees a bot in its zone with no client attached");

        // The first local bot visible to this client must change position. The runner separately
        // counts all eight bots; F2 checks the cross-node X patrol and F4 checks reversal.
        var moved = player.Connector.WaitFor<ZoneStateNotify>()
            .Where(message => message.Payload.Players.Any(bot =>
            {
                if (!bot.IsBot || !bots.TryGetValue(bot.PlayerId, out var before)) return false;
                return bot.X != before.X || bot.Y != before.Y;
            }))
            .Timeout(BotObservationTimeout)
            .Async(ct);
        await moved;
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
