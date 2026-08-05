using Zlink.Framework.E2E.Configuration;
using ToActorMessaging.Client.Scenarios;
using ToActorMessaging.Client.Support;

var options = ClientOptions.Parse(args);
using var context = new ToActorScenarioContext(options);

var scenarios = new Dictionary<string, Func<Task>>(StringComparer.OrdinalIgnoreCase)
{
    ["TA-A1"] = () => TaA1BoundActorMessagingScenario.RunAsync(context),
    ["TA-A2"] = () => TaA2UnboundActorMessagingScenario.RunAsync(context),
    ["TA-A3"] = () => TaA3LateBindScenario.RunAsync(context),
    ["TA-A4"] = () => TaA4DisconnectAndDestroyScenario.RunAsync(context),
    ["TA-B1"] = () => TaB1MissingActorScenario.RunAsync(context),
    ["TA-B2"] = () => TaB2StaleActorReferenceScenario.RunAsync(context),
    ["TA-B3"] = () => TaB3RouteReconnectScenario.RunAsync(context)
};

IEnumerable<string> selected = string.Equals(options.Scenario, "all", StringComparison.OrdinalIgnoreCase)
    ? scenarios.Keys
    : options.Scenario.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
foreach (var name in selected)
{
    if (!scenarios.TryGetValue(name, out var scenario))
        throw new ArgumentException($"Unknown scenario '{name}'.");
    await scenario();
}

Console.WriteLine("to-actor-messaging e2e result=passed");

internal sealed record ClientOptions(
    string ActorUrl,
    string ActorBUrl,
    string CallerUrl,
    string NoRouteCallerUrl,
    string SessionAStreamEndpoint,
    string SessionBStreamEndpoint,
    string SessionAUrl,
    string SessionBUrl,
    string Scenario)
{
    public static ClientOptions Parse(string[] args)
        => E2eConfiguration.Load<ClientOptions>(args);
}
