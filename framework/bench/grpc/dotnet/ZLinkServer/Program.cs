using Google.Protobuf;
using Systems.Zlink;
using WithGrpcBench.Shared;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Dispatch;

var endpoint = ArgValue(args, "--endpoint") ?? "tcp://127.0.0.1:5214";
var metricsUrl = ArgValue(args, "--metrics-url") ?? "http://127.0.0.1:5215";

var builder = WebApplication.CreateBuilder(args);
ConfigureQuietLogging(builder);
builder.WebHost.UseUrls(metricsUrl);
builder.Services.AddSingleton<BenchServerMetrics>();
#if !ZLINK_DOTNET_LADDER_L0
builder.Services.AddZLinkFramework(framework =>
{
    framework.Codecs.Use(ZLinkProtobufCodec.Default);
    if (BenchDiagnostics.FlowTraceEnabled)
        framework.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
    var mesh = framework
        .AddRouteMesh("bench")
        .Listen(endpoint)
        .SetRoutingId(RoutingId.From("bench-server"));
    mesh.AddRouteRequestHandler<EchoHandler, BenchPayload, BenchPayload>("BenchPayload")
        .AddRouteSendHandler<CommandHandler, BenchPayload>("BenchPayload");
});
#endif

var app = builder.Build();
var metrics = app.Services.GetRequiredService<BenchServerMetrics>();
app.MapGet("/ready", () => Results.Ok("ready"));
app.MapPost(
    "/bench/reset",
    () =>
    {
        metrics.Reset();
        return Results.Ok();
    }
);
app.MapGet("/bench/stats", () => Results.Ok(metrics.Snapshot()));
#if ZLINK_DOTNET_LADDER_L0
using var context = Systems.Zlink.Zlink.CreateContext();
using var router = context.CreateRouterSocket();
router.SetRoutingId(RoutingId.From("bench-server"));
router.Bind(endpoint);
var handler = new L0BenchHandler(metrics);
var receiver = Task.Run(() => RunL0Owner(router, handler, metrics, app.Lifetime.ApplicationStopping));
try
{
    await app.RunAsync();
}
finally
{
    context.Shutdown();
    await receiver;
}
#else
await app.RunAsync();
#endif

static void ConfigureQuietLogging(WebApplicationBuilder builder)
{
    builder.Logging.ClearProviders();
    builder.Logging.AddConsole();
    builder.Logging.SetMinimumLevel(
        BenchDiagnostics.FlowTraceEnabled ? LogLevel.Information : LogLevel.Warning
    );
}

static string? ArgValue(string[] args, string name)
{
    for (var i = 0; i < args.Length - 1; i++)
    {
        if (args[i] == name)
            return args[i + 1];
    }

    return null;
}

internal sealed class EchoHandler(BenchServerMetrics metrics)
    : IZLinkRouteRequestHandler<BenchPayload, BenchPayload>
{
    public ValueTask<BenchPayload> HandleAsync(
        BenchPayload request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken
    )
    {
        metrics.RecordReceived(request);
        return ValueTask.FromResult(request);
    }
}

internal sealed class CommandHandler(BenchServerMetrics metrics)
    : IZLinkRouteSendHandler<BenchPayload>
{
    public ValueTask HandleAsync(
        BenchPayload message,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken
    )
    {
        metrics.Record(message);
        return ValueTask.CompletedTask;
    }
}

#if ZLINK_DOTNET_LADDER_L0
internal sealed class L0BenchHandler(BenchServerMetrics metrics)
    : IZLinkRouteRequestHandler<Message, Message>, IZLinkRouteSendHandler<Message>
{
    public ValueTask<Message> HandleAsync(
        Message request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken
    ) => ValueTask.FromResult(HandleRequest(request));

    public ValueTask HandleAsync(
        Message message,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken
    )
    {
        HandleSend(message);
        return ValueTask.CompletedTask;
    }

    public Message HandleRequest(Message request)
    {
        var payload = BenchPayload.Parser.ParseFrom(request.AsReadOnlySpan());
        metrics.RecordReceived(payload);
        var reply = new BenchPayload { Body = payload.Body };
        var encoded = Message.Allocate(reply.CalculateSize());
        reply.WriteTo(encoded.AsSpan());
        return encoded;
    }

    public void HandleSend(Message message)
    {
        metrics.Record(BenchPayload.Parser.ParseFrom(message.AsReadOnlySpan()));
    }
}

static void RunL0Owner(
    IRouterSocket router,
    L0BenchHandler handler,
    BenchServerMetrics metrics,
    CancellationToken stopping
)
{
    using var received = Received.Create();
    while (!stopping.IsCancellationRequested)
    {
        try
        {
            if (!router.Recv(received, RecvFlags.None))
                continue;

            do
            {
                using var replyBody = received.ReplyToken is null
                    ? null
                    : handler.HandleRequest(PayloadPart(received));
                if (replyBody is not null)
                {
                    using var header = Message.From(RawEnvelopeHeaders.Response);
                    received.Reply().Message(header).Message(replyBody).Submit();
                }
                else
                {
                    handler.HandleSend(PayloadPart(received));
                }
            } while (router.Recv(received, RecvFlags.DontWait));
        }
        catch (ZlinkRecvException ex)
            when (stopping.IsCancellationRequested || ex.Result == ZlinkRecvException.ErrorCode.Terminated)
        {
            return;
        }
        catch (ZlinkRecvException ex) when (ex.Result == ZlinkRecvException.ErrorCode.NoData)
        {
            continue;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"L0 zlink owner failed: {ex.Message}");
            metrics.RecordError();
        }
    }
}

static Message PayloadPart(Received received) =>
    received.IsSinglePart ? received.FirstPart() : received.Parts[^1];
#endif
