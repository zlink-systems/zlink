// Verifies PS-A4 Subscriber Reconnect behavior.
using PubSub.Client.Support;
using PubSub.Shared;
using Zlink.HttpClient;

namespace PubSub.Client.Scenarios;

// PS-A4 keeps one subscriber application alive while an external TCP proxy
// interrupts and restores only its publisher transport.
internal static class PsA4SubscriberReconnectScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient publisher,
        ZLinkHttpClient reconnectSubscriber,
        IReadOnlyList<ZLinkHttpClient> alwaysOnSubscribers,
        ServerProcessLauncher processes,
        string reconnectSubscriberUrl,
        string publisherEndpoint)
    {
        var runId = Guid.NewGuid().ToString("N");
        await PublishAsync(publisher, $"activate-{runId}", 0, "activate-publisher");
        // Keep the proxy upstream blocked until the subscriber host is healthy,
        // so the connection this scenario interrupts is the one it established.
        await using var fault = new NetworkFaultProxy(new Uri(publisherEndpoint), initiallyBlocked: true);
        using var subscriberProcess = processes.StartSubscriber(
            "sub-reconnect",
            reconnectSubscriberUrl,
            "sub-reconnect.evidence.log",
            fault.Endpoint.GetComponents(UriComponents.SchemeAndServer, UriFormat.Unescaped));
        try
        {
            await WaitForHealthAsync(reconnectSubscriber);
            fault.Unblock();
            await fault.WaitForUpstreamConnectionAsync();
            try
            {
                await FanoutReadiness.WaitUntilReceivingAsync(
                    publisher, reconnectSubscriber, "PS-A4 reconnect subscriber");
            }
            catch (Exception error)
            {
                throw new InvalidOperationException(
                    $"PS-A4 initial proxy connection did not become ready: {fault.Diagnostics}",
                    error);
            }

            await PublishAsync(publisher, runId, 1, "before-disconnect");
            await WaitForSubscribersAsync(
                alwaysOnSubscribers.Append(reconnectSubscriber).ToArray(), runId, 1);

            fault.Block();
            await Task.Delay(250);

            await PublishAsync(publisher, runId, 2, "while-disconnected");
            await WaitForSubscribersAsync(alwaysOnSubscribers, runId, 2);

            fault.Unblock();
            await fault.WaitForUpstreamConnectionAsync();
            var reconnectRun = $"ready-{Guid.NewGuid():N}";
            await PublishAsync(publisher, reconnectRun, 0, "reconnected");
            await SubscriberObservation.WaitForEventAsync(
                reconnectSubscriber,
                reconnectRun,
                0);

            await PublishAsync(publisher, runId, 3, "after-reconnect");
            await WaitForSubscribersAsync(
                alwaysOnSubscribers.Append(reconnectSubscriber).ToArray(), runId, 3);
            var evidence = (await reconnectSubscriber.Get("/evidence").Async<string[]>()).Body;
            ZlinkStreamAssert.Ensure(
                evidence.All(line =>
                    !line.Contains($"run={runId}", StringComparison.Ordinal)
                    || !line.Contains("value=while-disconnected", StringComparison.Ordinal)),
                "PS-A4 reconnected subscriber replayed the event published during disconnection.");
            ZlinkStreamAssert.Ensure(
                !subscriberProcess.HasExited,
                "PS-A4 subscriber application exited during the transport fault.");
            Console.WriteLine("scenario PS-A4 passed");
        }
        finally
        {
            if (!subscriberProcess.HasExited)
            {
                subscriberProcess.Kill(true);
                await subscriberProcess.WaitForExitAsync();
            }
        }
    }

    private static async Task PublishAsync(
        ZLinkHttpClient publisher,
        string runId,
        int sequence,
        string value) =>
        await publisher.Post("/publish/event")
            .Query("topic", PubSubNames.MainTopic)
            .Query("runId", runId)
            .Query("sequence", sequence.ToString())
            .Query("value", value)
            .AsyncRaw();

    private static Task WaitForHealthAsync(ZLinkHttpClient subscriber) =>
        StateObservation.WaitUntilAsync(
            async () =>
            {
                try
                {
                    return (await subscriber.Get("/health").AsyncRaw()).Status == 200;
                }
                catch (Exception error) when (
                    error is HttpRequestException || error.InnerException is HttpRequestException)
                {
                    return false;
                }
            },
            "PS-A4 subscriber did not become healthy.");

    private static async Task WaitForSubscribersAsync(
        IReadOnlyList<ZLinkHttpClient> subscribers,
        string runId,
        int sequence) =>
        await Task.WhenAll(subscribers.Select(subscriber =>
            SubscriberObservation.WaitForEventAsync(subscriber, runId, sequence)));
}
