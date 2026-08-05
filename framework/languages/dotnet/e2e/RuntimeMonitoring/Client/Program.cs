using RuntimeMonitoring.Client.Scenarios;
using RuntimeMonitoring.Client.Support;

var options = ClientOptions.Parse(args);

var scenarios = new (string Name, Func<Task> Run)[]
{
    ("MON-A1", () => MonA1SocketEventsScenario.RunAsync(options)),
    ("MON-A2", () => MonA2RegistryEventsScenario.RunAsync(options)),
    ("MON-A3", () => MonA3SpotEventsScenario.RunAsync(options)),
    ("MON-A4A", () => MonA4AvailabilityTransitionScenario.RunPlannedAsync(options)),
    ("MON-A4B", () => MonA4AvailabilityTransitionScenario.RunCrashAsync(options)),
    ("MON-A5", () => MonA5FixedKindsScenario.RunAsync(options)),
    ("MON-B1", () => MonBPublishMonitoringAbsenceScenario.RunZeroTargetAsync(options)),
    ("MON-B2", () => MonBPublishMonitoringAbsenceScenario.RunLocalTargetAsync(options)),
    ("MON-C1", () => MonC1DispatchFailureScenario.RunAsync(options)),
    ("MON-D1A", () => MonD1FailureRecoveryScenario.RunValidationAsync(options)),
    ("MON-D1B", () => MonD1FailureRecoveryScenario.RunCrashRecoveryAsync(options))
};

foreach (var name in SelectedScenarioNames(options.Scenario, scenarios.Select(scenario => scenario.Name)))
{
    var selected = scenarios.FirstOrDefault(scenario =>
        string.Equals(scenario.Name, name, StringComparison.OrdinalIgnoreCase));
    if (selected.Run is null) throw new ArgumentException($"Unknown scenario '{name}'.");

    await selected.Run();
}

Console.WriteLine("runtime-monitoring client result=passed");

static IEnumerable<string> SelectedScenarioNames(string selector, IEnumerable<string> allNames)
{
    if (string.Equals(selector, "all", StringComparison.OrdinalIgnoreCase))
        return allNames;

    return selector
        .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
}
