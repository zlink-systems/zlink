// Verifies SF-A2 Polling Fallback behavior.
using StoreFailure.Client.Support;
using Zlink.HttpClient;

namespace StoreFailure.Client.Scenarios;

// SF-A2: the opaque Store SPI reflects peer additions and removals through
// polling without a provider-specific notification capability.
internal static class SfA2PollingFallbackScenario
{
    public static async Task RunAsync(
        ClientOptions options,
        StoreFailureProcessManager processes)
    {
        var consumerNw = await processes.StartConsumerNwAsync();
        using var observer = ZLinkHttpClient.Create(options.ConsumerNwUrl).Build();
        try
        {
            var providerC = await processes.StartProviderCAsync();
            await SfProbe.WaitPeersAsync(
                observer,
                SfProbe.PeerRows(options.PollingInterval * 8 + options.OwnerLeaseRenewInterval,
                    present: ["api-c"]),
                "SF-A2: the added provider did not appear through pure polling.");

            await providerC.StopAsync();
            await SfProbe.WaitPeersAsync(
                observer,
                SfProbe.PeerRows(options.PollingInterval * 8 + options.OwnerLeaseRenewInterval,
                    absent: ["api-c"]),
                "SF-A2: the removed provider did not disappear through pure polling.");
        }
        finally
        {
            await consumerNw.StopAsync();
        }

        Console.WriteLine("scenario SF-A2 passed");
    }
}
