namespace Zlink.Framework.UnitTests;

public sealed class DirectReplyCompletionRegistryTests
{
    [Theory]
    [InlineData(5)]
    [InlineData(-5)]
    public void OriginalDeadlineRetentionUsesOneMonotonicEpoch(int wallJumpSeconds)
    {
        var time = new ManualTimeProvider();
        var clock = new ZLinkDeadlineClock(time);
        var originalDeadline = time.GetUtcNow().AddSeconds(2).ToUnixTimeMilliseconds();
        var deadline = clock.FromUnixTimeMilliseconds(originalDeadline);
        var retainedUntil = deadline + TimeSpan.FromMinutes(5);

        time.AdvanceWallClockOnly(TimeSpan.FromSeconds(wallJumpSeconds));
        time.AdvanceMonotonicOnly(TimeSpan.FromSeconds(301));
        Assert.True(clock.Elapsed < retainedUntil);
        Assert.Equal(originalDeadline + 299_000, clock.GetUnixTimeMilliseconds());
        time.AdvanceWallClockOnly(TimeSpan.FromSeconds(-2 * wallJumpSeconds));
        time.AdvanceMonotonicOnly(TimeSpan.FromSeconds(1));
        Assert.Equal(retainedUntil, clock.Elapsed);
        Assert.Equal(deadline, clock.FromUnixTimeMilliseconds(originalDeadline));
    }

    [Theory]
    [InlineData(5)]
    [InlineData(-5)]
    public void AdmittedDeadlineAndRetentionIgnoreSubsequentWallClockReads(int wallJumpSeconds)
    {
        var time = new ManualTimeProvider();
        var clock = new ZLinkDeadlineClock(time);
        var originalDeadline = time.GetUtcNow().AddSeconds(2).ToUnixTimeMilliseconds();
        var deadline = clock.FromUnixTimeMilliseconds(originalDeadline);
        var retainedUntil = deadline + TimeSpan.FromMinutes(5);

        time.AdvanceWallClockOnly(TimeSpan.FromSeconds(wallJumpSeconds));
        _ = clock.GetUnixTimeMilliseconds();
        Assert.Equal(TimeSpan.FromSeconds(2), deadline - clock.Elapsed);
        time.AdvanceMonotonicOnly(TimeSpan.FromSeconds(301));
        Assert.True(clock.Elapsed < retainedUntil);
        time.AdvanceMonotonicOnly(TimeSpan.FromSeconds(1));
        Assert.Equal(retainedUntil, clock.Elapsed);
    }

    [Fact]
    public void NewWireDeadlineAfterForwardJumpKeepsItsRequestedBudget()
    {
        var time = new ManualTimeProvider();
        var clock = new ZLinkDeadlineClock(time);
        time.AdvanceWallClockOnly(TimeSpan.FromSeconds(5));

        var deadline = clock.FromUnixTimeMilliseconds(
            time.GetUtcNow().AddSeconds(2).ToUnixTimeMilliseconds());

        Assert.Equal(TimeSpan.FromSeconds(2), deadline - clock.Elapsed);
    }

    [Fact]
    public async Task BoundedRegistrationHasOneLinearizationPoint()
    {
        var registry = new ZLinkDirectReplyCompletionRegistry<string, object>(
            capacity: 1,
            terminalRetention: TimeSpan.FromMinutes(1));
        using var start = new ManualResetEventSlim();
        var attempts = new[] { "first", "second" }
            .Select(key => Task.Run(() =>
            {
                start.Wait();
                return registry.TryRegister(key, new object());
            }))
            .ToArray();

        start.Set();
        Assert.Equal(1, (await Task.WhenAll(attempts)).Count(static value => value));
    }

    [Fact]
    public void OnlyExpectedOwnerCanRemoveAndPublishTerminal()
    {
        var registry = new ZLinkDirectReplyCompletionRegistry<string, object>(
            capacity: 1,
            terminalRetention: TimeSpan.FromMinutes(1));
        var owner = new object();
        Assert.True(registry.TryRegister("reply", owner));
        Assert.Same(owner, registry.TryGet("reply"));

        Assert.False(registry.TryRemove(
            "reply",
            new object(),
            rememberTerminal: true));
        Assert.True(registry.TryRemove(
            "reply",
            owner,
            rememberTerminal: true));
        Assert.False(registry.TryRegister("reply", new object()));
    }

    [Theory]
    [InlineData(5)]
    [InlineData(-5)]
    public void ExpiredTerminalAllowsTheIdentityAgain(int wallJumpSeconds)
    {
        var time = new ManualTimeProvider();
        var registry = new ZLinkDirectReplyCompletionRegistry<string, object>(
            capacity: 1,
            terminalRetention: TimeSpan.FromMilliseconds(20),
            timeProvider: time);
        var owner = new object();
        Assert.True(registry.TryRegister("reply", owner));
        Assert.True(registry.TryRemove(
            "reply",
            owner,
            rememberTerminal: true));

        time.AdvanceWallClockOnly(TimeSpan.FromSeconds(wallJumpSeconds));
        time.AdvanceMonotonicOnly(TimeSpan.FromMilliseconds(19));
        Assert.False(registry.TryRegister("reply", new object()));
        time.AdvanceWallClockOnly(TimeSpan.FromSeconds(-2 * wallJumpSeconds));
        time.AdvanceMonotonicOnly(TimeSpan.FromMilliseconds(1));

        Assert.True(registry.TryRegister("reply", new object()));
    }
}
