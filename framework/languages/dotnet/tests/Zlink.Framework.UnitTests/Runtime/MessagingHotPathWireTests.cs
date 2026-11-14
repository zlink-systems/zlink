using System.Buffers.Binary;
using System.Text;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Service;
using StringValue = Google.Protobuf.WellKnownTypes.StringValue;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class MessagingHotPathWireTests
{
    public static IEnumerable<object?[]> ServiceApplicationHeadVectors()
    {
        (int Command, string Prefix, string Correlation)[] commands =
        [
            (16, "5A4D0110", ""),
            (17, "5A4D0111", "FEDCBA9876543210"),
            (18, "5A4D0112", ""),
            (19, "5A4D0113", "FEDCBA9876543210"),
        ];
        (string? Text, string Hex)[] channels =
        [
            ("a", "0161"),
            ("bench", "0562656E6368"),
            ("한글", "06ED959CEAB880"),
            (new string('a', 255), "FF" + string.Concat(Enumerable.Repeat("61", 255))),
            (new string('한', 85), "FF" + string.Concat(Enumerable.Repeat("ED959C", 85))),
            (
                string.Concat(Enumerable.Repeat("😀", 63)) + "한",
                "FF" + string.Concat(Enumerable.Repeat("F09F9880", 63)) + "ED959C"
            ),
        ];
        foreach (var (command, prefix, correlation) in commands)
        foreach (var metadata in new[] { false, true })
        foreach (
            var (text, hex) in command < 18 ? new (string?, string)[] { (null, "") } : channels
        )
            yield return
            [
                command,
                metadata,
                text,
                prefix + (metadata ? "01" : "00") + correlation + hex,
            ];
    }

    [Theory]
    [InlineData(
        "empty",
        "0100000042175A4C696E6B4672616D65776F726B4D756C7469706172741D6170706C69636174696F6E2F782D7A6C696E6B2D6D756C746970617274000000080000000100000000"
    )]
    [InlineData(
        "binary",
        "0100000050175A4C696E6B4672616D65776F726B4D756C7469706172741D6170706C69636174696F6E2F782D7A6C696E6B2D6D756C7469706172740000001600000003000000030102030000000000000003FF0080"
    )]
    public void Framework_multipart_matches_literal_golden_bytes(string name, string goldenHex)
    {
        byte[][] parts =
            name == "empty"
                ?
                [
                    [],
                ]
                :
                [
                    [1, 2, 3],
                    [],
                    [255, 0, 128],
                ];
        var expected = Convert.FromHexString(goldenHex);
        var malformed = (byte[])expected.Clone();
        malformed[5] = 22; // The schema packet name is 23 ASCII bytes.
        Assert.False(
            ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(malformed, out _)
        );
        using var malformedNative = Message.From(malformed);
        Assert.False(
            ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(
                malformedNative,
                out _
            )
        );
        var memories = parts.Select(static part => (ReadOnlyMemory<byte>)part).ToArray();
        Assert.Equal(
            expected,
            ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(memories)
        );
        var messages = parts.Select(static part => Message.From(part)).ToArray();
        try
        {
            Assert.Equal(
                expected,
                ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(messages)
            );
            using var native = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(
                messages
            );
            Assert.Equal(expected, native.ToArray());
            Assert.True(
                ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(
                    native,
                    out var view
                )
            );
            Assert.Equal(parts.Length, view.Count);
            for (var index = 0; index < parts.Length; index++)
                Assert.Equal(parts[index], view.GetSpan(index).ToArray());
            for (var length = 0; length < expected.Length; length++)
                Assert.False(
                    ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(
                        expected.AsMemory(0, length),
                        out _
                    )
                );
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(messages);
        }
    }

    [Theory]
    [MemberData(nameof(ServiceApplicationHeadVectors))]
    public void Service_application_heads_match_independent_golden_bytes(
        int commandId,
        bool metadata,
        string? channel,
        string goldenHex
    )
    {
        var command = (ServiceWireConstants.Command)commandId;
        var correlation = commandId is 17 or 19 ? 0xFEDCBA9876543210UL : 0;
        var expected = Convert.FromHexString(goldenHex);
        Assert.Equal(
            expected,
            ZLinkServiceWireCodec.EncodeApplication(command, correlation, channel, metadata)
        );
        using var native = ZLinkServiceWireCodec.EncodeApplicationMessage(
            command,
            correlation,
            channel,
            metadata
        );
        Assert.Equal(expected, native.ToArray());
        Assert.True(ZLinkServiceWireCodec.TryDecodeApplication(expected, out var decoded, out _));
        Assert.Equal(channel, decoded.ChannelName);
        Assert.Equal(correlation, decoded.Correlation);
        Assert.Equal(metadata, decoded.HasMetadata);
        for (var length = 0; length < expected.Length; length++)
            Assert.False(
                ZLinkServiceWireCodec.TryDecodeApplication(expected.AsSpan(0, length), out _, out _)
            );
        Assert.False(ZLinkServiceWireCodec.TryDecodeApplication([.. expected, 0], out _, out _));
    }

    [Theory]
    [InlineData(0, 0u, "", "0000000000000000")]
    [InlineData(0, 0u, "0001FF", "00000000000000000001FF")]
    [InlineData(101, 0u, "", "0000006500000000")]
    [InlineData(102, 1u, "", "0000006600000001")]
    [InlineData(105, 17u, "", "0000006900000011")]
    [InlineData(104, 16u, "", "0000006800000010")]
    public void Service_reply_heads_match_independent_golden_bytes(
        int terminal,
        uint failure,
        string tailHex,
        string fieldsHex
    )
    {
        const ulong correlation = 0xFEDCBA9876543210UL;
        var tail = Convert.FromHexString(tailHex);
        var expected = Convert.FromHexString("5A4D011400FEDCBA9876543210" + fieldsHex);
        Assert.Equal(
            expected,
            ZLinkServiceWireCodec.EncodeReply(correlation, terminal, failure, tail)
        );
        using var native = ZLinkServiceWireCodec.EncodeReplyMessage(
            correlation,
            terminal,
            failure,
            tail
        );
        Assert.Equal(expected, native.ToArray());
        Assert.True(ZLinkServiceWireCodec.TryDecodeReply(expected, out var decoded, out _));
        Assert.Equal(tail, decoded.Tail.ToArray());
        for (var length = 0; length < 21; length++)
            Assert.False(
                ZLinkServiceWireCodec.TryDecodeReply(expected.AsSpan(0, length), out _, out _)
            );
    }

    public static IEnumerable<object?[]> InvalidServiceText()
    {
        foreach (
            var text in new[]
            {
                null,
                "",
                "\0",
                "a\0b",
                "\uD800",
                "\uDC00",
                new string('a', 256),
                new string('한', 86),
                new string('a', 254) + "한",
            }
        )
            yield return [text];
    }

    [Theory]
    [MemberData(nameof(InvalidServiceText))]
    public void Service_text_rejection_is_shared_by_array_and_native_encoders(string? text)
    {
        Assert.ThrowsAny<ArgumentException>(() => ZLinkServiceWireCodec.EncodeText(text!));
        foreach (
            var command in new[]
            {
                ServiceWireConstants.Command.ChannelSend,
                ServiceWireConstants.Command.ChannelRequest,
            }
        )
        {
            var correlation = command == ServiceWireConstants.Command.ChannelRequest ? 1UL : 0;
            Assert.ThrowsAny<ArgumentException>(() =>
                ZLinkServiceWireCodec.EncodeApplication(command, correlation, text, false)
            );
            Assert.ThrowsAny<ArgumentException>(() =>
                ZLinkServiceWireCodec.EncodeApplicationMessage(command, correlation, text, false)
            );
        }
    }

    [Theory]
    [InlineData(16, 1UL)]
    [InlineData(17, 0UL)]
    [InlineData(18, 1UL)]
    [InlineData(19, 0UL)]
    [InlineData(20, 0UL)]
    public void Service_application_rejects_invalid_command_or_correlation(
        int commandId,
        ulong correlation
    )
    {
        var command = (ServiceWireConstants.Command)commandId;
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeApplication(command, correlation, "bench", false)
        );
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeApplicationMessage(command, correlation, "bench", false)
        );
    }

    [Theory]
    [InlineData(0UL, 0, 0u)]
    [InlineData(1UL, -1, 0u)]
    [InlineData(1UL, 0, 1u)]
    [InlineData(1UL, 102, 0u)]
    [InlineData(1UL, 105, 1u)]
    [InlineData(1UL, 104, 999u)]
    public void Service_reply_rejection_is_shared_by_array_and_native_encoders(
        ulong correlation,
        int terminal,
        uint failure
    )
    {
        Assert.ThrowsAny<ArgumentException>(() =>
            ZLinkServiceWireCodec.EncodeReply(correlation, terminal, failure)
        );
        Assert.ThrowsAny<ArgumentException>(() =>
            ZLinkServiceWireCodec.EncodeReplyMessage(correlation, terminal, failure)
        );
    }

    [Theory]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(3)]
    [InlineData(17)]
    public void BorrowedPayloadPreservesAllPartRangesAndOwnedMemory(int count)
    {
        var parts = Enumerable
            .Range(0, count)
            .Select(index => Message.From(new byte[index]))
            .ToArray();
        try
        {
            using var frame = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(
                parts
            );
            Assert.True(
                ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(
                    frame,
                    out var view
                )
            );
            Assert.Equal(count, view.Count);
            for (var index = 0; index < count; index++)
            {
                Assert.Equal(index, view.GetSpan(index).Length);
                Assert.True(view.GetSpan(index).Overlaps(frame.AsReadOnlySpan()) || index == 0);
            }
            Assert.Throws<ArgumentOutOfRangeException>(() => view.GetSpan(count));
            var retained = view.GetMemory(count - 1);
            frame.Dispose();
            Assert.Equal(new byte[count - 1], retained.ToArray());
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    [Fact]
    public void BorrowedTwoPartPayloadDecodingDoesNotAllocate()
    {
        using var header = Message.From("header"u8);
        using var body = Message.From(new byte[1024]);
        using var frame = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage([
            header,
            body,
        ]);
        for (var index = 0; index < 32; index++)
            ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(frame, out _);

        var allocated = PerCallAllocation.MinWindow(
            () =>
            {
                if (
                    !ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(
                        frame,
                        out var view
                    )
                    || view.Count != 2
                    || view.GetSpan(1).Length != 1024
                )
                    throw new InvalidOperationException(
                        "The borrowed payload must preserve both parts."
                    );
            },
            1024
        );
        Assert.Equal(0L, allocated);
    }

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
        yield return
        [
            (int)ZLinkMessageKind.Error,
            "corr-fixed",
            "Failed",
            "fixed failure",
            ErrorHeader,
        ];
    }

    [Theory]
    [MemberData(nameof(HeaderVectors))]
    public void Envelope_header_matches_fixed_wire_vector(
        int kind,
        string? correlationId,
        string? errorCode,
        string? errorMessage,
        string expectedJson
    )
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
        var payload = Enumerable
            .Range(0, payloadSize)
            .Select(static index => (byte)(index % 251))
            .ToArray();
        var headerBytes = Encoding.UTF8.GetBytes(RequestHeader);
        using var header = Message.From(headerBytes);
        using var body = Message.From(payload);

        var encoded = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart([header, body]);

        Assert.Equal(ReferenceMultipartFrame(headerBytes, payload), encoded);
        Assert.Equal(
            ReferenceMultipartFrame(headerBytes, payload),
            ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(
                new ReadOnlyMemory<byte>[] { headerBytes, payload }
            )
        );
        using var native = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage([
            header,
            body,
        ]);
        Assert.Equal(encoded, native.ToArray());
        Assert.True(
            ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(
                native,
                out var view
            )
        );
        Assert.Equal(headerBytes, view.GetSpan(0).ToArray());
        Assert.Equal(payload, view.GetSpan(1).ToArray());
        Assert.True(
            ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(
                encoded,
                out var decoded
            )
        );
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
            codecs
        );
        try
        {
            Assert.Equal(Encoding.UTF8.GetBytes(ProtobufHeader), parts[0].ToArray());
            Assert.Equal(
                new byte[] { 0x0A, 0x04, (byte)'w', (byte)'i', (byte)'r', (byte)'e' },
                parts[1].ToArray()
            );
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
            Deadline = new DateTimeOffset(2026, 9, 10, 12, 34, 56, TimeSpan.FromHours(9)).AddTicks(
                1234567
            ),
            Topic = "topic/one",
            Source = "node-a",
            FlowId = "0196f7c2-4cb4-7cc8-89d4-2d6aee6fca2d",
            FlowOrigin = ZLinkFlowOrigin.Application,
            Metadata = new Dictionary<string, string>()
            {
                ["first"] = "a\"b",
                ["second"] = "한글<&",
            },
        };
        const string expected =
            """{"formatMarker":242,"kind":1,"channelName":"wire","messageName":"payload","contentType":"application/json","correlationId":"0000000000000048","deadline":"2026-09-10T12:34:56.1234567+09:00","topic":"topic/one","errorCode":null,"errorMessage":null,"source":"node-a","flowId":"0196f7c2-4cb4-7cc8-89d4-2d6aee6fca2d","flowOrigin":3,"metadata":{"first":"a\"b","second":"한글<&"}}""";

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
            "{\"text\":\""
                + new string('x', length)
                + "\",\"signed\":\"-9223372036854775808\",\"unsigned\":\"18446744073709551615\"}"
        );
        var parts = ZLinkEnvelopeCodec.EncodeParts(
            Header(ZLinkMessageKind.Request, "corr-fixed"),
            value,
            typeof(JsonWireValue),
            null
        );
        try
        {
            Assert.Equal(Encoding.UTF8.GetBytes(RequestHeader), parts[0].ToArray());
            Assert.Equal(expectedBody, parts[1].ToArray());
            using var wire = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(
                parts
            );
            Assert.Equal(
                ReferenceMultipartFrame(Encoding.UTF8.GetBytes(RequestHeader), expectedBody),
                wire.ToArray()
            );
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
        codecs.AddSerializer(
            "application/x-wire-part",
            new FixedPartSerializer(),
            static type => type == typeof(WireValue)
        );

        var parts = ZLinkEnvelopeCodec.EncodeParts(
            Header(ZLinkMessageKind.Request, "corr-fixed"),
            new WireValue(),
            typeof(WireValue),
            codecs
        );
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
        string? errorMessage = null
    ) =>
        new(
            kind,
            "wire",
            "payload",
            ZLinkEnvelopeCodec.DefaultContentType,
            correlationId,
            null,
            null,
            errorCode,
            errorMessage
        );

    // Independent reference framing for the generated M6A multipart profile.
    // It intentionally does not call the production payload encoder.
    private static byte[] ReferenceMultipartFrame(params byte[][] parts)
    {
        const string packetName = "ZLinkFrameworkMultipart";
        const string contentType = "application/x-zlink-multipart";
        var packetNameBytes = Encoding.UTF8.GetBytes(packetName);
        var contentTypeBytes = Encoding.UTF8.GetBytes(contentType);
        var multipartLength = sizeof(uint) + parts.Sum(static part => sizeof(uint) + part.Length);
        var bodyLength =
            1
            + packetNameBytes.Length
            + 1
            + contentTypeBytes.Length
            + sizeof(uint)
            + multipartLength;
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
        BinaryPrimitives.WriteUInt32BigEndian(
            result.AsSpan(offset),
            checked((uint)multipartLength)
        );
        offset += sizeof(uint);
        BinaryPrimitives.WriteUInt32BigEndian(result.AsSpan(offset), checked((uint)parts.Length));
        offset += sizeof(uint);
        foreach (var part in parts)
        {
            BinaryPrimitives.WriteUInt32BigEndian(
                result.AsSpan(offset),
                checked((uint)part.Length)
            );
            offset += sizeof(uint);
            part.CopyTo(result, offset);
            offset += part.Length;
        }

        return result;
    }

    private sealed class WireValue { }

    [Fact]
    public void Wrapped_typed_message_uses_the_same_owned_part_serializer_as_direct_envelopes()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer(
            "application/x-wire-part",
            new FixedPartSerializer(),
            static type => type == typeof(WireValue)
        );
        var value = ZLinkMessage.From(new WireValue());
        using var raw = value.ToRawMessage(codecs);
        var parts = ZLinkEnvelopeCodec.EncodeParts(
            Header(ZLinkMessageKind.Request, "corr-fixed"),
            value,
            typeof(ZLinkMessage),
            codecs
        );
        try
        {
            Assert.Equal(new byte[] { 0xC0, 0xDE, 0x48, 0x4F, 0x54 }, raw.ToArray());
            Assert.Equal(raw.ToArray(), parts[1].ToArray());
            Assert.Equal(Encoding.UTF8.GetBytes(PartSerializerHeader), parts[0].ToArray());
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    private sealed class FixedPartSerializer : IZLinkMessageSerializer, IZLinkMessagePartSerializer
    {
        public ZLinkEncodedPayload Serialize(object value, Type type) =>
            throw new Xunit.Sdk.XunitException("The part serializer path must not use Serialize.");

        public object? Deserialize(ZLinkEncodedPayload payload, Type type) =>
            throw new NotSupportedException();

        public Message SerializePart(object value, Type type) =>
            Message.From(new byte[] { 0xC0, 0xDE, 0x48, 0x4F, 0x54 });
    }
}
