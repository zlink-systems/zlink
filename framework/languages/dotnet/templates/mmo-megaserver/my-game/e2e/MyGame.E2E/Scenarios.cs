using MyGame.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace MyGame.E2E;

internal static class Scenarios
{
    public static readonly string[] All = ["region", "movement", "dungeon", "reconnect", "update"];
    private const string Forest = "forest";
    private const int FirstMovedX = GameNames.MaxStep;
    private const int MovedX = GameNames.MaxStep * 2;

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
            await using var initialP1 = await GameClient.ConnectAsync(
                run.Outcome,
                endpoint,
                "p1",
                ct
            );
            var p1 = initialP1;
            var initial = await p1.JoinAsync(Forest, ct);
            var p2 = await GameClient.ConnectAsync(run.Outcome, endpoint, "p2", ct);
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
                await using var p3 = await GameClient.ConnectAsync(run.Outcome, endpoint, "p3", ct);
                var separate = await p3.JoinAsync(Forest, ct);
                Require(
                    separate.RegionId == GameNames.RegionId(Forest, 2)
                        && separate.Players.Select(p => p.PlayerId).SequenceEqual(new[] { "p3" }),
                    "Capacity overflow did not isolate p3."
                );
                var isolatedState = separate;
                if (selected.Contains("region"))
                {
                    await using var p4 = await GameClient.ConnectAsync(run.Outcome, endpoint, "p4", ct);
                    var secondTogether = p3.ReceiveAsync<RegionView>(
                        view => view.Players.Length == 2,
                        ct
                    );
                    var separateFull = await p4.JoinAsync(Forest, ct);
                    SameState(separateFull, await secondTogether);
                    isolatedState = separateFull;
                    Require(
                        separateFull.RegionId == GameNames.RegionId(Forest, 2)
                            && separateFull.Players.Select(p => p.PlayerId)
                                .SequenceEqual(new[] { "p3", "p4" }),
                        "Second Region membership mismatch."
                    );

                    await using var p5 = await GameClient.ConnectAsync(run.Outcome, endpoint, "p5", ct);
                    var exhausted = await p5.SendAsync<RegionUnavailable, EnterRegionRequest>(
                        new(p5.PlayerId, Forest),
                        result => result.Region == Forest,
                        ct
                    );
                    Require(
                        exhausted.Reason == Rejection.CapacityExhausted
                            && !p5.Disconnected
                            && (await p5.SessionAsync(ct)).ActorId == p5.PlayerId,
                        "Capacity exhaustion did not keep the Session connected."
                    );

                    await using var p6 = await GameClient.ConnectAsync(run.Outcome, endpoint, "p6", ct);
                    const string unknownRegion = "unknown";
                    var unknown = await p6.SendAsync<RegionUnavailable, EnterRegionRequest>(
                        new(p6.PlayerId, unknownRegion),
                        result => result.Region == unknownRegion,
                        ct
                    );
                    var denied = await p6.SendAsync<ActionRejected, RegionFull>(
                        new(Rejection.RegionFull, Forest, 1),
                        result => result.Reason == Rejection.PacketNotAllowed,
                        ct
                    );
                    Require(
                        unknown.Reason == Rejection.RegionUnknown
                            && denied == new ActionRejected(Rejection.PacketNotAllowed, 0)
                            && !p6.Disconnected
                            && (await p6.SessionAsync(ct)).ActorId == p6.PlayerId,
                        "Unknown Region or client packet rejection changed the Session."
                    );

                    Console.WriteLine("✓ region");
                    passed++;
                }

                var before1 = await p1.SessionAsync(ct);
                var before2 = await p2.SessionAsync(ct);
                var expectedState = region;
                if (selected.Contains("movement"))
                {
                    current = "movement";
                    var leaked = new TaskCompletionSource<RegionView>(
                        TaskCreationOptions.RunContinuationsAsynchronously
                    );
                    using (
                        var isolationSubscription = p3.Connector.On<RegionView>(
                            (message, _) =>
                            {
                                if (
                                    message.Payload.Players.Any(p =>
                                        p.PlayerId is not ("p3" or "p4")
                                    )
                                )
                                    leaked.TrySetResult(message.Payload);
                                return ValueTask.CompletedTask;
                            }
                        )
                    )
                    {
                        var firstReply = new TaskCompletionSource<RegionView>(
                            TaskCreationOptions.RunContinuationsAsynchronously
                        );
                        var firstReplyCount = 0;
                        var firstBroadcast = p2.ReceiveAsync<RegionView>(
                            view =>
                                view.Players.Any(p =>
                                    p.PlayerId == "p1" && p.X == FirstMovedX
                                ),
                            ct
                        );
                        RegionView firstMove;
                        ActionRejected rejected;
                        using (
                            p1.Connector.On<RegionView>(
                                (message, _) =>
                                {
                                    if (
                                        message.Payload.Players.Any(p =>
                                            p.PlayerId == p1.PlayerId && p.X == FirstMovedX
                                        )
                                    )
                                    {
                                        Interlocked.Increment(ref firstReplyCount);
                                        firstReply.TrySetResult(message.Payload);
                                    }
                                    return ValueTask.CompletedTask;
                                }
                            )
                        )
                        {
                            await p1.Connector.Send(new MoveRequest(GameNames.MaxStep, 1)).Async(ct);
                            var budgetRejected = p1.ReceiveAsync<ActionRejected>(
                                result => result.Reason == Rejection.MoveBudget,
                                ct
                            );
                            await p1.Connector.Send(new MoveRequest(GameNames.MaxStep, 2)).Async(ct);
                            firstMove = await firstReply.Task.WaitAsync(TimeSpan.FromSeconds(10), ct);
                            SameState(firstMove, await firstBroadcast);
                            rejected = await budgetRejected;
                            Require(
                                firstReplyCount == 1,
                                "Caller received a duplicate movement result."
                            );
                        }
                        Require(
                            rejected.RetryAfterMs > 0,
                            "Movement budget rejection omitted its retry time."
                        );
                        SameState(firstMove, await p1.RegionAsync(ct));
                        await Task.Delay(TimeSpan.FromMilliseconds(rejected.RetryAfterMs), ct);
                        var secondReply = new TaskCompletionSource<RegionView>(
                            TaskCreationOptions.RunContinuationsAsynchronously
                        );
                        var secondReplyCount = 0;
                        var secondBroadcast = p2.ReceiveAsync<RegionView>(
                            view => view.Players.Any(p => p.PlayerId == "p1" && p.X == MovedX),
                            ct
                        );
                        using (
                            p1.Connector.On<RegionView>(
                                (message, _) =>
                                {
                                    if (
                                        message.Payload.Players.Any(p =>
                                            p.PlayerId == p1.PlayerId && p.X == MovedX
                                        )
                                    )
                                    {
                                        Interlocked.Increment(ref secondReplyCount);
                                        secondReply.TrySetResult(message.Payload);
                                    }
                                    return ValueTask.CompletedTask;
                                }
                            )
                        )
                        {
                            await p1.Connector.Send(new MoveRequest(GameNames.MaxStep, 2)).Async(ct);
                            expectedState = await secondReply.Task.WaitAsync(
                                TimeSpan.FromSeconds(10),
                                ct
                            );
                        }
                        SameState(expectedState, await secondBroadcast);
                        Require(
                            secondReplyCount == 1,
                            "Caller received a duplicate accepted result."
                        );
                    }
                    SameState(isolatedState, await p3.RegionAsync(ct));
                    Require(!leaked.Task.IsCompleted, "p3 received another region's movement.");
                }
                if (selected.Contains("movement"))
                {
                    Console.WriteLine("✓ movement");
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
                    p1 = await GameClient.ConnectAsync(run.Outcome, endpoint, "p1", ct);
                    var restored = await p1.JoinAsync(Forest, ct);
                    SameState(expectedState, restored);
                    Require(
                        restored.NodeId == expectedState.NodeId,
                        "Reconnect changed the region host."
                    );
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
                    var source = run.Regions[expectedState.NodeId];
                    await source.CommandAsync(NodeProtocol.Drain);
                    await source.WaitLineAsync(NodeProtocol.RelocationRequested, ct);
                    await source.WaitLineAsync(NodeProtocol.Relocated, ct);
                    await source.WaitLineAsync(NodeProtocol.SafeToShutdown, ct);
                    var relocated = await p1.RegionAsync(
                        ct,
                        view => view.NodeId != expectedState.NodeId
                    );
                    SameState(expectedState, relocated);
                    var other = await p2.RegionAsync(ct, view => view.NodeId == relocated.NodeId);
                    SameState(relocated, other);
                    Require(
                        relocated.NodeId != expectedState.NodeId
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
                    await run.ReplaceRegionAsync(expectedState.NodeId, ct);
                    await using var fresh = await GameClient.ConnectAsync(
                        run.Outcome,
                        endpoint,
                        "fresh-p7",
                        ct
                    );
                    var created = await fresh.JoinAsync("desert", ct);
                    Require(
                        created.NodeId == expectedState.NodeId
                            && created.Players.Any(p => p.PlayerId == "fresh-p7"),
                        "New region was not accepted by the replacement Region node."
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
