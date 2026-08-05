// Verifies SF-D2 Long Outage Recovery behavior.
using System.Diagnostics;
using StoreFailure.Client.Support;
using StoreFailure.Shared;
using Zlink.HttpClient;

namespace StoreFailure.Client.Scenarios;

// SF-D2: after an outage long enough to expire every lease, each node
// re-registers itself before trusting lookups, and the disconnect diff
// waits one heartbeat interval — so the surviving peer is never cut,
// while the peer that actually died during the outage is dropped.
internal static class SfD2LongOutageRecoveryScenario
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
            "SF-D2: provider routes were not ready before the store outage.");

        var trafficWindow = options.OwnerLeaseTtl * 2 + options.OwnerLeaseRenewInterval * 4;
        var traffic = Task.Run(() => DriveTolerantRequestsAsync(
            consumer,
            trafficWindow,
            options));

        await processes.PauseStoreAsync();
        try
        {
            // All leases expire during this window, and api-b really dies:
            // recovery has to tell a stale-but-alive peer from a dead one.
            await providerB.KillAsync();
            await Task.Delay(options.OwnerLeaseTtl + options.OwnerLeaseRenewInterval);
        }
        finally
        {
            await processes.UnpauseStoreAsync();
        }

        // The surviving api-a link is never disconnected: requests that
        // round-robined into the killed peer may time out until the dead
        // transport is noticed, but successes must never stop flowing.
        var replies = await traffic;
        ZlinkStreamAssert.Ensure(
            replies.Any(reply => reply.ProviderRid == "api-a"),
            "SF-D2: no request was served by the surviving provider.");

        // api-a re-registers its lease and row before any disconnect diff.
        await SfProbe.WaitPeersAsync(
            consumer,
            SfProbe.PeerRows(options.OwnerLeaseRenewInterval * 6, present: ["api-a"]),
            "SF-D2: the surviving provider did not re-register after recovery.");

        // The dead peer drops out after the one-heartbeat re-registration
        // grace; the survivor keeps serving.
        await SfProbe.WaitPeersAsync(
            consumer,
            SfProbe.PeerRows(options.OwnerLeaseTtl * 2 + options.OwnerLeaseRenewInterval * 4,
                absent: ["api-b"]),
            "SF-D2: the provider that died during the outage was not dropped after recovery.");
        await WaitForDeadProviderDisconnectAsync(consumer, options);

        for (var i = 0; i < 8; i++)
        {
            var reply = await SfProbe.RequestAsync(consumer, $"sf-d2-after-{i}");
            ZlinkStreamAssert.Ensure(
                reply.ProviderRid == "api-a",
                $"SF-D2: post-recovery request {i} was served by '{reply.ProviderRid}'.");
        }

        Console.WriteLine("scenario SF-D2 passed");
    }

    /// <summary>
    /// Steady request cadence where a request that rode the killed peer's
    /// dying transport may fail, but the surviving link must keep serving:
    /// no success gap longer than a couple of seconds is allowed.
    /// </summary>
    private static async Task<List<ProfileRes>> DriveTolerantRequestsAsync(
        ZLinkHttpClient consumer,
        TimeSpan window,
        ClientOptions options)
    {
        var replies = new List<ProfileRes>();
        var elapsed = Stopwatch.StartNew();
        var lastSuccess = TimeSpan.Zero;
        var maxGap = TimeSpan.Zero;
        var index = 0;
        while (elapsed.Elapsed < window)
        {
            try
            {
                var reply = await SfProbe.RequestAsync(consumer, $"sf-d2-{index++}");
                ZlinkStreamAssert.Ensure(
                    reply.Value == "profile:fast",
                    "SF-D2: request returned an unexpected value.");
                replies.Add(reply);
                var gap = elapsed.Elapsed - lastSuccess;
                if (gap > maxGap) maxGap = gap;

                lastSuccess = elapsed.Elapsed;
            }
            catch
            {
                // A request that landed on the killed peer's transport.
            }

            await Task.Delay(150);
        }

        var finalGap = elapsed.Elapsed - lastSuccess;
        if (finalGap > maxGap) maxGap = finalGap;

        ZlinkStreamAssert.Ensure(replies.Count > 0, "SF-D2: the request window produced no successful traffic.");
        ZlinkStreamAssert.Ensure(
            maxGap < options.OwnerLeaseTtl * 2,
            $"SF-D2: successful traffic stalled for {maxGap}, longer than the dead-transport tolerance.");
        return replies;
    }

    private static async Task WaitForDeadProviderDisconnectAsync(
        ZLinkHttpClient consumer,
        ClientOptions options)
    {
        var timeout = options.OwnerLeaseRenewInterval + options.PollingInterval * 4;
        var elapsed = Stopwatch.StartNew();
        var probeTimeoutMilliseconds = (int)Math.Clamp(
            Math.Ceiling(options.PollingInterval.TotalMilliseconds),
            1,
            1000);
        Exception? last = null;
        while (elapsed.Elapsed < timeout)
        {
            var allOnSurvivor = true;
            for (var i = 0; i < 4; i++)
            {
                try
                {
                    var reply = await SfProbe.RequestAsync(
                        consumer,
                        $"sf-d2-disconnect-probe-{Guid.NewGuid():N}",
                        probeTimeoutMilliseconds);
                    if (reply.ProviderRid != "api-a")
                    {
                        allOnSurvivor = false;
                        break;
                    }
                }
                catch (Exception ex)
                {
                    last = ex;
                    allOnSurvivor = false;
                    break;
                }
            }

            if (allOnSurvivor) return;

            await Task.Delay(options.PollingInterval, CancellationToken.None);
        }

        throw new InvalidOperationException(
            "SF-D2: the dead provider transport was still selected after the disconnect grace.",
            last);
    }
}
