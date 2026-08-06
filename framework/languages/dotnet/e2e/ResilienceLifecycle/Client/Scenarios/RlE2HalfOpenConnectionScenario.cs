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
            && !string.IsNullOrWhiteSpace(options.ClientServerProxyControlUrl),
            "RL-E2 requires both directional fault proxy control endpoints.");

        await WaitForRouteReadyAsync(consumer, 2);
        await BlockAsync(options.RouteProxyControlUrl!);
        try
        {
            await WaitForRouteLossAsync(consumer);
        }
        catch
        {
            var status = (await consumer.Get("/route/status").Async<ZLinkRouteMeshStatus>()).Body;
            var proxy = (await ZLinkHttpClient.Create(options.RouteProxyControlUrl!).Build()
                .Get("/stats").Async<ProxyStats>()).Body;
            var peers = string.Join(",", status.Peers.Select(p => $"{p.NodeRid}:{p.State}"));
            var channels = string.Join(",", status.Channels.Select(c => $"{c.ChannelName}:{c.ReadyTargetCount}:{c.IsReady}"));
            Console.WriteLine($"RL-E2 route diagnostic peers={peers} channels={channels} proxy={proxy}");
            throw;
        }

        var routeMarker = $"rl-e2-route-{Guid.NewGuid():N}";
        var routeReply = (await consumer.Post("/profile/request")
            .Body(new ProfileReq("route-after-blackhole", routeMarker))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(routeReply.ProviderRid == "api-b" || routeReply.ProviderRid == "api-a",
            "RL-E2 route request returned an invalid provider identity.");
        await UnblockAsync(options.RouteProxyControlUrl!);

        await WaitForClientServerReadyAsync(consumer, 2);
        await BlockAsync(options.ClientServerProxyControlUrl!);
        await WaitForClientServerLossAsync(consumer);

        var clientServerMarker = $"rl-e2-client-server-{Guid.NewGuid():N}";
        var clientServerReply = (await consumer.Post("/profile/clientserver/request")
            .Body(new ProfileReq("client-server-after-blackhole", clientServerMarker))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(clientServerReply.ProviderRid == "api-b" || clientServerReply.ProviderRid == "api-a",
            "RL-E2 client-server request returned an invalid provider identity.");
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
            return status.ReadyTargetCount < 2;
        }, "one ClientServer target not-ready");

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

    private static async Task BlockAsync(string controlUrl)
        => await ZLinkHttpClient.Create(controlUrl).Build()
            .Post("/block/client-to-target").AsyncRaw();

    private static async Task UnblockAsync(string controlUrl)
        => await ZLinkHttpClient.Create(controlUrl).Build()
            .Post("/unblock").AsyncRaw();

    private sealed record ProxyStats(bool Blocked, int Accepted, long DroppedBytes);
}
