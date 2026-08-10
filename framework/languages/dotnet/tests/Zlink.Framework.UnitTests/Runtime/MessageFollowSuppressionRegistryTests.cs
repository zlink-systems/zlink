namespace Zlink.Framework.UnitTests;

public sealed class MessageFollowSuppressionRegistryTests
{
    [Fact]
    public async Task ConcurrentBeginAllowsOneWireSenderForTheExactFence()
    {
        var registry = new ZLinkMessageFollowSuppressionRegistry();
        var fence = Fence("target-a", targetNodeGeneration: 2);
        using var start = new ManualResetEventSlim();

        var attempts = Enumerable.Range(0, 64)
            .Select(_ => Task.Run(() =>
            {
                start.Wait();
                return registry.TryBegin(fence);
            }))
            .ToArray();

        start.Set();
        var results = await Task.WhenAll(attempts);
        Assert.Equal(1, results.Count(static result => result));
        Assert.True(registry.MarkSent(fence));
        Assert.False(registry.TryBegin(fence));
    }

    [Fact]
    public void RejectedSendReturnsTheExactFenceToIdle()
    {
        var registry = new ZLinkMessageFollowSuppressionRegistry();
        var fence = Fence("target-a", targetNodeGeneration: 2);

        Assert.True(registry.TryBegin(fence));
        registry.Abort(fence);

        Assert.True(registry.TryBegin(fence));
    }

    [Fact]
    public void FenceAndMulticastTargetIdentityDoNotCrossSuppress()
    {
        var registry = new ZLinkMessageFollowSuppressionRegistry();
        var firstTarget = Fence("target-a", targetNodeGeneration: 2);
        var nextGeneration = Fence("target-a", targetNodeGeneration: 3);
        var secondTarget = Fence("target-b", targetNodeGeneration: 2);

        Assert.True(registry.TryBegin(firstTarget));
        Assert.True(registry.MarkSent(firstTarget));
        Assert.True(registry.TryBegin(nextGeneration));
        Assert.True(registry.TryBegin(secondTarget));

        registry.Expire(firstTarget);
        Assert.True(registry.TryBegin(firstTarget));
    }

    [Fact]
    public void CapacityNeverEvictsAnExistingSentMarker()
    {
        var registry = new ZLinkMessageFollowSuppressionRegistry(capacity: 1);
        var retained = Fence("target-a", targetNodeGeneration: 2);
        var rejected = Fence("target-b", targetNodeGeneration: 2);

        Assert.True(registry.TryBegin(retained));
        Assert.True(registry.MarkSent(retained));
        Assert.False(registry.TryBegin(rejected));
        Assert.False(registry.TryBegin(retained));

        registry.Expire(retained);
        Assert.True(registry.TryBegin(rejected));
    }

    [Fact]
    public void ActorLeaseCannotStartANoticeBeforeCommitOrAfterExpiry()
    {
        var time = new ManualTimeProvider();
        var lease = new ZLinkActorMessageFollowLease(time);
        var fence = Fence("target-a", targetNodeGeneration: 2);

        Assert.False(lease.TryBeginMessageFollowNotice(fence));
        lease.Commit(TimeSpan.FromSeconds(1));
        Assert.True(lease.TryBeginMessageFollowNotice(fence));
        lease.AbortMessageFollowNotice(fence);

        time.AdvanceMonotonicOnly(TimeSpan.FromSeconds(2));

        Assert.False(lease.TryBeginMessageFollowNotice(fence));
    }

    [Fact]
    public void ActorLeaseCancellationAtomicallyClosesNoticeAdmission()
    {
        var lease = new ZLinkActorMessageFollowLease(TimeProvider.System);
        var fence = Fence("target-a", targetNodeGeneration: 2);
        lease.Commit(TimeSpan.FromMinutes(1));

        lease.Cancel();

        Assert.False(lease.TryBeginMessageFollowNotice(fence));
    }

    private static ZLinkMessageFollowFence Fence(
        string targetId,
        ulong targetNodeGeneration) =>
        new(
            ZLinkMessageFollowObjectKind.Actor,
            "actor-1",
            targetId,
            RoutingId.From("source"),
            RoutingId.From(targetId),
            SourceObjectGeneration: 7,
            TargetObjectGeneration: 7,
            SourceNodeGeneration: 1,
            TargetNodeGeneration: targetNodeGeneration,
            SourceAuthorityOwnerGeneration: 11,
            TargetAuthorityOwnerGeneration: 12,
            SourceOwnerLeaseGeneration: 21,
            TargetOwnerLeaseGeneration: 22);
}
