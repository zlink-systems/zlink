using Xunit;

namespace Zlink.NetworkSync.Simulation.Tests;

public sealed class InputAcceptanceTests
{
    [Theory]
    [InlineData(20)]
    [InlineData(30)]
    [InlineData(60)]
    public void LogicalDeadlineRejectsInputsBeforeDelayedWakeup(int rate)
    {
        var clock = new FakeClock();
        var game = new FakeSimulation(clock);
        var match = new NetworkSyncMatch<int, int>("match", clock, game, new() { TickRate = rate });
        var epoch = match.LatestTick.Epoch;
        match.AddPlayer("early");
        match.AddPlayer("equal");
        match.AddPlayer("late");
        clock.Advance((1_000_000L + rate - 1) / rate - 1);

        Assert.Equal(new(InputDisposition.Accepted, TimeSpan.FromTicks(10)),
            match.SubmitInput("match", epoch, "early", 1, 20, 7));
        clock.Advance(1);
        Assert.Equal(new(InputDisposition.Late, TimeSpan.Zero),
            match.SubmitInput("match", epoch, "equal", 1, 20, 20, [new(1, 30)]));
        clock.Advance(1);
        Assert.Equal(new(InputDisposition.Late, TimeSpan.FromTicks(-10)),
            match.SubmitInput("match", epoch, "late", 1, 20, 40, [new(1, 50)]));
        Assert.Empty(game.Ticks);

        match.Wakeup();
        Assert.Equal(7, game.State);
    }

    [Fact]
    public void FirstInputWinsAndActionIdsAreAcceptedOnce()
    {
        var clock = new FakeClock();
        var game = new FakeSimulation(clock);
        var match = new NetworkSyncMatch<int, int>("match", clock, game);
        var epoch = match.LatestTick.Epoch;
        match.AddPlayer("actor");

        Assert.Equal(InputDisposition.Accepted,
            match.SubmitInput("match", epoch, "actor", 1, 20, 2, [new(99, 3)]).Disposition);
        Assert.Equal(InputDisposition.DuplicateTick,
            match.SubmitInput("match", epoch, "actor", 1, 20, 200, [new(100, 300)]).Disposition);
        clock.Advance(33_334);
        match.Wakeup();

        Assert.Equal(InputDisposition.DuplicateAction,
            match.SubmitInput("match", epoch, "actor", 2, 20, 200, [new(99, 300)]).Disposition);
        Assert.Equal(InputDisposition.DuplicateAction,
            match.SubmitInput("match", epoch, "actor", 2, 20, 200, [new(100, 300), new(100, 300)]).Disposition);
        Assert.Equal(InputDisposition.Accepted,
            match.SubmitInput("match", epoch, "actor", 2, 20, 4, [new(100, 5)]).Disposition);
        clock.Advance(33_333);
        match.Wakeup();

        Assert.Equal(14, game.State);
    }

    [Fact]
    public void ActionIdsMustIncreasePerActorWithinEpoch()
    {
        var clock = new FakeClock();
        var game = new FakeSimulation(clock);
        var match = new NetworkSyncMatch<int, int>("match", clock, game);
        var epoch = match.LatestTick.Epoch;
        match.AddPlayer("actor");

        Assert.Equal(InputDisposition.Accepted,
            match.SubmitInput("match", epoch, "actor", 1, 20, 0, [new(5, 1)]).Disposition);
        Assert.Equal(InputDisposition.DuplicateAction,
            match.SubmitInput("match", epoch, "actor", 2, 20, 0, [new(3, 10)]).Disposition);
        Assert.Equal(InputDisposition.DuplicateAction,
            match.SubmitInput("match", epoch, "actor", 2, 20, 0, [new(7, 10), new(6, 10)]).Disposition);
        Assert.Equal(InputDisposition.Accepted,
            match.SubmitInput("match", epoch, "actor", 2, 20, 0, [new(6, 2), new(7, 4)]).Disposition);
        clock.Advance(66_667);
        match.Wakeup();

        Assert.Equal(7, game.State);
    }

    [Fact]
    public void HoldExpiresAndActionsNeverCarryForward()
    {
        var clock = new FakeClock();
        var game = new FakeSimulation(clock);
        var match = new NetworkSyncMatch<int, int>("match", clock, game, new() { MaxHeldTicks = 2 });
        var epoch = match.LatestTick.Epoch;
        match.AddPlayer("actor");
        SimulationAction<int>[] actions = [new(1, 3)];
        match.SubmitInput("match", epoch, "actor", 1, 20, 2, actions);
        actions[0] = new(1, 100);
        clock.Advance(133_334);

        match.Wakeup();

        Assert.Equal(new[] { 2, 2, 2, 0 }, game.Inputs.Select(input => input.Input));
        Assert.Equal(new[] { false, true, true, true }, game.Inputs.Select(input => input.UsedFallbackInput));
        Assert.All(game.Inputs, input => Assert.Equal(1L, input.LastAppliedInputTick));
        Assert.Equal(9, game.State);
    }

    [Fact]
    public void LimitsAreEnforcedBeforeBufferingAndRateWindowExpires()
    {
        var clock = new FakeClock();
        var game = new FakeSimulation(clock);
        var match = new NetworkSyncMatch<int, int>("match", clock, game, new()
        {
            MaxInputBytes = 20,
            MaxInputsPerSecond = 3,
            MaxFutureTicks = 3
        });
        var epoch = match.LatestTick.Epoch;
        match.AddPlayer("actor");
        match.AddPlayer("other");

        Assert.Equal(InputDisposition.WrongMatch,
            match.SubmitInput("different", epoch, "actor", 1, 20, 100).Disposition);
        Assert.Equal(InputDisposition.UnknownActor,
            match.SubmitInput("match", epoch, "absent", 1, 20, 100).Disposition);
        Assert.Equal(InputDisposition.InvalidSize,
            match.SubmitInput("match", epoch, "actor", 1, 21, 100).Disposition);
        Assert.Equal(InputDisposition.InvalidTick,
            match.SubmitInput("match", epoch, "actor", 0, 20, 100).Disposition);
        Assert.Equal(InputDisposition.TooFarAhead,
            match.SubmitInput("match", epoch, "actor", long.MaxValue, 20, 100).Disposition);
        Assert.Equal(InputDisposition.Accepted,
            match.SubmitInput("match", epoch, "actor", 3, 20, 2).Disposition);
        Assert.Equal(InputDisposition.DuplicateTick,
            match.SubmitInput("match", epoch, "actor", 3, 20, 100).Disposition);
        Assert.Equal(InputDisposition.RateLimited,
            match.SubmitInput("match", epoch, "actor", 1, 20, 100).Disposition);
        Assert.Equal(InputDisposition.Accepted,
            match.SubmitInput("match", epoch, "other", 1, 20, 1).Disposition);

        for (var wakeup = 0; wakeup < 10; wakeup++)
        {
            clock.Advance(100_000);
            match.Wakeup();
        }

        Assert.Equal(InputDisposition.Accepted,
            match.SubmitInput("match", epoch, "actor", 31, 20, 4).Disposition);
        Assert.True(match.RemovePlayer("actor"));
        Assert.Equal(InputDisposition.UnknownActor,
            match.SubmitInput("match", epoch, "actor", 31, 20, 100).Disposition);
        clock.Advance(33_334);
        match.Wakeup();
        Assert.Equal("other", game.Inputs[^1].ActorId);
    }

    [Fact]
    public void FutureLimitBoundsPendingInputWhileWakeupsAreDelayed()
    {
        var clock = new FakeClock();
        var match = new NetworkSyncMatch<int, int>("match", clock, new FakeSimulation(clock),
            new() { MaxFutureTicks = 3 });
        var epoch = match.LatestTick.Epoch;
        match.AddPlayer("actor");

        clock.Advance(66_667);
        Assert.Equal(InputDisposition.Late,
            match.SubmitInput("match", epoch, "actor", 2, 20, 1).Disposition);
        Assert.Equal(InputDisposition.Accepted,
            match.SubmitInput("match", epoch, "actor", 3, 20, 1).Disposition);
        Assert.Equal(InputDisposition.TooFarAhead,
            match.SubmitInput("match", epoch, "actor", 4, 20, 1).Disposition);

        Assert.Equal(2, match.Wakeup().ExecutedTicks);
        Assert.Equal(InputDisposition.Accepted,
            match.SubmitInput("match", epoch, "actor", 4, 20, 1).Disposition);
    }

    [Fact]
    public void RejoiningActorStartsANewActionSequence()
    {
        var clock = new FakeClock();
        var game = new FakeSimulation(clock);
        var match = new NetworkSyncMatch<int, int>("match", clock, game);
        var epoch = match.LatestTick.Epoch;
        match.AddPlayer("actor");
        Assert.Equal(InputDisposition.Accepted,
            match.SubmitInput("match", epoch, "actor", 1, 20, 1, [new(9, 2)]).Disposition);
        clock.Advance(33_334);
        match.Wakeup();

        Assert.True(match.RemovePlayer("actor"));
        Assert.True(match.AddPlayer("actor"));
        Assert.Equal(InputDisposition.Accepted,
            match.SubmitInput("match", epoch, "actor", 2, 20, 3, [new(1, 4)]).Disposition);
        clock.Advance(33_333);
        match.Wakeup();

        Assert.Equal(10, game.State);
    }

    [Fact]
    public void EveryAcceptanceReasonMapsToOneWireDisposition()
    {
        var expected = new Dictionary<InputDisposition, WireInputDisposition>
        {
            [InputDisposition.Accepted] = WireInputDisposition.Accepted,
            [InputDisposition.Late] = WireInputDisposition.Late,
            [InputDisposition.DuplicateTick] = WireInputDisposition.Duplicate,
            [InputDisposition.DuplicateAction] = WireInputDisposition.Duplicate,
            [InputDisposition.WrongMatch] = WireInputDisposition.Rejected,
            [InputDisposition.WrongEpoch] = WireInputDisposition.Rejected,
            [InputDisposition.UnknownActor] = WireInputDisposition.Rejected,
            [InputDisposition.InvalidSize] = WireInputDisposition.Rejected,
            [InputDisposition.RateLimited] = WireInputDisposition.Rejected,
            [InputDisposition.InvalidTick] = WireInputDisposition.Rejected,
            [InputDisposition.TooFarAhead] = WireInputDisposition.Rejected
        };

        Assert.Equal(Enum.GetValues<InputDisposition>().OrderBy(value => value), expected.Keys.OrderBy(value => value));
        foreach (var (reason, wire) in expected)
            Assert.Equal(wire, reason.ToWire());
    }
}
