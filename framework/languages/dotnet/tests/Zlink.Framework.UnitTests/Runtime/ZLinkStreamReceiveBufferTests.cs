using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ZLinkStreamReceiveBufferTests
{
    [Fact]
    public void Buffer_reassembles_a_frame_across_raw_parts()
    {
        var encoded = ZLinkStreamFrameCodec.Encode("header"u8, "payload"u8);
        using var buffer = new ZLinkStreamReceiveBuffer(1024);

        buffer.Append(encoded.AsSpan(0, 3));
        Assert.False(buffer.TryTakeFrame(out _));

        buffer.Append(encoded.AsSpan(3));
        Assert.True(buffer.TryTakeFrame(out var frame));
        var receivedFrame = frame ?? throw new InvalidOperationException();
        using (receivedFrame)
        {
            Assert.Equal("header"u8.ToArray(), receivedFrame.Header!.ToArray());
            Assert.Equal("payload"u8.ToArray(), receivedFrame.Payload!.ToArray());
        }
        Assert.False(buffer.TryTakeFrame(out _));
    }

    [Fact]
    public void Buffer_drains_multiple_frames_from_one_raw_part()
    {
        var first = ZLinkStreamFrameCodec.Encode("h1"u8, "p1"u8);
        var second = ZLinkStreamFrameCodec.Encode("h2"u8, "p2"u8);
        var combined = first.Concat(second).ToArray();
        using var buffer = new ZLinkStreamReceiveBuffer(1024);

        buffer.Append(combined);

        Assert.True(buffer.TryTakeFrame(out var firstFrame));
        var receivedFirstFrame = firstFrame ?? throw new InvalidOperationException();
        using (receivedFirstFrame)
            Assert.Equal("p1"u8.ToArray(), receivedFirstFrame.Payload!.ToArray());
        Assert.True(buffer.TryTakeFrame(out var secondFrame));
        var receivedSecondFrame = secondFrame ?? throw new InvalidOperationException();
        using (receivedSecondFrame)
            Assert.Equal("p2"u8.ToArray(), receivedSecondFrame.Payload!.ToArray());
        Assert.False(buffer.TryTakeFrame(out _));
    }

    [Fact]
    public void Buffer_retains_one_payload_owner_until_every_derived_frame_finishes()
    {
        var first = ZLinkStreamFrameCodec.Encode("h1"u8, "p1"u8);
        var second = ZLinkStreamFrameCodec.Encode("h2"u8, "p2"u8);
        var combined = first.Concat(second).ToArray();
        var payloadOwner = new CountingPayloadOwner();
        using var buffer = new ZLinkStreamReceiveBuffer(1024);

        buffer.Append(combined, payloadOwner);
        Assert.True(buffer.TryTakeFrame(out var firstFrame));
        Assert.True(buffer.TryTakeFrame(out var secondFrame));
        Assert.Equal(0, payloadOwner.DisposeCount);

        firstFrame!.Dispose();
        Assert.Equal(0, payloadOwner.DisposeCount);
        secondFrame!.Dispose();
        Assert.Equal(1, payloadOwner.DisposeCount);
    }

    [Fact]
    public void Buffer_retains_every_contributing_payload_owner_until_the_frame_finishes()
    {
        var encoded = ZLinkStreamFrameCodec.Encode("header"u8, "payload"u8);
        var firstOwner = new CountingPayloadOwner();
        var secondOwner = new CountingPayloadOwner();
        using var buffer = new ZLinkStreamReceiveBuffer(1024);

        buffer.Append(encoded.AsSpan(0, 3), firstOwner);
        buffer.Append(encoded.AsSpan(3), secondOwner);
        Assert.True(buffer.TryTakeFrame(out var frame));
        Assert.Equal(0, firstOwner.DisposeCount);
        Assert.Equal(0, secondOwner.DisposeCount);

        frame!.Dispose();
        Assert.Equal(1, firstOwner.DisposeCount);
        Assert.Equal(1, secondOwner.DisposeCount);
    }

    [Fact]
    public void Buffer_rejects_a_frame_larger_than_the_complete_message_limit()
    {
        var encoded = ZLinkStreamFrameCodec.Encode("header"u8, "body"u8);
        using var buffer = new ZLinkStreamReceiveBuffer(9);
        buffer.Append(encoded);

        var error = Assert.Throws<InvalidDataException>(() => buffer.TryTakeFrame(out _));
        Assert.Contains("EMSGSIZE", error.Message, StringComparison.Ordinal);
    }

    private sealed class CountingPayloadOwner : IDisposable
    {
        private int _disposeCount;

        internal int DisposeCount => Volatile.Read(ref _disposeCount);

        public void Dispose() => Interlocked.Increment(ref _disposeCount);
    }
}
