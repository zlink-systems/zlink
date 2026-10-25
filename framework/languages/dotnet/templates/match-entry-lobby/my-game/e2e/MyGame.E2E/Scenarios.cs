using System.Text.Json;
using MyGame.Shared;

namespace MyGame.E2E;

internal static class Scenarios
{
    private static readonly string[] Order = ["hit", "wall", "input", "reconnect", "update"];
    internal static IReadOnlyList<string> All => Order;
    private const double TargetX = GameNames.SecondSpawnX;
    private const double WallCenterY = (GameNames.WallMinY + GameNames.WallMaxY) / 2;
    private const double FarX = GameNames.MapMaxX;
    private const double FarY = GameNames.MapMaxY - 10;
    private const double LaneY = GameNames.FirstSpawnY;

    public static string[] Select(string? requested)
    {
        var selected = requested?.Split(',', StringSplitOptions.TrimEntries) ?? Order;
        if (
            selected.Length == 0
            || selected.Distinct().Count() != selected.Length
            || selected.Any(s => !Order.Contains(s))
        )
            throw new ArgumentException("Select distinct scenarios from hit,wall,input,reconnect,update.");
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
            Equal(ready1, ready2)
                && ready1.Players.All(p => p.Hp == GameNames.InitialHp && p.LastSequence == 0)
                && ready1.Phase == GameNames.PlayingPhase
                && ready1.Winner is null,
            "Initial room state differs."
        );
        if (scenario == "input")
        {
            await InputAsync(p1, p2, ct);
            Require(!p1.Disconnected && p1.Connector.IsConnected, "First player disconnected.");
            Require(!p2.Disconnected && p2.Connector.IsConnected, "Second player disconnected.");
            return;
        }
        var session1 = await p1.SessionAsync(ct);
        var session2 = await p2.SessionAsync(ct);
        await MoveByAsync(p1, p2, p1, 0, 0, 1, ct);
        await MoveByAsync(p1, p2, p2, 0, 0, 1, ct);
        var before = await HitAsync(p1, p2, 2, GameNames.InitialHp - GameNames.ShotDamage, ct);
        if (scenario == "hit")
        {
            await RejectActionAsync(
                p1,
                new RoomAssignment(found1.RoomId),
                Rejection.PacketNotAllowed,
                ct
            );
            await WalkAsync(p1, p2, p1, GameNames.FirstSpawnX, GameNames.FirstSpawnY, ct);
            await WalkAsync(p1, p2, p2, TargetX, WallCenterY, ct);
            await RejectShotAsync(
                p1,
                p2,
                await NextSequenceAsync(p1, ct),
                Rejection.BlockedByWall,
                ct
            );
            await WalkAsync(p1, p2, p2, FarX, FarY, ct);
            await RejectShotAsync(
                p1,
                p2,
                await NextSequenceAsync(p1, ct),
                Rejection.OutOfRange,
                ct
            );
            await RejectShotAsync(
                p1,
                p2,
                await NextSequenceAsync(p1, ct),
                Rejection.NoTarget,
                ct,
                "missing-player"
            );
            await WalkAsync(p1, p2, p2, TargetX, LaneY, ct);
            for (var hp = GameNames.InitialHp - 2 * GameNames.ShotDamage; hp >= 0; hp -= GameNames.ShotDamage)
                await HitAsync(
                    p1,
                    p2,
                    await NextSequenceAsync(p1, ct),
                    hp,
                    ct
                );
            Require(
                (await p1.WaitAsync<MatchEnded>(_ => true, ct)).Winner == p1.PlayerId
                    && (await p2.WaitAsync<MatchEnded>(_ => true, ct)).Winner == p1.PlayerId,
                "Wrong match winner."
            );
        }
        else if (scenario == "wall")
        {
            var left = GameNames.WallX - GameNames.MaxStep;
            var right = GameNames.WallX + GameNames.MaxStep;
            await WalkAsync(p1, p2, p1, left, WallCenterY, ct);
            await WalkAsync(p1, p2, p2, right, WallCenterY, ct);
            var beforeWall = await p1.ViewAsync(_ => true, ct);
            var targetBefore = beforeWall.Players.Single(player => player.PlayerId == p2.PlayerId);
            await RejectActionAsync(
                p1,
                new MoveRequest(GameNames.MaxStep, 0, await NextSequenceAsync(p1, ct)),
                Rejection.MoveBlockedByWall,
                ct
            );
            await RejectShotAsync(
                p1,
                p2,
                await NextSequenceAsync(p1, ct),
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
            var clearShot = await HitAsync(
                p1,
                p2,
                await NextSequenceAsync(p1, ct),
                expectedHp,
                ct
            );
            Require(
                clearShot.Players.Single(player => player.PlayerId == p2.PlayerId).Hp == expectedHp,
                "Wall behind target blocked a clear shot."
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
            Require(
                restored.Phase == GameNames.PlayingPhase
                    && restored.Winner is null
                    && restored.Players.Single(player => player.PlayerId == again.PlayerId).LastSequence == 1,
                "Reconnect did not restore the active phase and last sequence."
            );
            await MoveByAsync(again, p1, again, 0, 0, 2, ct);
            await HitAsync(p1, again, 3, GameNames.InitialHp - GameNames.ShotDamage * 2, ct);
            await HitAsync(p1, again, 4, GameNames.InitialHp - GameNames.ShotDamage * 3, ct);
            var finished = await HitAsync(p1, again, 5, 0, ct);
            Require(
                finished.Phase == GameNames.FinishedPhase && finished.Winner == p1.PlayerId,
                "Room view did not restore the finished phase and winner."
            );
            await using var finishedReconnect = await GameClient.ConnectAsync(
                run.Outcome,
                endpoint,
                again.PlayerId,
                ct
            );
            await finishedReconnect.QueueAsync(ct);
            var restoredFinished = await finishedReconnect.WaitAsync<RoomView>(
                view => view.Phase == GameNames.FinishedPhase,
                ct
            );
            var finishedIdentity = await finishedReconnect.SessionAsync(ct);
            Require(
                finishedIdentity.SessionId != identity.SessionId
                    && finishedIdentity.ActorId == identity.ActorId
                    && finishedIdentity.ObjectGeneration == identity.ObjectGeneration,
                "Finished reconnect changed Actor identity."
            );
            SameState(finished, restoredFinished);
            Require(
                restoredFinished.Winner == p1.PlayerId
                    && restoredFinished.Players.Single(player => player.PlayerId == again.PlayerId).LastSequence == 2,
                "Finished reconnect did not restore the winner and last sequence."
            );
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

    private static async Task InputAsync(GameClient p1, GameClient p2, CancellationToken ct)
    {
        await HitAsync(p1, p2, 1, GameNames.InitialHp - GameNames.ShotDamage, ct);
        var fireDelay = await RejectActionAsync(
            p1,
            new ShootRequest(p2.PlayerId, 2),
            Rejection.FireTooSoon,
            ct
        );
        await Task.Delay(TimeSpan.FromMilliseconds(fireDelay.RetryAfterMs), ct);
        await HitAsync(
            p1,
            p2,
            2,
            GameNames.InitialHp - GameNames.ShotDamage * 2,
            ct,
            allowFireRetry: false
        );

        var initial = await p1.ViewAsync(_ => true, ct);
        var firstSpawn = initial.Players.Single(
            player => player.X == GameNames.FirstSpawnX && player.Y == GameNames.FirstSpawnY
        );
        var mover = firstSpawn.PlayerId == p1.PlayerId ? p1 : p2;
        var movementStart = initial
            .Players.Single(player => player.PlayerId == mover.PlayerId)
            .LastSequence + 1;
        var budgetSequence = movementStart + 1;
        var firstMove = await MoveByAsync(p1, p2, mover, 1, 0, movementStart, ct);
        await MoveByAsync(
            p1,
            p2,
            mover,
            2,
            0,
            budgetSequence,
            ct,
            firstMove,
            requireMoveBudget: true
        );
        await RejectActionAsync(
            mover,
            new MoveRequest(0, 0, budgetSequence),
            Rejection.StaleSequence,
            ct
        );
        await RejectActionAsync(
            mover,
            new MoveRequest(0, 0, movementStart),
            Rejection.StaleSequence,
            ct
        );
        var stepTooLargeSequence = budgetSequence + 1;
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

    private static async Task<long> NextSequenceAsync(GameClient player, CancellationToken ct)
    {
        var view = await player.ViewAsync(_ => true, ct);
        return view.Players.Single(candidate => candidate.PlayerId == player.PlayerId).LastSequence + 1;
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
        RoomView? priorView = null,
        bool requireMoveBudget = false
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
            var resultTask = mover.WaitForActionResultAsync(Moved, ct);
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
                await Task.Delay(TimeSpan.FromMilliseconds(rejection.RetryAfterMs), ct);
                continue;
            }

            var moverView = (RoomView)result;
            var other = mover.PlayerId == a.PlayerId ? b : a;
            var otherView = await other.WaitAsync<RoomView>(Moved, ct);
            Require(Equal(moverView, otherView), "Movement views differ.");
            Require(!requireMoveBudget || moveBudgetRejected, "Move budget rejection was not observed.");
            return moverView;
        }
    }

    private static async Task<RoomView> HitAsync(
        GameClient a,
        GameClient b,
        long sequence,
        int hp,
        CancellationToken ct,
        bool allowFireRetry = true
    )
    {
        bool Hit(RoomView view) =>
            view.Players.Any(player => player.PlayerId == b.PlayerId && player.Hp == hp)
            && (hp != 0 || (view.Phase == GameNames.FinishedPhase && view.Winner == a.PlayerId));
        var before = await a.ViewAsync(_ => true, ct);
        var request = new ShootRequest(b.PlayerId, sequence);
        var fireTooSoonRejected = false;
        while (true)
        {
            a.ClearActionResults();
            var resultTask = a.WaitForActionResultAsync(Hit, ct);
            await a.SendAsync(request, ct);
            var result = await resultTask;
            if (result is ActionRejected rejection)
            {
                Require(!fireTooSoonRejected, "Shot was rejected again after RetryAfterMs.");
                fireTooSoonRejected = true;
                Require(
                    rejection.Reason == Rejection.FireTooSoon && rejection.RetryAfterMs > 0,
                    $"Unexpected shot rejection: {rejection.Reason}."
                );
                SameState(before, await a.ViewAsync(_ => true, ct));
                Require(allowFireRetry, "Shot was rejected after the explicit RetryAfterMs wait.");
                await Task.Delay(TimeSpan.FromMilliseconds(rejection.RetryAfterMs), ct);
                before = await a.ViewAsync(_ => true, ct);
                continue;
            }

            var view = (RoomView)result;
            var peerView = await b.WaitAsync<RoomView>(Hit, ct);
            Require(Equal(view, peerView), "Hit views differ.");
            return view;
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
        await RejectActionAsync(
            a,
            new ShootRequest(target ?? b.PlayerId, sequence),
            reason,
            ct
        );
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
