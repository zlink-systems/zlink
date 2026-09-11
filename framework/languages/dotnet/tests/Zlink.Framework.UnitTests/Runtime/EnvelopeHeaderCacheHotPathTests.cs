using System.Collections;
using System.Reflection;
using System.Text.Json;
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

    [Fact]
    public void CorrelatedHeadersReuseOneBoundedRoutePlan()
    {
        var header = Header("correlation-plan") with
        {
            Kind = ZLinkMessageKind.Request,
            FormatMarker = 242,
            CorrelationId = "warm"
        };
        using var warm = ZLinkEnvelopeCodec.EncodeHeader(header);
        var count = ((IDictionary)Field("SimpleHeaderCache")).Count;
        var decodedCount = ((Array)Field("DecodedHeaderCache")).Length;
        for (var index = 0; index < 128; index++)
        {
            var current = header with
            {
                CorrelationId = $"correlation-{index}",
                Deadline = DateTimeOffset.UnixEpoch.AddTicks(index),
                Metadata = new() { ["record"] = index.ToString() }
            };
            using var encoded = ZLinkEnvelopeCodec.EncodeHeader(current);
            Assert.Equal(ZLinkEnvelopeCodec.EncodeProtocolJsonBytes(current), encoded.ToArray());
            Assert.Equal(current.CorrelationId, ZLinkEnvelopeCodec.DecodeHeader(encoded).CorrelationId);
        }

        Assert.Equal(count, ((IDictionary)Field("SimpleHeaderCache")).Count);
        Assert.Equal(decodedCount, ((Array)Field("DecodedHeaderCache")).Length);
    }

    [Fact]
    public void PlannedHeaderPreservesJsonEscapingIncludingInvalidSurrogates()
    {
        var text = new string(Enumerable.Range(0, 128).Select(static value => (char)value).ToArray())
                   + "한글<&\u2028\u2029😀\ud800x\udc00";
        var header = Header("escaping-plan") with
        {
            Kind = ZLinkMessageKind.Error,
            FormatMarker = 242,
            CorrelationId = text,
            Topic = text,
            Source = text,
            ErrorCode = "failure",
            ErrorMessage = text,
            Metadata = new() { [text] = text, ["nullable"] = null! }
        };

        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);

        Assert.Equal(ZLinkEnvelopeCodec.EncodeProtocolJsonBytes(header), encoded.ToArray());
    }

    [Theory]
    [InlineData(0, 0)]
    [InlineData(1, 14)]
    [InlineData(100, -14)]
    [InlineData(1000000, 9)]
    [InlineData(1234500, -5)]
    [InlineData(9999999, 0)]
    public void PlannedDeadlinePreservesFractionTrimmingAndOffset(long ticks, int offsetHours)
    {
        var header = Header("deadline-plan") with
        {
            FormatMarker = 242,
            CorrelationId = "deadline",
            Deadline = new DateTimeOffset(2026, 9, 10, 12, 34, 56,
                TimeSpan.FromHours(offsetHours)).AddTicks(ticks)
        };
        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);

        Assert.Equal(ZLinkEnvelopeCodec.EncodeProtocolJsonBytes(header), encoded.ToArray());
        Assert.Equal(header.Deadline, ZLinkEnvelopeCodec.DecodeHeader(encoded).Deadline);
    }

    [Fact]
    public void DynamicHeaderEncodingDoesNotAllocateAManagedBufferProportionalToValues()
    {
        var small = Header("dynamic-allocation") with { CorrelationId = "small" };
        var large = small with { CorrelationId = new string('x', 4096) };
        for (var index = 0; index < 256; index++)
        {
            using var first = ZLinkEnvelopeCodec.EncodeHeader(small);
            using var second = ZLinkEnvelopeCodec.EncodeHeader(large);
        }

        const int iterations = 128;
        var start = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < iterations; index++)
        {
            using var encoded = ZLinkEnvelopeCodec.EncodeHeader(small);
        }
        var smallBytes = GC.GetAllocatedBytesForCurrentThread() - start;
        start = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < iterations; index++)
        {
            using var encoded = ZLinkEnvelopeCodec.EncodeHeader(large);
        }
        var largeBytes = GC.GetAllocatedBytesForCurrentThread() - start;

        Assert.True(largeBytes <= smallBytes + iterations * 16L,
            $"Dynamic values added {largeBytes - smallBytes} managed bytes across {iterations} headers.");
    }

    [Theory]
    [InlineData("""{"source":"first","unknown":{"nested":[true,null,12]},"KINd":3,"f\u006FrmatMarker":"242","CHANNELNAME":"cache","messageName":"flexible","contentType":"application/json","correlationId":"flexible-1","SoUrCe":"last"}""")]
    [InlineData("""{"formatMarker":242,"kind":3,"correlationId":"flexible-2","metadata":{"same":"first","same":"last","nullable":null}}""")]
    [InlineData("""{"formatMarker":"24\u0032","kind":3,"correlationId":"flexible-3","deadline":"2026-09-10T12:34:56Z","metadata":{"first":"discarded"},"METADATA":{}}""")]
    [InlineData("""{"formatMarker":242,"kind":3,"correlationId":"flexible-4","channelName":null,"messageName":null,"contentType":null,"metadata":null}""")]
    public void StreamingHeaderDecodePreservesAcceptedWebJsonSemantics(string wire)
    {
        var expected = JsonSerializer.Deserialize<ZLinkEnvelopeHeader>(wire,
            ZLinkJsonSerializerOptions.Default)!;
        using var encoded = Message.From(wire);
        var actual = ZLinkEnvelopeCodec.DecodeHeader(encoded);

        Assert.Equal(expected with { Metadata = null }, actual with { Metadata = null });
        Assert.Equal(expected.Metadata, actual.Metadata);
    }

    [Theory]
    [InlineData("null")]
    [InlineData("[]")]
    [InlineData("{\"formatMarker\":242")]
    [InlineData("""{"formatMarker":242,"kind":"3"}""")]
    [InlineData("""{"formatMarker":"256","kind":3}""")]
    [InlineData("""{"formatMarker":242,"kind":3,"deadline":123}""")]
    [InlineData("""{"formatMarker":242,"kind":3,"metadata":{"key":123}}""")]
    [InlineData("""{"formatMarker":242,"kind":3,"source":{}}""")]
    [InlineData("""{"formatMarker":242,"kind":3}{}""")]
    public void StreamingHeaderDecodePreservesProtocolErrorBoundary(string wire)
    {
        using var encoded = Message.From(wire);
        Assert.Throws<ZLinkEnvelopeProtocolException>(() => ZLinkEnvelopeCodec.DecodeHeader(encoded));
    }

    [Fact]
    public void StreamingHeaderDecodeRejectsInvalidUtf8AsProtocolError()
    {
        byte[] wire = [.. "{\"formatMarker\":242,\"kind\":3,\"source\":\""u8.ToArray(),
            0xFF, .. "\"}"u8.ToArray()];
        Assert.Throws<JsonException>(() => JsonSerializer.Deserialize<ZLinkEnvelopeHeader>(wire,
            ZLinkJsonSerializerOptions.Default));
        using var encoded = Message.From(wire);

        Assert.Throws<ZLinkEnvelopeProtocolException>(() => ZLinkEnvelopeCodec.DecodeHeader(encoded));
    }

    private static ZLinkEnvelopeHeader Header(string messageName) =>
        new(ZLinkMessageKind.Command, "cache", messageName, "application/json",
            null, null, null, null, null);

    private static object Field(string name) =>
        typeof(ZLinkEnvelopeCodec).GetField(name, BindingFlags.Static | BindingFlags.NonPublic)!
            .GetValue(null)!;
}
