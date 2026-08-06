// Verifies RL-E2 directional blackhole and failure isolation.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.Framework.Contracts.Configuration;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

internal static class RlE2HalfOpenConnectionScenario
{
    private static readonly TimeSpan LivenessDeadline = TimeSpan.FromSeconds(15);

    public static async Task RunAsync(
        ClientOptions options,
        ZLinkHttpClient consumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        ZlinkStreamAssert.Ensure(
            !string.IsNullOrWhiteSpace(options.RouteProxyControlUrl)
            && !string.IsNullOrWhiteSpace(options.ConsumerRouteProxyControlUrl)
            && !string.IsNullOrWhiteSpace(options.ClientServerProxyControlUrl),
            "RL-E2 requires all directional fault proxy control endpoints.");

        await WaitForRouteReadyAsync(consumer, 2);
        _ = (await consumer.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 1))
            .Async<TopologyEntryRes[]>()).Body;
        await providerA.Post("/admin/weight/exclude").AsyncRaw();
        await providerA.Post("/admin/weight/wait")
            .Body(new WeightWaitReq(0))
            .AsyncRaw();
        var routeMarker = $"rl-e2-route-{Guid.NewGuid():N}";
        await providerB.Post($"/admin/profile/hold/{routeMarker}").AsyncRaw();
        var routeRequest = CaptureAsync(consumer.Post("/profile/request")
            .Body(new ProfileReq("route-before-blackhole", routeMarker))
            .Async<ProfileRes>());
        await providerB.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([$"profile-start|rid=api-b|marker={routeMarker}"], []))
            .Async<string[]>();
        await BlockAsync(options.RouteProxyControlUrl!, "/block/client-to-target");
        await BlockAsync(options.ConsumerRouteProxyControlUrl!, "/block/target-to-client");
        await providerB.Post($"/admin/profile/release/{routeMarker}").AsyncRaw();
        var routeReply = await routeRequest;
        ZlinkStreamAssert.Ensure(routeReply.ProviderRid == "api-b",
            "RL-E2 reverse RouteMesh reply did not complete on the affected target.");
        try
        {
            await WaitForRouteLossAsync(consumer);
        }
        catch
        {
            var status = (await consumer.Get("/route/status").Async<ZLinkRouteMeshStatus>()).Body;
            var proxy = (await ZLinkHttpClient.Create(options.RouteProxyControlUrl!).Build()
                .Get("/stats").Async<ProxyStats>()).Body;
            var consumerProxy = (await ZLinkHttpClient.Create(options.ConsumerRouteProxyControlUrl!).Build()
                .Get("/stats").Async<ProxyStats>()).Body;
            var peers = string.Join(",", status.Peers.Select(p => $"{p.NodeRid}:{p.State}"));
            var channels = string.Join(",", status.Channels.Select(c => $"{c.ChannelName}:{c.ReadyTargetCount}:{c.IsReady}"));
            Console.WriteLine($"RL-E2 route diagnostic peers={peers} channels={channels} proxy={proxy} consumerProxy={consumerProxy}");
            throw;
        }

        await providerA.Post("/admin/weight/include").AsyncRaw();
        await providerA.Post("/admin/weight/wait")
            .Body(new WeightWaitReq(100))
            .AsyncRaw();
        var survivingRouteReply = (await consumer.Post("/profile/request")
            .Body(new ProfileReq("route-after-blackhole", routeMarker))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(survivingRouteReply.ProviderRid == "api-a",
            "RL-E2 route failure affected the surviving target.");
        await UnblockAsync(options.RouteProxyControlUrl!);
        await UnblockAsync(options.ConsumerRouteProxyControlUrl!);

        await WaitForClientServerReadyAsync(consumer, 2);
        await providerA.Post("/admin/clientserver/weight/exclude").AsyncRaw();
        await providerA.Post("/admin/clientserver/weight/wait")
            .Body(new WeightWaitReq(0))
            .AsyncRaw();
        await WaitForClientServerWeightAsync(consumer, 0);
        var clientServerMarker = $"rl-e2-client-server-{Guid.NewGuid():N}";
        await providerB.Post($"/admin/profile/hold/{clientServerMarker}").AsyncRaw();
        var clientServerRequest = CaptureAsync(consumer.Post("/profile/clientserver/request")
            .Body(new ProfileReq("client-server-before-blackhole", clientServerMarker))
            .Async<ProfileRes>());
        await providerB.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([$"profile-start|rid=api-b|marker={clientServerMarker}"], []))
            .Async<string[]>();
        await BlockAsync(options.ClientServerProxyControlUrl!);
        await providerB.Post($"/admin/profile/release/{clientServerMarker}").AsyncRaw();
        var clientServerReply = await clientServerRequest;
        ZlinkStreamAssert.Ensure(clientServerReply.ProviderRid == "api-b",
            "RL-E2 reverse ClientServer reply did not complete on the affected target.");
        await WaitForClientServerLossAsync(consumer);

        await providerA.Post("/admin/clientserver/weight/include").AsyncRaw();
        await providerA.Post("/admin/clientserver/weight/wait")
            .Body(new WeightWaitReq(100))
            .AsyncRaw();
        await WaitForClientServerWeightAsync(consumer, 100);
        await WaitForClientServerReadyAsync(consumer, 1);
        var survivingClientServerReply = (await consumer.Post("/profile/clientserver/request")
            .Body(new ProfileReq("client-server-after-blackhole", clientServerMarker))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(survivingClientServerReply.ProviderRid == "api-a",
            "RL-E2 ClientServer failure affected the surviving target.");
        await UnblockAsync(options.ClientServerProxyControlUrl!);

        Console.WriteLine("scenario RL-E2 passed");
    }

    private static async Task WaitForRouteReadyAsync(ZLinkHttpClient consumer, int count)
        => await WaitUntilAsync(async () =>
        {
            var status = (await consumer.Get("/route/status").Async<ZLinkRouteMeshStatus>()).Body;
            return status.Channels.Any(channel => channel.ChannelName == ResilienceLifecycleNames.Channel
                && channel.ReadyTargetCount >= count);
        }, "both RouteMesh peers ready");

    private static async Task WaitForRouteLossAsync(ZLinkHttpClient consumer)
        => await WaitUntilAsync(async () =>
        {
            var status = (await consumer.Get("/route/status").Async<ZLinkRouteMeshStatus>()).Body;
            return status.Channels.Any(channel => channel.ChannelName == ResilienceLifecycleNames.Channel
                && channel.ReadyTargetCount < 2);
        }, "one RouteMesh peer not-ready");

    private static async Task WaitForClientServerReadyAsync(ZLinkHttpClient consumer, int count)
        => await WaitUntilAsync(async () =>
        {
            var status = (await consumer.Get("/clientserver/status").Async<ZLinkClientServerStatus>()).Body;
            return status.ReadyTargetCount >= count;
        }, "both ClientServer targets ready");

    private static async Task WaitForClientServerLossAsync(ZLinkHttpClient consumer)
        => await WaitUntilAsync(async () =>
        {
            var status = (await consumer.Get("/clientserver/status").Async<ZLinkClientServerStatus>()).Body;
            return status.Targets.Any(target => target.Weight > 0
                && target.State != ZLinkPeerState.Ready);
        }, "ClientServer selected target not-ready");

    private static async Task WaitForClientServerWeightAsync(
        ZLinkHttpClient consumer,
        int weight)
    {
        try
        {
            await WaitUntilAsync(async () =>
            {
                var status = (await consumer.Get("/clientserver/status").Async<ZLinkClientServerStatus>()).Body;
                return weight == 0
                    ? status.Targets.Any(target => target.Weight == 0)
                    : status.Targets.Count >= 2 && status.Targets.All(target => target.Weight == weight);
            }, $"ClientServer target weight={weight}");
        }
        catch
        {
            var status = (await consumer.Get("/clientserver/status").Async<ZLinkClientServerStatus>()).Body;
            var targets = string.Join(",", status.Targets.Select(target => $"{target.NodeRid}:{target.Weight}:{target.State}"));
            Console.WriteLine($"RL-E2 ClientServer weight diagnostic={targets}");
            throw;
        }
    }

    private static async Task WaitUntilAsync(Func<Task<bool>> predicate, string description)
    {
        var deadline = DateTime.UtcNow + LivenessDeadline + TimeSpan.FromSeconds(5);
        while (DateTime.UtcNow < deadline)
        {
            try
            {
                if (await predicate())
                    return;
            }
            catch (Exception) when (DateTime.UtcNow < deadline)
            {
            }
            await Task.Delay(100);
        }
        throw new InvalidOperationException($"RL-E2 timed out waiting for {description}.");
    }

    private static async Task BlockAsync(string controlUrl, string path = "/block/client-to-target")
        => await ZLinkHttpClient.Create(controlUrl).Build()
            .Post(path).AsyncRaw();

    private static async Task UnblockAsync(string controlUrl)
        => await ZLinkHttpClient.Create(controlUrl).Build()
            .Post("/unblock").AsyncRaw();

    private static async Task<ProfileRes> CaptureAsync(
        ValueTask<Zlink.HttpClient.HttpResponse<ProfileRes>> request)
        => (await request).Body;

    private sealed record ProxyStats(bool Blocked, int Accepted, long DroppedBytes);
}
