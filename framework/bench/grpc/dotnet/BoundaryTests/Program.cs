using System.Diagnostics;
using System.Net;
using Grpc.Core;
using Microsoft.AspNetCore.Server.Kestrel.Core;
using WithGrpcBench.Client;
using WithGrpcBench.Shared;

var expectedNowNs = (long)((Int128)Stopwatch.GetTimestamp() * 1_000_000_000 / Stopwatch.Frequency);
var actualNowNs = BenchMetricHeaders.NowNs();
if (actualNowNs < expectedNowNs || actualNowNs - expectedNowNs > 1_000_000_000)
    throw new InvalidOperationException("Nanosecond timestamp overflowed at the current uptime.");

foreach (
    var ticks in new[]
    {
        0L,
        long.MaxValue / 1_000_000_000,
        long.MaxValue / 1_000_000_000 + 1,
        long.MaxValue,
    }
)
{
    if (BenchMetricHeaders.NowNs(ticks, 1_000_000_000) != ticks)
        throw new InvalidOperationException($"Nanosecond boundary conversion failed: {ticks}");
}
var serverMetrics = new BenchServerMetrics();
serverMetrics.Record(BenchMetricHeaders.CreatePayloadBytes(4096, 1, BenchPhase.Warmup, 0));
if (serverMetrics.Snapshot().AllPhaseMessages != 1 || serverMetrics.Snapshot().Received != 0)
    throw new InvalidOperationException("Warmup receipt was excluded from all-phase stats.");
serverMetrics.Reset();
if (serverMetrics.Snapshot().AllPhaseMessages != 0)
    throw new InvalidOperationException("All-phase stats were not reset.");

var sourceMetrics = new SourceMetrics(10);
sourceMetrics.Reset(Stopwatch.GetTimestamp() + Stopwatch.Frequency);
sourceMetrics.Complete(sourceMetrics.Begin(), true);
if (sourceMetrics.Result().CompletedAtClose != 1)
    throw new InvalidOperationException("A completion before the deadline was excluded.");
sourceMetrics.Reset(Stopwatch.GetTimestamp());
sourceMetrics.Complete(sourceMetrics.Begin(), true);
if (sourceMetrics.Result().Completed != 1 || sourceMetrics.Result().CompletedAtClose != 0)
    throw new InvalidOperationException("A completion after the deadline entered active metrics.");

sourceMetrics.Reset(10);
var firstTerminal = sourceMetrics.Begin();
var secondTerminal = sourceMetrics.Begin();
var thirdTerminal = sourceMetrics.Begin();
sourceMetrics.Complete(firstTerminal, true, null, 1);
sourceMetrics.FreezeTerminal();
sourceMetrics.Complete(secondTerminal, true, null, 20);
sourceMetrics.Complete(thirdTerminal, false, new InvalidOperationException("late"), 21);
var terminal = sourceMetrics.Result();
if (
    terminal.Completed + terminal.Errors + terminal.Abandoned != terminal.Submitted
    || terminal.Completed != 1
    || terminal.Errors != 0
    || terminal.ErrorSummary.Count != 0
)
    throw new InvalidOperationException($"Late terminal entered snapshot: {terminal}");

AppContext.SetSwitch("System.Net.Http.SocketsHttpHandler.Http2UnencryptedSupport", true);
var grpcBuilder = WebApplication.CreateBuilder();
grpcBuilder.Logging.ClearProviders();
grpcBuilder.WebHost.ConfigureKestrel(options =>
    options.Listen(IPAddress.Loopback, 0, endpoint => endpoint.Protocols = HttpProtocols.Http2)
);
grpcBuilder.Services.AddGrpc();
await using var grpc = grpcBuilder.Build();
grpc.MapGrpcService<DelayedEcho>();
await grpc.StartAsync();

var statsBuilder = WebApplication.CreateBuilder();
statsBuilder.Logging.ClearProviders();
statsBuilder.WebHost.UseUrls("http://127.0.0.1:0");
await using var stats = statsBuilder.Build();
stats.MapPost("/bench/reset", () => Results.Ok());

// A target snapshot may lag source send completion. The source must preserve its own count.
stats.MapGet(
    "/bench/stats",
    () =>
        Results.Json(
            new
            {
                received = 1,
                activeMessages = 1,
                errors = 0,
            }
        )
);
await stats.StartAsync();

BenchMeasurement.ConfigureThreadPool(8);
foreach (var pattern in args.DefaultIfEmpty("request-serial"))
{
    string[] optionsArgs =
    [
        "--implementation",
        "grpc-dotnet",
        "--scenario",
        pattern,
        "--trigger-url",
        "http://127.0.0.1:1",
        "--stats-url",
        "http://127.0.0.1:2",
        "--target-endpoint",
        grpc.Urls.Single(),
        "--target-stats-url",
        stats.Urls.Single(),
        "--warmup-seconds",
        "2",
    ];
    var options = BenchOptions.Parse(optionsArgs);
    var metrics = new SourceMetrics(200_000);
    await using var transport = new GrpcBenchTransport(grpc.Urls.Single(), 8);
    var warmupTrigger = new BenchTriggerRequest
    {
        runId = "boundary",
        cellId = "boundary",
        pattern = pattern,
        payloadBytes = 1024,
        phase = "warmup",
        durationMs = DelayedEcho.WarmupWindowMilliseconds,
        requestWindow = 100,
        sendConcurrency = 8,
    };
    var warmupResult = await BenchMeasurement.RunActiveAsync(
        transport,
        metrics,
        options,
        warmupTrigger,
        CancellationToken.None
    );
    if (
        warmupResult.Completed == 0
        || warmupResult.CompletedAtClose == 0
        || metrics.Phase != BenchPhase.Warmup
    )
        throw new InvalidOperationException(
            "Warmup did not complete the selected pattern during the warmup window."
        );
    metrics = new SourceMetrics(200_000);
    var trigger = new BenchTriggerRequest
    {
        runId = "boundary",
        cellId = "boundary",
        pattern = pattern,
        payloadBytes = 1024,
        phase = "active",
        durationMs = 200,
        requestWindow = 100,
        sendConcurrency = 8,
    };
    var result = await BenchMeasurement.RunActiveAsync(
        transport,
        metrics,
        options,
        trigger,
        CancellationToken.None
    );
    var completed = result.Completed;
    var submitted = result.Submitted;
    var throughput = result.Throughput;
    var latency = result.MeanMicros;
    if (pattern == "send-saturation")
    {
        if (completed != submitted || submitted != 8 || throughput != 5)
            throw new InvalidOperationException(
                "Source completion replaced by target snapshot: " + result
            );
    }
    else if (completed != 1 || throughput != 0 || latency != 0)
        throw new InvalidOperationException("Drain reply entered active metrics: " + result);
    Console.WriteLine(
        $"{pattern}: completed={completed}, submitted={submitted}, active rate={throughput}, latency={latency} PASS"
    );
}

sealed class DelayedEcho : BenchService.BenchServiceBase
{
    public const int DelayMilliseconds = 500;
    public const int WarmupWorkerStartupAllowanceMilliseconds = 1_000;
    public const int WarmupWindowMilliseconds =
        DelayMilliseconds + WarmupWorkerStartupAllowanceMilliseconds;

    public override async Task<BenchPayload> Echo(BenchPayload request, ServerCallContext context)
    {
        await Task.Delay(DelayMilliseconds, context.CancellationToken);
        return request;
    }

    public override async Task<Google.Protobuf.WellKnownTypes.Empty> Command(
        BenchPayload request,
        ServerCallContext context
    )
    {
        await Task.Delay(DelayMilliseconds, context.CancellationToken);
        return new Google.Protobuf.WellKnownTypes.Empty();
    }
}
