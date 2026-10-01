using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Runtime.Codecs;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class EnvelopeCodecTests
{
    [Fact]
    public void Received_header_rejects_invalid_utf8_as_protocol_error()
    {
        var prefix = System.Text.Encoding.UTF8.GetBytes(
            "{\"formatMarker\":242,\"kind\":3,\"channelName\":\"api\",\"messageName\":\""
        );
        var suffix = System.Text.Encoding.UTF8.GetBytes(
            "\",\"contentType\":\"application/json\",\"metadata\":{}}"
        );
        byte[] bytes = [.. prefix, 0xc3, 0x28, .. suffix];
        using var wire = Message.From(bytes);
        Assert.Throws<ZLinkEnvelopeProtocolException>(() => ZLinkEnvelopeCodec.DecodeHeader(wire));
    }

    [Theory]
    [InlineData((int)ZLinkMessageKind.Command)]
    [InlineData((int)ZLinkMessageKind.Publish)]
    public void One_way_envelope_does_not_create_reply_correlation(int kind)
    {
        var header = ZLinkClientCallCodec.CreateEnvelope(
            (ZLinkMessageKind)kind,
            "channel",
            "message"
        );
        Assert.Null(header.CorrelationId);
        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
        Assert.Null(ZLinkEnvelopeCodec.DecodeHeader(encoded).CorrelationId);
    }

    [Theory]
    [InlineData(0xd800, false)]
    [InlineData(0xdfff, true)]
    public void ClientServer_metadata_rejects_unpaired_surrogates_on_send(int code, bool invalidKey)
    {
        var invalid = new string((char)code, 1);
        var key = invalidKey ? invalid : "k";
        var value = invalidKey ? "v" : invalid;
        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Command,
            "work",
            "Notice"
        ) with
        {
            Metadata = new Dictionary<string, string> { [key] = value },
        };
        Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
            ZLinkEnvelopeCodec.EncodeHeader(header)
        );
    }

    [Fact]
    public void Message_metadata_owns_an_immutable_snapshot()
    {
        var source = new Dictionary<string, string> { ["tenant-id"] = "tenant-42" };
        var metadata = new ZLinkMessageMetadata(source);
        source["tenant-id"] = "changed";
        Assert.Equal("tenant-42", metadata.Find("tenant-id"));
        Assert.Throws<NotSupportedException>(() =>
            ((IDictionary<string, string>)metadata.Values)["tenant-id"] = "changed"
        );
    }

    [Fact]
    public void ClientServer_metadata_uses_shared_minimum_escape_fixture()
    {
        var path = Path.Combine(
            Zlink.Framework.Tests.Common.FrameworkTestEnvironment.GetRepoRoot(),
            "framework/runtime/protocol/fixtures/client-server-metadata.json"
        );
        using var fixture = System.Text.Json.JsonDocument.Parse(File.ReadAllText(path));
        foreach (var scenario in fixture.RootElement.GetProperty("cases").EnumerateArray())
        {
            var name = scenario.GetProperty("name").GetString();
            var present = scenario.TryGetProperty("metadata", out var value);
            var raw =
                "{\"formatMarker\":242,\"kind\":3,\"channelName\":\"work\",\"messageName\":\"Notice\",\"contentType\":\"application/json\",\"correlationId\":null,\"deadline\":null,\"topic\":null"
                + (
                    present
                        ? ",\"metadata\":"
                            + (
                                scenario.TryGetProperty("receivedEncoded", out var received)
                                    ? received.GetString()
                                    : value.GetRawText()
                            )
                        : ""
                )
                + "}";
            using var wire = Message.From(System.Text.Encoding.UTF8.GetBytes(raw));
            if (!scenario.GetProperty("valid").GetBoolean())
            {
                Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
                    ZLinkEnvelopeCodec.DecodeHeader(wire)
                );
                if (
                    value.ValueKind == System.Text.Json.JsonValueKind.Object
                    && value
                        .EnumerateObject()
                        .All(p =>
                            p.Value.ValueKind
                                is System.Text.Json.JsonValueKind.String
                                    or System.Text.Json.JsonValueKind.Null
                        )
                )
                {
                    var outbound = ZLinkClientCallCodec.CreateEnvelope(
                        ZLinkMessageKind.Command,
                        "work",
                        "Notice"
                    ) with
                    {
                        Metadata = value
                            .EnumerateObject()
                            .ToDictionary(p => p.Name, p => p.Value.GetString()!),
                    };
                    Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
                        ZLinkEnvelopeCodec.EncodeHeader(outbound)
                    );
                }
                continue;
            }
            var header = ZLinkEnvelopeCodec.DecodeHeader(wire);
            var expected = present
                ? value.EnumerateObject().ToDictionary(p => p.Name, p => p.Value.GetString()!)
                : new Dictionary<string, string>();
            Assert.Equal(expected, header.Metadata ?? new Dictionary<string, string>());
            var outboundHeader = header with { Metadata = expected };
            using var encoded = ZLinkEnvelopeCodec.EncodeHeader(outboundHeader);
            var text = System.Text.Encoding.UTF8.GetString(encoded.AsReadOnlySpan());
            var minimum = scenario.GetProperty("encoded").GetString()!;
            if (expected.Count > 0)
                Assert.Contains("\"metadata\":" + minimum, text);
            Assert.Equal(
                scenario.GetProperty("encodedSize").GetInt32(),
                System.Text.Encoding.UTF8.GetByteCount(minimum)
            );
            Assert.Equal(
                expected,
                ZLinkEnvelopeCodec.DecodeHeader(encoded).Metadata
                    ?? new Dictionary<string, string>()
            );
        }
    }

    [Fact]
    public void Request_envelope_keeps_protocol_correlation_when_observation_is_disabled()
    {
        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Request,
            "channel",
            "request",
            includeDeadline: false
        );

        Assert.False(string.IsNullOrWhiteSpace(header.CorrelationId));
        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
    }

    [Theory]
    [InlineData(2)]
    [InlineData(5)]
    public void Client_call_envelope_rejects_reply_kinds_that_require_the_request_correlation(
        int kind
    )
    {
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkClientCallCodec.CreateEnvelope((ZLinkMessageKind)kind, "channel", "reply")
        );
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
            null
        )
        {
            FlowId = flowId,
            FlowOrigin = ZLinkFlowOrigin.Application,
        };

        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
        var decoded = ZLinkEnvelopeCodec.DecodeHeader(encoded);

        Assert.Equal(0xF2, decoded.FormatMarker);
        Assert.Equal(flowId, decoded.FlowId);
        Assert.Equal(ZLinkFlowOrigin.Application, decoded.FlowOrigin);

        using var missingMarker = Message.From(
            "{\"Kind\":3,\"ChannelName\":\"play\",\"MessageName\":\"Move\",\"ContentType\":\"application/json\"}"
        );
        Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
            ZLinkEnvelopeCodec.DecodeHeader(missingMarker)
        );
    }

    [Fact]
    public void Off_decode_strips_flow_without_validating_observation_fields()
    {
        using var encoded = Message.From(
            "{\"FormatMarker\":242,\"Kind\":3,\"ChannelName\":\"play\","
                + "\"MessageName\":\"Move\",\"ContentType\":\"application/json\","
                + "\"FlowId\":\"not-a-uuid\",\"FlowOrigin\":3}"
        );

        Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
            ZLinkEnvelopeCodec.DecodeHeader(encoded)
        );

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
        string? errorMessage
    )
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
            errorMessage
        );

        Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
            ZLinkEnvelopeCodec.EncodeHeader(header)
        );
    }

    [Fact]
    public void Envelope_accepts_the_five_canonical_kind_numbers()
    {
        Assert.Equal(
            new[] { 1, 2, 3, 4, 5 },
            Enum.GetValues<ZLinkMessageKind>().Select(static kind => (int)kind).ToArray()
        );

        foreach (
            var header in new[]
            {
                Header(ZLinkMessageKind.Request, "request-1"),
                Header(ZLinkMessageKind.Response, "request-1"),
                Header(ZLinkMessageKind.Command),
                Header(ZLinkMessageKind.Publish),
                Header(ZLinkMessageKind.Error, "request-1", "RequestFailed"),
            }
        )
        {
            using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
            Assert.Equal(header.Kind, ZLinkEnvelopeCodec.DecodeHeader(encoded).Kind);
        }

        return;

        static ZLinkEnvelopeHeader Header(
            ZLinkMessageKind kind,
            string? correlationId = null,
            string? errorCode = null
        ) =>
            new(
                kind,
                "play",
                "Move",
                ZLinkEnvelopeCodec.DefaultContentType,
                correlationId,
                null,
                null,
                errorCode,
                errorCode is null ? null : "failed"
            );
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
