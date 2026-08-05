// Verifies RL-B3 Graceful Shutdown behavior.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-B3 verifies graceful shutdown avoids stale endpoint routing.
internal static class RlB3GracefulShutdownScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient consumer,
        ZLinkHttpClient registry,
        ResilienceProcessManager processes,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        var beforeMarker = $"rl-b3-before-{Guid.NewGuid():N}";
        var before = (await consumer.Post("/profile/request")
            .Body(new ProfileReq("fast", beforeMarker))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(before.ProviderRid is "api-a" or "api-b", "RL-B3 pre-shutdown request failed.");

        await providerB.Post("/shutdown").AsyncRaw();
        for (var attempt = 0; attempt < 100; attempt++)
        {
            try
            {
                var health = await providerB.Get("/health").AsyncRaw();
                if (health.Status != 200) break;
            }
            catch
            {
                break;
            }

            await Task.Delay(100);
        }

        await processes.WaitProviderBExitedAsync();
        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 0))
            .Async<TopologyEntryRes[]>();
        await ProviderTrafficProbe.WaitUntilProviderExcludedAsync(
            consumer,
            "api-b",
            "rl-b3-converge",
            "RL-B3");

        for (var i = 0; i < 12; i++)
        {
            var after = (await consumer.Post("/profile/request")
                .Body(new ProfileReq("fast", $"rl-b3-after-{i}"))
                .Async<ProfileRes>()).Body;
            ZlinkStreamAssert.Ensure(after.ProviderRid == "api-a",
                "RL-B3 request after graceful shutdown used stale api-b.");
        }

        await providerA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(["marker=rl-b3-after-"], []))
            .Async<string[]>();

        await processes.StartProviderBAsync();

        Console.WriteLine("scenario RL-B3 passed");
    }
}
