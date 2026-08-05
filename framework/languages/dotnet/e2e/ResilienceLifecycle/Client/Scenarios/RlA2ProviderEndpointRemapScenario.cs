// Verifies RL-A2 Provider Endpoint Remap behavior.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-A2 verifies same provider rid comes back on a different endpoint.
internal static class RlA2ProviderEndpointRemapScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient consumer,
        ZLinkHttpClient registry,
        ResilienceProcessManager processes,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        await providerA.Post("/admin/weight/exclude").AsyncRaw();
        await providerA.Post("/admin/weight/wait").Body(new WeightWaitReq(0)).AsyncRaw();
        var oldRows = (await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 1))
            .Async<TopologyEntryRes[]>()).Body;
        var oldGeneration = oldRows.Single().Generation;
        var oldRoutingId = oldRows.Single().RoutingId;

        var marker = $"rl-a2-inflight-{Guid.NewGuid():N}";
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
            $"RL-A2 old in-flight result was '{crashResult.ErrorKind}'.");
        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 0))
            .Async<TopologyEntryRes[]>();

        var replacementConnectionCount = (await consumer.Get("/connections").Async<string[]>()).Body.Length;
        var replacement = await processes.StartProviderBRemapAsync();
        using var replacementProvider = ZLinkHttpClient.Create(replacement.Url)
            .Timeout(TimeSpan.FromMinutes(5))
            .Build();
        var replacementRows = (await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 1))
            .Async<TopologyEntryRes[]>()).Body;
        var replacementRow = replacementRows.Single();
        ZlinkStreamAssert.Ensure(
            replacementRow.Endpoint == replacement.Endpoint
            && !string.Equals(replacementRow.RoutingId, oldRoutingId, StringComparison.Ordinal),
            $"RL-A2 replacement row mismatch: endpoint={replacementRow.Endpoint}, "
            + $"expectedEndpoint={replacement.Endpoint}, routingId={replacementRow.RoutingId}, "
            + $"oldRoutingId={oldRoutingId}, oldObservation={oldGeneration}, "
            + $"newObservation={replacementRow.Generation}.");
        await consumer.Post("/connections/wait")
            .Body(new ConnectionWaitReq(
                ["kind=ConnectionReady", $"remote={replacement.Endpoint}"], replacementConnectionCount))
            .Async<string[]>();

        await SendRequestBatchAsync(consumer, "rl-a2-rescheduled");
        await replacementProvider.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(["profile-request|rid=api-b|marker=rl-a2-rescheduled-19"], []))
            .Async<string[]>();

        await replacementProvider.Post("/shutdown").AsyncRaw();
        await WaitUntilUnavailableAsync(replacementProvider);
        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 0))
            .Async<TopologyEntryRes[]>();

        var restoredConnectionCount = (await consumer.Get("/connections").Async<string[]>()).Body.Length;
        var restored = await processes.StartProviderBAsync();
        await WaitUntilAvailableAsync(providerB);
        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 1))
            .Async<TopologyEntryRes[]>();
        await consumer.Post("/connections/wait")
            .Body(new ConnectionWaitReq(
                ["kind=ConnectionReady", $"remote={restored.Endpoint}"], restoredConnectionCount))
            .Async<string[]>();
        await ProviderTrafficProbe.DriveUntilProviderServesAsync(
            consumer, providerB, "rl-a2-original-restored", "RL-A2");
        await providerA.Post("/admin/weight/include").AsyncRaw();

        Console.WriteLine("scenario RL-A2 passed");
    }

    private static async Task SendRequestBatchAsync(
        ZLinkHttpClient consumer,
        string markerPrefix)
    {
        for (var i = 0; i < 20; i++)
        {
            var marker = $"{markerPrefix}-{i}";
            var reply = (await consumer.Post("/profile/request")
                .Body(new ProfileReq("fast", marker))
                .Async<ProfileRes>()).Body;
            ZlinkStreamAssert.Ensure(reply.Value == "profile:fast", "RL-A2 request returned an unexpected value.");
        }
    }

    private static async Task WaitUntilAvailableAsync(ZLinkHttpClient http)
    {
        for (var attempt = 0; attempt < 100; attempt++)
        {
            try
            {
                var health = await http.Get("/health").AsyncRaw();
                if (health.Status == 200) return;
            }
            catch
            {
            }

            await Task.Delay(100);
        }

        ZlinkStreamAssert.Ensure(false, "RL-A2 provider did not become healthy.");
    }

    private static async Task WaitUntilUnavailableAsync(ZLinkHttpClient http)
    {
        for (var attempt = 0; attempt < 100; attempt++)
        {
            try
            {
                var health = await http.Get("/health").AsyncRaw();
                if (health.Status != 200) return;
            }
            catch
            {
                return;
            }

            await Task.Delay(100);
        }

        ZlinkStreamAssert.Ensure(false, "RL-A2 provider did not stop.");
    }
}
