// Verifies RL-A3 Reconnect Storm behavior.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-A3 verifies many reconnecting requests return to normal request flow.
internal static class RlA3ReconnectStormScenario
{
    public static async Task RunAsync(
        ClientOptions options,
        ZLinkHttpClient registry,
        ResilienceProcessManager processes,
        ZLinkHttpClient providerA)
    {
        await providerA.Post("/admin/weight/exclude").AsyncRaw();
        await providerA.Post("/admin/weight/wait").Body(new WeightWaitReq(0)).AsyncRaw();

        await using var fleet = await StormClientProcessFleet.StartAsync(options);
        var baseline = await fleet.RequestAllAsync("rl-a3-before");
        EnsureBatch(baseline, "rl-a3-before", "RL-A3 baseline");

        await processes.StopProviderBWithSigtermAsync();
        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 0))
            .Async<TopologyEntryRes[]>();

        await processes.StartProviderBAsync();
        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 1))
            .Async<TopologyEntryRes[]>();
        await fleet.WaitReadyAfterRestartAsync();

        var recovered = await fleet.RequestAllAsync("rl-a3-after");
        EnsureBatch(recovered, "rl-a3-after", "RL-A3 recovered");

        await providerA.Post("/admin/weight/include").AsyncRaw();

        Console.WriteLine("scenario RL-A3 passed");
    }

    private static void EnsureBatch(ProfileRes[] replies, string markerPrefix, string scenario)
    {
        ZlinkStreamAssert.Ensure(replies.Length == 100, $"{scenario} did not return 100 replies.");
        ZlinkStreamAssert.Ensure(
            replies.Select(reply => reply.Marker).Distinct(StringComparer.Ordinal).Count() == 100
            && replies.All(reply => reply.ProviderRid == "api-b"
                                    && reply.Marker.StartsWith(markerPrefix, StringComparison.Ordinal)),
            $"{scenario} contained a duplicate or unexpected reply.");
    }
}
