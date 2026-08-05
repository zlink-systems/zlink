// Verifies SF-B2 Grace Exceeded behavior.
using StoreFailure.Client.Support;
using Zlink.HttpClient;

namespace StoreFailure.Client.Scenarios;

// SF-B2: past the store failure grace, only NEW outbound connects stop;
// connections that were already ready keep serving as long as their
// transport lives.
internal static class SfB2GraceExceededScenario
{
    public static async Task RunAsync(
        ClientOptions options,
        ZLinkHttpClient consumer,
        StoreFailureProcessManager processes)
    {
        await SfProbe.WaitProviderRoutesAsync(
            consumer,
            options.PollingInterval * 4,
            "SF-B2: provider routes were not ready before the store outage.");

        await processes.PauseStoreAsync();
        try
        {
            // Drive traffic through the whole grace window and beyond it:
            // every request must ride the established connections.
            await SfProbe.DriveRequestsAsync(
                consumer,
                "sf-b2-grace",
                options.StoreFailureGrace + options.OwnerLeaseRenewInterval * 2,
                "SF-B2");

            var status = await SfProbe.GetStatusAsync(consumer);
            ZlinkStreamAssert.Ensure(
                !status.StoreHealthy,
                "SF-B2: the store outage was not visible in runtime status past the grace.");
        }
        finally
        {
            await processes.UnpauseStoreAsync();
        }

        await SfProbe.WaitStatusAsync(
            consumer,
            SfProbe.Status(options.OwnerLeaseRenewInterval * 8, storeHealthy: true, ownerLeaseHealthy: true),
            "SF-B2: the consumer's runtime status did not recover after the outage.");
        await SfProbe.WaitPeersAsync(
            consumer,
            SfProbe.PeerRows(options.OwnerLeaseTtl + options.OwnerLeaseRenewInterval * 4,
                present: ["api-a", "api-b"]),
            "SF-B2: provider rows did not return to the live list after recovery.");
        await SfProbe.WaitRouteReadyAsync(
            consumer,
            minimumReadyMembers: 2,
            readyRids: ["api-a", "api-b"],
            notReadyRids: null,
            timeout: options.PollingInterval * 4,
            failure: "SF-B2: provider routes did not become ready after store recovery.");

        Console.WriteLine("scenario SF-B2 passed");
    }
}
