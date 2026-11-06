using System.Buffers.Binary;
using Google.Protobuf;
using Google.Protobuf.Reflection;
using Google.Protobuf.WellKnownTypes;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ProtobufNativePayloadTests
{
    [Fact]
    public void Reply_header_rejects_invalid_utf8_in_unknown_property_names()
    {
        var codecs = Codecs();
        var bytes = System.Text.Encoding.UTF8.GetBytes(
            "{\"formatMarker\":242,\"kind\":2,\"correlationId\":\"r\",\"contentType\":\"application/x-protobuf\",\"x\":0}"
        );
        bytes[Array.LastIndexOf(bytes, (byte)'x')] = 0xff;
        using var header = Message.From(bytes);
        using var body = Message.From(new byte[] { 10, 1, 1 });
        IReadOnlyList<Message> parts = new[] { header, body };
        var failure = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkEnvelopeReplyDecoder.Decode<BytesValue>(parts, "empty", "error", codecs)
        );
        Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, failure.Kind);
        using var packed = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(
            parts
        );
        Assert.True(
            ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(
                packed,
                out var view
            )
        );
        var borrowed = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkEnvelopeReplyDecoder.Decode<BytesValue>(
                parts,
                "empty",
                "error",
                codecs,
                applicationPayloadView: view
            )
        );
        Assert.Equal(failure.Kind, borrowed.Kind);
        Assert.Equal(failure.Message, borrowed.Message);
    }

    [Theory]
    [InlineData(1024)]
    [InlineData(4096)]
    public void Reply_decode_is_equivalent_for_identical_parts_and_borrowed_view(int size)
    {
        var codecs = Codecs();
        var value = new BytesValue { Value = ByteString.CopyFrom(new byte[size]) };
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Response,
            "bench",
            "BenchPayload",
            "application/x-protobuf",
            "reply-1",
            null,
            null,
            null,
            null
        );
        var parts = ZLinkEnvelopeCodec.EncodeParts(header, value, typeof(BytesValue), codecs);
        try
        {
            using var packed = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(
                parts
            );
            Assert.True(
                ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(
                    packed,
                    out var view
                )
            );
            for (var i = 0; i < parts.Count; i++)
                Assert.True(parts[i].AsReadOnlySpan().SequenceEqual(view.GetSpan(i)));
            var direct = ZLinkEnvelopeReplyDecoder.Decode<BytesValue>(
                parts,
                "empty",
                "error",
                codecs
            );
            var borrowed = ZLinkEnvelopeReplyDecoder.Decode<BytesValue>(
                parts,
                "empty",
                "error",
                codecs,
                applicationPayloadView: view
            );
            Assert.Equal(value, direct);
            Assert.Equal(direct, borrowed);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    [Theory]
    [InlineData("{", new byte[] { 10, 1, 1 })]
    [InlineData(
        "{\"formatMarker\":241,\"kind\":2,\"correlationId\":\"r\"}",
        new byte[] { 10, 1, 1 }
    )]
    [InlineData(
        "{\"formatMarker\":242,\"kind\":2,\"kind\":2,\"correlationId\":\"r\"}",
        new byte[] { 10, 1, 1 }
    )]
    [InlineData("{\"formatMarker\":242,\"kind\":2}", new byte[] { 10, 1, 1 })]
    [InlineData("{\"formatMarker\":242,\"kind\":3}", new byte[] { 10, 1, 1 })]
    [InlineData(
        "{\"formatMarker\":242,\"kind\":2,\"correlationId\":\"r\",\"contentType\":\"application/x-protobuf\"}",
        new byte[] { 10, 5, 1 }
    )]
    public void Malformed_reply_rejection_is_equivalent_for_parts_and_borrowed_view(
        string json,
        byte[] bytes
    )
    {
        var codecs = Codecs();
        using var header = Message.From(System.Text.Encoding.UTF8.GetBytes(json));
        using var body = Message.From(bytes);
        IReadOnlyList<Message> parts = new[] { header, body };
        using var packed = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(
            parts
        );
        Assert.True(
            ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(
                packed,
                out var view
            )
        );
        var direct = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkEnvelopeReplyDecoder.Decode<BytesValue>(parts, "empty", "error", codecs)
        );
        var borrowed = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkEnvelopeReplyDecoder.Decode<BytesValue>(
                parts,
                "empty",
                "error",
                codecs,
                applicationPayloadView: view
            )
        );
        Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, direct.Kind);
        Assert.Equal(direct.Kind, borrowed.Kind);
        Assert.Equal(direct.Message, borrowed.Message);
        Assert.Equal(direct.InnerException?.GetType(), borrowed.InnerException?.GetType());
    }

#if DEBUG
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Node_request_writes_one_native_payload_and_one_wire_list(bool metadata)
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new Zlink.Framework.Runtime.Service.ZLinkManagedMeshNode(
            context,
            "native-wire-count"
        );
        using var payload = Message.From("payload");
        using var cancelled = new CancellationTokenSource();
        cancelled.Cancel();
        var counts = new Dictionary<string, int>();
        var disposed = 0;
        ZLinkMessageParts.DisposingObserver.Value = _ => Interlocked.Increment(ref disposed);
        ZLinkApplicationPayloadEnvelopeCodec.EncodingObserver.Value = name =>
            counts[name] = counts.GetValueOrDefault(name) + 1;
        try
        {
            await Record.ExceptionAsync(async () =>
                await node.RequestToNodeDirectAsync(
                    RoutingId.From("missing"),
                    [payload],
                    SendFlags.None,
                    metadata ? new byte[] { 42 } : default,
                    TimeSpan.FromSeconds(5),
                    cancelled.Token
                )
            );
            Assert.Equal(0, counts.GetValueOrDefault("packed-array"));
            Assert.Equal(1, counts.GetValueOrDefault("packed-write"));
            Assert.Equal(1, counts.GetValueOrDefault("wire-list"));
            Assert.Equal(metadata ? 3 : 2, disposed);
        }
        finally
        {
            ZLinkApplicationPayloadEnvelopeCodec.EncodingObserver.Value = null;
            ZLinkMessageParts.DisposingObserver.Value = null;
        }
    }
#endif

    public static IEnumerable<object[]> RequestWireCases() =>
        from metadata in new[] { false, true }
        from count in new[] { 1, 2, 3 }
        from size in new[] { 0, 4096, 262144 }
        select new object[] { metadata, count, size };

    [Theory]
    [MemberData(nameof(RequestWireCases))]
    public void Native_request_matches_packed_wire(bool metadata, int count, int size)
    {
        var parts = Enumerable
            .Range(0, count)
            .Select(i => Message.From(new byte[size + i]))
            .ToArray();
        var meta = metadata ? new byte[] { 0, 42, 255 } : Array.Empty<byte>();
        try
        {
            var expected = new List<byte[]>
            {
                ZLinkServiceWireCodec.EncodeApplication(
                    ServiceWireConstants.Command.NodeRequest,
                    ulong.MaxValue,
                    null,
                    metadata
                ),
            };
            if (metadata)
                expected.Add(meta);
            expected.Add(ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(parts));
            var actual = ZLinkManagedMeshNode.CreateApplicationWire(
                ServiceWireConstants.Command.NodeRequest,
                ulong.MaxValue,
                null,
                parts,
                meta
            );
            try
            {
                Assert.Equal(expected.Count, actual.Count);
                for (var i = 0; i < actual.Count; i++)
                    Assert.Equal(expected[i], actual[i].ToArray());
                Assert.True(
                    ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(
                        actual[^1],
                        out var view
                    )
                );
                Assert.Equal(count, view.Count);
                for (var i = 0; i < count; i++)
                    Assert.True(parts[i].AsReadOnlySpan().SequenceEqual(view.GetSpan(i)));
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(actual);
            }
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(3)]
    public void Native_multipart_rejects_invalid_count_and_lengths(int field)
    {
        using var part = Message.From("body");
        using var packed = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage([
            part,
        ]);
        var bytes = packed.ToArray();
        var offset = 5;
        offset += 1 + bytes[offset];
        offset += 1 + bytes[offset];
        var payloadStart = offset + 4;
        var at = field switch
        {
            0 => 1,
            1 => offset,
            2 => payloadStart,
            _ => payloadStart + 4,
        };
        BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(at), uint.MaxValue);
        using var invalid = Message.From(bytes);
        Assert.False(
            ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(invalid, out _)
        );
        Assert.False(
            ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(bytes, out _)
        );
        Assert.False(
            ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(invalid, out _)
        );
    }

    [Theory]
    [InlineData((byte)ServiceWireConstants.Command.NodeSend, 0UL, null)]
    [InlineData((byte)ServiceWireConstants.Command.NodeRequest, ulong.MaxValue, null)]
    [InlineData((byte)ServiceWireConstants.Command.ChannelSend, 0UL, "채널")]
    [InlineData((byte)ServiceWireConstants.Command.ChannelRequest, 1UL, "channel")]
    public void Native_application_header_shares_the_byte_writer(
        byte commandValue,
        ulong correlation,
        string? channel
    )
    {
        var command = (ServiceWireConstants.Command)commandValue;
        foreach (var metadata in new[] { false, true })
        {
            using var header = ZLinkServiceWireCodec.EncodeApplicationMessage(
                command,
                correlation,
                channel,
                metadata
            );
            Assert.Equal(
                ZLinkServiceWireCodec.EncodeApplication(command, correlation, channel, metadata),
                header.ToArray()
            );
        }
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeApplicationMessage(
                command,
                correlation == 0 ? 1UL : 0UL,
                channel,
                false
            )
        );
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeApplication(
                command,
                correlation == 0 ? 1UL : 0UL,
                channel,
                false
            )
        );
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Durable_wire_is_disposed_once_before_submission(bool waitForReadiness)
    {
        using var cancellation = new CancellationTokenSource();
        var wire = new[] { Message.From("owned-wire") };
#if DEBUG
        var disposed = 0;
#endif
        var attempts = 0;
#if DEBUG
        ZLinkMessageParts.DisposingObserver.Value = _ => Interlocked.Increment(ref disposed);
#endif
        try
        {
            if (!waitForReadiness)
                cancellation.Cancel();
            var clock = new ControllableTimeProvider();
            var pending = ZLinkDurableRequest
                .RequestAsync<string>(
                    wire,
                    clock.GetTimestamp(),
                    TimeSpan.FromSeconds(5),
                    (_, _, _) =>
                    {
                        attempts++;
                        throw new InvalidOperationException();
                    },
                    cancellation.Token,
                    () => new(1, 0, new TaskCompletionSource().Task),
                    clock
                )
                .AsTask();
            if (waitForReadiness)
            {
                Assert.False(pending.IsCompleted);
#if DEBUG
                Assert.Equal(0, disposed);
#endif
                cancellation.Cancel();
            }
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => pending);
            Assert.Equal(0, attempts);
#if DEBUG
            Assert.Equal(1, disposed);
#endif
        }
        finally
        {
#if DEBUG
            ZLinkMessageParts.DisposingObserver.Value = null;
#endif
        }
    }

    [Fact]
    public async Task Retired_request_disposes_a_late_native_reply_once()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "late-native-reply");
        using var cancellation = new CancellationTokenSource();
        var physical = new TaskCompletionSource<IReadOnlyList<Message>>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Task observation = Task.CompletedTask;
#if DEBUG
        var disposed = 0;
        ZLinkMessageParts.DisposingObserver.Value = _ => Interlocked.Increment(ref disposed);
#endif
        try
        {
            var caller = node.RunDeadlineRequestAsync<IReadOnlyList<Message>>(
                TimeSpan.FromSeconds(5),
                cancellation.Token,
                completion => completion.Parts,
                (operation, _) =>
                {
                    observation = node.ObserveNativeDeadlineReplyAsync(
                        operation,
                        physical.Task,
                        completion => completion.Parts
                    );
                    started.SetResult();
                    return observation;
                }
            );
            await started.Task;
            cancellation.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => caller);
#if DEBUG
            Assert.Equal(0, disposed);
#endif
            physical.SetResult([Message.From("late")]);
            await observation;
#if DEBUG
            Assert.Equal(1, disposed);
#endif
        }
        finally
        {
#if DEBUG
            ZLinkMessageParts.DisposingObserver.Value = null;
#endif
        }
    }

    private const int AllocationPayloadSize = 1024;
    private const int AllocationIterations = 1024;
    private const int AllocationWarmupIterations = 256;

    [Fact]
    public void Buffered_message_encoding_allocates_only_the_final_native_owner()
    {
        var codecs = Codecs();
        var value = new BytesValue { Value = ByteString.CopyFrom(new byte[AllocationPayloadSize]) };
        var size = value.CalculateSize();
        var directBytes = SteadyBytes(() =>
        {
            using var direct = Message.Allocate(size);
            value.WriteTo(direct.AsSpan());
        });
        var encodedBytes = SteadyBytes(() =>
        {
            using var encoded = ZLinkEnvelopeCodec.EncodeBody(value, typeof(BytesValue), codecs);
        });
        Assert.Equal(directBytes, encodedBytes);
    }

    [Fact]
    public void Public_serialization_transfers_the_protobuf_buffer_without_a_second_copy()
    {
        var codecs = Codecs();
        Assert.True(codecs.TryGetSerializer("application/x-protobuf", out var serializer));
        var value = new BytesValue { Value = ByteString.CopyFrom(new byte[AllocationPayloadSize]) };
        var directBytes = SteadyBytes(() => ZLinkEncodedPayload.FromOwned(value.ToByteArray()));
        var encodedBytes = SteadyBytes(() => serializer.Serialize(value, typeof(BytesValue)));
        Assert.True(
            encodedBytes <= directBytes,
            $"Public serialization allocated {encodedBytes} bytes; owned buffer control allocated {directBytes} bytes."
        );
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
        Assert.Equal(
            value,
            spanDeserializer.Deserialize(part.AsReadOnlySpan(), typeof(StringValue))
        );
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
        using (
            var part = ZLinkEnvelopeCodec.EncodeBody(
                value,
                typeof(BytesValue),
                codecs,
                out var contentType
            )
        )
        {
            Assert.Equal("application/x-protobuf", contentType);
            Assert.Equal(value.ToByteArray(), part.ToArray());
            decoded = Assert.IsType<BytesValue>(
                spanDeserializer.Deserialize(part.AsReadOnlySpan(), typeof(BytesValue))
            );
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
        using var frame = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage([
            header,
            body,
        ]);
        for (var index = 0; index < iterations; index++)
        {
            Assert.True(
                ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(
                    frame,
                    out var warmup
                )
            );
            _ = ZLinkEnvelopeCodec.DecodeBody(
                warmup,
                typeof(BytesValue),
                "application/x-protobuf",
                codecs
            );
        }

        var before = GC.GetAllocatedBytesForCurrentThread();
        BytesValue? decoded = null;
        for (var index = 0; index < iterations; index++)
        {
            if (
                !ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(
                    frame,
                    out var view
                )
            )
                throw new InvalidOperationException("The fixed multipart frame must decode.");
            decoded = (BytesValue)
                ZLinkEnvelopeCodec.DecodeBody(
                    view,
                    typeof(BytesValue),
                    "application/x-protobuf",
                    codecs
                )!;
        }
        var allocated = GC.GetAllocatedBytesForCurrentThread() - before;

        Assert.Equal(value, decoded);
        // ByteString owns one decoded payload array. A native-to-managed frame
        // copy before parsing would add a second payload-sized allocation.
        Assert.True(
            allocated < (long)payloadSize * iterations * 5 / 4,
            $"managed bytes: {allocated}"
        );
    }

    [Fact]
    public void Envelope_serializer_preserves_support_for_legacy_IMessage_implementations()
    {
        var codecs = Codecs();
        var value = new LegacyMessage();
        using var part = ZLinkEnvelopeCodec.EncodeBody(value, typeof(LegacyMessage), codecs);

        Assert.Equal(new byte[] { 0x0a, 0x02, 0x6f, 0x6b }, part.ToArray());
    }

    // Smallest window of AllocationIterations calls after the same call path has been warmed.
    private static long SteadyBytes(Action call) =>
        PerCallAllocation.MinWindow(call, AllocationIterations, warmup: AllocationWarmupIterations);

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
