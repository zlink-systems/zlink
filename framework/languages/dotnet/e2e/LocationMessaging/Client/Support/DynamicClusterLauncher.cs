using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using LocationMessaging.Shared;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient;
using Zlink.Framework.E2E.Configuration;

namespace LocationMessaging.Client.Support;

internal sealed class DynamicClusterLauncher(
    string providerProject,
    string consumerProject,
    string workflowProject,
    string configDir,
    string logDir,
    string scenarioName) : IAsyncDisposable
{
    private readonly List<DynamicProcess> _processes = [];

    public string RedisEndpoint { get; private set; } = "";

    public string RedisKeyPrefix { get; private set; } = "";

    public async ValueTask DisposeAsync()
    {
        for (var i = _processes.Count - 1; i >= 0; i--) await _processes[i].StopAsync();

        _processes.Clear();
    }

    public static Task<DynamicClusterLauncher> StartAsync(ClientOptions options, string scenarioName)
    {
        // No registry process exists. Each dynamic cluster shares the run's
        // Redis instance but isolates its peer location rows under a
        // scenario-specific key prefix (mirrors the doc's per-run isolation).
        var scenarioConfigDir = Path.Combine(options.ConfigDir, scenarioName);
        var scenarioLogDir = Path.Combine(options.LogDir, "dynamic", scenarioName);
        Directory.CreateDirectory(scenarioConfigDir);
        Directory.CreateDirectory(scenarioLogDir);
        var launcher = new DynamicClusterLauncher(
            options.ProviderProject,
            options.ConsumerProject,
            options.WorkflowProject,
            scenarioConfigDir,
            scenarioLogDir,
            scenarioName)
        {
            RedisEndpoint = options.RedisEndpoint,
            RedisKeyPrefix = $"{options.RedisKeyPrefix}:{scenarioName}"
        };
        return Task.FromResult(launcher);
    }

    public async Task<DynamicProvider> StartProviderAsync(
        string name,
        string rid,
        int weight = 100,
        IReadOnlyList<string>? routePeers = null)
    {
        var processName = $"{scenarioName}-{name}";
        var httpUrl = PickHttpUrl();
        var channelEndpoint = PickEndpoint();
        var routeEndpoint = PickEndpoint();
        var process = StartServer(
            processName,
            providerProject,
            new DynamicProviderOptions(
                Role: "provider",
                HttpUrl: httpUrl,
                LogDir: logDir,
                Rid: rid,
                Weight: weight,
                EvidenceFile: Path.Combine(logDir, $"{processName}.evidence.log"),
                RedisEndpoint: RedisEndpoint,
                RedisKeyPrefix: RedisKeyPrefix,
                ChannelEndpoint: channelEndpoint,
                RouteEndpoint: routeEndpoint,
                RoutePeers: routePeers ?? []),
            httpUrl,
            channelEndpoint);
        await process.WaitReadyAsync();
        return new DynamicProvider(process, httpUrl, process.RequireChannelEndpoint(), routeEndpoint);
    }

    public async Task<DynamicConsumer> StartConsumerAsync(
        string name,
        bool registerWorkflowClient = false)
    {
        var processName = $"{scenarioName}-{name}";
        var httpUrl = PickHttpUrl();
        var process = StartServer(
            processName,
            consumerProject,
            new DynamicConsumerOptions(
                httpUrl,
                logDir,
                name,
                RedisEndpoint,
                RedisKeyPrefix,
                registerWorkflowClient),
            httpUrl,
            channelEndpoint: null);
        await process.WaitReadyAsync();
        return new DynamicConsumer(process, httpUrl);
    }

    public async Task<DynamicWorkflow> StartWorkflowAsync(
        string name,
        string rid,
        int weight = 100)
    {
        var processName = $"{scenarioName}-{name}";
        var httpUrl = PickHttpUrl();
        var workflowEndpoint = PickEndpoint();
        var process = StartServer(
            processName,
            workflowProject,
            new DynamicWorkflowOptions(
                Role: "workflow",
                HttpUrl: httpUrl,
                LogDir: logDir,
                Rid: rid,
                WorkflowEndpoint: workflowEndpoint,
                Weight: weight,
                EvidenceFile: Path.Combine(logDir, $"{processName}.evidence.log"),
                RedisEndpoint: RedisEndpoint,
                RedisKeyPrefix: RedisKeyPrefix),
            httpUrl,
            workflowEndpoint);
        await process.WaitReadyAsync();
        return new DynamicWorkflow(process, httpUrl, process.RequireChannelEndpoint());
    }

    public async Task<DrainResultRes> StopAsync(DynamicProvider provider)
    {
        var result = await provider.Process.DrainAsync();
        await provider.Process.StopAsync();
        _processes.Remove(provider.Process);
        return result;
    }

    public async Task CrashAsync(DynamicProvider provider)
    {
        await provider.Process.CrashAsync();
        _processes.Remove(provider.Process);
    }

    private DynamicProcess StartServer(
        string name,
        string projectPath,
        object roleOptions,
        string httpUrl,
        string? channelEndpoint)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = "dotnet",
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };
        startInfo.ArgumentList.Add("run");
        startInfo.ArgumentList.Add("--no-build");
        startInfo.ArgumentList.Add("--project");
        startInfo.ArgumentList.Add(projectPath);
        startInfo.ArgumentList.Add("--");
        startInfo.ArgumentList.Add("--config");
        startInfo.ArgumentList.Add(E2eConfiguration.Write(configDir, name, roleOptions));

        var process = Process.Start(startInfo)
                      ?? throw new InvalidOperationException($"Failed to start {name}.");
        _ = CopyToFileAsync(process.StandardOutput, Path.Combine(logDir, $"{name}.stdout.log"));
        _ = CopyToFileAsync(process.StandardError, Path.Combine(logDir, $"{name}.stderr.log"));
        var dynamicProcess = new DynamicProcess(process, httpUrl, channelEndpoint);
        _processes.Add(dynamicProcess);
        return dynamicProcess;
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

    private static string PickEndpoint()
    {
        return $"tcp://127.0.0.1:{PickPort()}";
    }

    private static string PickHttpUrl()
    {
        return $"http://127.0.0.1:{PickPort()}";
    }

    private static int PickPort()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        return ((IPEndPoint)listener.LocalEndpoint).Port;
    }
}

internal sealed record DynamicProviderOptions(
    string Role,
    string HttpUrl,
    string LogDir,
    string Rid,
    int Weight,
    string EvidenceFile,
    string RedisEndpoint,
    string RedisKeyPrefix,
    string ChannelEndpoint,
    string RouteEndpoint,
    IReadOnlyList<string>? RoutePeers = null);

internal sealed record DynamicConsumerOptions(
    string HttpUrl,
    string LogDir,
    string TraceLabel,
    string RedisEndpoint,
    string RedisKeyPrefix,
    bool RegisterWorkflowClient = false);

internal sealed record DynamicWorkflowOptions(
    string Role,
    string HttpUrl,
    string LogDir,
    string Rid,
    string WorkflowEndpoint,
    int Weight,
    string? EvidenceFile = null,
    string? RedisEndpoint = null,
    string? RedisKeyPrefix = null);

internal sealed record DynamicProvider(
    DynamicProcess Process,
    string HttpUrl,
    string ChannelEndpoint,
    string RouteEndpoint);

internal sealed record DynamicWorkflow(
    DynamicProcess Process,
    string HttpUrl,
    string WorkflowEndpoint);

internal sealed record DynamicConsumer(DynamicProcess Process, string HttpUrl);

internal sealed class DynamicProcess(Process process, string httpUrl, string? channelEndpoint)
{
    private static readonly TimeSpan ReadinessTimeout = TimeSpan.FromSeconds(3);
    private static readonly TimeSpan ReadinessPollInterval = TimeSpan.FromMilliseconds(100);
    private static readonly TimeSpan GracefulShutdownTimeout = TimeSpan.FromSeconds(30);
    private bool _disposed;

    public string HttpUrl { get; } = httpUrl;

    public string? ChannelEndpoint { get; } = channelEndpoint;

    public string RequireChannelEndpoint()
    {
        return ChannelEndpoint ??
               throw new InvalidOperationException("This process does not expose a channel endpoint.");
    }

    public async Task WaitReadyAsync()
    {
        using var client = ZLinkHttpClient.Create(HttpUrl)
            .Timeout(TimeSpan.FromMilliseconds(250))
            .Build();
        var elapsed = Stopwatch.StartNew();
        while (elapsed.Elapsed < ReadinessTimeout)
        {
            if (process.HasExited)
                throw new InvalidOperationException($"Process exited before readiness: {process.ExitCode}.");

            try
            {
                await client.Get("/health").Async<string>();
                return;
            }
            catch (ZLinkFrameworkException error) when (
                error.Kind is ZLinkFrameworkErrorKind.Unavailable
                    or ZLinkFrameworkErrorKind.DeadlineExceeded)
            {
            }

            await Task.Delay(ReadinessPollInterval);
        }

        throw new TimeoutException($"Process did not become ready: {HttpUrl}.");
    }

    public async Task StopAsync()
    {
        if (_disposed) return;

        _disposed = true;
        if (!process.HasExited)
            try
            {
                using var client = ZLinkHttpClient.Create(HttpUrl)
                    .Timeout(TimeSpan.FromSeconds(5))
                    .Build();
                await client.Post("/shutdown").AsyncRaw();
            }
            catch (ZLinkFrameworkException error) when (
                error.Kind is ZLinkFrameworkErrorKind.Unavailable
                    or ZLinkFrameworkErrorKind.DeadlineExceeded)
            {
                if (!process.HasExited) process.Kill(true);
            }

        try
        {
            await process.WaitForExitAsync().WaitAsync(GracefulShutdownTimeout);
        }
        catch (TimeoutException)
        {
            if (!process.HasExited) process.Kill(true);
            await process.WaitForExitAsync();
        }

        process.Dispose();
    }

    public async Task<DrainResultRes> DrainAsync()
    {
        using var client = ZLinkHttpClient.Create(HttpUrl)
            .Timeout(TimeSpan.FromSeconds(35))
            .Build();
        return (await client.Post("/admin/drain").Async<DrainResultRes>()).Body;
    }

    public async Task CrashAsync()
    {
        if (_disposed) return;

        _disposed = true;
        if (!process.HasExited) process.Kill(true);
        await process.WaitForExitAsync();
        process.Dispose();
    }
}
