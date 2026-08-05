using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;

namespace Zlink.Framework.Locations.Redis.Tests;

public sealed class ActorLifecycleContractTests
{
    [Fact]
    public void Entry_And_User_Spot_Lifecycle_Surfaces_Are_Separated()
    {
        var entryMethods = ContractMethods(typeof(IZLinkEntrySpot<TestActor>))
            .Select(static method => method.Name)
            .ToHashSet(StringComparer.Ordinal);
        var userMethods = ContractMethods(typeof(IZLinkSpot<TestActor>))
            .Select(static method => method.Name)
            .ToHashSet(StringComparer.Ordinal);

        Assert.DoesNotContain("OnActorJoinAsync", entryMethods);
        Assert.Contains("OnCreateActorAsync", entryMethods);
        Assert.DoesNotContain("OnActorRelocatedAsync", entryMethods);
        Assert.Contains("OnActorJoinAsync", userMethods);
    }

    private static IEnumerable<System.Reflection.MethodInfo> ContractMethods(
        Type contract)
    {
        return contract.GetMethods().Concat(
            contract.GetInterfaces().SelectMany(static type => type.GetMethods()));
    }

    [Fact]
    public void Actor_Create_Response_And_Terminal_Result_Preserve_Rejection()
    {
        var reply = ZLinkMessage.From("rejected");
        var response = ZLinkActorCreateResponse.Reject(reply);

        Assert.False(response.Accepted);
        Assert.Same(reply, response.Reply);

        var terminal = new ZLinkActorCreateResult.Rejected(response.Reply);
        Assert.Same(reply, terminal.Reply);
    }

    private sealed class TestActor : IZLinkActor
    {
        public string ActorId => "actor";

        public IZLinkActorContext Context => null!;
    }
}
