using System.Collections;
using System.Reflection;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.UnitTests;

public sealed class EnvelopeHeaderCacheHotPathTests
{
    [Fact]
    public void WarmEncodingAllocatesOnlyTheOwnedMessage()
    {
        var header = Header("cache-allocation");
        byte[] bytes;
        using (var encoded = ZLinkEnvelopeCodec.EncodeHeader(header))
            bytes = encoded.ToArray();
        for (var index = 0; index < 256; index++)
        {
            using var direct = Message.From(bytes);
            using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
        }
        const int iterations = 1024;
        var start = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < iterations; index++)
        {
            using var direct = Message.From(bytes);
        }
        var directBytes = GC.GetAllocatedBytesForCurrentThread() - start;
        start = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < iterations; index++)
        {
            using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
        }
        var encodedBytes = GC.GetAllocatedBytesForCurrentThread() - start;
        Assert.True(encodedBytes <= directBytes + iterations * 16L,
            $"Warm encoding allocated {encodedBytes} bytes; owned Message allocated {directBytes} bytes.");
    }

    [Fact]
    public async Task WarmHeaderEncodingAndBothDecodersDoNotWaitForCacheMutation()
    {
        var header = Header("cache-hot");
        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
        var decoded = ZLinkEnvelopeCodec.DecodeHeader(encoded);
        using var body = Message.From(new byte[] { 48 });
        using var multipart = ZLinkApplicationPayloadEnvelopeCodec
            .EncodeFrameworkMultipartMessage([encoded, body]);
        Assert.True(ZLinkApplicationPayloadEnvelopeCodec
            .TryDecodeFrameworkMultipartView(multipart, out var view));
        var lane = (ZLinkStateLane)Field("CacheLane");
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Assert.True(lane.TryPost(async () =>
        {
            entered.SetResult();
            await release.Task;
        }));
        await entered.Task.WaitAsync(TimeSpan.FromSeconds(3));
        Task? lookup = null;
        try
        {
            lookup = Task.Run(() =>
            {
                using var again = ZLinkEnvelopeCodec.EncodeHeader(header);
                Assert.Equal(encoded.ToArray(), again.ToArray());
                Assert.Same(decoded, ZLinkEnvelopeCodec.DecodeHeader(encoded));
                Assert.Same(decoded, ZLinkEnvelopeCodec.DecodeHeader(view));
            });
            await lookup.WaitAsync(TimeSpan.FromSeconds(3));
        }
        finally
        {
            release.TrySetResult();
            if (lookup is not null)
                await lookup.WaitAsync(TimeSpan.FromSeconds(3));
        }
    }

    [Fact]
    public void ReplacementPreservesTheExistingBoundsAndWireBytes()
    {
        using var first = ZLinkEnvelopeCodec.EncodeHeader(Header("cache-first"));
        for (var index = 0; index < 4097; index++)
        {
            using var encoded = ZLinkEnvelopeCodec.EncodeHeader(Header($"cache-replace-{index}"));
            Assert.Equal($"cache-replace-{index}", ZLinkEnvelopeCodec.DecodeHeader(encoded).MessageName);
        }

        Assert.True(((IDictionary)Field("SimpleHeaderCache")).Count <= 4096);
        Assert.True(((Array)Field("DecodedHeaderCache")).Length <= 64);
        using var replacement = ZLinkEnvelopeCodec.EncodeHeader(Header("cache-first"));
        Assert.Equal(first.ToArray(), replacement.ToArray());
        Assert.Equal("cache-first", ZLinkEnvelopeCodec.DecodeHeader(replacement).MessageName);
    }

    private static ZLinkEnvelopeHeader Header(string messageName) =>
        new(ZLinkMessageKind.Command, "cache", messageName, "application/json",
            null, null, null, null, null);

    private static object Field(string name) =>
        typeof(ZLinkEnvelopeCodec).GetField(name, BindingFlags.Static | BindingFlags.NonPublic)!
            .GetValue(null)!;
}
