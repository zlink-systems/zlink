using Xunit;

namespace Zlink.NetworkSync.Simulation.Tests;

public sealed class MatchTicksTests
{
    [Theory]
    [InlineData(20)]
    [InlineData(30)]
    [InlineData(60)]
    public void LateWakeupRunsConsecutiveTicksWithinCountBudget(int rate)
    {
        var clock = new FakeClock();
        var game = new FakeSimulation(clock);
        var match = new NetworkSyncMatch<int, int>("match", clock, game, new() { TickRate = rate });
        clock.Advance(200_000);

        var first = match.Wakeup();

        Assert.Equal(4, first.ExecutedTicks);
        Assert.Equal(rate / 5, first.BacklogTicks);
        Assert.Equal(0, first.SkippedTicks);
        Assert.Equal(new long[] { 1, 2, 3, 4 }, game.Ticks.Select(tick => tick.Tick));

        for (var completed = first.ExecutedTicks; completed < rate / 5; completed += 4)
            Assert.InRange(match.Wakeup().ExecutedTicks, 1, 4);

        Assert.Equal(0, match.Wakeup().ExecutedTicks);
        Assert.Equal(Enumerable.Range(1, rate / 5).Select(tick => (long)tick),
            game.Ticks.Select(tick => tick.Tick));
    }

    [Theory]
    [InlineData(20)]
    [InlineData(30)]
    [InlineData(60)]
    public void WakeupStopsStartingTicksWhenTimeBudgetIsSpent(int rate)
    {
        var clock = new FakeClock();
        var game = new FakeSimulation(clock) { WorkMicroseconds = 500_000 / rate + 1 };
        var match = new NetworkSyncMatch<int, int>("match", clock, game, new() { TickRate = rate });
        clock.Advance(200_000);

        Assert.Equal(1, match.Wakeup().ExecutedTicks);
        Assert.Equal(1, Assert.Single(game.Ticks).Tick);
        Assert.Equal(1, match.Wakeup().ExecutedTicks);
        Assert.Equal(new long[] { 1, 2 }, game.Ticks.Select(tick => tick.Tick));
    }

    [Theory]
    [InlineData(20)]
    [InlineData(30)]
    [InlineData(60)]
    public void LogicalClockProducesTwentySnapshotOpportunitiesPerSecond(int rate)
    {
        var clock = new FakeClock();
        var game = new FakeSimulation(clock);
        var match = new NetworkSyncMatch<int, int>("match", clock, game, new() { TickRate = rate });
        var origin = clock.GetTimestamp();
        var snapshots = 0;

        for (var tick = 1; tick <= rate; tick++)
        {
            var deadline = origin + (tick * 1_000_000L + rate - 1) / rate;
            clock.Advance(deadline - clock.GetTimestamp() - 1);
            Assert.Equal(0, match.Wakeup().ExecutedTicks);
            clock.Advance(1);

            var result = match.Wakeup();
            Assert.Equal(tick, result.LatestTick.Tick);
            Assert.Equal(deadline, result.LatestTick.LogicalStartTimestamp);
            if (result.SnapshotDue)
                snapshots++;
        }

        Assert.Equal(20, snapshots);
    }

    [Fact]
    public void ResyncPreservesAuthorityAndRejectsOldEpochInput()
    {
        var clock = new FakeClock();
        var game = new FakeSimulation(clock);
        var match = new NetworkSyncMatch<int, int>("match", clock, game);
        var firstEpoch = match.LatestTick.Epoch;
        match.AddPlayer("actor");
        match.SubmitInput("match", firstEpoch, "actor", 1, 20, 7, [new(10, 11)]);
        clock.Advance(33_334);
        match.Wakeup();
        match.SubmitInput("match", firstEpoch, "actor", 2, 20, 100);
        clock.Advance(533_334);

        var result = match.Wakeup();

        Assert.Equal(18, game.State);
        Assert.Equal(firstEpoch + 1, result.LatestTick.Epoch);
        Assert.Equal(0, result.LatestTick.Tick);
        Assert.Equal(clock.GetTimestamp(), result.LatestTick.LogicalStartTimestamp);
        Assert.Equal(16, result.SkippedTicks);
        Assert.Equal(0, result.ExecutedTicks);
        Assert.True(result.SnapshotDue);
        Assert.Equal(InputDisposition.WrongEpoch,
            match.SubmitInput("match", firstEpoch, "actor", 1, 20, 99).Disposition);
        Assert.Equal(18, game.State);

        clock.Advance(33_334);
        match.Wakeup();
        Assert.Equal(18, game.State);
        Assert.Null(game.Inputs[^1].LastAppliedInputTick);
        Assert.Equal(InputDisposition.Accepted,
            match.SubmitInput("match", result.LatestTick.Epoch, "actor", 2, 20, 2, [new(10, 3)]).Disposition);
        clock.Advance(33_333);
        match.Wakeup();
        Assert.Equal(23, game.State);
    }

    [Fact]
    public void BacklogAtLimitStillRunsTicks()
    {
        var clock = new FakeClock();
        var game = new FakeSimulation(clock);
        var match = new NetworkSyncMatch<int, int>("match", clock, game);
        var epoch = match.LatestTick.Epoch;
        clock.Advance(500_000);

        var result = match.Wakeup();

        Assert.Equal(15, result.BacklogTicks);
        Assert.Equal(epoch, result.LatestTick.Epoch);
        Assert.Equal(4, result.ExecutedTicks);
        Assert.Equal(0, result.SkippedTicks);
    }
}
