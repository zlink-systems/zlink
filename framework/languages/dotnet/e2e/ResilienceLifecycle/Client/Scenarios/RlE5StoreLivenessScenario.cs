// Verifies that Store failure does not mask transport liveness or shutdown.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.Framework.Contracts.Configuration;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

internal static class RlE5StoreLivenessScenario
{
    private static readonly TimeSpan LivenessObservationBudget = TimeSpan.FromSeconds(20);

    public static async Task RunAsync(
        ClientOptions options,
        ZLinkHttpClient consumer,
        ResilienceProcessManager processes,
        ZLinkHttpClient providerA)
    {
        ZlinkStreamAssert.Ensure(
            !string.IsNullOrWhiteSpace(options.RouteProxyControlUrl)
            && !string.IsNullOrWhiteSpace(options.ConsumerRouteProxyControlUrl)
            && options.ConsumerProcessId > 0,
            "RL-E5 requires both directional RouteMesh proxies and a consumer process id.");

        await WaitForRouteReadyAsync(consumer, 2);
        var evidenceBeforeShutdown = (await providerA.Get("/evidence")
            .Async<string[]>()).Body;
        await processes.PauseStoreAsync();
        try
        {
            await BlockAsync(options.RouteProxyControlUrl!, "/block/client-to-target");
            await BlockAsync(options.ConsumerRouteProxyControlUrl!, "/block/target-to-client");

            await WaitForRouteLossAsync(consumer);

            // The Store is still paused. Shutdown must complete without a
            // Store retry keeping the host in a reconnecting state.
            await consumer.Post("/shutdown").AsyncRaw();
            await processes.WaitConsumerExitedAsync();

            await Task.Delay(1000);
            var evidenceAfterShutdown = (await providerA.Get("/evidence")
                .Async<string[]>()).Body;
            ZlinkStreamAssert.Ensure(
                evidenceAfterShutdown.SequenceEqual(evidenceBeforeShutdown),
                "RL-E5 observed new provider handler evidence after consumer shutdown.");

            Console.WriteLine("scenario RL-E5 passed");
        }
        finally
        {
            await UnblockAsync(options.RouteProxyControlUrl!);
            await UnblockAsync(options.ConsumerRouteProxyControlUrl!);
            await processes.UnpauseStoreAsync();
        }
    }

    private static async Task WaitForRouteReadyAsync(
        ZLinkHttpClient consumer,
        int count)
        => await WaitUntilAsync(async () =>
        {
            var status = (await consumer.Get("/route/status")
                .Async<ZLinkRouteMeshStatus>()).Body;
            return status.Channels.Any(channel =>
                channel.ChannelName == ResilienceLifecycleNames.Channel
                && channel.ReadyTargetCount >= count);
        }, "both RouteMesh targets ready");

    private static async Task WaitForRouteLossAsync(ZLinkHttpClient consumer)
        => await WaitUntilAsync(async () =>
        {
            var status = (await consumer.Get("/route/status")
                .Async<ZLinkRouteMeshStatus>()).Body;
            return status.Channels.Any(channel =>
                channel.ChannelName == ResilienceLifecycleNames.Channel
                && channel.ReadyTargetCount < 2);
        }, "RouteMesh liveness loss while Store is paused");

    private static async Task WaitUntilAsync(
        Func<Task<bool>> predicate,
        string description)
    {
        var deadline = DateTime.UtcNow + LivenessObservationBudget;
        while (DateTime.UtcNow < deadline)
        {
            try
            {
                if (await predicate()) return;
            }
            catch (Exception) when (DateTime.UtcNow < deadline)
            {
            }

            await Task.Delay(100);
        }

        throw new TimeoutException($"RL-E5 timed out waiting for {description}.");
    }

    private static async Task BlockAsync(string controlUrl, string path)
        => await ZLinkHttpClient.Create(controlUrl).Build().Post(path).AsyncRaw();

    private static async Task UnblockAsync(string controlUrl)
        => await ZLinkHttpClient.Create(controlUrl).Build().Post("/unblock").AsyncRaw();
}
