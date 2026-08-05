// Verifies SF-B1 Fail Static behavior.
using StoreFailure.Client.Support;
using Zlink.HttpClient;

namespace StoreFailure.Client.Scenarios;

// SF-B1: a store outage must not translate into disconnects — the last
// desired set stays, established messaging keeps working, and the outage
// is visible only through runtime status.
internal static class SfB1FailStaticScenario
{
    public static async Task RunAsync(
        ClientOptions options,
        ZLinkHttpClient consumer,
        StoreFailureProcessManager processes)
    {
        await SfProbe.WaitProviderRoutesAsync(
            consumer,
            options.PollingInterval * 4,
            "SF-B1: provider routes were not ready before the store outage.");

        await processes.PauseStoreAsync();
        try
        {
            // Observe for less than the lease TTL: rows cannot have
            // expired yet, so any request failure would be a fail-static
            // violation, not lease bookkeeping.
            await SfProbe.DriveRequestsAsync(
                consumer,
                "sf-b1-outage",
                options.OwnerLeaseTtl * 0.7,
                "SF-B1");

            await SfProbe.WaitStatusAsync(
                consumer,
                SfProbe.Status(options.OwnerLeaseRenewInterval * 6,
                    storeHealthy: false),
                "SF-B1: the consumer's runtime status did not record the store outage.");
            await SfProbe.WaitStatusAsync(
                consumer,
                SfProbe.Status(options.OwnerLeaseRenewInterval * 6, ownerLeaseHealthy: false),
                "SF-B1: the consumer's owner lease heartbeat failure did not surface in status.");
        }
        finally
        {
            await processes.UnpauseStoreAsync();
        }

        await SfProbe.WaitStatusAsync(
            consumer,
            SfProbe.Status(options.OwnerLeaseRenewInterval * 8, storeHealthy: true, ownerLeaseHealthy: true),
            "SF-B1: the consumer's runtime status did not recover after the outage.");

        Console.WriteLine("scenario SF-B1 passed");
    }
}
