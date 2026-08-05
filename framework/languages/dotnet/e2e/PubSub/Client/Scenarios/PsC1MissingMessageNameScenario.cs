// Verifies PS-C1 Missing Message Name behavior.
using PubSub.Shared;
using Zlink.HttpClient;

namespace PubSub.Client.Scenarios;

// PS-C1: verifies that an unregistered publish message is dropped and normal delivery continues.
internal static class PsC1MissingMessageNameScenario
{
    public static async Task RunAsync(ZLinkHttpClient publisher, IReadOnlyList<ZLinkHttpClient> subscribers)
    {
        var runId = Guid.NewGuid().ToString("N");

        // Publish a packet name that subscribers did not register, so dispatch should drop it.
        await publisher.Post("/publish/missing")
            .Query("topic", PubSubNames.MainTopic)
            .Query("runId", runId)
            .Query("sequence", "1")
            .Query("value", "missing")
            .AsyncRaw();

        // The drop is observed on subscriber evidence because publisher submit only reaches transport.
        await WaitForSubscribersAsync(subscribers, new EvidenceWaitReq(
            [],
            [])
        {
            ContainsAllLineGroups =
            [
                ["dispatch-error|", "packet=MissingEventMsg", $"topic={PubSubNames.MainTopic}"]
            ]
        });

        // A normal event after the missing handler case proves the channel continues to work.
        await publisher.Post("/publish/event")
            .Query("topic", PubSubNames.MainTopic)
            .Query("runId", runId)
            .Query("sequence", "2")
            .Query("value", "after-missing")
            .AsyncRaw();
        await WaitForSubscribersAsync(subscribers, new EvidenceWaitReq(
            ["event|", $"run={runId}", $"topic={PubSubNames.MainTopic}"],
            []));

        Console.WriteLine("scenario PS-C1 passed");
    }

    private static async Task WaitForSubscribersAsync(
        IReadOnlyList<ZLinkHttpClient> subscribers,
        EvidenceWaitReq request)
    {
        var waits = subscribers
            .Select(subscriber => subscriber.Post("/evidence/wait").Body(request).Async<string[]>().AsTask())
            .ToArray();
        await Task.WhenAll(waits);
    }
}
