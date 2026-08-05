// Verifies SM-B0A concurrent actor creation converges to one durable actor.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmB0AActorCreationRaceScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient gateway,
        ZLinkHttpClient playA,
        ZLinkHttpClient playB)
    {
        var actorId = $"actor-sm-b0a-{Guid.NewGuid():N}";
        try
        {
            await SetWeightsAsync(playA, playB, 100, 0);
            var result = (await gateway.Post("/actor/create-race")
                .Body(new ActorCreateRaceReq(actorId))
                .Async<ActorCreateRaceRes>()).Body;
            ZlinkStreamAssert.Ensure(
                result.FirstState == "Rejected"
                && result.FirstReply == "rejected:first"
                && result.SecondState == "Created"
                && result.SecondActor is not null
                && result.FinalActor is not null
                && SameIdentity(result.SecondActor, result.FinalActor),
                "SM-B0A concurrent Actor create results did not remain operation-specific.");
            ZlinkStreamAssert.Ensure(
                result.SecondActor!.NodeRid.StartsWith("play-a-", StringComparison.Ordinal),
                "SM-B0A accepted Actor was not placed on play-a.");

            var evidence = (await playA.Get("/evidence").Async<string[]>()).Body;
            ZlinkStreamAssert.Ensure(
                evidence.Count(line => line.Contains($"entry-create-rejected|rid=play-a|actor={actorId}", StringComparison.Ordinal)) == 1
                && evidence.Count(line => line.Contains($"entry-created|rid=play-a|actor={actorId}", StringComparison.Ordinal)) == 1
                && evidence.All(line => !line.Contains($"actor-destroyed|actor={actorId}", StringComparison.Ordinal)),
                "SM-B0A rejected operation left handler or destroy evidence.");
            Console.WriteLine("operation SpotService.sm-b0a passed");
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
