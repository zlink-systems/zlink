using System.Diagnostics;

namespace Zlink.Framework.UnitTests;

internal static class IsolatedTestProcess
{
    private const string EnvironmentVariable = "ZLINK_TEST_ISOLATED_PROCESS";

    internal static async Task<bool> RunIsolatedAsync(Type testClass, string methodName)
    {
        var testName = $"{testClass.FullName}.{methodName}";
        if (Environment.GetEnvironmentVariable(EnvironmentVariable) == testName)
            return false;

        // The injected poller must own a fresh process pool. Other tests
        // may still retain sources in the current process generation.
        var start = new ProcessStartInfo("dotnet")
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        start.ArgumentList.Add("vstest");
        start.ArgumentList.Add(testClass.Assembly.Location);
        start.ArgumentList.Add($"--TestCaseFilter:FullyQualifiedName={testName}");
        start.ArgumentList.Add("--");
        start.ArgumentList.Add("RunConfiguration.TreatNoTestsAsError=true");
        start.Environment[EnvironmentVariable] = testName;
        using var process = Process.Start(start)!;
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        Assert.True(process.ExitCode == 0, $"{await output}\n{await error}");
        return true;
    }
}
