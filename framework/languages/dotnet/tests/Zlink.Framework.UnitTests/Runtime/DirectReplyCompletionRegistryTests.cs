namespace Zlink.Framework.UnitTests;

public sealed class DirectReplyCompletionRegistryTests
{
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

    [Fact]
    public async Task ExpiredTerminalAllowsTheIdentityAgain()
    {
        var registry = new ZLinkDirectReplyCompletionRegistry<string, object>(
            capacity: 1,
            terminalRetention: TimeSpan.FromMilliseconds(20));
        var owner = new object();
        Assert.True(registry.TryRegister("reply", owner));
        Assert.True(registry.TryRemove(
            "reply",
            owner,
            rememberTerminal: true));

        await Task.Delay(50);

        Assert.True(registry.TryRegister("reply", new object()));
    }
}
