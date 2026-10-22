using System.Text;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Xunit.Abstractions;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Codecs;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class EnvelopeCodecAllocationRegressionTests(ITestOutputHelper output)
{
    private const int Iterations = 128;

    [Fact]
    public void TypedActorReplyCopiesOnlyTheHeaderOverOneThousandDecodes()
    {
        const int decodeCount = 1000;
        const int allocationLimit = 1024 * 1024;
        // JSON whitespace enlarges the native frame without enlarging the decoded value.
        using var reply = ActorReply(
            Encoding.UTF8.GetBytes("{\"text\":\"reply\"}" + new string(' ', 65536))
        );
        Message[] parts = [reply];
        for (var i = 0; i < Iterations; i++)
            _ = ZLinkActorReplyDecoder.Decode<Probe>(parts);

        Probe? decoded = null;
        var start = GC.GetAllocatedBytesForCurrentThread();
        for (var i = 0; i < decodeCount; i++)
            decoded = ZLinkActorReplyDecoder.Decode<Probe>(parts);
        var bytes = GC.GetAllocatedBytesForCurrentThread() - start;

        Assert.Equal("reply", decoded!.Text);
        output.WriteLine($"typed Actor reply: {bytes} B/{decodeCount} decodes");
        // Budget covers decoded headers and values; a full frame copy costs over 65 MB.
        Assert.True(bytes <= allocationLimit, $"Actor reply decode allocated {bytes} bytes.");
    }

    [Fact]
    public void MetadataEncodePreservesUtf8WireBytes()
    {
        var metadata = new ZLinkMessageMetadata(
            new Dictionary<string, string> { ["키"] = "한글", ["empty"] = "" }
        );
        byte[] expected =
        [
            1,
            2,
            3,
            0xED,
            0x82,
            0xA4,
            0,
            6,
            0xED,
            0x95,
            0x9C,
            0xEA,
            0xB8,
            0x80,
            5,
            (byte)'e',
            (byte)'m',
            (byte)'p',
            (byte)'t',
            (byte)'y',
            0,
            0,
        ];
        var encoded = ZLinkMeshMetadataCodec.Encode(metadata);
        Assert.Equal(expected, encoded.ToArray());
        Assert.True(ZLinkMeshMetadataCodec.TryDecode(encoded.Span, out var decoded));
        Assert.Equal("한글", decoded.Find("키"));
        Assert.Equal("", decoded.Find("empty"));
    }

    [Fact]
    public void TerminalActorReplyDoesNotCopyThePayload()
    {
        using var small = ActorReply(1024);
        using var large = ActorReply(65536);
        Message[] smallParts = [small];
        Message[] largeParts = [large];
        var smallBytes = SteadyAllocated(() => ZLinkActorReplyDecoder.DecodeTerminal(smallParts));
        var largeBytes = SteadyAllocated(() => ZLinkActorReplyDecoder.DecodeTerminal(largeParts));
        output.WriteLine($"terminal small={smallBytes}, large={largeBytes} B/call");
        Assert.True(
            largeBytes <= smallBytes,
            $"Payload growth added {largeBytes - smallBytes} managed B/call."
        );
    }

    [Fact]
    public void MetadataDecodeDoesNotAllocateUtf8EncodersPerEntry()
    {
        var values = new ZLinkMessageMetadata(
            new Dictionary<string, string> { ["tenant"] = "한글", ["trace"] = "abc" }
        );
        var wire = ZLinkMeshMetadataCodec.Encode(values);
        ZLinkMessageMetadata? decoded = null;
        var bytes = Allocated(() =>
        {
            if (!ZLinkMeshMetadataCodec.TryDecode(wire.Span, out decoded))
                throw new InvalidOperationException("Invalid metadata fixture.");
        });
        Assert.Equal("한글", decoded!.Find("tenant"));
        Assert.Equal("abc", decoded.Find("trace"));
        output.WriteLine($"metadata decode={bytes} B/call");
        // Two strings per entry and the immutable dictionary snapshot fit in
        // this budget; per-entry UTF8Encoding/fallback objects do not.
        Assert.True(bytes <= 700, $"Metadata decode allocated {bytes} B/call.");
    }

    [Fact]
    public void DefaultWrappedEncodeAllocatesNoRegistry()
    {
        var value = new Probe("wrapped");
        var wrapped = ZLinkMessage.From(value);
        var directBytes = Allocated(() =>
        {
            using var encoded = ZLinkEnvelopeCodec.EncodeBody(value, typeof(Probe), null);
        });
        var wrappedBytes = Allocated(() =>
        {
            using var encoded = ZLinkEnvelopeCodec.EncodeBody(wrapped, typeof(ZLinkMessage), null);
        });
        output.WriteLine($"encode direct={directBytes}, wrapped={wrappedBytes} B/call");
        Assert.Equal(directBytes, wrappedBytes);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void DefaultWrappedDecodeAllocatesNoRegistry(bool borrowed)
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        using var body = Message.From("{\"text\":\"wrapped\"}");
        using var header = ZLinkEnvelopeCodec.EncodeHeader(
            new ZLinkEnvelopeHeader(
                ZLinkMessageKind.Command,
                "codec",
                "probe",
                "application/json",
                null,
                null,
                null,
                null,
                null
            )
        );
        using var frame = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage([
            header,
            body,
        ]);
        Assert.True(
            ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(
                frame,
                out var view
            )
        );
        ZLinkMessage? result = null;
        void Decode(ZLinkCodecRegistryBuilder? registry)
        {
            result = borrowed
                ? (ZLinkMessage)
                    ZLinkEnvelopeCodec.DecodeBody(
                        view,
                        typeof(ZLinkMessage),
                        "application/json",
                        registry
                    )!
                : (ZLinkMessage)
                    ZLinkEnvelopeCodec.DecodeBody(
                        body,
                        typeof(ZLinkMessage),
                        "application/json",
                        registry
                    )!;
        }
        var explicitBytes = Allocated(() => Decode(codecs));
        var defaultBytes = Allocated(() => Decode(null));
        Assert.Equal("wrapped", result!.Decode<Probe>().Text);
        output.WriteLine(
            $"borrowed={borrowed}: explicit={explicitBytes}, default={defaultBytes} B/call"
        );
        Assert.Equal(explicitBytes, defaultBytes);
    }

    [Fact]
    public void SnapshotTransfersTheSerializerOwnedBuffer()
    {
        var serializer = new OwnedSerializer();
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer("application/x-owned", serializer);
        var source = new MutableProbe { Text = "before" };
        var snapshot = ZLinkMessage.From(source).Snapshot(codecs);
        var owned = serializer.LastPayload.Bytes;
        source.Text = "after";
        // ReadOnlyMemory equality checks the backing owner, offset and length.
        Assert.True(
            owned.Equals(snapshot.Decode<ReadOnlyMemory<byte>>()),
            "Snapshot copied the serializer-owned buffer."
        );
        Assert.Equal("before", snapshot.Decode<MutableProbe>().Text);
        using var raw = snapshot.ToRawMessage(codecs);
        Assert.Equal("before", raw.GetString());
    }

    // Per-call bytes over Iterations warmed calls that no GC pause interrupted.
    // A GC suspension can retire the thread allocation context inside a call and
    // add its unused tail to the thread counter (see ProtobufNativePayloadTests).
    private long Allocated(Action action)
    {
        for (var i = 0; i < Iterations; i++)
            action();
        long allocated = 0;
        for (var measured = 0; measured < Iterations; )
        {
            var pause = GC.GetTotalPauseDuration();
            var before = GC.GetAllocatedBytesForCurrentThread();
            action();
            var bytes = GC.GetAllocatedBytesForCurrentThread() - before;
            if (GC.GetTotalPauseDuration() != pause)
                continue;
            allocated += bytes;
            measured++;
        }
        return allocated / Iterations;
    }

    // Steady per-call bytes: the value most warmed, pause-free calls allocate. A
    // payload copy adds to every call, while a one-time runtime allocation inside
    // the window (tier-up, a lazy runtime table) adds to a single call only, so a
    // size comparison must not average that one call into the result.
    private static long SteadyAllocated(Action action)
    {
        for (var i = 0; i < Iterations; i++)
            action();
        var counts = new Dictionary<long, int>();
        for (var measured = 0; measured < Iterations; )
        {
            var pause = GC.GetTotalPauseDuration();
            var before = GC.GetAllocatedBytesForCurrentThread();
            action();
            var bytes = GC.GetAllocatedBytesForCurrentThread() - before;
            if (GC.GetTotalPauseDuration() != pause)
                continue;
            counts[bytes] = counts.GetValueOrDefault(bytes) + 1;
            measured++;
        }
        // On a tie, report the larger count so a real per-call cost is never hidden.
        return counts.MaxBy(static entry => (entry.Value, entry.Key)).Key;
    }

    private static Message ActorReply(int size) => ActorReply(new byte[size]);

    private static Message ActorReply(byte[] payload)
    {
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Response,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.HasRequestSeq,
            new ZlinkStreamRequestSeq(1),
            string.Empty,
            ZlinkStreamMetadata.Empty
        );
        return Message.From(
            ZLinkStreamFrameCodec.Encode(
                ZLinkStreamProtocolDefaults.EncodeHeader(header).Span,
                payload
            )
        );
    }

    private sealed record Probe(string Text);

    private sealed class MutableProbe
    {
        public string Text { get; set; } = "";
    }

    private sealed class OwnedSerializer : IZLinkMessageSerializer
    {
        public ZLinkEncodedPayload LastPayload { get; private set; }

        public ZLinkEncodedPayload Serialize(object value, Type type)
        {
            LastPayload = ZLinkEncodedPayload.From(
                Encoding.UTF8.GetBytes(((MutableProbe)value).Text)
            );
            return LastPayload;
        }

        public object Deserialize(ZLinkEncodedPayload payload, Type type) =>
            new MutableProbe { Text = Encoding.UTF8.GetString(payload.Bytes.Span) };
    }
}
