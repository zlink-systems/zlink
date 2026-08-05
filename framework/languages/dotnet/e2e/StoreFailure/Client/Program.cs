using StoreFailure.Client.Scenarios;
using StoreFailure.Client.Support;
using Zlink.HttpClient;

var options = ClientOptions.Parse(args);

using var consumer = ZLinkHttpClient.Create(options.ConsumerUrl)
    .Timeout(TimeSpan.FromMinutes(10))
    .Build();
using var providerA = ZLinkHttpClient.Create(options.ProviderAUrl).Build();
using var providerB = ZLinkHttpClient.Create(options.ProviderBUrl).Build();
await using var processes = new StoreFailureProcessManager(options);

// api-b lives under the client so scenarios can SIGKILL and restart it;
// the run script only owns the store, api-a, and the consumer.
var providerBProcess = await processes.StartProviderBAsync();

async Task RestartProviderBAsync()
{
    providerBProcess = await processes.StartProviderBAsync();
    await SfProbe.WaitPeersAsync(
        consumer,
        SfProbe.PeerRows(options.OwnerLeaseTtl + options.OwnerLeaseRenewInterval * 4, present: ["api-b"]),
        "setup: restarted api-b did not re-register.");
}

var scenarios = new (string Name, Func<Task> Run)[]
{
    ("SF-A1", () => SfA1BaselineScenario.RunAsync(options, consumer, providerA, providerB)),
    ("SF-A2", () => SfA2PollingFallbackScenario.RunAsync(options, processes)),
    ("SF-B1", () => SfB1FailStaticScenario.RunAsync(options, consumer, processes)),
    ("SF-B2", () => SfB2GraceExceededScenario.RunAsync(options, consumer, processes)),
    ("SF-D1", () => SfD1ShortOutageRecoveryScenario.RunAsync(options, consumer, processes)),
    ("SF-D3", () => SfD3StatusTransitionScenario.RunAsync(options, consumer, processes)),
    ("SF-C2", async () =>
    {
        await SfC2GracefulRemovalScenario.RunAsync(options, consumer, providerBProcess);
        await RestartProviderBAsync();
    }),
    ("SF-C1", async () =>
    {
        await SfC1CrashLeaseExpiryScenario.RunAsync(options, consumer, processes, providerBProcess);
        await RestartProviderBAsync();
    }),
    ("SF-D2", async () =>
    {
        await SfD2LongOutageRecoveryScenario.RunAsync(options, consumer, processes, providerBProcess);
        await RestartProviderBAsync();
    }),
    ("SF-E1", () => SfE1StoreDelayNonBlockingScenario.RunAsync(options, consumer))
};

foreach (var name in SelectedScenarioNames(options.Scenario, scenarios.Select(scenario => scenario.Name)))
{
    var selected = scenarios.FirstOrDefault(scenario =>
        string.Equals(scenario.Name, name, StringComparison.OrdinalIgnoreCase));
    if (selected.Run is null) throw new ArgumentException($"Unknown scenario '{name}'.");

    await selected.Run();
}

Console.WriteLine("store-failure client result=passed");

static IEnumerable<string> SelectedScenarioNames(string selector, IEnumerable<string> allNames)
{
    if (string.Equals(selector, "all", StringComparison.OrdinalIgnoreCase))
        return allNames;

    return selector
        .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
}
