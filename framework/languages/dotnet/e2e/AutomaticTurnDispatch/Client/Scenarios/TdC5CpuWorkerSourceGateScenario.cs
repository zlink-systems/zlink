// Verifies TD-C5 Cpu Worker Source Gate behavior.
namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdC5CpuWorkerSourceGateScenario
{
    public static Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var root = Path.Combine(FindRoot(), "Server", "Play");
        foreach (var file in Directory.EnumerateFiles(root, "*.cs", SearchOption.AllDirectories))
        {
            var source = File.ReadAllText(file);
            if (!source.Contains("RunCpuWorker", StringComparison.Ordinal)) continue;
            ZlinkStreamAssert.Ensure(!source.Contains("GetAwaiter().GetResult()", StringComparison.Ordinal),
                $"TD-C5 found blocking I/O in CPU worker source: {file}");
        }
        return Task.CompletedTask;
    }

    private static string FindRoot()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            var candidate = Path.Combine(current.FullName, "e2e", "AutomaticTurnDispatch");
            if (Directory.Exists(candidate)) return candidate;
            current = current.Parent;
        }
        throw new DirectoryNotFoundException("AutomaticTurnDispatch source root was not found.");
    }
}
