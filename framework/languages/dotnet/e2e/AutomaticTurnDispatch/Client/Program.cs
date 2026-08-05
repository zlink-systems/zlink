using AutomaticTurnDispatch.Client.Scenarios;
using AutomaticTurnDispatch.Client.Support;
using AutomaticTurnDispatch.Shared;
using Zlink.HttpClient;

var options = ClientOptions.Parse(args);

if (options.Scenario is "shutdown-wait")
{
    await ShutdownAwaitProbe.RunWaitAsync(options);
    return;
}
if (options.Scenario is "shutdown-recovery")
{
    await ShutdownAwaitProbe.RunRecoveryAsync(options);
    return;
}

using var playA = CreateOptionalHttpClient(options.PlayAUrl);
using var playB = CreateOptionalHttpClient(options.PlayBUrl);

await using var client =
    ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
    {
        Endpoint = new Uri(options.SessionAStreamEndpoint),
        ConnectTimeout = TimeSpan.FromSeconds(5),
        RequestTimeout = TimeSpan.FromSeconds(60),
        Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
        DispatchMode = ZlinkStreamDispatchMode.Immediate,
        MaxReceivedMessages = 1024
    });
client.ErrorReceived += (error, _) =>
{
    Console.Error.WriteLine(
        $"automatic-turn-dispatch connector error code={error.Code} message={error.Message}");
    return ValueTask.CompletedTask;
};
client.ConnectionStateChanged += (change, _) =>
{
    Console.Error.WriteLine(
        $"automatic-turn-dispatch connector state previous={change.Previous} current={change.Current}"
        + $" error={change.Error?.Code}:{change.Error?.Message}");
    return ValueTask.CompletedTask;
};
client.Disconnected += (disconnected, _) =>
{
    Console.Error.WriteLine(
        $"automatic-turn-dispatch connector disconnected reason={disconnected.CloseReason}");
    return ValueTask.CompletedTask;
};
await client.Connect.Async();

var context = new ExecutionTurnScenarioContext(client, playA, playB);
var scenarios = new (string Id, Func<Task> Run)[]
{
    ("TD-A1", () => TdA1TerminatorSurfaceScenario.RunAsync(context)),
    ("TD-A2", () => TdA2AsyncCompletionOrderScenario.RunAsync(context)),
    ("TD-A3", () => TdA3AsyncCounterSerializationScenario.RunAsync(context)),
    ("TD-A4", () => TdA4DelayedAsyncCompletionScenario.RunAsync(context)),
    ("TD-A5", () => TdA5AsyncTimerExclusionScenario.RunAsync(context)),
    ("TD-B1", () => TdB1YieldProbeInterleaveScenario.RunAsync(context)),
    ("TD-B2", () => TdB2YieldQueuedProbeOrderScenario.RunAsync(context)),
    ("TD-B3", () => TdB3YieldLostUpdateScenario.RunAsync(context)),
    ("TD-B4", () => TdB4YieldTimerInterleaveScenario.RunAsync(context)),
    ("TD-C1", () => TdC1HttpYieldInterleaveScenario.RunAsync(context)),
    ("TD-C2", () => TdC2HttpAsyncExclusionScenario.RunAsync(context)),
    ("TD-C3", () => TdC3IoWorkerCapacityScenario.RunAsync(context)),
    ("TD-C4", () => TdC4CpuWorkerTurnOrderScenario.RunAsync(context)),
    ("TD-C5", () => TdC5CpuWorkerSourceGateScenario.RunAsync(context)),
    ("TD-D1", () => TdD1CrossActorYieldInterleaveScenario.RunAsync(context)),
    ("TD-D2", () => TdD2SameActorNoReentryScenario.RunAsync(context)),
    ("TD-D3", () => TdD3TimerNoReentryScenario.RunAsync(context)),
    ("TD-D4", () => TdD4PerActorAsyncIsolationScenario.RunAsync(context)),
    ("TD-D5", () => TdD5UnsupportedYieldScenario.RunAsync(context)),
    ("TD-D6", () => TdD6SameGateRejectionScenario.RunAsync(context)),
    ("TD-E1", () => TdE1EntryToUserSpotJoinScenario.RunAsync(context)),
    ("TD-E2", () => TdE2UserToUserSpotJoinScenario.RunAsync(context)),
    ("TD-E3", () => TdE3OppositeSpotJoinScenario.RunAsync(context)),
    ("TD-E2A", () => TdE2ADeferredJoinFailureScenario.RunAsync(context)),
    ("TD-F1", () => TdF1RemoteSpotContinuationScenario.RunAsync(context)),
    ("TD-F2", () => TdF2RouteBridgeYieldScenario.RunAsync(context)),
    ("TD-F3", () => TdF3SessionRelayYieldScenario.RunAsync(context)),
    ("TD-F4", () => TdF4RequestTimeoutRecoveryScenario.RunAsync(context)),
    ("TD-F5", () => TdF5CancellationShutdownRecoveryScenario.RunAsync(context)),
    ("TD-F6", () => TdF6SelfRequestTimeoutRecoveryScenario.RunAsync(context)),
    ("TD-G1", () => TdG1TerminatorConformanceScenario.RunAsync(context))
};
var requestedIds = options.Scenario == "full"
    ? scenarios.Select(static scenario => scenario.Id).ToHashSet(StringComparer.Ordinal)
    : options.Scenario.Split(
            ',',
            StringSplitOptions.RemoveEmptyEntries
            | StringSplitOptions.TrimEntries)
        .ToHashSet(StringComparer.Ordinal);
var knownIds = scenarios
    .Select(static scenario => scenario.Id)
    .ToHashSet(StringComparer.Ordinal);
var unknownIds = requestedIds.Except(knownIds, StringComparer.Ordinal).ToArray();
if (unknownIds.Length != 0 || requestedIds.Count == 0)
    throw new ArgumentException(
        $"Unknown scenario selector: {string.Join(",", unknownIds)}");

var executed = 0;
foreach (var scenario in scenarios.Where(item => requestedIds.Contains(item.Id)))
{
    await scenario.Run();
    executed++;
    Console.WriteLine($"{scenario.Id} result=passed");
}

Console.WriteLine(
    $"automatic-turn-dispatch client executed={executed} result=passed");

static ZLinkHttpClient? CreateOptionalHttpClient(string url)
{
    return string.IsNullOrWhiteSpace(url)
        ? null
        : ZLinkHttpClient.Create(url).Timeout(TimeSpan.FromSeconds(15)).Build();
}
