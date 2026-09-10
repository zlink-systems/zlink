using Google.Protobuf;
using Google.Protobuf.Reflection;
using Google.Protobuf.WellKnownTypes;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Runtime.Codecs;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ProtobufNativePayloadTests
{
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
