using System.Diagnostics;
using System.Net.Http.Json;
using System.Text.Json;
using MyGame.Shared;

namespace MyGame.E2E;

internal static class Scenarios
{
    private static readonly string[] Order = ["lobby", "hit", "wall", "input", "reconnect", "update"];
    private const double LaneY = GameNames.FirstSpawnY;
    private const double WallCenterY = (GameNames.WallMinY + GameNames.WallMaxY) / 2;
    internal static IReadOnlyList<string> All => Order;

    public static string[] Select(string? requested)
    {
        var selected = requested?.Split(',', StringSplitOptions.TrimEntries) ?? Order;
        if (
            selected.Length == 0
            || selected.Distinct().Count() != selected.Length
            || selected.Any(s => !Order.Contains(s))
        )
            throw new ArgumentException(
                "Select distinct scenarios from lobby,hit,wall,input,reconnect,update."
            );
        return Order.Where(selected.Contains).ToArray();
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
            throw new InvalidOperationException(message);
    }

    private static async Task WaitRetryAsync(int retryAfterMs, CancellationToken ct)
    {
        Require(retryAfterMs > 0, "Time-limited rejection omitted RetryAfterMs.");
        var started = Stopwatch.GetTimestamp();
        var duration = TimeSpan.FromMilliseconds(retryAfterMs);
        while (true)
        {
            var remaining = duration - Stopwatch.GetElapsedTime(started);
            if (remaining <= TimeSpan.Zero)
                return;
            await Task.Delay((int)Math.Ceiling(remaining.TotalMilliseconds), ct);
        }
    }

    private static bool Equal<T>(T a, T b) =>
        JsonSerializer.Serialize(a) == JsonSerializer.Serialize(b);

    private static void SameState(RoomView before, RoomView after) =>
        Require(Equal(before with { NodeId = after.NodeId }, after), "Room state changed.");

    private static bool HasRoomIdFormat(string roomId) =>
        roomId.StartsWith(GameNames.RoomPrefix, StringComparison.Ordinal)
        && Guid.TryParseExact(roomId[GameNames.RoomPrefix.Length..], "N", out _);

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
        var player1 = "p1-" + suffix;
        var player2 = "p2-" + suffix;
        var assignment = await MatchPairAsync(run.LobbyEndpoint, player1, player2, ct);
        Require(HasRoomIdFormat(assignment.RoomId), "Lobby returned an invalid Room ID.");
        Require(assignment.SessionEndpoint == endpoint, "Lobby returned a different Session endpoint.");

        if (scenario == "lobby")
            await VerifyLobbyAsync(run, assignment, player1, player2, suffix, ct);

        await using var p1 = await GameClient.ConnectAsync(
            run.Outcome,
            assignment.SessionEndpoint,
            player1,
            ct
        );
        await using var p2 = await GameClient.ConnectAsync(
            run.Outcome,
            assignment.SessionEndpoint,
            player2,
            ct
        );
        await p1.JoinAsync(assignment.RoomId, ct);
        await p2.JoinAsync(assignment.RoomId, ct);
        var ready1 = await p1.WaitViewAsync(
            view => view.Players.Length == GameNames.PlayersPerRoom,
            ct
        );
        var ready2 = await p2.WaitViewAsync(
            view => view.Players.Length == GameNames.PlayersPerRoom,
            ct
        );
        Require(Equal(ready1, ready2) && ready1.Players.All(p => p.Hp == GameNames.InitialHp),
            "Initial room state differs.");
        Require(ready1.RoomId == assignment.RoomId, "STREAM joined a different room.");
        Require(ready1.Phase == GameNames.PlayingPhase && ready1.Winner is null,
            "Room phase did not start in Playing.");
        Require(ready1.Players.All(p => p.LastSequence == 0), "Initial action sequence differs.");
        var spawnPositions = ready1.Players.Select(player => (player.X, player.Y)).ToArray();
        Require(
            spawnPositions.Contains((GameNames.FirstSpawnX, GameNames.FirstSpawnY))
                && spawnPositions.Contains((GameNames.SecondSpawnX, GameNames.SecondSpawnY))
                && ready1.Players.Any(player => player.X < GameNames.WallX)
                && ready1.Players.Any(player => player.X > GameNames.WallX),
            "Players did not spawn at the server-selected points on opposite wall sides."
        );
        var spawnDistance = Distance(ready1.Players[0], ready1.Players[1]);
        Require(spawnDistance <= GameNames.ShotRange, "Players spawned outside shot range.");

        if (scenario == "lobby")
        {
            await VerifyClientPacketAllowlistAsync(p1, p2, assignment.RoomId, ct);
            Require(
                p1.Connector.IsConnected
                    && p2.Connector.IsConnected
                    && !p1.Disconnected
                    && !p2.Disconnected,
                "Lobby clients disconnected."
            );
            await VerifyLobbyRestartAsync(run, assignment, suffix, ct);
            return;
        }

        var session1 = await p1.SessionAsync(ct);
        var session2 = await p2.SessionAsync(ct);
        if (scenario == "hit")
        {
            await PlayHitAsync(p1, p2, ready1, ct);
        }
        else if (scenario == "wall")
        {
            await PlayWallAsync(p1, p2, ct);
        }
        else if (scenario == "input")
        {
            await PlayInputAsync(p1, p2, ct);
        }
        else if (scenario == "reconnect")
        {
            await PlayReconnectAsync(p1, p2, run, endpoint, assignment.RoomId, session2, ct);
        }
        else
        {
            var moved = await MoveByAsync(p1, p2, p1, 1, 0, 1, ct);
            var before = await HitAsync(
                p1,
                p2,
                2,
                GameNames.InitialHp - GameNames.ShotDamage,
                ct
            );
            Require(before.Players.Single(p => p.PlayerId == p1.PlayerId).LastSequence == 2,
                "Update snapshot omitted the accepted action sequence.");
            Require(moved.Phase == GameNames.PlayingPhase && moved.Winner is null,
                "Update input changed match phase.");
            await run.DrainAsync(before.NodeId, ct);
            Require(
                await p1.SessionAsync(ct) == session1 && await p2.SessionAsync(ct) == session2,
                "Update changed session binding."
            );
            var after1 = await p1.ViewAsync(view => view.NodeId != before.NodeId, ct);
            var after2 = await p2.ViewAsync(view => view.NodeId != before.NodeId, ct);
            SameState(before, after1);
            SameState(before, after2);
            await run.ReplaceAsync(before.NodeId, ct);
            await NewMatchAsync(run, before.NodeId, assignment.RoomId, ct);
            await HitAsync(
                p1,
                p2,
                3,
                GameNames.InitialHp - GameNames.ShotDamage * 2,
                ct
            );
            Require(
                await p1.SessionAsync(ct) == session1 && await p2.SessionAsync(ct) == session2,
                "Replacement changed session binding."
            );
        }

        Require(!p1.Disconnected && p1.Connector.IsConnected, "First player disconnected.");
        if (scenario != "reconnect")
            Require(!p2.Disconnected && p2.Connector.IsConnected, "Second player disconnected.");
    }

    private static double Distance(PlayerView a, PlayerView b)
    {
        var dx = a.X - b.X;
        var dy = a.Y - b.Y;
        return Math.Sqrt(dx * dx + dy * dy);
    }

    private static async Task VerifyLobbyAsync(
        MatchRun run,
        MatchResponse assignment,
        string player1,
        string player2,
        string suffix,
        CancellationToken ct
    )
    {
        var outsiderId = "outsider-" + suffix;
        await using var outsider = await GameClient.ConnectAsync(
            run.Outcome,
            assignment.SessionEndpoint,
            outsiderId,
            ct
        );
        var rejectionTask = outsider.WaitActionAsync(_ => true, ct);
        await outsider.JoinAsync(assignment.RoomId, ct);
        var result = await rejectionTask;
        Require(
            result is ActionRejected { Reason: Rejection.AdmissionRejected, RetryAfterMs: 0 },
            "Unlisted player was not rejected from the prepared Room."
        );
    }

    private static async Task VerifyClientPacketAllowlistAsync(
        GameClient player,
        GameClient observer,
        string roomId,
        CancellationToken ct
    )
    {
        var before = await observer.ViewAsync(_ => true, ct);
        var response = player.WaitActionAsync(_ => true, ct);
        await player.SendAsync(
            new PrepareRoom(roomId, [player.PlayerId, observer.PlayerId]),
            ct
        );
        var result = await response;
        Require(
            result is ActionRejected { Reason: Rejection.PacketNotAllowed, RetryAfterMs: 0 },
            "Client packet allowlist accepted internal PrepareRoom."
        );
        SameState(before, await observer.ViewAsync(_ => true, ct));
    }

    private static async Task VerifyLobbyRestartAsync(
        MatchRun run,
        MatchResponse previous,
        string suffix,
        CancellationToken ct
    )
    {
        await run.RestartLobbyAsync(ct);
        var firstId = "restart-a-" + suffix;
        var secondId = "restart-b-" + suffix;
        var next = await MatchPairAsync(run.LobbyEndpoint, firstId, secondId, ct);
        Require(next.RoomId != previous.RoomId, "Lobby restart reused a Room ID.");
        await using var first = await GameClient.ConnectAsync(
            run.Outcome,
            next.SessionEndpoint,
            firstId,
            ct
        );
        await using var second = await GameClient.ConnectAsync(
            run.Outcome,
            next.SessionEndpoint,
            secondId,
            ct
        );
        await first.JoinAsync(next.RoomId, ct);
        await second.JoinAsync(next.RoomId, ct);
        var view = await first.WaitViewAsync(
            item => item.Players.Length == GameNames.PlayersPerRoom,
            ct
        );
        await second.WaitViewAsync(item => item.Players.Length == GameNames.PlayersPerRoom, ct);
        Require(view.RoomId == next.RoomId && view.RoomId != previous.RoomId,
            "New lobby assignment did not create a new prepared Room.");
    }

    private static async Task PlayHitAsync(
        GameClient p1,
        GameClient p2,
        RoomView initial,
        CancellationToken ct
    )
    {
        var p2Spawn = initial.Players.Single(player => player.PlayerId == p2.PlayerId);
        await HitAsync(p1, p2, 1, GameNames.InitialHp - GameNames.ShotDamage, ct);
        await WalkAsync(
            p1,
            p2,
            p2,
            GameNames.WallX - GameNames.MaxStep / 2,
            GameNames.FirstSpawnY + GameNames.ShotRange,
            ct
        );
        await RejectShotAsync(p1, p2, 2, Rejection.OutOfRange, ct);
        await RejectShotAsync(p1, p2, 2, Rejection.NoTarget, ct, "missing-player");
        await WalkAsync(p1, p2, p2, p2Spawn.X, p2Spawn.Y, ct);

        for (var sequence = 2L; sequence <= 4; sequence++)
            await HitAsync(
                p1,
                p2,
                sequence,
                GameNames.InitialHp - GameNames.ShotDamage * (int)sequence,
                ct
            );

        var ended1 = await p1.WaitAsync<MatchEnded>(_ => true, ct);
        var ended2 = await p2.WaitAsync<MatchEnded>(_ => true, ct);
        Require(ended1.Winner == p1.PlayerId && ended2.Winner == p1.PlayerId, "Wrong match winner.");
        var final = await p2.ViewAsync(view => view.Phase == GameNames.FinishedPhase, ct);
        Require(final.Winner == p1.PlayerId, "RoomView omitted the winner.");
        Require(final.Players.Single(player => player.PlayerId == p1.PlayerId).LastSequence == 4,
            "RoomView omitted the accepted action sequence.");
    }

    private static async Task PlayWallAsync(GameClient p1, GameClient p2, CancellationToken ct)
    {
        var left = GameNames.WallX - GameNames.MaxStep;
        var wallY = WallCenterY;
        await WalkAsync(p1, p2, p1, left, wallY, ct);
        await WalkAsync(p1, p2, p2, GameNames.WallX + GameNames.MaxStep, wallY, ct);

        var beforeWall = await p1.ViewAsync(_ => true, ct);
        var targetBefore = beforeWall.Players.Single(player => player.PlayerId == p2.PlayerId);
        var nextSequence = beforeWall.Players.Single(player => player.PlayerId == p1.PlayerId).LastSequence + 1;
        await RejectMoveAsync(
            p1,
            p2,
            new MoveRequest(GameNames.MaxStep, 0, nextSequence),
            Rejection.MoveBlockedByWall,
            ct
        );
        await RejectShotAsync(
            p1,
            p2,
            nextSequence,
            Rejection.BlockedByWall,
            ct
        );

        var afterWall = await p1.ViewAsync(_ => true, ct);
        SameState(beforeWall, afterWall);
        Require(
            afterWall.Players.Single(player => player.PlayerId == p2.PlayerId).Hp == targetBefore.Hp,
            "Wall-blocked shot changed HP."
        );

        await WalkAsync(p1, p2, p1, GameNames.FirstSpawnX, GameNames.FirstSpawnY, ct);
        await WalkAsync(p1, p2, p2, GameNames.SecondSpawnX, GameNames.SecondSpawnY, ct);
        var beforeClearShot = await p1.ViewAsync(_ => true, ct);
        var expectedHp = beforeClearShot.Players.Single(player => player.PlayerId == p2.PlayerId).Hp
            - GameNames.ShotDamage;
        var shotSequence = beforeClearShot.Players.Single(player => player.PlayerId == p1.PlayerId).LastSequence + 1;
        var clearShot = await HitAsync(
            p1,
            p2,
            shotSequence,
            expectedHp,
            ct
        );
        Require(
            clearShot.Players.Single(player => player.PlayerId == p2.PlayerId).Hp == expectedHp,
            "Wall behind target blocked a clear shot."
        );
    }

    private static async Task PlayInputAsync(GameClient p1, GameClient p2, CancellationToken ct)
    {
        var beforeMove = await p1.ViewAsync(_ => true, ct);
        var firstSpawn = beforeMove.Players.Single(
            player => player.X == GameNames.FirstSpawnX && player.Y == GameNames.FirstSpawnY
        );
        var mover = firstSpawn.PlayerId == p1.PlayerId ? p1 : p2;
        var other = mover.PlayerId == p1.PlayerId ? p2 : p1;
        var source = beforeMove.Players.Single(player => player.PlayerId == mover.PlayerId);
        var target = beforeMove.Players.Single(player => player.PlayerId == other.PlayerId);
        var moveDelta = target.X >= source.X ? GameNames.MaxStep : -GameNames.MaxStep;
        var moveSequence = source.LastSequence + 1;
        var acceptedMove = new MoveRequest(moveDelta, 0, moveSequence);
        var rejectedMove = new MoveRequest(moveDelta, 0, moveSequence + 1);
        var acceptedMoveTask = mover.WaitAsync<MoveResult>(result => result.Sequence == moveSequence, ct);
        var rejectedMoveTask = mover.WaitAsync<ActionRejected>(_ => true, ct);
        await mover.SendAsync(acceptedMove, ct);
        await mover.SendAsync(rejectedMove, ct);
        var moveResult = await acceptedMoveTask;
        var moveRejection = await rejectedMoveTask;
        Require(moveResult.Sequence == moveSequence, "Expected accepted movement before movement-budget rejection.");
        Require(
            moveRejection.Reason == Rejection.MoveBudget && moveRejection.RetryAfterMs > 0,
            "Expected movement-budget rejection."
        );
        var afterMoveA = await mover.ViewAsync(_ => true, ct);
        var afterMovePlayer = afterMoveA.Players.Single(player => player.PlayerId == mover.PlayerId);
        Require(
            afterMovePlayer.X == moveResult.X
                && afterMovePlayer.Y == moveResult.Y
                && afterMovePlayer.LastSequence == moveSequence
                && afterMoveA.Players.Single(player => player.PlayerId == other.PlayerId).Hp == target.Hp,
            "Movement-budget rejection changed the accepted movement state."
        );
        await WaitRetryAsync(moveRejection.RetryAfterMs, ct);
        await mover.SendAsync(rejectedMove, ct);
        var retriedMove = await mover.WaitAsync<MoveResult>(result => result.Sequence == moveSequence + 1, ct);
        Require(retriedMove.Sequence == rejectedMove.Sequence, "Server movement retry delay was insufficient.");

        var beforeShot = await p1.ViewAsync(_ => true, ct);
        var shotSequence = beforeShot.Players.Single(player => player.PlayerId == p1.PlayerId).LastSequence + 1;
        var shotTarget = beforeShot.Players.Single(player => player.PlayerId == p2.PlayerId);
        var acceptedShot = new ShootRequest(p2.PlayerId, shotSequence);
        var rejectedShot = new ShootRequest(p2.PlayerId, shotSequence + 1);
        var acceptedShotTask = p1.WaitAsync<ShotResult>(result => result.Sequence == shotSequence, ct);
        var rejectedShotTask = p1.WaitAsync<ActionRejected>(_ => true, ct);
        await p1.SendAsync(acceptedShot, ct);
        await p1.SendAsync(rejectedShot, ct);
        var shotResult = await acceptedShotTask;
        var fireRejection = await rejectedShotTask;
        Require(shotResult.Sequence == shotSequence, "Expected accepted shot before fire-too-soon rejection.");
        Require(
            fireRejection.Reason == Rejection.FireTooSoon && fireRejection.RetryAfterMs > 0,
            "Expected fire-interval rejection."
        );
        var afterShotA = await p1.ViewAsync(_ => true, ct);
        Require(
            afterShotA.Players.Single(player => player.PlayerId == p2.PlayerId).Hp
                == shotTarget.Hp - GameNames.ShotDamage
                && afterShotA.Players.Single(player => player.PlayerId == p1.PlayerId).LastSequence == shotSequence,
            "Fire-too-soon rejection changed the accepted shot state."
        );
        await WaitRetryAsync(fireRejection.RetryAfterMs, ct);
        await p1.SendAsync(rejectedShot, ct);
        var retriedShot = await p1.WaitAsync<ShotResult>(result => result.Sequence == shotSequence + 1, ct);
        Require(retriedShot.Sequence == rejectedShot.Sequence, "Server fire retry delay was insufficient.");
        var afterShotB = await p1.ViewAsync(_ => true, ct);
        Require(
            afterShotB.Players.Single(player => player.PlayerId == p2.PlayerId).Hp
                == shotTarget.Hp - 2 * GameNames.ShotDamage,
            "Retried shot did not update the target state."
        );

        var staleSequence = (await mover.ViewAsync(_ => true, ct))
            .Players.Single(player => player.PlayerId == mover.PlayerId)
            .LastSequence;
        await RejectActionAsync(
            mover,
            new MoveRequest(0, 0, staleSequence),
            Rejection.StaleSequence,
            ct
        );
        await RejectActionAsync(
            mover,
            new MoveRequest(0, 0, moveSequence),
            Rejection.StaleSequence,
            ct
        );
        var stepTooLargeSequence = staleSequence + 1;
        await RejectActionAsync(
            mover,
            new MoveRequest(GameNames.MaxStep + 0.01, 0, stepTooLargeSequence),
            Rejection.StepTooLarge,
            ct
        );
        await RejectActionAsync(
            mover,
            new MoveRequest(0, 0, GameNames.MaxSafeInteger + 1),
            Rejection.IntegerOutOfRange,
            ct
        );

        var outsideStart = await WalkAsync(p1, p2, mover, 1, LaneY, ct);
        var outsideSequence = outsideStart
            .Players.Single(player => player.PlayerId == mover.PlayerId)
            .LastSequence + 1;
        await RejectActionAsync(
            mover,
            new MoveRequest(-GameNames.MaxStep, 0, outsideSequence),
            Rejection.OutsideMap,
            ct
        );

        var beforeWall = await WalkAsync(
            p1,
            p2,
            mover,
            GameNames.WallX - GameNames.MaxStep,
            WallCenterY,
            ct
        );
        var wallSequence = beforeWall
            .Players.Single(player => player.PlayerId == mover.PlayerId)
            .LastSequence + 1;
        await RejectActionAsync(
            mover,
            new MoveRequest(GameNames.MaxStep, 0, wallSequence),
            Rejection.MoveBlockedByWall,
            ct
        );

        await MoveByAsync(p1, p2, mover, -0.1, 0, GameNames.MaxSafeInteger, ct);
        var atMaximum = await mover.ViewAsync(_ => true, ct);
        Require(
            atMaximum.Players.Single(player => player.PlayerId == mover.PlayerId).LastSequence
                == GameNames.MaxSafeInteger,
            "Maximum safe sequence was not accepted."
        );
        await RejectActionAsync(
            mover,
            new MoveRequest(0, 0, GameNames.MaxSafeInteger + 1),
            Rejection.IntegerOutOfRange,
            ct
        );
    }


    private static async Task PlayReconnectAsync(
        GameClient p1,
        GameClient p2,
        MatchRun run,
        string endpoint,
        string roomId,
        SessionInfo previousIdentity,
        CancellationToken ct
    )
    {
        RoomView before = null!;
        for (var sequence = 1L; sequence <= 4; sequence++)
            before = await HitAsync(
                p1,
                p2,
                sequence,
                GameNames.InitialHp - GameNames.ShotDamage * (int)sequence,
                ct
            );

        await p2.DisposeAsync();
        await using var again = await GameClient.ConnectAsync(
            run.Outcome,
            endpoint,
            p2.PlayerId,
            ct
        );
        await again.JoinAsync(roomId, ct);
        var restored = await again.WaitViewAsync(view => view.Phase == GameNames.FinishedPhase, ct);
        var identity = await again.SessionAsync(ct);
        Require(
            identity.SessionId != previousIdentity.SessionId
                && identity.ActorId == previousIdentity.ActorId
                && identity.ObjectGeneration == previousIdentity.ObjectGeneration,
            "Reconnect changed Actor identity."
        );
        SameState(before, restored);
        Require(
            restored.Winner == p1.PlayerId
                && restored.Phase == GameNames.FinishedPhase
                && restored.Players.Single(player => player.PlayerId == p1.PlayerId).LastSequence == 4,
            "Reconnect did not restore sequence, phase and winner."
        );
    }

    private static async Task<MatchResponse> MatchPairAsync(
        string lobbyEndpoint,
        string first,
        string second,
        CancellationToken ct
    )
    {
        using var http = new HttpClient
        {
            BaseAddress = new Uri(lobbyEndpoint),
            Timeout = GameClient.ResponseTimeout,
        };
        async Task<MatchResponse> MatchAsync(string player)
        {
            using var response = await http.PostAsJsonAsync(
                GameNames.MatchPath,
                new { PlayerId = player },
                ct
            );
            response.EnsureSuccessStatusCode();
            return await response.Content.ReadFromJsonAsync<MatchResponse>(ct)
                ?? throw new InvalidDataException("Missing HTTP assignment.");
        }
        var a = MatchAsync(first);
        var b = MatchAsync(second);
        var assignment = await a;
        Require(assignment == await b, "HTTP players received different assignments.");
        return assignment;
    }

    private static async Task NewMatchAsync(
        MatchRun run,
        string replacementId,
        string previousRoomId,
        CancellationToken ct
    )
    {
        var suffix = Guid.NewGuid().ToString("N");
        var player1 = "new-a-" + suffix;
        var player2 = "new-b-" + suffix;
        var assignment = await MatchPairAsync(run.LobbyEndpoint, player1, player2, ct);
        Require(assignment.RoomId != previousRoomId, "Matchmaker reused a Room ID.");
        await using var a = await GameClient.ConnectAsync(
            run.Outcome,
            assignment.SessionEndpoint,
            player1,
            ct
        );
        await using var b = await GameClient.ConnectAsync(
            run.Outcome,
            assignment.SessionEndpoint,
            player2,
            ct
        );
        await a.JoinAsync(assignment.RoomId, ct);
        await b.JoinAsync(assignment.RoomId, ct);
        var room = await a.WaitViewAsync(
            view => view.Players.Length == GameNames.PlayersPerRoom,
            ct
        );
        await b.WaitViewAsync(view => view.Players.Length == GameNames.PlayersPerRoom, ct);
        Require(
            room.RoomId == assignment.RoomId && room.NodeId == replacementId,
            "New Room is not on the replacement match node."
        );
    }

    private static async Task RejectMoveAsync(
        GameClient actor,
        GameClient observer,
        MoveRequest request,
        string reason,
        CancellationToken ct
    )
    {
        var before = await observer.ViewAsync(_ => true, ct);
        var response = actor.WaitActionAsync(_ => true, ct);
        await actor.SendAsync(request, ct);
        var result = await response;
        Require(result is ActionRejected rejected && rejected.Reason == reason,
            "Wrong movement rejection.");
        var rejection = (ActionRejected)result;
        Require(
            reason == Rejection.MoveBudget
                ? rejection.RetryAfterMs > 0
                : rejection.RetryAfterMs == 0,
            "Movement rejection returned an invalid RetryAfterMs."
        );
        SameState(before, await observer.ViewAsync(_ => true, ct));
    }

    private static async Task<RoomView> HitAsync(
        GameClient a,
        GameClient b,
        long sequence,
        int hp,
        CancellationToken ct
    )
    {
        bool Hit(RoomView view) =>
            view.Players.Any(player => player.PlayerId == b.PlayerId && player.Hp == hp)
            && view.Players.Single(player => player.PlayerId == a.PlayerId).LastSequence == sequence;

        while (true)
        {
            var before = await b.ViewAsync(_ => true, ct);
            var response = a.WaitActionAsync(Hit, ct);
            await a.SendAsync(new ShootRequest(b.PlayerId, sequence), ct);
            var result = await response;
            if (result is RoomView hit)
            {
                var observed = await b.WaitViewAsync(Hit, ct);
                Require(Equal(hit, observed), "Hit views differ.");
                return hit;
            }

            var rejected = (ActionRejected)result;
            SameState(before, await b.ViewAsync(_ => true, ct));
            if (rejected.Reason != Rejection.FireTooSoon)
                throw new InvalidOperationException($"Shot rejected: {rejected.Reason}.");
            Require(rejected.RetryAfterMs > 0, "Fire cooldown rejection omitted RetryAfterMs.");
            await WaitRetryAsync(rejected.RetryAfterMs, ct);
        }
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
        var response = a.WaitActionAsync(_ => true, ct);
        await a.SendAsync(new ShootRequest(target ?? b.PlayerId, sequence), ct);
        var result = await response;
        Require(result is ActionRejected rejected && rejected.Reason == reason,
            "Wrong shot rejection.");
        Require(((ActionRejected)result).RetryAfterMs == 0,
            "Non-cooldown shot rejection returned RetryAfterMs.");
        SameState(before, await b.ViewAsync(_ => true, ct));
    }
    private static async Task<RoomView> WalkAsync(
        GameClient a,
        GameClient b,
        GameClient mover,
        double destinationX,
        double destinationY,
        CancellationToken ct
    )
    {
        var view = await mover.ViewAsync(_ => true, ct);
        var player = view.Players.Single(candidate => candidate.PlayerId == mover.PlayerId);
        while (player.X != destinationX || player.Y != destinationY)
        {
            var remainingX = destinationX - player.X;
            var dx = remainingX == 0
                ? 0
                : Math.Clamp(remainingX, -GameNames.MaxStep, GameNames.MaxStep);
            var remainingY = destinationY - player.Y;
            var dy = dx != 0 || remainingY == 0
                ? 0
                : Math.Clamp(remainingY, -GameNames.MaxStep, GameNames.MaxStep);
            view = await MoveByAsync(
                a,
                b,
                mover,
                dx,
                dy,
                player.LastSequence + 1,
                ct,
                view
            );
            player = view.Players.Single(candidate => candidate.PlayerId == mover.PlayerId);
        }
        return view;
    }

    private static async Task<RoomView> MoveByAsync(
        GameClient a,
        GameClient b,
        GameClient mover,
        double dx,
        double dy,
        long sequence,
        CancellationToken ct,
        RoomView? priorView = null
    )
    {
        var before = priorView ?? await mover.ViewAsync(_ => true, ct);
        var player = before.Players.Single(view => view.PlayerId == mover.PlayerId);
        var targetX = player.X + dx;
        var targetY = player.Y + dy;
        bool Moved(RoomView view) => view.Players.Any(
            candidate =>
                candidate.PlayerId == mover.PlayerId
                && candidate.X == targetX
                && candidate.Y == targetY
                && candidate.LastSequence == sequence
        );
        var request = new MoveRequest(dx, dy, sequence);
        var moveBudgetRejected = false;
        while (true)
        {
            mover.ClearActionResults();
            var resultTask = mover.WaitActionAsync(Moved, ct);
            await mover.SendAsync(request, ct);
            var result = await resultTask;
            if (result is ActionRejected rejection)
            {
                Require(!moveBudgetRejected, "Move was rejected again after RetryAfterMs.");
                moveBudgetRejected = true;
                Require(
                    rejection.Reason == Rejection.MoveBudget && rejection.RetryAfterMs > 0,
                    $"Unexpected movement rejection: {rejection.Reason}."
                );
                SameState(before, await mover.ViewAsync(_ => true, ct));
                await WaitRetryAsync(rejection.RetryAfterMs, ct);
                continue;
            }

            var moverView = (RoomView)result;
            var other = mover.PlayerId == a.PlayerId ? b : a;
            var otherView = await other.WaitAsync<RoomView>(Moved, ct);
            Require(Equal(moverView, otherView), "Movement views differ.");
            return moverView;
        }
    }

    private static async Task<ActionRejected> RejectActionAsync<T>(
        GameClient player,
        T request,
        string reason,
        CancellationToken ct
    )
    {
        var before = await player.ViewAsync(_ => true, ct);
        player.ClearActionResults();
        var rejected = player.WaitAsync<ActionRejected>(_ => true, ct);
        await player.SendAsync(request, ct);
        var response = await rejected;
        Require(response.Reason == reason, $"Expected {reason}; received {response.Reason}.");
        var timeLimited = reason is Rejection.MoveBudget or Rejection.FireTooSoon;
        Require(
            timeLimited ? response.RetryAfterMs > 0 : response.RetryAfterMs == 0,
            $"Unexpected RetryAfterMs for {reason}."
        );
        SameState(before, await player.ViewAsync(_ => true, ct));
        return response;
    }
}
