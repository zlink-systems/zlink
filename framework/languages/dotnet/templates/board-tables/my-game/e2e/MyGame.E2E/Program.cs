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

var outcome = new RunOutcome();
Exception? scenarioFailure = null;
var result = 0;
try
{
    result = await RunAsync();
}
catch (OperationCanceledException)
    when (args.Contains("--serve") && cancellation.IsCancellationRequested)
{
}
catch (Exception error)
{
    scenarioFailure = error;
}
var failures = outcome.Failures;
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
    StructureCheck.Run(root, null);
    using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation.Token);
    deadline.CancelAfter(TimeSpan.FromMinutes(3));
    if (args.Contains("--serve"))
    {
        await using var server = new TableRun(root, 0, outcome);
        await server.StartAsync(deadline.Token);
        Console.WriteLine($"Server ready: {server.Endpoint}");
        await server.WaitForExitAsync(cancellation.Token);
        return 0;
    }

    var endpoint = Option("--attach");
    var selected = Scenarios.Select(
        Option("--scenario") ?? (endpoint is null ? null : "table,reconnect")
    );
    if (endpoint is not null)
    {
        if (selected.Contains("update"))
            throw new ArgumentException("The update scenario requires an isolated run.");
        await Scenarios.RunAsync(endpoint, null, outcome, selected, deadline.Token);
    }
    else
    {
        await DomainChecks.RunAsync();
        await using var run = new TableRun(root, selected.Length, outcome);
        await run.StartAsync(deadline.Token);
        await Scenarios.RunAsync(run.Endpoint, run, outcome, selected, deadline.Token);
    }
    Console.WriteLine("board-tables=completed");
    return 0;
}
