// Verifies ST-E1 Bound Session Push After Transfer behavior.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StE1BoundSessionPushAfterTransferScenario
{
    public static async Task RunAsync(SpotActorTransferScenarioContext context)
    {
        var actorId = $"actor-bound-session-{Guid.NewGuid():N}";
        var spotId = $"spot-bound-session-{Guid.NewGuid():N}";
        await context.CreateSpotAsync(context.NodeB, spotId);
        await context.CreateActorAsync(context.NodeA, actorId, SpotActorTransferNames.ActorTypeStateful, 91);
        var sourceRef = await context.GetActorRefAsync(context.NodeA, actorId);
        await using var bound = await context.ConnectAndBindAsync(context.Options.NodeAStreamEndpoint, "ST-E1", sourceRef);
        var beforeTransferPush = bound.WaitFor<BoundPushNotify>()
            .Where(message => message.Payload.Marker == "before-transfer")
            .Timeout(TimeSpan.FromSeconds(10))
            .Async().AsTask();
        await context.BoundPushAsync(context.NodeA, actorId, new BoundPushReq("ST-E1", "before-transfer"));
        await beforeTransferPush;

        var join = await context.JoinAsync(context.NodeA, actorId, new JoinTargetReq("ST-E1", spotId));
        ZlinkStreamAssert.Ensure(join.Accepted, "ST-E1 join was rejected.");
        await context.WaitEvidenceAsync(context.NodeB, [
            $"ST-E1|{actorId}|success_reply|{spotId}"
        ]);
        var targetRef = await context.GetActorRefAsync(context.NodeB, actorId);
        ZlinkStreamAssert.Ensure(
            SpotActorTransferScenarioContext.IsNode(targetRef.NodeRid, "actor-b"),
            $"ST-E1 target route expected actor-b, got {targetRef.NodeRid}.");
        ZlinkStreamAssert.Ensure(
            targetRef.Generation == sourceRef.Generation,
            $"ST-E1 relocation changed ObjectGeneration from {sourceRef.Generation} to {targetRef.Generation}.");
        var pushed = bound.WaitFor<BoundPushNotify>()
            .Where(message => message.Payload.Marker == "after-remote-transfer")
            .Timeout(TimeSpan.FromSeconds(10))
            .Async().AsTask();
        var pushReply = await context.BoundPushAsync(context.NodeB, actorId, new BoundPushReq("ST-E1", "after-remote-transfer"));
        var notify = await pushed;
        ZlinkStreamAssert.Ensure(SpotActorTransferScenarioContext.IsNode(pushReply.NodeRid, "actor-b"), $"ST-E1 bound push reply expected actor-b, got {pushReply.NodeRid}.");
        ZlinkStreamAssert.Ensure(SpotActorTransferScenarioContext.IsNode(notify.Payload.NodeRid, "actor-b"), $"ST-E1 bound push notify expected actor-b, got {notify.Payload.NodeRid}.");
        ZlinkStreamAssert.Ensure(notify.Payload.StateVersion == 91, $"ST-E1 bound push state expected 91, got {notify.Payload.StateVersion}.");
    }
}
