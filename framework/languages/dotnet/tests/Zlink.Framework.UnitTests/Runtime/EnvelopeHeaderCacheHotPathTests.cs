using System.Text.Json;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.UnitTests;

public sealed class EnvelopeHeaderCacheHotPathTests(Xunit.Abstractions.ITestOutputHelper output)
{
    [Fact]
    public async Task ParallelMissesPublishEveryDistinctPlanOnce()
    {
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                GetType(),
                nameof(ParallelMissesPublishEveryDistinctPlanOnce)
            )
        )
            return;

        const int participants = 8;
        const int rounds = 128;
        foreach (var sharedKey in new[] { true, false })
        {
            var prefix = $"cache-parallel-{sharedKey}-{Guid.NewGuid():N}";
            var headers = Enumerable
                .Range(0, sharedKey ? rounds : participants * rounds)
                .Select(index => Header($"{prefix}-{index}") with { FormatMarker = 242 })
                .ToArray();
            var expected = headers.Select(ZLinkEnvelopeCodec.EncodeProtocolJsonBytes).ToArray();
            var initialCount = ZLinkEnvelopeCodec.SnapshotCacheCounts().SimpleHeaders;
            using var start = new Barrier(participants);
            var workers = Enumerable
                .Range(0, participants)
                .Select(worker =>
                    Task.Factory.StartNew(
                        () =>
                        {
                            var actual = new byte[rounds][];
                            for (var round = 0; round < rounds; round++)
                            {
                                var index = sharedKey ? round : worker * rounds + round;
                                start.SignalAndWait();
                                using var encoded = ZLinkEnvelopeCodec.EncodeHeader(headers[index]);
                                actual[round] = encoded.ToArray();
                            }
                            return actual;
                        },
                        CancellationToken.None,
                        TaskCreationOptions.LongRunning,
                        TaskScheduler.Default
                    )
                )
                .ToArray();
            var results = await Task.WhenAll(workers);
            for (var worker = 0; worker < participants; worker++)
            for (var round = 0; round < rounds; round++)
                Assert.Equal(
                    expected[sharedKey ? round : worker * rounds + round],
                    results[worker][round]
                );
            Assert.Equal(
                initialCount + headers.Length,
                ZLinkEnvelopeCodec.SnapshotCacheCounts().SimpleHeaders
            );
            output.WriteLine(
                $"sharedKey={sharedKey}: {participants * rounds} encodes, {headers.Length} published plans"
            );
        }
    }

    [Fact]
    public async Task ReplacementRemainsFifoAndWarmHitsDoNotRefreshOrder()
    {
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                GetType(),
                nameof(ReplacementRemainsFifoAndWarmHitsDoNotRefreshOrder)
            )
        )
            return;

        var headers = Enumerable
            .Range(0, 4097)
            .Select(index => Header($"cache-fifo-{index}") with { FormatMarker = 242 })
            .ToArray();
        byte[] bytes;
        using (var warm = ZLinkEnvelopeCodec.EncodeHeader(headers[0]))
            bytes = warm.ToArray();
        for (var index = 0; index < 256; index++)
            _ = EncodingAllocation(headers[0]);
        var nativeBytes =
            PerCallAllocation.MinWindow(
                () =>
                {
                    using var message = Message.From(bytes);
                },
                1024
            ) / 1024;
        for (var index = 1; index < 4096; index++)
        {
            using var encoded = ZLinkEnvelopeCodec.EncodeHeader(headers[index]);
        }

        Assert.Equal(4096, ZLinkEnvelopeCodec.SnapshotCacheCounts().SimpleHeaders);
        Assert.True(EncodingAllocation(headers[0]) <= nativeBytes + 16);
        using var replacement = ZLinkEnvelopeCodec.EncodeHeader(headers[4096]);
        Assert.Equal(
            ZLinkEnvelopeCodec.EncodeProtocolJsonBytes(headers[4096]),
            replacement.ToArray()
        );
        Assert.True(EncodingAllocation(headers[4095]) <= nativeBytes + 16);
        var evictedBytes = EncodingAllocation(headers[0]);
        Assert.True(
            evictedBytes > nativeBytes + 16,
            $"The oldest plan was not evicted: {evictedBytes} B."
        );
        Assert.Equal(4096, ZLinkEnvelopeCodec.SnapshotCacheCounts().SimpleHeaders);
        output.WriteLine(
            $"FIFO: bound=4096, warm hit <= {nativeBytes + 16} B, evicted miss={evictedBytes} B"
        );

        static long EncodingAllocation(ZLinkEnvelopeHeader header)
        {
            var before = GC.GetAllocatedBytesForCurrentThread();
            using (var encoded = ZLinkEnvelopeCodec.EncodeHeader(header)) { }
            return GC.GetAllocatedBytesForCurrentThread() - before;
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void WarmEncodingAllocatesOnlyTheOwnedMessage(bool request)
    {
        var header = Header("cache-allocation");
        if (request)
            header = header with { Kind = ZLinkMessageKind.Request, CorrelationId = "request-1" };
        byte[] bytes;
        using (var encoded = ZLinkEnvelopeCodec.EncodeHeader(header))
            bytes = encoded.ToArray();
        for (var index = 0; index < 256; index++)
        {
            using var direct = Message.From(bytes);
            using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
        }
        const int iterations = 1024;
        var directBytes = PerCallAllocation.MinWindow(
            () =>
            {
                using var direct = Message.From(bytes);
            },
            iterations
        );
        var encodedBytes = PerCallAllocation.MinWindow(
            () =>
            {
                using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
            },
            iterations
        );
        output.WriteLine(
            $"request={request}: header={encodedBytes / iterations} B/message, native={directBytes / iterations} B/message"
        );
        Assert.True(
            encodedBytes <= directBytes + iterations * 16L,
            $"Warm encoding allocated {encodedBytes} B per {iterations} calls; owned Message allocated {directBytes} B."
        );
    }

    [Fact]
    public void ReplacementPreservesTheExistingBoundsAndWireBytes()
    {
        using var first = ZLinkEnvelopeCodec.EncodeHeader(Header("cache-first"));
        for (var index = 0; index < 4097; index++)
        {
            using var encoded = ZLinkEnvelopeCodec.EncodeHeader(Header($"cache-replace-{index}"));
            Assert.Equal(
                $"cache-replace-{index}",
                ZLinkEnvelopeCodec.DecodeHeader(encoded).MessageName
            );
        }

        Assert.True(ZLinkEnvelopeCodec.SnapshotCacheCounts().SimpleHeaders <= 4096);
        Assert.True(ZLinkEnvelopeCodec.SnapshotCacheCounts().DecodedHeaders <= 64);
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
            CorrelationId = "warm",
        };
        using var warm = ZLinkEnvelopeCodec.EncodeHeader(header);
        var count = ZLinkEnvelopeCodec.SnapshotCacheCounts().SimpleHeaders;
        var decodedCount = ZLinkEnvelopeCodec.SnapshotCacheCounts().DecodedHeaders;
        for (var index = 0; index < 128; index++)
        {
            var current = header with
            {
                CorrelationId = $"correlation-{index}",
                Deadline = DateTimeOffset.UnixEpoch.AddTicks(index),
                Metadata = new Dictionary<string, string>() { ["record"] = index.ToString() },
            };
            using var encoded = ZLinkEnvelopeCodec.EncodeHeader(current);
            Assert.Equal(ZLinkEnvelopeCodec.EncodeProtocolJsonBytes(current), encoded.ToArray());
            Assert.Equal(
                current.CorrelationId,
                ZLinkEnvelopeCodec.DecodeHeader(encoded).CorrelationId
            );
        }

        Assert.Equal(count, ZLinkEnvelopeCodec.SnapshotCacheCounts().SimpleHeaders);
        Assert.Equal(decodedCount, ZLinkEnvelopeCodec.SnapshotCacheCounts().DecodedHeaders);
    }

    [Fact]
    public void PlannedHeaderPreservesNonMetadataJsonEscapingIncludingInvalidSurrogates()
    {
        var text =
            new string(Enumerable.Range(0, 128).Select(static value => (char)value).ToArray())
            + "한글<&\u2028\u2029😀\ud800x\udc00";
        var header = Header("escaping-plan") with
        {
            Kind = ZLinkMessageKind.Error,
            FormatMarker = 242,
            CorrelationId = new string(
                Enumerable.Range(0, 64).Select(static value => (char)value).ToArray()
            ),
            Topic = text,
            Source = text,
            ErrorCode = "failure",
            ErrorMessage = text,
        };

        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);

        Assert.Equal(ZLinkEnvelopeCodec.EncodeProtocolJsonBytes(header), encoded.ToArray());
        Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
            ZLinkEnvelopeCodec.EncodeHeader(header with { CorrelationId = text })
        );
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
            Deadline = new DateTimeOffset(
                2026,
                9,
                10,
                12,
                34,
                56,
                TimeSpan.FromHours(offsetHours)
            ).AddTicks(ticks),
        };
        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);

        Assert.Equal(ZLinkEnvelopeCodec.EncodeProtocolJsonBytes(header), encoded.ToArray());
        Assert.Equal(header.Deadline, ZLinkEnvelopeCodec.DecodeHeader(encoded).Deadline);
    }

    [Fact]
    public void HeaderEnumValidationMatchesTheDefinedValues()
    {
        var kinds = Enum.GetValues<ZLinkMessageKind>();
        var kindValues = kinds
            .Select(static kind => (int)kind)
            .Concat(Enumerable.Range(-1, 258))
            .Append(int.MinValue)
            .Append(int.MaxValue)
            .Distinct();
        foreach (var value in kindValues)
        {
            var kind = (ZLinkMessageKind)value;
            var header = Header("kind-validation") with
            {
                FormatMarker = 242,
                Kind = kind,
                CorrelationId = "validation",
                ErrorCode = kind == ZLinkMessageKind.Error ? "error" : null,
            };
            AssertValidation(header, kinds.Contains(kind));
            if (
                kind
                is not (
                    ZLinkMessageKind.Request
                    or ZLinkMessageKind.Response
                    or ZLinkMessageKind.Error
                )
            )
                AssertValidation(header with { CorrelationId = null }, kinds.Contains(kind));
        }

        var origins = Enum.GetValues<ZLinkFlowOrigin>();
        for (var value = 0; value <= byte.MaxValue; value++)
        {
            var origin = (ZLinkFlowOrigin)value;
            var header = Header("origin-validation") with
            {
                FormatMarker = 242,
                FlowId = "0194f5a0-1234-7000-8000-000000000001",
                FlowOrigin = origin,
            };
            var valid = origins.Contains(origin);
            AssertValidation(header, valid);
            Assert.Equal(valid ? header.FlowId : null, ZLinkEnvelopeCodec.ValidFlow(header).FlowId);
        }

        static void AssertValidation(ZLinkEnvelopeHeader header, bool valid)
        {
            using var wire = Message.From(ZLinkEnvelopeCodec.EncodeProtocolJsonBytes(header));
            if (valid)
            {
                using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
                Assert.Equal(header.Kind, ZLinkEnvelopeCodec.DecodeHeader(wire).Kind);
                Assert.Equal(
                    header.FlowOrigin,
                    ZLinkEnvelopeCodec.DecodeHeader(encoded).FlowOrigin
                );
            }
            else
            {
                Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
                    ZLinkEnvelopeCodec.EncodeHeader(header)
                );
                Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
                    ZLinkEnvelopeCodec.DecodeHeader(wire)
                );
            }
        }
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
        var smallBytes = PerCallAllocation.MinWindow(
            () =>
            {
                using var encoded = ZLinkEnvelopeCodec.EncodeHeader(small);
            },
            iterations
        );
        var largeBytes = PerCallAllocation.MinWindow(
            () =>
            {
                using var encoded = ZLinkEnvelopeCodec.EncodeHeader(large);
            },
            iterations
        );

        Assert.True(
            largeBytes <= smallBytes + iterations * 16L,
            $"Dynamic values added {largeBytes - smallBytes} managed bytes across {iterations} headers."
        );
    }

    [Theory]
    [InlineData(
        """{"source":"first","unknown":{"nested":[true,null,12]},"kind":3,"f\u006FrmatMarker":"242","channelName":"cache","messageName":"flexible","contentType":"application/json","correlationId":"flexible-1","source":"last"}"""
    )]
    [InlineData(
        """{"formatMarker":242,"kind":3,"correlationId":"flexible-2","metadata":{"same":"first","same":"last"}}"""
    )]
    [InlineData(
        """{"formatMarker":"24\u0032","kind":3,"correlationId":"flexible-3","deadline":"2026-09-10T12:34:56Z","metadata":{"first":"discarded"},"metadata":{}}"""
    )]
    public void StreamingHeaderDecodeRejectsDuplicateWebJsonFields(string wire)
    {
        using var encoded = Message.From(wire);
        Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
            ZLinkEnvelopeCodec.DecodeHeader(encoded)
        );
    }

    [Theory]
    [InlineData(
        """{"formatMarker":242,"kind":3,"correlationId":"flexible-4","channelName":null,"messageName":null,"contentType":null}"""
    )]
    public void StreamingHeaderDecodePreservesAcceptedWebJsonSemantics(string wire)
    {
        var expected = JsonSerializer.Deserialize<ZLinkEnvelopeHeader>(
            wire,
            ZLinkJsonSerializerOptions.Default
        )!;
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
        Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
            ZLinkEnvelopeCodec.DecodeHeader(encoded)
        );
    }

    [Fact]
    public void StreamingHeaderDecodeRejectsInvalidUtf8AsProtocolError()
    {
        byte[] wire =
        [
            .. "{\"formatMarker\":242,\"kind\":3,\"source\":\""u8.ToArray(),
            0xFF,
            .. "\"}"u8.ToArray(),
        ];
        Assert.Throws<JsonException>(() =>
            JsonSerializer.Deserialize<ZLinkEnvelopeHeader>(
                wire,
                ZLinkJsonSerializerOptions.Default
            )
        );
        using var encoded = Message.From(wire);

        Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
            ZLinkEnvelopeCodec.DecodeHeader(encoded)
        );
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void RequestAndReplyHeaderBytesAndValuesArePinned(bool response)
    {
        var header = new ZLinkEnvelopeHeader(
            response ? ZLinkMessageKind.Response : ZLinkMessageKind.Request,
            "bench",
            response ? string.Empty : "BenchPayload",
            "application/x-protobuf",
            "abc123",
            response ? null : new DateTimeOffset(2026, 11, 13, 12, 34, 56, TimeSpan.Zero),
            null,
            null,
            null
        );
        var expected = response
            ? """{"formatMarker":242,"kind":2,"channelName":"bench","messageName":"","contentType":"application/x-protobuf","correlationId":"abc123","deadline":null,"topic":null,"errorCode":null,"errorMessage":null,"source":null,"flowId":null,"flowOrigin":null}"""
            : """{"formatMarker":242,"kind":1,"channelName":"bench","messageName":"BenchPayload","contentType":"application/x-protobuf","correlationId":"abc123","deadline":"2026-11-13T12:34:56+00:00","topic":null,"errorCode":null,"errorMessage":null,"source":null,"flowId":null,"flowOrigin":null}""";
        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);

        Assert.Equal(System.Text.Encoding.UTF8.GetBytes(expected), encoded.ToArray());
        Assert.Equal(header with { FormatMarker = 242 }, ZLinkEnvelopeCodec.DecodeHeader(encoded));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void EscapedKnownHeaderNamesAllocateOnlyTheDecodedValues(bool response)
    {
        var header = Header("escaped-allocation") with
        {
            Kind = response ? ZLinkMessageKind.Response : ZLinkMessageKind.Request,
            CorrelationId = "escaped-allocation-1",
        };
        using var plain = ZLinkEnvelopeCodec.EncodeHeader(header);
        var escapedJson = System.Text.RegularExpressions.Regex.Replace(
            plain.GetString(),
            "\"([a-zA-Z]+)\":",
            match =>
                "\"" + string.Concat(match.Groups[1].Value.Select(c => $"\\u{(int)c:x4}")) + "\":"
        );
        using var escaped = Message.From(escapedJson);
        Assert.Equal(
            ZLinkEnvelopeCodec.DecodeHeader(plain),
            ZLinkEnvelopeCodec.DecodeHeader(escaped)
        );
        const int calls = 1024;
        var plainBytes = PerCallAllocation.MinWindow(
            () => ZLinkEnvelopeCodec.DecodeHeader(plain),
            calls
        );
        var escapedBytes = PerCallAllocation.MinWindow(
            () => ZLinkEnvelopeCodec.DecodeHeader(escaped),
            calls
        );
        output.WriteLine(
            $"response={response}: plain={plainBytes / calls}, escaped={escapedBytes / calls} B/header"
        );
        Assert.True(
            escapedBytes <= plainBytes + calls * 16L,
            $"Escaped field names added {(escapedBytes - plainBytes) / calls} B/header."
        );
    }

    [Theory]
    [InlineData("\\uD800")]
    [InlineData("\\uDC00")]
    public void UnknownHeaderNamesStillRequireValidUnicode(string name)
    {
        using var encoded = Message.From("{\"formatMarker\":242,\"kind\":3,\"" + name + "\":null}");
        Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
            ZLinkEnvelopeCodec.DecodeHeader(encoded)
        );
    }

    [Theory]
    [InlineData(1)]
    [InlineData(14)]
    [InlineData(78)]
    [InlineData(79)]
    [InlineData(1024)]
    public void UnknownHeaderNamesPreserveUnicodeValidationAcrossStackBufferBounds(int length)
    {
        var prefix = "{\"formatMarker\":242,\"kind\":3,\"" + new string('x', length);
        using var valid = Message.From(prefix + "키\":null}");
        Assert.Equal(ZLinkMessageKind.Command, ZLinkEnvelopeCodec.DecodeHeader(valid).Kind);
        using var invalidEscape = Message.From(prefix + "\\uD800\":null}");
        Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
            ZLinkEnvelopeCodec.DecodeHeader(invalidEscape)
        );
        byte[] invalidBytes =
        [
            .. System.Text.Encoding.UTF8.GetBytes(prefix),
            0xff,
            .. "\":null}"u8.ToArray(),
        ];
        using var invalidUtf8 = Message.From(invalidBytes);
        Assert.Throws<ZLinkEnvelopeProtocolException>(() =>
            ZLinkEnvelopeCodec.DecodeHeader(invalidUtf8)
        );
    }

    private static ZLinkEnvelopeHeader Header(string messageName) =>
        new(
            ZLinkMessageKind.Command,
            "cache",
            messageName,
            "application/json",
            null,
            null,
            null,
            null,
            null
        );
}
