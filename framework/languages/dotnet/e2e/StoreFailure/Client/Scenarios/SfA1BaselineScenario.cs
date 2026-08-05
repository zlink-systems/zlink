// Verifies SF-A1 Baseline behavior.
using StoreFailure.Client.Support;
using Zlink.HttpClient;

namespace StoreFailure.Client.Scenarios;

// SF-A1: with a healthy store, both provider rows are live, requests are
// served, and every node's runtime status reads healthy — the baseline
// the failure scenarios diff against.
internal static class SfA1BaselineScenario
{
    public static async Task RunAsync(
        ClientOptions options,
        ZLinkHttpClient consumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        await SfProbe.WaitPeersAsync(
            consumer,
            SfProbe.PeerRows(options.OwnerLeaseTtl + options.PollingInterval * 4,
                present: ["api-a", "api-b"]),
            "SF-A1: both provider rows did not appear in the consumer's peer list.");
        await SfProbe.WaitProviderRoutesAsync(
            consumer,
            options.PollingInterval * 4,
            "SF-A1: both provider routes did not become ready.");

        var served = new HashSet<string>(StringComparer.Ordinal);
        for (var i = 0; i < 8; i++)
        {
            var reply = await SfProbe.RequestAsync(consumer, $"sf-a1-{i}");
            ZlinkStreamAssert.Ensure(reply.Value == "profile:fast", "SF-A1: request returned an unexpected value.");
            served.Add(reply.ProviderRid);
        }

        ZlinkStreamAssert.Ensure(served.Count > 0, "SF-A1: no provider served the baseline traffic.");

        foreach (var (node, name) in new[] { (consumer, "consumer"), (providerA, "api-a"), (providerB, "api-b") })
        {
            var status = await SfProbe.WaitStatusAsync(
                node,
                SfProbe.Status(options.OwnerLeaseRenewInterval * 4,
                    storeHealthy: true, ownerLeaseHealthy: true),
                $"SF-A1: {name} runtime status did not report a healthy store and lease.");
            ZlinkStreamAssert.Ensure(
                status.LastRefreshAt is not null,
                $"SF-A1: {name} did not report a last refresh timestamp.");
        }

        Console.WriteLine("scenario SF-A1 passed");
    }
}
