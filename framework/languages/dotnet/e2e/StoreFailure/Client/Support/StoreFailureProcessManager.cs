using System.Diagnostics;
using StoreFailure.Shared;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient;
using Zlink.Framework.E2E.Configuration;

namespace StoreFailure.Client.Support;

internal sealed class StoreFailureProcessManager(ClientOptions options) : IAsyncDisposable
{
    private readonly List<ManagedProcess> _processes = [];

    public async ValueTask DisposeAsync()
    {
        for (var i = _processes.Count - 1; i >= 0; i--) await _processes[i].StopAsync();

        _processes.Clear();
    }

    public async Task<ManagedProcess> StartProviderBAsync()
    {
        var process = StartProvider(
            "api-b",
            "api-b",
            options.ProviderBUrl,
            options.ProviderBEndpoint,
            options.ProviderBEvidenceFile);
        await process.WaitReadyAsync();
        return process;
    }

    public async Task<ManagedProcess> StartProviderCAsync()
    {
        var process = StartProvider(
            "api-c",
            "api-c",
            options.ProviderCUrl,
            options.ProviderCEndpoint,
            Path.Combine(options.LogDir, "api-c.evidence.log"));
        await process.WaitReadyAsync();
        return process;
    }

    /// <summary>
    /// Starts a separate consumer that proves the opaque Store SPI polling
    /// path observes provider additions and removals (SF-A2).
    /// </summary>
    public async Task<ManagedProcess> StartConsumerNwAsync()
    {
        var process = StartProcess(
            "consumer-nw",
            options.ConsumerProject,
            new DynamicConsumerOptions(
                options.ConsumerNwUrl,
                options.RedisEndpoint,
                options.RedisKeyPrefix,
                options.LogDir,
                "consumer-nw",
                "polling",
                options.LocationHeartbeatMs,
                options.LocationLeaseTtlMs,
                options.LocationPollingMs,
                options.LocationGraceMs),
            options.ConsumerNwUrl);
        await process.WaitReadyAsync();
        return process;
    }

    /// <summary>Pauses the store container: outage begins.</summary>
    public Task PauseStoreAsync() => RunDockerAsync("pause", options.RedisContainer);

    public Task UnpauseStoreAsync() => RunDockerAsync("unpause", options.RedisContainer);

    private static async Task RunDockerAsync(string verb, string container)
    {
        var startInfo = new ProcessStartInfo("docker")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };
        startInfo.ArgumentList.Add(verb);
        startInfo.ArgumentList.Add(container);
        using var process = Process.Start(startInfo)
                            ?? throw new InvalidOperationException($"Failed to run docker {verb}.");
        await process.WaitForExitAsync();
        if (process.ExitCode != 0)
        {
            var error = await process.StandardError.ReadToEndAsync();
            throw new InvalidOperationException($"docker {verb} {container} failed: {error}");
        }
    }

    private ManagedProcess StartProvider(
        string name,
        string rid,
        string url,
        string endpoint,
        string evidenceFile)
    {
        return StartProcess(
            name,
            options.ProviderProject,
            new DynamicProviderOptions(
                name, rid, url, options.LogDir, 100,
                options.LocationHeartbeatMs, options.LocationLeaseTtlMs,
                options.LocationPollingMs, options.LocationGraceMs,
                options.RedisEndpoint, options.RedisKeyPrefix, endpoint, evidenceFile),
            url);
    }

    private ManagedProcess StartProcess(
        string name,
        string project,
        object roleOptions,
        string healthUrl)
    {
        var projectDirectory = Path.GetDirectoryName(project)
                               ?? throw new InvalidOperationException(
                                   $"Project path '{project}' has no directory.");
        var application = Path.Combine(
            projectDirectory,
            "bin",
            "Debug",
            "net8.0",
            $"{Path.GetFileNameWithoutExtension(project)}.dll");
        if (!File.Exists(application))
            throw new InvalidOperationException(
                $"Built E2E application was not found: {application}");

        var startInfo = new ProcessStartInfo("setsid")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };
        startInfo.ArgumentList.Add("dotnet");
        startInfo.ArgumentList.Add(application);
        startInfo.ArgumentList.Add("--config");
        startInfo.ArgumentList.Add(E2eConfiguration.Write(options.ConfigDir, name, roleOptions));

        var process = Process.Start(startInfo)
                      ?? throw new InvalidOperationException($"Failed to start {name}.");
        _ = CopyToFileAsync(process.StandardOutput, Path.Combine(options.LogDir, $"{name}.stdout.log"));
        _ = CopyToFileAsync(process.StandardError, Path.Combine(options.LogDir, $"{name}.stderr.log"));
        var managed = new ManagedProcess(process, healthUrl);
        _processes.Add(managed);
        return managed;
    }

    private static async Task CopyToFileAsync(StreamReader reader, string path)
    {
        await using var stream = File.Open(path, FileMode.Create, FileAccess.Write, FileShare.Read);
        await using var writer = new StreamWriter(stream);
        while (await reader.ReadLineAsync() is { } line)
        {
            await writer.WriteLineAsync(line);
            await writer.FlushAsync();
        }
    }
}

internal sealed record DynamicProviderOptions(
    string Role,
    string Rid,
    string HttpUrl,
    string LogDir,
    int Weight,
    int LocationHeartbeatMs,
    int LocationLeaseTtlMs,
    int LocationPollingMs,
    int LocationGraceMs,
    string RedisEndpoint,
    string RedisKeyPrefix,
    string ChannelEndpoint,
    string EvidenceFile);

internal sealed record DynamicConsumerOptions(
    string HttpUrl,
    string RedisEndpoint,
    string RedisKeyPrefix,
    string LogDir,
    string TraceLabel,
    string StoreMode,
    int LocationHeartbeatMs,
    int LocationLeaseTtlMs,
    int LocationPollingMs,
    int LocationGraceMs);

internal sealed class ManagedProcess(Process process, string healthUrl)
{
    private static readonly TimeSpan ReadinessTimeout = TimeSpan.FromSeconds(3);
    private static readonly TimeSpan ReadinessPollInterval = TimeSpan.FromMilliseconds(100);
    private static readonly TimeSpan GracefulExitTimeout = TimeSpan.FromSeconds(35);
    private bool _shutdownRequested;
    private bool _stopped;

    public async Task WaitReadyAsync()
    {
        var elapsed = Stopwatch.StartNew();
        using var http = ZLinkHttpClient.Create(healthUrl)
            .Timeout(TimeSpan.FromMilliseconds(250))
            .Build();
        while (elapsed.Elapsed < ReadinessTimeout)
        {
            if (process.HasExited)
                throw new InvalidOperationException($"Process exited before readiness: {process.ExitCode}.");

            try
            {
                if ((await http.Get("/health").AsyncRaw()).Status == 200) return;
            }
            catch (ZLinkFrameworkException error) when (
                error.Kind is ZLinkFrameworkErrorKind.Unavailable
                    or ZLinkFrameworkErrorKind.DeadlineExceeded)
            {
            }

            await Task.Delay(ReadinessPollInterval);
        }

        throw new TimeoutException($"Process did not become ready: {healthUrl}.");
    }

    /// <summary>
    /// SIGKILL, process tree included: the crash path that leaves rows
    /// behind with no shutdown cleanup (SF-C1, SF-D2).
    /// </summary>
    public async Task KillAsync()
    {
        _stopped = true;
        if (!process.HasExited) process.Kill(true);

        await process.WaitForExitAsync();
    }

    public async Task RequestShutdownAsync()
    {
        if (_shutdownRequested || process.HasExited)
        {
            return;
        }

        using var http = ZLinkHttpClient.Create(healthUrl).Timeout(TimeSpan.FromSeconds(5)).Build();
        await http.Post("/shutdown").AsyncRaw();
        _shutdownRequested = true;
    }

    public async Task<DrainResultRes> RequestDrainAsync()
    {
        if (process.HasExited)
            throw new InvalidOperationException("Cannot drain an exited process.");

        using var http = ZLinkHttpClient.Create(healthUrl).Timeout(GracefulExitTimeout).Build();
        return (await http.Post("/drain").Async<DrainResultRes>()).Body;
    }

    public async Task WaitForGracefulExitAsync()
    {
        if (!_shutdownRequested)
            throw new InvalidOperationException("Graceful shutdown must be requested before waiting for exit.");

        using var exitWait = new CancellationTokenSource(GracefulExitTimeout);
        try
        {
            await process.WaitForExitAsync(exitWait.Token);
        }
        catch (OperationCanceledException) when (exitWait.IsCancellationRequested)
        {
            throw new TimeoutException(
                $"Process did not complete graceful shutdown within {GracefulExitTimeout}.");
        }

        if (process.ExitCode != 0)
            throw new InvalidOperationException($"Gracefully stopped process exited with code {process.ExitCode}.");

        _stopped = true;
    }

    public async Task StopAsync()
    {
        if (_stopped) return;

        _stopped = true;
        if (!process.HasExited)
            try
            {
                if (!_shutdownRequested)
                {
                    await RequestShutdownAsync();
                }
            }
            catch
            {
                if (!process.HasExited) process.Kill(true);
            }

        // The framework's public drain deadline is 30 seconds. The harness
        // must allow that operation to finish instead of turning a normal
        // shutdown into a SIGKILL while owner cleanup is still pending.
        using var exitWait = new CancellationTokenSource(GracefulExitTimeout);
        try
        {
            await process.WaitForExitAsync(exitWait.Token);
        }
        catch (OperationCanceledException)
        {
            if (!process.HasExited) process.Kill(true);
            await process.WaitForExitAsync();
        }
    }
}
