// Verifies RL-C4 Registry Outage behavior.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-C4 verifies established direct traffic during a location store
// outage (fail-static) and resolve recovery after the store returns.
internal static class RlC4RegistryOutageScenario
{
    public static async Task RunAsync(
        ClientOptions options,
        ZLinkHttpClient consumer,
        ZLinkHttpClient registry,
        ResilienceProcessManager processes,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        var before = (await consumer.Post("/profile/request")
            .Body(new ProfileReq("fast", "rl-c4-before-outage"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(before.Value == "profile:fast", "RL-C4 request failed before the store outage.");

        // The store goes away; every established connection must keep
        // working (fail-static) while resolves are impossible.
        await processes.PauseStoreAsync();

        var during = (await consumer.Post("/profile/request")
            .Body(new ProfileReq("fast", "rl-c4-during-outage"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(during.Value == "profile:fast", "RL-C4 existing channel failed during the store outage.");

        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            var waitA = providerA.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(["marker=rl-c4-before-outage"], [])).Async<string[]>(timeout.Token)
                .AsTask();
            var waitB = providerB.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(["marker=rl-c4-before-outage"], [])).Async<string[]>(timeout.Token)
                .AsTask();
            var completed = await Task.WhenAny(waitA, waitB);
            var evidence = (await completed).Body;
            timeout.Cancel();
            ZlinkStreamAssert.Ensure(
                evidence.Any(line => line.Contains("marker=rl-c4-before-outage", StringComparison.Ordinal)),
                "RL-C4 did not record expected evidence 'marker=rl-c4-before-outage'.");
        }
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            var waitA = providerA.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(["marker=rl-c4-during-outage"], [])).Async<string[]>(timeout.Token)
                .AsTask();
            var waitB = providerB.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(["marker=rl-c4-during-outage"], [])).Async<string[]>(timeout.Token)
                .AsTask();
            var completed = await Task.WhenAny(waitA, waitB);
            var evidence = (await completed).Body;
            timeout.Cancel();
            ZlinkStreamAssert.Ensure(
                evidence.Any(line => line.Contains("marker=rl-c4-during-outage", StringComparison.Ordinal)),
                "RL-C4 did not record expected evidence 'marker=rl-c4-during-outage'.");
        }

        await processes.UnpauseStoreAsync();
        await providerA.Post("/shutdown").AsyncRaw();
        await WaitUntilAsync(async () => !await IsHealthyAsync(providerA),
            "RL-C4 expected api-a restart after store recovery.");
        await processes.WaitInitialProviderAExitedAsync();
        await processes.StartProviderAAsync();
        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-a", "Ready", 1))
            .Async<TopologyEntryRes[]>();

        var after = await EphemeralRouteClient.RequestAsync(
            options,
            new ProfileReq("fast", "rl-c4-after-restart"));
        ZlinkStreamAssert.Ensure(after.Value == "profile:fast", "RL-C4 follow-up request failed after store recovery.");

        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            var waitA = providerA.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(["marker=rl-c4-after-restart"], [])).Async<string[]>(timeout.Token)
                .AsTask();
            var waitB = providerB.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(["marker=rl-c4-after-restart"], [])).Async<string[]>(timeout.Token)
                .AsTask();
            var completed = await Task.WhenAny(waitA, waitB);
            var evidence = (await completed).Body;
            timeout.Cancel();
            ZlinkStreamAssert.Ensure(
                evidence.Any(line => line.Contains("marker=rl-c4-after-restart", StringComparison.Ordinal)),
                "RL-C4 did not record expected evidence 'marker=rl-c4-after-restart'.");
        }

        Console.WriteLine("scenario RL-C4 passed");
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
