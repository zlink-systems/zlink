// Verifies runtime placement weight, zero-weight exclusion, and capacity-first selection.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmG5AAndG5BPlacementScenario
{
    private const int WeightSampleCount = 800;

    public static async Task RunWeightDistributionAsync(
        ZLinkHttpClient gateway,
        ZLinkHttpClient playA,
        ZLinkHttpClient playB)
    {
        var suffix = Guid.NewGuid().ToString("N");
        try
        {
            await VerifyRuntimeWeightBoundsAsync(playA);
            await SetWeightsAsync(playA, playB, 100, 0);
            await WaitForActorOwnerAsync(gateway, "play-a", suffix);
            var existing = await CreateActorAsync(
                gateway,
                $"actor-sm-g5-existing-{suffix}");
            ZlinkStreamAssert.Ensure(
                IsNode(existing.NodeRid, "play-a"),
                "SM-G5 baseline Actor was not placed on play-a.");

            await SetWeightsAsync(playA, playB, 100, 300);
            var actorCounts = await CreateActorsAsync(gateway, suffix);
            var spotCounts = await CreateSpotsAsync(gateway, suffix);
            AssertRatio(actorCounts, "Actor");
            AssertRatio(spotCounts, "User Spot");

            var existingAfter = await FindActorAsync(gateway, existing.ActorId);
            ZlinkStreamAssert.Ensure(
                existingAfter is not null
                && existingAfter.NodeRid == existing.NodeRid
                && existingAfter.Generation == existing.Generation,
                "SM-G5 runtime weight update changed an existing Actor owner.");

            Console.WriteLine("operation SpotService.sm-g5a passed");
        }
        finally
        {
            await SetWeightsAsync(playA, playB, 100, 100);
        }
    }

    public static async Task RunCapacityEligibilityAsync(
        ZLinkHttpClient gateway,
        ZLinkHttpClient playA,
        ZLinkHttpClient playB)
    {
        var suffix = Guid.NewGuid().ToString("N");
        try
        {
            await VerifyRuntimeWeightBoundsAsync(playA);
            await SetWeightsAsync(playA, playB, 1, 10_000);

            var high = await CreateTypedSpotAsync(
                gateway,
                $"spot-sm-g5-capacity-high-{suffix}",
                SpotServiceNames.WeightCapacitySpotType);
            ZlinkStreamAssert.Ensure(
                IsNode(high.NodeRid, "play-b"),
                "SM-G5B did not fill play-b typed capacity first.");

            var fallback = await CreateTypedSpotAsync(
                gateway,
                $"spot-sm-g5-capacity-fallback-{suffix}",
                SpotServiceNames.WeightCapacitySpotType);
            ZlinkStreamAssert.Ensure(
                IsNode(fallback.NodeRid, "play-a"),
                "SM-G5B did not filter the full high-weight node before selection.");

            Console.WriteLine("operation SpotService.sm-g5b passed");
        }
        finally
        {
            await SetWeightsAsync(playA, playB, 100, 100);
        }
    }

    private static async Task VerifyRuntimeWeightBoundsAsync(ZLinkHttpClient playA)
    {
        foreach (var weight in new[] { 0, 100, 10_000 })
        {
            var result = (await playA.Post("/placement-weight/probe")
                .Body(new PlacementWeightReq(weight))
                .Async<PlacementWeightProbeRes>()).Body;
            ZlinkStreamAssert.Ensure(
                result.Accepted && result.Current == weight,
                $"SM-G5 runtime rejected valid placement weight {weight}.");
        }

        foreach (var weight in new[] { -1, 10_001 })
        {
            var result = (await playA.Post("/placement-weight/probe")
                .Body(new PlacementWeightReq(weight))
                .Async<PlacementWeightProbeRes>()).Body;
            ZlinkStreamAssert.Ensure(
                !result.Accepted
                && result.Current == 10_000
                && result.ErrorKind == "ZLinkConfigurationException",
                $"SM-G5 runtime mutated placement weight after rejecting {weight}.");
        }
    }

    private static async Task<Dictionary<string, int>> CreateActorsAsync(
        ZLinkHttpClient gateway,
        string suffix)
    {
        var counts = NewCounts();
        for (var index = 0; index < WeightSampleCount; index++)
        {
            var actor = await CreateActorAsync(
                gateway,
                $"actor-sm-g5-ratio-{suffix}-{index:D3}");
            counts[Node(actor.NodeRid)]++;
        }

        return counts;
    }

    private static async Task WaitForActorOwnerAsync(
        ZLinkHttpClient gateway,
        string expectedNode,
        string suffix)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(15);
        var attempt = 0;
        while (DateTimeOffset.UtcNow < deadline)
        {
            var actor = await CreateActorAsync(
                gateway,
                $"actor-sm-g5-converge-{suffix}-{attempt++:D3}");
            if (Node(actor.NodeRid) == expectedNode) return;
            await Task.Delay(TimeSpan.FromMilliseconds(100));
        }

        throw new TimeoutException(
            $"SM-G5 placement weight did not converge to '{expectedNode}'.");
    }

    private static async Task<Dictionary<string, int>> CreateSpotsAsync(
        ZLinkHttpClient gateway,
        string suffix)
    {
        var counts = NewCounts();
        for (var index = 0; index < WeightSampleCount; index++)
        {
            var spot = await CreateTypedSpotAsync(
                gateway,
                $"spot-sm-g5-ratio-{suffix}-{index:D3}",
                SpotServiceNames.UserSpotType);
            counts[Node(spot.NodeRid)]++;
        }

        return counts;
    }

    private static async Task<ActorRefRes> CreateActorAsync(
        ZLinkHttpClient gateway,
        string actorId)
    {
        var result = (await gateway.Post("/actor/manager-probe")
            .Body(new ActorManagerProbeReq("create", actorId))
            .Async<ActorManagerProbeRes>()).Body;
        ZlinkStreamAssert.Ensure(
            result.State == "Created" && result.Actor is not null,
            $"SM-G5 failed to create Actor '{actorId}'.");
        return result.Actor
            ?? throw new InvalidOperationException(
                $"SM-G5 Actor '{actorId}' was created without a reference.");
    }

    private static async Task<ActorRefRes?> FindActorAsync(
        ZLinkHttpClient gateway,
        string actorId)
    {
        var result = (await gateway.Post("/actor/manager-probe")
            .Body(new ActorManagerProbeReq("find", actorId))
            .Async<ActorManagerProbeRes>()).Body;
        return result.Actor;
    }

    private static async Task<CreateSpotRes> CreateTypedSpotAsync(
        ZLinkHttpClient gateway,
        string spotId,
        string spotType) =>
        (await gateway.Post("/spot/get-or-create-typed")
            .Body(new TypedSpotCreateReq(spotId, spotType))
            .Async<CreateSpotRes>()).Body;

    private static Dictionary<string, int> NewCounts() =>
        new(StringComparer.Ordinal)
        {
            ["play-a"] = 0,
            ["play-b"] = 0
        };

    private static void AssertRatio(
        IReadOnlyDictionary<string, int> counts,
        string objectKind)
    {
        ZlinkStreamAssert.Ensure(
            counts.Values.Sum() == WeightSampleCount
            && counts["play-b"] * 100 >= WeightSampleCount * 65
            && counts["play-b"] * 100 <= WeightSampleCount * 85,
            $"SM-G5 {objectKind} ratio was "
            + $"{counts["play-a"]}:{counts["play-b"]}, expected play-b between 65% and 85%.");
    }

    private static string Node(string nodeRid) =>
        IsNode(nodeRid, "play-a")
            ? "play-a"
            : IsNode(nodeRid, "play-b")
                ? "play-b"
                : throw new InvalidOperationException(
                    $"Unexpected SM-G5 owner RID '{nodeRid}'.");

    private static bool IsNode(string nodeRid, string prefix) =>
        nodeRid.StartsWith($"{prefix}-", StringComparison.Ordinal);

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
