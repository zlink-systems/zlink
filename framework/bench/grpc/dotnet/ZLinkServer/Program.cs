using Google.Protobuf;
using Systems.Zlink;
using WithGrpcBench.Shared;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Runtime.Dispatch;

var endpoint = ArgValue(args, "--endpoint") ?? "tcp://127.0.0.1:5214";
var metricsUrl = ArgValue(args, "--metrics-url") ?? "http://127.0.0.1:5215";

var builder = WebApplication.CreateBuilder(args);
ConfigureQuietLogging(builder);
builder.WebHost.UseUrls(metricsUrl);
builder.Services.AddSingleton<BenchServerMetrics>();
#if !ZLINK_DOTNET_LADDER_L0 && !ZLINK_DOTNET_LADDER_L1
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
#if ZLINK_DOTNET_LADDER_L0 || ZLINK_DOTNET_LADDER_L1
using var context = Systems.Zlink.Zlink.CreateContext();
using var router = context.CreateRouterSocket();
router.SetRoutingId(RoutingId.From("bench-server"));
router.Options.ReceiveTimeout = TimeSpan.FromMilliseconds(100);
router.Bind(endpoint);
var handler = new L0BenchHandler(metrics);
#if ZLINK_DOTNET_LADDER_L1
var queueCapacity = ZLinkApplicationJobQueueCapacityResolver.Resolve(
    ZLinkApplicationJobQueueProfile.Balanced,
    null,
    ZLinkApplicationJobQueueCapacityResolver.ResolveEffectiveProcessorCount(
        Environment.ProcessorCount
    )
);
using var queue = new ZLinkApplicationJobQueue(queueCapacity);
var receiver = Task.Run(() =>
    RunL1Owner(router, handler, metrics, queue, app.Lifetime.ApplicationStopping)
);
#else
var receiver = Task.Run(() => RunL0Owner(router, handler, metrics, app.Lifetime.ApplicationStopping));
#endif
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

#if ZLINK_DOTNET_LADDER_L0
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

#if ZLINK_DOTNET_LADDER_L1
static async Task RunL1Owner(
    IRouterSocket router,
    L0BenchHandler handler,
    BenchServerMetrics metrics,
    ZLinkApplicationJobQueue queue,
    CancellationToken stopping
)
{
    using var received = Received.Create();
    while (!stopping.IsCancellationRequested)
    {
        ZLinkApplicationJobQueueLease? permit = null;
        try
        {
            permit = await queue.AcquireAsync(stopping, ZLinkApplicationJobOrigin.Remote);
            if (!router.Recv(received, RecvFlags.None))
                continue;

            if (received.ReplyToken is not null)
            {
                await permit.ReleaseForHandlerStartAsync();
                using var replyBody = handler.HandleRequest(PayloadPart(received));
                using var header = Message.From(RawEnvelopeHeaders.Response);
                received.Reply().Message(header).Message(replyBody).Submit();
            }
            else
            {
                await permit.ReleaseForHandlerStartAsync();
                handler.HandleSend(PayloadPart(received));
            }
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
        catch (OperationCanceledException) when (stopping.IsCancellationRequested)
        {
            return;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"L1 zlink owner failed: {ex.Message}");
            metrics.RecordError();
        }
        finally
        {
            permit?.Dispose();
        }
    }
}

static Message PayloadPart(Received received) =>
    received.IsSinglePart ? received.FirstPart() : received.Parts[^1];
#endif

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

#if ZLINK_DOTNET_LADDER_L0 || ZLINK_DOTNET_LADDER_L1
internal sealed class L0BenchHandler(BenchServerMetrics metrics)
    : IZLinkRouteRequestHandler<Message, Message>, IZLinkRouteSendHandler<Message>
{
    ValueTask<Message> IZLinkRouteRequestHandler<Message, Message>.HandleAsync(
        Message request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken
    ) => ValueTask.FromResult(HandleRequest(request));

    ValueTask IZLinkRouteSendHandler<Message>.HandleAsync(
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
#endif
