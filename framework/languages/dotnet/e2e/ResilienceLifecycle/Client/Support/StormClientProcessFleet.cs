using System.Diagnostics;
using System.Reflection;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using ResilienceLifecycle.Shared;
using Systems.Zlink;
using Zlink.Framework;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.E2E.Diagnostics;

namespace ResilienceLifecycle.Client.Support;

internal sealed class StormClientProcessFleet : IAsyncDisposable
{
    private const int ClientCount = 100;
    private const int StartupBatchSize = 8;
    private const int ShutdownBatchSize = 4;
    private readonly StormProcess[] _workers;

    private StormClientProcessFleet(StormProcess[] workers)
    {
        _workers = workers;
    }

    public static async Task<StormClientProcessFleet> StartAsync(
        ClientOptions options,
        CancellationToken cancellationToken = default)
    {
        var assemblyPath = Assembly.GetExecutingAssembly().Location;
        var workers = new List<StormProcess>(ClientCount);
        try
        {
            for (var firstIndex = 0; firstIndex < ClientCount; firstIndex += StartupBatchSize)
            {
                var batch = new List<StormProcess>(
                    Math.Min(StartupBatchSize, ClientCount - firstIndex));
                for (var index = firstIndex;
                     index < Math.Min(firstIndex + StartupBatchSize, ClientCount);
                     index++)
                {
                    var worker = StormProcess.Start(
                        assemblyPath,
                        options.RedisEndpoint,
                        options.RedisKeyPrefix,
                        options.LogDir,
                        index);
                    workers.Add(worker);
                    batch.Add(worker);
                }

                await Task.WhenAll(batch.Select(worker =>
                    worker.WaitStartedAsync(cancellationToken)));
            }

            return new StormClientProcessFleet(workers.ToArray());
        }
        catch
        {
            await DisposeWorkersAsync(workers);
            throw;
        }
    }

    public async Task<ProfileRes[]> RequestAllAsync(
        string markerPrefix,
        CancellationToken cancellationToken = default)
    {
        return await Task.WhenAll(_workers.Select((worker, index) =>
            worker.RequestAsync($"{markerPrefix}-{index}", cancellationToken)));
    }

    public Task WaitReadyAfterRestartAsync(CancellationToken cancellationToken = default)
    {
        return Task.WhenAll(_workers.Select(worker =>
            worker.WaitReadyAfterRestartAsync(cancellationToken)));
    }

    public async ValueTask DisposeAsync()
    {
        var failures = await DisposeWorkersAsync(_workers);
        if (failures.Count != 0)
            throw new AggregateException(
                "One or more storm clients failed during teardown.",
                failures);
    }

    private static async Task<List<Exception>> DisposeWorkersAsync(
        IReadOnlyList<StormProcess> workers)
    {
        var failures = new List<Exception>();
        for (var firstIndex = 0;
             firstIndex < workers.Count;
             firstIndex += ShutdownBatchSize)
        {
            var batchEnd = Math.Min(firstIndex + ShutdownBatchSize, workers.Count);
            var results = new Task<Exception?>[batchEnd - firstIndex];
            for (var index = firstIndex; index < batchEnd; index++)
            {
                results[index - firstIndex] = DisposeWorkerAsync(workers[index]);
            }

            foreach (var failure in await Task.WhenAll(results))
            {
                if (failure is not null)
                    failures.Add(failure);
            }
        }

        return failures;
    }

    private static async Task<Exception?> DisposeWorkerAsync(StormProcess worker)
    {
        try
        {
            await worker.DisposeAsync();
            return null;
        }
        catch (Exception exception)
        {
            return exception;
        }
    }

    private sealed class StormProcess : IAsyncDisposable
    {
        private readonly int _index;
        private readonly Process _process;
        private readonly Task<string> _stderr;
        private readonly string _stderrLogPath;
        private ulong _initialProviderGeneration;

        private StormProcess(int index, Process process, string stderrLogPath)
        {
            _index = index;
            _process = process;
            _stderr = process.StandardError.ReadToEndAsync();
            _stderrLogPath = stderrLogPath;
        }

        public static StormProcess Start(
            string assemblyPath,
            string redisEndpoint,
            string redisKeyPrefix,
            string logDir,
            int index)
        {
            var start = new ProcessStartInfo("dotnet")
            {
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false
            };
            start.ArgumentList.Add(assemblyPath);
            start.ArgumentList.Add("--storm-worker");
            start.ArgumentList.Add(redisEndpoint);
            start.ArgumentList.Add(redisKeyPrefix);
            start.ArgumentList.Add(logDir);
            start.ArgumentList.Add(index.ToString(System.Globalization.CultureInfo.InvariantCulture));
            start.Environment["ZLINK_ROUTER_DEBUG"] = "1";
            var process = Process.Start(start)
                          ?? throw new InvalidOperationException($"Unable to start storm client {index}.");
            return new StormProcess(
                index,
                process,
                Path.Combine(logDir, $"storm-{index}-core.log"));
        }

        public async Task WaitStartedAsync(CancellationToken cancellationToken)
        {
            var line = await ReadLineAsync(TimeSpan.FromSeconds(30), cancellationToken);
            var parts = line.Split('\t');
            if (parts is not ["READY", var generation]
                || !ulong.TryParse(generation, out _initialProviderGeneration))
                throw new InvalidOperationException(
                    $"Storm client {_index} returned an invalid startup line '{line}'.");
        }

        public async Task<ProfileRes> RequestAsync(
            string marker,
            CancellationToken cancellationToken)
        {
            await WriteLineAsync($"REQUEST\t{marker}", cancellationToken);
            string line;
            try
            {
                line = await ReadLineAsync(TimeSpan.FromSeconds(15), cancellationToken);
            }
            catch (Exception exception)
            {
                throw new InvalidOperationException(
                    $"Storm client {_index} request '{marker}' failed.",
                    exception);
            }
            var parts = line.Split('\t');
            if (parts is not ["REPLY", var value, var providerRid, var replyMarker])
                throw new InvalidOperationException(
                    $"Storm client {_index} returned an invalid reply line '{line}'.");
            return new ProfileRes(value, providerRid, replyMarker);
        }

        public async Task WaitReadyAfterRestartAsync(CancellationToken cancellationToken)
        {
            await WriteLineAsync(
                $"WAIT-READY-AFTER\t{_initialProviderGeneration}",
                cancellationToken);
            var line = await ReadLineAsync(TimeSpan.FromSeconds(30), cancellationToken);
            var parts = line.Split('\t');
            if (parts is not ["READY", var generation]
                || !ulong.TryParse(generation, out var parsed)
                || parsed == _initialProviderGeneration)
                throw new InvalidOperationException(
                    $"Storm client {_index} did not observe a new provider generation: '{line}'.");
            _initialProviderGeneration = parsed;
        }

        public async ValueTask DisposeAsync()
        {
            try
            {
                if (!_process.HasExited)
                {
                    try
                    {
                        await WriteLineAsync("EXIT", CancellationToken.None);
                        await _process.WaitForExitAsync()
                            .WaitAsync(TimeSpan.FromSeconds(5));
                    }
                    catch
                    {
                        if (!_process.HasExited)
                            _process.Kill(entireProcessTree: true);
                    }
                }

                if (!_process.HasExited)
                    await _process.WaitForExitAsync();
                var exitCode = _process.ExitCode;
                var stderr = await _stderr;
                if (!string.IsNullOrEmpty(stderr))
                    await File.WriteAllTextAsync(_stderrLogPath, stderr);
                if (exitCode != 0)
                    throw new InvalidOperationException(
                        $"Storm client {_index} exited with code {exitCode}. "
                        + $"See '{_stderrLogPath}'.");
            }
            finally
            {
                _process.Dispose();
            }
        }

        private async Task WriteLineAsync(string line, CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            await _process.StandardInput.WriteLineAsync(line);
            await _process.StandardInput.FlushAsync();
        }

        private async Task<string> ReadLineAsync(
            TimeSpan timeout,
            CancellationToken cancellationToken)
        {
            var line = await _process.StandardOutput.ReadLineAsync(cancellationToken)
                .AsTask()
                .WaitAsync(timeout, cancellationToken);
            if (line is not null)
                return line;

            var stderr = await _stderr.WaitAsync(TimeSpan.FromSeconds(1), cancellationToken);
            throw new InvalidOperationException(
                $"Storm client {_index} exited before replying: {stderr}");
        }
    }
}

internal static class StormClientWorker
{
    public static async Task RunAsync(string[] args)
    {
        if (args.Length != 4 || !int.TryParse(args[3], out var index))
            throw new ArgumentException(
                "Storm worker requires redis endpoint, key prefix, log directory and index.");

        var redisEndpoint = args[0];
        var redisKeyPrefix = args[1];
        var logDir = args[2];
        using var host = Host.CreateDefaultBuilder()
            .ConfigureAppConfiguration(static configuration =>
                configuration.Sources.Clear())
            .ConfigureLogging(static logging => logging.ClearProviders())
            .ConfigureServices(services =>
            {
                services.AddSingleton(new E2eMessageFlowListener(
                    Path.Combine(logDir, $"storm-{index}-flow.log"),
                    $"storm-{index}"));
                services.AddZLinkFramework(framework =>
                {
                    //  This E2E host is not started inside a memory-limited
                    //  container. Supply a deterministic finite limit so the
                    //  default Auto HWM contract does not depend on the host.
                    framework.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
                        1UL * 1024 * 1024 * 1024;
                    framework.AddLocationStore(new ZLinkRedisLocationStore(redis => { redis.ConnectionString = redisEndpoint; redis.KeyPrefix = redisKeyPrefix; }));
                    framework.ConfigureDispatch().Diagnostics
                        .SetLevel(ZLinkDiagnosticsLevel.Normal);
                    var mesh = framework.AddRouteMesh(ResilienceLifecycleNames.Channel)
                        .Listen("tcp://127.0.0.1:0")
                        .SetRoutingIdPrefix("storm");
                    mesh.Channel(ResilienceLifecycleNames.Channel).Client();
                });
            })
            .Build();

        await host.StartAsync();
        var runtime = host.Services.GetRequiredService<IZLinkRouteMeshRuntime>();
        var client = host.Services.GetRequiredService<IZLinkRouteClient>();
        var initialGeneration = await WaitForProviderAsync(runtime, null);
        Console.WriteLine($"READY\t{initialGeneration}");

        while (await Console.In.ReadLineAsync() is { } command)
        {
            var parts = command.Split('\t');
            switch (parts)
            {
                case ["REQUEST", var marker]:
                {
                    var reply = await client.RequestToChannel(
                            ResilienceLifecycleNames.Channel,
                            new ProfileReq("fast", marker))
                        .Timeout(TimeSpan.FromSeconds(10))
                        .Async<ProfileRes>();
                    Console.WriteLine(
                        $"REPLY\t{reply.Value}\t{reply.ProviderRid}\t{reply.Marker}");
                    break;
                }
                case ["WAIT-READY-AFTER", var generation]
                    when ulong.TryParse(generation, out var previousGeneration):
                {
                    var readyGeneration =
                        await WaitForProviderAsync(runtime, previousGeneration);
                    Console.WriteLine($"READY\t{readyGeneration}");
                    break;
                }
                case ["EXIT"]:
                    await host.StopAsync();
                    return;
                default:
                    throw new InvalidOperationException(
                        $"Unsupported storm worker command '{command}'.");
            }
        }
    }

    private static async Task<ulong> WaitForProviderAsync(
        IZLinkRouteMeshRuntime runtime,
        ulong? previousGeneration)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(30);
        while (DateTimeOffset.UtcNow < deadline)
        {
            var status = runtime.GetStatus(ResilienceLifecycleNames.Channel);
            var peer = status.Peers
                .FirstOrDefault(candidate =>
                    candidate.State == ZLinkPeerState.Ready
                    && IsProviderRid(candidate.NodeRid)
                    && (previousGeneration is null
                        || status.Sequence != previousGeneration.Value));
            if (peer is not null)
                return status.Sequence;
            await Task.Delay(100);
        }

        throw new TimeoutException("Storm worker did not observe provider api-b ready.");
    }

    private static bool IsProviderRid(RoutingId nodeRid)
    {
        var value = nodeRid.ToString();
        return value.Equals("api-b", StringComparison.Ordinal)
               || value.StartsWith("api-b-", StringComparison.Ordinal);
    }
}
