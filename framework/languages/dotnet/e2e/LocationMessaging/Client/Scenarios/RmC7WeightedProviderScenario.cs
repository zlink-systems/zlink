// Verifies RM-C7 Weighted Provider behavior.
using LocationMessaging.Client.Support;
using LocationMessaging.Shared;
using Zlink.HttpClient;

namespace LocationMessaging.Client.Scenarios;

// RM-C7 verifies that providers advertising different build-time weights via
// their peer location rows send
// distinctly more requests to the higher-weight provider.
internal static class RmC7WeightedProviderScenario
{
    public static async Task RunAsync(ClientOptions options)
    {
        await using var cluster = await DynamicClusterLauncher.StartAsync(options, "rm-c7");
        var providerA = await cluster.StartProviderAsync("api-a-weighted", "api-a", 75);
        var providerB = await cluster.StartProviderAsync("api-b-weighted", "api-b", 25);
        var consumer = await cluster.StartConsumerAsync("weighted-consumer");
        using var requester = ZLinkHttpClient.Create(consumer.HttpUrl)
            .Timeout(TimeSpan.FromMinutes(5))
            .Build();
        using var providerAClient = ZLinkHttpClient.Create(providerA.HttpUrl)
            .Timeout(TimeSpan.FromMinutes(5))
            .Build();
        using var providerBClient = ZLinkHttpClient.Create(providerB.HttpUrl)
            .Timeout(TimeSpan.FromMinutes(5))
            .Build();

        var beforeA = await ReadEvidenceAsync(providerAClient);
        var beforeB = await ReadEvidenceAsync(providerBClient);
        await WaitForProviderRowAsync(requester, "api-a");
        await WaitForProviderRowAsync(requester, "api-b");
        await WaitConnectionEvidenceAsync(
            requester,
            "monitor-mesh|source=profile|kind=ConnectionReady|remote=|routing=api-a-");
        await WaitConnectionEvidenceAsync(
            requester,
            "monitor-mesh|source=profile|kind=ConnectionReady|remote=|routing=api-b-");
        var first = (await requester.Post("/profile/request")
            .Body(new ProfileReq("rm-c7-first-after-rows"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(
            first.ProviderRid is "api-a" or "api-b"
            && first.Value == "profile:rm-c7-first-after-rows",
            "RM-C7 first request after peer convergence failed.");
        var marker = $"rm-c7-{Guid.NewGuid():N}";
        var values = Enumerable.Range(0, 240)
            .Select(index => $"{marker}-{index}")
            .ToArray();
        var replies = new List<ProfileRes>(values.Length);
        foreach (var value in values)
        {
            var reply = (await requester.Post("/profile/request")
                .Body(new ProfileReq(value))
                .Async<ProfileRes>()).Body;
            replies.Add(reply);
        }

        ZlinkStreamAssert.Ensure(replies.Count == values.Length, "RM-C7 reply count mismatch.");
        ZlinkStreamAssert.Ensure(
            replies.All(reply => reply.ProviderRid is "api-a" or "api-b"),
            "RM-C7 reply provider mismatch.");

        var apiAValues = values.Zip(replies)
            .Where(result => result.Second.ProviderRid == "api-a")
            .Select(result => result.First)
            .ToArray();
        var apiBValues = values.Zip(replies)
            .Where(result => result.Second.ProviderRid == "api-b")
            .Select(result => result.First)
            .ToArray();
        ZlinkStreamAssert.Ensure(apiAValues.Length > 0 && apiBValues.Length > 0, "RM-C7 expected both weighted providers.");
        var afterA = await WaitEvidenceAsync(providerAClient, apiAValues[^1]);
        var afterB = await WaitEvidenceAsync(providerBClient, apiBValues[^1]);
        var counts = new Dictionary<string, int>(StringComparer.Ordinal)
        {
            ["apiA"] = EvidenceDelta.CountMatching(afterA, beforeA, "profile-request|rid=api-a", marker),
            ["apiB"] = EvidenceDelta.CountMatching(afterB, beforeB, "profile-request|rid=api-b", marker)
        };
        ZlinkStreamAssert.Ensure(
            counts["apiA"] == apiAValues.Length
            && counts["apiB"] == apiBValues.Length
            && counts["apiA"] + counts["apiB"] == values.Length
            && counts["apiA"] > counts["apiB"] * 2,
            "RM-C7 weighted provider counts did not favor api-a.");
    }

    private static async Task<string[]> ReadEvidenceAsync(ZLinkHttpClient http)
    {
        return (await http.Get("/evidence").Async<string[]>()).Body;
    }

    private static async Task<string[]> WaitEvidenceAsync(ZLinkHttpClient http, string contains)
    {
        return (await http.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(contains, 20000))
            .Async<string[]>()).Body;
    }

    private static async Task WaitForProviderRowAsync(ZLinkHttpClient requester, string rid)
    {
        await requester.Post("/locations/peers/wait")
            .Body(new PeerLocationWaitReq("profile", "Router", rid, Present: true))
            .Async<PeerLocationRow[]>();
    }

    private static async Task<string[]> WaitConnectionEvidenceAsync(
        ZLinkHttpClient http,
        string contains)
    {
        return (await http.Post("/connections/wait")
            .Body(new EvidenceWaitReq(contains))
            .Async<string[]>()).Body;
    }
}
