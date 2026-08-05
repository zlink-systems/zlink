using System.Buffers.Binary;
using System.Text;

namespace Systems.Zlink.Stream.Connector.Runtime.Protocol;

internal sealed class ZlinkStreamHeaderCodec
{
    // Keep byte-compatible with Zlink.Framework stream headers; StreamWireInteropTests is the drift gate.
    private const int MaxMetadataPayloadSize = 1024;

    private const ZlinkStreamHeaderFlags KnownFlags =
        ZlinkStreamHeaderFlags.HasRequestSeq |
        ZlinkStreamHeaderFlags.HasMetadata |
        ZlinkStreamHeaderFlags.PayloadCompressed |
        ZlinkStreamHeaderFlags.HasCorrelationId |
        ZlinkStreamHeaderFlags.HasFlowId;

    public ReadOnlyMemory<byte> Encode(ZlinkStreamHeader header)
    {
        ValidateOutboundPacketName(header.Kind, header.Name);
        ValidateEnum(header.Kind, header.Codec, header.Flags);

        var nameBytes = Encoding.UTF8.GetBytes(header.Name);
        var hasRequestSeq = header.RequestSeq is not null;
        var hasMetadata = header.Metadata.Count > 0;
        var hasCorrelationId = !string.IsNullOrEmpty(header.CorrelationId);
        var correlationBytes = hasCorrelationId
            ? Encoding.UTF8.GetBytes(header.CorrelationId!)
            : Array.Empty<byte>();
        if (correlationBytes.Length > byte.MaxValue)
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.ValidationFailed, "Correlation id is too long.");
        var hasFlowId = header.FlowId is not null || header.FlowOrigin is not null;
        if (hasFlowId && (header.FlowId is null || header.FlowOrigin is null))
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ValidationFailed,
                "Flow id and flow origin must be present together.");
        if (header.FlowId is not null && !ZlinkStreamFlowId.IsValid(header.FlowId))
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.ValidationFailed, "Flow id must be UUIDv7.");
        if (header.FlowOrigin is { } origin && !Enum.IsDefined(origin))
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.ValidationFailed, "Flow origin is invalid.");

        var flags = header.Flags;
        ValidateHeaderSemantics(
            header.Kind, header.Codec, flags, hasRequestSeq, hasMetadata, hasCorrelationId, hasFlowId);

        flags = hasRequestSeq
            ? flags | ZlinkStreamHeaderFlags.HasRequestSeq
            : flags & ~ZlinkStreamHeaderFlags.HasRequestSeq;
        flags = hasMetadata ? flags | ZlinkStreamHeaderFlags.HasMetadata : flags & ~ZlinkStreamHeaderFlags.HasMetadata;
        flags = hasCorrelationId
            ? flags | ZlinkStreamHeaderFlags.HasCorrelationId
            : flags & ~ZlinkStreamHeaderFlags.HasCorrelationId;
        flags = hasFlowId ? flags | ZlinkStreamHeaderFlags.HasFlowId : flags & ~ZlinkStreamHeaderFlags.HasFlowId;

        var metadataSize = hasMetadata ? ZlinkStreamMetadataCodec.GetPayloadSize(header.Metadata) : 0;
        if (metadataSize > MaxMetadataPayloadSize)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ValidationFailed,
                $"Metadata payload exceeds fixed limit ({MaxMetadataPayloadSize}).");

        var size = 4 + (hasRequestSeq ? 8 : 0) + 1 + nameBytes.Length
                   + (hasMetadata ? 2 + metadataSize : 0)
                   + (hasCorrelationId ? 1 + correlationBytes.Length : 0)
                   + (hasFlowId ? ZlinkStreamFlowId.EncodedLength + 1 : 0);
        var buffer = new byte[size];
        var offset = 0;
        buffer[offset++] = ZlinkStreamFlowId.FormatMarker;
        buffer[offset++] = (byte)header.Kind;
        buffer[offset++] = (byte)header.Codec;
        buffer[offset++] = (byte)flags;

        if (hasRequestSeq)
        {
            if (header.RequestSeq!.Value.Value == 0)
                throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.ValidationFailed,
                    "Request sequence must not be zero.");

            BinaryPrimitives.WriteUInt64BigEndian(buffer.AsSpan(offset, 8), header.RequestSeq.Value.Value);
            offset += 8;
        }

        buffer[offset++] = (byte)nameBytes.Length;
        nameBytes.CopyTo(buffer.AsSpan(offset));
        offset += nameBytes.Length;

        if (hasMetadata)
        {
            BinaryPrimitives.WriteUInt16BigEndian(buffer.AsSpan(offset, 2), checked((ushort)metadataSize));
            offset += 2;
            ZlinkStreamMetadataCodec.Write(header.Metadata, buffer.AsSpan(offset, metadataSize));
            offset += metadataSize;
        }

        if (hasCorrelationId)
        {
            buffer[offset++] = (byte)correlationBytes.Length;
            correlationBytes.CopyTo(buffer.AsSpan(offset));
            offset += correlationBytes.Length;
        }

        if (hasFlowId)
        {
            Encoding.ASCII.GetBytes(header.FlowId!, buffer.AsSpan(offset, ZlinkStreamFlowId.EncodedLength));
            offset += ZlinkStreamFlowId.EncodedLength;
            buffer[offset++] = (byte)header.FlowOrigin!.Value;
        }

        return buffer;
    }

    public ZlinkStreamHeader Decode(ReadOnlyMemory<byte> header)
    {
        var span = header.Span;
        if (span.Length < 5)
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed, "Helper header is too short.");
        if (span[0] != ZlinkStreamFlowId.FormatMarker)
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed, "Stream format marker is invalid.");

        var kind = (ZlinkStreamMessageKind)span[1];
        var codec = (ZlinkStreamCodec)span[2];
        var flags = (ZlinkStreamHeaderFlags)span[3];
        ValidateEnum(kind, codec, flags);

        var offset = 4;
        ZlinkStreamRequestSeq? requestSeq = null;
        if (flags.HasFlag(ZlinkStreamHeaderFlags.HasRequestSeq))
        {
            if (span.Length - offset < 8)
                throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Helper header request sequence is incomplete.");

            var requestSeqValue = BinaryPrimitives.ReadUInt64BigEndian(span.Slice(offset, 8));
            if (requestSeqValue == 0)
                throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Request sequence must not be zero.");

            requestSeq = new ZlinkStreamRequestSeq(requestSeqValue);
            offset += 8;
        }

        if (span.Length - offset < 1)
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                "Helper header name length is missing.");

        var nameLength = span[offset++];
        if (span.Length - offset < nameLength)
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                "Helper header packet name is invalid.");

        var name = Encoding.UTF8.GetString(span.Slice(offset, nameLength));
        offset += nameLength;

        var metadata = ZlinkStreamMetadata.Empty;
        if (flags.HasFlag(ZlinkStreamHeaderFlags.HasMetadata))
        {
            if (span.Length - offset < 2)
                throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Helper header metadata length is missing.");

            var metadataLength = BinaryPrimitives.ReadUInt16BigEndian(span.Slice(offset, 2));
            offset += 2;
            if (span.Length - offset < metadataLength)
                throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Helper header metadata is incomplete.");

            metadata = ZlinkStreamMetadataCodec.Decode(span.Slice(offset, metadataLength));
            offset += metadataLength;
        }

        string? correlationId = null;
        if (flags.HasFlag(ZlinkStreamHeaderFlags.HasCorrelationId))
        {
            if (span.Length - offset < 1)
                throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Helper header correlation id length is missing.");

            var correlationLength = span[offset++];
            if (correlationLength == 0 || span.Length - offset < correlationLength)
                throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Helper header correlation id is invalid.");

            correlationId = Encoding.UTF8.GetString(span.Slice(offset, correlationLength));
            offset += correlationLength;
        }

        string? flowId = null;
        ZlinkStreamFlowOrigin? flowOrigin = null;
        if (flags.HasFlag(ZlinkStreamHeaderFlags.HasFlowId))
        {
            if (span.Length - offset < ZlinkStreamFlowId.EncodedLength + 1)
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Helper header flow fields are incomplete.");

            flowId = Encoding.ASCII.GetString(span.Slice(offset, ZlinkStreamFlowId.EncodedLength));
            offset += ZlinkStreamFlowId.EncodedLength;
            flowOrigin = (ZlinkStreamFlowOrigin)span[offset++];
            if (!ZlinkStreamFlowId.IsValid(flowId) || !Enum.IsDefined(flowOrigin.Value))
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Helper header flow fields are invalid.");
        }

        if (offset != span.Length)
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                "Helper header contains trailing bytes.");

        if (kind is not (ZlinkStreamMessageKind.Response or ZlinkStreamMessageKind.Error))
            ZlinkStreamConnector.ValidateName(
                name,
                kind == ZlinkStreamMessageKind.Control,
                ZlinkStreamErrorCode.FrameDecodeFailed);
        ValidateHeaderSemantics(
            kind, codec, flags, requestSeq is not null, metadata.Count > 0,
            correlationId is not null, flowId is not null);
        return new ZlinkStreamHeader(
            kind, codec, flags, requestSeq, name, metadata, correlationId, flowId, flowOrigin);
    }

    private static void ValidateOutboundPacketName(ZlinkStreamMessageKind kind, string name)
    {
        var isReply = kind is ZlinkStreamMessageKind.Response or ZlinkStreamMessageKind.Error;
        if (isReply)
        {
            if (name.Length != 0)
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.ValidationFailed,
                    "Response and error packets must not contain a packet name.");
            return;
        }

        ZlinkStreamConnector.ValidateName(name, kind == ZlinkStreamMessageKind.Control);
    }

    private static void ValidateEnum(
        ZlinkStreamMessageKind kind,
        ZlinkStreamCodec codec,
        ZlinkStreamHeaderFlags flags)
    {
        if (!Enum.IsDefined(kind))
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed, "Unknown stream message kind.");

        if (!Enum.IsDefined(codec))
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed, "Unknown stream codec.");

        if ((flags & ~KnownFlags) != 0)
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed, "Unknown stream header flag.");
    }

    private static void ValidateHeaderSemantics(
        ZlinkStreamMessageKind kind,
        ZlinkStreamCodec codec,
        ZlinkStreamHeaderFlags flags,
        bool hasRequestSeq,
        bool hasMetadata,
        bool hasCorrelationId,
        bool hasFlowId)
    {
        if (kind == ZlinkStreamMessageKind.Send && hasRequestSeq)
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                "Send packet must not contain a request sequence.");

        if (kind is ZlinkStreamMessageKind.Request or ZlinkStreamMessageKind.Response && !hasRequestSeq)
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                "Request and response packets must contain a request sequence.");

        if (kind == ZlinkStreamMessageKind.Error && codec != ZlinkStreamCodec.Json)
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                "Error packet must use the JSON codec.");

        if (kind == ZlinkStreamMessageKind.Control)
        {
            if (flags != ZlinkStreamHeaderFlags.None)
                throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Control packet must not contain flags.");

            if (codec != ZlinkStreamCodec.Raw)
                throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Control packet must use the raw codec.");

            if (hasRequestSeq)
                throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Control packet must not contain a request sequence.");

            if (hasMetadata)
                throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Control packet must not contain metadata.");

            if (hasCorrelationId)
                throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Control packet must not contain a correlation id.");

            if (hasFlowId)
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Control packet must not contain flow fields.");
        }
    }
}
