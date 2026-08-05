using System.Diagnostics;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient;
using Zlink.Framework.E2E.Configuration;

namespace ResilienceLifecycle.Client.Support;

internal sealed class ResilienceProcessManager(ClientOptions options) : IAsyncDisposable
{
    private readonly List<ManagedProcess> _processes = [];
    private ManagedProcess? _providerB;
    private bool _initialProviderBActive = true;

    public async ValueTask DisposeAsync()
    {
        for (var i = _processes.Count - 1; i >= 0; i--) await _processes[i].StopAsync();

        _processes.Clear();
    }

    public async Task<ProviderStartResult> StartProviderAAsync()
    {
        var process = StartProvider(
            "api-a-restart",
            "api-a",
            options.ProviderAUrl,
            options.ProviderAEndpoint,
            options.ProviderAEvidenceFile);
        await process.WaitReadyAsync();
        return new ProviderStartResult("api-a", "started", options.ProviderAUrl, options.ProviderAEndpoint);
    }

    public async Task<ProviderStartResult> StartProviderBAsync()
    {
        var process = StartProvider(
            "api-b-restart",
            "api-b",
            options.ProviderBUrl,
            options.ProviderBEndpoint,
            options.ProviderBEvidenceFile);
        await process.WaitReadyAsync();
        _providerB = process;
        return new ProviderStartResult("api-b", "started", options.ProviderBUrl, options.ProviderBEndpoint);
    }

    public async Task<bool> TryStopProviderBAsync()
    {
        if (_providerB is not { } process) return false;

        _providerB = null;
        await process.StopAsync();
        _processes.Remove(process);
        return true;
    }

    public async Task StopProviderBWithSigtermAsync()
    {
        if (_providerB is { } managed)
        {
            _providerB = null;
            await managed.SendSigtermAsync();
            _processes.Remove(managed);
            return;
        }

        if (!_initialProviderBActive) return;
        _initialProviderBActive = false;
        await SignalAndWaitAsync(options.ProviderBProcessId, "TERM");
    }

    public async Task KillProviderBAsync()
    {
        if (_providerB is { } managed)
        {
            _providerB = null;
            await managed.KillAsync();
            _processes.Remove(managed);
            return;
        }

        if (!_initialProviderBActive) return;
        _initialProviderBActive = false;
        await SignalAndWaitAsync(options.ProviderBProcessId, "KILL");
    }

    public async Task WaitProviderBExitedAsync()
    {
        if (_providerB is not { } process) return;

        _providerB = null;
        await process.WaitExitedAsync(TimeSpan.FromSeconds(10));
        _processes.Remove(process);
    }

    public async Task WaitInitialProviderBExitedAsync()
    {
        await WaitProcessExitedAsync(options.ProviderBProcessId);
    }

    public async Task WaitInitialProviderAExitedAsync()
    {
        await WaitProcessExitedAsync(options.ProviderAProcessId);
    }

    private static async Task WaitProcessExitedAsync(int processId)
    {
        Process process;
        try
        {
            process = Process.GetProcessById(processId);
        }
        catch (ArgumentException)
        {
            return;
        }

        using (process)
        {
            await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(10));
        }
    }

    private static async Task SignalAndWaitAsync(int processId, string signal)
    {
        Process target;
        try
        {
            target = Process.GetProcessById(processId);
        }
        catch (ArgumentException)
        {
            return;
        }

        using (target)
        using (var signalProcess = Process.Start(new ProcessStartInfo("kill")
               {
                   ArgumentList = { $"-{signal}", "--", $"-{processId}" },
                   UseShellExecute = false
               }) ?? throw new InvalidOperationException($"Failed to send SIG{signal} to provider B."))
        {
            await signalProcess.WaitForExitAsync();
            if (signalProcess.ExitCode != 0)
                throw new InvalidOperationException($"Sending SIG{signal} to provider B failed.");

            // Process.GetProcessById returns an observer for a process started by
            // the shell runner. It can wait for termination, but it cannot expose
            // the exit code as a Process instance owned by this client. The signal
            // command and the termination observation are the reliable evidence
            // for this externally owned process.
            await target.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(15));
        }
    }

    public async Task<ProviderStartResult> StartProviderBRemapAsync()
    {
        var process = StartProvider(
            "api-b-rescheduled",
            "api-b",
            options.ProviderBRemapUrl,
            options.ProviderBRemapEndpoint,
            Path.Combine(options.LogDir, "api-b-rescheduled.evidence.log"));
        await process.WaitReadyAsync();
        return new ProviderStartResult("api-b", "started", options.ProviderBRemapUrl, options.ProviderBRemapEndpoint);
    }

    public async Task<ProviderStartResult> StartProviderBGreenAsync()
    {
        var process = StartProvider(
            "api-b-green",
            "api-green",
            options.ProviderBGreenUrl,
            options.ProviderBGreenEndpoint,
            Path.Combine(options.LogDir, "api-b-green.evidence.log"));
        await process.WaitReadyAsync();
        return new ProviderStartResult("api-b", "started", options.ProviderBGreenUrl, options.ProviderBGreenEndpoint);
    }

    /// <summary>
    /// Pauses the run's Redis container: the location store becomes
    /// unreachable while every established connection keeps working
    /// (fail-static, RL-C4).
    /// </summary>
    public async Task PauseStoreAsync()
    {
        await RunDockerAsync("pause", options.RedisContainer);
    }

    public async Task UnpauseStoreAsync()
    {
        await RunDockerAsync("unpause", options.RedisContainer);
    }

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
                "provider", rid, url, options.LogDir, 100,
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

        var startInfo = new ProcessStartInfo("dotnet")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };
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

    private static async Task<bool> IsHealthyAsync(string url)
    {
        try
        {
            using var http = ZLinkHttpClient.Create(url).Timeout(TimeSpan.FromSeconds(2)).Build();
            return (await http.Get("/health").AsyncRaw()).Status == 200;
        }
        catch
        {
            return false;
        }
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
    string RedisEndpoint,
    string RedisKeyPrefix,
    string ChannelEndpoint,
    string EvidenceFile);

internal sealed record ProviderStartResult(string Rid, string Status, string Url, string Endpoint);

internal sealed class ManagedProcess(Process process, string healthUrl)
{
    private static readonly TimeSpan ReadinessTimeout = TimeSpan.FromSeconds(3);
    private static readonly TimeSpan ShutdownTimeout = TimeSpan.FromSeconds(45);
    private static readonly TimeSpan ReadinessPollInterval = TimeSpan.FromMilliseconds(100);
    private bool _disposed;

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

    public async Task StopAsync()
    {
        if (_disposed) return;

        _disposed = true;
        try
        {
            if (!process.HasExited)
            {
                try
                {
                    using var http = ZLinkHttpClient.Create(healthUrl).Timeout(TimeSpan.FromSeconds(5)).Build();
                    await http.Post("/shutdown").AsyncRaw();
                }
                catch
                {
                    if (!process.HasExited) process.Kill(true);
                }
            }

            try
            {
                await process.WaitForExitAsync().WaitAsync(ShutdownTimeout);
            }
            catch (TimeoutException)
            {
                if (!process.HasExited) process.Kill(true);
                await process.WaitForExitAsync();
            }

            EnsureSuccessfulExit();
        }
        finally
        {
            process.Dispose();
        }
    }

    public async Task WaitExitedAsync(TimeSpan timeout)
    {
        if (_disposed) return;

        _disposed = true;
        try
        {
            try
            {
                await process.WaitForExitAsync().WaitAsync(timeout);
            }
            catch (TimeoutException)
            {
                if (!process.HasExited) process.Kill(true);
                await process.WaitForExitAsync();
            }

            EnsureSuccessfulExit();
        }
        finally
        {
            process.Dispose();
        }
    }

    public async Task SendSigtermAsync()
    {
        if (_disposed) return;
        _disposed = true;
        try
        {
            using var signalProcess = Process.Start(new ProcessStartInfo("kill")
            {
                ArgumentList = { "-TERM", process.Id.ToString() },
                UseShellExecute = false
            }) ?? throw new InvalidOperationException("Failed to send SIGTERM to provider process.");
            await signalProcess.WaitForExitAsync();
            if (signalProcess.ExitCode != 0)
                throw new InvalidOperationException("Sending SIGTERM to provider process failed.");
            await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(15));
            EnsureSuccessfulExit();
        }
        finally
        {
            process.Dispose();
        }
    }

    public async Task KillAsync()
    {
        if (_disposed) return;
        _disposed = true;
        if (!process.HasExited) process.Kill(true);
        await process.WaitForExitAsync();
        process.Dispose();
    }

    private void EnsureSuccessfulExit()
    {
        if (process.ExitCode != 0)
            throw new InvalidOperationException(
                $"Provider process '{healthUrl}' exited with code {process.ExitCode}.");
    }
}
