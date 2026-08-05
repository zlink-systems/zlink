// Verifies SF-D1 Short Outage Recovery behavior.
using System.Diagnostics;
using StoreFailure.Client.Support;
using Zlink.HttpClient;

namespace StoreFailure.Client.Scenarios;

// SF-D1: an outage shorter than the lease TTL passes without any harm —
// the first successful lookup resumes reconcile, no connection is torn
// down, and requests never miss a beat.
internal static class SfD1ShortOutageRecoveryScenario
{
    public static async Task RunAsync(
        ClientOptions options,
        ZLinkHttpClient consumer,
        StoreFailureProcessManager processes)
    {
        await SfProbe.WaitProviderRoutesAsync(
            consumer,
            options.PollingInterval * 4,
            "SF-D1: provider routes were not ready before the store outage.");

        var traffic = Task.Run(() => SfProbe.DriveRequestsAsync(
            consumer,
            "sf-d1",
            options.OwnerLeaseTtl * 2,
            "SF-D1"));

        var outage = Stopwatch.StartNew();
        await processes.PauseStoreAsync();
        try
        {
            await Task.Delay(options.OwnerLeaseTtl * 0.5);
        }
        finally
        {
            await processes.UnpauseStoreAsync();
        }
        outage.Stop();
        ZlinkStreamAssert.Ensure(
            outage.Elapsed < options.OwnerLeaseTtl,
            $"SF-D1: the requested short outage took {outage.Elapsed}, exceeding the lease TTL.");
        Console.WriteLine($"SF-D1 short outage elapsed_ms={outage.Elapsed.TotalMilliseconds:0}");

        // Every request across pause, outage, and recovery must succeed.
        await traffic;

        await SfProbe.WaitStatusAsync(
            consumer,
            SfProbe.Status(options.OwnerLeaseRenewInterval * 8, storeHealthy: true, ownerLeaseHealthy: true),
            "SF-D1: runtime status did not return to healthy after the short outage.");
        await SfProbe.WaitPeersAsync(
            consumer,
            SfProbe.PeerRows(options.OwnerLeaseTtl + options.OwnerLeaseRenewInterval * 4,
                present: ["api-a", "api-b"]),
            "SF-D1: provider rows were not all live after the short outage.");

        Console.WriteLine("scenario SF-D1 passed");
    }
}
