using Google.Protobuf;
using Systems.Zlink;
using WithGrpcBench.Shared;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;
using Systems.Zlink.Framework.Runtime.Protocol;

var endpoint = ArgValue(args, "--endpoint") ?? "tcp://127.0.0.1:5214";
var metricsUrl = ArgValue(args, "--metrics-url") ?? "http://127.0.0.1:5215";
var ladderLevel = Environment.GetEnvironmentVariable("ZLINK_DOTNET_LADDER_LEVEL") ?? "L0";

var builder = WebApplication.CreateBuilder(args);
ConfigureQuietLogging(builder);
builder.WebHost.UseUrls(metricsUrl);
builder.Services.AddSingleton<BenchServerMetrics>();
#if ZLINK_DOTNET_LADDER_L5_7
if (ladderLevel == "L5_7")
{
    builder.Services.AddZLinkFramework(framework =>
    {
        if (BenchDiagnostics.FlowTraceEnabled)
            framework.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        var mesh = framework
            .AddRouteMesh("bench")
            .Listen(endpoint)
            .SetRoutingId(RoutingId.From("bench-server"));
        mesh.AddRouteRequestHandler<TypedEchoHandler, BenchTypedPayload, BenchTypedPayload>(
                "BenchTypedPayload"
            )
            .AddRouteSendHandler<TypedCommandHandler, BenchTypedPayload>("BenchTypedPayload");
    });
}
#elif !ZLINK_DOTNET_LADDER_L0 && !ZLINK_DOTNET_LADDER_L1 && !ZLINK_DOTNET_LADDER_L2 && !ZLINK_DOTNET_LADDER_L3 && !ZLINK_DOTNET_LADDER_L4
{
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
}
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
#if ZLINK_DOTNET_LADDER_L0 || ZLINK_DOTNET_LADDER_L1 || ZLINK_DOTNET_LADDER_L2 || ZLINK_DOTNET_LADDER_L3 || ZLINK_DOTNET_LADDER_L4
if (ladderLevel is not ("L0" or "L1" or "L2" or "L3" or "L4" or "L5_7"))
    throw new ArgumentOutOfRangeException(nameof(ladderLevel), ladderLevel, "Unknown ladder level.");
if (ladderLevel == "L5_7")
{
#if ZLINK_DOTNET_LADDER_L5_7
    await app.RunAsync();
#else
    throw new ArgumentOutOfRangeException(nameof(ladderLevel), ladderLevel, "This build does not contain L5_7.");
#endif
}
else
{
    using var context = Systems.Zlink.Zlink.CreateContext();
    using var router = context.CreateRouterSocket();
    router.SetRoutingId(RoutingId.From("bench-server"));
    router.Options.ReceiveTimeout = TimeSpan.FromMilliseconds(100);
    router.Bind(endpoint);
    var handler = new L0BenchHandler(metrics);
    var decodeEnvelope = ladderLevel is "L2" or "L3" or "L4";
    var decodeServiceWire = ladderLevel is "L3" or "L4";
    var decodeTypedPayload = ladderLevel == "L4";
    ZLinkEnvelopeHeader? preparedReplyHeader = null;
#if ZLINK_DOTNET_LADDER_L2
    var ladderCodecs = CreateLadderCodecs();
    preparedReplyHeader = ZLinkEnvelopeCodec.PrepareResponse(
        "bench",
        typeof(BenchPayload),
        ladderCodecs!
    );
#endif
    if (ladderLevel == "L0")
    {
        var receiver = Task.Run(() =>
            RunL0Owner(
                router,
                handler,
                metrics,
                decodeEnvelope,
                preparedReplyHeader,
                app.Lifetime.ApplicationStopping
            )
        );
        try
        {
            await app.RunAsync();
        }
        finally
        {
            context.Shutdown();
            await receiver;
        }
    }
    else
    {
        var queueCapacity = ZLinkApplicationJobQueueCapacityResolver.Resolve(
            ZLinkApplicationJobQueueProfile.Balanced,
            null,
            ZLinkApplicationJobQueueCapacityResolver.ResolveEffectiveProcessorCount(
                Environment.ProcessorCount
            )
        );
        using var queue = new ZLinkApplicationJobQueue(queueCapacity);
        var receiver = Task.Run(() =>
            RunL1Owner(
                router,
                handler,
                metrics,
                queue,
                decodeEnvelope,
                preparedReplyHeader,
                decodeServiceWire,
                decodeTypedPayload,
                app.Lifetime.ApplicationStopping
            )
        );
        try
        {
            await app.RunAsync();
        }
        finally
        {
            context.Shutdown();
            await receiver;
        }
    }
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

#if ZLINK_DOTNET_LADDER_L2
static ZLinkCodecRegistryBuilder CreateLadderCodecs()
{
    var codecs = new ZLinkCodecRegistryBuilder();
    codecs.Use(ZLinkProtobufCodec.Default);
    return codecs;
}
#endif

static string? ArgValue(string[] args, string name)
{
    for (var i = 0; i < args.Length - 1; i++)
    {
        if (args[i] == name)
            return args[i + 1];
    }

    return null;
}

#if ZLINK_DOTNET_LADDER_L0 || ZLINK_DOTNET_LADDER_L1 || ZLINK_DOTNET_LADDER_L2 || ZLINK_DOTNET_LADDER_L3 || ZLINK_DOTNET_LADDER_L4
static void RunL0Owner(
    IRouterSocket router,
    L0BenchHandler handler,
    BenchServerMetrics metrics,
    bool decodeEnvelope,
    ZLinkEnvelopeHeader? preparedReplyHeader,
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

#if ZLINK_DOTNET_LADDER_L1 || ZLINK_DOTNET_LADDER_L2 || ZLINK_DOTNET_LADDER_L3 || ZLINK_DOTNET_LADDER_L4
            ZLinkEnvelopeHeader? requestHeader = null;
            if (decodeEnvelope)
                requestHeader = ZLinkEnvelopeCodec.DecodeHeader(received.FirstPart());
#endif

            using var replyBody = received.ReplyToken is null
                ? null
                : handler.HandleRequest(PayloadPart(received));
            if (replyBody is not null)
            {
#if ZLINK_DOTNET_LADDER_L2
                if (decodeEnvelope)
                {
                    ReplyL2(received, requestHeader!, replyBody, preparedReplyHeader!);
                    continue;
                }
#endif
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

#endif

#if ZLINK_DOTNET_LADDER_L1 || ZLINK_DOTNET_LADDER_L2 || ZLINK_DOTNET_LADDER_L3 || ZLINK_DOTNET_LADDER_L4
static async Task RunL1Owner(
    IRouterSocket router,
    L0BenchHandler handler,
    BenchServerMetrics metrics,
    ZLinkApplicationJobQueue queue,
    bool decodeEnvelope,
    ZLinkEnvelopeHeader? preparedReplyHeader,
    bool decodeServiceWire,
    bool decodeTypedPayload,
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

#if ZLINK_DOTNET_LADDER_L1 || ZLINK_DOTNET_LADDER_L2 || ZLINK_DOTNET_LADDER_L3 || ZLINK_DOTNET_LADDER_L4
            ZLinkEnvelopeHeader? requestHeader = null;
            if (decodeEnvelope && !decodeServiceWire)
                requestHeader = ZLinkEnvelopeCodec.DecodeHeader(received.FirstPart());
#endif

#if ZLINK_DOTNET_LADDER_L3 || ZLINK_DOTNET_LADDER_L4
            if (decodeServiceWire)
            {
                await HandleServiceWire(
                    received,
                    handler,
                    permit,
                    decodeTypedPayload
                );
                continue;
            }
#endif

            if (received.ReplyToken is not null)
            {
                await permit.ReleaseForHandlerStartAsync();
                using var replyBody = handler.HandleRequest(PayloadPart(received));
#if ZLINK_DOTNET_LADDER_L2
                if (decodeEnvelope)
                {
                    ReplyL2(received, requestHeader!, replyBody, preparedReplyHeader!);
                    continue;
                }
#endif
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

#endif

#if ZLINK_DOTNET_LADDER_L2
static void ReplyL2(
    Received received,
    ZLinkEnvelopeHeader requestHeader,
    Message replyBody,
    ZLinkEnvelopeHeader preparedReplyHeader
)
{
    var replyHeader = ZLinkChannelReplyWriter.CreateReplyHeader(
        ZLinkMessageKind.Response,
        requestHeader.ChannelName,
        requestHeader,
        preparedReplyHeader
    );
    var replyParts = ZLinkEnvelopeCodec.EncodeRawBodyParts(replyHeader, replyBody);
    try
    {
        received.Reply().Messages(replyParts).Submit();
    }
    finally
    {
        replyParts[0].Dispose();
    }
}
#endif

#if ZLINK_DOTNET_LADDER_L0 || ZLINK_DOTNET_LADDER_L1 || ZLINK_DOTNET_LADDER_L2 || ZLINK_DOTNET_LADDER_L3 || ZLINK_DOTNET_LADDER_L4
static Message PayloadPart(Received received) =>
    received.IsSinglePart ? received.FirstPart() : received.Parts[^1];
#endif

#if ZLINK_DOTNET_LADDER_L3 || ZLINK_DOTNET_LADDER_L4
static async Task HandleServiceWire(
    Received received,
    L0BenchHandler handler,
    ZLinkApplicationJobQueueLease permit,
    bool decodeTypedPayload
)
{
    if (
        received.Parts.Count != 2
        || !ZLinkServiceWireCodec.TryDecodeApplication(
            received.FirstPart().AsReadOnlySpan(),
            out var application,
            out _
        )
        || application.ChannelName != "bench"
        || application.HasMetadata
        || !ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(
            received.Parts[1],
            out var envelopeParts
        )
        || envelopeParts.Count != 2
    )
        throw new InvalidOperationException("Service wire request was invalid.");

    var isRequest = application.Command == ServiceWireConstants.Command.ChannelRequest;
    if (
        (!isRequest && application.Command != ServiceWireConstants.Command.ChannelSend)
        || isRequest != (received.ReplyToken is not null)
    )
        throw new InvalidOperationException("Service wire request kind was invalid.");

    var envelopeHeader = ZLinkEnvelopeCodec.DecodeHeader(envelopeParts);
    if (decodeTypedPayload && (
        envelopeHeader.MessageName != "BenchTypedPayload"
        || envelopeHeader.ContentType != ZLinkEnvelopeCodec.DefaultContentType
    ))
        throw new InvalidOperationException("Typed JSON service payload was invalid.");
    await permit.ReleaseForHandlerStartAsync();
    if (!isRequest)
    {
        if (decodeTypedPayload)
            handler.HandleSend((BenchTypedPayload)ZLinkEnvelopeCodec.DecodeBody(
                envelopeParts,
                typeof(BenchTypedPayload),
                envelopeHeader.ContentType,
                null
            )!);
        else
            handler.HandleSend(envelopeParts.GetSpan(1));
        return;
    }

    Message framedReply;
    if (decodeTypedPayload)
    {
        var request = (BenchTypedPayload)
            ZLinkEnvelopeCodec.DecodeBody(
                envelopeParts,
                typeof(BenchTypedPayload),
                envelopeHeader.ContentType,
                null
            )!;
        framedReply = EncodeTypedReply(handler.HandleRequest(request), envelopeHeader);
    }
    else
        framedReply = EncodeRawReply(handler.HandleRequest(envelopeParts.GetSpan(1)));
    using (framedReply)
    {
        using var serviceReply = Message.From(
            ZLinkServiceWireCodec.EncodeReply(application.Correlation, 0, 0)
        );
        received.Reply().Message(serviceReply).Message(framedReply).Submit();
    }
}

static Message EncodeRawReply(Message replyBody)
{
    using (replyBody)
    using (var responseHeader = Message.From(RawEnvelopeHeaders.Response))
        return ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(
            [responseHeader, replyBody]
        );
}

#if ZLINK_DOTNET_LADDER_L4
static Message EncodeTypedReply(BenchTypedPayload reply, ZLinkEnvelopeHeader requestHeader)
{
    var header = ZLinkChannelReplyWriter.CreateReplyHeader(
        ZLinkMessageKind.Response,
        "bench",
        requestHeader
    );
    var parts = ZLinkEnvelopeCodec.EncodeParts(header, reply, typeof(BenchTypedPayload), null);
    try
    {
        return ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(parts);
    }
    finally
    {
        foreach (var part in parts)
            part.Dispose();
    }
}
#endif
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

internal sealed class TypedEchoHandler(BenchServerMetrics metrics)
    : IZLinkRouteRequestHandler<BenchTypedPayload, BenchTypedPayload>
{
    public ValueTask<BenchTypedPayload> HandleAsync(
        BenchTypedPayload request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken
    )
    {
        metrics.RecordReceived(request);
        return ValueTask.FromResult(request);
    }
}

internal sealed class TypedCommandHandler(BenchServerMetrics metrics)
    : IZLinkRouteSendHandler<BenchTypedPayload>
{
    public ValueTask HandleAsync(
        BenchTypedPayload message,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken
    )
    {
        metrics.Record(message);
        return ValueTask.CompletedTask;
    }
}

#if ZLINK_DOTNET_LADDER_L0 || ZLINK_DOTNET_LADDER_L1 || ZLINK_DOTNET_LADDER_L2 || ZLINK_DOTNET_LADDER_L3 || ZLINK_DOTNET_LADDER_L4
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
        => HandleRequest(request.AsReadOnlySpan());

    public Message HandleRequest(ReadOnlySpan<byte> request)
    {
        var payload = BenchPayload.Parser.ParseFrom(request);
        metrics.RecordReceived(payload);
        var reply = new BenchPayload { Body = payload.Body };
        var encoded = Message.Allocate(reply.CalculateSize());
        reply.WriteTo(encoded.AsSpan());
        return encoded;
    }

    public BenchPayload HandleRequest(BenchPayload request)
    {
        metrics.RecordReceived(request);
        return new BenchPayload { Body = request.Body };
    }

    public BenchTypedPayload HandleRequest(BenchTypedPayload request)
    {
        metrics.RecordReceived(request);
        return request;
    }

    public void HandleSend(Message message)
    {
        HandleSend(message.AsReadOnlySpan());
    }

    public void HandleSend(ReadOnlySpan<byte> message) => metrics.Record(BenchPayload.Parser.ParseFrom(message));

    public void HandleSend(BenchPayload message) => metrics.Record(message);

    public void HandleSend(BenchTypedPayload message) => metrics.Record(message);
}
#endif
