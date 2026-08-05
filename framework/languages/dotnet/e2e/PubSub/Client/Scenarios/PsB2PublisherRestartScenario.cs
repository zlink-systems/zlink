// Verifies PS-B2 Publisher Restart behavior.
using System.Diagnostics;
using PubSub.Client.Support;
using PubSub.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace PubSub.Client.Scenarios;

// PS-B2: verifies that subscribers receive new events after the publisher process restarts.
internal static class PsB2PublisherRestartScenario
{
    public static async Task<Process> RunAsync(
        ZLinkHttpClient publisher,
        IReadOnlyList<ZLinkHttpClient> subscribers,
        ServerProcessLauncher processes)
    {
        var runId = Guid.NewGuid().ToString("N");

        // Establish a baseline delivery before stopping the publisher process.
        await publisher.Post("/publish/event")
            .Query("topic", PubSubNames.MainTopic)
            .Query("runId", runId)
            .Query("sequence", "1")
            .Query("value", "before-publisher-restart")
            .AsyncRaw();
        await WaitForSubscribersAsync(subscribers, new EvidenceWaitReq(
            ["event|", $"run={runId}", $"topic={PubSubNames.MainTopic}", "seq=1"],
            []));
        // Classic fanout registers no location store (config-3): a normal
        // replacement reaches terminal Drained, and the subscribers observe the
        // publisher going away through their own socket disconnect evidence
        // rather than a store row removal.
        var drain = (await publisher.Post("/admin/drain")
            .Timeout(TimeSpan.FromSeconds(35))
            .Async<DrainResultRes>()).Body;
        ZlinkStreamAssert.Ensure(
            drain.Result == nameof(Zlink.Framework.Contracts.Configuration.ZLinkFrameworkTerminationOutcome.Stopped),
            $"PS-B2 expected terminal Stopped, got {drain.Result}:{drain.Reason}.");

        await publisher.Post("/shutdown").AsyncRaw();
        await StateObservation.WaitUntilAsync(
            async () =>
            {
                try
                {
                    return (await publisher.Get("/health").AsyncRaw()).Status != 200;
                }
                catch (Exception ex) when ((ex is HttpRequestException || ex.InnerException is HttpRequestException))
                {
                    return true;
                }
            },
            "PS-B2 expected publisher to stop before restart.");
        // A publish while the process is down should fail at the HTTP boundary.
        await ZlinkStreamAssert.ExpectFailureAsync(async cancellationToken =>
            _ = await publisher.Post("/publish/event")
                .Query("topic", PubSubNames.MainTopic)
                .Query("runId", runId)
                .Query("sequence", "2")
                .Query("value", "during-publisher-down")
                .AsyncRaw(cancellationToken));

        // Restart the same publisher role and wait for the health endpoint before measuring recovery.
        var restartedPublisher = processes.StartPublisher();
        await StateObservation.WaitUntilAsync(
            async () =>
            {
                try
                {
                    return (await publisher.Get("/health").AsyncRaw()).Status == 200;
                }
                catch (Exception ex) when ((ex is HttpRequestException || ex.InnerException is HttpRequestException))
                {
                    return false;
                }
            },
            "PS-B2 expected restarted publisher to become healthy.");
        // A publisher health response does not mean that every existing fanout
        // subscription has completed its new transport handshake. Classic
        // fanout has no public connection-status surface, so use the existing
        // bounded delivery barrier to wait for each subscriber's first valid
        // post-restart record before publishing the measured marker.
        await Task.WhenAll(subscribers.Select((subscriber, index) =>
            FanoutReadiness.WaitUntilReceivingAsync(
                publisher,
                subscriber,
                $"PS-B2 subscriber {index + 1} after publisher restart")));

        // The subscribers keep their subscriptions; this second marker is the
        // scenario-visible recovery evidence after all subscriptions are ready.
        var readyRun = $"ready-{Guid.NewGuid():N}";
        await publisher.Post("/publish/event")
            .Query("topic", PubSubNames.MainTopic)
            .Query("runId", readyRun)
            .Query("sequence", "0")
            .Query("value", "publisher-restarted")
            .AsyncRaw();
        await Task.WhenAll(subscribers.Select(subscriber =>
            SubscriberObservation.WaitForEventAsync(subscriber, readyRun, 0)));

        // Every subscriber received the readiness probe before this measurement.
        await publisher.Post("/publish/event")
            .Query("topic", PubSubNames.MainTopic)
            .Query("runId", runId)
            .Query("sequence", "3")
            .Query("value", "after-publisher-restart")
            .AsyncRaw();

        // Recovery is proven by this exact first post-readiness event at every subscriber.
        await WaitForSubscribersAsync(subscribers, new EvidenceWaitReq(
            [],
            [])
        {
            ContainsAllLineGroups =
            [
                ["event|", $"run={runId}", $"topic={PubSubNames.MainTopic}", "seq=3|"]
            ]
        });
        Console.WriteLine("scenario PS-B2 passed");
        return restartedPublisher;
    }

    private static async Task WaitForSubscribersAsync(
        IReadOnlyList<ZLinkHttpClient> subscribers,
        EvidenceWaitReq request)
    {
        var waits = subscribers
            .Select(subscriber => subscriber.Post("/evidence/wait")
                .Body(request)
                // The server may use the full evidence wait window. Keep the HTTP
                // deadline slightly longer so a valid terminal response is observable.
                .Timeout(TimeSpan.FromMilliseconds(request.TimeoutMilliseconds + 2000))
                .Async<string[]>()
                .AsTask())
            .ToArray();
        await Task.WhenAll(waits);
    }
}
