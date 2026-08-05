using PubSub.Shared;
using Zlink.HttpClient;

namespace PubSub.Client.Support;

//  Classic fanout has no readiness surface to poll. Spec 24 §2.2 scopes
//  topology status to RouteMesh, ClientServer and automatic fanout, so the
//  runtime has nothing to answer about a manual fanout channel. Spec 29 §170
//  defines classic fanout readiness as the subscriber receiving its first valid
//  application record, which makes delivery itself the only available proof.
internal static class FanoutReadiness
{
    private static readonly TimeSpan DefaultTimeout = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan RetryInterval = TimeSpan.FromMilliseconds(200);

    //  The record is republished until a copy lands. A record published before
    //  the subscription has reached the publisher is dropped with no trace, so
    //  publishing once and waiting can wait forever on a record that no longer
    //  exists.
    public static async Task WaitUntilReceivingAsync(
        ZLinkHttpClient publisher,
        ZLinkHttpClient subscriber,
        string label,
        TimeSpan? timeout = null)
    {
        var runId = $"ready-{Guid.NewGuid():N}";
        var deadline = DateTimeOffset.UtcNow + (timeout ?? DefaultTimeout);
        var attempt = 0;
        while (true)
        {
            attempt++;
            await publisher.Post("/publish/event")
                .Query("topic", PubSubNames.MainTopic)
                .Query("runId", runId)
                .Query("sequence", attempt.ToString())
                .Query("value", "fanout-ready")
                .AsyncRaw();
            if (await ReceivedAsync(subscriber, runId))
                return;
            if (DateTimeOffset.UtcNow >= deadline)
                throw new InvalidOperationException(
                    $"{label} received none of the {attempt} readiness records "
                    + $"published to topic '{PubSubNames.MainTopic}'.");
            await Task.Delay(RetryInterval);
        }
    }

    private static async Task<bool> ReceivedAsync(ZLinkHttpClient subscriber, string runId)
    {
        var entries = (await subscriber.Get("/evidence").Async<string[]>()).Body;
        return entries.Any(entry => entry.Contains($"run={runId}", StringComparison.Ordinal));
    }
}
