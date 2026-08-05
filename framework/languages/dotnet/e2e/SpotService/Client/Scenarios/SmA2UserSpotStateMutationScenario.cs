// Verifies SM-A2 User Spot State Mutation behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmA2UserSpotStateMutationScenario
{
    public static async Task RunAsync(ZLinkHttpClient api)
    {
        var spotRid = $"spot-sm-a2-{Guid.NewGuid():N}";
        await api.Post("/spot/create").Body(new CreateSpotReq(spotRid)).Async<CreateSpotRes>();

        var first = (await api.Post("/spot/state/request")
            .Body(new SpotStateRouteReq(spotRid, "add", 2))
            .Async<StateRes>()).Body;
        var second = (await api.Post("/spot/state/request")
            .Body(new SpotStateRouteReq(spotRid, "add", 3))
            .Async<StateRes>()).Body;
        ZlinkStreamAssert.Ensure(first.Value == 2 && second.Value == 5, "SM-A2 sequential mutations lost order.");

        var concurrent = await Task.WhenAll(Enumerable.Range(0, 4).Select(_ => api.Post("/spot/state/request")
            .Body(new SpotStateRouteReq(spotRid, "add", 1))
            .Async<StateRes>()
            .AsTask()));
        var values = concurrent.Select(response => response.Body.Value).Order().ToArray();
        ZlinkStreamAssert.Ensure(
            values.SequenceEqual(new[] { 6, 7, 8, 9 }),
            "SM-A2 concurrent mutations were not serialized without lost updates.");

        var expectedEvidence = new[] { $"spot-state-request|rid=play-a|spot={spotRid}|value=9" };
        var evidence = (await api.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(expectedEvidence))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            expectedEvidence.All(expected => evidence.Any(line => line.Contains(expected, StringComparison.Ordinal))),
            "SM-A2 state mutation evidence mismatch.");
        ZlinkStreamAssert.Ensure(
            evidence.Any(line =>
                line.Contains($"spot-state-request|rid=play-a|spot={spotRid}|value=9",
                    StringComparison.Ordinal)),
            "SM-A2 state mutation did not preserve order.");
        Console.WriteLine("operation SpotService.sm-a2 passed");
    }
}
