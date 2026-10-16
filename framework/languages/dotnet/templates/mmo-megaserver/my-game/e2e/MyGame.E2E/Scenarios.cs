using MyGame.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace MyGame.E2E;

internal static class Scenarios
{
    public static readonly string[] All = ["region", "dungeon", "reconnect", "update"];
    private const string Forest = "forest";
    private const int MovedX = 42;

    public static string[] Select(string? value)
    {
        var selected = value is null
            ? All
            : value.Split(
                ',',
                StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries
            );
        if (
            selected.Length == 0
            || selected.Distinct().Count() != selected.Length
            || selected.Any(id => !All.Contains(id))
        )
            throw new ArgumentException("Select unique scenarios from: " + string.Join(',', All));
        return All.Where(selected.Contains).ToArray();
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
            throw new InvalidDataException(message);
    }

    private static void SameState(RegionView before, RegionView after) =>
        Require(
            before.RegionId == after.RegionId && before.Players.SequenceEqual(after.Players),
            "Region state changed."
        );

    public static async Task RunAsync(
        string endpoint,
        WorldRun run,
        string[] selected,
        CancellationToken ct
    )
    {
        var passed = 0;
        var current = selected[0];
        try
        {
            await using var initialP1 = await GameClient.ConnectAsync(endpoint, "p1", ct);
            var p1 = initialP1;
            var initial = await p1.JoinAsync(Forest, ct);
            var p2 = await GameClient.ConnectAsync(endpoint, "p2", ct);
            try
            {
                var together = p1.ReceiveAsync<RegionView>(view => view.Players.Length == 2, ct);
                var region = await p2.JoinAsync(Forest, ct);
                SameState(region, await together);
                Require(
                    initial.RegionId == GameNames.RegionId(Forest, 1)
                        && region.RegionId == initial.RegionId
                        && region
                            .Players.Select(p => p.PlayerId)
                            .SequenceEqual(new[] { "p1", "p2" }),
                    "First region membership mismatch."
                );
                await using var p3 = await GameClient.ConnectAsync(endpoint, "p3", ct);
                var separate = await p3.JoinAsync(Forest, ct);
                Require(
                    separate.RegionId == GameNames.RegionId(Forest, 2)
                        && separate.Players.Select(p => p.PlayerId).SequenceEqual(new[] { "p3" }),
                    "Capacity overflow did not isolate p3."
                );
                var before1 = await p1.SessionAsync(ct);
                var before2 = await p2.SessionAsync(ct);
                var leaked = new TaskCompletionSource<RegionView>(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
                RegionView moved;
                using (
                    var subscription = p3.Connector.On<RegionView>(
                        (message, _) =>
                        {
                            if (message.Payload.Players.Any(p => p.PlayerId != "p3"))
                                leaked.TrySetResult(message.Payload);
                            return ValueTask.CompletedTask;
                        }
                    )
                )
                {
                    var observed = p2.ReceiveAsync<RegionView>(
                        view => view.Players.Any(p => p.PlayerId == "p1" && p.X == MovedX),
                        ct
                    );
                    moved = await p1.SendAsync<RegionView, MoveRequest>(
                        new(MovedX, 1),
                        view => view.Players.Any(p => p.PlayerId == "p1" && p.X == MovedX),
                        ct
                    );
                    SameState(moved, await observed);
                }
                SameState(separate, await p3.RegionAsync(ct));
                Require(!leaked.Task.IsCompleted, "p3 received another region's movement.");
                if (selected.Contains("region"))
                {
                    Console.WriteLine("✓ region");
                    passed++;
                }
                if (selected.Contains("dungeon"))
                {
                    current = "dungeon";
                    const string dungeonId = "dungeon-1";
                    var opened = await p1.SendAsync<DungeonOpened, OpenDungeonRequest>(
                        new(dungeonId),
                        _ => true,
                        ct
                    );
                    Require(
                        opened.DungeonId == dungeonId && opened.NodeId == "instance",
                        "Dungeon host mismatch."
                    );
                    Console.WriteLine("✓ dungeon");
                    passed++;
                }
                if (selected.Contains("reconnect"))
                {
                    current = "reconnect";
                    await p1.DisposeAsync();
                    p1 = await GameClient.ConnectAsync(endpoint, "p1", ct);
                    var restored = await p1.JoinAsync(Forest, ct);
                    SameState(moved, restored);
                    Require(restored.NodeId == moved.NodeId, "Reconnect changed the region host.");
                    var after = await p1.SessionAsync(ct);
                    Require(
                        before1.ActorId == after.ActorId
                            && before1.ObjectGeneration == after.ObjectGeneration,
                        "Reconnect changed the Actor identity."
                    );
                    before1 = after;
                    Console.WriteLine("✓ reconnect");
                    passed++;
                }
                if (selected.Contains("update"))
                {
                    current = "update";
                    var source = run.Regions[moved.NodeId];
                    await source.CommandAsync(NodeProtocol.Drain);
                    await source.WaitLineAsync(NodeProtocol.RelocationRequested, ct);
                    await source.WaitLineAsync(NodeProtocol.Relocated, ct);
                    await source.WaitLineAsync(NodeProtocol.SafeToShutdown, ct);
                    var relocated = await p1.RegionAsync(ct, view => view.NodeId != moved.NodeId);
                    SameState(moved, relocated);
                    var other = await p2.RegionAsync(ct, view => view.NodeId == relocated.NodeId);
                    SameState(relocated, other);
                    Require(
                        relocated.NodeId != moved.NodeId
                            && other.NodeId == relocated.NodeId
                            && run.Regions.ContainsKey(relocated.NodeId),
                        "Region did not move to the surviving host."
                    );
                    Require(
                        before1 == await p1.SessionAsync(ct)
                            && before2 == await p2.SessionAsync(ct)
                            && !p1.Disconnected
                            && !p2.Disconnected,
                        "Host relocation changed the Sessions."
                    );
                    await run.ReplaceRegionAsync(moved.NodeId, ct);
                    await using var fresh = await GameClient.ConnectAsync(endpoint, "p4", ct);
                    var created = await fresh.JoinAsync("desert", ct);
                    Require(
                        run.Regions.ContainsKey(created.NodeId)
                            && created.Players.Any(p => p.PlayerId == "p4"),
                        "New region was not accepted by a live Region node."
                    );
                    Console.WriteLine("✓ update");
                    passed++;
                }
            }
            finally
            {
                try
                {
                    await p2.DisposeAsync();
                }
                finally
                {
                    if (!ReferenceEquals(p1, initialP1))
                        await p1.DisposeAsync();
                }
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
}
