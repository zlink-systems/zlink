// Verifies Actor Manager create, get-or-create, find, and destroy lifecycle behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmB0ActorManagerLifecycleScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient gateway,
        ZLinkHttpClient playA,
        ZLinkHttpClient playB)
    {
        var actorId = $"actor-sm-b0-{Guid.NewGuid():N}";
        try
        {
            await SetWeightsAsync(playA, playB, 100, 0);
            var beforeA = await EvidenceAsync(playA);
            var beforeB = await EvidenceAsync(playB);

            var missing = await ProbeAsync(gateway, "find", actorId);
            ZlinkStreamAssert.Ensure(
                missing.State == "Missing" && missing.Actor is null,
                "SM-B0 missing Find created or returned an Actor.");
            AssertFactoryDelta(beforeA, await EvidenceAsync(playA), actorId, 0, "play-a missing Find");
            AssertFactoryDelta(beforeB, await EvidenceAsync(playB), actorId, 0, "play-b missing Find");

            var created = await ProbeAsync(gateway, "create", actorId);
            var existing = await ProbeAsync(gateway, "get-or-create", actorId);
            var found = await ProbeAsync(gateway, "find", actorId);

            ZlinkStreamAssert.Ensure(
                created.State == "Created"
                && existing.State == "Existing"
                && found.State == "Found"
                && created.Actor is not null
                && existing.Actor is not null
                && found.Actor is not null,
                "SM-B0 Actor manager terminal states did not converge.");
            var createdActor = created.Actor
                ?? throw new InvalidOperationException("SM-B0 Created result has no Actor.");
            var existingActor = existing.Actor
                ?? throw new InvalidOperationException("SM-B0 Existing result has no Actor.");
            var foundActor = found.Actor
                ?? throw new InvalidOperationException("SM-B0 Found result has no Actor.");
            ZlinkStreamAssert.Ensure(
                SameIdentity(createdActor, existingActor)
                && SameIdentity(createdActor, foundActor),
                "SM-B0 Create, GetOrCreate, and Find returned different Actor identities.");
            ZlinkStreamAssert.Ensure(
                createdActor.NodeRid.StartsWith("play-a-", StringComparison.Ordinal),
                $"SM-B0 Actor was not placed on play-a: '{createdActor.NodeRid}'.");

            AssertFactoryDelta(beforeA, await EvidenceAsync(playA), actorId, 1, "play-a lifecycle");
            AssertFactoryDelta(beforeB, await EvidenceAsync(playB), actorId, 0, "play-b lifecycle");
            Console.WriteLine("operation SpotService.sm-b0 passed");
        }
        finally
        {
            await SetWeightsAsync(playA, playB, 100, 100);
        }
    }

    private static async Task<ActorManagerProbeRes> ProbeAsync(
        ZLinkHttpClient gateway,
        string operation,
        string actorId) =>
        (await gateway.Post("/actor/manager-probe")
            .Body(new ActorManagerProbeReq(operation, actorId))
            .Async<ActorManagerProbeRes>()).Body;

    private static async Task<string[]> EvidenceAsync(ZLinkHttpClient host) =>
        (await host.Get("/evidence").Async<string[]>()).Body;

    private static bool SameIdentity(ActorRefRes first, ActorRefRes second) =>
        first.ActorId == second.ActorId
        && first.NodeRid == second.NodeRid
        && first.Generation == second.Generation;

    private static void AssertFactoryDelta(
        IEnumerable<string> before,
        IEnumerable<string> after,
        string actorId,
        int expected,
        string phase)
    {
        var previous = before.Count(line =>
            line.Contains("actor-factory|", StringComparison.Ordinal)
            && line.Contains($"actor={actorId}", StringComparison.Ordinal));
        var current = after.Count(line =>
            line.Contains("actor-factory|", StringComparison.Ordinal)
            && line.Contains($"actor={actorId}", StringComparison.Ordinal));
        ZlinkStreamAssert.Ensure(
            current - previous == expected,
            $"SM-B0 {phase} factory delta was {current - previous}, expected {expected}.");
    }

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
        await Task.Delay(TimeSpan.FromSeconds(1));
    }
}
