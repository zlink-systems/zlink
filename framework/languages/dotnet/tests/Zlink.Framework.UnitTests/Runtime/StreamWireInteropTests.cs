using System.Text;
using Systems.Zlink.Stream.Connector.Contracts;
using ConnectorFrameCodec = Systems.Zlink.Stream.Connector.Runtime.Protocol.Framing.ZlinkStreamFrameCodec;
using ConnectorHeaderCodec = Systems.Zlink.Stream.Connector.Runtime.Protocol.ZlinkStreamHeaderCodec;
using ConnectorClosingCodec = Systems.Zlink.Stream.Connector.Runtime.Protocol.ZlinkStreamSessionClosingCodec;
using CoreFrameCodec = Zlink.Framework.Runtime.Streams.ZLinkStreamFrameCodec;
using CoreHeaderCodec = Zlink.Framework.Runtime.Streams.ZLinkStreamHeaderCodec;

namespace Zlink.Framework.UnitTests;

public sealed class StreamWireInteropTests
{
    [Fact]
    public void Framework_error_wire_code_preserves_the_public_error_kind()
    {
        var error = ZLinkStreamWireError.FromException(
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "session binding is changing."));

        Assert.Equal("unavailable", error.Code);
        Assert.Equal("session binding is changing.", error.Message);
    }

    [Fact]
    public void Reply_header_echoes_request_correlation_and_keeps_the_root_flow_only_from_context()
    {
        // Spec 27 §7: a reply preserves the request's correlation id, and its
        // flow fields only when tracing captured a request flow context. The
        // reply header itself carries no flow; the encode step fills it from
        // the ambient context (which an Off host never installs).
        const string requestFlowId = "0196f7c2-4cb4-7cc8-89d4-2d6aee6fca2d";
        var request = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Request,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.HasRequestSeq,
            new ZlinkStreamRequestSeq(7),
            "order.place",
            ZlinkStreamMetadata.Empty,
            "corr-request",
            requestFlowId,
            ZlinkStreamFlowOrigin.Application);

        var reply = ZLinkStreamReplyHeaders.CreateForRequest(
            request,
            ZlinkStreamMessageKind.Response,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            request.RequestSeq!.Value,
            ZlinkStreamMetadata.Empty);

        Assert.Equal(request.CorrelationId, reply.CorrelationId);
        Assert.Null(reply.FlowId);
        Assert.Null(reply.FlowOrigin);
        Assert.Equal(string.Empty, reply.Name);

        ZlinkStreamHeader encodedWithContext;
        using (ZLinkFlowContext.EnterExisting(
                   requestFlowId,
                   Zlink.Framework.Runtime.Diagnostics.ZLinkFlowOrigin.Application))
            encodedWithContext = CoreHeaderCodec.Decode(CoreHeaderCodec.Encode(reply));
        Assert.Equal(requestFlowId, encodedWithContext.FlowId);
        Assert.Equal(ZlinkStreamFlowOrigin.Application, encodedWithContext.FlowOrigin);

        var encodedWithoutContext = CoreHeaderCodec.Decode(CoreHeaderCodec.Encode(reply));
        Assert.Null(encodedWithoutContext.FlowId);
        Assert.Null(encodedWithoutContext.FlowOrigin);
    }

    [Fact]
    public void CoreAndConnectorHeaderCodecs_EncodeSameBytes()
    {
        var header = CreateHeader();

        var core = CoreHeaderCodec.Encode(header);
        var connector = new ConnectorHeaderCodec().Encode(header);

        Assert.Equal(core.ToArray(), connector.ToArray());
    }

    [Fact]
    public void CoreHeaderCodec_RoundTripsEmptyMetadataValue()
    {
        var header = CreateHeader();

        var decoded = CoreHeaderCodec.Decode(CoreHeaderCodec.Encode(header));

        Assert.Equal(string.Empty, decoded.Metadata.Values["optional"]);
    }

    [Fact]
    public void ConnectorHeaderEncoding_DecodesWithCoreHeaderCodec()
    {
        var encoded = new ConnectorHeaderCodec().Encode(CreateHeader());

        var decoded = CoreHeaderCodec.Decode(encoded);

        Assert.Equal(string.Empty, decoded.Metadata.Values["optional"]);
    }

    [Fact]
    public void ConnectorFrameEncoding_DecodesWithCoreFrameCodec()
    {
        var header = new ConnectorHeaderCodec().Encode(CreateHeader());
        var payload = Encoding.UTF8.GetBytes("payload");
        var frame = new byte[ConnectorFrameCodec.GetFrameSize(header.Length, payload.Length)];

        ConnectorFrameCodec.WriteFrame(frame, header, payload);

        Assert.True(CoreFrameCodec.TryDecode(frame, out var decodedHeader, out var decodedPayload));
        Assert.Equal(header.ToArray(), decodedHeader.ToArray());
        Assert.Equal(payload, decodedPayload.ToArray());
    }

    [Fact]
    public void CoreAndConnectorFrameCodecs_EncodeSameBytes()
    {
        var header = new ConnectorHeaderCodec().Encode(CreateHeader());
        var payload = Encoding.UTF8.GetBytes("payload");
        var connector = new byte[ConnectorFrameCodec.GetFrameSize(header.Length, payload.Length)];

        ConnectorFrameCodec.WriteFrame(connector, header, payload);
        var core = CoreFrameCodec.Encode(header.Span, payload);

        Assert.Equal(core, connector);
    }

    [Fact]
    public void SessionClosingServerDrainPayload_DecodesInConnector()
    {
        var header = ZLinkStreamProtocolDefaults.EncodeHeader(
            ConnectorClosingCodec.CreateHeader());
        var decodedHeader = new ConnectorHeaderCodec().Decode(header);
        var closing = ConnectorClosingCodec.Decode(
            ConnectorClosingCodec.EncodeServerDrain("rolling drain"));

        Assert.Equal(ConnectorClosingCodec.ControlName, decodedHeader.Name);
        Assert.Equal(ZlinkStreamMessageKind.Control, decodedHeader.Kind);
        Assert.Equal(ZlinkStreamCloseReason.ServerDrain, closing.Reason);
        Assert.Equal("rolling drain", closing.Diagnostic);
    }

    [Theory]
    [InlineData("idle", ZlinkStreamCloseReason.IdleTimeout)]
    [InlineData("heartbeat", ZlinkStreamCloseReason.HeartbeatTimeout)]
    [InlineData("drain", ZlinkStreamCloseReason.ServerDrain)]
    [InlineData("protocol", ZlinkStreamCloseReason.ProtocolError)]
    public void SessionClosingServerReasons_DecodeInConnector(
        string producer,
        ZlinkStreamCloseReason expected)
    {
        var payload = producer switch
        {
            "idle" => ConnectorClosingCodec.EncodeIdleTimeout(),
            "heartbeat" => ConnectorClosingCodec.EncodeHeartbeatTimeout(),
            "drain" => ConnectorClosingCodec.EncodeServerDrain(),
            "protocol" => ConnectorClosingCodec.EncodeProtocolError(),
            _ => throw new ArgumentOutOfRangeException(nameof(producer))
        };

        Assert.Equal(expected, ConnectorClosingCodec.Decode(payload).Reason);
    }

    private static ZlinkStreamHeader CreateHeader()
    {
        return new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Request,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.HasRequestSeq | ZlinkStreamHeaderFlags.PayloadCompressed,
            new ZlinkStreamRequestSeq(42),
            "profile.get",
            ZlinkStreamMetadata.Empty
                .With("traceId", "abc")
                .With("optional", ""),
            "corr-1",
            "0196f7c2-4cb4-7cc8-89d4-2d6aee6fca2d",
            ZlinkStreamFlowOrigin.Application);
    }

    [Fact]
    public void Off_decode_skips_stream_flow_materialization_and_validation()
    {
        var encoded = ZLinkStreamProtocolDefaults.EncodeHeader(CreateHeader()).ToArray();
        encoded[^2] = (byte)'x';

        Assert.ThrowsAny<Exception>(() => ZLinkStreamProtocolDefaults.DecodeHeader(encoded));

        var decoded = ZLinkStreamProtocolDefaults.DecodeHeader(encoded, captureFlow: false);
        Assert.Null(decoded.FlowId);
        Assert.Null(decoded.FlowOrigin);
        Assert.Equal("corr-1", decoded.CorrelationId);
    }
}
