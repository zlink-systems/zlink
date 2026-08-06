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
        var previousRoutingId = (await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 1))
            .Async<TopologyEntryRes[]>()).Body.Single().RoutingId;

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
            var restarted = await processes.StartProviderBAsync(TimeSpan.FromSeconds(30));
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
                rows.Length == 1 && rows[0].Endpoint == restarted.Endpoint
                    && !string.Equals(rows[0].RoutingId, previousRoutingId, StringComparison.Ordinal),
                $"RL-A5 did not converge to exactly the current api-b endpoint "
                + $"(rows={rows.Length}, previous={previousRoutingId}, "
                + $"current={string.Join(',', rows.Select(row => row.Endpoint))}, "
                + $"expected={restarted.Endpoint}, currentRid={rows[0].RoutingId}).");
            previousRoutingId = rows[0].RoutingId;
            await consumer.Post("/connections/wait")
                .Body(new ConnectionWaitReq(
                    ["kind=ConnectionReady", $"remote={restarted.Endpoint}"], connectionCount))
                .Async<string[]>();

            await providerA.Post("/admin/weight/exclude").AsyncRaw();
            await providerA.Post("/admin/weight/wait")
                .Body(new WeightWaitReq(0))
                .AsyncRaw();
            await ProviderTrafficProbe.WaitUntilProviderExcludedAsync(
                consumer,
                "api-a",
                $"rl-a5-directed-{cycle}",
                "RL-A5");

            for (var i = 0; i < 10; i++)
            {
                var reply = (await consumer.Post("/profile/request")
                    .Body(new ProfileReq("fast", $"rl-a5-directed-{cycle}-{i}"))
                    .Async<ProfileRes>()).Body;
                ZlinkStreamAssert.Ensure(
                    reply.ProviderRid == "api-b",
                    "RL-A5 directed request did not use the current provider B.");
            }

            await providerA.Post("/admin/weight/include").AsyncRaw();
            await providerA.Post("/admin/weight/wait")
                .Body(new WeightWaitReq(100))
                .AsyncRaw();
        }

        Console.WriteLine("scenario RL-A5 passed");
    }
}
