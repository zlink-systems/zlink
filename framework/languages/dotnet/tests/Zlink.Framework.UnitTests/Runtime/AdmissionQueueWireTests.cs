using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Zlink.Framework.UnitTests;

public sealed class AdmissionQueueWireTests
{
    [Theory]
    [MemberData(nameof(Messages))]
    public void ValidMessage(string name, string json)
    {
        using var document = JsonDocument.Parse(json);
        var item = document.RootElement;
        var packet = item.GetProperty("packetName").GetString()!;
        var types = Types(packet);
        Assert.Equal(packet, ZLinkMessageNameResolver.ResolveFromType(types.Request));
        RoundTrip(item.GetProperty(types.Reply is null ? "send" : "request"), types.Request);
        if (types.Reply is { } reply)
        {
            var body = item.GetProperty("response");
            RoundTrip(body, reply);
            if (item.TryGetProperty("replyPacketName", out var nameInEnvelope))
            {
                var parts = ZLinkSpotReplyEnvelope.EncodeResponseParts(
                    "admission",
                    packet,
                    null,
                    Read(body, reply),
                    reply
                );
                try
                {
                    var header = ZLinkEnvelopeCodec.DecodeHeader(parts);
                    Assert.Equal(ZLinkMessageKind.Response, header.Kind);
                    Assert.Equal(nameInEnvelope.GetString(), header.MessageName);
                }
                finally
                {
                    foreach (var part in parts)
                        part.Dispose();
                }
            }
        }
    }

    [Theory]
    [MemberData(nameof(InvalidMessages))]
    public void InvalidMessage(string name, string json)
    {
        using var document = JsonDocument.Parse(json);
        var item = document.RootElement;
        var types = Types(item.GetProperty("packetName").GetString()!);
        var isResponse = item.TryGetProperty("responsePayload", out var payload);
        if (!isResponse)
            payload = item.GetProperty("payload");
        var type = isResponse ? types.Reply! : types.Request;
        if (item.GetProperty("expect").GetString() == "accepted")
        {
            var value = Read(payload, type);
            var bytes = ZLinkFrameworkJsonPayloadCodec.Serialize(value, type);
            using var result = JsonDocument.Parse(bytes);
            Assert.False(result.RootElement.TryGetProperty("future", out _));
        }
        else
            Assert.ThrowsAny<JsonException>(() => Read(payload, type));
    }

    public static IEnumerable<object[]> Messages() =>
        AdmissionQueueFixture.Read("admission-queue-messages-v1.json", "messages");

    public static IEnumerable<object[]> InvalidMessages() =>
        AdmissionQueueFixture.Read("admission-queue-messages-v1.json", "invalid");

    private static (Type Request, Type? Reply) Types(string packet) =>
        packet switch
        {
            "$zlink.admission-queue.enqueue" => (
                typeof(AdmissionEnqueue),
                typeof(AdmissionEnqueued)
            ),
            "$zlink.admission-queue.report" => (typeof(AdmissionReport), typeof(AdmissionReported)),
            "$zlink.admission-queue.admit-permit" => (
                typeof(AdmissionPermitRequest),
                typeof(AdmissionPermitResult)
            ),
            "$zlink.admission-queue.release-permit" => (typeof(AdmissionPermitRelease), null),
            _ => throw new ArgumentOutOfRangeException(nameof(packet)),
        };

    private static object Read(JsonElement payload, Type type) =>
        ZLinkFrameworkJsonPayloadCodec.Deserialize(
            Encoding.UTF8.GetBytes(payload.GetRawText()),
            type
        )!;

    private static void RoundTrip(JsonElement payload, Type type)
    {
        var value = Read(payload, type);
        var expected = JsonNode.Parse(payload.GetRawText());
        var actual = JsonNode.Parse(ZLinkFrameworkJsonPayloadCodec.Serialize(value, type));
        Assert.True(JsonNode.DeepEquals(expected, actual), $"Expected {expected}, got {actual}.");
    }
}

internal static class AdmissionQueueFixture
{
    internal const string Queue = "login-admission-queue-v1.json";
    internal const string Key = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8";

    internal static IEnumerable<object[]> Read(string file, string section)
    {
        var path = Path.Combine(
            Common.FrameworkTestEnvironment.GetRepoRoot(),
            "framework",
            "runtime",
            "conformance",
            file
        );
        using var fixture = JsonDocument.Parse(File.ReadAllText(path));
        foreach (var item in fixture.RootElement.GetProperty(section).EnumerateArray())
            yield return [item.GetProperty("name").GetString()!, item.GetRawText()];
    }
}
