using Google.Protobuf;
using Systems.Zlink;
using WithGrpcBench.Shared;

var endpoint = ArgValue(args, "--endpoint") ?? "tcp://127.0.0.1:5207";
var commandEndpoint = ArgValue(args, "--command-endpoint") ?? "tcp://127.0.0.1:5208";
var metricsUrl = ArgValue(args, "--metrics-url") ?? "http://127.0.0.1:5209";
var gameJson = args.Contains("--game-json", StringComparer.Ordinal);

var builder = WebApplication.CreateBuilder(args);
ConfigureQuietLogging(builder);
builder.WebHost.UseUrls(metricsUrl);
builder.Services.AddSingleton<BenchServerMetrics>();

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

using var context = Systems.Zlink.Zlink.CreateContext();
using var requestRouter = context.CreateRouterSocket();
using var commandRouter = context.CreateRouterSocket();

// FB-001 / bench spec 1.3: a ROUTER client addresses this server by routing id,
// so both sockets must announce their well-known id before bind. A DEALER client
// ignores these ids, so the legacy DEALER->ROUTER configuration still works.
requestRouter.SetRoutingId(RoutingId.From(BenchRoutingIds.RawRequestServer));
commandRouter.SetRoutingId(RoutingId.From(BenchRoutingIds.RawCommandServer));

// A blocking Recv never returns by itself, so each receive returns NoData after
// ReceivePollInterval and the loop stops once host shutdown begins.
requestRouter.Options.ReceiveTimeout = RawServerTiming.ReceivePollInterval;
commandRouter.Options.ReceiveTimeout = RawServerTiming.ReceivePollInterval;
requestRouter.Bind(endpoint);
commandRouter.Bind(commandEndpoint);

var stopping = app.Lifetime.ApplicationStopping;
var requestReceiver = Task.Run(() => RunRequestRouter(requestRouter, metrics, stopping, gameJson));
var commandReceiver = Task.Run(() => RunCommandRouter(commandRouter, metrics, stopping, gameJson));

await app.RunAsync();
await Task.WhenAll(requestReceiver, commandReceiver);

static void ConfigureQuietLogging(WebApplicationBuilder builder)
{
    builder.Logging.ClearProviders();
    builder.Logging.AddConsole();
    builder.Logging.SetMinimumLevel(LogLevel.Warning);
}

static void RunRequestRouter(
    IRouterSocket router,
    BenchServerMetrics metrics,
    CancellationToken stopping,
    bool gameJson
)
{
    using var received = Received.Create();
    while (!stopping.IsCancellationRequested)
    {
        try
        {
            if (!router.Recv(received))
            {
                continue;
            }

            var body = PayloadPart(received);
            BenchPayload? payload = null;
            BenchGameMessage? game = null;
            if (gameJson)
            {
                game = BenchGamePayloads.DecodeStandard(body.AsReadOnlySpan());
                metrics.RecordReceived(game);
            }
            else
            {
                payload = RawWire.Decode(body.AsReadOnlySpan());
                metrics.RecordReceived(payload);
            }
            if (received.ReplyToken is not null)
            {
                if (game is null)
                    ReplyMultipart(received, new BenchPayload { Body = payload!.Body });
                else
                    ReplyJson(received, game);
            }
            else
            {
                if (game is null)
                    SendMultipart(received, new BenchPayload { Body = payload!.Body });
                else
                    SendJson(received, game);
            }
        }
        catch (ZlinkRecvException ex) when (ex.Result == ZlinkRecvException.ErrorCode.NoData)
        {
            // A blocking Recv returns `false` only under RecvFlags.DontWait; with
            // RecvFlags.None an expired RCVTIMEO surfaces as NoData/EAGAIN. Core
            // spec 07-router sec.8 defines that as "no record available", not a
            // failure, so it must not be logged or counted as a server error.
            continue;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"raw zlink request loop failed: {ex.Message}");
            metrics.RecordError();
        }
    }
}

static void ReplyMultipart(Received received, BenchPayload payload)
{
    using var header = Message.From(RawEnvelopeHeaders.Response);
    using var replyBody = Message.Allocate(payload.CalculateSize());
    RawWire.Encode(payload, replyBody.AsSpan());
    received.Reply().Message(header).Message(replyBody).Submit();
}

static void SendMultipart(Received received, BenchPayload payload)
{
    using var header = Message.From(RawEnvelopeHeaders.Response);
    using var replyBody = Message.Allocate(payload.CalculateSize());
    RawWire.Encode(payload, replyBody.AsSpan());
    received.Send().Message(header).Message(replyBody).Submit();
}

static void ReplyJson(Received received, BenchGameMessage payload)
{
    using var header = Message.From(RawEnvelopeHeaders.JsonResponse);
    using var body = EncodeGamePayload(payload);
    received.Reply().Message(header).Message(body).Submit();
}

static void SendJson(Received received, BenchGameMessage payload)
{
    using var header = Message.From(RawEnvelopeHeaders.JsonResponse);
    using var body = EncodeGamePayload(payload);
    received.Send().Message(header).Message(body).Submit();
}

static Message EncodeGamePayload(BenchGameMessage payload)
{
    var bytes = BenchGamePayloads.EncodeStandard(payload);
    var message = Message.Allocate(bytes.Length);
    bytes.CopyTo(message.AsSpan());
    return message;
}

static void RunCommandRouter(
    IRouterSocket router,
    BenchServerMetrics metrics,
    CancellationToken stopping,
    bool gameJson
)
{
    using var received = Received.Create();
    while (!stopping.IsCancellationRequested)
    {
        try
        {
            if (!router.Recv(received))
            {
                continue;
            }

            var body = PayloadPart(received);
            if (gameJson)
                metrics.Record(BenchGamePayloads.DecodeStandard(body.AsReadOnlySpan()));
            else
                metrics.Record(RawWire.Decode(body.AsReadOnlySpan()));
        }
        catch (ZlinkRecvException ex) when (ex.Result == ZlinkRecvException.ErrorCode.NoData)
        {
            // See RunRequestRouter: NoData/EAGAIN from a blocking Recv is a normal
            // idle return, not an error.
            continue;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"raw zlink command loop failed: {ex.Message}");
            metrics.RecordError();
        }
    }
}

static Message PayloadPart(Received received)
{
    if (received.IsSinglePart)
    {
        return received.FirstPart();
    }

    return received.Parts[^1];
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

static class RawServerTiming
{
    public static readonly TimeSpan ReceivePollInterval = TimeSpan.FromMilliseconds(100);
}
