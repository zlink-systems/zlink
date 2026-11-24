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
TableRun? run = null;
Exception? scenarioFailure = null;
var result = 0;
try
{
    result = await RunAsync();
}
catch (OperationCanceledException)
    when (cancellation.IsCancellationRequested && args.Contains("--serve")) { }
catch (Exception error)
{
    scenarioFailure = error;
}
var failures = (run?.Outcome ?? outcome).Failures;
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
    if (args.Contains("--is-running"))
        return ServerHost.IsRunning(root) ? 0 : 1;
    if (args.Contains("--check-structure"))
    {
        StructureCheck.Run(root, Option("--structure"));
        return 0;
    }
    StructureCheck.Run(root, null);
    if (args.Contains("--serve"))
    {
        run = new TableRun(root, 0, foreground: true);
        await ServerHost.RunAsync(root, run, cancellation.Token);
        return 0;
    }
    var requested = Option("--scenario");
    var selected = Scenarios.Select(requested);
    var attach = args.Contains("--attach");
    if (attach && requested is null)
    {
        selected = selected.Where(scenario => scenario != "update").ToArray();
        Console.WriteLine(
            "Running against the started server. Update runs with an isolated server."
        );
    }
    if (attach && selected.Contains("update"))
        throw new InvalidOperationException(
            "The update scenario requires a fresh server with capacity set for its test run."
        );
    await DomainChecks.RunAsync();
    using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation.Token);
    deadline.CancelAfter(TimeSpan.FromMinutes(3));
    if (attach)
    {
        var endpoint = ServerHost.Endpoint(root);
        Console.WriteLine($"Attached to Session STREAM: {endpoint}");
        await Scenarios.RunAsync(endpoint, null, outcome, selected, deadline.Token);
    }
    else
    {
        run = new TableRun(root, selected.Length);
        await using var ownedRun = run;
        await run.StartAsync(deadline.Token);
        await Scenarios.RunAsync(run.Endpoint, run, run.Outcome, selected, deadline.Token);
    }
    if ((run?.Outcome ?? outcome).Failures.Count == 0)
        Console.WriteLine("board-tables=completed");
    return 0;
}
