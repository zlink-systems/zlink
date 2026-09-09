using Google.Protobuf.WellKnownTypes;
using Grpc.Core;
using Microsoft.AspNetCore.Server.Kestrel.Core;
using WithGrpcBench.Shared;

var url = ArgValue(args, "--url") ?? "http://127.0.0.1:5071";
var metricsUrl = ArgValue(args, "--metrics-url") ?? "http://127.0.0.1:5074";
var listen = new Uri(url);
var metrics = new BenchServerMetrics();

var builder = WebApplication.CreateBuilder(args);
ConfigureQuietLogging(builder);
builder.WebHost.ConfigureKestrel(options =>
{
    options.ListenAnyIP(listen.Port, endpoint =>
    {
        endpoint.Protocols = HttpProtocols.Http2;
    });
});
builder.Services.AddGrpc();
builder.Services.AddSingleton(metrics);

var app = builder.Build();
app.MapGrpcService<BenchGrpcService>();

var metricsBuilder = WebApplication.CreateBuilder(args);
ConfigureQuietLogging(metricsBuilder);
metricsBuilder.WebHost.UseUrls(metricsUrl);
metricsBuilder.Services.AddSingleton(metrics);

var metricsApp = metricsBuilder.Build();
metricsApp.MapGet("/ready", () => Results.Ok("ready"));
metricsApp.MapPost("/bench/reset", (BenchServerMetrics metrics) =>
{
    metrics.Reset();
    return Results.Ok();
});
metricsApp.MapGet("/bench/stats", (BenchServerMetrics metrics) => Results.Ok(metrics.Snapshot()));

await Task.WhenAll(app.RunAsync(), metricsApp.RunAsync());

static void ConfigureQuietLogging(WebApplicationBuilder builder)
{
    builder.Logging.ClearProviders();
    builder.Logging.AddConsole();
    builder.Logging.SetMinimumLevel(LogLevel.Warning);
}

static string? ArgValue(string[] args, string name)
{
    for (var i = 0; i < args.Length - 1; i++)
    {
        if (args[i] == name) return args[i + 1];
    }

    return null;
}

internal sealed class BenchGrpcService(BenchServerMetrics metrics) : BenchService.BenchServiceBase
{
    public override Task<BenchPayload> Echo(BenchPayload request, ServerCallContext context)
    {
        return Task.FromResult(request);
    }

    public override Task<Empty> Command(BenchPayload request, ServerCallContext context)
    {
        metrics.Record(request);
        return Task.FromResult(new Empty());
    }
}
