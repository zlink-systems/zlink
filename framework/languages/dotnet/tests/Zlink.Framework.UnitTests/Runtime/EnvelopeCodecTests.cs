using Google.Protobuf;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Runtime.Codecs;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class EnvelopeCodecTests
{
    [Fact]
    public void Off_decode_drops_flow_fields_without_changing_protocol_correlation()
    {
        var proto = new Zlink.Framework.Runtime.Protocol.ChannelHeaderV1.ChannelHeader
        {
            Kind = Zlink.Framework.Runtime.Protocol.ChannelHeaderV1.ChannelKind.Request,
            ChannelName = "route",
            MessageName = "request",
            ContentType = "application/json",
            CorrelationId = "request-correlation",
            FlowId = "not-a-uuid",
        };
        using var input = Message.From(proto.ToByteArray());

        var decoded = ZLinkEnvelopeCodec.DecodeHeader(input, validateFlow: false);

        Assert.Null(decoded.FlowId);
        Assert.Null(decoded.FlowOrigin);
        Assert.Equal("request-correlation", decoded.CorrelationId);
    }

    [Theory]
    [InlineData((int)ZLinkMessageKind.Command)]
    [InlineData((int)ZLinkMessageKind.Publish)]
    public void One_way_envelope_keeps_correlation_absent(int kindValue)
    {
        var kind = (ZLinkMessageKind)kindValue;
        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(
            new ZLinkEnvelopeHeader(
                kind,
                "route",
                "event",
                ZLinkEnvelopeCodec.DefaultContentType,
                null,
                null,
                null,
                null,
                null
            )
        );

        var decoded = ZLinkEnvelopeCodec.DecodeHeader(encoded);

        Assert.Null(decoded.CorrelationId);
    }

    [Theory]
    [InlineData((int)ZLinkMessageKind.Response)]
    [InlineData((int)ZLinkMessageKind.Error)]
    public void Client_call_envelope_rejects_reply_kinds(int kindValue)
    {
        var kind = (ZLinkMessageKind)kindValue;
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkClientCallCodec.CreateEnvelope(TimeProvider.System, kind, "route", "message")
        );
    }

    [Fact]
    public void Message_metadata_owns_an_immutable_snapshot()
    {
        var values = new Dictionary<string, string> { ["tenant"] = "before" };
        var metadata = new Zlink.Framework.Contracts.Streams.ZLinkMessageMetadata(values);
        values["tenant"] = "after";

        Assert.Equal("before", metadata.Find("tenant"));
        Assert.Throws<NotSupportedException>(() =>
            ((IDictionary<string, string>)metadata.Values)["tenant"] = "changed"
        );
    }

    [Fact]
    public void Decode_accepts_non_spec_error_fields_and_deadline_text()
    {
        var proto = new Zlink.Framework.Runtime.Protocol.ChannelHeaderV1.ChannelHeader
        {
            Kind = Zlink.Framework.Runtime.Protocol.ChannelHeaderV1.ChannelKind.Request,
            ChannelName = "route",
            MessageName = "request",
            ContentType = "application/json",
            Deadline = "not-a-date",
            ErrorMessage = "peer field",
        };
        using var input = Message.From(proto.ToByteArray());

        var decoded = ZLinkEnvelopeCodec.DecodeHeader(input);

        Assert.Equal("not-a-date", decoded.DeadlineText);
        Assert.Null(decoded.Deadline);
        Assert.Equal("peer field", decoded.ErrorMessage);
    }

    [Fact]
    public void Encode_rejects_unpaired_surrogate_in_header_metadata()
    {
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Request,
            "route",
            "request",
            "application/json",
            "correlation",
            null,
            null,
            null,
            null
        )
        {
            Metadata = new Dictionary<string, string> { ["key"] = "\uD800" },
        };

        Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
            ZLinkEnvelopeCodec.EncodeHeader(header)
        );
    }

    [Theory]
    [InlineData(0L, "2030-01-02T03:04:05+02:00")]
    [InlineData(1L, "2030-01-02T03:04:05.0000001+02:00")]
    [InlineData(1_230_000L, "2030-01-02T03:04:05.123+02:00")]
    public void Deadline_encoding_trims_only_trailing_fractional_zeroes(long ticks, string expected)
    {
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Request,
            "route",
            "request",
            "application/json",
            "correlation",
            new DateTimeOffset(2030, 1, 2, 3, 4, 5, TimeSpan.FromHours(2)).AddTicks(ticks),
            null,
            null,
            null
        );

        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
        var parsed =
            Zlink.Framework.Runtime.Protocol.ChannelHeaderV1.ChannelHeader.Parser.ParseFrom(
                encoded.AsReadOnlySpan()
            );

        Assert.Equal(expected, parsed.Deadline);
    }

    [Fact]
    public void DecodeBody_Returns_Message_When_BodyType_Is_Message()
    {
        using var body = Message.From("raw-join-request");

        var decoded = ZLinkEnvelopeCodec.DecodeBody(
            body,
            typeof(Message),
            ZLinkEnvelopeCodec.DefaultContentType,
            null
        );

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
                new ZLinkCodecRegistryBuilder()
            )
        );

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
            ZLinkRemoteActorJoinPackets.BoundSessionBindPacketName.StartsWith(
                "__zlink.",
                StringComparison.Ordinal
            )
        );

        var encoded = ZLinkStreamHeaderCodec.Encode(
            new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                null,
                ZLinkRemoteActorJoinPackets.BoundSessionBindPacketName,
                ZlinkStreamMetadata.Empty
            )
        );

        Assert.False(encoded.IsEmpty);
    }

    [Theory]
    [InlineData(nameof(OperationCanceledException), typeof(OperationCanceledException))]
    [InlineData(nameof(TaskCanceledException), typeof(TaskCanceledException))]
    public void DecodeEnvelopeReply_Restores_Cancellation_Error(
        string errorCode,
        Type expectedExceptionType
    )
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
            "A task was canceled."
        );
        var parts = ZLinkEnvelopeCodec.EncodeParts(header, null, null, null);

        var exception = Assert.ThrowsAny<OperationCanceledException>(() =>
            ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<object>(
                parts,
                "empty",
                "failed",
                null
            )
        );

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
                null
            )
        );

        Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, exception.Kind);
    }

    [Fact]
    public void DecodeEnvelopeReply_Maps_Malformed_Header_To_Framework_Error()
    {
        var parts = ZLinkMessageParts.Create(
            Message.From("not-json"u8),
            Message.From(ReadOnlySpan<byte>.Empty)
        );

        var exception = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<object>(
                parts,
                "empty",
                "failed",
                null
            )
        );

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
            null
        );
        var parts = ZLinkMessageParts.Create(
            ZLinkEnvelopeCodec.EncodeHeader(header),
            Message.From("""{"Value":"valid-json"}""")
        );

        var exception = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<object>(
                parts,
                "empty",
                "failed",
                new ZLinkCodecRegistryBuilder()
            )
        );

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
            "remote failed"
        );
        var parts = ZLinkEnvelopeCodec.EncodeParts(header, null, null, null);

        var exception = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<object>(
                parts,
                "empty",
                "failed",
                null
            )
        );

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
            null
        );
        var parts = ZLinkEnvelopeCodec.EncodeParts(header, new object(), typeof(object), null);

        var exception = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<object>(
                parts,
                "empty",
                "failed",
                null
            )
        );

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
            null
        );
        IReadOnlyList<Message> parts = [ZLinkEnvelopeCodec.EncodeHeader(header)];

        var exception = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<object>(
                parts,
                "empty",
                "failed",
                null
            )
        );

        Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, exception.Kind);
    }
}
