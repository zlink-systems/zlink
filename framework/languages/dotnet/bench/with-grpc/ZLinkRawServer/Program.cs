using Google.Protobuf;
using Systems.Zlink;
using System.Text;
using WithGrpcBench.Shared;

var endpoint = ArgValue(args, "--endpoint") ?? "tcp://127.0.0.1:5075";
var commandEndpoint = ArgValue(args, "--command-endpoint") ?? "tcp://127.0.0.1:5077";
var metricsUrl = ArgValue(args, "--metrics-url") ?? "http://127.0.0.1:5076";

var builder = WebApplication.CreateBuilder(args);
ConfigureQuietLogging(builder);
builder.WebHost.UseUrls(metricsUrl);
builder.Services.AddSingleton<BenchServerMetrics>();

var app = builder.Build();
app.MapGet("/ready", () => Results.Ok("ready"));
app.MapPost("/bench/reset", (BenchServerMetrics metrics) =>
{
    metrics.Reset();
    return Results.Ok();
});
app.MapGet("/bench/stats", (BenchServerMetrics metrics) => Results.Ok(metrics.Snapshot()));

using var context = Zlink.CreateContext();
using var requestRouter = context.CreateRouterSocket();
using var commandRouter = context.CreateRouterSocket();
requestRouter.Bind(endpoint);
commandRouter.Bind(commandEndpoint);

var metrics = app.Services.GetRequiredService<BenchServerMetrics>();
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
            if (received.RequestSeq.HasValue)
            {
                ReplyMultipart(received, body);
            }
            else
            {
                SendMultipart(received, body);
            }
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
    using var replyBody = EncodeRawReplyBody(body);
    received.Reply()
        .Message(header)
        .Message(replyBody)
        .Submit();
}

static void SendMultipart(Received received, Message body)
{
    using var header = Message.From(RawEnvelopeHeaders.Response);
    using var replyBody = EncodeRawReplyBody(body);
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
            metrics.Record(DecodeRawPayload(body));
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

static BenchPayload DecodeRawPayload(Message body)
{
    var payload = new BenchPayload();
    payload.MergeFrom(body.AsReadOnlySpan());
    return payload;
}

static Message EncodeRawReplyBody(Message requestBody)
{
    if (!TryGetBenchPayloadBody(requestBody.AsReadOnlySpan(), out var payload))
    {
        throw new InvalidOperationException("Invalid raw protobuf payload.");
    }

    var encodedSize = 1 + VarintSize(payload.Length) + payload.Length;
    var body = Message.Allocate(encodedSize);
    try
    {
        var span = body.AsSpan();
        span[0] = 0x0a;
        var offset = WriteVarint(span[1..], payload.Length) + 1;
        payload.CopyTo(span[offset..(offset + payload.Length)]);
        return body;
    }
    catch
    {
        body.Dispose();
        throw;
    }
}

static int VarintSize(int value)
{
    var size = 1;
    var remaining = (uint)value;
    while (remaining >= 0x80)
    {
        remaining >>= 7;
        size++;
    }

    return size;
}

static int WriteVarint(Span<byte> destination, int value)
{
    var remaining = (uint)value;
    var index = 0;
    while (remaining >= 0x80)
    {
        destination[index++] = (byte)((remaining & 0x7f) | 0x80);
        remaining >>= 7;
    }

    destination[index++] = (byte)remaining;
    return index;
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
