using System.Buffers.Binary;
using System.Text;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Xunit;

public sealed partial class StreamConnectorTests
{
    [Fact]
    public void HeaderProtocolRoundTripsMetadataAndRequestSeq()
    {
        var codec = new ZlinkStreamHeaderCodec();
        var source = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Request,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.HasRequestSeq,
            new ZlinkStreamRequestSeq(42),
            "profile.get",
            ZlinkStreamMetadata.Empty.With("traceId", "abc"));

        var decoded = codec.Decode(codec.Encode(source));

        Assert.Equal(source.Kind, decoded.Kind);
        Assert.Equal(source.Codec, decoded.Codec);
        Assert.Equal(source.RequestSeq, decoded.RequestSeq);
        Assert.Equal(source.Name, decoded.Name);
        Assert.Equal("abc", decoded.Metadata.Get("traceId"));
    }

    [Fact]
    public void HeaderProtocolRoundTripsEmptyMetadataValue()
    {
        var codec = new ZlinkStreamHeaderCodec();
        var source = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            null,
            "profile.update",
            ZlinkStreamMetadata.Empty.With("optional", ""));

        var decoded = codec.Decode(codec.Encode(source));

        Assert.Equal("", decoded.Metadata.Get("optional"));
    }

    // MFLOW-009: correlation id is a first-class header trailer (flag 0x08), wire layout
    // = after metadata, u8 length + UTF-8 bytes. Round-trips and is byte-exact.
    [Fact]
    public void HeaderProtocolRoundTripsCorrelationIdAfterMetadata()
    {
        var codec = new ZlinkStreamHeaderCodec();
        var flowId = ZlinkStreamFlowId.Create();
        var source = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Request,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.HasRequestSeq,
            new ZlinkStreamRequestSeq(7),
            "order.place",
            ZlinkStreamMetadata.Empty.With("k", "v"),
            "a1b2",
            flowId,
            ZlinkStreamFlowOrigin.Application);

        var encoded = codec.Encode(source);
        var decoded = codec.Decode(encoded);

        Assert.Equal("a1b2", decoded.CorrelationId);
        Assert.True(decoded.Flags.HasFlag(ZlinkStreamHeaderFlags.HasCorrelationId));
        Assert.True(decoded.Flags.HasFlag(ZlinkStreamHeaderFlags.HasFlowId));
        Assert.Equal(flowId, decoded.FlowId);
        Assert.Equal(ZlinkStreamFlowOrigin.Application, decoded.FlowOrigin);

        var span = encoded.Span;
        Assert.Equal(ZlinkStreamFlowId.FormatMarker, span[0]);
        Assert.Equal((byte)4, span[^(ZlinkStreamFlowId.EncodedLength + 6)]);
        Assert.Equal(
            "a1b2",
            Encoding.UTF8.GetString(
                span.Slice(span.Length - ZlinkStreamFlowId.EncodedLength - 5, 4)));
    }

    [Fact]
    public void OutboundFrameCreatesFlowOnceAndCodecRemainsDeterministic()
    {
        var codec = new ZlinkStreamHeaderCodec();
        var sender = new ZlinkStreamFrameSender(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                Compression = ZlinkStreamCompression.None
            },
            codec,
            null,
            new SemaphoreSlim(1, 1),
            static () => null);

        var frame = sender.BuildOutboundFrame(
            ZlinkStreamMessageKind.Send,
            "flow.start",
            new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, ReadOnlyMemory<byte>.Empty),
            ZlinkStreamMetadata.Empty,
            false,
            null);
        var header = codec.Decode(frame.HeaderBytes);

        // One-way sends have no reply, so no correlation id is created (flow-correlation spec §2).
        Assert.Null(header.CorrelationId);
        Assert.False(header.Flags.HasFlag(ZlinkStreamHeaderFlags.HasCorrelationId));
        Assert.True(ZlinkStreamFlowId.IsValid(header.FlowId));
        Assert.Equal(ZlinkStreamFlowOrigin.Application, header.FlowOrigin);
        Assert.Equal(codec.Encode(header).ToArray(), codec.Encode(header).ToArray());
    }

    [Fact]
    public void OutboundRequestFrameKeepsCorrelationIdAtEveryDiagnosticsLevel()
    {
        var codec = new ZlinkStreamHeaderCodec();
        foreach (var level in new[]
                 {
                     ZlinkStreamDiagnosticsLevel.Off,
                     ZlinkStreamDiagnosticsLevel.Errors,
                     ZlinkStreamDiagnosticsLevel.Normal,
                     ZlinkStreamDiagnosticsLevel.Detailed
                 })
        {
            var sender = new ZlinkStreamFrameSender(
                new ZlinkStreamConnectorOptions
                {
                    Endpoint = new Uri("tcp://127.0.0.1:1"),
                    Compression = ZlinkStreamCompression.None,
                    DiagnosticsLevel = level
                },
                codec,
                null,
                new SemaphoreSlim(1, 1),
                static () => null);

            var frame = sender.BuildOutboundFrame(
                ZlinkStreamMessageKind.Request,
                "flow.request",
                new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, ReadOnlyMemory<byte>.Empty),
                ZlinkStreamMetadata.Empty,
                false,
                new ZlinkStreamRequestSeq(1));
            var header = codec.Decode(frame.HeaderBytes);

            // Correlation ids are protocol information: kept per request even at Off.
            Assert.False(string.IsNullOrWhiteSpace(header.CorrelationId));
            Assert.True(header.Flags.HasFlag(ZlinkStreamHeaderFlags.HasCorrelationId));
        }
    }

    [Fact]
    public void OutboundFramesAtOffCarryNoFlowFieldsAndNoSendCorrelation()
    {
        var codec = new ZlinkStreamHeaderCodec();
        var sender = new ZlinkStreamFrameSender(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                Compression = ZlinkStreamCompression.None,
                DiagnosticsLevel = ZlinkStreamDiagnosticsLevel.Off
            },
            codec,
            null,
            new SemaphoreSlim(1, 1),
            static () => null);

        using var ambient = ZlinkStreamFlowContext.Enter(
            ZlinkStreamFlowId.Create(),
            ZlinkStreamFlowOrigin.Inbound);
        var send = codec.Decode(sender.BuildOutboundFrame(
            ZlinkStreamMessageKind.Send,
            "flow.off",
            new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, ReadOnlyMemory<byte>.Empty),
            ZlinkStreamMetadata.Empty,
            false,
            null).HeaderBytes);
        var request = codec.Decode(sender.BuildOutboundFrame(
            ZlinkStreamMessageKind.Request,
            "flow.off",
            new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, ReadOnlyMemory<byte>.Empty),
            ZlinkStreamMetadata.Empty,
            false,
            new ZlinkStreamRequestSeq(2)).HeaderBytes);

        Assert.False(send.Flags.HasFlag(ZlinkStreamHeaderFlags.HasFlowId));
        Assert.Null(send.FlowId);
        Assert.Null(send.FlowOrigin);
        Assert.Null(send.CorrelationId);
        Assert.False(request.Flags.HasFlag(ZlinkStreamHeaderFlags.HasFlowId));
        Assert.Null(request.FlowId);
        Assert.Null(request.FlowOrigin);
    }

    [Fact]
    public void HeaderDecodeWithoutFlowCaptureKeepsStructuralChecks()
    {
        var codec = new ZlinkStreamHeaderCodec();
        var encoded = codec.Encode(new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            null,
            "flow.test",
            ZlinkStreamMetadata.Empty,
            FlowId: ZlinkStreamFlowId.Create(),
            FlowOrigin: ZlinkStreamFlowOrigin.Inbound));

        var decoded = codec.Decode(encoded, false);
        Assert.Null(decoded.FlowId);
        Assert.Null(decoded.FlowOrigin);
        Assert.True(decoded.Flags.HasFlag(ZlinkStreamHeaderFlags.HasFlowId));

        // The structural length check is kept: a flow flag with truncated flow bytes fails.
        var truncated = encoded[..^1];
        Assert.Throws<ZlinkStreamException>(() => codec.Decode(truncated, false));
    }

    [Fact]
    public async Task InboundFlowIsReusedAndExpiresAfterCallbackScope()
    {
        var flowId = ZlinkStreamFlowId.Create();
        var releaseDetached = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Task<(string FlowId, ZlinkStreamFlowOrigin Origin)?> detached;

        using (ZlinkStreamFlowContext.Enter(flowId, ZlinkStreamFlowOrigin.Inbound))
        {
            Assert.Equal(
                (flowId, ZlinkStreamFlowOrigin.Inbound),
                ZlinkStreamFlowContext.Current);
            detached = Task.Run(async () =>
            {
                await releaseDetached.Task.ConfigureAwait(false);
                return ZlinkStreamFlowContext.Current;
            });
        }

        releaseDetached.SetResult();
        Assert.Null(await detached);
        Assert.Null(ZlinkStreamFlowContext.Current);
    }

    [Fact]
    public void HeaderProtocolRoundTripsExplicitUuidV7AndRootOrigin()
    {
        var codec = new ZlinkStreamHeaderCodec();
        var flowId = ZlinkStreamFlowId.Create();
        var source = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            null,
            "flow.test",
            ZlinkStreamMetadata.Empty,
            FlowId: flowId,
            FlowOrigin: ZlinkStreamFlowOrigin.Timer);

        var decoded = codec.Decode(codec.Encode(source));

        Assert.Equal(flowId, decoded.FlowId);
        Assert.Equal(ZlinkStreamFlowOrigin.Timer, decoded.FlowOrigin);
    }

    [Fact]
    public void HeaderProtocolRejectsMissingMarkerAndInvalidFlowFields()
    {
        var codec = new ZlinkStreamHeaderCodec();
        Assert.Throws<ZlinkStreamException>(() => codec.Decode(new byte[] { 1, 1, 0, 1, (byte)'x' }));

        var invalid = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            null,
            "flow.test",
            ZlinkStreamMetadata.Empty,
            FlowId: "00000000-0000-4000-8000-000000000000",
            FlowOrigin: ZlinkStreamFlowOrigin.Application);
        Assert.Throws<ZlinkStreamException>(() => codec.Encode(invalid));
    }

    [Fact]
    public void HeaderProtocolRejectsUnknownFlag()
    {
        var codec = new ZlinkStreamHeaderCodec();
        var bytes = new byte[]
        {
            ZlinkStreamFlowId.FormatMarker,
            (byte)ZlinkStreamMessageKind.Send,
            (byte)ZlinkStreamCodec.Json,
            0x80,
            1,
            (byte)'x'
        };

        var exception = Assert.Throws<ZlinkStreamException>(() => codec.Decode(bytes));

        Assert.Equal(ZlinkStreamErrorCode.FrameDecodeFailed, exception.Error.Code);
    }

    [Fact]
    public void HeaderProtocolRejectsInvalidRequestSeqAndErrorCodecCombinations()
    {
        var codec = new ZlinkStreamHeaderCodec();

        var sendWithRequestSeq = new byte[14];
        sendWithRequestSeq[0] = ZlinkStreamFlowId.FormatMarker;
        sendWithRequestSeq[1] = (byte)ZlinkStreamMessageKind.Send;
        sendWithRequestSeq[2] = (byte)ZlinkStreamCodec.Json;
        sendWithRequestSeq[3] = (byte)ZlinkStreamHeaderFlags.HasRequestSeq;
        BinaryPrimitives.WriteUInt64BigEndian(sendWithRequestSeq.AsSpan(4, 8), 1);
        sendWithRequestSeq[12] = 1;
        sendWithRequestSeq[13] = (byte)'x';

        Assert.Throws<ZlinkStreamException>(() => codec.Decode(sendWithRequestSeq));

        var responseWithoutRequestSeq = new byte[]
        {
            ZlinkStreamFlowId.FormatMarker,
            (byte)ZlinkStreamMessageKind.Response,
            (byte)ZlinkStreamCodec.Json,
            0,
            1,
            (byte)'x'
        };

        Assert.Throws<ZlinkStreamException>(() => codec.Decode(responseWithoutRequestSeq));

        var errorWithRawCodec = new byte[]
        {
            ZlinkStreamFlowId.FormatMarker,
            (byte)ZlinkStreamMessageKind.Error,
            (byte)ZlinkStreamCodec.Raw,
            0,
            1,
            (byte)'x'
        };

        Assert.Throws<ZlinkStreamException>(() => codec.Decode(errorWithRawCodec));
    }

    [Fact]
    public void HeaderProtocolEnforcesControlPacketContract()
    {
        var codec = new ZlinkStreamHeaderCodec();

        var control = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Control,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            null,
            "$zlink.heartbeat.ping",
            ZlinkStreamMetadata.Empty);
        var decoded = codec.Decode(codec.Encode(control));
        Assert.Equal(ZlinkStreamMessageKind.Control, decoded.Kind);
        Assert.Equal("$zlink.heartbeat.ping", decoded.Name);

        Assert.Throws<ZlinkStreamException>(() => codec.Encode(new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Control,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            null,
            "$zlink.heartbeat.ping",
            ZlinkStreamMetadata.Empty)));

        Assert.Throws<ZlinkStreamException>(() => codec.Encode(new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Control,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.PayloadCompressed,
            null,
            "$zlink.heartbeat.ping",
            ZlinkStreamMetadata.Empty)));

        Assert.Throws<ZlinkStreamException>(() => codec.Encode(new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Control,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            new ZlinkStreamRequestSeq(1),
            "$zlink.heartbeat.ping",
            ZlinkStreamMetadata.Empty)));

        Assert.Throws<ZlinkStreamException>(() => codec.Encode(new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Control,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            null,
            "$zlink.heartbeat.ping",
            ZlinkStreamMetadata.Empty.With("traceId", "abc"))));

        Assert.Throws<ZlinkStreamException>(() => codec.Encode(new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            null,
            "$zlink.user",
            ZlinkStreamMetadata.Empty)));
    }

    [Fact]
    public void SessionClosingCodecDecodesTheVersionedClosedReason()
    {
        var diagnostic = Encoding.UTF8.GetBytes("rolling drain");
        var payload = new byte[4 + diagnostic.Length];
        payload[0] = 1;
        payload[1] = 4;
        BinaryPrimitives.WriteUInt16BigEndian(payload.AsSpan(2, 2), (ushort)diagnostic.Length);
        diagnostic.CopyTo(payload.AsSpan(4));

        var closing = ZlinkStreamSessionClosingCodec.Decode(payload);

        Assert.Equal(ZlinkStreamCloseReason.ServerDrain, closing.Reason);
        Assert.Equal("rolling drain", closing.Diagnostic);
    }

    [Theory]
    [InlineData("idle", ZlinkStreamCloseReason.IdleTimeout, 2)]
    [InlineData("heartbeat", ZlinkStreamCloseReason.HeartbeatTimeout, 3)]
    [InlineData("drain", ZlinkStreamCloseReason.ServerDrain, 4)]
    [InlineData("protocol", ZlinkStreamCloseReason.ProtocolError, 5)]
    public void SessionClosingCodecEncodesAndDecodesServerReasons(
        string producer,
        ZlinkStreamCloseReason expectedReason,
        byte expectedWireReason)
    {
        var payload = producer switch
        {
            "idle" => ZlinkStreamSessionClosingCodec.EncodeIdleTimeout("done"),
            "heartbeat" => ZlinkStreamSessionClosingCodec.EncodeHeartbeatTimeout("done"),
            "drain" => ZlinkStreamSessionClosingCodec.EncodeServerDrain("done"),
            "protocol" => ZlinkStreamSessionClosingCodec.EncodeProtocolError("done"),
            _ => throw new ArgumentOutOfRangeException(nameof(producer))
        };

        Assert.Equal(new byte[] { 1, expectedWireReason, 0, 4 }, payload[..4]);
        var closing = ZlinkStreamSessionClosingCodec.Decode(payload);
        Assert.Equal(expectedReason, closing.Reason);
        Assert.Equal("done", closing.Diagnostic);
    }

    [Fact]
    public void SessionClosingCodecRejectsOversizedAndInvalidUtf8Diagnostics()
    {
        var oversized = Assert.Throws<ZlinkStreamException>(() =>
            ZlinkStreamSessionClosingCodec.EncodeServerDrain(new string('x', 513)));
        var invalidUtf8 = Assert.Throws<ZlinkStreamException>(() =>
            ZlinkStreamSessionClosingCodec.EncodeProtocolError("\ud800"));

        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, oversized.Error.Code);
        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, invalidUtf8.Error.Code);
    }

    [Theory]
    [InlineData(new byte[] { 2, 4, 0, 0 })]
    [InlineData(new byte[] { 1, 0, 0, 0 })]
    [InlineData(new byte[] { 1, 4, 0, 1 })]
    [InlineData(new byte[] { 1, 4, 0, 1, 0xff })]
    public void SessionClosingCodecRejectsInvalidVersionReasonLengthAndUtf8(byte[] payload)
    {
        var exception = Assert.Throws<ZlinkStreamException>(() =>
            ZlinkStreamSessionClosingCodec.Decode(payload));

        Assert.Equal(ZlinkStreamErrorCode.FrameDecodeFailed, exception.Error.Code);
    }
}
