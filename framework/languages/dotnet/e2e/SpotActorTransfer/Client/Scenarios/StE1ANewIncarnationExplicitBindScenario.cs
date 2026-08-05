// Verifies ST-E1A new-incarnation explicit binding behavior.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StE1ANewIncarnationExplicitBindScenario
{
    public static async Task RunAsync(SpotActorTransferScenarioContext context)
    {
        const string scenario = "ST-E1A";
        var actorId = $"actor-new-incarnation-{Guid.NewGuid():N}";
        var otherActorId = $"actor-binding-control-{Guid.NewGuid():N}";
        await context.CreateActorAsync(
            context.NodeA,
            actorId,
            SpotActorTransferNames.ActorTypeStateful,
            101);
        await context.CreateActorAsync(
            context.NodeA,
            otherActorId,
            SpotActorTransferNames.ActorTypeStateful,
            202);
        var generationOne = await context.GetActorRefAsync(context.NodeA, actorId);
        var other = await context.GetActorRefAsync(context.NodeA, otherActorId);

        await using var session = await context.ConnectAndBindAsync(
            context.Options.NodeAStreamEndpoint,
            scenario,
            generationOne);
        await SpotActorTransferScenarioContext.BindAsync(session, scenario, other);
        var initialBindings =
            await SpotActorTransferScenarioContext.GetSessionBindingsAsync(
                session,
                scenario);
        AssertBinding(initialBindings, generationOne);
        AssertBinding(initialBindings, other);

        var destroyed = await context.DestroyActorAsync(context.NodeA, actorId);
        ZlinkStreamAssert.Ensure(
            destroyed.Destroyed && destroyed.Generation == generationOne.Generation,
            $"{scenario} did not destroy generation {generationOne.Generation}.");
        await context.CreateActorAsync(
            context.NodeA,
            actorId,
            SpotActorTransferNames.ActorTypeStateful,
            303);
        var generationTwo = await context.GetActorRefAsync(context.NodeA, actorId);
        ZlinkStreamAssert.Ensure(
            generationTwo.Generation > generationOne.Generation,
            $"{scenario} recreate did not issue a new ObjectGeneration.");

        var beforeExplicitBind =
            await SpotActorTransferScenarioContext.GetSessionBindingsAsync(
                session,
                scenario);
        ZlinkStreamAssert.Ensure(
            beforeExplicitBind.Bindings.All(binding =>
                !string.Equals(binding.ActorId, actorId, StringComparison.Ordinal)
                || binding.Generation != generationTwo.Generation),
            $"{scenario} automatically retargeted the old binding to the new incarnation.");
        AssertBinding(beforeExplicitBind, other);

        ZlinkStreamException? stale = null;
        try
        {
            _ = await session.Request(new BoundPushReq(
                    scenario,
                    "stale-generation",
                    actorId))
                .Async<BoundPushRes>();
        }
        catch (ZlinkStreamException error)
        {
            stale = error;
        }
        ZlinkStreamAssert.Ensure(
            stale?.Error.Code == ZlinkStreamErrorCode.RemoteError
            && stale.Error.Message.StartsWith(
                "NotFound:",
                StringComparison.Ordinal),
            $"{scenario} expected NotFound from the old binding, got '{stale?.Error.Message}'.");

        await SpotActorTransferScenarioContext.BindAsync(
            session,
            scenario,
            generationTwo);
        var afterExplicitBind =
            await SpotActorTransferScenarioContext.GetSessionBindingsAsync(
                session,
                scenario);
        AssertBinding(afterExplicitBind, generationTwo);
        AssertBinding(afterExplicitBind, other);
        ZlinkStreamAssert.Ensure(
            afterExplicitBind.Bindings.All(binding =>
                !string.Equals(binding.ActorId, actorId, StringComparison.Ordinal)
                || binding.Generation != generationOne.Generation),
            $"{scenario} retained the old incarnation after explicit bind.");

        var newNotify = session.WaitFor<BoundPushNotify>()
            .Where(message => message.Payload.Marker == "new-incarnation")
            .Timeout(TimeSpan.FromSeconds(10))
            .Async().AsTask();
        var otherNotify = session.WaitFor<BoundPushNotify>()
            .Where(message => message.Payload.Marker == "control-binding")
            .Timeout(TimeSpan.FromSeconds(10))
            .Async().AsTask();
        await context.BoundPushAsync(
            context.NodeA,
            actorId,
            new BoundPushReq(scenario, "new-incarnation"));
        await context.BoundPushAsync(
            context.NodeA,
            otherActorId,
            new BoundPushReq(scenario, "control-binding"));
        var deliveredNew = await newNotify;
        var deliveredOther = await otherNotify;
        ZlinkStreamAssert.Ensure(
            deliveredNew.Payload.StateVersion == 303,
            $"{scenario} explicit binding did not deliver from the new incarnation.");
        ZlinkStreamAssert.Ensure(
            deliveredOther.Payload.StateVersion == 202,
            $"{scenario} changed the other Actor binding.");
    }

    private static void AssertBinding(
        SessionBindingsRes bindings,
        ActorRefRes expected)
    {
        ZlinkStreamAssert.Ensure(
            bindings.Bindings.Any(binding =>
                binding.ActorId == expected.ActorId
                && binding.NodeRid == expected.NodeRid
                && binding.Generation == expected.Generation),
            $"ST-E1A binding '{expected.ActorId}' generation {expected.Generation} was not preserved.");
    }
}
