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

try
{
    var root = Path.GetFullPath(Option("--root") ?? Directory.GetCurrentDirectory());
    if (args.Contains("--check-structure"))
    {
        StructureCheck.Run(root, Option("--structure"));
        return 0;
    }
    var mode = Option("--mode") ?? "run";
    if (mode == "client")
    {
        var endpoint =
            Option("--endpoint")
            ?? throw new ArgumentException("--mode client requires --endpoint.");
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation.Token);
        deadline.CancelAfter(TimeSpan.FromMinutes(2));
        await Scenarios.RunAsync(
            endpoint,
            null,
            Scenarios.Select(Option("--scenario"), false),
            deadline.Token
        );
    }
    else if (mode == "run")
    {
        var selected = Scenarios.Select(Option("--scenario"), true);
        await using var run = new WorldRun(root);
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation.Token);
        deadline.CancelAfter(TimeSpan.FromMinutes(3));
        try
        {
            StructureCheck.Run(root, null);
            await run.StartAsync(deadline.Token);
            await Scenarios.RunAsync(run.Endpoint, run, selected, deadline.Token);
        }
        catch (Exception failure)
        {
            Console.Error.WriteLine("Startup or scenario failure: " + failure);
            throw;
        }
    }
    else
        throw new ArgumentException($"Unknown mode: {mode}");
    Console.WriteLine("mmo-seamless=completed");
    return 0;
}
catch (Exception error)
{
    Console.Error.WriteLine(error);
    return 1;
}
