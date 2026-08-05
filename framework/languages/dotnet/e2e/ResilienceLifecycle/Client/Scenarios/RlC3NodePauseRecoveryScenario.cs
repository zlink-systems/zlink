// Verifies RL-C3 normal process stop and topology recovery.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-C3 verifies normal SIGTERM restart with a new owner generation.
internal static class RlC3NodePauseRecoveryScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient consumer,
        ZLinkHttpClient registry,
        ResilienceProcessManager processes,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        var oldRows = (await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 1))
            .Async<TopologyEntryRes[]>()).Body;
        var oldGeneration = oldRows.Single().Generation;

        await processes.StopProviderBWithSigtermAsync();
        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 0))
            .Async<TopologyEntryRes[]>();
        await ProviderTrafficProbe.WaitUntilProviderExcludedAsync(
            consumer,
            "api-b",
            "rl-c3-converge",
            "RL-C3");

        var during = (await consumer.Post("/profile/request")
            .Body(new ProfileReq("fast", "rl-c3-during-down"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(during.ProviderRid == "api-a", "RL-C3 did not use surviving provider during node down.");

        var connectionCount = (await consumer.Get("/connections").Async<string[]>()).Body.Length;
        var restarted = await processes.StartProviderBAsync();
        var recoveredRows = (await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 1))
            .Async<TopologyEntryRes[]>()).Body;
        ZlinkStreamAssert.Ensure(
            recoveredRows.Length == 1 && recoveredRows[0].Generation != oldGeneration,
            "RL-C3 did not converge to exactly one new owner generation.");
        await consumer.Post("/connections/wait")
            .Body(new ConnectionWaitReq(
                ["kind=ConnectionReady", $"remote={restarted.Endpoint}"], connectionCount))
            .Async<string[]>();
        await ProviderTrafficProbe.DriveUntilProviderServesAsync(
            consumer,
            providerB,
            "rl-c3-recovered",
            "RL-C3",
            "profile-request|rid=api-b|marker=rl-c3-recovered-");

        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            var waitA = providerA.Post("/evidence/wait").Body(new EvidenceWaitReq(["marker=rl-c3-during-down"], []))
                .Async<string[]>(timeout.Token).AsTask();
            var waitB = providerB.Post("/evidence/wait").Body(new EvidenceWaitReq(["marker=rl-c3-during-down"], []))
                .Async<string[]>(timeout.Token).AsTask();
            var completed = await Task.WhenAny(waitA, waitB);
            var evidence = (await completed).Body;
            timeout.Cancel();
            ZlinkStreamAssert.Ensure(
                evidence.Any(line => line.Contains("marker=rl-c3-during-down", StringComparison.Ordinal)),
                "RL-C3 did not record expected evidence 'marker=rl-c3-during-down'.");
        }
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            var waitA = providerA.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(["profile-request|rid=api-b|marker=rl-c3-recovered-"], []))
                .Async<string[]>(timeout.Token).AsTask();
            var waitB = providerB.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(["profile-request|rid=api-b|marker=rl-c3-recovered-"], []))
                .Async<string[]>(timeout.Token).AsTask();
            var completed = await Task.WhenAny(waitA, waitB);
            var evidence = (await completed).Body;
            timeout.Cancel();
            ZlinkStreamAssert.Ensure(
                evidence.Any(line =>
                    line.Contains("profile-request|rid=api-b|marker=rl-c3-recovered-", StringComparison.Ordinal)),
                "RL-C3 did not record expected evidence 'marker=rl-c3-recovered-'.");
        }

        Console.WriteLine("scenario RL-C3 passed");
    }

}
