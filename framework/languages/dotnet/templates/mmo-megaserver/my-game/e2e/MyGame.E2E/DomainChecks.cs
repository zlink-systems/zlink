using System.Text.Json;
using MyGame.Region.Domain;
using MyGame.Shared;

namespace MyGame.E2E;

internal static class DomainChecks
{
    private sealed class ManualClock(long timestamp = 0) : TimeProvider
    {
        private long timestamp = timestamp;

        public override long TimestampFrequency => TimeSpan.TicksPerSecond;

        public override long GetTimestamp() => timestamp;

        public void AdvanceMilliseconds(long milliseconds) =>
            timestamp += TimeSpan.FromMilliseconds(milliseconds).Ticks;
    }

    public static void Run()
    {
        var checkedCount = 0;
        void Check(bool condition, string name)
        {
            if (!condition)
                throw new InvalidOperationException("Domain boundary failed: " + name);
            checkedCount++;
        }

        static bool Equal<T>(T left, T right) =>
            JsonSerializer.Serialize(left) == JsonSerializer.Serialize(right);

        Check(GameNames.MaxSpeed == GameNames.MaxStep * 5.0, "movement speed contract");

        var clock = new ManualClock();
        var positions = new Positions(clock);
        positions.Add("p1");
        Check(
            positions.Move("p1", new(GameNames.MaxStep, 1)) is null
                && positions.View().Single() is { X: GameNames.MaxStep, Sequence: 1 },
            "first movement accepts MaxStep"
        );

        var before = positions.Capture();
        var budget = positions.Move("p1", new(GameNames.MaxStep, 2));
        Check(
            budget is { Reason: Rejection.MoveBudget, RetryAfterMs: 200 }
                && Equal(before, positions.Capture()),
            "move budget rejection preserves position, sequence and remaining time"
        );
        var fractionalBudget = positions.Move("p1", new(0.01, 2));
        Check(
            fractionalBudget is { Reason: Rejection.MoveBudget, RetryAfterMs: > 0 }
                && Equal(before, positions.Capture()),
            "positive movement deficit returns positive retry time"
        );

        var tooLarge = positions.Move("p1", new(GameNames.MaxStep + 1, 2));
        Check(
            tooLarge is { Reason: Rejection.StepTooLarge, RetryAfterMs: 0 }
                && Equal(before, positions.Capture()),
            "MaxStep plus one is rejected without state change"
        );
        var stale = positions.Move("p1", new(1, 1));
        Check(
            stale is { Reason: Rejection.StaleSequence, RetryAfterMs: 0 }
                && Equal(before, positions.Capture()),
            "current sequence is stale without state change"
        );

        foreach (var value in new[] { double.NaN, double.PositiveInfinity, double.NegativeInfinity })
        {
            var nonFinite = positions.Move("p1", new(value, 2));
            Check(
                nonFinite is { Reason: Rejection.NonFinite, RetryAfterMs: 0 }
                    && Equal(before, positions.Capture()),
                "non-finite movement is rejected without state change"
            );
        }

        var edge = new Positions(clock);
        edge.Restore(new PositionsSnapshot([new PositionSnapshot("edge", GameNames.RegionMaxX, 0, 0)]));
        before = edge.Capture();
        var outsideMaximum = edge.Move("edge", new(1, 1));
        Check(
            outsideMaximum is { Reason: Rejection.OutsideRegion, RetryAfterMs: 0 }
                && Equal(before, edge.Capture()),
            "region maximum plus one is rejected without state change"
        );
        edge.Restore(new PositionsSnapshot([new PositionSnapshot("edge", GameNames.RegionMinX, 0, 0)]));
        before = edge.Capture();
        var outsideMinimum = edge.Move("edge", new(-1, 1));
        Check(
            outsideMinimum is { Reason: Rejection.OutsideRegion, RetryAfterMs: 0 }
                && Equal(before, edge.Capture()),
            "region minimum minus one is rejected without state change"
        );

        var safeSequence = new Positions(clock);
        safeSequence.Add("safe");
        Check(
            safeSequence.Move("safe", new(0, GameNames.MaxSafeInteger)) is null,
            "maximum safe integer sequence is accepted"
        );
        before = safeSequence.Capture();
        var unsafeSequence = safeSequence.Move(
            "safe",
            new(0, GameNames.MaxSafeInteger + 1)
        );
        Check(
            unsafeSequence is { Reason: Rejection.IntegerOutOfRange, RetryAfterMs: 0 }
                && Equal(before, safeSequence.Capture()),
            "sequence above maximum safe integer is rejected without overflow"
        );
        var oversizedDomain = new Positions(clock);
        oversizedDomain.Add("oversized");
        before = oversizedDomain.Capture();
        var oversizedNumber = oversizedDomain.Move("oversized", new(double.MaxValue, 1));
        Check(
            oversizedNumber is { Reason: Rejection.StepTooLarge, RetryAfterMs: 0 }
                && Equal(before, oversizedDomain.Capture()),
            "finite step above MaxStep is rejected without overflow"
        );

        var sourceClock = new ManualClock();
        var source = new Positions(sourceClock);
        source.Add("relocated");
        Check(source.Move("relocated", new(GameNames.MaxStep, 1)) is null, "snapshot source move");
        var snapshot = source.Capture();
        Check(snapshot.Players.Single().RemainingMs == 200, "snapshot stores remaining milliseconds");
        Check(Equal(snapshot, source.Capture()), "capture does not change the source domain");

        var destinationClock = new ManualClock(TimeSpan.FromHours(8).Ticks);
        var destination = new Positions(destinationClock);
        destination.Restore(
            JsonSerializer.Deserialize<PositionsSnapshot>(JsonSerializer.Serialize(snapshot))!
        );
        Check(
            Equal(source.View(), destination.View())
                && destination.Capture().Players.Single().RemainingMs == 200,
            "relocation restores state under a different clock origin"
        );
        before = destination.Capture();
        var relocatedBudget = destination.Move("relocated", new(GameNames.MaxStep, 2));
        Check(
            relocatedBudget is { Reason: Rejection.MoveBudget, RetryAfterMs: 200 }
                && Equal(before, destination.Capture()),
            "relocated movement retains its remaining budget"
        );
        destinationClock.AdvanceMilliseconds(relocatedBudget!.RetryAfterMs);
        Check(
            destination.Move("relocated", new(GameNames.MaxStep, 2)) is null,
            "relocated movement accepts at the server-provided boundary"
        );

        Console.WriteLine($"Domain boundaries: {checkedCount}/{checkedCount} passed");
    }
}
