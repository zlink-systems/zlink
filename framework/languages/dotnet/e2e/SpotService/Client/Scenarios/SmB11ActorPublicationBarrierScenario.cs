// Verifies SM-B11 Actor readiness follows initial factory and Entry membership.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmB11ActorPublicationBarrierScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient gateway,
        ZLinkHttpClient playA,
        ZLinkHttpClient playB)
    {
        var actorId = $"actor-sm-b11-{Guid.NewGuid():N}";
        try
        {
            await SetWeightsAsync(playA, playB, 100, 0);
            await gateway.Post("/actor/b11/start")
                .Body(new ActorRefReq(actorId))
                .Async<ActorManagerProbeRes>();
            await playA.Post("/evidence/wait")
                .Body(new EvidenceWaitReq([
                    $"actor-factory-started|rid=play-a|actor={actorId}"
                ]))
                .Async<string[]>();

            var beforeFind = (await gateway.Post("/actor/manager-probe")
                .Body(new ActorManagerProbeReq("find", actorId))
                .Async<ActorManagerProbeRes>()).Body;
            var beforeRequest = (await gateway.Post("/actor/request")
                .Body(new ActorRequestReq(actorId, "before-release", TimeoutMilliseconds: 1000))
                .Async<ActorRequestRes>()).Body;
            ZlinkStreamAssert.Ensure(
                beforeFind.State == "Missing"
                && beforeFind.Actor is null
                && !beforeRequest.Succeeded,
                "SM-B11 exposed or served an Actor before factory completion.");

            Console.WriteLine("spot-service sm-b11 release-play-a-ready");
            var created = (await gateway.Post("/actor/b11/status")
                .Body(new ActorRefReq(actorId))
                .Async<ActorManagerProbeRes>()).Body;
            ZlinkStreamAssert.Ensure(
                created.State == "Created" && created.Actor is not null,
                "SM-B11 Actor creation did not complete after factory release.");
            var createdActor = created.Actor
                ?? throw new InvalidOperationException("SM-B11 Created result has no Actor reference.");

            var afterFind = (await gateway.Post("/actor/manager-probe")
                .Body(new ActorManagerProbeReq("find", actorId))
                .Async<ActorManagerProbeRes>()).Body;
            var afterRequest = (await gateway.Post("/actor/request")
                .Body(new ActorRequestReq(actorId, "after-release"))
                .Async<ActorRequestRes>()).Body;
            ZlinkStreamAssert.Ensure(
                afterFind.State == "Found"
                && afterFind.Actor is not null
                && SameIdentity(createdActor, afterFind.Actor)
                && afterRequest.Succeeded
                && afterRequest.Reply is not null,
                "SM-B11 Find/request did not converge after initial membership completed.");
            Console.WriteLine("operation SpotService.sm-b11 passed");
        }
        finally
        {
            await SetWeightsAsync(playA, playB, 100, 100);
        }
    }

    private static bool SameIdentity(ActorRefRes first, ActorRefRes second) =>
        first.ActorId == second.ActorId
        && first.NodeRid == second.NodeRid
        && first.Generation == second.Generation;

    private static async Task SetWeightsAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        int playAWeight,
        int playBWeight)
    {
        await playA.Post("/placement-weight")
            .Body(new PlacementWeightReq(playAWeight))
            .Async<PlacementWeightRes>();
        await playB.Post("/placement-weight")
            .Body(new PlacementWeightReq(playBWeight))
            .Async<PlacementWeightRes>();
        await Task.Delay(TimeSpan.FromSeconds(2));
    }
}
