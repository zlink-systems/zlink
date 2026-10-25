using System.Text.Json;
using System.Text.Json.Nodes;
using MyGame.Match.Domain;
using MyGame.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace MyGame.E2E;

internal static class DomainChecks
{
    public static void Run()
    {
        var checkedCount = 0;
        void Check(bool condition, string name)
        {
            if (!condition)
                throw new InvalidOperationException("Domain boundary failed: " + name);
            checkedCount++;
        }

        static bool SameSnapshot(byte[] left, byte[] right) => left.AsSpan().SequenceEqual(right);

        static PlayerView Player(RoomState room, string playerId) =>
            room.View().Single(player => player.PlayerId == playerId);

        static RoomState NewRoom(ManualTimeProvider clock)
        {
            var room = new RoomState(clock);
            if (!room.Admit("a") || !room.Admit("b"))
                throw new InvalidOperationException("Could not create domain-check players.");
            return room;
        }

        static RoomState Positioned(
            ManualTimeProvider clock,
            double ax,
            double ay,
            double bx = 10,
            double by = 10
        )
        {
            var room = NewRoom(clock);
            var snapshot = JsonNode.Parse(room.Capture())
                ?? throw new InvalidOperationException("Room snapshot was empty.");
            var players = snapshot["Players"]?.AsArray()
                ?? throw new InvalidOperationException("Room snapshot omitted players.");
            players[0]!["X"] = ax;
            players[0]!["Y"] = ay;
            players[1]!["X"] = bx;
            players[1]!["Y"] = by;
            room.Restore(JsonSerializer.SerializeToUtf8Bytes(snapshot));
            return room;
        }

        var sequenceRoom = NewRoom(new ManualTimeProvider(0));
        Check(sequenceRoom.Move("a", new MoveRequest(0, 0, 1)) is null, "sequence baseline accepted");
        var sequenceBefore = sequenceRoom.Capture();
        Check(
            sequenceRoom.Move("a", new MoveRequest(0, 0, 1)) is { Reason: Rejection.StaleSequence }
                && SameSnapshot(sequenceBefore, sequenceRoom.Capture()),
            "duplicate sequence rejection preserves all state"
        );
        Check(
            sequenceRoom.Shoot("a", new ShootRequest("b", 0)) is { Reason: Rejection.StaleSequence }
                && SameSnapshot(sequenceBefore, sequenceRoom.Capture()),
            "reversed shared sequence rejection preserves all state"
        );
        Check(
            sequenceRoom.Move("a", new MoveRequest(0, 0, GameNames.MaxSafeInteger + 1))
                is { Reason: Rejection.IntegerOutOfRange }
                && SameSnapshot(sequenceBefore, sequenceRoom.Capture()),
            "out-of-range sequence rejection preserves all state"
        );
        var oversizedStoredSequenceRoom = NewRoom(new ManualTimeProvider(0));
        var oversizedStoredSequence = JsonNode.Parse(oversizedStoredSequenceRoom.Capture())
            ?? throw new InvalidOperationException("Room snapshot was empty.");
        oversizedStoredSequence["Players"]!.AsArray()[0]!["LastSequence"] =
            GameNames.MaxSafeInteger + 2;
        oversizedStoredSequenceRoom.Restore(
            JsonSerializer.SerializeToUtf8Bytes(oversizedStoredSequence)
        );
        var oversizedStoredSequenceBefore = oversizedStoredSequenceRoom.Capture();
        Check(
            oversizedStoredSequenceRoom.Move(
                "a",
                new MoveRequest(0, 0, GameNames.MaxSafeInteger + 1)
            ) is { Reason: Rejection.IntegerOutOfRange }
                && SameSnapshot(oversizedStoredSequenceBefore, oversizedStoredSequenceRoom.Capture()),
            "out-of-range sequence precedes stale sequence when both apply"
        );

        var stepRoom = NewRoom(new ManualTimeProvider(0));
        Check(stepRoom.Move("a", new MoveRequest(GameNames.MaxStep, 0, 1)) is null, "MaxStep boundary accepted");
        var stepBefore = stepRoom.Capture();
        Check(
            stepRoom.Move("a", new MoveRequest(GameNames.MaxStep + 0.001, 0, 2))
                is { Reason: Rejection.StepTooLarge }
                && SameSnapshot(stepBefore, stepRoom.Capture()),
            "movement above MaxStep is rejected without state change"
        );

        var edgeClock = new ManualTimeProvider(0);
        var mapEdges = new[]
        {
            (Name: "x lower inclusive", X: 2.0, Y: 50.0, Dx: -2.0, Dy: 0.0, TargetX: 0.0, TargetY: 50.0),
            (Name: "x upper inclusive", X: 98.0, Y: 50.0, Dx: 2.0, Dy: 0.0, TargetX: 100.0, TargetY: 50.0),
            (Name: "y lower inclusive", X: 50.0, Y: 2.0, Dx: 0.0, Dy: -2.0, TargetX: 50.0, TargetY: 0.0),
            (Name: "y upper inclusive", X: 50.0, Y: 98.0, Dx: 0.0, Dy: 2.0, TargetX: 50.0, TargetY: 100.0),
        };
        foreach (var edge in mapEdges)
        {
            var room = Positioned(edgeClock, edge.X, edge.Y);
            Check(
                room.Move("a", new MoveRequest(edge.Dx, edge.Dy, 1)) is null
                    && Player(room, "a").X == edge.TargetX
                    && Player(room, "a").Y == edge.TargetY,
                edge.Name
            );
        }
        foreach (var edge in new[]
        {
            (Name: "x lower outside", X: 0.0, Y: 50.0, Dx: -0.01, Dy: 0.0),
            (Name: "x upper outside", X: 100.0, Y: 50.0, Dx: 0.01, Dy: 0.0),
            (Name: "y lower outside", X: 50.0, Y: 0.0, Dx: 0.0, Dy: -0.01),
            (Name: "y upper outside", X: 50.0, Y: 100.0, Dx: 0.0, Dy: 0.01),
        })
        {
            var room = Positioned(edgeClock, edge.X, edge.Y);
            var before = room.Capture();
            Check(
                room.Move("a", new MoveRequest(edge.Dx, edge.Dy, 1))
                    is { Reason: Rejection.OutsideMap }
                    && SameSnapshot(before, room.Capture()),
                edge.Name
            );
        }

        var wallClock = new ManualTimeProvider(0);
        foreach (var contact in new[]
        {
            (Name: "wall upper endpoint contact", X: GameNames.WallX - GameNames.MaxStep, Y: GameNames.WallMinY),
            (Name: "wall lower endpoint contact", X: GameNames.WallX - GameNames.MaxStep, Y: GameNames.WallMaxY),
        })
        {
            var room = Positioned(wallClock, contact.X, contact.Y);
            var before = room.Capture();
            Check(
                room.Move("a", new MoveRequest(GameNames.MaxStep, 0, 1))
                    is { Reason: Rejection.MoveBlockedByWall }
                    && SameSnapshot(before, room.Capture()),
                contact.Name
            );
        }
        var crossing = Positioned(wallClock, GameNames.WallX - 1, WallCenterY());
        var crossingBefore = crossing.Capture();
        Check(
            crossing.Move("a", new MoveRequest(GameNames.MaxStep, 0, 1))
                is { Reason: Rejection.MoveBlockedByWall }
                && SameSnapshot(crossingBefore, crossing.Capture()),
            "wall segment crossing rejected"
        );
        var clearLane = Positioned(wallClock, GameNames.WallX - GameNames.MaxStep, GameNames.WallMinY - 1);
        Check(
            clearLane.Move("a", new MoveRequest(GameNames.MaxStep, 0, 1)) is null,
            "movement below wall endpoint remains clear"
        );

        var movementClock = new ManualTimeProvider(0);
        var movementRoom = NewRoom(movementClock);
        Check(
            movementRoom.Move("a", new MoveRequest(GameNames.MaxStep, 0, 1)) is null,
            "first movement receives full MaxStep budget"
        );
        movementClock.Advance(TimeSpan.FromMilliseconds(100));
        Check(
            movementRoom.Move("a", new MoveRequest(1, 0, 2)) is null,
            "partial elapsed time grants proportional movement budget"
        );
        var partialBefore = movementRoom.Capture();
        Check(
            movementRoom.Move("a", new MoveRequest(0.1, 0, 3))
                is { Reason: Rejection.MoveBudget, RetryAfterMs: 10 }
                && SameSnapshot(partialBefore, movementRoom.Capture()),
            "movement budget rejection preserves sequence and timing state"
        );
        movementClock.Advance(TimeSpan.FromMilliseconds(10));
        Check(
            movementRoom.Move("a", new MoveRequest(0.1, 0, 3)) is null,
            "movement budget exact retry boundary"
        );
        movementClock.Advance(TimeSpan.FromSeconds(1));
        Check(
            movementRoom.Move("a", new MoveRequest(GameNames.MaxStep, 0, 4)) is null,
            "movement budget remains capped at MaxStep"
        );

        var invalidClock = new ManualTimeProvider(0);
        foreach (var value in new[] { double.NaN, double.PositiveInfinity, double.NegativeInfinity })
        {
            var room = NewRoom(invalidClock);
            var before = room.Capture();
            Check(
                room.Move("a", new MoveRequest(value, 0, 1))
                    is { Reason: Rejection.InvalidMove }
                    && SameSnapshot(before, room.Capture()),
                $"nonfinite move {value} rejected without state change"
            );
        }

        var fireClock = new ManualTimeProvider(0);
        var fireRoom = NewRoom(fireClock);
        Check(fireRoom.Shoot("a", new ShootRequest("b", 1)) is null, "first shot accepted");
        var fireBefore = fireRoom.Capture();
        Check(
            fireRoom.Shoot("a", new ShootRequest("b", 2))
                is { Reason: Rejection.FireTooSoon, RetryAfterMs: GameNames.FireIntervalMs }
                && SameSnapshot(fireBefore, fireRoom.Capture()),
            "fire interval rejects at zero elapsed time without state change"
        );
        fireClock.Advance(TimeSpan.FromMilliseconds(GameNames.FireIntervalMs - 1));
        var lastMillisecondBefore = fireRoom.Capture();
        Check(
            fireRoom.Shoot("a", new ShootRequest("b", 2))
                is { Reason: Rejection.FireTooSoon, RetryAfterMs: 1 }
                && SameSnapshot(lastMillisecondBefore, fireRoom.Capture()),
            "fire interval reports the last millisecond"
        );
        fireClock.Advance(TimeSpan.FromMilliseconds(1));
        Check(fireRoom.Shoot("a", new ShootRequest("b", 2)) is null, "fire interval exact boundary accepted");

        var wallShotRoom = Positioned(new ManualTimeProvider(0), 40, WallCenterY(), 60, WallCenterY());
        var wallShotBefore = wallShotRoom.Capture();
        Check(
            wallShotRoom.Shoot("a", new ShootRequest("b", 1)) is { Reason: Rejection.BlockedByWall }
                && SameSnapshot(wallShotBefore, wallShotRoom.Capture()),
            "wall blocks shot without changing state"
        );
        var rangeShotRoom = Positioned(new ManualTimeProvider(0), 10, 10, 95, 10);
        var rangeShotBefore = rangeShotRoom.Capture();
        Check(
            rangeShotRoom.Shoot("a", new ShootRequest("b", 1)) is { Reason: Rejection.OutOfRange }
                && SameSnapshot(rangeShotBefore, rangeShotRoom.Capture()),
            "range rejects shot without changing state"
        );
        var missingTargetRoom = NewRoom(new ManualTimeProvider(0));
        var missingTargetBefore = missingTargetRoom.Capture();
        Check(
            missingTargetRoom.Shoot("a", new ShootRequest("missing", 1)) is { Reason: Rejection.NoTarget }
                && SameSnapshot(missingTargetBefore, missingTargetRoom.Capture()),
            "missing target rejects shot without changing state"
        );

        var sourceClock = new ManualTimeProvider(5_000_000);
        var source = NewRoom(sourceClock);
        Check(source.Move("a", new MoveRequest(GameNames.MaxStep, 0, 1)) is null, "snapshot source move accepted");
        Check(source.Shoot("a", new ShootRequest("b", 2)) is null, "snapshot source shot accepted");
        sourceClock.Advance(TimeSpan.FromMilliseconds(60));
        var snapshot = source.Capture();
        var snapshotJson = JsonDocument.Parse(snapshot);
        var sourcePlayer = snapshotJson.RootElement.GetProperty("Players")[0];
        Check(
            sourcePlayer.GetProperty("MoveBudgetRemainingMs").GetInt64() == 140
                && sourcePlayer.GetProperty("FireCooldownRemainingMs").GetInt64() == 190,
            "snapshot stores remaining movement and fire milliseconds"
        );
        Check(
            !System.Text.Encoding.UTF8.GetString(snapshot).Contains("Timestamp", StringComparison.Ordinal)
                && SameSnapshot(snapshot, source.Capture()),
            "snapshot stores no absolute timestamp and capture is stable"
        );

        var destinationClock = new ManualTimeProvider(TimeSpan.TicksPerSecond * 900_000);
        var destination = new RoomState(destinationClock);
        destination.Restore(snapshot);
        Check(
            JsonSerializer.Serialize(source.View()) == JsonSerializer.Serialize(destination.View())
                && source.Phase == destination.Phase
                && Player(destination, "a").LastSequence == 2,
            "relocation restores state under a different clock origin"
        );
        Check(
            destination.Move("a", new MoveRequest(GameNames.MaxStep, 0, 3))
                is { Reason: Rejection.MoveBudget, RetryAfterMs: 140 },
            "relocated movement budget retains remaining milliseconds"
        );
        Check(
            destination.Shoot("a", new ShootRequest("b", 3))
                is { Reason: Rejection.FireTooSoon, RetryAfterMs: 190 },
            "relocated fire interval retains remaining milliseconds"
        );
        destinationClock.Advance(TimeSpan.FromMilliseconds(140));
        Check(
            destination.Move("a", new MoveRequest(GameNames.MaxStep, 0, 3)) is null,
            "relocated movement budget expires at its exact boundary"
        );
        destinationClock.Advance(TimeSpan.FromMilliseconds(50));
        Check(
            destination.Shoot("a", new ShootRequest("b", 4)) is null,
            "relocated fire cooldown expires at its exact boundary"
        );

        var finishedClock = new ManualTimeProvider(0);
        var finished = NewRoom(finishedClock);
        for (var sequence = 1L; sequence <= GameNames.InitialHp / GameNames.ShotDamage; sequence++)
        {
            Check(
                finished.Shoot("a", new ShootRequest("b", sequence)) is null,
                $"lethal sequence shot {sequence} accepted"
            );
            finishedClock.Advance(TimeSpan.FromMilliseconds(GameNames.FireIntervalMs));
        }
        Check(
            finished.Phase == GameNames.FinishedPhase
                && finished.Phase == "Finished"
                && finished.Winner == "a"
                && Player(finished, "b").Hp == 0,
            "lethal shots finish the match"
        );
        var finishedBefore = finished.Capture();
        Check(
            finished.Move("a", new MoveRequest(0, 0, 5)) is { Reason: Rejection.MatchFinished }
                && SameSnapshot(finishedBefore, finished.Capture()),
            "finished movement rejection preserves state"
        );
        Check(
            finished.Shoot("a", new ShootRequest("b", 5)) is { Reason: Rejection.MatchFinished }
                && SameSnapshot(finishedBefore, finished.Capture()),
            "finished shooting rejection preserves state"
        );

        Console.WriteLine($"Game domain checks: 9 categories; {checkedCount}/{checkedCount} assertions passed");

        static double WallCenterY() => (GameNames.WallMinY + GameNames.WallMaxY) / 2;
    }

    private sealed class ManualTimeProvider(long startingTimestamp) : TimeProvider
    {
        private long timestamp = startingTimestamp;

        public override long TimestampFrequency => TimeSpan.TicksPerSecond;

        public override long GetTimestamp() => timestamp;

        public void Advance(TimeSpan elapsed) => timestamp += elapsed.Ticks;
    }
}
