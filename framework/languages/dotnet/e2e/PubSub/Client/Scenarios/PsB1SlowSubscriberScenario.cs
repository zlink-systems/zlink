// Verifies PS-B1 Slow Subscriber behavior.
using PubSub.Client.Support;
using PubSub.Shared;
using Zlink.HttpClient;

namespace PubSub.Client.Scenarios;

// PS-B1: verifies that a slow subscriber handler does not block other subscribers.
internal static class PsB1SlowSubscriberScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient publisher,
        IReadOnlyList<ZLinkHttpClient> fastSubscribers,
        ZLinkHttpClient slowSubscriber)
    {
        var runId = Guid.NewGuid().ToString("N");

        // The first value triggers delay in the configured slow subscriber handler.
        for (var i = 1; i <= 8; i++)
        {
            var value = i == 1 ? "slow-first" : $"fast-after-slow-{i}";
            await publisher.Post("/publish/event")
                .Query("topic", PubSubNames.MainTopic)
                .Query("runId", runId)
                .Query("sequence", i.ToString())
                .Query("value", value)
                .AsyncRaw();
        }

        // Fast subscribers should still reach the tail event while the slow handler is delayed.
        var fastWaits = fastSubscribers
            .Select(subscriber => subscriber.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(
                    [],
                    [],
                    2000)
                {
                    ContainsAllLineGroups =
                    [
                        ["event|", $"run={runId}", $"topic={PubSubNames.MainTopic}", "seq=8"]
                    ]
                })
                .Async<string[]>()
                .AsTask())
            .ToArray();
        await Task.WhenAll(fastWaits);

        // The slow subscriber evidence proves that this scenario actually exercised the delayed path.
        var slowEvidence = (await slowSubscriber.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(
                [],
                [])
            {
                ContainsAllLineGroups =
                [
                    ["delay-start|", $"run={runId}"]
                ]
            })
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            slowEvidence.Any(line => line.Contains("delay-start|", StringComparison.Ordinal)
                                     && line.Contains($"run={runId}", StringComparison.Ordinal)),
            "PS-B1 expected slow subscriber delay evidence.");
        Console.WriteLine("scenario PS-B1 passed");
    }
}
