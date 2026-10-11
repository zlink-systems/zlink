using System.Net;
using System.Reflection;
using Grpc.Core;
using Microsoft.AspNetCore.Server.Kestrel.Core;
using WithGrpcBench.Client;
using WithGrpcBench.Shared;

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

var assembly = Assembly.LoadFrom(Path.GetFullPath(args[0]));
var program = assembly.EntryPoint!.DeclaringType!;
var runActive = program
    .GetMethods(BindingFlags.NonPublic | BindingFlags.Static)
    .Single(method => method.Name.Contains("g__RunActiveAsync|", StringComparison.Ordinal));
var configure = program
    .GetMethods(BindingFlags.NonPublic | BindingFlags.Static)
    .Single(method => method.Name.Contains("g__ConfigureThreadPool|", StringComparison.Ordinal));
configure.Invoke(null, [8]);
foreach (var pattern in args.Skip(1).DefaultIfEmpty("request-serial"))
{
    var optionsType = assembly.GetType("BenchOptions", throwOnError: true)!;
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
        "--warmup",
        "0",
    ];
    var options = optionsType.GetMethod("Parse")!.Invoke(null, [optionsArgs]);
    var metrics = Activator.CreateInstance(assembly.GetType("SourceMetrics")!, [200_000]);
    await using var transport = (IAsyncDisposable)
        Activator.CreateInstance(assembly.GetType("GrpcBenchTransport")!, [grpc.Urls.Single(), 8])!;
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
    var pending = (Task)
        runActive.Invoke(null, [transport, metrics, options, trigger, CancellationToken.None])!;
    await pending;
    var result = pending.GetType().GetProperty("Result")!.GetValue(pending)!;
    var completed = (long)result.GetType().GetProperty("Completed")!.GetValue(result)!;
    var submitted = (long)result.GetType().GetProperty("Submitted")!.GetValue(result)!;
    var throughput = (double)result.GetType().GetProperty("Throughput")!.GetValue(result)!;
    var latency = (double)result.GetType().GetProperty("MeanMicros")!.GetValue(result)!;
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
    public override async Task<BenchPayload> Echo(BenchPayload request, ServerCallContext context)
    {
        await Task.Delay(500, context.CancellationToken);
        return request;
    }

    public override async Task<Google.Protobuf.WellKnownTypes.Empty> Command(
        BenchPayload request,
        ServerCallContext context
    )
    {
        await Task.Delay(500, context.CancellationToken);
        return new Google.Protobuf.WellKnownTypes.Empty();
    }
}
