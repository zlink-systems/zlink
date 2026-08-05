using System.Diagnostics;
using Zlink.Framework.E2E.Configuration;

namespace PubSub.Client.Support;

internal sealed class ServerProcessLauncher(ClientOptions options)
{
    public string PublisherEndpoint => options.PublisherEndpoint;

    public Process StartSubscriber(
        string name,
        string httpUrl,
        string evidenceFile,
        string? publisherEndpoint = null)
    {
        var startInfo = CreateServerStartInfo(options.SubscriberProject, name,
            new DynamicSubscriberOptions(
                name, httpUrl, options.LogDir, publisherEndpoint ?? options.PublisherEndpoint,
                0, Path.Combine(options.LogDir, evidenceFile)));

        return Start(name, startInfo);
    }

    public Process StartPublisher()
    {
        var startInfo = CreateServerStartInfo(options.PublisherProject, "pub-restart",
            new DynamicPublisherOptions(
                "pub-a", options.PublisherUrl, options.LogDir, options.PublisherEndpoint,
                Path.Combine(options.LogDir, "pub-restart.evidence.log")));

        return Start("pub-restart", startInfo);
    }

    private ProcessStartInfo CreateServerStartInfo(
        string project,
        string name,
        object roleOptions)
    {
        var startInfo = new ProcessStartInfo("dotnet")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };
        startInfo.ArgumentList.Add("run");
        startInfo.ArgumentList.Add("--no-build");
        startInfo.ArgumentList.Add("--project");
        startInfo.ArgumentList.Add(project);
        startInfo.ArgumentList.Add("--");
        startInfo.ArgumentList.Add("--config");
        startInfo.ArgumentList.Add(E2eConfiguration.Write(options.ConfigDir, name, roleOptions));
        return startInfo;
    }

    private Process Start(string name, ProcessStartInfo startInfo)
    {
        var stdout = Path.Combine(options.LogDir, $"{name}.stdout.log");
        var stderr = Path.Combine(options.LogDir, $"{name}.stderr.log");
        var process = Process.Start(startInfo)
                      ?? throw new InvalidOperationException($"Failed to start {name}.");
        _ = Task.Run(async () => await File.WriteAllTextAsync(stdout, await process.StandardOutput.ReadToEndAsync()));
        _ = Task.Run(async () => await File.WriteAllTextAsync(stderr, await process.StandardError.ReadToEndAsync()));
        return process;
    }
}

internal sealed record DynamicSubscriberOptions(
    string Rid, string HttpUrl, string LogDir, string PublisherEndpoint,
    int HandlerDelayMs, string EvidenceFile);

internal sealed record DynamicPublisherOptions(
    string Rid, string HttpUrl, string LogDir, string PublisherEndpoint,
    string EvidenceFile);
