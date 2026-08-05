// Verifies SF-C1 Crash Lease Expiry behavior.
using System.Diagnostics;
using StoreFailure.Client.Support;
using Zlink.HttpClient;

namespace StoreFailure.Client.Scenarios;

// SF-C1: a SIGKILLed provider leaves its row behind, but the owner lease
// expiring is enough to drop the row from live results and to make the
// consumer stop routing there.
internal static class SfC1CrashLeaseExpiryScenario
{
    public static async Task RunAsync(
        ClientOptions options,
        ZLinkHttpClient consumer,
        StoreFailureProcessManager processes,
        ManagedProcess providerB)
    {
        await SfProbe.WaitProviderRoutesAsync(
            consumer,
            options.PollingInterval * 4,
            "SF-C1: provider routes were not ready before api-b crashed.");

        await providerB.KillAsync();

        await SfProbe.WaitPeersAsync(
            consumer,
            SfProbe.PeerRows(options.OwnerLeaseTtl * 2 + options.PollingInterval * 4,
                present: ["api-a"],
                absent: ["api-b"]),
            "SF-C1: the crashed provider's row was not excluded after its lease expired.");

        await SfProbe.WaitRouteReadyAsync(
            consumer,
            minimumReadyMembers: 1,
            readyRids: ["api-a"],
            notReadyRids: ["api-b"],
            timeout: options.PollingInterval * 4,
            failure: "SF-C1: the surviving provider did not become selectable after the crashed row expired.");

        // Verify traffic lands on the survivor only — and fast (no repeated
        // timeouts against the dead endpoint).
        var stopwatch = Stopwatch.StartNew();
        for (var i = 0; i < 12; i++)
        {
            var reply = await SfProbe.RequestAsync(consumer, $"sf-c1-{i}");
            ZlinkStreamAssert.Ensure(
                reply.ProviderRid == "api-a",
                $"SF-C1: request {i} was served by '{reply.ProviderRid}' instead of the survivor.");
        }

        stopwatch.Stop();
        ZlinkStreamAssert.Ensure(
            stopwatch.Elapsed < TimeSpan.FromSeconds(12),
            "SF-C1: follow-up requests were slow, suggesting repeated timeouts against the dead endpoint.");

        Console.WriteLine("scenario SF-C1 passed");
    }
}
