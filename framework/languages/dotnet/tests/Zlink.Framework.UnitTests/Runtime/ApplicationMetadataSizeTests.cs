using System.Text;
using Google.Protobuf;
using Zlink.Framework.Runtime.Protocol.ChannelHeaderV1;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ApplicationMetadataSizeTests
{
    [Theory]
    [InlineData(1018, false, 1027)]
    [InlineData(1009, true, 1024)]
    [InlineData(1010, true, 1025)]
    public void Field_13_size_controls_outbound_and_inbound_metadata(
        int valueLength,
        bool includeEmptyValue,
        int expectedSize
    )
    {
        var values = new Dictionary<string, string> { ["k"] = new('v', valueLength) };
        if (includeEmptyValue)
            values.Add("é", string.Empty);

        Assert.True(
            values.Sum(pair =>
                Encoding.UTF8.GetByteCount(pair.Key) + Encoding.UTF8.GetByteCount(pair.Value)
            ) <= 1024
        );

        var proto = new ChannelHeader
        {
            Kind = ChannelKind.Command,
            ChannelName = "route",
            MessageName = "event",
            ContentType = ZLinkEnvelopeCodec.DefaultContentType,
        };
        var sizeWithoutMetadata = proto.CalculateSize();
        foreach (var (key, value) in values)
            proto.Metadata.Add(new MetadataEntry { Key = key, Value = value });
        Assert.Equal(expectedSize, proto.CalculateSize() - sizeWithoutMetadata);

        var call = new ZLinkCallMetadata();
        foreach (var (key, value) in values)
            call.Set(key, value);
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Command,
            proto.ChannelName,
            proto.MessageName,
            proto.ContentType,
            null,
            null,
            null,
            null,
            null,
            null
        )
        {
            Metadata = call.Snapshot().Values,
        };
        using var receivedHeader = Message.From(proto.ToByteArray());
        var receivedFrame = CreatePackedFrame(values);
        Assert.InRange(receivedFrame.Length, 1, 1024);

        if (expectedSize == 1024)
        {
            Assert.Multiple(
                () => Assert.Equal(receivedFrame, call.Encode().ToArray()),
                () =>
                {
                    using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
                    Assert.Equal(proto.ToByteArray(), encoded.AsReadOnlySpan().ToArray());
                },
                () =>
                    Assert.Equal(values, ZLinkEnvelopeCodec.DecodeHeader(receivedHeader).Metadata),
                () =>
                {
                    Assert.True(ZLinkMeshMetadataCodec.TryDecode(receivedFrame, out var decoded));
                    Assert.Equal(values, decoded.Values);
                }
            );
        }
        else
        {
            Assert.Multiple(
                () => Assert.Throws<ArgumentException>(() => call.Encode()),
                () =>
                    Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
                        ZLinkEnvelopeCodec.EncodeHeader(header)
                    ),
                () =>
                    Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
                        ZLinkEnvelopeCodec.DecodeHeader(receivedHeader)
                    ),
                () => Assert.False(ZLinkMeshMetadataCodec.TryDecode(receivedFrame, out _))
            );
        }
    }

    private static byte[] CreatePackedFrame(Dictionary<string, string> values)
    {
        using var frame = new MemoryStream();
        frame.WriteByte(1);
        frame.WriteByte((byte)values.Count);
        foreach (var (key, value) in values)
        {
            var keyBytes = Encoding.UTF8.GetBytes(key);
            var valueBytes = Encoding.UTF8.GetBytes(value);
            frame.WriteByte((byte)keyBytes.Length);
            frame.Write(keyBytes);
            frame.WriteByte((byte)(valueBytes.Length >> 8));
            frame.WriteByte((byte)valueBytes.Length);
            frame.Write(valueBytes);
        }
        return frame.ToArray();
    }
}
