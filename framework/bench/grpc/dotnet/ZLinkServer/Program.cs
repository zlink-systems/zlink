using WithGrpcBench.Shared;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Contracts.Handlers;

var endpoint = ArgValue(args, "--endpoint") ?? "tcp://127.0.0.1:5072";
var metricsUrl = ArgValue(args, "--metrics-url") ?? "http://127.0.0.1:5073";

var builder = WebApplication.CreateBuilder(args);
ConfigureQuietLogging(builder);
builder.WebHost.UseUrls(metricsUrl);
builder.Services.AddSingleton<BenchServerMetrics>();
builder.Services.AddZLinkFramework(framework =>
{
    framework.Codecs.Use(ZLinkProtobufCodec.Default);
    var mesh = framework.AddRouteMesh("bench")
        .Listen(endpoint)
        .SetRoutingId(RoutingId.From("bench-server"));
    mesh.Channel("bench").Server()
        .AddRequestHandler<EchoHandler, BenchPayload, BenchPayload>("BenchPayload")
        .AddSendHandler<CommandHandler, BenchPayload>("BenchPayload");
});

var app = builder.Build();
app.MapGet("/ready", () => Results.Ok("ready"));
app.MapPost("/bench/reset", (BenchServerMetrics metrics) =>
{
    metrics.Reset();
    return Results.Ok();
});
app.MapGet("/bench/stats", (BenchServerMetrics metrics) => Results.Ok(metrics.Snapshot()));
await app.RunAsync();

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

internal sealed class EchoHandler : IZLinkRequestHandler<BenchPayload, BenchPayload>
{
    public ValueTask<BenchPayload> HandleAsync(
        BenchPayload request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        return ValueTask.FromResult(request);
    }
}

internal sealed class CommandHandler(BenchServerMetrics metrics) : IZLinkSendHandler<BenchPayload>
{
    public ValueTask HandleAsync(
        BenchPayload message,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        metrics.Record(message);
        return ValueTask.CompletedTask;
    }
}
