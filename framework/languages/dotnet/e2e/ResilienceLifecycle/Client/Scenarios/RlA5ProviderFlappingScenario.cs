// Verifies RL-A5 Provider Flapping behavior.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-A5 verifies provider repeatedly goes down and comes back while traffic converges.
internal static class RlA5ProviderFlappingScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient consumer,
        ZLinkHttpClient registry,
        ResilienceProcessManager processes,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        var previousGeneration = (await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 1))
            .Async<TopologyEntryRes[]>()).Body.Single().Generation;

        for (var cycle = 0; cycle < 5; cycle++)
        {
            await processes.StopProviderBWithSigtermAsync();

            await registry.Post("/topology/wait")
                .Body(new TopologyWaitReq("api-b", "Ready", 0))
                .Async<TopologyEntryRes[]>();
            await ProviderTrafficProbe.WaitUntilProviderExcludedAsync(
                consumer,
                "api-b",
                $"rl-a5-converge-{cycle}",
                "RL-A5");

            for (var i = 0; i < 10; i++)
            {
                var downMarker = $"rl-a5-down-{cycle}-{i}";
                var downReply = (await consumer.Post("/profile/request")
                    .Body(new ProfileReq("fast", downMarker))
                    .Async<ProfileRes>()).Body;
                ZlinkStreamAssert.Ensure(downReply.ProviderRid == "api-a",
                    "RL-A5 request during the down window did not use api-a.");
            }
            await providerA.Post("/evidence/wait")
                .Body(new EvidenceWaitReq([$"marker=rl-a5-down-{cycle}-9"], []))
                .Async<string[]>();

            var connectionCount = (await consumer.Get("/connections").Async<string[]>()).Body.Length;
            var restarted = await processes.StartProviderBAsync();
            using var restartedProviderB = ZLinkHttpClient.Create(restarted.Url)
                .Timeout(TimeSpan.FromMinutes(5))
                .Build();
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

            var rows = (await registry.Post("/topology/wait")
                .Body(new TopologyWaitReq("api-b", "Ready", 1))
                .Async<TopologyEntryRes[]>()).Body;
            ZlinkStreamAssert.Ensure(
                rows.Length == 1 && rows[0].Generation != previousGeneration,
                "RL-A5 did not converge to exactly one new api-b owner generation.");
            previousGeneration = rows[0].Generation;
            await consumer.Post("/connections/wait")
                .Body(new ConnectionWaitReq(
                    ["kind=ConnectionReady", $"remote={restarted.Endpoint}"], connectionCount))
                .Async<string[]>();

            var seen = new HashSet<string>(StringComparer.Ordinal);
            for (var i = 0; i < 20 && seen.Count < 2; i++)
            {
                var reply = (await consumer.Post("/profile/request")
                    .Body(new ProfileReq("fast", $"rl-a5-up-{cycle}-{i}"))
                    .Async<ProfileRes>()).Body;
                seen.Add(reply.ProviderRid);
            }
            ZlinkStreamAssert.Ensure(seen.SetEquals(["api-a", "api-b"]),
                "RL-A5 did not restore both providers within 20 requests.");
        }

        Console.WriteLine("scenario RL-A5 passed");
    }
}
