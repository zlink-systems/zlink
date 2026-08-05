using System.Diagnostics;
using PubSub.Client.Scenarios;
using PubSub.Client.Support;
using Zlink.HttpClient;

var options = ClientOptions.Parse(args);
Directory.CreateDirectory(options.LogDir);

using var publisher = ZLinkHttpClient.Create(options.PublisherUrl).Build();
using var lateSubscriber = ZLinkHttpClient.Create(options.LateSubscriberUrl).Build();
var subscribers = options.SubscriberUrls
    .Select(url => ZLinkHttpClient.Create(url).Build())
    .ToArray();
var processLauncher = new ServerProcessLauncher(options);
Process? restartedPublisher = null;

try
{
    var scenarios = new Dictionary<string, Func<Task>>(StringComparer.OrdinalIgnoreCase)
    {
        ["PS-A1"] = () => PsA1FanoutBasicDeliveryScenario.RunAsync(publisher, subscribers),
        ["PS-A2"] = () => PsA2TopicFilterScenario.RunAsync(publisher, subscribers),
        ["PS-A3"] = () => PsA3LateSubscriberScenario.RunAsync(
            publisher,
            lateSubscriber,
            subscribers,
            processLauncher,
            options.LateSubscriberUrl),
        ["PS-A4"] = () => PsA4SubscriberReconnectScenario.RunAsync(
            publisher,
            lateSubscriber,
            subscribers,
            processLauncher,
            options.LateSubscriberUrl,
            options.PublisherEndpoint),
        ["PS-B1"] = () => PsB1SlowSubscriberScenario.RunAsync(publisher, subscribers.Take(2).ToArray(), subscribers[^1]),
        ["PS-B2"] = async () => restartedPublisher = await PsB2PublisherRestartScenario.RunAsync(
            publisher,
            subscribers,
            processLauncher),
        ["PS-C1"] = () => PsC1MissingMessageNameScenario.RunAsync(publisher, subscribers)
    };

    foreach (var name in SelectedScenarioNames(options.Scenario, scenarios.Keys))
    {
        if (scenarios.TryGetValue(name, out var selected))
            await selected();
        else
            throw new ArgumentException($"Unknown scenario '{name}'.");
    }
}
finally
{
    if (restartedPublisher is { HasExited: false })
    {
        await publisher.Post("/shutdown").AsyncRaw();
        await restartedPublisher.WaitForExitAsync();
    }

    foreach (var subscriber in subscribers) subscriber.Dispose();
}

Console.WriteLine("pubsub e2e result=passed");

static IEnumerable<string> SelectedScenarioNames(string selector, IEnumerable<string> allNames)
{
    if (string.Equals(selector, "all", StringComparison.OrdinalIgnoreCase))
        return allNames;

    return selector
        .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
}
