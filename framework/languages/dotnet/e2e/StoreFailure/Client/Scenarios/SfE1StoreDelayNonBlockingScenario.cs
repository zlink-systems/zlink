// Verifies SF-E1 Store Delay Non Blocking behavior.
using System.Diagnostics;
using StoreFailure.Client.Support;
using StoreFailure.Shared;
using Zlink.HttpClient;

namespace StoreFailure.Client.Scenarios;

internal static class SfE1StoreDelayNonBlockingScenario
{
    private const int DelayMilliseconds = 1200;

    public static async Task RunAsync(
        ClientOptions options,
        ZLinkHttpClient consumer)
    {
        await SfProbe.WaitPeersAsync(
            consumer,
            SfProbe.PeerRows(options.OwnerLeaseTtl + options.OwnerLeaseRenewInterval * 4,
                present: ["api-a", "api-b"]),
            "SF-E1: baseline peers were not ready.");

        var baseline = await MeasureRequestsAsync(consumer, "SF-E1-baseline", 10);
        var baselineP99 = PercentileMilliseconds(baseline, 0.99);

        await SetDelayAsync(consumer, DelayMilliseconds);
        try
        {
            var delayedStoreRead = MeasureStoreReadAsync(consumer);
            await Task.Delay(150);
            var concurrent = await MeasureRequestsAsync(consumer, "SF-E1-concurrent", 12);
            var delayedStoreReadMs = await delayedStoreRead;
            var concurrentP99 = PercentileMilliseconds(concurrent, 0.99);
            var budget = Math.Max(baselineP99 * 8, 750);

            ZlinkStreamAssert.Ensure(
                delayedStoreReadMs >= DelayMilliseconds * 0.75,
                $"SF-E1: delayed store read finished too quickly ({delayedStoreReadMs:0} ms).");
            ZlinkStreamAssert.Ensure(
                concurrentP99 <= budget,
                $"SF-E1: unrelated request p99 grew too much during store delay. baseline={baselineP99:0} ms concurrent={concurrentP99:0} ms budget={budget:0} ms.");

            var recovery = await SfProbe.RequestAsync(consumer, "SF-E1-recovery");
            ZlinkStreamAssert.Ensure(
                recovery.Value == "profile:fast",
                "SF-E1: request path did not recover after delayed store read.");

            Console.WriteLine("scenario SF-E1 passed");
        }
        finally
        {
            await SetDelayAsync(consumer, 0);
        }
    }

    private static async Task<double> MeasureStoreReadAsync(ZLinkHttpClient consumer)
    {
        var sw = Stopwatch.StartNew();
        var peers = await SfProbe.WaitPeersAsync(
            consumer,
            SfProbe.PeerRows(TimeSpan.FromSeconds(10), present: ["api-a", "api-b"]),
            "SF-E1: delayed peer query did not return both providers.");
        sw.Stop();
        ZlinkStreamAssert.Ensure(peers.Length >= 2, "SF-E1: delayed peer query returned too few rows.");
        return sw.Elapsed.TotalMilliseconds;
    }

    private static async Task<IReadOnlyList<double>> MeasureRequestsAsync(
        ZLinkHttpClient consumer,
        string markerPrefix,
        int count)
    {
        var timings = new List<double>(count);
        for (var i = 0; i < count; i++)
        {
            var sw = Stopwatch.StartNew();
            var reply = await SfProbe.RequestAsync(consumer, $"{markerPrefix}-{i}", 3000);
            sw.Stop();
            ZlinkStreamAssert.Ensure(
                reply.Value == "profile:fast",
                $"SF-E1: request {i} returned an unexpected value.");
            timings.Add(sw.Elapsed.TotalMilliseconds);
        }

        return timings;
    }

    private static double PercentileMilliseconds(IReadOnlyList<double> values, double percentile)
    {
        var sorted = values.OrderBy(static value => value).ToArray();
        var index = (int)Math.Ceiling(percentile * sorted.Length) - 1;
        return sorted[Math.Clamp(index, 0, sorted.Length - 1)];
    }

    private static async Task SetDelayAsync(ZLinkHttpClient consumer, int milliseconds) =>
        await consumer.Post("/admin/store-delay")
            .Body(new StoreDelayReq(milliseconds))
            .AsyncRaw();
}
