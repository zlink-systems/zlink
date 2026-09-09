using Google.Protobuf;
using Systems.Zlink;
using System.Text;
using WithGrpcBench.Shared;

var endpoint = ArgValue(args, "--endpoint") ?? "tcp://127.0.0.1:5207";
var commandEndpoint = ArgValue(args, "--command-endpoint") ?? "tcp://127.0.0.1:5208";
var metricsUrl = ArgValue(args, "--metrics-url") ?? "http://127.0.0.1:5209";

var builder = WebApplication.CreateBuilder(args);
ConfigureQuietLogging(builder);
builder.WebHost.UseUrls(metricsUrl);
builder.Services.AddSingleton<BenchServerMetrics>();

var app = builder.Build();
var metrics = app.Services.GetRequiredService<BenchServerMetrics>();
app.MapGet("/ready", () => Results.Ok("ready"));
app.MapPost("/bench/reset", () =>
{
    metrics.Reset();
    return Results.Ok();
});
app.MapGet("/bench/stats", () => Results.Ok(metrics.Snapshot()));

using var context = Systems.Zlink.Zlink.CreateContext();
using var requestRouter = context.CreateRouterSocket();
using var commandRouter = context.CreateRouterSocket();
// FB-001 / bench spec 1.3: a ROUTER client addresses this server by routing id,
// so both sockets must announce their well-known id before bind. A DEALER client
// ignores these ids, so the legacy DEALER->ROUTER configuration still works.
requestRouter.SetRoutingId(RoutingId.From(BenchRoutingIds.RawRequestServer));
commandRouter.SetRoutingId(RoutingId.From(BenchRoutingIds.RawCommandServer));
requestRouter.Bind(endpoint);
commandRouter.Bind(commandEndpoint);

var requestReceiver = Task.Run(() => RunRequestRouter(requestRouter, metrics));
var commandReceiver = Task.Run(() => RunCommandRouter(commandRouter, metrics));

await app.RunAsync();
await Task.WhenAll(requestReceiver, commandReceiver);

static void ConfigureQuietLogging(WebApplicationBuilder builder)
{
    builder.Logging.ClearProviders();
    builder.Logging.AddConsole();
    builder.Logging.SetMinimumLevel(LogLevel.Warning);
}

static void RunRequestRouter(IRouterSocket router, BenchServerMetrics metrics)
{
    using var received = Received.Create();
    while (true)
    {
        try
        {
            if (!router.Recv(received))
            {
                continue;
            }

            var body = PayloadPart(received);
            if (!TryGetBenchPayloadBody(body.AsReadOnlySpan(), out var payload))
            {
                throw new InvalidOperationException("Invalid raw protobuf payload.");
            }
            metrics.RecordReceived(payload);
            if (received.ReplyToken is not null)
            {
                ReplyMultipart(received, body);
            }
            else
            {
                SendMultipart(received, body);
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

static void ReplyMultipart(Received received, Message body)
{
    using var header = Message.From(RawEnvelopeHeaders.Response);
    using var replyBody = Message.From(body.AsReadOnlySpan());
    received.Reply()
        .Message(header)
        .Message(replyBody)
        .Submit();
}

static void SendMultipart(Received received, Message body)
{
    using var header = Message.From(RawEnvelopeHeaders.Response);
    using var replyBody = Message.From(body.AsReadOnlySpan());
    received.Send()
        .Message(header)
        .Message(replyBody)
        .Submit();
}

static void RunCommandRouter(IRouterSocket router, BenchServerMetrics metrics)
{
    using var received = Received.Create();
    while (true)
    {
        try
        {
            if (!router.Recv(received))
            {
                continue;
            }

            var body = PayloadPart(received);
            if (!TryGetBenchPayloadBody(body.AsReadOnlySpan(), out var payload))
            {
                throw new InvalidOperationException("Invalid raw protobuf payload.");
            }
            metrics.Record(payload);
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

static bool TryReadVarint(ref ReadOnlySpan<byte> source, out int value)
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

static bool TryGetBenchPayloadBody(ReadOnlySpan<byte> encoded, out ReadOnlySpan<byte> body)
{
    var source = encoded;
    while (!source.IsEmpty)
    {
        if (!TryReadVarint(ref source, out var key))
            break;
        var field = key >> 3;
        var wireType = key & 0x07;
        if (wireType != 2)
            break;
        if (!TryReadVarint(ref source, out var length) || source.Length < length)
            break;
        if (field == 1)
        {
            body = source[..length];
            return true;
        }

        source = source[length..];
    }

    body = default;
    return false;
}

static string? ArgValue(string[] args, string name)
{
    for (var i = 0; i < args.Length - 1; i++)
    {
        if (args[i] == name) return args[i + 1];
    }

    return null;
}

static class RawEnvelopeHeaders
{
    public static readonly byte[] Response = Encoding.UTF8.GetBytes(
        "{\"kind\":2,\"channelName\":\"bench\",\"messageName\":\"BenchPayload\",\"contentType\":\"application/x-protobuf\",\"correlationId\":null,\"deadline\":null,\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null}");
}
