using System.Text.Json;
using MyGame.Shared;

namespace MyGame.E2E;

internal static class Scenarios
{
    private static readonly string[] Order = ["hit", "reconnect", "update"];
    internal static IReadOnlyList<string> All => Order;
    private const double NearX = 10;
    private const double TargetX = 20;
    private const double WallBehindX = 60;
    private const double FarX = 200;
    private const double LaneY = 10;

    public static string[] Select(string? requested)
    {
        var selected = requested?.Split(',', StringSplitOptions.TrimEntries) ?? Order;
        if (
            selected.Length == 0
            || selected.Distinct().Count() != selected.Length
            || selected.Any(s => !Order.Contains(s))
        )
            throw new ArgumentException("Select distinct scenarios from hit,reconnect,update.");
        return Order.Where(selected.Contains).ToArray();
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
            throw new InvalidOperationException(message);
    }

    private static bool Equal<T>(T a, T b) =>
        JsonSerializer.Serialize(a) == JsonSerializer.Serialize(b);

    private static void SameState(RoomView before, RoomView after) =>
        Require(Equal(before with { NodeId = after.NodeId }, after), "Room state changed.");

    public static async Task RunAsync(
        string root,
        string[] selected,
        List<MatchRun> runs,
        CancellationToken ct
    )
    {
        var passed = 0;
        try
        {
            foreach (var scenario in selected)
            {
                try
                {
                    await using (var run = new MatchRun(root))
                    {
                        runs.Add(run);
                        await run.StartAsync(ct);
                        await PlayAsync(run.Endpoint, scenario, run, ct);
                    }
                    Console.WriteLine($"✓ {scenario}");
                    passed++;
                }
                catch
                {
                    Console.WriteLine($"✗ {scenario}");
                    throw;
                }
            }
        }
        finally
        {
            Console.WriteLine($"Scenarios: {passed}/{selected.Length} passed");
        }
    }

    private static async Task PlayAsync(
        string endpoint,
        string scenario,
        MatchRun run,
        CancellationToken ct
    )
    {
        var suffix = Guid.NewGuid().ToString("N");
        await using var p1 = await GameClient.ConnectAsync(
            run.Outcome,
            endpoint,
            "p1-" + suffix,
            ct
        );
        await using var p2 = await GameClient.ConnectAsync(
            run.Outcome,
            endpoint,
            "p2-" + suffix,
            ct
        );
        await p1.QueueAsync(ct);
        await p2.QueueAsync(ct);
        var found1 = await p1.WaitAsync<MatchFound>(_ => true, ct);
        var found2 = await p2.WaitAsync<MatchFound>(_ => true, ct);
        Require(found1.RoomId == found2.RoomId, "Players entered different rooms.");
        var ready1 = await p1.WaitAsync<RoomView>(
            v => v.Players.Length == GameNames.PlayersPerRoom,
            ct
        );
        var ready2 = await p2.WaitAsync<RoomView>(
            v => v.Players.Length == GameNames.PlayersPerRoom,
            ct
        );
        Require(
            Equal(ready1, ready2) && ready1.Players.All(p => p.Hp == GameNames.InitialHp),
            "Initial room state differs."
        );
        var session1 = await p1.SessionAsync(ct);
        var session2 = await p2.SessionAsync(ct);
        await MoveAsync(p1, p2, p1, NearX, LaneY, 1, ct);
        await MoveAsync(p1, p2, p2, TargetX, LaneY, 1, ct);
        var before = await HitAsync(p1, p2, 2, GameNames.InitialHp - GameNames.ShotDamage, ct);
        if (scenario == "hit")
        {
            await MoveAsync(p1, p2, p2, WallBehindX, LaneY, 2, ct);
            await RejectShotAsync(p1, p2, 3, Rejection.BlockedByWall, ct);
            await MoveAsync(p1, p2, p2, FarX, LaneY, 3, ct);
            await RejectShotAsync(p1, p2, 4, Rejection.OutOfRange, ct);
            await RejectShotAsync(p1, p2, 5, Rejection.NoTarget, ct, "missing-player");
            await MoveAsync(p1, p2, p2, TargetX, LaneY, 4, ct);
            for (var sequence = 6L; sequence <= 8; sequence++)
                await HitAsync(
                    p1,
                    p2,
                    sequence,
                    GameNames.InitialHp - GameNames.ShotDamage * (int)(sequence - 4),
                    ct
                );
            Require(
                (await p1.WaitAsync<MatchEnded>(_ => true, ct)).Winner == p1.PlayerId
                    && (await p2.WaitAsync<MatchEnded>(_ => true, ct)).Winner == p1.PlayerId,
                "Wrong match winner."
            );
        }
        else if (scenario == "reconnect")
        {
            await p2.DisposeAsync();
            await using var again = await GameClient.ConnectAsync(
                run.Outcome,
                endpoint,
                p2.PlayerId,
                ct
            );
            await again.QueueAsync(ct);
            var restored = await again.WaitAsync<RoomView>(_ => true, ct);
            var identity = await again.SessionAsync(ct);
            Require(
                identity.SessionId != session2.SessionId
                    && identity.ActorId == session2.ActorId
                    && identity.ObjectGeneration == session2.ObjectGeneration,
                "Reconnect changed Actor identity."
            );
            SameState(before, restored);
            await HitAsync(p1, again, 3, GameNames.InitialHp - GameNames.ShotDamage * 2, ct);
        }
        else
        {
            await run.DrainAsync(before.NodeId, ct);
            Require(
                await p1.SessionAsync(ct) == session1 && await p2.SessionAsync(ct) == session2,
                "Update changed session binding."
            );
            var after1 = await p1.ViewAsync(v => v.NodeId != before.NodeId, ct);
            var after2 = await p2.ViewAsync(v => v.NodeId != before.NodeId, ct);
            SameState(before, after1);
            SameState(before, after2);
            await run.ReplaceAsync(before.NodeId, ct);
            // New actors exercise the replacement process's entry and admission responsibilities.
            await NewMatchAsync(endpoint, run, before.NodeId, ct);
            await HitAsync(p1, p2, 3, GameNames.InitialHp - GameNames.ShotDamage * 2, ct);
            Require(
                await p1.SessionAsync(ct) == session1 && await p2.SessionAsync(ct) == session2,
                "Replacement changed session binding."
            );
        }
        Require(!p1.Disconnected && p1.Connector.IsConnected, "First player disconnected.");
        if (scenario != "reconnect")
            Require(!p2.Disconnected && p2.Connector.IsConnected, "Second player disconnected.");
    }

    private static async Task NewMatchAsync(
        string endpoint,
        MatchRun run,
        string replacementId,
        CancellationToken ct
    )
    {
        var suffix = Guid.NewGuid().ToString("N");
        await using var a = await GameClient.ConnectAsync(
            run.Outcome,
            endpoint,
            "new-a-" + suffix,
            ct
        );
        await using var b = await GameClient.ConnectAsync(
            run.Outcome,
            endpoint,
            "new-b-" + suffix,
            ct
        );
        await a.QueueAsync(ct);
        await b.QueueAsync(ct);
        var first = await a.WaitAsync<MatchFound>(_ => true, ct);
        var second = await b.WaitAsync<MatchFound>(_ => true, ct);
        Require(first.RoomId == second.RoomId, "Replacement matchmaking failed.");
        var room = await a.WaitAsync<RoomView>(
            v => v.Players.Length == GameNames.PlayersPerRoom,
            ct
        );
        await b.WaitAsync<RoomView>(v => v.Players.Length == GameNames.PlayersPerRoom, ct);
        Require(room.NodeId == replacementId, "Replacement node did not accept the new room.");
    }

    private static async Task MoveAsync(
        GameClient a,
        GameClient b,
        GameClient mover,
        double x,
        double y,
        long sequence,
        CancellationToken ct
    )
    {
        bool Moved(RoomView v) =>
            v.Players.Any(p => p.PlayerId == mover.PlayerId && p.X == x && p.Y == y);
        var va = a.WaitAsync<RoomView>(Moved, ct);
        var vb = b.WaitAsync<RoomView>(Moved, ct);
        await mover.SendAsync(new MoveRequest(x, y, sequence), ct);
        Require(Equal(await va, await vb), "Movement views differ.");
    }

    private static async Task<RoomView> HitAsync(
        GameClient a,
        GameClient b,
        long sequence,
        int hp,
        CancellationToken ct
    )
    {
        bool Hit(RoomView v) => v.Players.Any(p => p.PlayerId == b.PlayerId && p.Hp == hp);
        var va = a.WaitAsync<RoomView>(Hit, ct);
        var vb = b.WaitAsync<RoomView>(Hit, ct);
        await a.SendAsync(new ShootRequest(b.PlayerId, sequence), ct);
        var view = await va;
        Require(Equal(view, await vb), "Hit views differ.");
        return view;
    }

    private static async Task RejectShotAsync(
        GameClient a,
        GameClient b,
        long sequence,
        string reason,
        CancellationToken ct,
        string? target = null
    )
    {
        var before = await b.ViewAsync(_ => true, ct);
        var rejected = a.WaitAsync<ShotRejected>(_ => true, ct);
        await a.SendAsync(new ShootRequest(target ?? b.PlayerId, sequence), ct);
        Require((await rejected).Reason == reason, "Wrong shot rejection.");
        SameState(before, await b.ViewAsync(_ => true, ct));
    }
}
