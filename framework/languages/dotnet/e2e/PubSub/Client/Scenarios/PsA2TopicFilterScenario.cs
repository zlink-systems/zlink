// Verifies PS-A2 Topic Filter behavior.
using PubSub.Client.Support;
using PubSub.Shared;
using Zlink.HttpClient;

namespace PubSub.Client.Scenarios;

// PS-A2: verifies that subscribers accept the target topic and ignore non-interest topics.
internal static class PsA2TopicFilterScenario
{
    public static async Task RunAsync(ZLinkHttpClient publisher, IReadOnlyList<ZLinkHttpClient> subscribers)
    {
        var runId = Guid.NewGuid().ToString("N");

        // .NET exposes topic filtering at handler level here: one accepted topic and one ignored topic.
        await publisher.Post("/publish/event")
            .Query("topic", PubSubNames.MainTopic)
            .Query("runId", runId)
            .Query("sequence", "1")
            .Query("value", "accepted")
            .AsyncRaw();
        await publisher.Post("/publish/event")
            .Query("topic", PubSubNames.OtherTopic)
            .Query("runId", runId)
            .Query("sequence", "2")
            .Query("value", "ignored")
            .AsyncRaw();

        // Each subscriber records both the accepted event and the ignored marker for the other topic.
        var snapshots = await WaitForAllSubscribersAsync(
            subscribers,
            new EvidenceWaitReq(
                ["event|", $"run={runId}", $"topic={PubSubNames.MainTopic}"],
                [])
            {
                ContainsAllLineGroups =
                [
                    ["ignored|", $"run={runId}", $"topic={PubSubNames.OtherTopic}"]
                ]
            });

        // The non-interest topic must not be recorded as an accepted business event.
        ZlinkStreamAssert.Ensure(
            snapshots.All(lines => lines.Any(line => Evidence.IsEvent(line, runId, PubSubNames.MainTopic))),
            "PS-A2 accepted topic was not recorded.");
        ZlinkStreamAssert.Ensure(
            snapshots.All(lines => lines.All(line =>
                !line.Contains("event|", StringComparison.Ordinal)
                || !line.Contains($"run={runId}", StringComparison.Ordinal)
                || !line.Contains($"topic={PubSubNames.OtherTopic}", StringComparison.Ordinal))),
            "PS-A2 non-interest topic was recorded as accepted.");

        Console.WriteLine("scenario PS-A2 passed");
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
