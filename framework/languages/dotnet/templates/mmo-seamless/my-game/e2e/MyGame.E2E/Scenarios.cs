using MyGame.Shared;
using MyGame.Zone;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;

namespace MyGame.E2E;

internal static class Scenarios
{
    private static readonly string[] Order = ["cross", "border", "reconnect", "update"];
    internal static IReadOnlyList<string> All => Order;

    public static string[] Select(string? requested, bool hasOrchestrator)
    {
        var selected = requested is null
            ? Order.Where(name => hasOrchestrator || name != "update").ToArray()
            : requested.Split(',', StringSplitOptions.TrimEntries);
        if (
            selected.Length == 0
            || selected.Distinct().Count() != selected.Length
            || selected.Any(name => !Order.Contains(name))
        )
            throw new ArgumentException(
                "Select distinct scenarios from cross,border,reconnect,update."
            );
        if (!hasOrchestrator && selected.Contains("update"))
            throw new ArgumentException("Update requires --mode run.");
        return Order.Where(selected.Contains).ToArray();
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
            throw new InvalidOperationException(message);
    }

    private static void SameActor(SessionInfo before, SessionInfo after)
    {
        Require(before.ActorId == after.ActorId, "Actor ID changed.");
        Require(
            before.ObjectGeneration == after.ObjectGeneration,
            "Actor generation changed."
        );
    }

    private static void SameState(WorldState expected, WorldState actual, string reason)
    {
        Require(
            expected.X == actual.X
                && expected.Sequence == actual.Sequence
                && expected.ZoneId == actual.ZoneId
                && expected.NodeId == actual.NodeId,
            $"{reason} changed authoritative state."
        );
    }

    public static async Task RunAsync(
        string endpoint,
        WorldRun run,
        string[] selected,
        CancellationToken ct
    )
    {
        var passed = 0;
        var current = string.Empty;
        try
        {
            foreach (var scenario in selected)
            {
                current = scenario;
                switch (scenario)
                {
                    case "cross":
                        await CrossAsync(endpoint, run, ct);
                        break;
                    case "border":
                        await BorderAsync(endpoint, run, ct);
                        break;
                    case "reconnect":
                        await ReconnectAsync(endpoint, run, ct);
                        break;
                    case "update":
                        await UpdateAsync(endpoint, run, ct);
                        break;
                }
                Console.WriteLine($"✓ {scenario}");
                passed++;
            }
        }
        catch
        {
            Console.WriteLine($"✗ {current}");
            throw;
        }
        finally
        {
            Console.WriteLine($"Scenarios: {passed}/{selected.Length} passed");
        }
    }

    private static async Task CrossAsync(string endpoint, WorldRun run, CancellationToken ct)
    {
        await using var client = await GameClient.ConnectAsync(
            run.Outcome,
            endpoint,
            "cross-" + Guid.NewGuid().ToString("N"),
            ct
        );
        var before = await client.JoinAsync(294, ct);
        var identity = await client.SessionAsync(ct);
        Require(
            before.ZoneId == "zone-3" && before.NodeId == "zoneA",
            "Cross did not start in Zone node A."
        );

        var tooLarge = await client.MoveRejectedAsync(ZonePositionDomain.MaxStep + 1, 1, ct);
        Require(tooLarge.Reason == "step-too-large" && tooLarge.RetryAfterMs == 0,
            "MaxStep rejection did not return step-too-large.");
        var unchanged = await client.StateAsync(ct);
        SameState(before, unchanged, "step-too-large rejection");

        var firstResponse = client.WaitForMoveAsync(1, ct);
        var budgetResponse = client.WaitForRejectionAsync("move-budget", ct);
        await client.SendMoveAsync(2, 1, ct);
        await client.SendMoveAsync(ZonePositionDomain.MaxStep, 2, ct);
        var first = await firstResponse;
        Require(first.X == 296 && first.Sequence == 1, "The first server-calculated move was incorrect.");
        var budget = await budgetResponse;
        Require(budget.Reason == "move-budget" && budget.RetryAfterMs > 0,
            "Move budget rejection did not include a positive server retry delay.");
        var afterBudgetReject = await client.StateAsync(ct);
        SameState(first, afterBudgetReject, "move-budget rejection");
        await Task.Delay(TimeSpan.FromMilliseconds(budget.RetryAfterMs), ct);
        var after = await client.MoveWithBudgetRetryAsync(ZonePositionDomain.MaxStep, 2, ct);
        SameActor(identity, await client.SessionAsync(ct));
        Require(
            after.ZoneId == "zone-4" && after.NodeId == "zoneB" && after.X == 306,
            "Cross did not relocate to Zone node B after multiple accepted moves."
        );
        Require(!client.Disconnected, "Cross disconnected the session.");

        var continued = await client.MoveWithBudgetRetryAsync(1, 3, ct);
        SameActor(identity, await client.SessionAsync(ct));
        Require(continued.X == 307 && continued.NodeId == "zoneB", "Movement failed after cross.");
        var forbidden = await client.PacketRejectedAsync(
            new EnterZone(new Position(307), 3, 0),
            ct
        );
        Require(forbidden.Reason == "packet-not-allowed" && forbidden.RetryAfterMs == 0,
            "Session relayed an internal packet.");
        SameState(continued, await client.StateAsync(ct), "packet-not-allowed rejection");

        await using var edge = await GameClient.ConnectAsync(
            run.Outcome,
            endpoint,
            "edge-" + Guid.NewGuid().ToString("N"),
            ct
        );
        var atEdge = await edge.JoinAsync(GameNames.WorldWidth - 1, ct);
        var outside = await edge.MoveRejectedAsync(1, 1, ct);
        Require(outside.Reason == "outside-world" && outside.RetryAfterMs == 0,
            "World boundary rejection did not return outside-world.");
        SameState(atEdge, await edge.StateAsync(ct), "outside-world rejection");

        var dungeon = await client.DungeonAsync("dungeon-" + Guid.NewGuid().ToString("N"), ct);
        Require(
            dungeon.NodeId == "instance",
            "Dungeon GetOrCreate did not resolve to the Instance node."
        );
    }

    private static async Task BorderAsync(string endpoint, WorldRun run, CancellationToken ct)
    {
        await using var observer = await GameClient.ConnectAsync(
            run.Outcome,
            endpoint,
            "observer-" + Guid.NewGuid().ToString("N"),
            ct
        );
        var observed = await observer.JoinAsync(300, ct);
        await using var source = await GameClient.ConnectAsync(
            run.Outcome,
            endpoint,
            "border-" + Guid.NewGuid().ToString("N"),
            ct
        );
        Require(observed.NodeId == "zoneB", "Border observer did not start on Zone node B.");

        var appearedTask = observer
            .Connector.WaitFor<PlayerAppeared>()
            .Where(message => message.Payload.PlayerId == source.PlayerId)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        var origin = await source.JoinAsync(299, ct);
        Require(origin.NodeId == "zoneA", "Border source did not start on Zone node A.");
        var appeared = (await appearedTask).Payload;
        Require(appeared.Position.X == 299, "Adjacent Zone did not observe PlayerAppeared.");

        var movedTask = observer
            .Connector.WaitFor<PlayerMoved>()
            .Where(message => message.Payload.PlayerId == source.PlayerId && message.Payload.Sequence == 1)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        var moved = await source.MoveWithBudgetRetryAsync(-1, 1, ct);
        var border = (await movedTask).Payload;
        Require(
            moved.X == 298
                && border.Position.X == moved.X
                && border.Sequence == moved.Sequence,
            "Adjacent Zone did not observe the authoritative border movement."
        );

        var leftTask = observer
            .Connector.WaitFor<PlayerLeft>()
            .Where(message =>
                message.Payload.PlayerId == source.PlayerId
                && message.Payload.Reason == PlayerLeftReason.LeftZone
            )
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        var reappearedTask = observer
            .Connector.WaitFor<PlayerAppeared>()
            .Where(message => message.Payload.PlayerId == source.PlayerId)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        var crossed = await source.MoveWithBudgetRetryAsync(2, 2, ct);
        var left = (await leftTask).Payload;
        var reappeared = (await reappearedTask).Payload;
        Require(left.Reason == PlayerLeftReason.LeftZone, "Zone departure used the wrong reason.");
        Require(crossed.ZoneId == "zone-4" && reappeared.Position.X == 300,
            "Zone arrival was not observed by the destination Zone.");

        var sameZoneMovedTask = observer
            .Connector.WaitFor<PlayerMoved>()
            .Where(message =>
                message.Payload.PlayerId == source.PlayerId && message.Payload.Sequence == 3
            )
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        var settled = await source.MoveWithBudgetRetryAsync(1, 3, ct);
        var sameZoneMoved = (await sameZoneMovedTask).Payload;
        Require(
            settled.X == 301
                && settled.Sequence == 3
                && sameZoneMoved.Position.X == settled.X
                && sameZoneMoved.Sequence == settled.Sequence,
            "Same-Zone players did not observe the completed movement."
        );

        var disconnectedTask = observer
            .Connector.WaitFor<PlayerLeft>()
            .Where(message =>
                message.Payload.PlayerId == source.PlayerId
                && message.Payload.Reason == PlayerLeftReason.Disconnected
            )
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        await source.DisposeAsync();
        var disconnected = (await disconnectedTask).Payload;
        Require(
            disconnected.Reason == PlayerLeftReason.Disconnected,
            "Connection loss used the wrong visibility end reason."
        );
    }

    private static async Task ReconnectAsync(string endpoint, WorldRun run, CancellationToken ct)
    {
        var id = "reconnect-" + Guid.NewGuid().ToString("N");
        WorldState before;
        SessionInfo identity;
        string session;
        await using (var client = await GameClient.ConnectAsync(run.Outcome, endpoint, id, ct))
        {
            await client.JoinAsync(299, ct);
            before = await client.MoveWithBudgetRetryAsync(1, 1, ct);
            identity = await client.SessionAsync(ct);
            session = identity.SessionId;
        }
        await using var reconnect = await GameClient.ConnectAsync(run.Outcome, endpoint, id, ct);
        var after = await reconnect.JoinAsync(0, ct);
        SameActor(identity, await reconnect.SessionAsync(ct));
        Require(
            before.X == after.X
                && before.Sequence == after.Sequence
                && before.ZoneId == after.ZoneId,
            "Reconnect overwrote authoritative world state."
        );
        Require(session != (await reconnect.SessionAsync(ct)).SessionId,
            "Reconnect reused the old physical session.");
        var moved = await reconnect.MoveWithBudgetRetryAsync(1, 2, ct);
        SameActor(identity, await reconnect.SessionAsync(ct));
        Require(moved.Sequence == 2, "Movement did not continue after reconnect.");
    }

    private static async Task UpdateAsync(string endpoint, WorldRun run, CancellationToken ct)
    {
        await using var client = await GameClient.ConnectAsync(
            run.Outcome,
            endpoint,
            "update-" + Guid.NewGuid().ToString("N"),
            ct
        );
        var before = await client.JoinAsync(299, ct);
        var identity = await client.SessionAsync(ct);
        Require(before.NodeId == "zoneA", "Update did not start on Zone node A.");
        var first = await client.MoveWithBudgetRetryAsync(-1, 1, ct);
        SameActor(identity, await client.SessionAsync(ct));
        async Task ObserveDrainAsync()
        {
            await run.ZoneA.WaitLineAsync(NodeProtocol.RelocationRequested, ct);
            await run.ZoneA.WaitLineAsync(NodeProtocol.Relocated, ct);
            await run.ZoneA.WaitLineAsync(NodeProtocol.SafeToShutdown, ct);
        }
        await run.ZoneA.CommandAsync(NodeProtocol.VacateZone);
        await run.ZoneA.WaitLineAsync(NodeProtocol.ZoneVacated, ct);
        var safe = ObserveDrainAsync();
        async Task<(WorldState State, long Sequence)> ContinuePlayingAsync()
        {
            var sequence = first.Sequence;
            var state = first;
            do
            {
                sequence++;
                var target = sequence % 2 == 0 ? 299 : 298;
                state = await client.MoveWithBudgetRetryAsync(target - state.X, sequence, ct);
                SameActor(identity, await client.SessionAsync(ct));
                Require(
                    state.Sequence == sequence && !client.Disconnected,
                    "Drain lost movement or disconnected the session."
                );
            } while (!safe.IsCompleted || state.NodeId != "zoneB");
            return (state, sequence);
        }
        var gameplay = ContinuePlayingAsync();
        await run.ZoneA.CommandAsync(NodeProtocol.Drain);
        await Task.WhenAll(safe, gameplay);
        var (state, sequence) = await gameplay;
        Require(state.ZoneId == "zone-3", "Drain changed the logical Zone.");
        await run.ReplaceZoneAAsync(ct);
        var after = await client.MoveWithBudgetRetryAsync(299 - state.X, sequence + 1, ct);
        SameActor(identity, await client.SessionAsync(ct));
        Require(
            after.NodeId == "zoneB"
                && !client.Disconnected,
            "Replacement interrupted the relocated player."
        );
        var dungeon = await client.DungeonAsync("after-update-" + Guid.NewGuid().ToString("N"), ct);
        Require(dungeon.NodeId == "instance", "Dungeon access failed after replacement.");
        await using var fresh = await GameClient.ConnectAsync(
            run.Outcome,
            endpoint,
            "replacement-" + Guid.NewGuid().ToString("N"),
            ct
        );
        var placed = await fresh.JoinAsync(NodeProtocol.ReplacementX, ct);
        Require(placed.NodeId == before.NodeId, "Replacement did not accept the new Zone.");
        var freshIdentity = await fresh.SessionAsync(ct);
        var moved = await fresh.MoveWithBudgetRetryAsync(1, 1, ct);
        Require(moved.NodeId == before.NodeId, "Replacement movement failed.");
        SameActor(freshIdentity, await fresh.SessionAsync(ct));
        Require(!fresh.Disconnected, "Replacement changed SessionInfo.");
    }
}
