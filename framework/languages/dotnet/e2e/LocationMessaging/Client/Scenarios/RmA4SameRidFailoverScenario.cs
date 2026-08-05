// Verifies RM-A4 Same Rid Failover behavior.
using System.Text.Json;
using LocationMessaging.Client.Support;
using LocationMessaging.Shared;
using Zlink.HttpClient;

namespace LocationMessaging.Client.Scenarios;

// RM-A4 verifies replacement under the same application prefix. Automatic
// topology issues a new physical RID for the new process, so the runtime query
// must remove the old row and expose one live api-a-prefixed row at the new
// endpoint before traffic resumes.
internal static class RmA4SameRidFailoverScenario
{
    public static async Task RunAsync(ClientOptions options)
    {
        await using var cluster = await DynamicClusterLauncher.StartAsync(options, "rm-a4");
        var providerV1 = await cluster.StartProviderAsync("api-a-v1", "api-a");
        var consumer = await cluster.StartConsumerAsync("consumer");
        using var observer = ZLinkHttpClient.Create(consumer.HttpUrl)
            .Timeout(TimeSpan.FromSeconds(40))
            .Build();

        await WaitForPeerAsync(observer, "api-a", present: true, providerV1.ChannelEndpoint);

        var first = (await observer.Post("/profile/request")
            .Body(new ProfileReq("rm-a4-v1"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(
            first.ProviderRid == "api-a",
            "RM-A4 initial request should reach api-a.");

        using var providerV1Client = ZLinkHttpClient.Create(providerV1.HttpUrl)
            .Timeout(TimeSpan.FromSeconds(10))
            .Build();
        await WaitForEvidenceAsync(providerV1Client, "value=rm-a4-v1");

        var drained = await cluster.StopAsync(providerV1);
        ZlinkStreamAssert.Ensure(
            drained is { Result: "Stopped", Reason: null },
            $"RM-A4 v1 did not reach terminal Stopped: {drained.Result}/{drained.Reason}.");
        await WaitForPeerAsync(observer, "api-a", present: false);

        var providerV2 = await cluster.StartProviderAsync("api-a-v2", "api-a");
        using var providerV2Client = ZLinkHttpClient.Create(providerV2.HttpUrl)
            .Timeout(TimeSpan.FromSeconds(10))
            .Build();

        // Wait until the runtime query shows one current api-a-prefixed row at
        // v2's endpoint; the previous physical RID must no longer be live.
        await WaitForSingleLiveRowAsync(observer, providerV2.ChannelEndpoint);
        await WaitForRouteReadyAsync(observer);

        var beforeV1 = await ReadEvidenceIgnoringStoppedAsync(providerV1Client);
        var beforeV2 = await ReadEvidenceAsync(providerV2Client);
        var marker = $"rm-a4-{Guid.NewGuid():N}";
        for (var i = 0; i < 20; i++)
        {
            var reply = (await observer.Post("/profile/request")
                .Body(new ProfileReq($"{marker}-{i}"))
                .Async<ProfileRes>()).Body;
            ZlinkStreamAssert.Ensure(
                reply.ProviderRid == "api-a",
                "RM-A4 replacement request should reach api-a.");
        }

        var afterV2 = await WaitForEvidenceAsync(providerV2Client, $"{marker}-19");
        var afterV1 = await ReadEvidenceIgnoringStoppedAsync(providerV1Client);
        var v1Count = EvidenceDelta.CountMatching(
            afterV1,
            beforeV1,
            "profile-request|rid=api-a",
            marker);
        var v2Count = EvidenceDelta.CountMatching(
            afterV2,
            beforeV2,
            "profile-request|rid=api-a",
            marker);
        ZlinkStreamAssert.Ensure(v1Count == 0 && v2Count == 20, "RM-A4 replacement provider evidence did not match.");
    }

    private static async Task WaitForSingleLiveRowAsync(ZLinkHttpClient client, string expectedEndpoint)
    {
        await client.Post("/locations/peers/wait")
            .Body(new PeerLocationWaitReq(
                "profile",
                "Router",
                "api-a",
                Present: true,
                Endpoint: expectedEndpoint))
            .Async<PeerLocationRow[]>();
    }

    private static async Task WaitForRouteReadyAsync(ZLinkHttpClient client)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(30);
        while (DateTimeOffset.UtcNow < deadline)
        {
            var status = (await client.Get("/topology/ready?count=1")
                    .Async<JsonElement>())
                .Body;
            if (status.GetProperty("ready").GetBoolean())
                return;

            await Task.Delay(TimeSpan.FromMilliseconds(100));
        }

        throw new TimeoutException(
            "RM-A4 replacement peer became visible before the profile Channel became ready.");
    }

    private static Task WaitForPeerAsync(
        ZLinkHttpClient client,
        string rid,
        bool present,
        string? endpoint = null) =>
        client.Post("/locations/peers/wait")
            .Body(new PeerLocationWaitReq(
                "profile",
                "Router",
                rid,
                present,
                Endpoint: endpoint))
            .Async<PeerLocationRow[]>()
            .AsTask();

    private static async Task<string[]> ReadEvidenceAsync(ZLinkHttpClient client)
    {
        return (await client.Get("/evidence").Async<string[]>()).Body;
    }

    private static async Task<string[]> ReadEvidenceIgnoringStoppedAsync(ZLinkHttpClient client)
    {
        try
        {
            return await ReadEvidenceAsync(client);
        }
        catch
        {
            return [];
        }
    }

    private static async Task<string[]> WaitForEvidenceAsync(ZLinkHttpClient client, string contains)
    {
        return (await client.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(contains))
            .Async<string[]>()).Body;
    }
}
