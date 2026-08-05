// Verifies RM-B2 Scale In behavior.
using LocationMessaging.Client.Support;
using LocationMessaging.Shared;
using Zlink.HttpClient;

namespace LocationMessaging.Client.Scenarios;

// RM-B2 verifies that after a graceful scale-in the stopped provider's peer
// location row is removed on the shutdown path (no owner lease expiry wait)
// and traffic continues through the remaining provider only.
internal static class RmB2ScaleInScenario
{
    public static async Task RunAsync(ClientOptions options)
    {
        await using var cluster = await DynamicClusterLauncher.StartAsync(options, "rm-b2");
        var providerA = await cluster.StartProviderAsync("api-a", "api-a");
        var providerB = await cluster.StartProviderAsync("api-b", "api-b");
        var consumer = await cluster.StartConsumerAsync("consumer");
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

        await WaitForPeerRowAsync(requester, "api-b", expected: true);
        await WaitConnectionEvidenceAsync(
            requester,
            "monitor-mesh|source=profile|kind=ConnectionReady|remote=|routing=api-a-");
        await WaitConnectionEvidenceAsync(
            requester,
            "monitor-mesh|source=profile|kind=ConnectionReady|remote=|routing=api-b-");
        var firstBefore = (await requester.Post("/profile/request")
            .Body(new ProfileReq("rm-b2-first-before-scale-in"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(
            firstBefore.ProviderRid is "api-a" or "api-b"
            && firstBefore.Value == "profile:rm-b2-first-before-scale-in",
            "RM-B2 first request after peer convergence failed.");
        var markerBefore = $"rm-b2-before-{Guid.NewGuid():N}";
        var valuesBefore = Enumerable.Range(0, 40)
            .Select(index => $"{markerBefore}-{index}")
            .ToArray();
        var repliesBefore = new List<ProfileRes>(valuesBefore.Length);
        foreach (var value in valuesBefore)
        {
            var reply = (await requester.Post("/profile/request")
                .Body(new ProfileReq(value))
                .Async<ProfileRes>()).Body;
            repliesBefore.Add(reply);
        }

        ZlinkStreamAssert.Ensure(repliesBefore.Count == valuesBefore.Length, "RM-B2 pre-scale reply count mismatch.");
        ZlinkStreamAssert.Ensure(
            repliesBefore.All(reply => reply.ProviderRid is "api-a" or "api-b"),
            "RM-B2 reply provider mismatch before scale-in.");

        var apiABeforeValues = valuesBefore.Zip(repliesBefore)
            .Where(result => result.Second.ProviderRid == "api-a")
            .Select(result => result.First)
            .ToArray();
        var apiBBeforeValues = valuesBefore.Zip(repliesBefore)
            .Where(result => result.Second.ProviderRid == "api-b")
            .Select(result => result.First)
            .ToArray();
        ZlinkStreamAssert.Ensure(apiABeforeValues.Length > 0 && apiBBeforeValues.Length > 0,
            "RM-B2 expected both providers before scale-in.");
        var scaleOutA = await WaitEvidenceAsync(providerAClient, apiABeforeValues[^1]);
        var scaleOutB = await WaitEvidenceAsync(providerBClient, apiBBeforeValues[^1]);
        var preA = EvidenceDelta.CountMatching(scaleOutA, beforeA, "profile-request|rid=api-a", markerBefore);
        var preB = EvidenceDelta.CountMatching(scaleOutB, beforeB, "profile-request|rid=api-b", markerBefore);
        ZlinkStreamAssert.Ensure(preA == apiABeforeValues.Length && preB == apiBBeforeValues.Length
                                                            && preA + preB == valuesBefore.Length,
            "RM-B2 expected both providers before scale-in.");

        var beforeDisconnect = await WaitConnectionEvidenceAsync(
            requester,
            "monitor-mesh|source=profile|kind=ConnectionReady|remote=|routing=api-b-");

        var transitionMarker = $"rm-b2-continuous-{Guid.NewGuid():N}";
        var continuingResults = new List<ProfileRes>();
        var firstDuringDrain = RequestProfileAsync(requester, $"{transitionMarker}-0");
        var stopProviderB = cluster.StopAsync(providerB);
        continuingResults.Add(await firstDuringDrain);
        for (var index = 1; !stopProviderB.IsCompleted || index < 20; index++)
        {
            continuingResults.Add(await RequestProfileAsync(requester, $"{transitionMarker}-{index}"));
        }
        var drained = await stopProviderB;
        ZlinkStreamAssert.Ensure(
            drained is { Result: "Stopped", Reason: null },
            $"RM-B2 provider did not reach terminal Stopped: {drained.Result}/{drained.Reason}.");
        ZlinkStreamAssert.Ensure(
            continuingResults.All(result =>
                result.ProviderRid is "api-a" or "api-b"
                && result.Value.StartsWith("profile:", StringComparison.Ordinal)),
            "RM-B2 target-free request failed during graceful scale-in.");

        // Graceful shutdown must remove api-b's peer row from the runtime
        // query peer list without waiting for owner lease expiry (doc RM-B2).
        await WaitForPeerRowGoneAsync(requester, "api-b");
        await WaitConnectionEvidenceAsync(
            requester,
            "monitor-mesh|source=profile|kind=Disconnected|remote=|routing=api-b-",
            beforeDisconnect.Length);
        var firstAfter = (await requester.Post("/profile/request")
            .Body(new ProfileReq("rm-b2-first-after-scale-in"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(
            firstAfter.ProviderRid == "api-a"
            && firstAfter.Value == "profile:rm-b2-first-after-scale-in",
            "RM-B2 first post-scale request did not succeed on api-a.");

        beforeA = await ReadEvidenceAsync(providerAClient);
        var markerAfter = $"rm-b2-after-{Guid.NewGuid():N}";
        var valuesAfter = Enumerable.Range(0, 20)
            .Select(index => $"{markerAfter}-{index}")
            .ToArray();
        var repliesAfter = new List<ProfileRes>(valuesAfter.Length);
        foreach (var value in valuesAfter)
        {
            var reply = (await requester.Post("/profile/request")
                .Body(new ProfileReq(value))
                .Async<ProfileRes>()).Body;
            repliesAfter.Add(reply);
        }

        ZlinkStreamAssert.Ensure(repliesAfter.Count == valuesAfter.Length, "RM-B2 post-scale reply count mismatch.");
        ZlinkStreamAssert.Ensure(
            repliesAfter.All(reply => reply.ProviderRid == "api-a"),
            "RM-B2 after scale-in should reach api-a only.");

        var afterA = await WaitEvidenceAsync(providerAClient, valuesAfter[^1]);
        var a = EvidenceDelta.CountMatching(afterA, beforeA, "profile-request|rid=api-a", markerAfter);
        ZlinkStreamAssert.Ensure(a == valuesAfter.Length, "RM-B2 expected only api-a after scale-in.");
    }

    private static Task WaitForPeerRowGoneAsync(ZLinkHttpClient http, string rid)
        => WaitForPeerRowAsync(http, rid, expected: false);

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
        string contains,
        int afterCount = 0)
    {
        return (await http.Post("/connections/wait")
            .Body(new EvidenceWaitReq(contains, AfterCount: afterCount))
            .Async<string[]>()).Body;
    }

    private static async Task<ProfileRes> RequestProfileAsync(ZLinkHttpClient http, string value)
    {
        var reply = (await http.Post("/profile/request")
            .Body(new ProfileReq(value))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(
            reply.Value == $"profile:{value}",
            "RM-B2 transition reply payload mismatch.");
        return reply;
    }
}
