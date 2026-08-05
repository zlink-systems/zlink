// Verifies RL-C2 Topology Recovery behavior.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-C2 verifies topology state changes and request recovery after provider crash.
internal static class RlC2TopologyRecoveryScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient consumer,
        ZLinkHttpClient registry,
        ResilienceProcessManager processes,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        await processes.KillProviderBAsync();
        await WaitUntilAsync(async () => !await IsHealthyAsync(providerB), "RL-C2 expected api-b crash.");
        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 0))
            .Async<TopologyEntryRes[]>();
        await ProviderTrafficProbe.WaitUntilProviderExcludedAsync(
            consumer,
            "api-b",
            "rl-c2-converge",
            "RL-C2");

        for (var i = 0; i < 8; i++)
        {
            var reply = (await consumer.Post("/profile/request")
                .Body(new ProfileReq("fast", $"rl-c2-after-crash-{i}"))
                .Async<ProfileRes>()).Body;
            ZlinkStreamAssert.Ensure(reply.ProviderRid == "api-a", "RL-C2 request used stale crashed api-b.");
        }

        var connectionCount = (await consumer.Get("/connections").Async<string[]>()).Body.Length;
        var restarted = await processes.StartProviderBAsync();
        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 1))
            .Async<TopologyEntryRes[]>();
        await consumer.Post("/connections/wait")
            .Body(new ConnectionWaitReq(
                ["kind=ConnectionReady", $"remote={restarted.Endpoint}"], connectionCount))
            .Async<string[]>();
        await ProviderTrafficProbe.DriveUntilProviderServesAsync(
            consumer,
            providerB,
            "rl-c2-restored",
            "RL-C2 restored provider traffic",
            "profile-request|rid=api-b|marker=rl-c2-restored-");

        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            var waitA = providerA.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(["marker=rl-c2-after-crash-"], [])).Async<string[]>(timeout.Token)
                .AsTask();
            var waitB = providerB.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(["marker=rl-c2-after-crash-"], [])).Async<string[]>(timeout.Token)
                .AsTask();
            var completed = await Task.WhenAny(waitA, waitB);
            var evidence = (await completed).Body;
            timeout.Cancel();
            ZlinkStreamAssert.Ensure(
                evidence.Any(line => line.Contains("marker=rl-c2-after-crash-", StringComparison.Ordinal)),
                "RL-C2 did not record expected evidence 'marker=rl-c2-after-crash-'.");
        }
        Console.WriteLine("scenario RL-C2 passed");
    }

    private static async Task<bool> IsHealthyAsync(ZLinkHttpClient provider)
    {
        try
        {
            return (await provider.Get("/health").AsyncRaw()).Status == 200;
        }
        catch
        {
            return false;
        }
    }

    private static async Task WaitUntilAsync(Func<Task<bool>> condition, string message)
    {
        for (var attempt = 0; attempt < 120; attempt++)
        {
            if (await condition()) return;

            await Task.Delay(250);
        }

        throw new InvalidOperationException(message);
    }
}
