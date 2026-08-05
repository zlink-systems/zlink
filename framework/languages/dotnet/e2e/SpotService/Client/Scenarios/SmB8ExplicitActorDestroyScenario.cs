// Verifies SM-B8 Exact ActorRef destroy behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmB8ExplicitActorDestroyScenario
{
    public static async Task RunAsync(ZLinkHttpClient gateway)
    {
        var actorId = $"actor-sm-b8-destroy-{Guid.NewGuid():N}";
        var oldActor = (await gateway.Post("/actor/get-or-create")
            .Body(new EnsureActorReq(actorId, "destroy"))
            .Async<EnsureActorRes>()).Body;
        var oldRef = new ActorRefRes(oldActor.ActorId, oldActor.NodeRid, oldActor.Generation);

        var first = await DestroyAsync(gateway, oldRef);
        ZlinkStreamAssert.Ensure(
            first.Succeeded && first.Destroyed && string.IsNullOrEmpty(first.ErrorKind),
            $"SM-B8 current ActorRef destroy did not return true: success={first.Succeeded} "
            + $"destroyed={first.Destroyed} kind={first.ErrorKind}.");

        var repeated = await DestroyAsync(gateway, oldRef);
        ZlinkStreamAssert.Ensure(
            repeated.Succeeded && !repeated.Destroyed && string.IsNullOrEmpty(repeated.ErrorKind),
            $"SM-B8 repeated ActorRef destroy was not idempotent false: success={repeated.Succeeded} "
            + $"destroyed={repeated.Destroyed} kind={repeated.ErrorKind}.");

        var recreated = (await gateway.Post("/actor/get-or-create")
            .Body(new EnsureActorReq(actorId, "recreated"))
            .Async<EnsureActorRes>()).Body;
        var newRef = new ActorRefRes(recreated.ActorId, recreated.NodeRid, recreated.Generation);
        ZlinkStreamAssert.Ensure(
            recreated.ActorId == oldActor.ActorId
            && recreated.Generation != oldActor.Generation,
            "SM-B8 recreate did not create a new Actor incarnation.");

        var stale = await DestroyAsync(gateway, oldRef);
        ZlinkStreamAssert.Ensure(
            !stale.Succeeded && !stale.Destroyed && stale.ErrorKind == "InvalidOperation",
            $"SM-B8 old ActorRef after recreate was not rejected as InvalidOperation: "
            + $"success={stale.Succeeded} destroyed={stale.Destroyed} kind={stale.ErrorKind}.");

        var request = (await gateway.Post("/actor/request")
            .Body(new ActorRequestReq(newRef.ActorId, "recreated-request"))
            .Async<ActorRequestRes>()).Body;
        ZlinkStreamAssert.Ensure(
            request.Succeeded
            && request.Reply is { } reply
            && reply.ActorId == actorId,
            $"SM-B8 recreated Actor did not process a request: success={request.Succeeded} "
            + $"kind={request.ErrorKind}.");

        var cleanup = await DestroyAsync(gateway, newRef);
        ZlinkStreamAssert.Ensure(
            cleanup.Succeeded && cleanup.Destroyed,
            $"SM-B8 cleanup destroy failed: success={cleanup.Succeeded} destroyed={cleanup.Destroyed} "
            + $"kind={cleanup.ErrorKind}.");

        Console.WriteLine("operation SpotService.sm-b8 passed");
    }

    private static async Task<ActorRefDestroyRes> DestroyAsync(
        ZLinkHttpClient gateway,
        ActorRefRes actor) =>
        (await gateway.Post("/actor/destroy-ref")
            .Body(new ActorRefDestroyReq(actor))
            .Async<ActorRefDestroyRes>()).Body;
}
