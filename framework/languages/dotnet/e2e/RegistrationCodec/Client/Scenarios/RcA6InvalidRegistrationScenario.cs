// Verifies RC-A6 Invalid Registration behavior.
using System.Diagnostics;
using RegistrationCodec.Client.Support;
using Zlink.Framework.E2E.Configuration;

namespace RegistrationCodec.Client.Scenarios;

// RC-A6: verifies duplicate registration fails during server startup.
internal static class RcA6InvalidRegistrationScenario
{
    public static async Task RunAsync(ClientOptions options)
    {
        var invalidHttpPort = ProcessSupport.PickPort();
        var invalidChannelPort = ProcessSupport.PickPort();
        var stdout = Path.Combine(options.LogDir, "invalid-duplicate.stdout.log");
        var stderr = Path.Combine(options.LogDir, "invalid-duplicate.stderr.log");
        using var process = new Process();
        process.StartInfo = new ProcessStartInfo("dotnet")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };
        process.StartInfo.ArgumentList.Add("run");
        process.StartInfo.ArgumentList.Add("--no-build");
        process.StartInfo.ArgumentList.Add("--project");
        process.StartInfo.ArgumentList.Add(options.InvalidServerProject);
        process.StartInfo.ArgumentList.Add("--");
        process.StartInfo.ArgumentList.Add("--config");
        process.StartInfo.ArgumentList.Add(E2eConfiguration.Write(
            options.ConfigDir,
            "invalid-duplicate",
            new InvalidRegistrationOptions(
                "invalid-duplicate",
                $"http://127.0.0.1:{invalidHttpPort}",
                options.LogDir,
                "all",
                $"tcp://127.0.0.1:{invalidChannelPort}")));
        process.Start();

        var stdoutTask = process.StandardOutput.ReadToEndAsync();
        var stderrTask = process.StandardError.ReadToEndAsync();
        var completed = true;
        try
        {
            await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(15));
        }
        catch (TimeoutException)
        {
            completed = false;
            process.Kill(entireProcessTree: true);
            await process.WaitForExitAsync();
        }

        File.WriteAllText(stdout, await stdoutTask);
        var errorText = await stderrTask;
        File.WriteAllText(stderr, errorText);

        // A duplicate handler should stop the server before it becomes usable.
        ZlinkStreamAssert.Ensure(completed, "RC-A6 invalid registration server did not exit.");
        ZlinkStreamAssert.Ensure(process.ExitCode != 0, "RC-A6 invalid registration server unexpectedly started.");
        ZlinkStreamAssert.Ensure(
            errorText.Contains("duplicate", StringComparison.OrdinalIgnoreCase)
            || errorText.Contains("EchoManual", StringComparison.Ordinal),
            "RC-A6 invalid registration error did not mention duplicate registration.");

        Console.WriteLine("scenario RC-A6 passed");
    }
}

internal sealed record InvalidRegistrationOptions(
    string Rid,
    string HttpUrl,
    string LogDir,
    string CodecMode,
    string ChannelEndpoint);
