using System.Diagnostics;
using System.Net;
using System.Net.Http.Json;
using System.Runtime.InteropServices;
using System.Reflection;
using System.Text;
using System.Text.Json;
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
using ZLink.Framework.Perf;

AppContext.SetSwitch("System.Net.Http.SocketsHttpHandler.Http2UnencryptedSupport", true);

var options = BenchOptions.Parse(args);
options.Validate();
ConfigureThreadPool(options.SendConcurrency);
Directory.CreateDirectory(options.Output);

await using var transport = await BenchTransport.CreateAsync(options);
var metrics = new SourceMetrics(options.LatencySampleLimit);
BenchPhaseController? controller = null;
controller = new BenchPhaseController(
    () => transport.Ready,
    metrics.Snapshot,
    async (request, cancellationToken) =>
    {
        options.Validate(request);
        if (request.phase == "warmup")
        {
            await RunWarmupAsync(transport, options, request, cancellationToken);
            return;
        }

        var result = await RunActiveAsync(transport, metrics, options, request, cancellationToken);
        await WriteResultAsync(options, controller!.LastTrigger!, result);
    });

var builder = BenchHttpApplication.Builder(options.TriggerUrl, options.StatsUrl);
var app = builder.Build();
BenchHttpApplication.MapSource(app, options.TriggerUrl, options.StatsUrl, controller);
await app.RunAsync();

static void ConfigureThreadPool(int sendConcurrency)
{
    ThreadPool.GetMinThreads(out var workers, out var completionPorts);
    ThreadPool.SetMinThreads(
        Math.Max(workers, sendConcurrency + Environment.ProcessorCount * 2),
        completionPorts);
}

static async Task RunWarmupAsync(
    IBenchTransport transport,
    BenchOptions options,
    BenchTriggerRequest trigger,
    CancellationToken stopping)
{
    using var timeout = CancellationTokenSource.CreateLinkedTokenSource(stopping);
    timeout.CancelAfter(options.Timeout);
    var runId = HeaderRunId(trigger.runId);
    for (var index = 0; index < options.Warmup; index++)
    {
        var payload = BenchMetricHeaders.CreatePayload(
            trigger.payloadBytes,
            runId,
            BenchPhase.Warmup,
            (ulong)index);
        if (trigger.pattern == "send-saturation")
        {
            await transport.SendAsync(index % trigger.sendConcurrency, payload, timeout.Token);
        }
        else
        {
            var reply = await transport.RequestAsync(0, payload, timeout.Token);
            ValidateReply(reply, runId, BenchPhase.Warmup, trigger.payloadBytes, (ulong)index);
        }
    }
}

static async Task<BenchResult> RunActiveAsync(
    IBenchTransport transport,
    SourceMetrics metrics,
    BenchOptions options,
    BenchTriggerRequest trigger,
    CancellationToken stopping)
{
    using var operationCancellation = CancellationTokenSource.CreateLinkedTokenSource(stopping);
    operationCancellation.CancelAfter(options.Timeout);
    using var adminCancellation = CancellationTokenSource.CreateLinkedTokenSource(stopping);
    adminCancellation.CancelAfter(options.Timeout);
    using var http = new HttpClient { Timeout = options.Timeout };
    using (var reset = await http.PostAsync($"{options.TargetStatsUrl}/bench/reset", null, adminCancellation.Token))
    {
        reset.EnsureSuccessStatusCode();
    }

    metrics.Reset();
    var resources = ResourceSample.Start();
    var deadline = Stopwatch.GetTimestamp()
        + checked((long)(Stopwatch.Frequency * (trigger.durationMs / 1000.0)));
    var runId = HeaderRunId(trigger.runId);
    var next = new Sequence();

    switch (trigger.pattern)
    {
        case "request-serial":
            await RunRequestWorkersAsync(
                transport, metrics, trigger, runId, next, deadline, 1, 0, operationCancellation.Token);
            break;
        case "request-window":
            await RunRequestWorkersAsync(
                transport,
                metrics,
                trigger,
                runId,
                next,
                deadline,
                trigger.requestWindow,
                0,
                operationCancellation.Token);
            break;
        case "request-backpressure":
            await RunRequestBackpressureAsync(
                transport, metrics, trigger, runId, next, deadline, operationCancellation, options.DrainBoundMs);
            break;
        case "send-saturation":
            await RunSendWorkersAsync(
                transport,
                metrics,
                trigger,
                runId,
                next,
                deadline,
                trigger.sendConcurrency,
                operationCancellation.Token);
            break;
        default:
            throw new InvalidOperationException($"Unsupported pattern {trigger.pattern}.");
    }

    var elapsedSeconds = trigger.durationMs / 1000.0;
    var sourceResources = resources.Finish();
    var target = await http.GetFromJsonAsync<BenchServerSnapshot>(
                     $"{options.TargetStatsUrl}/bench/stats",
                     adminCancellation.Token)
                 ?? BenchServerSnapshot.Empty;
    var snapshot = metrics.Result();
    var send = trigger.pattern == "send-saturation";
    var completed = send ? target.Received : snapshot.Completed;
    return new BenchResult(
        $"{options.Implementation}-{trigger.pattern}",
        send ? "KMSG/s" : "KOPS",
        trigger.payloadBytes,
        elapsedSeconds,
        completed,
        snapshot.Errors,
        target.Errors,
        options.Warmup,
        completed / Math.Max(0.001, elapsedSeconds),
        snapshot.MeanMicros,
        snapshot.P95Micros,
        snapshot.P99Micros,
        send ? target.MeanMicros : null,
        send ? target.P95Micros : null,
        send ? target.P99Micros : null,
        sourceResources.CpuSeconds,
        sourceResources.WorkingSetMb,
        target.CpuSeconds,
        target.WorkingSetMb,
        snapshot.PeakInFlight,
        trigger.pattern == "request-window" ? trigger.requestWindow : null,
        snapshot.CurrentInFlight);
}

static async Task RunRequestWorkersAsync(
    IBenchTransport transport,
    SourceMetrics metrics,
    BenchTriggerRequest trigger,
    uint runId,
    Sequence next,
    long deadline,
    int workers,
    int stream,
    CancellationToken cancellationToken)
{
    var tasks = Enumerable.Range(0, workers).Select(_ => Task.Run(async () =>
    {
        while (Stopwatch.GetTimestamp() < deadline)
        {
            var sequence = next.Next();
            await ExecuteRequestAsync(
                transport, metrics, trigger, runId, stream, sequence, cancellationToken);
        }
    }, cancellationToken));
    await Task.WhenAll(tasks);
}

static async Task RunRequestBackpressureAsync(
    IBenchTransport transport,
    SourceMetrics metrics,
    BenchTriggerRequest trigger,
    uint runId,
    Sequence next,
    long deadline,
    CancellationTokenSource operationCancellation,
    int drainBoundMs)
{
    var pending = new HashSet<Task>();
    var issuedSincePump = 0;
    while (Stopwatch.GetTimestamp() < deadline)
    {
        var sequence = next.Next();
        pending.Add(ExecuteRequestAsync(
            transport, metrics, trigger, runId, 0, sequence, operationCancellation.Token));
        if (++issuedSincePump == 256)
        {
            pending.RemoveWhere(static task => task.IsCompleted);
            issuedSincePump = 0;
            await Task.Yield();
        }
    }

    if (pending.Count == 0) return;
    try
    {
        await Task.WhenAll(pending).WaitAsync(
            TimeSpan.FromMilliseconds(drainBoundMs),
            operationCancellation.Token);
    }
    catch (TimeoutException)
    {
        metrics.RecordAbandoned(metrics.Snapshot().currentInFlight);
        operationCancellation.Cancel();
        try
        {
            await Task.WhenAll(pending).WaitAsync(TimeSpan.FromSeconds(5));
        }
        catch (TimeoutException)
        {
            // The recorded abandoned count is the bounded observation result.
        }
    }
}

static async Task RunSendWorkersAsync(
    IBenchTransport transport,
    SourceMetrics metrics,
    BenchTriggerRequest trigger,
    uint runId,
    Sequence next,
    long deadline,
    int workers,
    CancellationToken cancellationToken)
{
    var tasks = Enumerable.Range(0, workers).Select(stream => Task.Run(async () =>
    {
        while (Stopwatch.GetTimestamp() < deadline)
        {
            var sequence = next.Next();
            var payload = BenchMetricHeaders.CreatePayload(
                trigger.payloadBytes,
                runId,
                BenchPhase.Active,
                sequence);
            var started = metrics.Begin();
            try
            {
                await transport.SendAsync(stream, payload, cancellationToken);
                metrics.Complete(started, true);
            }
            catch
            {
                metrics.Complete(started, false);
            }
        }
    }, cancellationToken));
    await Task.WhenAll(tasks);
}

static async Task ExecuteRequestAsync(
    IBenchTransport transport,
    SourceMetrics metrics,
    BenchTriggerRequest trigger,
    uint runId,
    int stream,
    ulong sequence,
    CancellationToken cancellationToken)
{
    var payload = BenchMetricHeaders.CreatePayload(
        trigger.payloadBytes,
        runId,
        BenchPhase.Active,
        sequence);
    var started = metrics.Begin();
    try
    {
        var reply = await transport.RequestAsync(stream, payload, cancellationToken);
        ValidateReply(reply, runId, BenchPhase.Active, trigger.payloadBytes, sequence);
        metrics.Complete(started, true);
    }
    catch
    {
        metrics.Complete(started, false);
    }
}

static void ValidateReply(
    BenchPayload reply,
    uint runId,
    BenchPhase phase,
    int payloadBytes,
    ulong sequence)
{
    BenchPayloads.Validate(reply, payloadBytes);
    if (!BenchMetricHeaders.TryDecode(reply, out var header)
        || !BenchMetricHeaders.IsExpected(header, runId, phase, payloadBytes, sequence))
    {
        throw new InvalidOperationException("Echo reply did not carry the expected metric header.");
    }
}

static uint HeaderRunId(string runId)
{
    var hash = 2166136261U;
    foreach (var value in Encoding.UTF8.GetBytes(runId))
    {
        hash = (hash ^ value) * 16777619U;
    }
    return hash == 0 ? 1U : hash;
}

static async Task WriteResultAsync(
    BenchOptions options,
    BenchTriggerObservation trigger,
    BenchResult result)
{
    var streams = StreamDescription.For(trigger.pattern, trigger.requestWindow, trigger.sendConcurrency);
    var metadata = await BenchMetadata.CreateAsync(options, trigger);
    var cellTrigger = new BenchCellTrigger(
        trigger.runId,
        trigger.cellId,
        trigger.pattern,
        trigger.payloadBytes,
        trigger.durationMs,
        options.Warmup,
        options.TriggerUrl,
        trigger.receivedAtUnixMs);
    var report = new BenchReport("with-grpc-cell-v1", metadata, [BenchCell.From(result, cellTrigger, streams)]);
    var json = JsonSerializer.Serialize(report, BenchJson.Options);
    await File.WriteAllTextAsync(Path.Combine(options.Output, "results.json"), json);
    var text = FormatText(result, metadata);
    await File.WriteAllTextAsync(options.ReportPath, text);
    Console.Write(text);
}

static string FormatText(BenchResult result, BenchMetadata metadata)
{
    var lines = new StringBuilder();
    lines.AppendLine(".NET messaging local bench (server-driven source A)");
    lines.AppendLine($"implementation: {metadata.Implementation}");
    lines.AppendLine($"pattern: {result.Pattern}");
    lines.AppendLine($"payload_size: {result.PayloadSize}");
    lines.AppendLine($"warmup: {metadata.Warmup}");
    lines.AppendLine($"duration_seconds: {result.DurationSeconds:F3}");
    foreach (var line in result.PerfLines) lines.AppendLine(line);
    return lines.ToString();
}

internal interface IBenchTransport : IAsyncDisposable
{
    bool Ready { get; }
    ValueTask ProbeAsync(CancellationToken cancellationToken);
    ValueTask<BenchPayload> RequestAsync(int stream, BenchPayload payload, CancellationToken cancellationToken);
    ValueTask SendAsync(int stream, BenchPayload payload, CancellationToken cancellationToken);
}

internal static class BenchTransport
{
    public static async Task<IBenchTransport> CreateAsync(BenchOptions options)
    {
        IBenchTransport transport = options.Implementation switch
        {
            "grpc-dotnet" => new GrpcBenchTransport(
                options.TargetEndpoint,
                options.Scenario == "send-saturation" ? options.SendConcurrency : 1),
            "zlink-dotnet" => new RawBenchTransport(options),
            "zlink-framework-dotnet" => await FrameworkBenchTransport.CreateAsync(options),
            _ => throw new InvalidOperationException($"Unknown implementation {options.Implementation}.")
        };
        await WaitForRouteAsync(transport, options.Timeout);
        return transport;
    }

    private static async Task WaitForRouteAsync(IBenchTransport transport, TimeSpan timeout)
    {
        using var cancellation = new CancellationTokenSource(timeout);
        Exception? last = null;
        while (!cancellation.IsCancellationRequested)
        {
            try
            {
                await transport.ProbeAsync(cancellation.Token);
                TransportReady.Set(transport);
                return;
            }
            catch (Exception error) when (!cancellation.IsCancellationRequested)
            {
                last = error;
                await Task.Delay(20, cancellation.Token);
            }
        }
        throw new InvalidOperationException("Target route did not become ready before the timeout.", last);
    }
}

internal static class TransportReady
{
    public static void Set(IBenchTransport transport)
    {
        switch (transport)
        {
            case GrpcBenchTransport grpc: grpc.MarkReady(); break;
            case RawBenchTransport raw: raw.MarkReady(); break;
            case FrameworkBenchTransport framework: framework.MarkReady(); break;
        }
    }
}

internal sealed class GrpcBenchTransport : IBenchTransport
{
    private readonly SocketsHttpHandler handler = new()
    {
        EnableMultipleHttp2Connections = false,
        MaxConnectionsPerServer = 1
    };
    private readonly GrpcChannel channel;
    private readonly BenchService.BenchServiceClient[] clients;

    public GrpcBenchTransport(string endpoint, int streams)
    {
        channel = GrpcChannel.ForAddress(endpoint, new GrpcChannelOptions { HttpHandler = handler });
        clients = Enumerable.Range(0, Math.Max(1, streams))
            .Select(_ => new BenchService.BenchServiceClient(channel))
            .ToArray();
    }

    public bool Ready { get; private set; }
    public void MarkReady() => Ready = true;

    public async ValueTask ProbeAsync(CancellationToken cancellationToken)
    {
        var payload = BenchMetricHeaders.CreatePayload(1024, 1, BenchPhase.Warmup, 0);
        var reply = await RequestAsync(0, payload, cancellationToken);
        if (!BenchMetricHeaders.TryDecode(reply, out var header)
            || !BenchMetricHeaders.IsExpected(header, 1, BenchPhase.Warmup, 1024, 0))
            throw new InvalidOperationException("gRPC target probe returned an invalid payload.");
    }

    public async ValueTask<BenchPayload> RequestAsync(
        int stream, BenchPayload payload, CancellationToken cancellationToken) =>
        await clients[stream % clients.Length].EchoAsync(payload, cancellationToken: cancellationToken);

    public async ValueTask SendAsync(int stream, BenchPayload payload, CancellationToken cancellationToken) =>
        await clients[stream % clients.Length].CommandAsync(payload, cancellationToken: cancellationToken);

    public ValueTask DisposeAsync()
    {
        channel.Dispose();
        handler.Dispose();
        return ValueTask.CompletedTask;
    }
}

internal sealed class FrameworkBenchTransport : IBenchTransport
{
    private readonly IHost host;
    private readonly IZLinkRouteClient client;

    private FrameworkBenchTransport(IHost host, IZLinkRouteClient client)
    {
        this.host = host;
        this.client = client;
    }

    public bool Ready { get; private set; }
    public void MarkReady() => Ready = true;

    public async ValueTask ProbeAsync(CancellationToken cancellationToken)
    {
        var payload = BenchMetricHeaders.CreatePayload(1024, 1, BenchPhase.Warmup, 0);
        var reply = await RequestAsync(0, payload, cancellationToken);
        if (!BenchMetricHeaders.TryDecode(reply, out var header)
            || !BenchMetricHeaders.IsExpected(header, 1, BenchPhase.Warmup, 1024, 0))
            throw new InvalidOperationException("Framework target probe returned an invalid payload.");
    }

    public static async Task<FrameworkBenchTransport> CreateAsync(BenchOptions options)
    {
        var builder = Host.CreateApplicationBuilder(Array.Empty<string>());
        builder.Logging.ClearProviders();
        builder.Logging.AddConsole();
        builder.Logging.SetMinimumLevel(LogLevel.Warning);
        builder.Services.AddZLinkFramework(framework =>
        {
            framework.Codecs.Use(ZLinkProtobufCodec.Default);
            var mesh = framework.AddRouteMesh("bench")
                .Listen("tcp://127.0.0.1:0")
                .SetRoutingId(RoutingId.From($"bench-source-{Environment.ProcessId}"));
            mesh.Channel("bench").Client();
            mesh.PeerConnections.Connect(RoutingId.From("bench-server"), options.TargetEndpoint);
        });
        var host = builder.Build();
        await host.StartAsync();
        return new FrameworkBenchTransport(
            host,
            host.Services.GetRequiredService<IZLinkRouteClient>());
    }

    public async ValueTask<BenchPayload> RequestAsync(
        int stream, BenchPayload payload, CancellationToken cancellationToken) =>
        await client.RequestToChannel("bench", payload).Async<BenchPayload>(cancellationToken);

    public async ValueTask SendAsync(int stream, BenchPayload payload, CancellationToken cancellationToken) =>
        await client.SendToChannel("bench", payload).Async(cancellationToken);

    public async ValueTask DisposeAsync()
    {
        await host.StopAsync();
        host.Dispose();
    }
}

internal sealed class RawBenchTransport : IBenchTransport
{
    private readonly IContext context;
    private readonly RawBenchSocket? request;
    private readonly RawBenchSocket[] commands;

    public RawBenchTransport(BenchOptions options)
    {
        context = Systems.Zlink.Zlink.CreateContext();
        if (options.Scenario == "send-saturation")
        {
            commands = Enumerable.Range(0, options.SendConcurrency).Select(index => RawBenchSocket.Create(
                context,
                options.RawSocket,
                RoutingId.From($"bench-send-{Environment.ProcessId}-{index}"),
                RoutingId.From(BenchRoutingIds.RawCommandServer),
                options.TargetCommandEndpoint!)).ToArray();
        }
        else
        {
            request = RawBenchSocket.Create(
                context,
                options.RawSocket,
                RoutingId.From($"bench-request-{Environment.ProcessId}"),
                RoutingId.From(BenchRoutingIds.RawRequestServer),
                options.TargetEndpoint);
            commands = [];
        }
    }

    public bool Ready { get; private set; }
    public void MarkReady() => Ready = true;

    public async ValueTask ProbeAsync(CancellationToken cancellationToken)
    {
        var payload = BenchMetricHeaders.CreatePayload(1024, 1, BenchPhase.Warmup, 0);
        if (commands.Length > 0)
        {
            await SendAsync(0, payload, cancellationToken);
            return;
        }
        var reply = await RequestAsync(0, payload, cancellationToken);
        if (!BenchMetricHeaders.TryDecode(reply, out var header)
            || !BenchMetricHeaders.IsExpected(header, 1, BenchPhase.Warmup, 1024, 0))
            throw new InvalidOperationException("Raw target probe returned an invalid payload.");
    }

    public async ValueTask<BenchPayload> RequestAsync(
        int stream, BenchPayload payload, CancellationToken cancellationToken)
    {
        Message? header = null;
        Message? body = null;
        IReadOnlyList<Message>? parts = null;
        try
        {
            header = Message.From(RawEnvelopeHeaders.Request);
            body = EncodeRawPayload(payload);
            RequestSubmission submission = await request!.SubmitRequestAsync(
                header, body, cancellationToken);
            parts = await submission.Reply;
            if (parts.Count == 0) throw new InvalidOperationException("Raw request returned no reply parts.");
            return RawWire.Decode(parts.Count == 1 ? parts[0].AsReadOnlySpan() : parts[^1].AsReadOnlySpan());
        }
        finally
        {
            if (parts is not null) foreach (var part in parts) part.Dispose();
            body?.Dispose();
            header?.Dispose();
        }
    }

    public async ValueTask SendAsync(int stream, BenchPayload payload, CancellationToken cancellationToken)
    {
        Message? header = null;
        Message? body = null;
        try
        {
            header = Message.From(RawEnvelopeHeaders.Request);
            body = EncodeRawPayload(payload);
            await commands[stream % commands.Length].SubmitSendAsync(
                header, body, cancellationToken);
        }
        finally
        {
            body?.Dispose();
            header?.Dispose();
        }
    }

    public ValueTask DisposeAsync()
    {
        request?.Dispose();
        foreach (var command in commands) command.Dispose();
        context.Dispose();
        return ValueTask.CompletedTask;
    }

    private static Message EncodeRawPayload(BenchPayload payload)
    {
        var body = Message.Allocate(payload.CalculateSize());
        try
        {
            RawWire.Encode(payload, body.AsSpan());
            return body;
        }
        catch
        {
            body.Dispose();
            throw;
        }
    }
}

internal sealed class RawBenchSocket : IDisposable
{
    private readonly IDealerSocket? dealer;
    private readonly IRouterSocket? router;
    private readonly RoutingId peer;
    private readonly SemaphoreSlim submissionGate = new(1, 1);

    private RawBenchSocket(IDealerSocket? dealer, IRouterSocket? router, RoutingId peer)
    {
        this.dealer = dealer;
        this.router = router;
        this.peer = peer;
    }

    public static RawBenchSocket Create(
        IContext context,
        string mode,
        RoutingId self,
        RoutingId peer,
        string endpoint)
    {
        if (mode == "dealer")
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

    public RequestOperation Request() => router is null ? dealer!.Request() : router.Request(peer);
    public SendOperation Send() => router is null ? dealer!.Send() : router.Send(peer);

    public async ValueTask<RequestSubmission> SubmitRequestAsync(
        Message header, Message body, CancellationToken cancellationToken)
    {
        await submissionGate.WaitAsync(cancellationToken);
        try
        {
            RequestSubmission submission = Request().Message(header)
                .Message(body).Async(cancellationToken);
            if (submission.Result == SubmitResult.Backpressured)
                await submission.Admitted;
            return submission;
        }
        finally
        {
            submissionGate.Release();
        }
    }

    public async ValueTask SubmitSendAsync(Message header, Message body,
        CancellationToken cancellationToken)
    {
        await submissionGate.WaitAsync(cancellationToken);
        try
        {
            SendSubmission submission = Send().Message(header).Message(body)
                .Async(cancellationToken);
            if (submission.Result == SubmitResult.Backpressured)
                await submission.Admitted;
        }
        finally
        {
            submissionGate.Release();
        }
    }

    public void Dispose()
    {
        dealer?.Dispose();
        router?.Dispose();
        submissionGate.Dispose();
    }
}

internal sealed class SourceMetrics(int sampleLimit)
{
    private readonly object gate = new();
    private readonly List<long> samples = new(sampleLimit);
    private long submitted;
    private long completed;
    private long errors;
    private long inFlight;
    private long peakInFlight;
    private long abandoned;
    private long sampleCount;
    private long sampleSum;

    public void Reset()
    {
        lock (gate)
        {
            submitted = completed = errors = inFlight = peakInFlight = abandoned = 0;
            sampleCount = sampleSum = 0;
            samples.Clear();
        }
    }

    public long Begin()
    {
        var started = Stopwatch.GetTimestamp();
        lock (gate)
        {
            submitted++;
            inFlight++;
            peakInFlight = Math.Max(peakInFlight, inFlight);
        }
        return started;
    }

    public void Complete(long started, bool success)
    {
        var elapsed = (long)((Stopwatch.GetTimestamp() - started) * 1_000_000.0 / Stopwatch.Frequency);
        lock (gate)
        {
            inFlight--;
            if (success) completed++; else errors++;
            sampleSum += elapsed;
            sampleCount++;
            if (samples.Count < sampleLimit) samples.Add(elapsed);
        }
    }

    public void RecordAbandoned(long count)
    {
        lock (gate) abandoned = Math.Max(abandoned, count);
    }

    public BenchCounterSnapshot Snapshot()
    {
        lock (gate) return new(submitted, completed, errors, 0, inFlight, peakInFlight);
    }

    public SourceResultSnapshot Result()
    {
        lock (gate)
        {
            var sorted = samples.ToArray();
            Array.Sort(sorted);
            return new SourceResultSnapshot(
                completed,
                errors,
                inFlight,
                peakInFlight,
                abandoned,
                sampleCount == 0 ? 0 : (double)sampleSum / sampleCount,
                Percentile(sorted, 0.95),
                Percentile(sorted, 0.99));
        }
    }

    private static long Percentile(long[] sorted, double percentile)
    {
        if (sorted.Length == 0) return 0;
        return sorted[Math.Clamp((int)Math.Ceiling(percentile * sorted.Length) - 1, 0, sorted.Length - 1)];
    }
}

internal sealed class Sequence
{
    private long value = -1;
    public ulong Next() => (ulong)Interlocked.Increment(ref value);
}

internal readonly record struct SourceResultSnapshot(
    long Completed,
    long Errors,
    long CurrentInFlight,
    long PeakInFlight,
    long Abandoned,
    double MeanMicros,
    long P95Micros,
    long P99Micros);

internal readonly record struct ResourceSample(TimeSpan CpuStart)
{
    public static ResourceSample Start() => new(Process.GetCurrentProcess().TotalProcessorTime);
    public ResourceResult Finish()
    {
        var process = Process.GetCurrentProcess();
        return new ResourceResult(
            Math.Max(0, (process.TotalProcessorTime - CpuStart).TotalSeconds),
            process.WorkingSet64 / 1024.0 / 1024.0);
    }
}

internal readonly record struct ResourceResult(double CpuSeconds, double WorkingSetMb);

internal sealed record StreamDescription(int count, int? inFlightPerStream, string implementation)
{
    public static StreamDescription For(string pattern, int requestWindow, int sendConcurrency) => pattern switch
    {
        "request-serial" => new(1, 1, ".NET Task; one sequential loop"),
        "request-window" => new(1, requestWindow, ".NET Tasks sharing one logical-stream window"),
        "request-backpressure" => new(1, null, ".NET Tasks submitted without an application in-flight cap"),
        "send-saturation" => new(sendConcurrency, 1, ".NET Task per logical stream"),
        _ => throw new InvalidOperationException($"Unknown pattern {pattern}.")
    };
}

internal sealed record BenchReport(
    string schema,
    BenchMetadata metadata,
    IReadOnlyList<BenchCell> cells);

internal sealed record BenchCellTrigger(
    string runId,
    string cellId,
    string pattern,
    int payloadBytes,
    int durationMs,
    int warmup,
    string endpoint,
    long receivedAtUnixMs);

internal sealed record BenchCell(
    string implementation,
    string pattern,
    int payload_size,
    string role,
    BenchCellTrigger trigger,
    StreamDescription streams,
    object? target_stats,
    long completed,
    long errors,
    long server_errors,
    double throughput_per_second,
    double bandwidth_mb_s,
    double latency_mean_ms,
    double latency_p95_ms,
    double latency_p99_ms,
    double client_cpu_percent,
    double client_memory_mb,
    double server_cpu_percent,
    double server_memory_mb,
    double client_cores,
    int client_parallelism_ceiling,
    string client_saturation_metric,
    long peak_in_flight,
    int? request_window,
    long abandoned,
    long? server_received_at_close)
{
    public static BenchCell From(
        BenchResult result,
        BenchCellTrigger trigger,
        StreamDescription streams) => new(
        result.Implementation,
        result.Pattern,
        result.PayloadSize,
        "source",
        trigger,
        streams,
        null,
        result.Completed,
        result.Errors,
        result.ServerErrors,
        result.Throughput,
        result.Throughput * result.PayloadSize / 1_000_000.0,
        result.EffectiveMeanMicros / 1000.0,
        result.EffectiveP95Micros / 1000.0,
        result.EffectiveP99Micros / 1000.0,
        result.CpuPercent(result.ClientCpuSeconds),
        result.ClientWorkingSetMb,
        result.CpuPercent(result.ServerCpuSeconds),
        result.ServerWorkingSetMb,
        result.ClientCores,
        result.ClientParallelismCeiling,
        "client_cores",
        result.PeakInFlight,
        result.RequestWindow,
        result.Abandoned,
        result.Pattern == "send-saturation" ? result.Completed : null);
}

internal sealed record BenchMetadata(
    DateTimeOffset GeneratedUtc,
    string Cpu,
    string Os,
    string DotNetSdk,
    string DotNetRuntime,
    string GrpcVersion,
    string ZLinkVersion,
    string Commit,
    string Configuration,
    string Implementation,
    int Warmup,
    int RequestWindow,
    int SendConcurrency,
    int LatencySampleLimit,
    int DrainBoundMs,
    string TriggerUrl,
    string StatsUrl,
    string TargetEndpoint,
    string? TargetCommandEndpoint,
    string TargetStatsUrl,
    string GrpcServerConfiguration,
    string RawSocket,
    string ResultJson,
    string ReportText)
{
    public int LogicalCores => Environment.ProcessorCount;
    public int ClientParallelismCeiling => Environment.ProcessorCount;

    public static async Task<BenchMetadata> CreateAsync(
        BenchOptions options,
        BenchTriggerObservation trigger) => new(
        DateTimeOffset.UtcNow,
        CpuName(),
        RuntimeInformation.OSDescription,
        await RunCommandAsync("dotnet", "--version"),
        Environment.Version.ToString(),
        InformationalVersion(typeof(GrpcChannel).Assembly),
        typeof(Systems.Zlink.Zlink).Assembly.GetName().Version?.ToString() ?? "unknown",
        await RunCommandAsync("git", "rev-parse", "--short", "HEAD"),
        options.Configuration,
        options.Implementation,
        options.Warmup,
        trigger.requestWindow,
        trigger.sendConcurrency,
        options.LatencySampleLimit,
        options.DrainBoundMs,
        options.TriggerUrl,
        options.StatsUrl,
        options.TargetEndpoint,
        options.TargetCommandEndpoint,
        options.TargetStatsUrl,
        "ASP.NET Core gRPC on Kestrel HTTP/2 with language defaults; one channel/connection",
        options.RawSocket,
        Path.Combine(options.Output, "results.json"),
        options.ReportPath);

    private static async Task<string> RunCommandAsync(string fileName, params string[] args)
    {
        try
        {
            var info = new ProcessStartInfo(fileName)
            {
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            foreach (var arg in args) info.ArgumentList.Add(arg);
            using var process = Process.Start(info);
            if (process is null) return "unknown";
            var output = await process.StandardOutput.ReadToEndAsync();
            await process.WaitForExitAsync();
            return process.ExitCode == 0 ? output.Trim() : "unknown";
        }
        catch
        {
            return "unknown";
        }
    }

    private static string InformationalVersion(Assembly assembly) =>
        assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion
        ?? assembly.GetName().Version?.ToString()
        ?? "unknown";

    private static string CpuName()
    {
        const string cpuInfo = "/proc/cpuinfo";
        if (!File.Exists(cpuInfo)) return RuntimeInformation.ProcessArchitecture.ToString();
        var line = File.ReadLines(cpuInfo)
            .FirstOrDefault(static value => value.StartsWith("model name", StringComparison.OrdinalIgnoreCase));
        var separator = line?.IndexOf(':') ?? -1;
        return separator >= 0 ? line![(separator + 1)..].Trim() : RuntimeInformation.ProcessArchitecture.ToString();
    }
}

internal sealed record BenchResult(
    string Scenario,
    string Unit,
    int PayloadSize,
    double DurationSeconds,
    long Completed,
    long Errors,
    long ServerErrors,
    int Warmup,
    double Throughput,
    double MeanMicros,
    long P95Micros,
    long P99Micros,
    double? ServerMeanMicros,
    double? ServerP95Micros,
    double? ServerP99Micros,
    double ClientCpuSeconds,
    double ClientWorkingSetMb,
    double ServerCpuSeconds,
    double ServerWorkingSetMb,
    long PeakInFlight,
    int? RequestWindow,
    long Abandoned)
{
    public string Pattern => Scenario[(Implementation.Length + 1)..];
    public string Implementation => Scenario.StartsWith("zlink-framework-dotnet-", StringComparison.Ordinal)
        ? "zlink-framework-dotnet"
        : Scenario.StartsWith("zlink-dotnet-", StringComparison.Ordinal)
            ? "zlink-dotnet"
            : "grpc-dotnet";
    public double EffectiveMeanMicros => ServerMeanMicros ?? MeanMicros;
    public double EffectiveP95Micros => ServerP95Micros ?? P95Micros;
    public double EffectiveP99Micros => ServerP99Micros ?? P99Micros;
    public double ClientCores => ClientCpuSeconds / Math.Max(0.001, DurationSeconds);
    public int ClientParallelismCeiling => Environment.ProcessorCount;

    public IEnumerable<string> PerfLines
    {
        get
        {
            yield return Line("throughput", Throughput);
            yield return Line("bandwidth", Throughput * PayloadSize / 1_000_000.0);
            yield return Line("latency", EffectiveMeanMicros / 1000.0);
            yield return Line("latency_p95", EffectiveP95Micros / 1000.0);
            yield return Line("latency_p99", EffectiveP99Micros / 1000.0);
            yield return Line("client_cpu_percent", CpuPercent(ClientCpuSeconds));
            yield return Line("client_memory_mb", ClientWorkingSetMb);
            yield return Line("server_cpu_percent", CpuPercent(ServerCpuSeconds));
            yield return Line("server_memory_mb", ServerWorkingSetMb);
        }
    }

    public double CpuPercent(double seconds) =>
        seconds / Math.Max(0.001, DurationSeconds) / Environment.ProcessorCount * 100.0;
    private string Line(string metric, double value) =>
        $"RESULT,current,{Scenario},local,{PayloadSize},{metric},{value:F3}";
}

internal sealed record BenchOptions(
    string Implementation,
    string Scenario,
    int PayloadSize,
    int RequestWindow,
    int SendConcurrency,
    int LatencySampleLimit,
    int Warmup,
    int DrainBoundMs,
    string TriggerUrl,
    string StatsUrl,
    string TargetEndpoint,
    string? TargetCommandEndpoint,
    string TargetStatsUrl,
    string RawSocket,
    string Output,
    string ReportFile,
    string Configuration,
    TimeSpan Timeout)
{
    public string ReportPath => Path.IsPathRooted(ReportFile) ? ReportFile : Path.Combine(Output, ReportFile);

    public static BenchOptions Parse(string[] args) => new(
        Value(args, "--implementation") ?? throw new ArgumentException("--implementation is required."),
        Value(args, "--scenario") ?? throw new ArgumentException("--scenario is required."),
        ParseInt(Value(args, "--payload-size"), 1024),
        ParseInt(Value(args, "--request-window"), 100),
        ParseInt(Value(args, "--send-concurrency"), 8),
        ParseInt(Value(args, "--latency-sample-limit"), 200_000),
        ParseInt(Value(args, "--warmup"), 1000),
        ParseInt(Value(args, "--drain-bound-ms"), 30_000),
        Value(args, "--trigger-url") ?? throw new ArgumentException("--trigger-url is required."),
        Value(args, "--stats-url") ?? throw new ArgumentException("--stats-url is required."),
        Value(args, "--target-endpoint") ?? throw new ArgumentException("--target-endpoint is required."),
        Value(args, "--target-command-endpoint"),
        Value(args, "--target-stats-url") ?? throw new ArgumentException("--target-stats-url is required."),
        Value(args, "--raw-socket") ?? Environment.GetEnvironmentVariable("RAW_SOCKET") ?? "router",
        Value(args, "--output") ?? "log/latest",
        Value(args, "--report-file") ?? "report.txt",
        Value(args, "--configuration") ?? Environment.GetEnvironmentVariable("CONFIGURATION") ?? "Release",
        TimeSpan.FromSeconds(ParseInt(Value(args, "--timeout-seconds"), 300)));

    public void Validate()
    {
        if (Implementation is not ("grpc-dotnet" or "zlink-dotnet" or "zlink-framework-dotnet"))
            throw new InvalidOperationException("Unknown implementation.");
        if (Scenario is not ("request-serial" or "request-window" or "request-backpressure" or "send-saturation"))
            throw new InvalidOperationException("Unknown scenario.");
        if (PayloadSize < BenchMetricHeaders.HeaderSize || RequestWindow <= 0 || SendConcurrency <= 0
            || LatencySampleLimit <= 0 || Warmup < 0 || DrainBoundMs <= 0 || Timeout <= TimeSpan.Zero)
            throw new InvalidOperationException("Numeric benchmark options are invalid.");
        if (RawSocket is not ("router" or "dealer")) throw new InvalidOperationException("RAW_SOCKET must be router or dealer.");
        ValidateHttp(TriggerUrl, nameof(TriggerUrl));
        ValidateHttp(StatsUrl, nameof(StatsUrl));
        ValidateHttp(TargetStatsUrl, nameof(TargetStatsUrl));
        if (Implementation == "grpc-dotnet") ValidateHttp(TargetEndpoint, nameof(TargetEndpoint));
        else ValidateTcp(TargetEndpoint, nameof(TargetEndpoint));
        if (Implementation == "zlink-dotnet")
        {
            if (TargetCommandEndpoint is null) throw new InvalidOperationException("Raw target command endpoint is required.");
            ValidateTcp(TargetCommandEndpoint, nameof(TargetCommandEndpoint));
        }
    }

    public void Validate(BenchTriggerRequest request)
    {
        if (request.pattern != Scenario || request.payloadBytes != PayloadSize
            || request.requestWindow != RequestWindow || request.sendConcurrency != SendConcurrency)
            throw new InvalidOperationException("Trigger values do not match the runner-owned cell configuration.");
    }

    private static string? Value(string[] args, string name)
    {
        for (var index = 0; index < args.Length - 1; index++) if (args[index] == name) return args[index + 1];
        return null;
    }
    private static int ParseInt(string? value, int fallback) => int.TryParse(value, out var parsed) ? parsed : fallback;
    private static void ValidateHttp(string value, string name)
    {
        if (!Uri.TryCreate(value, UriKind.Absolute, out var uri) || uri.Scheme != "http" || !IsLoopback(uri.Host))
            throw new InvalidOperationException($"{name} must be an HTTP loopback URL.");
    }
    private static void ValidateTcp(string value, string name)
    {
        if (!Uri.TryCreate(value, UriKind.Absolute, out var uri) || uri.Scheme != "tcp" || !IsLoopback(uri.Host))
            throw new InvalidOperationException($"{name} must be a TCP loopback endpoint.");
    }
    private static bool IsLoopback(string host) => host.Equals("localhost", StringComparison.OrdinalIgnoreCase)
        || IPAddress.TryParse(host, out var address) && IPAddress.IsLoopback(address);
}

internal static class BenchJson
{
    public static JsonSerializerOptions Options { get; } = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
    };
}

internal static class RawEnvelopeHeaders
{
    public static readonly byte[] Request = Encoding.UTF8.GetBytes(
        "{\"kind\":1,\"channelName\":\"bench\",\"messageName\":\"BenchPayload\",\"contentType\":\"application/x-protobuf\",\"correlationId\":null,\"deadline\":null,\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null}");
}
