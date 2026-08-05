// Verifies SM-G2 scale-out while Framework placement selects every owner.
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmG2OwnerSpotRemapScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        ZLinkHttpClient gateway)
    {
        var suffix = Guid.NewGuid().ToString("N");
        var existingSpotRid = $"spot-sm-g2-existing-{suffix}";
        var existingActorId = $"actor-sm-g2-existing-{suffix}";
        var newActorId = $"actor-sm-g2-new-{suffix}";
        var newSpotRid = $"spot-sm-g2-new-{suffix}";

        await gateway.Post("/node/wait-ready")
            .Body(new NodeReadinessWaitReq("play-a"))
            .Async<NodeReadinessWaitRes>();
        var existingSpot = (await gateway.Post("/spot/get-or-create")
            .Body(new CreateSpotReq(existingSpotRid)).Async<CreateSpotRes>()).Body;
        var existingActor = (await gateway.Post("/actor/get-or-create")
            .Body(new EnsureActorReq(existingActorId, "existing"))
            .Async<EnsureActorRes>()).Body;
        ZlinkStreamAssert.Ensure(
            IsNode(existingSpot.NodeRid, "play-a")
            && IsNode(existingActor.NodeRid, "play-a"),
            "SM-G2 baseline owner was not placed on play-a.");

        Console.WriteLine("spot-service sm-g2 scale-out-ready");
        var readiness = (await gateway.Post("/node/wait-ready")
            .Body(new NodeReadinessWaitReq("play-b"))
            .Async<NodeReadinessWaitRes>()).Body;
        ZlinkStreamAssert.Ensure(readiness.PeerReady,
            "SM-G2 play-b peer readiness did not converge.");

        await SetPlacementWeightsAsync(playA, playB, 0, 100);
        try
        {
            await WaitForPlacementAsync(gateway, "play-b");
            var existingFollowUp = (await gateway.Post("/spot/route-state")
                .Body(new SpotStateRouteReq(existingSpotRid, "add", 1))
                .Async<StateRes>()).Body;
            var newActor = (await gateway.Post("/actor/get-or-create")
                .Body(new EnsureActorReq(newActorId, "new"))
                .Async<EnsureActorRes>()).Body;
            var newSpot = (await gateway.Post("/spot/get-or-create")
                .Body(new CreateSpotReq(newSpotRid)).Async<CreateSpotRes>()).Body;

            ZlinkStreamAssert.Ensure(IsNode(existingFollowUp.NodeRid, "play-a"),
                "SM-G2 scale-out changed the existing Spot owner.");
            ZlinkStreamAssert.Ensure(IsNode(newActor.NodeRid, "play-b"),
                "SM-G2 Actor manager did not use the only eligible placement target.");
            ZlinkStreamAssert.Ensure(IsNode(newSpot.NodeRid, "play-b"),
                "SM-G2 Spot manager did not use the only eligible placement target.");
            Console.WriteLine("operation SpotService.sm-g2 passed");
        }
        finally
        {
            await SetPlacementWeightsAsync(playA, playB, 100, 100);
        }
    }

    private static async Task SetPlacementWeightsAsync(
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
    }

    private static bool IsNode(string actualRid, string expectedPrefix) =>
        string.Equals(actualRid, expectedPrefix, StringComparison.Ordinal)
        || actualRid.StartsWith($"{expectedPrefix}-", StringComparison.Ordinal);

    private static async Task WaitForPlacementAsync(
        ZLinkHttpClient gateway,
        string expectedNodePrefix)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(15);
        while (DateTimeOffset.UtcNow < deadline)
        {
            var actorId = $"actor-sm-g2-placement-probe-{Guid.NewGuid():N}";
            var placed = (await gateway.Post("/actor/get-or-create")
                .Body(new EnsureActorReq(actorId, "placement-probe"))
                .Async<EnsureActorRes>()).Body;
            if (IsNode(placed.NodeRid, expectedNodePrefix)) return;
            await Task.Delay(TimeSpan.FromMilliseconds(100));
        }

        throw new TimeoutException(
            $"SM-G2 placement did not converge to '{expectedNodePrefix}'.");
    }
}
