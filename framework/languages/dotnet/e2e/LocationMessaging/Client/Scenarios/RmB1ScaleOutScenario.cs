// Verifies RM-B1 Scale Out behavior.
using LocationMessaging.Client.Support;
using LocationMessaging.Shared;
using Zlink.HttpClient;

namespace LocationMessaging.Client.Scenarios;

// RM-B1 verifies that a newly started provider's peer location row appears in
// the runtime query peer list and that it receives profile traffic after
// scale-out, without restarting the consumer.
internal static class RmB1ScaleOutScenario
{
    public static async Task RunAsync(ClientOptions options)
    {
        await using var cluster = await DynamicClusterLauncher.StartAsync(options, "rm-b1");
        var providerA = await cluster.StartProviderAsync("api-a", "api-a");
        var consumer = await cluster.StartConsumerAsync("consumer");
        using var requester = ZLinkHttpClient.Create(consumer.HttpUrl)
            .Timeout(TimeSpan.FromMinutes(5))
            .Build();
        using var providerAClient = ZLinkHttpClient.Create(providerA.HttpUrl)
            .Timeout(TimeSpan.FromMinutes(5))
            .Build();

        await WaitConnectionEvidenceAsync(
            requester,
            // Spec 24 §7 keeps the endpoint out of public status, so a peer is
            // identified by its automatic RID, which carries the role prefix.
            "monitor-mesh|source=profile|kind=ConnectionReady|remote=|routing=api-a-");
        var beforeA = await ReadEvidenceAsync(providerAClient);
        var markerBefore = $"rm-b1-before-{Guid.NewGuid():N}";
        for (var i = 0; i < 10; i++)
        {
            var reply = (await requester.Post("/profile/request")
                .Body(new ProfileReq($"{markerBefore}-{i}"))
                .Async<ProfileRes>()).Body;
            ZlinkStreamAssert.Ensure(reply.ProviderRid == "api-a", "RM-B1 before scale-out should reach api-a.");
        }

        var preScaleEvidence = await WaitEvidenceAsync(providerAClient, $"{markerBefore}-9");
        ZlinkStreamAssert.Ensure(
            EvidenceDelta.CountMatching(
                preScaleEvidence,
                beforeA,
                "profile-request|rid=api-a",
                markerBefore) == 10,
            "RM-B1 pre-scale evidence was not api-a only.");

        var providerB = await cluster.StartProviderAsync("api-b", "api-b");
        using var providerBClient = ZLinkHttpClient.Create(providerB.HttpUrl)
            .Timeout(TimeSpan.FromMinutes(5))
            .Build();

        // Wait until the runtime query peer list reflects api-b's row before
        // checking the first request result without an application retry.
        await WaitForPeerRowAsync(requester, "api-b", expected: true);
        await WaitConnectionEvidenceAsync(
            requester,
            "monitor-mesh|source=profile|kind=ConnectionReady|remote=|routing=api-b-");
        var first = (await requester.Post("/profile/request")
            .Body(new ProfileReq("rm-b1-first-after-row"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(
            first.ProviderRid is "api-a" or "api-b"
            && first.Value == "profile:rm-b1-first-after-row",
            "RM-B1 first request after row convergence did not complete exactly once.");

        beforeA = await ReadEvidenceAsync(providerAClient);
        var beforeB = await ReadEvidenceAsync(providerBClient);
        var markerAfter = $"rm-b1-after-{Guid.NewGuid():N}";
        var values = Enumerable.Range(0, 60)
            .Select(index => $"{markerAfter}-{index}")
            .ToArray();
        var replies = new List<ProfileRes>(values.Length);
        foreach (var value in values)
        {
            var reply = (await requester.Post("/profile/request")
                .Body(new ProfileReq(value))
                .Async<ProfileRes>()).Body;
            replies.Add(reply);
        }

        ZlinkStreamAssert.Ensure(replies.Count == values.Length, "RM-B1 scale-out reply count mismatch.");
        foreach (var reply in replies)
            ZlinkStreamAssert.Ensure(reply.ProviderRid is "api-a" or "api-b", "RM-B1 reply provider mismatch.");

        var apiAValues = values.Zip(replies)
            .Where(result => result.Second.ProviderRid == "api-a")
            .Select(result => result.First)
            .ToArray();
        var apiBValues = values.Zip(replies)
            .Where(result => result.Second.ProviderRid == "api-b")
            .Select(result => result.First)
            .ToArray();
        ZlinkStreamAssert.Ensure(apiAValues.Length > 0 && apiBValues.Length > 0,
            "RM-B1 expected both providers after scale-out.");

        var afterA = await WaitEvidenceAsync(providerAClient, apiAValues[^1]);
        var afterB = await WaitEvidenceAsync(providerBClient, apiBValues[^1]);
        var a = EvidenceDelta.CountMatching(afterA, beforeA, "profile-request|rid=api-a", markerAfter);
        var b = EvidenceDelta.CountMatching(afterB, beforeB, "profile-request|rid=api-b", markerAfter);
        ZlinkStreamAssert.Ensure(a == apiAValues.Length && b == apiBValues.Length && a + b == values.Length,
            "RM-B1 expected evidence to match scale-out replies.");
    }

    private static async Task WaitForPeerRowAsync(ZLinkHttpClient http, string rid, bool expected)
    {
        await http.Post("/locations/peers/wait")
            .Body(new PeerLocationWaitReq("profile", "Router", rid, expected))
            .Async<PeerLocationRow[]>();
    }

    private static async Task<string[]> ReadEvidenceAsync(ZLinkHttpClient http)
    {
        return (await http.Get("/evidence").Async<string[]>()).Body;
    }

    private static async Task<string[]> WaitEvidenceAsync(ZLinkHttpClient http, string contains)
    {
        return (await http.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(contains))
            .Async<string[]>()).Body;
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
