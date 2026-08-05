namespace Zlink.Framework.UnitTests.Runtime;

public sealed class DeferredActorJoinContractTests
{
    [Fact]
    public void Public_join_terminal_is_synchronous_defer_only()
    {
        var methods = typeof(IZLinkActorDeferredJoinCall).GetMethods();

        Assert.Contains(methods, method => method.Name == "Defer"
                                           && method.ReturnType == typeof(void));
        Assert.DoesNotContain(methods, method => method.Name is "Async" or "Yield");
        Assert.True(typeof(IZLinkActorDeferredJoinCall)
            .IsAssignableFrom(typeof(IZLinkActorJoinSpotCall)));
        Assert.True(typeof(IZLinkActorDeferredJoinCall)
            .IsAssignableFrom(typeof(IZLinkActorJoinEntrySpotCall)));
        Assert.Contains(
            typeof(IZLinkActor).GetMethods(),
            method => method.Name == "OnJoinCompletedAsync"
                      && method.ReturnType == typeof(ValueTask));
    }

    [Fact]
    public async Task Reserved_barrier_runs_before_already_queued_actor_work()
    {
        var mailbox = new ZLinkActorDispatchMailbox();
        var current = await mailbox.EnterAsync(CancellationToken.None);
        var queued = mailbox.EnterAsync(CancellationToken.None).AsTask();
        var barrier = mailbox.ReserveBarrier();

        current.Dispose();
        var barrierTurn = await barrier.ClaimAsync();

        Assert.False(queued.IsCompleted);

        barrierTurn.Dispose();
        using var queuedTurn = await queued;
    }
}
