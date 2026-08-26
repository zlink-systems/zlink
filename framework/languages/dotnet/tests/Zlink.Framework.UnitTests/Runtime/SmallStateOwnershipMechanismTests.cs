using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Identifiers;

namespace Zlink.Framework.UnitTests;

public sealed class SmallStateOwnershipMechanismTests
{
    [Fact]
    public void BoundSessionRegistry_ReplacesMatchingEntry_AndCleansUpRegisteredEntries()
    {
        var unbound = new List<(string ActorId, string BindingToken)>();
        var registry = new ZLinkActorBoundSessionRegistry(
            (actorId, bindingToken) => unbound.Add((actorId, bindingToken)));
        var sessionRid = RoutingId.From("session-a");
        var bindingToken = ZLinkActorBoundSessionBindingToken.Native(sessionRid);

        registry.Register("actor-a", sessionRid, bindingToken);
        registry.Register("actor-a", sessionRid, bindingToken);
        registry.Register("actor-b", sessionRid, "native:session-b");
        registry.Unregister("actor-a", bindingToken);
        registry.Cleanup(sessionRid);

        Assert.Equal([("actor-b", "native:session-b")], unbound);
    }

    [Fact]
    public void ActivationConcurrencyAdmission_EnforcesLimit_AndPreservesCallbacks()
    {
        var observed = new List<int>();
        var admission = new ZLinkActivationConcurrencyAdmission(1, observed.Add);

        admission.Acquire("actor-a");
        var exhausted = Assert.Throws<ZLinkFrameworkException>(
            () => admission.Acquire("actor-b"));
        admission.Release();

        Assert.Equal(ZLinkFrameworkErrorKind.CapacityExceeded, exhausted.Kind);
        Assert.Equal(ZLinkRetryAdvice.RetryAfterBackoff, exhausted.RetryAdvice);
        Assert.Equal(0, admission.Active);
        Assert.Equal([1, 0], observed);
        Assert.Throws<InvalidOperationException>(admission.Release);
    }

    [Fact]
    public void SpotActorMembership_PreservesIdentityAndSortedSnapshot()
    {
        var membership = new ZLinkSpotActorMembership();
        var actorB = new TestActor("actor-b");
        var actorA = new TestActor("actor-a");

        membership.Add(actorB);
        membership.Add(actorA);
        membership.Add(actorA);

        Assert.True(membership.TryGetActor(
            ZLinkActorId.FromBoundary("actor-a", "actorId"), out var resolved));
        Assert.Same(actorA, resolved);
        Assert.Equal<IZLinkActor>([actorA, actorB], membership.Snapshot());
        Assert.Throws<InvalidOperationException>(() => membership.Add(new TestActor("actor-a")));

        membership.RemoveIfCurrent(actorA);
        Assert.False(membership.TryGetActor(
            ZLinkActorId.FromBoundary("actor-a", "actorId"), out _));
        Assert.Equal(1, membership.Count);
    }
}
