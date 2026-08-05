// Verifies RL-B2 Crash During Inflight behavior.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-B2 verifies public failure for in-flight work when a provider crashes.
internal static class RlB2CrashDuringInflightScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient consumer,
        ZLinkHttpClient registry,
        ResilienceProcessManager processes,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        await providerA.Post("/admin/weight/exclude").AsyncRaw();
        await providerA.Post("/admin/weight/wait")
            .Body(new WeightWaitReq(0))
            .AsyncRaw();

        var marker = $"rl-b2-slow-{Guid.NewGuid():N}";
        var inFlight = consumer.Post("/profile/request/attempt/3000")
            .Body(new ProfileReq("slow", marker))
            .Async<ProfileAttemptRes>();

        await providerB.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([$"profile-start|rid=api-b|marker={marker}"], []))
            .Async<string[]>();

        await processes.KillProviderBAsync();
        var crashResult = (await inFlight).Body;
        ZlinkStreamAssert.Ensure(
            crashResult.Reply is null
            && crashResult.IsRetriable
            && crashResult.ErrorKind is "Unavailable" or nameof(TimeoutException),
            $"RL-B2 crash result was '{crashResult.ErrorKind}', expected Unavailable or timeout.");

        await providerA.Post("/admin/weight/include").AsyncRaw();
        await providerA.Post("/admin/weight/wait")
            .Body(new WeightWaitReq(100))
            .AsyncRaw();

        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 0))
            .Async<TopologyEntryRes[]>();
        await ProviderTrafficProbe.WaitUntilProviderExcludedAsync(
            consumer,
            "api-b",
            "rl-b2-converge",
            "RL-B2");
        var followUp = (await consumer.Post("/profile/request")
            .Body(new ProfileReq("fast", "rl-b2-after-crash"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(followUp.ProviderRid == "api-a", "RL-B2 surviving provider traffic failed.");

        var restarted = await processes.StartProviderBAsync();
        using var restartedProviderB = ZLinkHttpClient.Create(restarted.Url)
            .Timeout(TimeSpan.FromMinutes(5))
            .Build();
        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 1))
            .Async<TopologyEntryRes[]>();
        for (var attempt = 0; attempt < 100; attempt++)
        {
            try
            {
                var health = await restartedProviderB.Get("/health").AsyncRaw();
                if (health.Status == 200) break;
            }
            catch
            {
                // The scenario keeps polling until api-b accepts HTTP traffic again.
            }

            await Task.Delay(100);
        }

        await ProviderTrafficProbe.DriveUntilProviderServesAsync(
            consumer,
            restartedProviderB,
            "rl-b2-restored",
            "RL-B2 restored provider traffic");

        Console.WriteLine("scenario RL-B2 passed");
    }
}
