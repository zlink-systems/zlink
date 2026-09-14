using Google.Protobuf;
using Google.Protobuf.Reflection;
using Google.Protobuf.WellKnownTypes;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Contracts.Codecs;
using Zlink.Framework.Runtime.Codecs;
using Xunit.Abstractions;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ProtobufNativePayloadTests(ITestOutputHelper output)
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Both_deserializer_inputs_preserve_the_null_type_error(bool spanInput)
    {
        var codecs = Codecs();
        Assert.True(codecs.TryGetSerializer("application/x-protobuf", out var serializer));
        var error = spanInput
            ? Assert.Throws<InvalidOperationException>(() =>
                ((IZLinkMessageSpanDeserializer)serializer).Deserialize(ReadOnlySpan<byte>.Empty, null!))
            : Assert.Throws<InvalidOperationException>(() =>
                serializer.Deserialize(ZLinkEncodedPayload.From(Array.Empty<byte>()), null!));
        Assert.Equal("Protobuf codec cannot deserialize payload type ''.", error.Message);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Both_deserializer_inputs_reject_an_incompatible_declared_type(bool spanInput)
    {
        var codecs = Codecs();
        Assert.True(codecs.TryGetSerializer("application/x-protobuf", out var serializer));
        for (var attempt = 0; attempt < 2; attempt++)
        {
            var error = spanInput
                ? Assert.Throws<InvalidOperationException>(() =>
                    ((IZLinkMessageSpanDeserializer)serializer).Deserialize(ReadOnlySpan<byte>.Empty, typeof(object)))
                : Assert.Throws<InvalidOperationException>(() =>
                    serializer.Deserialize(ZLinkEncodedPayload.From(Array.Empty<byte>()), typeof(object)));
            Assert.Equal("Protobuf codec cannot deserialize payload type 'System.Object'.", error.Message);
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Both_serializer_outputs_reject_an_incompatible_declared_type(bool nativePart)
    {
        var codecs = Codecs();
        Assert.True(codecs.TryGetSerializer("application/x-protobuf", out var serializer));
        var value = new BytesValue();
        var error = nativePart
            ? Assert.Throws<InvalidOperationException>(() =>
                ((IZLinkMessagePartSerializer)serializer).SerializePart(value, typeof(object)))
            : Assert.Throws<InvalidOperationException>(() => serializer.Serialize(value, typeof(object)));
        Assert.Equal("Protobuf codec cannot serialize payload type 'System.Object'.", error.Message);
    }

    [Theory]
    [InlineData(64)]
    [InlineData(4096)]
    public void Native_bytes_writer_does_not_allocate_a_default_stream_writer_buffer(int size)
    {
        const int iterations = 64;
        var codecs = Codecs();
        var value = new BytesValue { Value = ByteString.CopyFrom(new byte[size]) };
        for (var index = 0; index < iterations; index++)
        {
            using var warmup = ZLinkEnvelopeCodec.EncodeBody(value, typeof(BytesValue), codecs);
        }
        var before = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < iterations; index++)
        {
            using var part = ZLinkEnvelopeCodec.EncodeBody(value, typeof(BytesValue), codecs);
        }
        var perEncode = (GC.GetAllocatedBytesForCurrentThread() - before) / iterations;
        output.WriteLine($"payload bytes={size}; managed bytes per encode={perEncode}");
        Assert.True(perEncode < 1024, $"managed bytes per encode: {perEncode}");
    }

    [Theory]
    [InlineData(127)]
    [InlineData(128)]
    [InlineData(129)]
    [InlineData(4095)]
    [InlineData(4096)]
    [InlineData(4097)]
    public void Native_writer_preserves_string_bytes_across_buffer_boundaries(int length)
    {
        var value = new StringValue { Value = string.Concat(Enumerable.Repeat("한😀", length)) };
        var expected = value.ToByteArray();
        using var part = ZLinkEnvelopeCodec.EncodeBody(value, typeof(StringValue), Codecs());
        Assert.Equal(expected, part.ToArray());
    }

    [Fact]
    public void Repeated_string_encoding_does_not_add_a_payload_sized_scratch_graph()
    {
        const int iterations = 32;
        var codecs = Codecs();
        var value = new ListValue();
        for (var index = 0; index < 64; index++)
            value.Values.Add(new Value { StringValue = new string('x', 256) });
        var expected = value.ToByteArray();
        for (var index = 0; index < iterations; index++)
        {
            using var warmup = ZLinkEnvelopeCodec.EncodeBody(value, typeof(ListValue), codecs);
            Assert.Equal(expected, warmup.ToArray());
        }

        var before = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < iterations; index++)
        {
            using var part = ZLinkEnvelopeCodec.EncodeBody(value, typeof(ListValue), codecs);
        }
        var allocated = GC.GetAllocatedBytesForCurrentThread() - before;
        output.WriteLine($"managed bytes per encode={allocated / iterations}; wire bytes={expected.Length}");
        Assert.True(allocated < (long)expected.Length * iterations / 2,
            $"managed bytes per encode: {allocated / iterations}; wire bytes: {expected.Length}");
    }

    [Fact]
    public void Envelope_encode_does_not_allocate_a_managed_payload_sized_buffer()
    {
        const int size = 262144;
        const int iterations = 16;
        var codecs = Codecs();
        var value = new BytesValue { Value = ByteString.CopyFrom(new byte[size]) };
        var expected = value.ToByteArray();
        for (var index = 0; index < iterations; index++)
        {
            using var warmup = ZLinkEnvelopeCodec.EncodeBody(value, typeof(BytesValue), codecs);
            Assert.Equal(expected, warmup.ToArray());
        }
        var before = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < iterations; index++)
        {
            using var part = ZLinkEnvelopeCodec.EncodeBody(value, typeof(BytesValue), codecs);
            Assert.Equal(expected.Length, part.Size);
        }
        var allocated = GC.GetAllocatedBytesForCurrentThread() - before;
        Assert.True(allocated < (long)size * iterations / 4, $"managed bytes: {allocated}");
    }

    [Fact]
    public void Envelope_and_public_serializers_preserve_fixed_protobuf_bytes()
    {
        var codecs = Codecs();
        Assert.True(codecs.TryGetSerializer("application/x-protobuf", out var serializer));
        var spanDeserializer = Assert.IsAssignableFrom<IZLinkMessageSpanDeserializer>(serializer);
        var value = new StringValue { Value = "wire" };
        byte[] expected = [0x0a, 0x04, 0x77, 0x69, 0x72, 0x65];

        using var part = ZLinkEnvelopeCodec.EncodeBody(value, typeof(StringValue), codecs);
        var publicPayload = serializer.Serialize(value, typeof(StringValue));

        Assert.Equal(expected, part.ToArray());
        Assert.Equal(expected, publicPayload.Bytes.ToArray());
        Assert.Equal(value, spanDeserializer.Deserialize(part.AsReadOnlySpan(), typeof(StringValue)));
        Assert.Equal(value, serializer.Deserialize(publicPayload, typeof(StringValue)));
        value.Value = "changed";
        Assert.Equal(expected, publicPayload.Bytes.ToArray());
    }

    [Theory]
    [InlineData(0)]
    [InlineData(4096)]
    [InlineData(262144)]
    public void Span_decode_retains_an_owned_value_after_message_disposal(int size)
    {
        var codecs = Codecs();
        var value = new BytesValue { Value = ByteString.CopyFrom(new byte[size]) };
        Assert.True(codecs.TryGetSerializer("application/x-protobuf", out var serializer));
        var spanDeserializer = Assert.IsAssignableFrom<IZLinkMessageSpanDeserializer>(serializer);
        BytesValue decoded;
        using (var part = ZLinkEnvelopeCodec.EncodeBody(value, typeof(BytesValue), codecs, out var contentType))
        {
            Assert.Equal("application/x-protobuf", contentType);
            Assert.Equal(value.ToByteArray(), part.ToArray());
            decoded = Assert.IsType<BytesValue>(spanDeserializer.Deserialize(
                part.AsReadOnlySpan(), typeof(BytesValue)));
        }

        Assert.Equal(value, decoded);
    }

    [Fact]
    public void Multipart_view_decode_allocates_only_the_owned_protobuf_body()
    {
        const int payloadSize = 262144;
        const int iterations = 16;
        var codecs = Codecs();
        var value = new BytesValue { Value = ByteString.CopyFrom(new byte[payloadSize]) };
        using var header = Message.From("header");
        using var body = ZLinkEnvelopeCodec.EncodeBody(value, typeof(BytesValue), codecs);
        using var frame = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage([header, body]);
        for (var index = 0; index < iterations; index++)
        {
            Assert.True(ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(frame, out var warmup));
            _ = ZLinkEnvelopeCodec.DecodeBody(warmup, typeof(BytesValue), "application/x-protobuf", codecs);
        }

        var before = GC.GetAllocatedBytesForCurrentThread();
        BytesValue? decoded = null;
        for (var index = 0; index < iterations; index++)
        {
            if (!ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(frame, out var view))
                throw new InvalidOperationException("The fixed multipart frame must decode.");
            decoded = (BytesValue)ZLinkEnvelopeCodec.DecodeBody(
                view, typeof(BytesValue), "application/x-protobuf", codecs)!;
        }
        var allocated = GC.GetAllocatedBytesForCurrentThread() - before;

        Assert.Equal(value, decoded);
        // ByteString owns one decoded payload array. A native-to-managed frame
        // copy before parsing would add a second payload-sized allocation.
        Assert.True(allocated < (long)payloadSize * iterations * 5 / 4, $"managed bytes: {allocated}");
    }

    [Fact]
    public void Envelope_serializer_preserves_support_for_legacy_IMessage_implementations()
    {
        var codecs = Codecs();
        var value = new LegacyMessage();
        using var part = ZLinkEnvelopeCodec.EncodeBody(value, typeof(LegacyMessage), codecs);

        Assert.Equal(new byte[] { 0x0a, 0x02, 0x6f, 0x6b }, part.ToArray());
    }

    private static ZLinkCodecRegistryBuilder Codecs()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.Use(ZLinkProtobufCodec.Default);
        return codecs;
    }

    private sealed class LegacyMessage : IMessage
    {
        public MessageDescriptor Descriptor => StringValue.Descriptor;
        public int CalculateSize() => 4;
        public void WriteTo(CodedOutputStream output)
        {
            output.WriteRawTag(0x0a);
            output.WriteString("ok");
        }
        public void MergeFrom(CodedInputStream input) => throw new NotSupportedException();
    }
}
