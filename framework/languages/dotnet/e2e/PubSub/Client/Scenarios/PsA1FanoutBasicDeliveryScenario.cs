// Verifies PS-A1 Fanout Basic Delivery behavior.
using PubSub.Client.Support;
using PubSub.Shared;
using Zlink.HttpClient;

namespace PubSub.Client.Scenarios;

// PS-A1: verifies that one publisher fanouts a shared event range to every subscriber.
internal static class PsA1FanoutBasicDeliveryScenario
{
    public static async Task RunAsync(ZLinkHttpClient publisher, IReadOnlyList<ZLinkHttpClient> subscribers)
    {
        var runId = Guid.NewGuid().ToString("N");
        await publisher.Post("/publish/event")
            .Query("topic", PubSubNames.MainTopic)
            .Query("runId", $"activate-{runId}")
            .Query("sequence", "0")
            .Query("value", "activate-publisher")
            .AsyncRaw();
        await Task.WhenAll(subscribers.Select(subscriber =>
            SubscriberObservation.WaitForEventAsync(subscriber, $"activate-{runId}", 0)));

        var measureStart = 100;
        var measureCount = 12;

        // The measured range is separate from warm-up so the fanout oracle uses only post-barrier data.
        for (var i = measureStart; i < measureStart + measureCount; i++)
            await publisher.Post("/publish/event")
                .Query("topic", PubSubNames.MainTopic)
                .Query("runId", runId)
                .Query("sequence", i.ToString())
                .Query("value", $"measure-{i}")
                .AsyncRaw();

        // Publish submit does not promise remote delivery, so the scenario checks a shared contiguous range.
        var measuredSnapshots = await WaitForAllSubscribersAsync(
            subscribers,
            new EvidenceWaitReq(
                ["event|", $"run={runId}", $"topic={PubSubNames.MainTopic}"],
                [
                    [$"seq={measureStart}|"],
                    [$"seq={measureStart + 1}|"],
                    [$"seq={measureStart + 2}|"]
                ]));
        ZlinkStreamAssert.Ensure(
            Evidence.CommonContiguousSequence(
                measuredSnapshots,
                runId,
                PubSubNames.MainTopic,
                measureStart,
                measureStart + measureCount - 1).Count >= 3,
            "PS-A1 expected common contiguous sequence on all subscribers.");

        Console.WriteLine("scenario PS-A1 passed");
    }

    private static async Task<string[][]> WaitForAllSubscribersAsync(
        IReadOnlyList<ZLinkHttpClient> subscribers,
        EvidenceWaitReq request)
    {
        var waits = subscribers
            .Select(subscriber => subscriber.Post("/evidence/wait").Body(request).Async<string[]>().AsTask())
            .ToArray();
        var responses = await Task.WhenAll(waits);
        return responses.Select(response => response.Body).ToArray();
    }
}
