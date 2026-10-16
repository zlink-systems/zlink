using MyGame.Shared;
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

    private static void SameActor(WorldState before, WorldState after)
    {
        Require(before.ActorId == after.ActorId, "Actor ID changed.");
        Require(before.ObjectGeneration == after.ObjectGeneration, "Actor generation changed.");
    }

    public static async Task RunAsync(
        string endpoint,
        WorldRun? run,
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
                        await CrossAsync(endpoint, ct);
                        break;
                    case "border":
                        await BorderAsync(endpoint, ct);
                        break;
                    case "reconnect":
                        await ReconnectAsync(endpoint, ct);
                        break;
                    case "update":
                        await UpdateAsync(endpoint, run!, ct);
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

    private static async Task CrossAsync(string endpoint, CancellationToken ct)
    {
        await using var client = await GameClient.ConnectAsync(
            endpoint,
            "cross-" + Guid.NewGuid().ToString("N"),
            ct
        );
        try
        {
            var before = await client.JoinAsync(299, ct);
            var session = await client.SessionAsync(ct);
            Require(
                before.ZoneId == "zone-3" && before.NodeId == "zoneA",
                "Cross did not start in Zone node A."
            );
            var after = await client.MoveAsync(300, 1, ct);
            SameActor(before, after);
            Require(
                after.ZoneId == "zone-4" && after.NodeId == "zoneB" && after.X == 300,
                "Cross did not relocate to Zone node B."
            );
            Require(
                session == await client.SessionAsync(ct) && !client.Disconnected,
                "Cross disconnected the session."
            );
            var continued = await client.MoveAsync(301, 2, ct);
            SameActor(before, continued);
            Require(
                continued.X == 301 && continued.NodeId == "zoneB",
                "Movement failed after cross."
            );
            var dungeon = await client.DungeonAsync("dungeon-" + Guid.NewGuid().ToString("N"), ct);
            Require(
                dungeon.NodeId == "instance",
                "Dungeon GetOrCreate did not resolve to the Instance node."
            );
        }
        catch (Exception failureBeforeCleanup)
        {
            Console.Error.WriteLine("Failure before cleanup: " + failureBeforeCleanup);
            throw;
        }
    }

    private static async Task BorderAsync(string endpoint, CancellationToken ct)
    {
        await using var observer = await GameClient.ConnectAsync(
            endpoint,
            "observer-" + Guid.NewGuid().ToString("N"),
            ct
        );
        try
        {
            var observed = await observer.JoinAsync(300, ct);
            await using var source = await GameClient.ConnectAsync(
                endpoint,
                "border-" + Guid.NewGuid().ToString("N"),
                ct
            );
            try
            {
                var origin = await source.JoinAsync(299, ct);
                Require(
                    observed.NodeId == "zoneB" && origin.NodeId == "zoneA",
                    "Border must cross physical nodes."
                );
                var visible = observer
                    .Connector.WaitFor<BorderVisible>()
                    .Where(message =>
                        message.Payload.ActorId == source.PlayerId && message.Payload.Sequence == 1
                    )
                    .Timeout(TimeSpan.FromSeconds(10))
                    .Async(ct);
                await source.MoveAsync(298, 1, ct);
                var border = (await visible).Payload;
                Require(
                    border.ObserverZoneId == "zone-4"
                        && border.SourceZoneId == "zone-3"
                        && border.X == 298,
                    "The adjacent zone did not observe the authoritative border update."
                );
            }
            catch (Exception failureBeforeCleanup)
            {
                Console.Error.WriteLine("Failure before cleanup: " + failureBeforeCleanup);
                throw;
            }
        }
        catch (Exception failureBeforeCleanup)
        {
            Console.Error.WriteLine("Failure before cleanup: " + failureBeforeCleanup);
            throw;
        }
    }

    private static async Task ReconnectAsync(string endpoint, CancellationToken ct)
    {
        var id = "reconnect-" + Guid.NewGuid().ToString("N");
        WorldState before;
        string session;
        await using (var client = await GameClient.ConnectAsync(endpoint, id, ct))
        {
            try
            {
                await client.JoinAsync(299, ct);
                before = await client.MoveAsync(300, 1, ct);
                session = await client.SessionAsync(ct);
            }
            catch (Exception failureBeforeCleanup)
            {
                Console.Error.WriteLine("Failure before cleanup: " + failureBeforeCleanup);
                throw;
            }
        }
        await using var reconnect = await GameClient.ConnectAsync(endpoint, id, ct);
        try
        {
            var after = await reconnect.JoinAsync(0, ct);
            SameActor(before, after);
            Require(
                before.X == after.X
                    && before.Sequence == after.Sequence
                    && before.ZoneId == after.ZoneId,
                "Reconnect overwrote authoritative world state."
            );
            Require(
                session != await reconnect.SessionAsync(ct),
                "Reconnect reused the old physical session."
            );
            var moved = await reconnect.MoveAsync(301, 2, ct);
            SameActor(before, moved);
        }
        catch (Exception failureBeforeCleanup)
        {
            Console.Error.WriteLine("Failure before cleanup: " + failureBeforeCleanup);
            throw;
        }
    }

    private static async Task UpdateAsync(string endpoint, WorldRun run, CancellationToken ct)
    {
        await using var client = await GameClient.ConnectAsync(
            endpoint,
            "update-" + Guid.NewGuid().ToString("N"),
            ct
        );
        try
        {
            var before = await client.JoinAsync(299, ct);
            var session = await client.SessionAsync(ct);
            Require(before.NodeId == "zoneA", "Update did not start on Zone node A.");
            var first = await client.MoveAsync(298, 1, ct);
            SameActor(before, first);
            async Task ObserveDrainAsync()
            {
                await run.ZoneA.WaitLineAsync(NodeProtocol.RelocationRequested, ct);
                await run.ZoneA.WaitLineAsync(NodeProtocol.Relocated, ct);
                await run.ZoneA.WaitLineAsync(NodeProtocol.SafeToShutdown, ct);
            }
            await run.ZoneA.CommandAsync(NodeProtocol.VacateZone);
            await run.ZoneA.WaitLineAsync(NodeProtocol.ZoneVacated, ct);
            var safe = ObserveDrainAsync();
            async Task<(WorldState State, int Sequence)> ContinuePlayingAsync()
            {
                var currentSequence = 1;
                WorldState currentState;
                do
                {
                    currentSequence++;
                    currentState = await client.MoveAsync(
                        currentSequence % 2 == 0 ? 299 : 298,
                        currentSequence,
                        ct
                    );
                    SameActor(before, currentState);
                    Require(
                        currentState.Sequence == currentSequence && !client.Disconnected,
                        "Drain lost movement or disconnected the session."
                    );
                } while (!safe.IsCompleted || currentState.NodeId != "zoneB");
                return (currentState, currentSequence);
            }
            var gameplay = ContinuePlayingAsync();
            await run.ZoneA.CommandAsync(NodeProtocol.Drain);
            await Task.WhenAll(safe, gameplay);
            var (state, sequence) = await gameplay;
            Require(state.ZoneId == "zone-3", "Drain changed the logical zone.");
            await run.ReplaceZoneAAsync(ct);
            var after = await client.MoveAsync(299, sequence + 1, ct);
            SameActor(before, after);
            Require(
                after.NodeId == "zoneB"
                    && session == await client.SessionAsync(ct)
                    && !client.Disconnected,
                "Replacement interrupted the relocated player."
            );
            var dungeon = await client.DungeonAsync(
                "after-update-" + Guid.NewGuid().ToString("N"),
                ct
            );
            Require(dungeon.NodeId == "instance", "Dungeon access failed after replacement.");
            await using var fresh = await GameClient.ConnectAsync(
                endpoint,
                "replacement-" + Guid.NewGuid().ToString("N"),
                ct
            );
            try
            {
                var placed = await fresh.JoinAsync(NodeProtocol.ReplacementX, ct);
                Require(placed.NodeId == before.NodeId, "Replacement did not accept the new Zone.");
                var identity = await fresh.SessionAsync(ct);
                var moved = await fresh.MoveAsync(NodeProtocol.ReplacementX + 1, 1, ct);
                Require(moved.NodeId == before.NodeId, "Replacement movement failed.");
                Require(
                    identity == await fresh.SessionAsync(ct) && !fresh.Disconnected,
                    "Replacement changed SessionInfo."
                );
            }
            catch (Exception failureBeforeCleanup)
            {
                Console.Error.WriteLine("Failure before cleanup: " + failureBeforeCleanup);
                throw;
            }
        }
        catch (Exception failureBeforeCleanup)
        {
            Console.Error.WriteLine("Failure before cleanup: " + failureBeforeCleanup);
            throw;
        }
    }
}
