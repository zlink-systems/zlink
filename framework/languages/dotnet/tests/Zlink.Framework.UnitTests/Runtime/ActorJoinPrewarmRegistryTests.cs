using Zlink.Framework.Runtime.Actors;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ActorJoinPrewarmRegistryTests
{
    [Fact]
    public void ParkOrDeliver_WithNoAttempt_ReportsNotFound()
    {
        var registry = new ZLinkActorJoinPrewarmRegistry();
        var route = registry.ParkOrDeliver(
            "actor-1",
            1,
            () => Frame("body"),
            onFailed: () => Assert.Fail("must not fail an arrival with no attempt"));

        Assert.Equal(ZLinkActorJoinPrewarmRegistry.IngressRoute.NotFound, route);
    }

    [Fact]
    public void ParkOrDeliver_AfterRegister_Parks()
    {
        var registry = new ZLinkActorJoinPrewarmRegistry();
        registry.Register("handoff-1", "actor-1", actorGeneration: 1);

        var failed = false;
        var route = registry.ParkOrDeliver(
            "actor-1",
            1,
            () => Frame("body"),
            onFailed: () => failed = true);

        Assert.Equal(ZLinkActorJoinPrewarmRegistry.IngressRoute.Parked, route);
        Assert.False(failed);
    }

    [Fact]
    public void CompleteMigration_DeliversParkedArrivalsInOrder_ThenRemovesTheAttempt()
    {
        var registry = new ZLinkActorJoinPrewarmRegistry();
        registry.Register("handoff-1", "actor-1", actorGeneration: 1);
        registry.ParkOrDeliver(
            "actor-1", 1, () => Frame("first"), onFailed: () => Assert.Fail());
        registry.ParkOrDeliver(
            "actor-1", 1, () => Frame("second"), onFailed: () => Assert.Fail());

        IReadOnlyList<ZLinkActorHandoffFrame>? delivered = null;
        registry.CompleteMigration("handoff-1", frames => delivered = frames);

        Assert.NotNull(delivered);
        Assert.Equal(["first", "second"], delivered!.Select(Body));

        //  The attempt no longer exists: ingress for the object now falls
        //  through to the caller's normal (post-migration) actor lookup.
        var route = registry.ParkOrDeliver(
            "actor-1", 1, () => Frame("late"), onFailed: () => Assert.Fail());
        Assert.Equal(ZLinkActorJoinPrewarmRegistry.IngressRoute.NotFound, route);
    }

    [Fact]
    public void CompleteMigration_WithNoParkedArrivals_DoesNotInvokeDeliver()
    {
        var registry = new ZLinkActorJoinPrewarmRegistry();
        registry.Register("handoff-1", "actor-1", actorGeneration: 1);

        var invoked = false;
        registry.CompleteMigration("handoff-1", _ => invoked = true);

        Assert.False(invoked);
    }

    [Fact]
    public void CompleteMigration_ForAnUnknownHandoff_Throws()
    {
        var registry = new ZLinkActorJoinPrewarmRegistry();

        Assert.Throws<InvalidOperationException>(
            () => registry.CompleteMigration("missing", _ => { }));
    }

    [Fact]
    public void Register_NewerHandoffForSameObject_EvictsTheOlderOne_FailingItsParkedArrivalsOnce()
    {
        var registry = new ZLinkActorJoinPrewarmRegistry();
        registry.Register("handoff-old", "actor-1", actorGeneration: 1);
        var failedCount = 0;
        registry.ParkOrDeliver(
            "actor-1", 1, () => Frame("stale"), onFailed: () => failedCount++);

        string? evicted = null;
        registry.Register(
            "handoff-new",
            "actor-1",
            actorGeneration: 1,
            onEvicted: id => evicted = id);

        Assert.Equal("handoff-old", evicted);
        Assert.Equal(1, failedCount);

        //  The evicted identity's late PREPARE must be discarded, not
        //  installed (spec 15 §4.2).
        Assert.Throws<InvalidOperationException>(
            () => registry.CompleteMigration("handoff-old", _ => { }));

        //  The newer identity owns the object now.
        var route = registry.ParkOrDeliver(
            "actor-1", 1, () => Frame("fresh"), onFailed: () => Assert.Fail());
        Assert.Equal(ZLinkActorJoinPrewarmRegistry.IngressRoute.Parked, route);
    }

    [Fact]
    public void Register_RetriedWithSameHandoffId_ReusesTheExistingAttempt()
    {
        var registry = new ZLinkActorJoinPrewarmRegistry();
        registry.Register("handoff-1", "actor-1", actorGeneration: 1);
        registry.ParkOrDeliver(
            "actor-1", 1, () => Frame("first"), onFailed: () => Assert.Fail());

        //  A retried admission of the same exact identity must not evict
        //  its own already-parked arrival.
        registry.Register("handoff-1", "actor-1", actorGeneration: 1);

        IReadOnlyList<ZLinkActorHandoffFrame>? delivered = null;
        registry.CompleteMigration("handoff-1", frames => delivered = frames);
        Assert.Equal(["first"], delivered!.Select(Body));
    }

    [Fact]
    public void Release_FailsAnyStillParkedArrival_ExactlyOnce()
    {
        var registry = new ZLinkActorJoinPrewarmRegistry();
        registry.Register("handoff-1", "actor-1", actorGeneration: 1);
        var failedCount = 0;
        registry.ParkOrDeliver(
            "actor-1", 1, () => Frame("expired"), onFailed: () => failedCount++);

        registry.Release("handoff-1");
        Assert.Equal(1, failedCount);

        //  Idempotent: releasing again must not fail it a second time.
        registry.Release("handoff-1");
        Assert.Equal(1, failedCount);
    }

    [Fact]
    public void Release_AfterSuccessfulMigration_IsANoOp()
    {
        var registry = new ZLinkActorJoinPrewarmRegistry();
        registry.Register("handoff-1", "actor-1", actorGeneration: 1);
        registry.ParkOrDeliver(
            "actor-1", 1, () => Frame("migrated"), onFailed: () => Assert.Fail());
        registry.CompleteMigration("handoff-1", _ => { });

        //  Must not throw and must not resurrect the attempt.
        registry.Release("handoff-1");
        Assert.True(registry.IsEmpty);
    }

    [Fact]
    public void DifferentObjectGenerations_AreIndependentAttempts()
    {
        var registry = new ZLinkActorJoinPrewarmRegistry();
        registry.Register("handoff-gen1", "actor-1", actorGeneration: 1);
        registry.Register("handoff-gen2", "actor-1", actorGeneration: 2);

        Assert.Equal(
            ZLinkActorJoinPrewarmRegistry.IngressRoute.Parked,
            registry.ParkOrDeliver(
                "actor-1", 1, () => Frame("g1"), onFailed: () => Assert.Fail()));
        Assert.Equal(
            ZLinkActorJoinPrewarmRegistry.IngressRoute.Parked,
            registry.ParkOrDeliver(
                "actor-1", 2, () => Frame("g2"), onFailed: () => Assert.Fail()));
    }

    private static ZLinkActorHandoffFrame Frame(string body) =>
        new(
            ReplyActorNodeRid: [],
            ReplyActorGeneration: 0,
            SourceNodeRid: [],
            SourceSessionRid: [],
            RequestId: 1,
            Flags: 0,
            Header: [],
            Body: System.Text.Encoding.UTF8.GetBytes(body),
            ArrivalIndex: 0);

    private static string Body(ZLinkActorHandoffFrame frame) =>
        System.Text.Encoding.UTF8.GetString(frame.Body);
}
