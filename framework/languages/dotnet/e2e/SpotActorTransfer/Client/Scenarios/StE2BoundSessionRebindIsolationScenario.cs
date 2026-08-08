// Verifies ST-E2 Bound Session Rebind Isolation behavior.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StE2BoundSessionRebindIsolationScenario
{
    public static async Task RunAsync(SpotActorTransferScenarioContext context)
    {
        var actorId = $"actor-bound-session-rebind-{Guid.NewGuid():N}";
        var spotId = $"spot-bound-session-rebind-{Guid.NewGuid():N}";
        await context.CreateSpotAsync(context.NodeB, spotId);
        await context.CreateActorAsync(context.NodeA, actorId, SpotActorTransferNames.ActorTypeStateful, 92);
        var sourceRef = await context.GetActorRefAsync(context.NodeA, actorId);
        await using var oldSession = await context.ConnectAndBindAsync(context.Options.NodeAStreamEndpoint, "ST-E2", sourceRef);
        var guidanceReceived = 0;
        var closedBeforeGuidance = 0;
        var closed = new TaskCompletionSource<ZlinkStreamDisconnected>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        oldSession.Disconnected += (message, _) =>
        {
            if (Volatile.Read(ref guidanceReceived) == 0)
                Interlocked.Exchange(ref closedBeforeGuidance, 1);
            closed.TrySetResult(message);
            return ValueTask.CompletedTask;
        };
        var replacementGuidance = oldSession.WaitFor<ActorBindingReplacedNotify>()
            .Where(message =>
                message.Payload.ActorId == actorId
                && message.Payload.Marker == "callback-before-close")
            .Timeout(TimeSpan.FromSeconds(10))
            .Async()
            .AsTask();
        var beforeTransferPush = oldSession.WaitFor<BoundPushNotify>()
            .Where(message => message.Payload.Marker == "before-rebind-transfer")
            .Timeout(TimeSpan.FromSeconds(10))
            .Async().AsTask();
        await context.BoundPushAsync(context.NodeA, actorId, new BoundPushReq("ST-E2", "before-rebind-transfer"));
        await beforeTransferPush;

        var join = await context.JoinAsync(context.NodeA, actorId, new JoinTargetReq("ST-E2", spotId));
        ZlinkStreamAssert.Ensure(join.Accepted, "ST-E2 join was rejected.");
        //  Spec 15: `.Defer()` returns before the Join runs and the Accepted
        //  completion is delivered to the target Actor, so the reply above does
        //  not mean the relocation finished. Rebinding while it is still in
        //  flight moves the bound session under the route seal, which then
        //  seals the wrong node. Wait for the target's completion first.
        await context.WaitEvidenceAsync(context.NodeB, [
            $"ST-E2|{actorId}|success_reply|{spotId}"
        ]);
        var targetRef = await context.GetActorRefAsync(context.NodeB, actorId);
        await using var newSession = await context.ConnectAndBindAsync(context.Options.NodeBStreamEndpoint, "ST-E2", targetRef);

        var oldPush = oldSession.ExpectNone<BoundPushNotify>()
            .Within(TimeSpan.FromMilliseconds(500))
            .Async().AsTask();
        var newPush = newSession.WaitFor<BoundPushNotify>()
            .Where(message => message.Payload.Marker == "after-rebind")
            .Timeout(TimeSpan.FromSeconds(10))
            .Async().AsTask();
        var pushReply = await context.BoundPushAsync(context.NodeB, actorId, new BoundPushReq("ST-E2", "after-rebind"));
        var guidance = await replacementGuidance;
        Interlocked.Exchange(ref guidanceReceived, 1);
        var disconnected = await closed.Task.WaitAsync(TimeSpan.FromSeconds(10));
        var notify = await newPush;
        ZlinkStreamAssert.Ensure(SpotActorTransferScenarioContext.IsNode(pushReply.NodeRid, "actor-b"), $"ST-E2 bound push reply expected actor-b, got {pushReply.NodeRid}.");
        ZlinkStreamAssert.Ensure(notify.Payload.Marker == "after-rebind", "ST-E2 new bound session notify marker mismatch.");
        ZlinkStreamAssert.Ensure(guidance.Payload.ActorId == actorId, "ST-E2 replacement guidance actor mismatch.");
        ZlinkStreamAssert.Ensure(
            Volatile.Read(ref closedBeforeGuidance) == 0,
            "ST-E2 retired session closed before callback guidance reached the client.");
        Console.WriteLine(
            $"ST-E2 callback guidance received before close actor={actorId} "
            + $"close_reason={disconnected.CloseReason}");
        await oldPush;
    }
}
