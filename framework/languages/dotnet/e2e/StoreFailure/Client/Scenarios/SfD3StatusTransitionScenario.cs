// Verifies SF-D3 Status Transition behavior.
using StoreFailure.Client.Support;
using StoreFailure.Shared;
using Zlink.HttpClient;

namespace StoreFailure.Client.Scenarios;

// SF-D3: one outage cycle shows up in runtime status as an ordered
// healthy -> unhealthy(health and lease failure) -> healthy(+fresh refresh)
// transition.
internal static class SfD3StatusTransitionScenario
{
    public static async Task RunAsync(
        ClientOptions options,
        ZLinkHttpClient consumer,
        StoreFailureProcessManager processes)
    {
        var before = await SfProbe.WaitStatusAsync(
            consumer,
            SfProbe.Status(options.OwnerLeaseRenewInterval * 6, storeHealthy: true, ownerLeaseHealthy: true),
            "SF-D3: the pre-outage status was not healthy.");

        await processes.PauseStoreAsync();
        RuntimeStatusRes during;
        try
        {
            during = await SfProbe.WaitStatusAsync(
                consumer,
                SfProbe.Status(options.OwnerLeaseRenewInterval * 8,
                    storeHealthy: false, ownerLeaseHealthy: false),
                "SF-D3: the outage did not surface as unhealthy status.");
        }
        finally
        {
            await processes.UnpauseStoreAsync();
        }

        var after = await SfProbe.WaitStatusAsync(
            consumer,
            SfProbe.Status(options.OwnerLeaseRenewInterval * 8,
                storeHealthy: true,
                ownerLeaseHealthy: true,
                requireLastRefresh: true,
                lastRefreshAfter: before.LastRefreshAt),
            "SF-D3: status did not return to healthy after recovery.");

        ZlinkStreamAssert.Ensure(
            before.LastRefreshAt is not null && after.LastRefreshAt > before.LastRefreshAt,
            $"SF-D3: the post-recovery refresh timestamp did not advance " +
            $"(before={before.LastRefreshAt:O}, during={during.LastRefreshAt:O}, " +
            $"after={after.LastRefreshAt:O}).");
        Console.WriteLine("scenario SF-D3 passed");
    }
}
