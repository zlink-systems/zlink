using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Runtime.Codecs;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class EnvelopeCodecTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Native_and_view_body_paths_reject_unknown_serializer_before_returning_raw_parts(bool viewBody)
    {
        using var header = Message.From("{}");
        using var body = Message.From("{}");
        using var packed = Zlink.Framework.Runtime.Spots.ZLinkApplicationPayloadEnvelopeCodec
            .EncodeFrameworkMultipartMessage([header, body]);
        Assert.True(Zlink.Framework.Runtime.Spots.ZLinkApplicationPayloadEnvelopeCodec
            .TryDecodeFrameworkMultipartView(packed, out var view));
        var error = viewBody
            ? Assert.Throws<ZLinkFrameworkException>(() => ZLinkEnvelopeCodec.DecodeBody(
                view!, typeof(Message), "application/x-unregistered", new ZLinkCodecRegistryBuilder()))
            : Assert.Throws<ZLinkFrameworkException>(() => ZLinkEnvelopeCodec.DecodeBody(
                body, typeof(Message), "application/x-unregistered", new ZLinkCodecRegistryBuilder()));
        Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, error.Kind);
        Assert.Null(error.InnerException);
    }

    [Theory]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(3)]
    [InlineData(4)]
    [InlineData(5)]
    public void Header_output_omits_only_absent_optional_fields(int kind)
    {
        var header = new ZLinkEnvelopeHeader((ZLinkMessageKind)kind, "wire", "payload",
            "application/json", kind is 1 or 2 or 5 ? "output-corr" : null,
            null, null, kind == 5 ? "Failed" : null, null) { FormatMarker = 242 };
        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
        Assert.Equal(ZLinkEnvelopeCodec.EncodeProtocolJsonBytes(header), encoded.ToArray());
        using var json = System.Text.Json.JsonDocument.Parse(encoded.ToArray());
        foreach (var name in new[] { "errorMessage", "source", "flowId", "flowOrigin" })
            Assert.False(json.RootElement.TryGetProperty(name, out _), name);
        Assert.Equal(kind == 5, json.RootElement.TryGetProperty("errorCode", out var errorCode));
        if (kind == 5) Assert.Equal("Failed", errorCode.GetString());
        foreach (var name in new[] { "correlationId", "deadline", "topic" })
            Assert.True(json.RootElement.TryGetProperty(name, out _), name);
        var decoded = ZLinkEnvelopeCodec.DecodeHeader(encoded);
        Assert.Equal(header.Kind, decoded.Kind);
        Assert.Equal(header.CorrelationId, decoded.CorrelationId);
        Assert.Equal(header.ErrorCode, decoded.ErrorCode);
    }

    [Theory]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(3)]
    [InlineData(4)]
    [InlineData(5)]
    public void Header_decode_preserves_values_when_optional_null_fields_are_absent(int kind)
    {
        var fields = new Dictionary<string, object?>
        {
            ["formatMarker"] = 242,
            ["kind"] = kind,
            ["channelName"] = "wire",
            ["messageName"] = "payload",
            ["contentType"] = "application/json",
            ["correlationId"] = kind is 1 or 2 or 5 ? "compat-corr" : null,
            ["deadline"] = null,
            ["topic"] = null,
            ["errorCode"] = kind == 5 ? "Failed" : null,
            ["errorMessage"] = null,
            ["source"] = null,
            ["flowId"] = null,
            ["flowOrigin"] = null
        };
        using var explicitNulls = Message.From(System.Text.Json.JsonSerializer.SerializeToUtf8Bytes(fields));
        foreach (var name in new[] { "errorCode", "errorMessage", "source", "flowId", "flowOrigin" })
            if (fields[name] is null) fields.Remove(name);
        using var absentNulls = Message.From(System.Text.Json.JsonSerializer.SerializeToUtf8Bytes(fields));

        Assert.Equal(ZLinkEnvelopeCodec.DecodeHeader(explicitNulls), ZLinkEnvelopeCodec.DecodeHeader(absentNulls));
    }

    [Theory]
    [InlineData("KiNd")]
    [InlineData("\\u006b\\u0069\\u006e\\u0064")]
    public void HeaderPropertyNamesPreserveCaseInsensitiveAndEscapedMatching(string kindProperty)
    {
        using var input = Message.From(System.Text.Encoding.UTF8.GetBytes(
            $"{{\"formatMarker\":242,\"{kindProperty}\":1,\"channelName\":\"channel\","
            + "\"messageName\":\"request\",\"contentType\":\"application/json\",\"correlationId\":\"corr\"}"));
        var header = ZLinkEnvelopeCodec.DecodeHeader(input);
        Assert.Equal(ZLinkMessageKind.Request, header.Kind);
        Assert.Equal("channel", header.ChannelName);
        Assert.Equal("corr", header.CorrelationId);
    }

    [Theory]
    [InlineData(255)]
    [InlineData(128)]
    [InlineData(192)]
    public void HeaderRejectsInvalidUtf8EvenInUnknownPropertyNames(int invalidByte)
    {
        var bytes = System.Text.Encoding.UTF8.GetBytes(
            "{\"x\":null,\"formatMarker\":242,\"kind\":1,\"channelName\":\"channel\","
            + "\"messageName\":\"request\",\"contentType\":\"application/json\",\"correlationId\":\"corr\"}");
        bytes[2] = (byte)invalidByte;
        using var input = Message.From(bytes);
        Assert.Throws<ZLinkEnvelopeProtocolException>(() => ZLinkEnvelopeCodec.DecodeHeader(input));
    }

    [Fact]
    public void Request_envelope_keeps_protocol_correlation_when_observation_is_disabled()
    {
        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Request,
            "channel",
            "request",
            includeDeadline: false);

        Assert.False(string.IsNullOrWhiteSpace(header.CorrelationId));
        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
    }

    [Theory]
    [InlineData(2)]
    [InlineData(5)]
    public void Client_call_envelope_rejects_reply_kinds_that_require_the_request_correlation(
        int kind)
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => ZLinkClientCallCodec.CreateEnvelope(
            (ZLinkMessageKind)kind,
            "channel",
            "reply"));
    }

    [Fact]
    public void Envelope_requires_marker_and_roundtrips_flow_fields()
    {
        var flowId = "0196f7c2-4cb4-7cc8-89d4-2d6aee6fca2d";
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Command,
            "play",
            "Move",
            ZLinkEnvelopeCodec.DefaultContentType,
            null,
            null,
            null,
            null,
            null)
        {
            FlowId = flowId,
            FlowOrigin = ZLinkFlowOrigin.Application
        };

        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
        var decoded = ZLinkEnvelopeCodec.DecodeHeader(encoded);

        Assert.Equal(0xF2, decoded.FormatMarker);
        Assert.Equal(flowId, decoded.FlowId);
        Assert.Equal(ZLinkFlowOrigin.Application, decoded.FlowOrigin);

        using var missingMarker = Message.From(
            "{\"Kind\":3,\"ChannelName\":\"play\",\"MessageName\":\"Move\",\"ContentType\":\"application/json\"}");
        Assert.Throws<ZLinkEnvelopeProtocolException>(() => ZLinkEnvelopeCodec.DecodeHeader(missingMarker));
    }

    [Fact]
    public void Encoding_failure_keeps_canonical_wire_fields_without_mutating_input()
    {
        var header = new ZLinkEnvelopeHeader(ZLinkMessageKind.Command, "play", "Move",
            ZLinkEnvelopeCodec.DefaultContentType, null, null, null, null, null)
        {
            FlowId = "invalid-flow",
            FlowOrigin = ZLinkFlowOrigin.Application
        };
        var error = Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
        {
            using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header, "application/x-protobuf");
        });
        Assert.Equal((byte)0xF2, error.Header.FormatMarker);
        Assert.Equal("application/x-protobuf", error.Header.ContentType);
        Assert.Equal(header.FlowId, error.Header.FlowId);
        Assert.Equal(header.FlowOrigin, error.Header.FlowOrigin);
        Assert.Equal((byte)0, header.FormatMarker);
        Assert.Equal(ZLinkEnvelopeCodec.DefaultContentType, header.ContentType);
    }

    [Fact]
    public void Off_decode_strips_flow_without_validating_observation_fields()
    {
        using var encoded = Message.From(
            "{\"FormatMarker\":242,\"Kind\":3,\"ChannelName\":\"play\","
            + "\"MessageName\":\"Move\",\"ContentType\":\"application/json\","
            + "\"FlowId\":\"not-a-uuid\",\"FlowOrigin\":3}");

        Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
            ZLinkEnvelopeCodec.DecodeHeader(encoded));

        var decoded = ZLinkEnvelopeCodec.DecodeHeader(encoded, validateFlow: false);
        Assert.Null(decoded.FlowId);
        Assert.Null(decoded.FlowOrigin);
    }

    [Theory]
    [InlineData(1, null, null, null)]
    [InlineData(2, null, null, null)]
    [InlineData(5, "request-1", null, "failed")]
    [InlineData(3, null, "Invalid", null)]
    [InlineData(4, null, null, "failed")]
    public void Envelope_rejects_noncanonical_kind_field_combinations(
        int kind,
        string? correlationId,
        string? errorCode,
        string? errorMessage)
    {
        var header = new ZLinkEnvelopeHeader(
            (ZLinkMessageKind)kind,
            "play",
            "Move",
            ZLinkEnvelopeCodec.DefaultContentType,
            correlationId,
            null,
            null,
            errorCode,
            errorMessage);

        Assert.Throws<ZLinkEnvelopeProtocolException>(() => ZLinkEnvelopeCodec.EncodeHeader(header));
    }

    [Fact]
    public void Envelope_accepts_the_five_canonical_kind_numbers()
    {
        Assert.Equal(
            new[] { 1, 2, 3, 4, 5 },
            Enum.GetValues<ZLinkMessageKind>().Select(static kind => (int)kind).ToArray());

        foreach (var header in new[]
                 {
                     Header(ZLinkMessageKind.Request, "request-1"),
                     Header(ZLinkMessageKind.Response, "request-1"),
                     Header(ZLinkMessageKind.Command),
                     Header(ZLinkMessageKind.Publish),
                     Header(ZLinkMessageKind.Error, "request-1", "RequestFailed")
                 })
        {
            using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
            Assert.Equal(header.Kind, ZLinkEnvelopeCodec.DecodeHeader(encoded).Kind);
        }

        return;

        static ZLinkEnvelopeHeader Header(
            ZLinkMessageKind kind,
            string? correlationId = null,
            string? errorCode = null) => new(
            kind,
            "play",
            "Move",
            ZLinkEnvelopeCodec.DefaultContentType,
            correlationId,
            null,
            null,
            errorCode,
            errorCode is null ? null : "failed");
    }

    [Fact]
    public void DecodeBody_Returns_Message_When_BodyType_Is_Message()
    {
        using var body = Message.From("raw-join-request");

        var decoded = ZLinkEnvelopeCodec.DecodeBody(
            body,
            typeof(Message),
            ZLinkEnvelopeCodec.DefaultContentType,
            null);

        Assert.Same(body, decoded);
    }

    [Fact]
    public void DecodeBody_Rejects_Unregistered_NonJson_ContentType_Before_Json_Decode()
    {
        using var body = Message.From("""{"Value":"valid-json"}""");

        var exception = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkEnvelopeCodec.DecodeBody(
                body,
                typeof(object),
                "application/x-unregistered",
                new ZLinkCodecRegistryBuilder()));

        Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, exception.Kind);
        Assert.Null(exception.InnerException);
    }

    [Fact]
    public void EncodeBody_Copies_Message_When_BodyType_Is_Message()
    {
        using var body = Message.From("raw-join-reply");
        using var encoded = ZLinkEnvelopeCodec.EncodeBody(body, typeof(Message), null);

        Assert.NotSame(body, encoded);
        Assert.Equal(body.ToArray(), encoded.ToArray());
    }

    [Fact]
    public void BoundSessionBindPacketName_Can_Be_Encoded_As_Stream_Send()
    {
        Assert.False(
            ZLinkRemoteActorJoinPackets.BoundSessionBindPacketName.StartsWith("__zlink.", StringComparison.Ordinal));

        var encoded = ZLinkStreamHeaderCodec.Encode(
            new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                null,
                ZLinkRemoteActorJoinPackets.BoundSessionBindPacketName,
                ZlinkStreamMetadata.Empty));

        Assert.False(encoded.IsEmpty);
    }

    [Theory]
    [InlineData(nameof(OperationCanceledException), typeof(OperationCanceledException))]
    [InlineData(nameof(TaskCanceledException), typeof(TaskCanceledException))]
    public void DecodeEnvelopeReply_Restores_Cancellation_Error(
        string errorCode,
        Type expectedExceptionType)
    {
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Error,
            "yield.route",
            "YieldReq",
            ZLinkEnvelopeCodec.DefaultContentType,
            "cancelled",
            null,
            null,
            errorCode,
            "A task was canceled.");
        var parts = ZLinkEnvelopeCodec.EncodeParts(header, null, null, null);

        var exception = Assert.ThrowsAny<OperationCanceledException>(
            () => ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<object>(
                parts,
                "empty",
                "failed",
                null));

        Assert.IsType(expectedExceptionType, exception);
        Assert.Equal("A task was canceled.", exception.Message);
    }

    [Fact]
    public void DecodeEnvelopeReply_Maps_Empty_Reply_To_Framework_Error()
    {
        var exception = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<object>(
                [],
                "reply was empty",
                "failed",
                null));

        Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, exception.Kind);
    }

    [Fact]
    public void DecodeEnvelopeReply_Maps_Malformed_Header_To_Framework_Error()
    {
        var parts = ZLinkMessageParts.Create(
            Message.From("not-json"u8),
            Message.From(ReadOnlySpan<byte>.Empty));

        var exception = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<object>(
                parts,
                "empty",
                "failed",
                null));

        Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, exception.Kind);
    }

    [Fact]
    public void DecodeEnvelopeReply_Rejects_Unregistered_NonJson_ContentType_As_PayloadDecodeFailed()
    {
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Response,
            "route",
            "Reply",
            "application/x-unregistered",
            "correlation",
            null,
            null,
            null,
            null);
        var parts = ZLinkMessageParts.Create(
            ZLinkEnvelopeCodec.EncodeHeader(header),
            Message.From("""{"Value":"valid-json"}"""));

        var exception = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<object>(
                parts,
                "empty",
                "failed",
                new ZLinkCodecRegistryBuilder()));

        Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, exception.Kind);
        Assert.Null(exception.InnerException);
    }

    [Theory]
    [InlineData("UnknownRemoteError")]
    [InlineData("999")]
    public void DecodeEnvelopeReply_Maps_Unknown_Remote_Error_To_Framework_Error(string errorCode)
    {
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Error,
            "route",
            "Request",
            ZLinkEnvelopeCodec.DefaultContentType,
            "correlation",
            null,
            null,
            errorCode,
            "remote failed");
        var parts = ZLinkEnvelopeCodec.EncodeParts(header, null, null, null);

        var exception = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<object>(
                parts,
                "empty",
                "failed",
                null));

        Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, exception.Kind);
    }

    [Fact]
    public void DecodeEnvelopeReply_Maps_NonResponse_Kind_To_Protocol_Error()
    {
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Command,
            "route",
            "Message",
            ZLinkEnvelopeCodec.DefaultContentType,
            null,
            null,
            null,
            null,
            null);
        var parts = ZLinkEnvelopeCodec.EncodeParts(header, new object(), typeof(object), null);

        var exception = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<object>(
                parts,
                "empty",
                "failed",
                null));

        Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, exception.Kind);
    }

    [Fact]
    public void DecodeEnvelopeReply_Maps_Missing_Response_Body_To_Protocol_Error()
    {
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Response,
            "route",
            "Reply",
            ZLinkEnvelopeCodec.DefaultContentType,
            "correlation",
            null,
            null,
            null,
            null);
        IReadOnlyList<Message> parts = [ZLinkEnvelopeCodec.EncodeHeader(header)];

        var exception = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<object>(
                parts,
                "empty",
                "failed",
                null));

        Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, exception.Kind);
    }

}
