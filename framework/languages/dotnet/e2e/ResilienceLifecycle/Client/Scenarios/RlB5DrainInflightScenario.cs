// Verifies RL-B5 in-flight completion across a socket weight change.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-B5 verifies a weight-only change does not interrupt accepted work.
internal static class RlB5DrainInflightScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient consumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        await providerA.Post("/admin/weight/include").AsyncRaw();
        await providerB.Post("/admin/weight/include").AsyncRaw();
        await WaitForWeightAsync(providerA, 100);
        await WaitForWeightAsync(providerB, 100);

        var slowMarker = $"rl-b5-slow-{Guid.NewGuid():N}";
        var slowTask = consumer.Post("/profile/request")
            .Body(new ProfileReq("slow", slowMarker))
            .Async<ProfileRes>();

        var slowProvider = await WaitForSlowStartAsync(providerA, providerB, slowMarker);
        var drainedProvider = slowProvider == "api-a" ? providerA : providerB;
        var healthyProvider = slowProvider == "api-a" ? "api-b" : "api-a";
        await drainedProvider.Post("/admin/weight/exclude").AsyncRaw();
        await WaitForWeightAsync(drainedProvider, 0);
        await ProviderTrafficProbe.WaitUntilProviderExcludedAsync(
            consumer, slowProvider, "rl-b5-propagation", "RL-B5");
        var beforeDrain = (await drainedProvider.Get("/evidence").Async<string[]>()).Body;

        for (var i = 0; i < 12; i++)
        {
            var reply = await ProviderTrafficProbe.RequestWithoutRetryAsync(
                consumer,
                new ProfileReq("fast", $"rl-b5-drained-{i}"));
            ZlinkStreamAssert.Ensure(reply.ProviderRid == healthyProvider,
                "RL-B5 weight exclusion did not block new requests to the excluded provider.");
        }

        var slowReply = (await slowTask).Body;
        ZlinkStreamAssert.Ensure(
            slowReply.ProviderRid == slowProvider && slowReply.Marker == slowMarker,
            "RL-B5 in-flight slow reply did not complete on the drained provider.");

        var afterDrain = (await drainedProvider.Get("/evidence").Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            CountNew(afterDrain, beforeDrain, $"profile-request|rid={slowProvider}|marker=rl-b5-drained-") == 0,
            "RL-B5 excluded provider accepted new requests after weight propagation.");
        ZlinkStreamAssert.Ensure(
            afterDrain.Any(line =>
                line.Contains($"profile-request|rid={slowProvider}|marker={slowMarker}", StringComparison.Ordinal)),
            "RL-B5 in-flight completion evidence missing.");

        await drainedProvider.Post("/admin/weight/include").AsyncRaw();
        await WaitForWeightAsync(drainedProvider, 100);

        for (var i = 0; i < 40; i++)
        {
            var reply = (await consumer.Post("/profile/request")
                .Body(new ProfileReq("fast", $"rl-b5-after-{i}"))
                .Async<ProfileRes>()).Body;
            ZlinkStreamAssert.Ensure(reply.Value == "profile:fast", "RL-B5 restored request returned an unexpected value.");
        }

        await drainedProvider.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([$"profile-request|rid={slowProvider}|marker=rl-b5-after-"], []))
            .Async<string[]>();

        Console.WriteLine("scenario RL-B5 passed");
    }

    private static async Task<string> WaitForSlowStartAsync(
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB,
        string marker)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var waitA = providerA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([$"profile-start|rid=api-a|marker={marker}"], []))
            .Async<string[]>(timeout.Token)
            .AsTask();
        var waitB = providerB.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([$"profile-start|rid=api-b|marker={marker}"], []))
            .Async<string[]>(timeout.Token)
            .AsTask();

        var completed = await Task.WhenAny(waitA, waitB);
        await completed;
        timeout.Cancel();
        return completed == waitA ? "api-a" : "api-b";
    }

    private static async Task WaitForWeightAsync(ZLinkHttpClient provider, int expected)
    {
        await provider.Post("/admin/weight/wait")
            .Body(new WeightWaitReq(expected))
            .AsyncRaw();
    }

    private static int CountNew(string[] after, string[] before, string pattern)
    {
        return Math.Max(0, after.Count(line => line.Contains(pattern, StringComparison.Ordinal))
                           - before.Count(line => line.Contains(pattern, StringComparison.Ordinal)));
    }
}
