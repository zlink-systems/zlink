using System.Buffers;
using System.Diagnostics;
using System.Net;
using System.Net.Http.Json;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using Google.Protobuf;
using Grpc.Net.Client;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink;
using WithGrpcBench.Shared;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Contracts.Channels;

AppContext.SetSwitch("System.Net.Http.SocketsHttpHandler.Http2UnencryptedSupport", true);

var options = BenchOptions.Parse(args);
options.Validate();
ConfigureThreadPool(options);
Directory.CreateDirectory(options.Output);

using var grpcHandler = new SocketsHttpHandler
{
    EnableMultipleHttp2Connections = false,
    MaxConnectionsPerServer = 1
};
using var grpcChannel = GrpcChannel.ForAddress(options.GrpcUrl, new GrpcChannelOptions
{
    HttpHandler = grpcHandler
});
var grpc = new BenchService.BenchServiceClient(grpcChannel);

var zlinkBuilder = Host.CreateApplicationBuilder(args);
ConfigureQuietLogging(zlinkBuilder);
zlinkBuilder.Services.AddZLinkFramework(framework =>
{
    framework.Codecs.Use(ZLinkProtobufCodec.Default);
    var mesh = framework.AddRouteMesh("bench")
        .Listen("tcp://127.0.0.1:0")
        .SetRoutingId(RoutingId.From("bench-client"));
    mesh.Channel("bench").Client();
    mesh.PeerConnections.Connect(
        RoutingId.From("bench-server"),
        options.ZLinkEndpoint);
});

using var zlinkHost = zlinkBuilder.Build();
await zlinkHost.StartAsync();
var zlink = zlinkHost.Services.GetRequiredService<IZLinkRouteClient>();
using var rawContext = Systems.Zlink.Zlink.CreateContext();
var metadata = await CreateMetadataAsync(options);

var results = new List<BenchResult>();
var failures = new List<string>();
var contaminated = new List<string>();
var drainState = new DrainState();
foreach (var payloadSize in options.PayloadSizes)
{
    if (options.Scenario is "all" or "request" or "request-serial")
    {
        Console.Error.WriteLine($"[bench] request payload={payloadSize} mode=serial");
        if (options.ShouldRunImplementation("grpc-dotnet"))
        {
            await AddCellAsync(results, failures, contaminated, drainState, "grpc-dotnet-request-serial", async () =>
                await RunRequestSerialAsync(
                "grpc-dotnet-request-serial",
                payloadSize,
                options,
                options.GrpcStatsUrl,
                async (payload, ct) =>
                {
                    return await grpc.EchoAsync(payload, cancellationToken: ct);
                }));
        }

        if (options.ShouldRunImplementation("zlink-dotnet"))
        {
            await AddCellAsync(results, failures, contaminated, drainState, "zlink-dotnet-request-serial", async () =>
                await RunRawRequestSerialAsync(rawContext, payloadSize, options));
        }

        if (options.ShouldRunImplementation("zlink-framework-dotnet"))
        {
            await AddCellAsync(results, failures, contaminated, drainState, "zlink-framework-dotnet-request-serial", async () =>
                await RunRequestSerialAsync(
                "zlink-framework-dotnet-request-serial",
                payloadSize,
                options,
                options.ZLinkStatsUrl,
                async (payload, ct) =>
                {
                    return await zlink.RequestToChannel("bench", payload)
                        .Async<BenchPayload>(ct);
                }));
        }
    }

    if (options.Scenario is "all" or "request" or "request-window")
    {
        Console.Error.WriteLine($"[bench] request payload={payloadSize} mode=window window={options.RequestWindow}");
        if (options.ShouldRunImplementation("grpc-dotnet"))
        {
            await AddCellAsync(results, failures, contaminated, drainState, "grpc-dotnet-request-window", async () =>
                await RunRequestAsync(
                "grpc-dotnet-request-window",
                payloadSize,
                options,
                options.GrpcStatsUrl,
                async (payload, ct) =>
                {
                    return await grpc.EchoAsync(payload, cancellationToken: ct);
                }));
        }

        if (options.ShouldRunImplementation("zlink-dotnet"))
        {
            await AddCellAsync(results, failures, contaminated, drainState, "zlink-dotnet-request-window", async () =>
                await RunRawRequestAsync(rawContext, payloadSize, options));
        }

        if (options.ShouldRunImplementation("zlink-framework-dotnet"))
        {
            await AddCellAsync(results, failures, contaminated, drainState, "zlink-framework-dotnet-request-window", async () =>
                await RunRequestAsync(
                "zlink-framework-dotnet-request-window",
                payloadSize,
                options,
                options.ZLinkStatsUrl,
                async (payload, ct) =>
                {
                    return await zlink.RequestToChannel("bench", payload)
                        .Async<BenchPayload>(ct);
                }));
        }
    }

    if (options.Scenario is "all" or "send" or "command" or "send-saturation")
    {
        Console.Error.WriteLine($"[bench] send payload={payloadSize} concurrency={options.SendConcurrency}");
        if (options.ShouldRunImplementation("grpc-dotnet"))
        {
            await AddCellAsync(results, failures, contaminated, drainState, "grpc-dotnet-send-saturation", async () =>
                await RunSendAsync(
                "grpc-dotnet-send-saturation",
                payloadSize,
                options,
                options.GrpcStatsUrl,
                drainState,
                async (_, payload, ct) => await grpc.CommandAsync(payload, cancellationToken: ct)));
        }

        if (options.ShouldRunImplementation("zlink-dotnet"))
        {
            await AddCellAsync(results, failures, contaminated, drainState, "zlink-dotnet-send-saturation", async () =>
                await RunRawSendAsync(rawContext, payloadSize, options, drainState));
        }

        if (options.ShouldRunImplementation("zlink-framework-dotnet"))
        {
            await AddCellAsync(results, failures, contaminated, drainState, "zlink-framework-dotnet-send-saturation", async () =>
                await RunSendAsync(
                "zlink-framework-dotnet-send-saturation",
                payloadSize,
                options,
                options.ZLinkStatsUrl,
                drainState,
                async (_, payload, ct) =>
                {
                    await zlink.SendToChannel("bench", payload).Async(ct);
                }));
        }
    }
}

Print(results);

var resultFile = Path.Combine(options.Output, "results.json");
var report = new BenchReport(metadata, results);
var jsonOptions = new JsonSerializerOptions
{
    WriteIndented = true,
    PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    Converters = { new JsonStringEnumConverter(JsonNamingPolicy.CamelCase) }
};
await File.WriteAllTextAsync(
    resultFile,
    JsonSerializer.Serialize(report, jsonOptions));
await File.WriteAllTextAsync(options.ReportPath, FormatText(report));

// BENCH_POLICY 6.6 style failure summary. A cell that failed is absent from the
// tables above; it is listed here and in failures.txt so a partial run can never
// be mistaken for a complete one.
if (failures.Count > 0 || contaminated.Count > 0 || drainState.Observations.Count > 0)
{
    var summary = new StringBuilder();
    if (drainState.Observations.Count > 0)
    {
        summary.AppendLine("## Drain (FB-008)");
        foreach (var observation in drainState.Observations)
        {
            summary.AppendLine($"- {observation}");
        }

        summary.AppendLine();
    }

    if (contaminated.Count > 0)
    {
        summary.AppendLine("## Contaminated (excluded from tables and judgement)");
        foreach (var cell in contaminated)
        {
            summary.AppendLine($"- {cell}");
        }

        summary.AppendLine();
    }

    summary.AppendLine("## Failures");
    foreach (var failure in failures)
    {
        summary.AppendLine($"- {failure}");
    }

    var text = summary.ToString();
    Console.Error.Write(text);
    await File.AppendAllTextAsync(options.ReportPath, Environment.NewLine + text);
    await File.WriteAllTextAsync(Path.Combine(options.Output, "failures.txt"), text);
}

Console.Error.WriteLine(
    $"[bench] cells completed={results.Count} failed={failures.Count} "
    + $"contaminated={contaminated.Count}");

await zlinkHost.StopAsync();

static async ValueTask<BenchResult> RunRequestSerialAsync(
    string name,
    int payloadSize,
    BenchOptions options,
    string statsUrl,
    Func<BenchPayload, CancellationToken, ValueTask<BenchPayload>> operation)
{
    using var cts = new CancellationTokenSource(options.Timeout);
    using var http = new HttpClient();

    for (var i = 0; i < options.Warmup; i++)
    {
        var payload = BenchMetricHeaders.CreatePayload(payloadSize, options.RunId, BenchPhase.Warmup, (ulong)i);
        var reply = await operation(payload, cts.Token);
        ValidateReply(reply, options.RunId, BenchPhase.Warmup, payloadSize, (ulong)i);
    }

    using var reset = await http.PostAsync($"{statsUrl}/bench/reset", null, cts.Token);
    reset.EnsureSuccessStatusCode();

    var samples = new List<long>();
    var errors = 0;
    var completed = 0;
    var resources = ResourceSample.Start();
    var total = Stopwatch.StartNew();
    var activeUntil = total.Elapsed + TimeSpan.FromSeconds(options.DurationSeconds);

    while (total.Elapsed < activeUntil)
    {
        var index = completed + errors;
        var payload = BenchMetricHeaders.CreatePayload(
            payloadSize,
            options.RunId,
            BenchPhase.Active,
            (ulong)index);
        var started = Stopwatch.GetTimestamp();
        try
        {
            var reply = await operation(payload, cts.Token);
            ValidateReply(reply, options.RunId, BenchPhase.Active, payloadSize, (ulong)index);
            completed++;
        }
        catch
        {
            errors++;
        }

        AddLatencySample(samples, ElapsedMicros(started, Stopwatch.GetTimestamp()), options.LatencySampleLimit);
    }

    total.Stop();
    var clientCpuSeconds = resources.CpuSeconds();
    var clientMemoryMb = resources.WorkingSetMb();
    var server = await http.GetFromJsonAsync<BenchServerSnapshot>($"{statsUrl}/bench/stats", cts.Token)
        ?? BenchServerSnapshot.Empty;

    return new BenchResult(
        name,
        "KOPS",
        payloadSize,
        options.DurationSeconds,
        completed,
        errors,
        server.Errors,
        options.Warmup,
        Math.Max(1.0, options.DurationSeconds),
        completed / Math.Max(1.0, options.DurationSeconds),
        PercentileSuccessful(samples, 0.95),
        PercentileSuccessful(samples, 0.99),
        MeanSuccessful(samples),
        null,
        null,
        null,
        clientCpuSeconds,
        clientMemoryMb,
        server.CpuSeconds,
        server.WorkingSetMb);
}

static async ValueTask<BenchResult> RunRequestAsync(
    string name,
    int payloadSize,
    BenchOptions options,
    string statsUrl,
    Func<BenchPayload, CancellationToken, ValueTask<BenchPayload>> operation)
{
    using var cts = new CancellationTokenSource(options.Timeout);
    using var http = new HttpClient();

    for (var i = 0; i < options.Warmup; i++)
    {
        var payload = BenchMetricHeaders.CreatePayload(payloadSize, options.RunId, BenchPhase.Warmup, (ulong)i);
        var reply = await operation(payload, cts.Token);
        ValidateReply(reply, options.RunId, BenchPhase.Warmup, payloadSize, (ulong)i);
    }

    using var reset = await http.PostAsync($"{statsUrl}/bench/reset", null, cts.Token);
    reset.EnsureSuccessStatusCode();

    var samples = new List<long>();
    var pending = new List<PendingBenchRequest>(options.RequestWindow);
    var next = 0UL;
    var errors = 0;
    var completed = 0;
    var resources = ResourceSample.Start();
    var total = Stopwatch.StartNew();
    var activeUntil = total.Elapsed + TimeSpan.FromSeconds(options.DurationSeconds);

    while (total.Elapsed < activeUntil || pending.Count > 0)
    {
        while (total.Elapsed < activeUntil && pending.Count < options.RequestWindow)
        {
            var index = next++;
            var payload = BenchMetricHeaders.CreatePayload(
                payloadSize,
                options.RunId,
                BenchPhase.Active,
                index);
            var started = Stopwatch.GetTimestamp();
            try
            {
                var request = operation(payload, cts.Token);
                if (request.IsCompletedSuccessfully)
                {
                    ValidateReply(request.Result, options.RunId, BenchPhase.Active, payloadSize, index);
                    completed++;
                    AddLatencySample(samples, ElapsedMicros(started, Stopwatch.GetTimestamp()), options.LatencySampleLimit);
                }
                else
                {
                    pending.Add(new PendingBenchRequest(request.AsTask(), started, index));
                }
            }
            catch
            {
                errors++;
                AddLatencySample(samples, ElapsedMicros(started, Stopwatch.GetTimestamp()), options.LatencySampleLimit);
            }
        }

        if (DrainCompletedRequests(pending, payloadSize, options, samples, ref completed, ref errors) == 0
            && pending.Count > 0)
        {
            await WaitAnyPendingRequestAsync(pending, cts.Token);
            DrainCompletedRequests(pending, payloadSize, options, samples, ref completed, ref errors);
        }
    }

    total.Stop();
    var clientCpuSeconds = resources.CpuSeconds();
    var clientMemoryMb = resources.WorkingSetMb();
    var server = await http.GetFromJsonAsync<BenchServerSnapshot>($"{statsUrl}/bench/stats", cts.Token)
        ?? BenchServerSnapshot.Empty;

    return new BenchResult(
        name,
        "KOPS",
        payloadSize,
        options.DurationSeconds,
        completed,
        errors,
        server.Errors,
        options.Warmup,
        Math.Max(1.0, options.DurationSeconds),
        completed / Math.Max(1.0, options.DurationSeconds),
        PercentileSuccessful(samples, 0.95),
        PercentileSuccessful(samples, 0.99),
        MeanSuccessful(samples),
        null,
        null,
        null,
        clientCpuSeconds,
        clientMemoryMb,
        server.CpuSeconds,
        server.WorkingSetMb);

}

static void ConfigureQuietLogging(HostApplicationBuilder builder)
{
    builder.Logging.ClearProviders();
    builder.Logging.AddConsole();
    builder.Logging.SetMinimumLevel(LogLevel.Warning);
}

static void ConfigureThreadPool(BenchOptions options)
{
    ThreadPool.GetMinThreads(out var workerThreads, out var completionPortThreads);
    var requestedWorkers = Math.Max(
        workerThreads,
        options.SendConcurrency + Environment.ProcessorCount * 2);
    ThreadPool.SetMinThreads(requestedWorkers, completionPortThreads);
}

static async ValueTask<BenchResult> RunSendAsync(
    string name,
    int payloadSize,
    BenchOptions options,
    string statsUrl,
    DrainState drain,
    Func<int, BenchPayload, CancellationToken, ValueTask> operation)
{
    using var cts = new CancellationTokenSource(options.Timeout);
    using var http = new HttpClient();

    for (var i = 0; i < options.Warmup; i++)
    {
        var payload = BenchMetricHeaders.CreatePayload(payloadSize, options.RunId, BenchPhase.Warmup, (ulong)i);
        await operation(0, payload, cts.Token);
    }

    using var reset = await http.PostAsync($"{statsUrl}/bench/reset", null, cts.Token);
    reset.EnsureSuccessStatusCode();

    var samples = new List<long>();
    var samplesGate = new object();
    var errors = 0;
    var sent = -1;
    var resources = ResourceSample.Start();
    var total = Stopwatch.StartNew();
    var activeUntil = total.Elapsed + TimeSpan.FromSeconds(options.DurationSeconds);

    var workers = Enumerable.Range(0, options.SendConcurrency)
        .Select(slot => Task.Run(async () =>
        {
            while (total.Elapsed < activeUntil)
            {
                var sequence = Interlocked.Increment(ref sent);
                var payload = BenchMetricHeaders.CreatePayload(
                    payloadSize,
                    options.RunId,
                    BenchPhase.Active,
                    (ulong)sequence);
                var started = Stopwatch.GetTimestamp();
                try
                {
                    await operation(slot, payload, cts.Token);
                }
                catch
                {
                    Interlocked.Increment(ref errors);
                }

                AddLatencySampleLocked(
                    samples,
                    samplesGate,
                    ElapsedMicros(started, Stopwatch.GetTimestamp()),
                    options.LatencySampleLimit);
            }
        }, cts.Token))
        .ToArray();

    await Task.WhenAll(workers);
    total.Stop();
    var clientCpuSeconds = resources.CpuSeconds();
    var clientMemoryMb = resources.WorkingSetMb();

    var attempted = sent + 1;
    var expectedServerMessages = Math.Max(0, attempted - errors);
    // FB-008: settle is a bounded wait that CONFIRMS the server drained, not a
    // fixed sleep. If the bound expires while the server is still advancing, the
    // next cell on this same server is marked contaminated and excluded.
    var outcome = await WaitForServerDrainAsync(
        http,
        statsUrl,
        expectedServerMessages,
        options.CommandSettleMs,
        options.DrainBoundMs,
        cts.Token);
    var server = outcome.Snapshot;
    drain.RecordDrain(ImplementationOf(name), name, outcome.DrainMs, outcome.BoundHit, options.DrainBoundMs);
    var missing = Math.Max(0, attempted - errors - server.ActiveMessages);

    return new BenchResult(
        name,
        "KMSG/s",
        payloadSize,
        options.DurationSeconds,
        server.ActiveMessages,
        errors,
        server.Errors + missing,
        options.Warmup,
        Math.Max(1.0, options.DurationSeconds),
        server.ActiveMessages / Math.Max(1.0, options.DurationSeconds),
        PercentileSuccessful(samples, 0.95),
        PercentileSuccessful(samples, 0.99),
        MeanSuccessful(samples),
        server.MeanMicros,
        server.P95Micros,
        server.P99Micros,
        clientCpuSeconds,
        clientMemoryMb,
        server.CpuSeconds,
        server.WorkingSetMb);
}

static async ValueTask<BenchResult> RunRawSendAsync(
    IContext context,
    int payloadSize,
    BenchOptions options,
    DrainState drain)
{
    using var socket = RawBenchSocket.Create(
        context,
        options.RawSocket,
        RoutingId.From(Encoding.ASCII.GetBytes($"bench-send-{Environment.ProcessId}")),
        RoutingId.From(BenchRoutingIds.RawCommandServer),
        options.ZLinkRawCommandEndpoint);
    await EnsureRawRouteReadyAsync(
        socket,
        options,
        token => RawSendAsync(
            socket,
            BenchMetricHeaders.CreatePayload(payloadSize, options.RunId, BenchPhase.Warmup, 0UL),
            token));

    return await RunSendAsync(
        "zlink-dotnet-send-saturation",
        payloadSize,
        options,
        options.ZLinkRawStatsUrl,
        drain,
        (_, payload, cancellationToken) =>
            RawSendAsync(socket, payload, cancellationToken));
}

static async ValueTask<BenchResult> RunRawRequestSerialAsync(
    IContext context,
    int payloadSize,
    BenchOptions options)
{
    using var socket = RawBenchSocket.Create(
        context,
        options.RawSocket,
        RoutingId.From(Encoding.ASCII.GetBytes($"bench-request-serial-{Environment.ProcessId}")),
        RoutingId.From(BenchRoutingIds.RawRequestServer),
        options.ZLinkRawEndpoint);
    await EnsureRawRouteReadyAsync(
        socket,
        options,
        async token => await RawRequestBytesAsync(
            socket, payloadSize, options.RunId, BenchPhase.Warmup, 0UL, token));

    return await RunRawRequestSerialBytesAsync(
        "zlink-dotnet-request-serial",
        payloadSize,
        options,
        options.ZLinkRawStatsUrl,
        socket);
}

static async ValueTask<BenchResult> RunRawRequestAsync(
    IContext context,
    int payloadSize,
    BenchOptions options)
{
    using var socket = RawBenchSocket.Create(
        context,
        options.RawSocket,
        RoutingId.From(Encoding.ASCII.GetBytes($"bench-request-{Environment.ProcessId}")),
        RoutingId.From(BenchRoutingIds.RawRequestServer),
        options.ZLinkRawEndpoint);
    await EnsureRawRouteReadyAsync(
        socket,
        options,
        async token => await RawRequestBytesAsync(
            socket, payloadSize, options.RunId, BenchPhase.Warmup, 0UL, token));

    return await RunRawRequestBytesAsync(
        "zlink-dotnet-request-window",
        payloadSize,
        options,
        options.ZLinkRawStatsUrl,
        socket);
}

static async ValueTask<BenchResult> RunRawRequestSerialBytesAsync(
    string name,
    int payloadSize,
    BenchOptions options,
    string statsUrl,
    RawBenchSocket socket)
{
    using var cts = new CancellationTokenSource(options.Timeout);
    using var http = new HttpClient();

    for (var i = 0; i < options.Warmup; i++)
    {
        var payload = BenchMetricHeaders.CreatePayload(payloadSize, options.RunId, BenchPhase.Warmup, (ulong)i);
        await RawRequestAsync(
            socket,
            payload,
            options.RunId,
            BenchPhase.Warmup,
            payloadSize,
            (ulong)i,
            cts.Token);
    }

    using var reset = await http.PostAsync($"{statsUrl}/bench/reset", null, cts.Token);
    reset.EnsureSuccessStatusCode();

    var samples = new List<long>();
    var errors = 0;
    var completed = 0;
    var resources = ResourceSample.Start();
    var total = Stopwatch.StartNew();
    var activeUntil = total.Elapsed + TimeSpan.FromSeconds(options.DurationSeconds);

    while (total.Elapsed < activeUntil)
    {
        var index = completed + errors;
        var payload = BenchMetricHeaders.CreatePayload(
            payloadSize,
            options.RunId,
            BenchPhase.Active,
            (ulong)index);
        var started = Stopwatch.GetTimestamp();
        try
        {
            await RawRequestAsync(
                socket,
                payload,
                options.RunId,
                BenchPhase.Active,
                payloadSize,
                (ulong)index,
                cts.Token);
            completed++;
        }
        catch
        {
            errors++;
        }

        AddLatencySample(samples, ElapsedMicros(started, Stopwatch.GetTimestamp()), options.LatencySampleLimit);
    }

    total.Stop();
    var clientCpuSeconds = resources.CpuSeconds();
    var clientMemoryMb = resources.WorkingSetMb();
    var server = await http.GetFromJsonAsync<BenchServerSnapshot>($"{statsUrl}/bench/stats", cts.Token)
        ?? BenchServerSnapshot.Empty;

    return new BenchResult(
        name,
        "KOPS",
        payloadSize,
        options.DurationSeconds,
        completed,
        errors,
        server.Errors,
        options.Warmup,
        Math.Max(1.0, options.DurationSeconds),
        completed / Math.Max(1.0, options.DurationSeconds),
        PercentileSuccessful(samples, 0.95),
        PercentileSuccessful(samples, 0.99),
        MeanSuccessful(samples),
        null,
        null,
        null,
        clientCpuSeconds,
        clientMemoryMb,
        server.CpuSeconds,
        server.WorkingSetMb);
}

static async ValueTask<BenchResult> RunRawRequestBytesAsync(
    string name,
    int payloadSize,
    BenchOptions options,
    string statsUrl,
    RawBenchSocket socket)
{
    using var cts = new CancellationTokenSource(options.Timeout);
    using var requestStop = CancellationTokenSource.CreateLinkedTokenSource(cts.Token);
    using var http = new HttpClient();

    for (var i = 0; i < options.Warmup; i++)
    {
        await RawRequestBytesAsync(
            socket,
            payloadSize,
            options.RunId,
            BenchPhase.Warmup,
            (ulong)i,
            requestStop.Token);
    }

    using var reset = await http.PostAsync($"{statsUrl}/bench/reset", null, cts.Token);
    reset.EnsureSuccessStatusCode();

    var pending = new List<PendingRawRequest>(options.RequestWindow);
    var metrics = new RawRequestWindowMetrics(options.LatencySampleLimit);
    var next = 0UL;
    var resources = ResourceSample.Start();
    var total = Stopwatch.StartNew();
    var activeUntil = total.Elapsed + TimeSpan.FromSeconds(options.DurationSeconds);

    while (total.Elapsed < activeUntil)
    {
        while (total.Elapsed < activeUntil
               && pending.Count < options.RequestWindow)
        {
            var sequence = next++;
            try
            {
                var request = RawRequestBytesAsync(
                    socket,
                    payloadSize,
                    options.RunId,
                    BenchPhase.Active,
                    sequence,
                    requestStop.Token);
                if (request.IsCompletedSuccessfully)
                {
                    metrics.RecordSuccess(request.Result);
                }
                else
                {
                    pending.Add(new PendingRawRequest(request.AsTask()));
                }
            }
            catch
            {
                metrics.RecordError();
            }
        }

        if (DrainCompletedRawRequests(
                pending,
                metrics) == 0
            && pending.Count > 0)
        {
            await WaitAnyPendingRawRequestAsync(
                pending,
                Timeout.InfiniteTimeSpan,
                cts.Token);
            DrainCompletedRawRequests(
                pending,
                metrics);
        }
    }

    var drainUntil = total.Elapsed + TimeSpan.FromMilliseconds(5000);
    while (pending.Count > 0 && total.Elapsed < drainUntil)
    {
        if (DrainCompletedRawRequests(
                pending,
                metrics) != 0)
            continue;

        var remaining = drainUntil - total.Elapsed;
        if (remaining <= TimeSpan.Zero
            || !await WaitAnyPendingRawRequestAsync(
                pending,
                remaining,
                cts.Token))
            break;
    }

    DrainCompletedRawRequests(
        pending,
        metrics);
    if (pending.Count > 0)
    {
        metrics.RecordErrors(pending.Count);
        var abandoned = pending.Select(static item => item.Task).ToArray();
        requestStop.Cancel();
        try
        {
            await Task.WhenAll(abandoned).ConfigureAwait(false);
        }
        catch
        {
            // The requests were already counted as failed at the drain bound.
        }
        pending.Clear();
    }

    total.Stop();
    var clientCpuSeconds = resources.CpuSeconds();
    var clientMemoryMb = resources.WorkingSetMb();
    var server = await http.GetFromJsonAsync<BenchServerSnapshot>($"{statsUrl}/bench/stats", cts.Token)
        ?? BenchServerSnapshot.Empty;

    return new BenchResult(
        name,
        "KOPS",
        payloadSize,
        options.DurationSeconds,
        metrics.Completed,
        metrics.Errors,
        server.Errors,
        options.Warmup,
        Math.Max(1.0, options.DurationSeconds),
        metrics.Completed / Math.Max(1.0, options.DurationSeconds),
        metrics.Percentile(0.95),
        metrics.Percentile(0.99),
        metrics.Mean,
        null,
        null,
        null,
        clientCpuSeconds,
        clientMemoryMb,
        server.CpuSeconds,
        server.WorkingSetMb);
}

static ValueTask<long> RawRequestAsync(
    RawBenchSocket socket,
    BenchPayload payload,
    uint runId,
    BenchPhase phase,
    int payloadSize,
    ulong sequence,
    CancellationToken cancellationToken) =>
    RawRequestMessageAsync(
        socket,
        EncodeRawPayload(payload),
        runId,
        phase,
        payloadSize,
        sequence,
        cancellationToken);

static ValueTask<long> RawRequestBytesAsync(
    RawBenchSocket socket,
    int payloadSize,
    uint runId,
    BenchPhase phase,
    ulong sequence,
    CancellationToken cancellationToken) =>
    RawRequestMessageAsync(
        socket,
        RawPayloadCodec.Encode(payloadSize, runId, phase, sequence),
        runId,
        phase,
        payloadSize,
        sequence,
        cancellationToken);

static async ValueTask<long> RawRequestMessageAsync(
    RawBenchSocket socket,
    Message body,
    uint runId,
    BenchPhase phase,
    int payloadSize,
    ulong sequence,
    CancellationToken cancellationToken)
{
    Message? header = null;
    IReadOnlyList<Message>? parts = null;
    try
    {
        header = Message.From(RawEnvelopeHeaders.Request);
        var request = socket.Request()
            .Message(header)
            .Message(body)
            .Async(cancellationToken);
        parts = await request.ConfigureAwait(false);
        if (parts.Count == 0)
            throw new InvalidOperationException("Raw zlink request returned no reply parts.");
        var replyPart = parts.Count == 1 ? parts[0] : parts[^1];
        return ValidateRawReply(
            replyPart.AsReadOnlySpan(),
            runId,
            phase,
            payloadSize,
            sequence);
    }
    finally
    {
        if (parts is not null)
            foreach (var part in parts)
                part.Dispose();
        header?.Dispose();
        body.Dispose();
    }
}

static async ValueTask<BenchServerSnapshot> WaitForServerStatsAsync(
    HttpClient http,
    string statsUrl,
    long expectedServerMessages,
    int settleMs,
    CancellationToken cancellationToken)
{
    if (settleMs > 0)
    {
        await Task.Delay(settleMs, cancellationToken);
    }

	    var deadline = Stopwatch.GetTimestamp() + Stopwatch.Frequency * Math.Max(1, settleMs) / 1000;
	    var latest = BenchServerSnapshot.Empty;
	    while (Stopwatch.GetTimestamp() < deadline)
	    {
	        latest = await http.GetFromJsonAsync<BenchServerSnapshot>($"{statsUrl}/bench/stats", cancellationToken)
	            ?? latest;
	        if (latest.ActiveMessages + latest.Errors >= expectedServerMessages)
	        {
	            return latest;
	        }

	        await Task.Delay(10, cancellationToken);
	    }

	    return latest;
	}

static void ValidateReply(BenchPayload reply, uint runId, BenchPhase phase, int payloadSize, ulong sequence)
{
    BenchPayloads.Validate(reply, payloadSize);
    if (!BenchMetricHeaders.TryDecode(reply, out var header)
        || !BenchMetricHeaders.IsExpected(header, runId, phase, payloadSize, sequence))
    {
        throw new InvalidOperationException($"Echo reply did not carry the expected {phase} metric header.");
    }
}

static long ValidateRawReply(ReadOnlySpan<byte> reply, uint runId, BenchPhase phase, int payloadSize, ulong sequence)
{
    if (!RawPayloadCodec.TryDecodeHeader(reply, out var header)
        || !BenchMetricHeaders.IsExpected(header, runId, phase, payloadSize, sequence))
    {
        throw new InvalidOperationException($"Raw echo reply did not carry the expected {phase} metric header.");
    }

    return Math.Max(0, BenchMetricHeaders.NowNs() - header.SentTimestampNs) / 1000;
}

// FB-008: bounded, drain-confirming settle.
//
// The previous behaviour slept up to --command-settle-ms and returned even if the
// server was still receiving, so a saturating cell's backlog rolled into the next
// cell on the same server. Here the server's received count is polled until it
// stops advancing for `quietMs`, bounded by `boundMs`. "Stopped advancing" is the
// drain signal: if the server never receives everything the client submitted, the
// count stalls below `expected` and that still means nothing is in flight.
static async ValueTask<DrainOutcome> WaitForServerDrainAsync(
    HttpClient http,
    string statsUrl,
    long expectedServerMessages,
    int quietMs,
    int boundMs,
    CancellationToken cancellationToken)
{
    var watch = Stopwatch.StartNew();
    var latest = BenchServerSnapshot.Empty;
    var lastCount = -1L;
    var lastChange = TimeSpan.Zero;

    while (watch.Elapsed < TimeSpan.FromMilliseconds(boundMs))
    {
        latest = await http.GetFromJsonAsync<BenchServerSnapshot>(
                     $"{statsUrl}/bench/stats", cancellationToken)
                 ?? latest;
        var count = latest.ActiveMessages + latest.Errors;
        if (count != lastCount)
        {
            lastCount = count;
            lastChange = watch.Elapsed;
        }
        else if ((watch.Elapsed - lastChange).TotalMilliseconds >= quietMs)
        {
            return new DrainOutcome(latest, watch.Elapsed.TotalMilliseconds, false);
        }

        await Task.Delay(10, cancellationToken);
    }

    return new DrainOutcome(latest, watch.Elapsed.TotalMilliseconds, true);
}

/// <summary>
///     Which of the three implementations a cell name belongs to. Contamination is
///     scoped per implementation because each one has its own server process: the
///     framework backlog that killed request-serial @4096 was invisible to the
///     grpc and raw cells that ran between the two framework cells.
/// </summary>
static string ImplementationOf(string cellName)
{
    foreach (var pattern in new[] { "request-serial", "request-window", "send-saturation" })
    {
        if (cellName.EndsWith(pattern, StringComparison.Ordinal))
        {
            return cellName[..^(pattern.Length + 1)];
        }
    }

    return cellName;
}

// Cell-level failure isolation. A single cell that throws (for example the
// framework channel request timing out during warmup) previously escaped as an
// unhandled exception and killed the whole process, destroying the other 17
// cells of the run. Record the failure, leave the cell absent from the report so
// it reads as `unsupported`, and continue. No retry and no fabricated result:
// this only stops one bad cell from taking the run down with it.
static async ValueTask AddCellAsync(
    List<BenchResult> results,
    List<string> failures,
    List<string> contaminated,
    DrainState drain,
    string name,
    Func<ValueTask<BenchResult>> body)
{
    // FB-008: a cell that follows a server which failed to drain is excluded from
    // the tables and from every judgement rather than measured and published.
    if (drain.TryTakeContamination(ImplementationOf(name), out var reason))
    {
        contaminated.Add($"{name}: {reason}");
        Console.Error.WriteLine($"[bench] CONTAMINATED {name}: {reason}");
        return;
    }

    Console.Error.WriteLine($"[bench] running {name}");
    try
    {
        results.Add(await body());
        Console.Error.WriteLine($"[bench] finished {name}");
    }
    catch (Exception ex)
    {
        var detail = $"{name}: {ex.GetType().Name}: {ex.Message}";
        failures.Add(detail);
        Console.Error.WriteLine($"[bench] FAILED {detail}");
    }
}

// A ROUTER addressing a peer by routing id fails with NOT_CONNECTED until that
// peer's routing id is in the local routing map (core spec 07-router 6/7), and a
// freshly connected DEALER fails its first submit with ENOTCONN for the same
// underlying reason: connect has not completed. Every cell builds its own socket,
// so wait once, before warmup, so no measured window contains connect latency.
//
// This applies to BOTH configurations deliberately. Giving the readiness wait only
// to ROUTER would bias the DEALER-vs-ROUTER comparison this job exists to produce.
// It runs entirely before the /bench/reset that opens the active phase and changes
// no measurement condition (duration, window, payload, concurrency).
static async ValueTask EnsureRawRouteReadyAsync(
    RawBenchSocket socket,
    BenchOptions options,
    Func<CancellationToken, ValueTask> probe)
{
    using var cts = new CancellationTokenSource(options.Timeout);
    var deadline = Stopwatch.GetTimestamp() + (Stopwatch.Frequency * RawBenchSocket.RouteReadyTimeoutSeconds);
    Exception? last = null;
    while (Stopwatch.GetTimestamp() < deadline)
    {
        try
        {
            await probe(cts.Token).ConfigureAwait(false);
            return;
        }
        catch (Exception ex)
        {
            last = ex;
            await Task.Delay(20, cts.Token).ConfigureAwait(false);
        }
    }

    throw new InvalidOperationException(
        $"Raw {(socket.IsRouter ? "ROUTER" : "DEALER")} route to the bench server was "
        + $"not ready within {RawBenchSocket.RouteReadyTimeoutSeconds}s.",
        last);
}

static async ValueTask RawSendAsync(
    RawBenchSocket socket,
    BenchPayload payload,
    CancellationToken cancellationToken)
{
    var body = EncodeRawPayload(payload);
    Message? header = null;
    try
    {
        header = Message.From(RawEnvelopeHeaders.Request);
        await socket.Send()
            .Message(header)
            .Message(body)
            .Async(cancellationToken)
            .ConfigureAwait(false);
    }
    finally
    {
        header?.Dispose();
        body.Dispose();
    }
}

static Message EncodeRawPayload(BenchPayload payload)
{
    var body = Message.Allocate(payload.CalculateSize());
    try
    {
        payload.WriteTo(body.AsSpan());
        return body;
    }
    catch
    {
        body.Dispose();
        throw;
    }
}

static long ElapsedMicros(long started, long stopped)
{
    return (long)((stopped - started) * 1_000_000.0 / Stopwatch.Frequency);
}

static long Percentile(long[] sortedSamples, double percentile)
{
    if (sortedSamples.Length == 0) return 0;
    var index = (int)Math.Ceiling(percentile * sortedSamples.Length) - 1;
    return sortedSamples[Math.Clamp(index, 0, sortedSamples.Length - 1)];
}

static long PercentileSuccessful(List<long> samples, double percentile)
{
    var sorted = samples.Where(static sample => sample > 0).ToArray();
    Array.Sort(sorted);
    return Percentile(sorted, percentile);
}

static double MeanSuccessful(List<long> samples)
{
    var positives = samples.Where(static sample => sample > 0).ToArray();
    return positives.Length == 0 ? 0 : positives.Average();
}

static void AddLatencySample(List<long> samples, long elapsedMicros, int limit)
{
    if (samples.Count < limit)
    {
        samples.Add(elapsedMicros);
    }
}

static int DrainCompletedRequests(
    List<PendingBenchRequest> pending,
    int payloadSize,
    BenchOptions options,
    List<long> samples,
    ref int completed,
    ref int errors)
{
    var drained = 0;
    for (var i = pending.Count - 1; i >= 0; i--)
    {
        var item = pending[i];
        if (!item.Task.IsCompleted) continue;

        pending.RemoveAt(i);
        drained++;
        try
        {
            var reply = item.Task.GetAwaiter().GetResult();
            ValidateReply(reply, options.RunId, BenchPhase.Active, payloadSize, item.Sequence);
            completed++;
        }
        catch
        {
            errors++;
        }
        finally
        {
            AddLatencySample(samples, ElapsedMicros(item.Started, Stopwatch.GetTimestamp()), options.LatencySampleLimit);
        }
    }

    return drained;
}

static async ValueTask WaitAnyPendingRequestAsync(
    List<PendingBenchRequest> pending,
    CancellationToken cancellationToken)
{
    var tasks = ArrayPool<Task<BenchPayload>>.Shared.Rent(pending.Count);
    try
    {
        for (var i = 0; i < pending.Count; i++)
            tasks[i] = pending[i].Task;

        await Task.WhenAny(new ArraySegment<Task<BenchPayload>>(tasks, 0, pending.Count))
            .WaitAsync(cancellationToken);
    }
    finally
    {
        Array.Clear(tasks, 0, pending.Count);
        ArrayPool<Task<BenchPayload>>.Shared.Return(tasks);
    }
}

static int DrainCompletedRawRequests(
    List<PendingRawRequest> pending,
    RawRequestWindowMetrics metrics)
{
    var drained = 0;
    for (var index = pending.Count - 1; index >= 0; index--)
    {
        var item = pending[index];
        if (!item.Task.IsCompleted)
            continue;

        pending.RemoveAt(index);
        drained++;
        try
        {
            metrics.RecordSuccess(item.Task.GetAwaiter().GetResult());
        }
        catch
        {
            metrics.RecordError();
        }
    }

    return drained;
}

static async ValueTask<bool> WaitAnyPendingRawRequestAsync(
    List<PendingRawRequest> pending,
    TimeSpan timeout,
    CancellationToken cancellationToken)
{
    var tasks = ArrayPool<Task<long>>.Shared.Rent(pending.Count);
    try
    {
        for (var index = 0; index < pending.Count; index++)
            tasks[index] = pending[index].Task;

        var completion = Task.WhenAny(
            new ArraySegment<Task<long>>(tasks, 0, pending.Count));
        if (timeout == Timeout.InfiniteTimeSpan)
        {
            await completion.WaitAsync(cancellationToken);
            return true;
        }

        try
        {
            await completion.WaitAsync(timeout, cancellationToken);
            return true;
        }
        catch (TimeoutException)
        {
            return false;
        }
    }
    finally
    {
        Array.Clear(tasks, 0, pending.Count);
        ArrayPool<Task<long>>.Shared.Return(tasks);
    }
}

static void AddLatencySampleLocked(List<long> samples, object gate, long elapsedMicros, int limit)
{
    lock (gate)
    {
        AddLatencySample(samples, elapsedMicros, limit);
    }
}

static void Print(IReadOnlyList<BenchResult> results)
{
    Console.Write(FormatText(new BenchReport(BenchReportMetadata.Empty, results)));
}

static string FormatText(BenchReport report)
{
    var builder = new StringBuilder();
    builder.AppendLine(".NET messaging local bench");
    if (!report.Metadata.IsEmpty)
    {
        builder.AppendLine();
        builder.AppendLine("Effective Options:");
        builder.AppendLine($"  generated_utc: {report.Metadata.GeneratedUtc:O}");
        builder.AppendLine($"  commit: {report.Metadata.Commit}");
        builder.AppendLine($"  cpu: {report.Metadata.Cpu}");
        builder.AppendLine($"  os: {report.Metadata.Os}");
        builder.AppendLine($"  dotnet_sdk: {report.Metadata.DotNetSdk}");
        builder.AppendLine($"  configuration: {report.Metadata.Configuration}");
        builder.AppendLine($"  payload_sizes: {string.Join(",", report.Metadata.PayloadSizes)}");
        builder.AppendLine($"  request_window: {report.Metadata.RequestWindow}");
        builder.AppendLine($"  send_concurrency: {report.Metadata.SendConcurrency}");
        builder.AppendLine($"  latency_sample_limit: {report.Metadata.LatencySampleLimit}");
        builder.AppendLine($"  warmup: {report.Metadata.Warmup}");
        builder.AppendLine($"  duration_seconds: {report.Metadata.DurationSeconds}");
        builder.AppendLine($"  grpc_url: {report.Metadata.GrpcUrl}");
        builder.AppendLine($"  grpc_stats_url: {report.Metadata.GrpcStatsUrl}");
        builder.AppendLine($"  zlink_endpoint: {report.Metadata.ZLinkEndpoint}");
        builder.AppendLine($"  zlink_stats_url: {report.Metadata.ZLinkStatsUrl}");
        builder.AppendLine($"  zlink_raw_endpoint: {report.Metadata.ZLinkRawEndpoint}");
        builder.AppendLine($"  zlink_raw_command_endpoint: {report.Metadata.ZLinkRawCommandEndpoint}");
        builder.AppendLine($"  raw_socket: {report.Metadata.RawSocket}");
        builder.AppendLine($"  zlink_raw_stats_url: {report.Metadata.ZLinkRawStatsUrl}");
        builder.AppendLine($"  result_json: {report.Metadata.ResultJson}");
        builder.AppendLine($"  report_txt: {report.Metadata.ReportText}");
    }

    builder.AppendLine();
    foreach (var group in report.Results.GroupBy(static result => result.Pattern))
    {
        builder.AppendLine($"  > Benchmarking current for {group.Key}...");
        builder.AppendLine("    Testing local:");
        builder.AppendLine("      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Client CPU | Client Mem | Server CPU | Server Mem |");
        builder.AppendLine("      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|");
        foreach (var result in group)
        {
            builder.AppendLine(
                $"      | {result.Implementation,-23} | {result.SizeText,-8} | {result.ThroughputText,16} | {result.BandwidthText,12} | {result.LatencyMeanText,12} | {result.LatencyP95Text,12} | {result.LatencyP99Text,12} | {result.ClientCpuText,10} | {result.ClientMemoryText,10} | {result.ServerCpuText,10} | {result.ServerMemoryText,10} |");
        }

        builder.AppendLine();
    }

    foreach (var result in report.Results)
    {
        foreach (var line in result.PerfLines)
        {
            builder.AppendLine(line);
        }
    }

    return builder.ToString();
}

static async ValueTask<BenchReportMetadata> CreateMetadataAsync(BenchOptions options)
{
    return new BenchReportMetadata(
        DateTimeOffset.UtcNow,
        CpuName(),
        RuntimeInformation.OSDescription,
        await RunShellCommandAsync("dotnet", "--version"),
        await RunShellCommandAsync("git", "rev-parse", "--short", "HEAD"),
        options.Configuration,
        options.PayloadSizes,
        options.RequestWindow,
        options.SendConcurrency,
        options.LatencySampleLimit,
        options.Warmup,
        options.DurationSeconds,
        options.CommandSettleMs,
        options.GrpcUrl,
        options.GrpcStatsUrl,
        options.ZLinkEndpoint,
        options.ZLinkStatsUrl,
        options.ZLinkRawEndpoint,
        options.ZLinkRawCommandEndpoint,
        options.ZLinkRawStatsUrl,
        options.RawSocket,
        Path.Combine(options.Output, "results.json"),
        options.ReportPath);
}

static async ValueTask<string> RunShellCommandAsync(string fileName, params string[] args)
{
    try
    {
        var startInfo = new ProcessStartInfo(fileName)
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        foreach (var arg in args)
        {
            startInfo.ArgumentList.Add(arg);
        }

        using var process = Process.Start(startInfo);
        if (process is null)
        {
            return "unknown";
        }

        var output = await process.StandardOutput.ReadToEndAsync();
        await process.WaitForExitAsync();
        return process.ExitCode == 0 ? output.Trim() : "unknown";
    }
    catch
    {
        return "unknown";
    }
}

static string CpuName()
{
    const string cpuInfo = "/proc/cpuinfo";
    if (File.Exists(cpuInfo))
    {
        var model = File.ReadLines(cpuInfo)
            .FirstOrDefault(static line => line.StartsWith("model name", StringComparison.OrdinalIgnoreCase));
        if (model is not null)
        {
            var separator = model.IndexOf(':');
            if (separator >= 0)
            {
                return model[(separator + 1)..].Trim();
            }
        }
    }

    return RuntimeInformation.ProcessArchitecture.ToString();
}

internal sealed record BenchReport(
    BenchReportMetadata Metadata,
    IReadOnlyList<BenchResult> Results);

internal readonly record struct PendingBenchRequest(
    Task<BenchPayload> Task,
    long Started,
    ulong Sequence);

internal readonly record struct PendingRawRequest(Task<long> Task);

internal sealed class RawRequestWindowMetrics
{
    private readonly int _latencySampleLimit;
    private readonly List<long> _samples;
    private long _sampleCount;
    private long _sampleSum;

    internal RawRequestWindowMetrics(int latencySampleLimit)
    {
        _latencySampleLimit = latencySampleLimit;
        _samples = new List<long>(latencySampleLimit);
    }

    internal int Completed { get; private set; }
    internal int Errors { get; private set; }
    internal double Mean => _sampleCount == 0
        ? 0
        : (double)_sampleSum / _sampleCount;

    internal void RecordSuccess(long elapsedMicros)
    {
        _sampleSum += elapsedMicros;
        _sampleCount++;
        if (_samples.Count < _latencySampleLimit)
            _samples.Add(elapsedMicros);
        Completed++;
    }

    internal void RecordError() => Errors++;

    internal void RecordErrors(int count) => Errors += count;

    internal long Percentile(double q)
    {
        if (_samples.Count == 0)
            return (long)Mean;

        _samples.Sort();
        var index = (int)Math.Min(
            _samples.Count - 1,
            q * Math.Max(0, _samples.Count - 1));
        return _samples[index];
    }
}

internal readonly record struct RequestSample(long ElapsedMicros, bool Success);

internal readonly record struct ResourceSample(TimeSpan CpuStart)
{
    public static ResourceSample Start()
    {
        return new ResourceSample(Process.GetCurrentProcess().TotalProcessorTime);
    }

    public double CpuSeconds()
    {
        return Math.Max(0, (Process.GetCurrentProcess().TotalProcessorTime - CpuStart).TotalSeconds);
    }

    public double WorkingSetMb()
    {
        return Process.GetCurrentProcess().WorkingSet64 / 1024.0 / 1024.0;
    }
}

internal sealed record BenchReportMetadata(
    DateTimeOffset GeneratedUtc,
    string Cpu,
    string Os,
    string DotNetSdk,
    string Commit,
    string Configuration,
    int[] PayloadSizes,
    int RequestWindow,
    int SendConcurrency,
    int LatencySampleLimit,
    int Warmup,
    int DurationSeconds,
    int CommandSettleMs,
    string GrpcUrl,
    string GrpcStatsUrl,
    string ZLinkEndpoint,
    string ZLinkStatsUrl,
    string ZLinkRawEndpoint,
    string ZLinkRawCommandEndpoint,
    string ZLinkRawStatsUrl,
    string RawSocket,
    string ResultJson,
    string ReportText)
{
    public static BenchReportMetadata Empty { get; } = new(
        default,
        "",
        "",
	        "",
	        "",
	        "",
	        [],
	        0,
	        0,
	        0,
	        0,
	        0,
	        0,
	        "",
	        "",
	        "",
	        "",
	        "",
	        "",
	        "",
	        "",
	        "",
	        "");

    public bool IsEmpty => GeneratedUtc == default;
}

internal sealed record BenchResult(
    string Scenario,
    string Unit,
    int PayloadSize,
    int DurationSeconds,
    long Completed,
    long Errors,
    long ServerErrors,
    int Warmup,
    double ElapsedSeconds,
    double Throughput,
    long P95Micros,
    long P99Micros,
    double MeanMicros,
    double? ServerMeanMicros,
    double? ServerP95Micros,
    double? ServerP99Micros,
    double ClientCpuSeconds,
    double ClientWorkingSetMb,
    double ServerCpuSeconds,
    double ServerWorkingSetMb)
{
    private double LatencyMeanMicros => ServerMeanMicros ?? MeanMicros;
    private double LatencyP95Micros => ServerP95Micros ?? P95Micros;
    private double LatencyP99Micros => ServerP99Micros ?? P99Micros;

    public string SizeText => $"{PayloadSize}B";
    public string ThroughputText => $"{Throughput / 1000.0:F2} {Unit}";
    public string BandwidthText => $"{Throughput * PayloadSize / 1_000_000.0:F2} MB/s";
    public string LatencyMeanText => $"{LatencyMeanMicros / 1000.0:F3} ms";
    public string LatencyP95Text => $"{LatencyP95Micros / 1000.0:F3} ms";
    public string LatencyP99Text => $"{LatencyP99Micros / 1000.0:F3} ms";
    public string ClientCpuText => $"{CpuPercent(ClientCpuSeconds):F1}%";
    public string ClientMemoryText => $"{ClientWorkingSetMb:F1} MB";
    public string ServerCpuText => $"{CpuPercent(ServerCpuSeconds):F1}%";
    public string ServerMemoryText => $"{ServerWorkingSetMb:F1} MB";
    public string Implementation
    {
        get
        {
            if (Scenario.StartsWith("grpc-dotnet-", StringComparison.Ordinal))
            {
                return "grpc-dotnet";
            }

            if (Scenario.StartsWith("zlink-framework-dotnet-", StringComparison.Ordinal))
            {
                return "zlink-framework-dotnet";
            }

            if (Scenario.StartsWith("zlink-dotnet-", StringComparison.Ordinal))
            {
                return "zlink-dotnet";
            }

            return "unknown";
        }
    }

    public string Pattern
    {
        get
        {
            return Implementation == "unknown"
                ? Scenario
                : Scenario[(Implementation.Length + 1)..];
        }
    }

    public IEnumerable<string> PerfLines
    {
        get
        {
            yield return PerfLine("throughput", Throughput);
            yield return PerfLine("bandwidth", Throughput * PayloadSize / 1_000_000.0);
            yield return PerfLine("latency", LatencyMeanMicros / 1000.0);
            yield return PerfLine("latency_p95", LatencyP95Micros / 1000.0);
            yield return PerfLine("latency_p99", LatencyP99Micros / 1000.0);
            yield return PerfLine("client_cpu_percent", CpuPercent(ClientCpuSeconds));
            yield return PerfLine("client_memory_mb", ClientWorkingSetMb);
            yield return PerfLine("server_cpu_percent", CpuPercent(ServerCpuSeconds));
            yield return PerfLine("server_memory_mb", ServerWorkingSetMb);
        }
    }

    private double CpuPercent(double cpuSeconds)
    {
        return cpuSeconds / Math.Max(0.001, ElapsedSeconds) / Environment.ProcessorCount * 100.0;
    }

    private string PerfLine(string metric, double value)
    {
        return string.Join(',',
            "RESULT",
            "current",
            Scenario,
            "local",
            PayloadSize,
            metric,
            $"{value:F3}");
    }
}

/// <summary>
///     The raw ZLink bench socket. FB-001 / bench spec 1.3 require the
///     <c>zlink-dotnet</c> row to be ROUTER-ROUTER so that
///     <c>zlink-framework-dotnet / zlink-dotnet</c> measures framework-layer cost
///     alone. The DEALER-ROUTER configuration is kept behind --raw-socket dealer
///     so both configurations remain measurable.
/// </summary>
internal readonly record struct DrainOutcome(
    BenchServerSnapshot Snapshot, double DrainMs, bool BoundHit);

internal sealed class DrainState
{
    private readonly Dictionary<string, string> _contaminated = new(StringComparer.Ordinal);
    private readonly List<string> _observations = [];

    public IReadOnlyList<string> Observations => _observations;

    public void RecordDrain(
        string implementation, string cell, double drainMs, bool boundHit, int boundMs)
    {
        _observations.Add(boundHit
            ? $"{cell}: server did NOT drain within the {boundMs} ms bound"
            : $"{cell}: server drained in {drainMs:F0} ms");
        Console.Error.WriteLine(
            $"[bench] drain {cell}: {drainMs:F0} ms bound_hit={boundHit}");
        if (boundHit)
        {
            _contaminated[implementation] =
                $"the preceding {implementation} send-saturation cell did not drain "
                + $"within the {boundMs} ms bound";
        }
    }

    /// <summary>Consumes the mark so only the next cell on that server is excluded.</summary>
    public bool TryTakeContamination(string implementation, out string reason)
    {
        if (_contaminated.TryGetValue(implementation, out reason!))
        {
            _contaminated.Remove(implementation);
            return true;
        }

        reason = "";
        return false;
    }
}

internal sealed class RawBenchSocket : IDisposable
{
    private readonly IDealerSocket? _dealer;
    private readonly IRouterSocket? _router;
    private readonly RoutingId _peer;

    private RawBenchSocket(IDealerSocket? dealer, IRouterSocket? router, RoutingId peer)
    {
        _dealer = dealer;
        _router = router;
        _peer = peer;
    }

    /// <summary>Bounded pre-warmup wait for the peer routing id to appear.</summary>
    public const int RouteReadyTimeoutSeconds = 15;

    public bool IsRouter => _router is not null;

    public static RawBenchSocket Create(
        IContext context,
        string mode,
        RoutingId self,
        RoutingId peer,
        string endpoint)
    {
        if (string.Equals(mode, "dealer", StringComparison.Ordinal))
        {
            var dealer = context.CreateDealerSocket();
            dealer.SetRoutingId(self);
            dealer.Connect(endpoint);
            return new RawBenchSocket(dealer, null, peer);
        }

        var router = context.CreateRouterSocket();
        router.SetRoutingId(self);
        router.Connect(endpoint);
        return new RawBenchSocket(null, router, peer);
    }

    public RequestOperation Request() =>
        _router is not null ? _router.Request(_peer) : _dealer!.Request();

    public SendOperation Send() =>
        _router is not null ? _router.Send(_peer) : _dealer!.Send();

    public void Dispose()
    {
        _dealer?.Dispose();
        _router?.Dispose();
    }
}

internal sealed record BenchOptions(
    string Scenario,
    string Implementation,
    int[] PayloadSizes,
    int RequestWindow,
    int SendConcurrency,
    int LatencySampleLimit,
    int Warmup,
    int DurationSeconds,
    int CommandSettleMs,
    int DrainBoundMs,
    string GrpcUrl,
    string ZLinkEndpoint,
    string GrpcStatsUrl,
    string ZLinkStatsUrl,
    string ZLinkRawEndpoint,
    string ZLinkRawCommandEndpoint,
    string ZLinkRawStatsUrl,
    string RawSocket,
    uint RunId,
    string Output,
    string ReportFile,
    string Configuration,
    TimeSpan Timeout)
{
    public string ReportPath =>
        Path.IsPathRooted(ReportFile)
            ? ReportFile
            : Path.Combine(Output, ReportFile);

    public static BenchOptions Parse(string[] args)
    {
        var reportStamp = DateTimeOffset.Now.ToString("yyyyMMdd_HHmmss");
        return new BenchOptions(
            Value(args, "--scenario") ?? "all",
            Value(args, "--implementation") ?? "all",
            ParseInts(Value(args, "--payload-sizes") ?? "1024,4096"),
            ParseInt(Value(args, "--request-window"), 100),
            ParseInt(Value(args, "--send-concurrency"), 8),
            ParseInt(Value(args, "--latency-sample-limit"), 200_000),
            ParseInt(Value(args, "--warmup"), 1000),
            ParseInt(Value(args, "--duration-seconds") ?? Value(args, "--duration"), 5),
            ParseInt(Value(args, "--command-settle-ms"), 200),
            ParseInt(Value(args, "--drain-bound-ms"), 30_000),
            Value(args, "--grpc-url") ?? "http://127.0.0.1:5071",
            Value(args, "--zlink-endpoint") ?? "tcp://127.0.0.1:5072",
            Value(args, "--grpc-stats-url") ?? "http://127.0.0.1:5074",
            Value(args, "--zlink-stats-url") ?? "http://127.0.0.1:5073",
            Value(args, "--zlink-raw-endpoint") ?? "tcp://127.0.0.1:5075",
            Value(args, "--zlink-raw-command-endpoint") ?? "tcp://127.0.0.1:5077",
            Value(args, "--zlink-raw-stats-url") ?? "http://127.0.0.1:5076",
            Value(args, "--raw-socket")
                ?? Environment.GetEnvironmentVariable("RAW_SOCKET")
                ?? "router",
            (uint)Random.Shared.Next(1, int.MaxValue),
            Value(args, "--output") ?? "log/latest",
            Value(args, "--report-file") ?? $"with_grpc_dotnet_{reportStamp}.txt",
            Value(args, "--configuration") ?? Environment.GetEnvironmentVariable("CONFIGURATION") ?? "Release",
            TimeSpan.FromSeconds(ParseInt(Value(args, "--timeout-seconds"), 300)));
    }

    public bool ShouldRunImplementation(string implementation)
    {
        return Implementation == "all"
            || string.Equals(Implementation, implementation, StringComparison.Ordinal);
    }

    public void Validate()
    {
        if (Implementation is not "all"
            and not "grpc-dotnet"
            and not "zlink-dotnet"
            and not "zlink-framework-dotnet")
        {
            throw new InvalidOperationException(
                "Implementation must be one of all, grpc-dotnet, zlink-dotnet, zlink-framework-dotnet.");
        }

        if (RawSocket is not "router" and not "dealer")
        {
            throw new InvalidOperationException("RawSocket must be router or dealer.");
        }

        if (PayloadSizes.Length == 0)
        {
            throw new InvalidOperationException("At least one payload size is required.");
        }

        if (RequestWindow <= 0)
        {
            throw new InvalidOperationException("RequestWindow must be positive.");
        }

        if (SendConcurrency <= 0)
        {
            throw new InvalidOperationException("SendConcurrency must be positive.");
        }

        if (LatencySampleLimit <= 0)
        {
            throw new InvalidOperationException("LatencySampleLimit must be positive.");
        }

        if (DurationSeconds <= 0)
        {
            throw new InvalidOperationException("DurationSeconds must be positive.");
        }

        foreach (var payloadSize in PayloadSizes)
        {
            if (payloadSize < BenchMetricHeaders.HeaderSize)
            {
                throw new InvalidOperationException(
                    $"Payload size must be at least {BenchMetricHeaders.HeaderSize} bytes.");
            }
        }

        ValidateHttpLoopback(GrpcUrl, nameof(GrpcUrl));
        ValidateHttpLoopback(GrpcStatsUrl, nameof(GrpcStatsUrl));
        ValidateHttpLoopback(ZLinkStatsUrl, nameof(ZLinkStatsUrl));
        ValidateHttpLoopback(ZLinkRawStatsUrl, nameof(ZLinkRawStatsUrl));
        ValidateTcpLoopback(ZLinkEndpoint, nameof(ZLinkEndpoint));
        ValidateTcpLoopback(ZLinkRawEndpoint, nameof(ZLinkRawEndpoint));
        ValidateTcpLoopback(ZLinkRawCommandEndpoint, nameof(ZLinkRawCommandEndpoint));
    }

    private static string? Value(string[] args, string name)
    {
        for (var i = 0; i < args.Length - 1; i++)
        {
            if (args[i] == name) return args[i + 1];
        }

        return null;
    }

    private static int ParseInt(string? value, int fallback)
    {
        return int.TryParse(value, out var parsed) ? parsed : fallback;
    }

    private static int[] ParseInts(string value)
    {
        return value.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Select(static item => int.Parse(item))
            .ToArray();
    }

    private static void ValidateHttpLoopback(string value, string name)
    {
        if (!Uri.TryCreate(value, UriKind.Absolute, out var uri)
            || uri.Scheme != Uri.UriSchemeHttp
            || !IsLoopback(uri.Host))
        {
            throw new InvalidOperationException($"{name} must be an http loopback URL.");
        }
    }

    private static void ValidateTcpLoopback(string value, string name)
    {
        if (!Uri.TryCreate(value, UriKind.Absolute, out var uri)
            || uri.Scheme != "tcp"
            || !IsLoopback(uri.Host))
        {
            throw new InvalidOperationException($"{name} must be a tcp loopback endpoint.");
        }
    }

    private static bool IsLoopback(string host)
    {
        if (host.Equals("localhost", StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }

        return IPAddress.TryParse(host, out var address) && IPAddress.IsLoopback(address);
    }
}

static class RawEnvelopeHeaders
{
    public static readonly byte[] Request = Encoding.UTF8.GetBytes(
        "{\"kind\":1,\"channelName\":\"bench\",\"messageName\":\"BenchPayload\",\"contentType\":\"application/x-protobuf\",\"correlationId\":null,\"deadline\":null,\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null}");
}

static class RawPayloadCodec
{
    public static Message Encode(int payloadSize, uint runId, BenchPhase phase, ulong sequence)
    {
        var bodySize = Math.Max(payloadSize, BenchMetricHeaders.HeaderSize);
        var encodedSize = 1 + VarintSize(bodySize) + bodySize;
        var body = Message.Allocate(encodedSize);
        try
        {
            var span = body.AsSpan();
            span[0] = 0x0a;
            var offset = WriteVarint(span[1..], bodySize) + 1;
            var payload = span[offset..(offset + bodySize)];
            BenchMetricHeaders.FillPayload(payload, runId, phase, sequence);
            return body;
        }
        catch
        {
            body.Dispose();
            throw;
        }
    }

    public static bool TryDecodeHeader(ReadOnlySpan<byte> encoded, out BenchMetricHeader header)
    {
        header = default;
        var source = encoded;
        while (!source.IsEmpty)
        {
            if (!TryReadVarint(ref source, out var key))
                return false;
            var field = key >> 3;
            var wireType = key & 0x07;
            if (wireType != 2)
                return false;
            if (!TryReadVarint(ref source, out var length) || source.Length < length)
                return false;
            var body = source[..length];
            if (field == 1)
                return BenchMetricHeaders.TryDecode(body, out header);
            source = source[length..];
        }

        return false;
    }

    private static int VarintSize(int value)
    {
        var size = 1;
        var remaining = (uint)value;
        while (remaining >= 0x80)
        {
            remaining >>= 7;
            size++;
        }

        return size;
    }

    private static int WriteVarint(Span<byte> destination, int value)
    {
        var remaining = (uint)value;
        var index = 0;
        while (remaining >= 0x80)
        {
            destination[index++] = (byte)((remaining & 0x7f) | 0x80);
            remaining >>= 7;
        }

        destination[index++] = (byte)remaining;
        return index;
    }

    private static bool TryReadVarint(ref ReadOnlySpan<byte> source, out int value)
    {
        var result = 0;
        var shift = 0;
        var span = source;
        for (var i = 0; i < span.Length && shift < 32; i++)
        {
            var b = span[i];
            result |= (b & 0x7f) << shift;
            if ((b & 0x80) == 0)
            {
                source = span[(i + 1)..];
                value = result;
                return true;
            }

            shift += 7;
        }

        value = 0;
        return false;
    }
}
