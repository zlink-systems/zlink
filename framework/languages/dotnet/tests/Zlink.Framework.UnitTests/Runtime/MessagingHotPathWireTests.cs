using System.Buffers.Binary;
using System.Text;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Runtime.Codecs;
using StringValue = Google.Protobuf.WellKnownTypes.StringValue;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class MessagingHotPathWireTests
{
    private const string CommandHeader =
        "{\"formatMarker\":242,\"kind\":3,\"channelName\":\"wire\",\"messageName\":\"payload\",\"contentType\":\"application/json\",\"correlationId\":null,\"deadline\":null,\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null,\"flowId\":null,\"flowOrigin\":null}";
    private const string RequestHeader =
        "{\"formatMarker\":242,\"kind\":1,\"channelName\":\"wire\",\"messageName\":\"payload\",\"contentType\":\"application/json\",\"correlationId\":\"corr-fixed\",\"deadline\":null,\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null,\"flowId\":null,\"flowOrigin\":null}";
    private const string ResponseHeader =
        "{\"formatMarker\":242,\"kind\":2,\"channelName\":\"wire\",\"messageName\":\"payload\",\"contentType\":\"application/json\",\"correlationId\":\"corr-fixed\",\"deadline\":null,\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null,\"flowId\":null,\"flowOrigin\":null}";
    private const string ErrorHeader =
        "{\"formatMarker\":242,\"kind\":5,\"channelName\":\"wire\",\"messageName\":\"payload\",\"contentType\":\"application/json\",\"correlationId\":\"corr-fixed\",\"deadline\":null,\"topic\":null,\"errorCode\":\"Failed\",\"errorMessage\":\"fixed failure\",\"source\":null,\"flowId\":null,\"flowOrigin\":null}";
    private const string ProtobufHeader =
        "{\"formatMarker\":242,\"kind\":1,\"channelName\":\"wire\",\"messageName\":\"payload\",\"contentType\":\"application/x-protobuf\",\"correlationId\":\"corr-fixed\",\"deadline\":null,\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null,\"flowId\":null,\"flowOrigin\":null}";
    private const string PartSerializerHeader =
        "{\"formatMarker\":242,\"kind\":1,\"channelName\":\"wire\",\"messageName\":\"payload\",\"contentType\":\"application/x-wire-part\",\"correlationId\":\"corr-fixed\",\"deadline\":null,\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null,\"flowId\":null,\"flowOrigin\":null}";

    public static IEnumerable<object[]> HeaderVectors()
    {
        yield return [(int)ZLinkMessageKind.Command, null!, null!, null!, CommandHeader];
        yield return [(int)ZLinkMessageKind.Request, "corr-fixed", null!, null!, RequestHeader];
        yield return [(int)ZLinkMessageKind.Response, "corr-fixed", null!, null!, ResponseHeader];
        yield return [(int)ZLinkMessageKind.Error, "corr-fixed", "Failed", "fixed failure", ErrorHeader];
    }

    [Theory]
    [MemberData(nameof(HeaderVectors))]
    public void Envelope_header_matches_fixed_wire_vector(
        int kind,
        string? correlationId,
        string? errorCode,
        string? errorMessage,
        string expectedJson)
    {
        var header = Header((ZLinkMessageKind)kind, correlationId, errorCode, errorMessage);
        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);

        Assert.Equal(Encoding.UTF8.GetBytes(expectedJson), encoded.ToArray());
    }

    [Theory]
    [InlineData(1024)]
    [InlineData(4096)]
    public void Framework_multipart_matches_reference_frame_for_large_payload(int payloadSize)
    {
        var payload = Enumerable.Range(0, payloadSize)
            .Select(static index => (byte)(index % 251))
            .ToArray();
        var headerBytes = Encoding.UTF8.GetBytes(RequestHeader);
        using var header = Message.From(headerBytes);
        using var body = Message.From(payload);

        var encoded = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart([header, body]);

        Assert.Equal(ReferenceMultipartFrame(headerBytes, payload), encoded);
        using var native = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage([header, body]);
        Assert.Equal(encoded, native.ToArray());
        Assert.True(ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(native, out var view));
        Assert.Equal(headerBytes, view.GetSpan(0).ToArray());
        Assert.Equal(payload, view.GetSpan(1).ToArray());
        Assert.True(ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(encoded, out var decoded));
        try
        {
            Assert.Equal(headerBytes, decoded[0].ToArray());
            Assert.Equal(payload, decoded[1].ToArray());
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(decoded);
        }
    }

    [Fact]
    public void Protobuf_serializer_keeps_fixed_content_type_header_and_body_bytes()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.Use(ZLinkProtobufCodec.Default);

        var parts = ZLinkEnvelopeCodec.EncodeParts(
            Header(ZLinkMessageKind.Request, "corr-fixed"),
            new StringValue { Value = "wire" },
            typeof(StringValue),
            codecs);
        try
        {
            Assert.Equal(Encoding.UTF8.GetBytes(ProtobufHeader), parts[0].ToArray());
            Assert.Equal(new byte[] { 0x0A, 0x04, (byte)'w', (byte)'i', (byte)'r', (byte)'e' }, parts[1].ToArray());
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    [Fact]
    public void Correlated_header_with_deadline_flow_and_metadata_matches_fixed_wire_bytes()
    {
        var header = Header(ZLinkMessageKind.Request, "0000000000000048") with
        {
            Deadline = new DateTimeOffset(2026, 9, 10, 12, 34, 56, TimeSpan.FromHours(9)).AddTicks(1234567),
            Topic = "topic/one",
            Source = "node-a",
            FlowId = "0196f7c2-4cb4-7cc8-89d4-2d6aee6fca2d",
            FlowOrigin = ZLinkFlowOrigin.Application,
            Metadata = new() { ["first"] = "a\"b", ["second"] = "한글<&" }
        };
        const string expected = """{"formatMarker":242,"kind":1,"channelName":"wire","messageName":"payload","contentType":"application/json","correlationId":"0000000000000048","deadline":"2026-09-10T12:34:56.1234567+09:00","topic":"topic/one","errorCode":null,"errorMessage":null,"source":"node-a","flowId":"0196f7c2-4cb4-7cc8-89d4-2d6aee6fca2d","flowOrigin":3,"metadata":{"first":"a\u0022b","second":"\uD55C\uAE00\u003C\u0026"}}""";

        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
        Assert.Equal(Encoding.UTF8.GetBytes(expected), encoded.ToArray());
        using var fixedWire = Message.From(Encoding.UTF8.GetBytes(expected));
        var decoded = ZLinkEnvelopeCodec.DecodeHeader(fixedWire);
        Assert.Equal(header.Deadline, decoded.Deadline);
        Assert.Equal(header.CorrelationId, decoded.CorrelationId);
        Assert.Equal(header.FlowId, decoded.FlowId);
        Assert.Equal(header.Metadata, decoded.Metadata);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1024)]
    [InlineData(4096)]
    public void Typed_json_envelope_and_final_multipart_match_fixed_wire_bytes(int length)
    {
        var value = new JsonWireValue(new string('x', length), long.MinValue, ulong.MaxValue);
        var expectedBody = Encoding.UTF8.GetBytes(
            "{\"text\":\"" + new string('x', length)
            + "\",\"signed\":\"-9223372036854775808\",\"unsigned\":\"18446744073709551615\"}");
        var parts = ZLinkEnvelopeCodec.EncodeParts(
            Header(ZLinkMessageKind.Request, "corr-fixed"), value, typeof(JsonWireValue), null);
        try
        {
            Assert.Equal(Encoding.UTF8.GetBytes(RequestHeader), parts[0].ToArray());
            Assert.Equal(expectedBody, parts[1].ToArray());
            using var wire = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(parts);
            Assert.Equal(ReferenceMultipartFrame(Encoding.UTF8.GetBytes(RequestHeader), expectedBody), wire.ToArray());
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    private sealed record JsonWireValue(string Text, long Signed, ulong Unsigned);

    [Fact]
    public void Part_serializer_keeps_fixed_content_type_header_and_body_bytes()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer("application/x-wire-part", new FixedPartSerializer(),
            static type => type == typeof(WireValue));

        var parts = ZLinkEnvelopeCodec.EncodeParts(
            Header(ZLinkMessageKind.Request, "corr-fixed"),
            new WireValue(),
            typeof(WireValue),
            codecs);
        try
        {
            Assert.Equal(Encoding.UTF8.GetBytes(PartSerializerHeader), parts[0].ToArray());
            Assert.Equal(new byte[] { 0xC0, 0xDE, 0x48, 0x4F, 0x54 }, parts[1].ToArray());
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    private static ZLinkEnvelopeHeader Header(
        ZLinkMessageKind kind,
        string? correlationId = null,
        string? errorCode = null,
        string? errorMessage = null) => new(
        kind,
        "wire",
        "payload",
        ZLinkEnvelopeCodec.DefaultContentType,
        correlationId,
        null,
        null,
        errorCode,
        errorMessage);

    // Independent reference framing for the generated M6A multipart profile.
    // It intentionally does not call the production payload encoder.
    private static byte[] ReferenceMultipartFrame(params byte[][] parts)
    {
        const string packetName = "ZLinkFrameworkMultipart";
        const string contentType = "application/x-zlink-multipart";
        var packetNameBytes = Encoding.UTF8.GetBytes(packetName);
        var contentTypeBytes = Encoding.UTF8.GetBytes(contentType);
        var multipartLength = sizeof(uint) + parts.Sum(static part => sizeof(uint) + part.Length);
        var bodyLength = 1 + packetNameBytes.Length + 1 + contentTypeBytes.Length
            + sizeof(uint) + multipartLength;
        var result = new byte[sizeof(byte) + sizeof(uint) + bodyLength];
        var offset = 0;
        result[offset++] = 1;
        BinaryPrimitives.WriteUInt32BigEndian(result.AsSpan(offset), checked((uint)bodyLength));
        offset += sizeof(uint);
        result[offset++] = checked((byte)packetNameBytes.Length);
        packetNameBytes.CopyTo(result, offset);
        offset += packetNameBytes.Length;
        result[offset++] = checked((byte)contentTypeBytes.Length);
        contentTypeBytes.CopyTo(result, offset);
        offset += contentTypeBytes.Length;
        BinaryPrimitives.WriteUInt32BigEndian(result.AsSpan(offset), checked((uint)multipartLength));
        offset += sizeof(uint);
        BinaryPrimitives.WriteUInt32BigEndian(result.AsSpan(offset), checked((uint)parts.Length));
        offset += sizeof(uint);
        foreach (var part in parts)
        {
            BinaryPrimitives.WriteUInt32BigEndian(result.AsSpan(offset), checked((uint)part.Length));
            offset += sizeof(uint);
            part.CopyTo(result, offset);
            offset += part.Length;
        }

        return result;
    }

    private sealed class WireValue
    {
    }

    private sealed class FixedPartSerializer :
        IZLinkMessageSerializer,
        IZLinkMessagePartSerializer
    {
        public ZLinkEncodedPayload Serialize(object value, Type type) =>
            throw new Xunit.Sdk.XunitException("The part serializer path must not use Serialize.");

        public object? Deserialize(ZLinkEncodedPayload payload, Type type) =>
            throw new NotSupportedException();

        public Message SerializePart(object value, Type type) =>
            Message.From(new byte[] { 0xC0, 0xDE, 0x48, 0x4F, 0x54 });
    }
}
