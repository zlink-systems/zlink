// Verifies SF-C2 Graceful Removal behavior.
using System.Diagnostics;
using StoreFailure.Client.Support;
using StoreFailure.Shared;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient;

namespace StoreFailure.Client.Scenarios;

// SF-C2: a gracefully stopped provider removes its lease and rows on the
// way out, so the row disappears without waiting for lease expiry — the
// contrast to SF-C1's crash path.
internal static class SfC2GracefulRemovalScenario
{
    public static async Task RunAsync(
        ClientOptions options,
        ZLinkHttpClient consumer,
        ManagedProcess providerB)
    {
        await SfProbe.WaitProviderRoutesAsync(
            consumer,
            options.PollingInterval * 4,
            "SF-C2: provider routes were not ready before api-b began draining.");

        using var provider = ZLinkHttpClient.Create(options.ProviderBUrl)
            .Timeout(TimeSpan.FromSeconds(5))
            .Build();
        var drainTask = providerB.RequestDrainAsync();

        await SfProbe.WaitPeersAsync(
            consumer,
            SfProbe.PeerRows(options.OwnerLeaseTtl, draining: ["api-b"]),
            "SF-C2: api-b did not publish its draining marker promptly.");

        var drainingStatus = await SfProbe.GetStatusAsync(provider);
        ZlinkStreamAssert.Ensure(
            drainingStatus.OwnerLeaseHealthy,
            "SF-C2: api-b stopped renewing its owner lease while drain was in progress.");

        var propagationTimeout = options.StoreFailureGrace + options.OwnerLeaseTtl;
        var propagationElapsed = Stopwatch.StartNew();
        var consecutiveSurvivorReplies = 0;
        var probe = 0;
        while (propagationElapsed.Elapsed < propagationTimeout && consecutiveSurvivorReplies < 20)
        {
            try
            {
                var reply = await SfProbe.RequestAsync(
                    consumer,
                    $"sf-c2-propagation-{probe++}",
                    timeoutMilliseconds: 1000);
                consecutiveSurvivorReplies = reply.ProviderRid == "api-a"
                    ? consecutiveSurvivorReplies + 1
                    : 0;
            }
            catch (ZLinkFrameworkException) when (propagationElapsed.Elapsed < propagationTimeout)
            {
                consecutiveSurvivorReplies = 0;
            }
        }

        ZlinkStreamAssert.Ensure(
            consecutiveSurvivorReplies == 20,
            "SF-C2: the consumer did not exclude draining api-b within the propagation bound.");

        for (var i = 0; i < 8; i++)
        {
            var reply = await SfProbe.RequestAsync(consumer, $"sf-c2-draining-{i}");
            ZlinkStreamAssert.Ensure(
                reply.ProviderRid == "api-a",
                $"SF-C2: request {i} was assigned to draining provider '{reply.ProviderRid}'.");
        }

        var drainResult = await drainTask;
        ZlinkStreamAssert.Ensure(
            drainResult.Result == "drained" && drainResult.Reason is null,
            $"SF-C2: framework drain ended as '{drainResult.Result}' ({drainResult.Reason}).");

        // Drained is terminal only after owner cleanup. The row must therefore
        // already be absent, allowing only query propagation after that result.
        var stopwatch = Stopwatch.StartNew();

        await SfProbe.WaitPeersAsync(
            consumer,
            SfProbe.PeerRows(options.OwnerLeaseTtl, absent: ["api-b"]),
            "SF-C2: the gracefully stopped provider's row did not disappear promptly.");
        stopwatch.Stop();

        // The whole removal must beat the lease TTL by a margin — that is
        // what distinguishes shutdown cleanup from lease expiry.
        ZlinkStreamAssert.Ensure(
            stopwatch.Elapsed < options.OwnerLeaseTtl,
            $"SF-C2: row removal took {stopwatch.Elapsed}, which does not beat the lease TTL.");

        await providerB.RequestShutdownAsync();
        await providerB.WaitForGracefulExitAsync();

        for (var i = 0; i < 8; i++)
        {
            var reply = await SfProbe.RequestAsync(consumer, $"sf-c2-{i}");
            ZlinkStreamAssert.Ensure(
                reply.ProviderRid == "api-a",
                $"SF-C2: request {i} was served by '{reply.ProviderRid}' after api-b left.");
        }

        Console.WriteLine("scenario SF-C2 passed");
    }
}
