// Verifies RL-E1 orderly disconnect behavior.
using System.Diagnostics;
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;
using Zlink.Framework.Contracts.Errors;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-E1 verifies that normal close and an abrupt process close remove only the
// affected RouteMesh target before the common peer deadline.
internal static class RlE1OrderlyDisconnectScenario
{
    private static readonly TimeSpan PeerDeadline = TimeSpan.FromSeconds(15);

    public static async Task RunAsync(
        ZLinkHttpClient consumer,
        ZLinkHttpClient registry,
        ResilienceProcessManager processes,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        await RunVariantAsync(
            "normal-close",
            consumer,
            registry,
            processes,
            providerA,
            providerB,
            stop: async () => await providerB.Post("/shutdown").AsyncRaw());

        await processes.StartProviderBAsync();
        await WaitUntilHealthAsync(providerB);
        await RunVariantAsync(
            "rst",
            consumer,
            registry,
            processes,
            providerA,
            providerB,
            stop: processes.KillProviderBAsync);

        await processes.StartProviderBAsync();
        await WaitUntilHealthAsync(providerB);
        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 1))
            .Async<TopologyEntryRes[]>();

        Console.WriteLine("scenario RL-E1 passed");
    }

    private static async Task RunVariantAsync(
        string variant,
        ZLinkHttpClient consumer,
        ZLinkHttpClient registry,
        ResilienceProcessManager processes,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB,
        Func<Task> stop)
    {
        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-a", "Ready", 1))
            .Async<TopologyEntryRes[]>();
        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 1))
            .Async<TopologyEntryRes[]>();

        var marker = $"rl-e1-{variant}-{Guid.NewGuid():N}";
        await stop();
        // Measure the public liveness observation after the close operation
        // has completed. Process startup/kill and HTTP health probing are
        // harness overhead, not the peer deadline being verified.
        var before = Stopwatch.StartNew();
        await WaitUntilUnavailableAsync(providerB);
        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq(
                "api-b",
                "Ready",
                0,
                TimeoutMilliseconds: 30000))
            .Async<TopologyEntryRes[]>();
        before.Stop();

        ZlinkStreamAssert.Ensure(
            before.Elapsed < PeerDeadline,
            $"RL-E1 {variant} exceeded peer deadline: {before.Elapsed}.");

        var surviving = (await consumer.Post("/profile/request")
            .Body(new ProfileReq("fast", marker))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(
            surviving.ProviderRid == "api-a",
            $"RL-E1 {variant} affected surviving target: {surviving.ProviderRid}.");
        await providerA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([$"profile-request|rid=api-a|marker={marker}"], []))
            .Async<string[]>();

        // The stopped provider must not be selected again while the surviving
        // provider remains ready.
        var followUp = (await consumer.Post("/profile/request/attempt/1000")
            .Body(new ProfileReq("fast", $"{marker}-follow-up"))
            .Async<ProfileAttemptRes>()).Body;
        ZlinkStreamAssert.Ensure(
            followUp.Reply?.ProviderRid == "api-a",
            $"RL-E1 {variant} follow-up selected an unavailable target.");

        Console.WriteLine($"scenario RL-E1 variant={variant} elapsedMs={before.ElapsedMilliseconds}");
    }

    private static async Task WaitUntilHealthAsync(ZLinkHttpClient provider)
    {
        for (var attempt = 0; attempt < 100; attempt++)
        {
            try
            {
                if ((await provider.Get("/health").AsyncRaw()).Status == 200) return;
            }
            catch (ZLinkFrameworkException)
            {
            }

            await Task.Delay(100);
        }

        throw new TimeoutException("RL-E1 provider did not become healthy.");
    }

    private static async Task WaitUntilUnavailableAsync(ZLinkHttpClient provider)
    {
        for (var attempt = 0; attempt < 100; attempt++)
        {
            try
            {
                if ((await provider.Get("/health").AsyncRaw()).Status != 200) return;
            }
            catch (ZLinkFrameworkException)
            {
                return;
            }

            await Task.Delay(100);
        }

        throw new TimeoutException("RL-E1 provider did not become unavailable.");
    }
}
