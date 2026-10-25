using System.Text.Json;
using System.Net.Http.Json;
using MyGame.Shared;

namespace MyGame.E2E;

internal static class Scenarios
{
    private static readonly string[] Order = ["lobby", "hit", "wall", "input", "reconnect", "update"];
    internal static IReadOnlyList<string> All => Order;

    public static string[] Select(string? requested)
    {
        var selected = requested?.Split(',', StringSplitOptions.TrimEntries) ?? Order;
        if (selected.Length == 0 || selected.Distinct().Count() != selected.Length
            || selected.Any(s => !Order.Contains(s)))
            throw new ArgumentException("Select distinct scenarios from " + string.Join(',', Order));
        return Order.Where(selected.Contains).ToArray();
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private static bool Equal<T>(T a, T b) => JsonSerializer.Serialize(a) == JsonSerializer.Serialize(b);
    private static void SameState(RoomView before, RoomView after) =>
        Require(Equal(before with { NodeId = after.NodeId }, after), "Room state changed.");
    private static PlayerView Player(RoomView view, GameClient client) =>
        view.Players.Single(p => p.PlayerId == client.PlayerId);
    private static Task<RoomView> ViewAsync(GameClient client, CancellationToken ct) =>
        client.ViewAsync(_ => true, ct);

    public static async Task RunAsync(string root, string[] selected, List<MatchRun> runs,
        CancellationToken ct)
    {
        var passed = 0;
        try
        {
            foreach (var scenario in selected)
            {
                try
                {
                    await using (var run = new MatchRun(root, scenario == "lobby" ? MatchRun.LobbyRoomCapacity : MatchRun.DefaultRoomCapacity))
                    {
                        runs.Add(run);
                        await run.StartAsync(ct);
                        await PlayAsync(run.Endpoint, scenario, run, ct);
                    }
                    Console.WriteLine($"✓ {scenario}");
                    passed++;
                }
                catch { Console.WriteLine($"✗ {scenario}"); throw; }
            }
        }
        finally { Console.WriteLine($"Scenarios: {passed}/{selected.Length} passed"); }
    }

    private static async Task PlayAsync(string endpoint, string scenario, MatchRun run, CancellationToken ct)
    {
        if (scenario == "lobby")
        {
            await LobbyAsync(endpoint, run, ct);
            return;
        }
        var suffix = Guid.NewGuid().ToString("N");
        var assignment = await MatchPairAsync(run.LobbyEndpoint, "p1-" + suffix, "p2-" + suffix, ct);
        Require(assignment.SessionEndpoint == endpoint, "Lobby returned a different Session endpoint.");
        await using var p1 = await GameClient.ConnectAsync(run.Outcome, endpoint, "p1-" + suffix, ct);
        await using var p2 = await GameClient.ConnectAsync(run.Outcome, endpoint, "p2-" + suffix, ct);
        await p1.JoinAsync(assignment.RoomId, ct);
        await p2.JoinAsync(assignment.RoomId, ct);
        var found1 = await p1.WaitAsync<MatchFound>(_ => true, ct);
        var found2 = await p2.WaitAsync<MatchFound>(_ => true, ct);
        Require(found1.RoomId == found2.RoomId, "Same-mode players entered different rooms.");
        var ready1 = await p1.WaitAsync<RoomView>(v => v.Players.Length == GameNames.PlayersPerRoom, ct);
        var ready2 = await p2.WaitAsync<RoomView>(v => v.Players.Length == GameNames.PlayersPerRoom, ct);
        Require(Equal(ready1, ready2) && ready1.Phase == MatchPhase.Playing
            && ready1.Players.All(p => p.Hp == GameNames.InitialHp && p.LastSequence == 0),
            "Initial room state differs.");
        var session1 = await p1.SessionAsync(ct);
        var session2 = await p2.SessionAsync(ct);
        switch (scenario)
        {
            case "hit":
                await HitAsync(p1, p2, GameNames.InitialHp - GameNames.ShotDamage, ct);
                await ShootAsync(p1, new Vec3(0, 1, 0), ShotOutcome.Miss, ct);
                Require(Player(await ViewAsync(p2, ct), p2).Hp == 75, "An upward miss caused damage.");
                // The upper lane avoids both walls; the target is more than 80 units away.
                await WalkAsync(p1, new Vec3(5, 0, 18), ct);
                await WalkAsync(p1, new Vec3(5, 0, 65), ct);
                await WalkAsync(p2, new Vec3(52, 0, 65), ct);
                await WalkAsync(p2, new Vec3(95, 0, 65), ct);
                await ShootAsync(p1, await AimAsync(p1, p2, ct), ShotOutcome.Miss, ct);
                Require(Player(await ViewAsync(p2, ct), p2).Hp == 75, "An out-of-range ray caused damage.");
                await WalkAsync(p1, new Vec3(20, 0, 65), ct);
                for (var hp = 50; hp >= 0; hp -= GameNames.ShotDamage)
                    await HitAsync(p1, p2, hp, ct);
                await EndedAsync(p1, p2, ct);
                await RejectedAsync(p1, new MoveRequest(new(0, 0, 1), Next(await ViewAsync(p1, ct), p1)),
                    Rejection.MatchFinished, ct);
                await RejectedAsync(p1, new ShootRequest(new(1, 0, 0), Next(await ViewAsync(p1, ct), p1)),
                    Rejection.MatchFinished, ct);
                break;
            case "wall":
                await WalkAsync(p1, new Vec3(48, 0, 22), ct);
                await WalkAsync(p2, new Vec3(52, 0, 22), ct);
                var beforeWall = await ViewAsync(p2, ct);
                await ShootAsync(p1, await AimAsync(p1, p2, ct), ShotOutcome.BlockedByWall, ct);
                Require(Player(await ViewAsync(p2, ct), p2).Hp == Player(beforeWall, p2).Hp,
                    "A wall-blocked ray caused damage.");
                await WalkAsync(p1, ArenaMap.Spawns[0], ct);
                await WalkAsync(p2, ArenaMap.Spawns[1], ct);
                await HitAsync(p1, p2, 75, ct);
                Console.WriteLine("wall: intervening wall blocks; wall behind target permits hit");
                break;
            case "input":
                await InputAsync(p1, p2, ct);
                break;
            case "reconnect":
                await WalkAsync(p2, new Vec3(54, 0, 18), ct);
                await HitAsync(p1, p2, 75, ct);
                var beforeReconnect = await ViewAsync(p2, ct);
                await p2.DisposeAsync();
                await using (var again = await GameClient.ConnectAsync(run.Outcome, endpoint, p2.PlayerId, ct))
                {
                    await again.JoinAsync(assignment.RoomId, ct);
                    SameState(beforeReconnect, await again.WaitAsync<RoomView>(_ => true, ct));
                    var identity = await again.SessionAsync(ct);
                    Require(identity.SessionId != session2.SessionId && identity.ActorId == session2.ActorId
                        && identity.ObjectGeneration == session2.ObjectGeneration,
                        "Reconnect changed Actor identity.");
                    await MoveAsync(again, new Vec3(0, 0, 1), ct);
                    for (var hp = 50; hp >= 0; hp -= GameNames.ShotDamage)
                        await HitAsync(p1, again, hp, ct);
                    await EndedAsync(p1, again, ct);
                }
                await using (var finished = await GameClient.ConnectAsync(run.Outcome, endpoint, p2.PlayerId, ct))
                {
                    await finished.JoinAsync(assignment.RoomId, ct);
                    var restored = await finished.WaitAsync<RoomView>(_ => true, ct);
                    Require(restored.Phase == MatchPhase.Finished && restored.Winner == p1.PlayerId
                        && Player(restored, finished).Hp == 0, "Finished reconnect lost winner or HP.");
                }
                break;
            case "update":
                await MoveAsync(p1, new Vec3(0, 0, -1), ct);
                await HitAsync(p1, p2, 75, ct);
                var before = await ViewAsync(p1, ct);
                await run.DrainAsync(before.NodeId, ct);
                Require(await p1.SessionAsync(ct) == session1 && await p2.SessionAsync(ct) == session2,
                    "Update changed session binding.");
                SameState(before, await p1.ViewAsync(v => v.NodeId != before.NodeId, ct));
                SameState(before, await p2.ViewAsync(v => v.NodeId != before.NodeId, ct));
                await RejectOutsiderAsync(endpoint, run, before.RoomId, ct);
                SameState(before, await ViewAsync(p1, ct));
                await run.ReplaceAsync(before.NodeId, ct);
                await NewMatchAsync(run, before.NodeId, ct);
                await HitAsync(p1, p2, 50, ct);
                Require(await p1.SessionAsync(ct) == session1 && await p2.SessionAsync(ct) == session2,
                    "Replacement changed session binding.");
                break;
        }
        Require(!p1.Disconnected && p1.Connector.IsConnected, "First player disconnected.");
        if (scenario != "reconnect")
            Require(!p2.Disconnected && p2.Connector.IsConnected, "Second player disconnected.");
    }

    private static long Next(RoomView view, GameClient client) => Player(view, client).LastSequence + 1;

    private static async Task<MatchResponse> RequestMatchAsync(string endpoint, string player,
        string? mode, CancellationToken ct)
    {
        using var http = new HttpClient { BaseAddress = new Uri(endpoint), Timeout = GameClient.ResponseTimeout };
        using var response = mode is null
            ? await http.PostAsJsonAsync(LobbyNames.MatchPath, new { PlayerId = player }, ct)
            : await http.PostAsJsonAsync(LobbyNames.MatchPath, new MatchRequest(player, mode), ct);
        response.EnsureSuccessStatusCode();
        return await response.Content.ReadFromJsonAsync<MatchResponse>(ct)
            ?? throw new InvalidDataException("Missing HTTP assignment.");
    }

    private static async Task<MatchResponse> MatchPairAsync(string endpoint, string first,
        string second, CancellationToken ct)
    {
        var a = RequestMatchAsync(endpoint, first, null, ct);
        var b = RequestMatchAsync(endpoint, second, GameNames.DefaultMode, ct);
        var assignment = await a;
        Require(assignment == await b, "HTTP players received different Room or SessionEndpoint.");
        return assignment;
    }

    private static async Task RejectOutsiderAsync(string endpoint, MatchRun run, string roomId,
        CancellationToken ct)
    {
        await using var outsider = await GameClient.ConnectAsync(run.Outcome, endpoint,
            "outsider-" + Guid.NewGuid().ToString("N"), ct);
        var rejected = await outsider.ActionAsync(new JoinRoomRequest(outsider.PlayerId, roomId), ct);
        Require(rejected is ActionRejected { Reason: Rejection.AdmissionRejected },
            "Room accepted a player absent from its prepared roster.");
    }

    private static async Task LobbyAsync(string endpoint, MatchRun run, CancellationToken ct)
    {
        var suffix = Guid.NewGuid().ToString("N");
        var defaultAId = "duel-a-" + suffix;
        var defaultBId = "duel-b-" + suffix;
        var rankedAId = "ranked-a-" + suffix;
        var rankedBId = "ranked-b-" + suffix;
        var defaultARequest = RequestMatchAsync(run.LobbyEndpoint, defaultAId, null, ct);
        var rankedARequest = RequestMatchAsync(run.LobbyEndpoint, rankedAId, "ranked", ct);
        var defaultBRequest = RequestMatchAsync(run.LobbyEndpoint, defaultBId, GameNames.DefaultMode, ct);
        var rankedBRequest = RequestMatchAsync(run.LobbyEndpoint, rankedBId, "ranked", ct);
        var duel = await defaultARequest;
        var ranked = await rankedARequest;
        Require(duel == await defaultBRequest && ranked == await rankedBRequest
            && duel.RoomId != ranked.RoomId && duel.SessionEndpoint == endpoint
            && ranked.SessionEndpoint == endpoint, "HTTP mode pairing or endpoint differs.");
        // The outsider arrives while the prepared Room is still empty, so capacity cannot mask roster defects.
        await RejectOutsiderAsync(endpoint, run, duel.RoomId, ct);
        await using var a = await GameClient.ConnectAsync(run.Outcome, endpoint, defaultAId, ct);
        await using var b = await GameClient.ConnectAsync(run.Outcome, endpoint, defaultBId, ct);
        await using var c = await GameClient.ConnectAsync(run.Outcome, endpoint, rankedAId, ct);
        await using var d = await GameClient.ConnectAsync(run.Outcome, endpoint, rankedBId, ct);
        var forged = new PrepareRoom(duel.RoomId, new[] { "forged-a", "forged-b" });
        Require(await a.ActionAsync(forged, ct) is ActionRejected { Reason: Rejection.PacketNotAllowed },
            "Session accepted a client PrepareRoom before admission.");
        await a.JoinAsync(duel.RoomId, ct); await b.JoinAsync(duel.RoomId, ct);
        await c.JoinAsync(ranked.RoomId, ct); await d.JoinAsync(ranked.RoomId, ct);
        var duelView = await a.WaitAsync<RoomView>(v => v.Players.Length == GameNames.PlayersPerRoom, ct);
        var rankedView = await c.WaitAsync<RoomView>(v => v.Players.Length == GameNames.PlayersPerRoom, ct);
        Require(duelView.Players.Select(p => p.PlayerId).ToHashSet().SetEquals(new[] { a.PlayerId, b.PlayerId })
            && rankedView.Players.Select(p => p.PlayerId).ToHashSet().SetEquals(new[] { c.PlayerId, d.PlayerId }),
            "Prepared Room roster differs from HTTP members.");
        Require(await a.ActionAsync(forged, ct) is ActionRejected { Reason: Rejection.PacketNotAllowed },
            "Session accepted a client PrepareRoom during the match.");
        SameState(duelView, await ViewAsync(a, ct));
        await MoveAsync(a, new Vec3(0, 0, -1), ct);
        await HitAsync(a, b, GameNames.InitialHp - GameNames.ShotDamage, ct);
        var beforeRestart = await ViewAsync(a, ct);
        var session = await a.SessionAsync(ct);
        await run.RestartLobbyAsync(ct);
        SameState(beforeRestart, await ViewAsync(a, ct));
        Require(await a.SessionAsync(ct) == session, "Lobby restart changed an existing Session.");
        var freshA = "restart-a-" + suffix;
        var freshB = "restart-b-" + suffix;
        var fresh = await MatchPairAsync(run.LobbyEndpoint, freshA, freshB, ct);
        Require(fresh.RoomId != duel.RoomId && fresh.RoomId != ranked.RoomId
            && fresh.SessionEndpoint == endpoint, "Lobby restart reused an existing Room.");
        await using var x = await GameClient.ConnectAsync(run.Outcome, endpoint, freshA, ct);
        await using var y = await GameClient.ConnectAsync(run.Outcome, endpoint, freshB, ct);
        await x.JoinAsync(fresh.RoomId, ct); await y.JoinAsync(fresh.RoomId, ct);
        var freshView = await x.WaitAsync<RoomView>(v => v.Players.Length == GameNames.PlayersPerRoom, ct);
        Require(freshView.RoomId == fresh.RoomId && freshView.Players.All(p => p.Hp == GameNames.InitialHp),
            "Restarted Lobby did not provision a new Room.");
        SameState(beforeRestart, await ViewAsync(a, ct));
        Require(new[] { a, b, c, d, x, y }.All(p => !p.Disconnected && p.Connector.IsConnected),
            "Lobby flow disconnected a client.");
        Console.WriteLine("lobby: default duel, mode separation, roster, PrepareRoom rejection and restart verified");
    }

    private static async Task NewMatchAsync(MatchRun run, string replacementId, CancellationToken ct)
    {
        var suffix = Guid.NewGuid().ToString("N");
        var assignment = await MatchPairAsync(run.LobbyEndpoint, "new-a-" + suffix, "new-b-" + suffix, ct);
        await using var a = await GameClient.ConnectAsync(run.Outcome, assignment.SessionEndpoint, "new-a-" + suffix, ct);
        await using var b = await GameClient.ConnectAsync(run.Outcome, assignment.SessionEndpoint, "new-b-" + suffix, ct);
        await a.JoinAsync(assignment.RoomId, ct); await b.JoinAsync(assignment.RoomId, ct);
        var room = await a.WaitAsync<RoomView>(v => v.Players.Length == GameNames.PlayersPerRoom, ct);
        await b.WaitAsync<RoomView>(v => v.Players.Length == GameNames.PlayersPerRoom, ct);
        Require(room.RoomId == assignment.RoomId && room.NodeId == replacementId,
            "Replacement node did not accept the new Room.");
    }

    private static async Task MoveAsync(GameClient client, Vec3 delta, CancellationToken ct)
    {
        var before = await ViewAsync(client, ct);
        var request = new MoveRequest(delta, Next(before, client));
        while (true)
        {
            var result = await client.ActionAsync(request, ct);
            if (result is ActionRejected { Reason: Rejection.MoveBudget, RetryAfterMs: > 0 } retry)
            {
                SameState(before, await ViewAsync(client, ct));
                await Task.Delay(retry.RetryAfterMs, ct);
                continue;
            }
            Require(result is MoveResult moved && moved.Sequence == request.Sequence
                && moved.Position == Player(before, client).Position + delta, $"Movement was not accepted: {result}; delta={delta}; before={Player(before, client)}.");
            var after = await ViewAsync(client, ct);
            Require(Player(after, client).LastSequence == request.Sequence, "Movement lost sequence.");
            Require(after.Players.All(p => p.Position.Y == 0), "Movement changed feet height.");
            return;
        }
    }

    private static async Task WalkAsync(GameClient client, Vec3 destination, CancellationToken ct)
    {
        var position = Player(await ViewAsync(client, ct), client).Position;
        while (position != destination)
        {
            var remaining = destination - position;
            // Admission order chooses spawn positions; cross X in the clear lane before changing Z.
            var delta = remaining.X != 0 ? new Vec3(Math.Clamp(remaining.X, -GameNames.MaxStep, GameNames.MaxStep), 0, 0)
                : remaining.Y != 0 ? new Vec3(0, Math.Clamp(remaining.Y, -GameNames.MaxStep, GameNames.MaxStep), 0)
                : new Vec3(0, 0, Math.Clamp(remaining.Z, -GameNames.MaxStep, GameNames.MaxStep));
            await MoveAsync(client, delta, ct);
            position = Player(await ViewAsync(client, ct), client).Position;
        }
    }

    private static async Task<Vec3> AimAsync(GameClient shooter, GameClient target, CancellationToken ct)
    {
        var view = await ViewAsync(shooter, ct);
        return Player(view, target).Position - Player(view, shooter).Position;
    }

    private static async Task ShootAsync(GameClient shooter, Vec3 aim, string outcome, CancellationToken ct)
    {
        var before = await ViewAsync(shooter, ct);
        var request = new ShootRequest(aim, Next(before, shooter));
        while (true)
        {
            var result = await shooter.ActionAsync(request, ct);
            if (result is ShotRejected { Reason: Rejection.FireTooSoon, RetryAfterMs: > 0 } retry)
            {
                SameState(before, await ViewAsync(shooter, ct));
                await Task.Delay(retry.RetryAfterMs, ct);
                continue;
            }
            Require(result is ShotResult shot && shot.Outcome == outcome && shot.Sequence == request.Sequence,
                $"Expected {outcome}, received {result}.");
            return;
        }
    }

    private static async Task HitAsync(GameClient shooter, GameClient target, int hp, CancellationToken ct)
    {
        await ShootAsync(shooter, await AimAsync(shooter, target, ct), ShotOutcome.Hit, ct);
        var a = await ViewAsync(shooter, ct);
        var b = await ViewAsync(target, ct);
        Require(Equal(a, b) && Player(a, target).Hp == hp, "Hit views or damage differ.");
    }

    private static async Task EndedAsync(GameClient a, GameClient b, CancellationToken ct)
    {
        Require((await a.WaitAsync<MatchEnded>(_ => true, ct)).Winner == a.PlayerId
            && (await b.WaitAsync<MatchEnded>(_ => true, ct)).Winner == a.PlayerId, "Wrong match winner.");
        var view = await ViewAsync(a, ct);
        Require(view.Phase == MatchPhase.Finished && view.Winner == a.PlayerId, "Finished state differs.");
    }

    private static async Task RejectedAsync<T>(GameClient client, T request, string reason, CancellationToken ct)
    {
        var before = await ViewAsync(client, ct);
        var result = await client.ActionAsync(request, ct);
        Require(result is ActionRejected { RetryAfterMs: 0 } move && move.Reason == reason
            || result is ShotRejected { RetryAfterMs: 0 } shot && shot.Reason == reason,
            $"Expected rejection {reason}, received {result}.");
        SameState(before, await ViewAsync(client, ct));
        Console.WriteLine($"input rejected: {reason}; state unchanged");
    }

    private static async Task InputAsync(GameClient a, GameClient b, CancellationToken ct)
    {
        await MoveAsync(a, new Vec3(0, 0, -1), ct);
        var view = await ViewAsync(a, ct);
        var sequence = Player(view, a).LastSequence;
        await RejectedAsync(a, new MoveRequest(new(0, 0, 1), sequence), Rejection.StaleSequence, ct);
        await RejectedAsync(a, new ShootRequest(new(1, 0, 0), sequence - 1), Rejection.StaleSequence, ct);
        await RejectedAsync(a, new MoveRequest(new(GameNames.MaxStep + 1, 0, 0), sequence + 1),
            Rejection.StepTooLarge, ct);
        await RejectedAsync(a, new MoveRequest(new(0, 1, 0), sequence + 1), Rejection.VerticalMove, ct);
        await RejectedAsync(a, new MoveRequest(new(0, -1, 0), sequence + 1), Rejection.VerticalMove, ct);
        await WalkAsync(a, new Vec3(Player(view, a).Position.X, 0, 0), ct);
        sequence = Player(await ViewAsync(a, ct), a).LastSequence;
        await RejectedAsync(a, new MoveRequest(new(0, 0, -1), sequence + 1), Rejection.OutsideMap, ct);
        await RejectedAsync(a, new ShootRequest(default, sequence + 1), Rejection.InvalidAim, ct);
        await WalkAsync(a, new Vec3(48, 0, 22), ct);
        sequence = Next(await ViewAsync(a, ct), a);
        await RejectedAsync(a, new MoveRequest(new(2, 0, 0), sequence), Rejection.MoveBlockedByWall, ct);
        await MoveAsync(a, new Vec3(0, 0, -2), ct);
        await MoveAsync(a, new Vec3(0, 0, -2), ct);
        // Back-to-back accepted actions exercise server-issued retry delays without a fixed sleep.
        await MoveAsync(a, new Vec3(0, 0, -2), ct);
        var beforeBudget = await ViewAsync(a, ct);
        var move = new MoveRequest(new(0, 0, -2), Next(beforeBudget, a));
        var budget = await a.ActionAsync(move, ct);
        Require(budget is ActionRejected { Reason: Rejection.MoveBudget, RetryAfterMs: > 0 },
            "Expected movement-budget rejection.");
        SameState(beforeBudget, await ViewAsync(a, ct));
        await Task.Delay(((ActionRejected)budget).RetryAfterMs, ct);
        Require(await a.ActionAsync(move, ct) is MoveResult, "Server movement retry delay was insufficient.");
        await ShootAsync(a, new Vec3(0, 1, 0), ShotOutcome.Miss, ct);
        var beforeFire = await ViewAsync(a, ct);
        var shot = new ShootRequest(new(0, 1, 0), Next(beforeFire, a));
        var fire = await a.ActionAsync(shot, ct);
        Require(fire is ShotRejected { Reason: Rejection.FireTooSoon, RetryAfterMs: > 0 },
            "Expected fire-interval rejection.");
        SameState(beforeFire, await ViewAsync(a, ct));
        await Task.Delay(((ShotRejected)fire).RetryAfterMs, ct);
        Require(await a.ActionAsync(shot, ct) is ShotResult { Outcome: ShotOutcome.Miss },
            "Server fire retry delay was insufficient.");
        Require(Player(await ViewAsync(b, ct), b).Hp == GameNames.InitialHp, "Input checks damaged target.");
        Console.WriteLine("input: move-budget and fire-too-soon retry delays accepted unchanged requests");
    }
}
