using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Runtime.Codecs;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class EnvelopeCodecTests
{
    [Fact]
    public void Request_envelope_keeps_protocol_correlation_when_observation_is_disabled()
    {
        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Request,
            "channel",
            "request",
            includeCorrelationId: false,
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
