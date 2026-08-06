// Verifies RL-E1 orderly disconnect behavior.
using System.Diagnostics;
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Configuration;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-E1 verifies that normal close and an abrupt process close remove only the
// affected RouteMesh and ClientServer targets before the common peer deadline.
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
        await WaitForClientServerReadyAsync(consumer, 2);

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
        await WaitForClientServerReadyAsync(consumer, 2);

        var marker = $"rl-e1-{variant}-{Guid.NewGuid():N}";
        await stop();
        // Measure the public liveness observation after the close operation
        // has completed. Process startup/kill is harness overhead, not the
        // peer deadline being verified. The process manager already owns
        // process termination; probing its dead HTTP endpoint would wait on a
        // separate client liveness deadline and contaminate this measurement.
        var before = Stopwatch.StartNew();
        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq(
                "api-b",
                "Ready",
                0,
                TimeoutMilliseconds: 30000))
            .Async<TopologyEntryRes[]>();
        await WaitForClientServerLossAsync(consumer);
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

        var clientServerMarker = $"{marker}-client-server";
        var clientServerSurviving = (await consumer.Post("/profile/clientserver/request")
            .Body(new ProfileReq("client-server", clientServerMarker))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(
            clientServerSurviving.ProviderRid == "api-a",
            $"RL-E1 {variant} ClientServer selected the affected target: {clientServerSurviving.ProviderRid}.");

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

    private static async Task WaitForClientServerReadyAsync(
        ZLinkHttpClient consumer,
        int expected)
    {
        await WaitForClientServerAsync(
            consumer,
            status => status.ReadyTargetCount >= expected,
            $"ClientServer ready target count {expected}");
    }

    private static async Task WaitForClientServerLossAsync(ZLinkHttpClient consumer)
    {
        await WaitForClientServerAsync(
            consumer,
            status => status.ReadyTargetCount < 2,
            "ClientServer affected target not-ready");
    }

    private static async Task WaitForClientServerAsync(
        ZLinkHttpClient consumer,
        Func<ZLinkClientServerStatus, bool> predicate,
        string description)
    {
        var deadline = DateTime.UtcNow + PeerDeadline + TimeSpan.FromSeconds(5);
        while (DateTime.UtcNow < deadline)
        {
            try
            {
                var status = (await consumer.Get("/clientserver/status")
                    .Async<ZLinkClientServerStatus>()).Body;
                if (predicate(status)) return;
            }
            catch (Exception) when (DateTime.UtcNow < deadline)
            {
            }

            await Task.Delay(100);
        }

        try
        {
            var status = (await consumer.Get("/clientserver/status")
                .Async<ZLinkClientServerStatus>()).Body;
            var targets = string.Join(",", status.Targets.Select(target =>
                $"{target.NodeRid}:{target.Weight}:{target.State}"));
            Console.WriteLine($"RL-E1 ClientServer diagnostic description={description} ready={status.ReadyTargetCount} targets={targets}");
        }
        catch
        {
        }

        throw new TimeoutException($"RL-E1 timed out waiting for {description}.");
    }
}
