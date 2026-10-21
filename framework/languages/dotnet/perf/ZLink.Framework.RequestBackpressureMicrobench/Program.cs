using System.Diagnostics;
using System.Reflection;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text.Json;

if (args.Length is < 2 or > 3)
    throw new ArgumentException("baseline candidate [registration|blocked|admitted] required");
var mode = args.Length == 3 ? args[2] : "registration";
var before = new Snapshot(args[0]);
var after = new Snapshot(args[1]);
Console.WriteLine(
    JsonSerializer.Serialize(
        new
        {
            runtime = Environment.Version.ToString(),
            frameworkBefore = before.FrameworkHash,
            frameworkAfter = after.FrameworkHash,
            harnessBefore = before.HarnessHash,
            harnessAfter = after.HarnessHash,
            before.OptimizerDisabled,
            afterOptimizerDisabled = after.OptimizerDisabled,
            before.FrameworkOptimizerDisabled,
            afterFrameworkOptimizerDisabled = after.FrameworkOptimizerDisabled,
            tieredCompilation = Environment.GetEnvironmentVariable("DOTNET_TieredCompilation"),
            Stopwatch.Frequency,
        }
    )
);
if (before.HarnessHash != after.HarnessHash)
    throw new InvalidOperationException("Harness DLLs differ");
foreach (var snapshot in new[] { before, after })
{
    var instance = Activator.CreateInstance(snapshot.TestType)!;
    foreach (var alreadyCanceled in new[] { false, true })
        await (Task)
            snapshot
                .TestType.GetMethod("Cancellation_preserves_token_and_discards_late_result")!
                .Invoke(instance, [alreadyCanceled])!;
    await (Task)
        snapshot.TestType.GetMethod("None_token_completes_normally")!.Invoke(instance, null)!;
    await (Task)
        snapshot.TestType.GetMethod("Disposal_unregisters_cancellation")!.Invoke(instance, null)!;
}
if (mode == "admitted")
{
    foreach (var snapshot in new[] { before, after })
        for (var warmup = 0; warmup < 8; warmup++)
            await snapshot.RunAdmitted(1024);
    for (var pair = 0; pair < 16; pair++)
    {
        var sequence = pair % 2 == 0 ? new[] { before, after } : new[] { after, before };
        foreach (var snapshot in sequence)
        {
            var measured = await snapshot.RunAdmitted(4096);
            Console.WriteLine(
                JsonSerializer.Serialize(
                    new
                    {
                        mode,
                        pair,
                        version = ReferenceEquals(snapshot, before) ? "before" : "after",
                        nsPerSubmission = measured[0],
                        producerBytesPerSubmission = measured[1],
                        wholeFixtureCpuMs = measured[2],
                        gen0 = measured[3],
                        gen1 = measured[4],
                        gen2 = measured[5],
                    }
                )
            );
        }
    }
    return;
}
if (mode == "blocked")
{
    foreach (var snapshot in new[] { before, after })
        for (var warmup = 0; warmup < 4; warmup++)
            await snapshot.RunBlocked(8);
    for (var pair = 0; pair < 8; pair++)
    {
        var sequence = pair % 2 == 0 ? new[] { before, after } : new[] { after, before };
        foreach (var snapshot in sequence)
        {
            var measured = await snapshot.RunBlocked(16);
            Console.WriteLine(
                JsonSerializer.Serialize(
                    new
                    {
                        mode,
                        pair,
                        version = ReferenceEquals(snapshot, before) ? "before" : "after",
                        nsPerSubmission = measured[0],
                        frameworkProducerBytesPerSubmission = measured[1],
                        wholeFixtureCpuMs = measured[2],
                        gen0 = measured[3],
                        gen1 = measured[4],
                        gen2 = measured[5],
                    }
                )
            );
        }
    }
    return;
}
if (mode != "registration")
    throw new ArgumentException("Unknown mode");
foreach (var live in new[] { false, true })
{
    for (var warmup = 0; warmup < 8; warmup++)
    {
        before.Run(10000, live);
        after.Run(10000, live);
    }
    for (var pair = 0; pair < 16; pair++)
    {
        var sequence = pair % 2 == 0 ? new[] { before, after } : new[] { after, before };
        foreach (var snapshot in sequence)
        {
            var measured = snapshot.Run(100000, live);
            Console.WriteLine(
                JsonSerializer.Serialize(
                    new
                    {
                        live,
                        pair,
                        version = ReferenceEquals(snapshot, before) ? "before" : "after",
                        nsPerOp = measured[0],
                        bytesPerOp = measured[1],
                        cpuMs = measured[2],
                        gen0 = measured[3],
                        gen1 = measured[4],
                        gen2 = measured[5],
                    }
                )
            );
        }
    }
}

sealed class Snapshot : AssemblyLoadContext
{
    private readonly string directory;
    private readonly MethodInfo entry;
    private readonly MethodInfo blockedEntry;
    private readonly MethodInfo? admittedEntry;
    internal readonly Type TestType;
    internal string FrameworkHash { get; }
    internal string HarnessHash { get; }
    internal bool OptimizerDisabled { get; }
    internal bool FrameworkOptimizerDisabled { get; }

    internal Snapshot(string path)
        : base(isCollectible: false)
    {
        directory = Path.GetFullPath(path);
        var harnessPath = Path.Combine(directory, "Zlink.Framework.UnitTests.dll");
        FrameworkHash = Convert.ToHexString(
            SHA256.HashData(File.ReadAllBytes(Path.Combine(directory, "Zlink.Framework.dll")))
        );
        HarnessHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(harnessPath)));
        var framework = LoadFromAssemblyPath(Path.Combine(directory, "Zlink.Framework.dll"));
        FrameworkOptimizerDisabled =
            framework.GetCustomAttribute<DebuggableAttribute>()?.IsJITOptimizerDisabled ?? false;
        var harness = LoadFromAssemblyPath(harnessPath);
        OptimizerDisabled =
            harness.GetCustomAttribute<DebuggableAttribute>()?.IsJITOptimizerDisabled ?? false;
        TestType = harness.GetType(
            "Zlink.Framework.UnitTests.Runtime.RequestCompletionCancellationTests",
            true
        )!;
        entry = TestType.GetMethod("MeasureRegistration")!;
        admittedEntry = harness
            .GetType("Zlink.Framework.UnitTests.Runtime.RequestAdmissionObservationTests")
            ?.GetMethod("MeasureAcceptedAdmission");
        blockedEntry = harness
            .GetType("Zlink.Framework.UnitTests.Runtime.SubmitAdmissionRuntimeTests", true)!
            .GetMethod("MeasureBackpressuredSubmission")!;
    }

    protected override Assembly? Load(AssemblyName name)
    {
        var file = Path.Combine(directory, name.Name + ".dll");
        return File.Exists(file) ? LoadFromAssemblyPath(file) : null;
    }

    internal Task<double[]> RunAdmitted(int count) =>
        (Task<double[]>)
            (
                admittedEntry
                ?? throw new InvalidOperationException("Admission benchmark entry missing")
            ).Invoke(null, [count])!;

    internal Task<double[]> RunBlocked(int count) =>
        (Task<double[]>)blockedEntry.Invoke(null, [count])!;

    internal double[] Run(int count, bool live) => (double[])entry.Invoke(null, [count, live])!;
}
