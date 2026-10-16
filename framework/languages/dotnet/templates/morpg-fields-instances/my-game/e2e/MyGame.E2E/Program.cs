using System.Runtime.InteropServices;
using MyGame.E2E;

using var cancellation = new CancellationTokenSource();
Console.CancelKeyPress += (_, signal) =>
{
    signal.Cancel = true;
    cancellation.Cancel();
};
using var termination = OperatingSystem.IsWindows()
    ? null
    : PosixSignalRegistration.Create(
        PosixSignal.SIGTERM,
        signal =>
        {
            signal.Cancel = true;
            cancellation.Cancel();
        }
    );

string? Option(string name)
{
    var index = Array.IndexOf(args, name);
    if (index < 0)
        return null;
    if (index + 1 >= args.Length)
        throw new ArgumentException($"{name} requires a value.");
    return args[index + 1];
}

WorldRun? run = null;
Exception? scenarioFailure = null;
var result = 0;
try
{
    result = await RunAsync();
}
catch (Exception error)
{
    scenarioFailure = error;
}
var failures = run?.Outcome.Failures ?? [];
if (scenarioFailure is not null)
    failures.Insert(0, scenarioFailure);
if (failures.Count > 0)
{
    Console.Error.WriteLine(
        new AggregateException("Run failed (scenario first, then cleanup).", failures)
    );
    return 1;
}
return result;

async Task<int> RunAsync()
{
    var root = Path.GetFullPath(Option("--root") ?? Directory.GetCurrentDirectory());
    if (args.Contains("--check-structure"))
    {
        StructureCheck.Run(root, Option("--structure"));
        return 0;
    }
    var selected = Scenarios.Select(Option("--scenario"));
    run = new WorldRun(root);
    await using var ownedRun = run;
    using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation.Token);
    deadline.CancelAfter(TimeSpan.FromMinutes(3));
    try
    {
        StructureCheck.Run(root, null);
        await run.StartAsync(deadline.Token);
    }
    catch
    {
        Console.WriteLine($"✗ {selected[0]} (startup)");
        Console.WriteLine($"Scenarios: 0/{selected.Length} passed");
        throw;
    }
    await Scenarios.RunAsync(run.Endpoint, run, selected, deadline.Token);
    Console.WriteLine("morpg-fields-instances=completed");
    return 0;
}
