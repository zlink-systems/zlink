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

var root = Path.GetFullPath(Option("--root") ?? Directory.GetCurrentDirectory());
if (args.Contains("--is-running"))
    return ServerHost.IsRunning(root) ? 0 : 1;

var outcome = new RunOutcome();
RunLog? log = null;
Exception? failure = null;
try
{
    await RunAsync();
}
catch (OperationCanceledException)
    when (cancellation.IsCancellationRequested && args.Contains("--serve")) { }
catch (Exception error)
{
    failure = error;
}
if (failure is not null)
    outcome.Failures.Insert(0, failure);
if (outcome.Failures.Count > 0)
{
    var error =
        outcome.Failures.Count == 1
            ? outcome.Failures[0]
            : new AggregateException(outcome.Failures[0].Message, outcome.Failures);
    if (log is null)
        Console.Error.WriteLine(error);
    else
        log.Fail(error);
}
log?.Complete();
if (outcome.Failures.Count == 0 && log is not null && !args.Contains("--serve"))
    Console.WriteLine("fps-http-lobby=completed");
return outcome.Failures.Count == 0 ? 0 : 1;

async Task RunAsync()
{
    if (args.Contains("--check-structure"))
    {
        StructureCheck.Run(root, Option("--structure"));
        return;
    }
    if (args.Contains("--serve"))
    {
        log = new RunLog(root, []);
        StructureCheck.Run(root, null);
        var server = new MatchRun(root, outcome, log.DirectoryPath, foreground: true);
        await ServerHost.RunAsync(
            root,
            async ct =>
            {
                await server.StartAsync(ct);
                return server.PublishedEndpoints;
            },
            server.WaitForExitAsync,
            server.DisposeAsync,
            TimeSpan.FromMinutes(3),
            cancellation.Token
        );
        return;
    }
    if (args.Contains("--check-domain"))
    {
        DomainChecks.Run();
        await WireChecks.RunAsync();
        await LobbyDomainChecks.RunAsync();
        return;
    }
    var attach = args.Contains("--attach");
    var selected = Scenarios.Select(Option("--scenario"));
    if (attach)
        selected = RunLog.ForAttached(
            selected,
            scenario =>
                scenario switch
                {
                    "update" => "node replacement",
                    "lobby" => "API node restart",
                    _ => null,
                }
        );
    log = new RunLog(root, selected);
    if (selected.Length == 0)
    {
        Console.WriteLine("Scenarios: 0/0 passed");
        return;
    }
    DomainChecks.Run();
    await WireChecks.RunAsync();
    await LobbyDomainChecks.RunAsync();
    using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation.Token);
    deadline.CancelAfter(TimeSpan.FromMinutes(3));
    var ct = deadline.Token;
    StructureCheck.Run(root, null);
    var endpoints = attach ? ServerHost.Read(root) : null;
    foreach (var scenario in selected)
    {
        log.Start(scenario);
        await using (
            var active = new MatchRun(
                root,
                outcome,
                Path.Combine(log.DirectoryPath, scenario),
                roomCapacity: scenario == "lobby"
                    ? MatchRun.LobbyRoomCapacity
                    : MatchRun.DefaultRoomCapacity
            )
        )
        {
            if (endpoints is null)
                await active.StartAsync(ct);
            else
                active.Attach(endpoints);
            await Scenarios.PlayAsync(active.Endpoint, scenario, active, ct);
        }
        if (outcome.Failures.Count > 0)
            return;
        log.Pass(scenario);
    }
}
