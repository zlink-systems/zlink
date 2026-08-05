using Systems.Zlink.Stream.Connector.Runtime;
using Xunit;

public sealed class TypedHandlerRegistryTests
{
    [Fact]
    public void Snapshot_ReturnsCurrentArrayWithoutPerReadCopy()
    {
        var registry = new ZlinkStreamTypedHandlerRegistry();
        using var subscription = registry.Add("packet", static (_, _) => ValueTask.CompletedTask);

        var first = registry.Snapshot("packet");
        var second = registry.Snapshot("packet");

        Assert.Same(first, second);
    }

    [Fact]
    public void Remove_UsesCopyOnWriteAndKeepsExistingSnapshotStable()
    {
        var registry = new ZlinkStreamTypedHandlerRegistry();
        var firstSubscription = registry.Add("packet", static (_, _) => ValueTask.CompletedTask);
        using var secondSubscription = registry.Add("packet", static (_, _) => ValueTask.CompletedTask);
        var snapshotBeforeRemove = registry.Snapshot("packet");

        firstSubscription.Dispose();
        var snapshotAfterRemove = registry.Snapshot("packet");

        Assert.Equal(2, snapshotBeforeRemove.Count);
        Assert.Single(snapshotAfterRemove);
        Assert.NotSame(snapshotBeforeRemove, snapshotAfterRemove);
        Assert.Same(snapshotAfterRemove, registry.Snapshot("packet"));
    }
}