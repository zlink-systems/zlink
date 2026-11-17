using System.Buffers;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.UnitTests;

public sealed class TypedJsonPayloadStorageTests
{
    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(3)]
    [InlineData(127)]
    [InlineData(128)]
    [InlineData(1024)]
    [InlineData(4096)]
    public void Default_typed_payload_preserves_exact_padded_base64_json_and_value(int size)
    {
        var body = Bytes(size);
        var payload = new BytePayload(body);
        var expected = Encoding.UTF8.GetBytes(
            "{\"body\":\"" + Convert.ToBase64String(body) + "\"}"
        );
        Assert.Equal(expected, ZLinkFrameworkJsonPayloadCodec.Serialize(payload));
        Assert.Equal(
            expected,
            ZLinkFrameworkJsonPayloadCodec.Serialize(payload, typeof(BytePayload))
        );
        using var genericPart = ZLinkEnvelopeCodec.EncodeJsonPart(payload);
        using var declaredPart = ZLinkEnvelopeCodec.EncodeJsonPart(payload, typeof(BytePayload));
        Assert.Equal(expected, genericPart.ToArray());
        Assert.Equal(expected, declaredPart.ToArray());

        var header = ZLinkClientCallCodec.CreateEnvelope(
            TimeProvider.System,
            ZLinkMessageKind.Command,
            "test",
            "bytes"
        );
        var parts = ZLinkClientCallCodec.EncodeEnvelopeParts(header, payload, null);
        try
        {
            Assert.Equal(expected, parts[1].ToArray());
            Assert.Equal("application/json", ZLinkEnvelopeCodec.DecodeHeader(parts[0]).ContentType);
            Assert.Equal(
                body,
                Assert
                    .IsType<BytePayload>(ZLinkEnvelopeCodec.DecodeBody(parts, typeof(BytePayload)))
                    .Body
            );
        }
        finally
        {
            foreach (var part in parts)
                part.Dispose();
        }
        Assert.Equal(body, ZLinkFrameworkJsonPayloadCodec.Deserialize<BytePayload>(expected)!.Body);
        Assert.Equal(
            body,
            Assert
                .IsType<BytePayload>(
                    ZLinkFrameworkJsonPayloadCodec.Deserialize(expected, typeof(BytePayload))
                )
                .Body
        );
        var connector = payload.ToJson();
        Assert.Equal(expected, connector.Payload.ToArray());
        Assert.Equal(body, connector.FromJson<BytePayload>().Body);
    }

    [Theory]
    [InlineData("\"\"", true)]
    [InlineData("\"AA==\"", true)]
    [InlineData("\"AAA=\"", true)]
    [InlineData("\"AAAA\"", true)]
    [InlineData("\"/w==\"", true)]
    [InlineData("\"//8=\"", true)]
    [InlineData("\"AA== \"", false)]
    [InlineData("\" AA==\"", false)]
    [InlineData("\"A A = = \"", false)]
    [InlineData("\"AAAA    \"", false)]
    [InlineData("\"AA==\\n\"", false)]
    [InlineData("\"AB==\"", false)]
    [InlineData("\"AAF=\"", false)]
    [InlineData("\"\\u0041A==\"", true)]
    [InlineData("\"\\/w==\"", true)]
    [InlineData("\"A\"", false)]
    [InlineData("\"AA\"", false)]
    [InlineData("\"AAA\"", false)]
    [InlineData("\"====\"", false)]
    [InlineData("\"A===\"", false)]
    [InlineData("\"AA=A\"", false)]
    [InlineData("\"AA==AA==\"", false)]
    [InlineData("\"!!!!\"", false)]
    [InlineData("\"AA-_\"", false)]
    [InlineData("0", false)]
    [InlineData("{}", false)]
    [InlineData("[]", false)]
    [InlineData("null", true)]
    public void Byte_decoder_enforces_canonical_base64(string json, bool accept)
    {
        var encoded = Encoding.UTF8.GetBytes(json);
        var connector = new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Json, encoded);
        if (accept)
        {
            var expected = JsonSerializer.Deserialize<byte[]>(encoded);
            Assert.Equal(expected, ZLinkFrameworkJsonPayloadCodec.Deserialize<byte[]>(encoded));
            Assert.Equal(expected, connector.FromJson<byte[]>());
        }
        else
        {
            Assert.ThrowsAny<JsonException>(() =>
                ZLinkFrameworkJsonPayloadCodec.Deserialize<byte[]>(encoded)
            );
            Assert.ThrowsAny<JsonException>(() => connector.FromJson<byte[]>());
        }
    }

    [Fact]
    public void Byte_decoder_checks_padding_bits_for_every_base64_symbol()
    {
        const string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        foreach (var prefix in new[] { "A", "AA" })
        {
            foreach (var symbol in alphabet)
            {
                var text = prefix + symbol + (prefix.Length == 1 ? "==" : "=");
                var expected = Convert.FromBase64String(text);
                var json = Encoding.UTF8.GetBytes("\"" + text + "\"");
                var connector = new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Json, json);
                if (Convert.ToBase64String(expected) == text)
                {
                    Assert.Equal(
                        expected,
                        ZLinkFrameworkJsonPayloadCodec.Deserialize<byte[]>(json)
                    );
                    Assert.Equal(expected, connector.FromJson<byte[]>());
                }
                else
                {
                    Assert.ThrowsAny<JsonException>(() =>
                        ZLinkFrameworkJsonPayloadCodec.Deserialize<byte[]>(json)
                    );
                    Assert.ThrowsAny<JsonException>(() => connector.FromJson<byte[]>());
                }
            }
        }
    }

    [Theory]
    [InlineData("\"AA==\"")]
    [InlineData("\"\\u0041A==\"")]
    [InlineData("\"/w==\"")]
    public void Byte_decoder_preserves_values_across_sequence_segments(string json)
    {
        var bytes = Encoding.UTF8.GetBytes(json);
        for (var split = 1; split < bytes.Length; split++)
        {
            var first = new Segment(bytes.AsMemory(0, split));
            var last = first.Append(bytes.AsMemory(split));
            var sequence = new ReadOnlySequence<byte>(first, 0, last, last.Memory.Length);
            var reader = new Utf8JsonReader(sequence);
            Assert.Equal(
                JsonSerializer.Deserialize<byte[]>(bytes),
                JsonSerializer.Deserialize<byte[]>(
                    ref reader,
                    ZLinkJsonSerializerOptions.FrameworkPayload
                )
            );
        }
    }

    [Fact]
    public void Pooled_part_storage_preserves_growth_escaping_and_native_snapshot()
    {
        var payload = new NestedPayload(
            "\"\\\n\u00e9\u4e2d\ud83d\ude00",
            [new(Bytes(100_000)), new(Bytes(4096))]
        );
        var expected = ZLinkFrameworkJsonPayloadCodec.Serialize(payload);
        using var part = ZLinkEnvelopeCodec.EncodeJsonPart(payload);
        for (var i = 0; i < 16; i++)
        {
            using var replacement = ZLinkEnvelopeCodec.EncodeJsonPart(
                new BytePayload(Bytes(100_000))
            );
        }
        Assert.Equal(expected, part.ToArray());
        Assert.Equal(
            payload.Text,
            ZLinkFrameworkJsonPayloadCodec.Deserialize<NestedPayload>(part.AsReadOnlySpan())!.Text
        );
    }

    [Fact]
    public void Part_serialization_failure_does_not_affect_subsequent_payloads()
    {
        Assert.Throws<InvalidOperationException>(() =>
            ZLinkEnvelopeCodec.EncodeJsonPart(new ThrowingPayload())
        );
        using var part = ZLinkEnvelopeCodec.EncodeJsonPart(new BytePayload([1, 2]));
        Assert.Equal("{\"body\":\"AQI=\"}", part.GetString());
    }

    [Fact]
    public void Native_json_part_does_not_allocate_an_owned_json_sized_array()
    {
        var payload = new BytePayload(Bytes(4096));
        const int calls = 128;
        var allocated =
            PerCallAllocation.MinWindow(
                () =>
                {
                    using var part = ZLinkEnvelopeCodec.EncodeJsonPart(
                        payload,
                        typeof(BytePayload)
                    );
                },
                calls,
                warmup: calls
            ) / calls;
        Assert.True(allocated < 1024, $"Native JSON part allocated {allocated} B/call.");
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(3)]
    [InlineData(1024)]
    [InlineData(4096)]
    public void Framework_and_connector_replies_own_decoded_bytes(int size)
    {
        var expected = Bytes(size);
        var request = new BytePayload(expected).ToJson();
        var nativeRequest = Message.From(request.Payload.Span);
        var serverValue = Assert.IsType<BytePayload>(
            ZLinkEnvelopeCodec.DecodeBody(
                nativeRequest,
                typeof(BytePayload),
                "application/json",
                null
            )
        );
        nativeRequest.Dispose();
        var responseBytes = ZLinkFrameworkJsonPayloadCodec.Serialize(serverValue);
        var connectorReply = new ZlinkStreamEncodedPayload(
            ZlinkStreamCodec.Json,
            responseBytes
        ).FromJson<BytePayload>();
        var responseHeader = ZLinkEnvelopeCodec.PrepareResponse(
            "test",
            typeof(BytePayload),
            new ZLinkCodecRegistryBuilder()
        ) with
        {
            CorrelationId = "typed-reply",
        };
        var response = ZLinkClientCallCodec.EncodeEnvelopeParts(responseHeader, serverValue, null);
        var frameworkReply = ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<BytePayload>(
            response,
            "empty",
            "error",
            null,
            validateFlow: false
        );
        Array.Fill(responseBytes, (byte)'x');
        if (
            System.Runtime.InteropServices.MemoryMarshal.TryGetArray(
                request.Payload,
                out var requestArray
            )
        )
            Array.Fill(requestArray.Array!, (byte)'x');
        Assert.Equal(expected, serverValue.Body);
        Assert.Equal(expected, connectorReply.Body);
        Assert.Equal(expected, frameworkReply.Body);
        Assert.Equal(
            new BytePayload(expected).ToJson().Payload.ToArray(),
            ZLinkFrameworkJsonPayloadCodec.Serialize(serverValue)
        );
    }

    private static byte[] Bytes(int count) =>
        Enumerable.Range(0, count).Select(i => (byte)(31 * i + 17 * (i / 251) + 29)).ToArray();

    public sealed record BytePayload(byte[] Body);

    public sealed record NestedPayload(string Text, BytePayload[] Items);

    public sealed class ThrowingPayload
    {
        [JsonPropertyOrder(0)]
        public byte[] Body { get; } = Bytes(4096);

        [JsonPropertyOrder(1)]
        public string Failure => throw new InvalidOperationException("serializer failure");
    }

    private sealed class Segment : ReadOnlySequenceSegment<byte>
    {
        internal Segment(ReadOnlyMemory<byte> memory) => Memory = memory;

        internal Segment Append(ReadOnlyMemory<byte> memory)
        {
            var next = new Segment(memory) { RunningIndex = RunningIndex + Memory.Length };
            Next = next;
            return next;
        }
    }
}
