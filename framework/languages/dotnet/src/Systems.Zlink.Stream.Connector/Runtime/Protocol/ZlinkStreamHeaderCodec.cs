using System.Buffers.Binary;
using System.Text;

namespace Systems.Zlink.Stream.Connector.Runtime.Protocol;

internal sealed class ZlinkStreamHeaderCodec
{
    // Keep byte-compatible with Zlink.Framework stream headers; StreamWireInteropTests is the drift gate.
    private const byte InboundWire = 1;
    private const byte TimerWire = 2;
    private const byte ApplicationWire = 3;
    private const byte LifecycleWire = 4;

    private const int FixedPrefixSize = 4 * sizeof(byte);
    private const int KindOffset = sizeof(byte);
    private const int CodecOffset = KindOffset + sizeof(byte);
    private const int FlagsOffset = CodecOffset + sizeof(byte);
    private const int MaxMetadataPayloadSize = 1024;

    private const ZlinkStreamHeaderFlags KnownFlags =
        ZlinkStreamHeaderFlags.HasRequestSeq
        | ZlinkStreamHeaderFlags.HasMetadata
        | ZlinkStreamHeaderFlags.PayloadCompressed
        | ZlinkStreamHeaderFlags.HasCorrelationId
        | ZlinkStreamHeaderFlags.HasFlowId
        | ZlinkStreamHeaderFlags.HasActorSlot;

    public ReadOnlyMemory<byte> Encode(ZlinkStreamHeader header) =>
        Encode(header, header.CorrelationId.AsSpan());

    internal ReadOnlyMemory<byte> Encode(ZlinkStreamHeader header, ReadOnlySpan<char> correlationId)
    {
        ValidatePacketName(header.Kind, header.Name, ZlinkStreamErrorCode.ValidationFailed);
        ValidateEnum(header.Kind, header.Codec, header.Flags);

        var nameLength = Encoding.UTF8.GetByteCount(header.Name);
        var hasRequestSeq = header.RequestSeq is not null;
        var hasMetadata = header.Metadata.Count > 0;
        var hasCorrelationId = !correlationId.IsEmpty;
        var correlationLength = hasCorrelationId ? Encoding.UTF8.GetByteCount(correlationId) : 0;
        if (correlationLength > byte.MaxValue)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ValidationFailed,
                "Correlation id is too long."
            );
        var hasFlowId = header.FlowId is not null || header.FlowOrigin is not null;
        if (hasFlowId && (header.FlowId is null || header.FlowOrigin is null))
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ValidationFailed,
                "Flow id and flow origin must be present together."
            );
        if (header.FlowId is not null && !ZlinkStreamFlowId.IsValid(header.FlowId))
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ValidationFailed,
                "Flow id must be UUIDv7."
            );
        if (header.FlowOrigin is { } origin && FlowOriginToWire(origin) is null)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ValidationFailed,
                "Flow origin is invalid."
            );
        var hasActorSlot = header.ActorSlot is not null;
        if (header.ActorSlot == 0)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ValidationFailed,
                "Actor slot must not be zero."
            );

        var flags = header.Flags;
        ValidateHeaderSemantics(
            header.Kind,
            header.Codec,
            flags,
            hasRequestSeq,
            hasMetadata,
            hasCorrelationId,
            hasFlowId,
            hasActorSlot
        );

        flags = hasRequestSeq
            ? flags | ZlinkStreamHeaderFlags.HasRequestSeq
            : flags & ~ZlinkStreamHeaderFlags.HasRequestSeq;
        flags = hasMetadata
            ? flags | ZlinkStreamHeaderFlags.HasMetadata
            : flags & ~ZlinkStreamHeaderFlags.HasMetadata;
        flags = hasCorrelationId
            ? flags | ZlinkStreamHeaderFlags.HasCorrelationId
            : flags & ~ZlinkStreamHeaderFlags.HasCorrelationId;
        flags = hasFlowId
            ? flags | ZlinkStreamHeaderFlags.HasFlowId
            : flags & ~ZlinkStreamHeaderFlags.HasFlowId;
        flags = hasActorSlot
            ? flags | ZlinkStreamHeaderFlags.HasActorSlot
            : flags & ~ZlinkStreamHeaderFlags.HasActorSlot;

        var metadataSize = hasMetadata
            ? ZlinkStreamMetadataCodec.GetPayloadSize(header.Metadata)
            : 0;
        if (metadataSize > MaxMetadataPayloadSize)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ValidationFailed,
                $"Metadata payload exceeds fixed limit ({MaxMetadataPayloadSize})."
            );

        var size =
            FixedPrefixSize
            + (hasRequestSeq ? sizeof(ulong) : 0)
            + sizeof(byte)
            + nameLength
            + (hasMetadata ? sizeof(ushort) + metadataSize : 0)
            + (hasCorrelationId ? sizeof(byte) + correlationLength : 0)
            + (hasFlowId ? ZlinkStreamFlowId.EncodedLength + sizeof(byte) : 0)
            + (hasActorSlot ? sizeof(ushort) : 0);
        var buffer = new byte[size];
        var offset = 0;
        buffer[offset++] = ZlinkStreamFlowId.FormatMarker;
        buffer[offset++] = (byte)header.Kind;
        buffer[offset++] = (byte)header.Codec;
        buffer[offset++] = (byte)flags;

        if (hasRequestSeq)
        {
            if (header.RequestSeq!.Value.Value == 0)
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.ValidationFailed,
                    "Request sequence must not be zero."
                );

            BinaryPrimitives.WriteUInt64BigEndian(
                buffer.AsSpan(offset, sizeof(ulong)),
                header.RequestSeq.Value.Value
            );
            offset += sizeof(ulong);
        }

        buffer[offset++] = (byte)nameLength;
        Encoding.UTF8.GetBytes(header.Name, buffer.AsSpan(offset, nameLength));
        offset += nameLength;

        if (hasMetadata)
        {
            BinaryPrimitives.WriteUInt16BigEndian(
                buffer.AsSpan(offset, sizeof(ushort)),
                checked((ushort)metadataSize)
            );
            offset += sizeof(ushort);
            ZlinkStreamMetadataCodec.Write(header.Metadata, buffer.AsSpan(offset, metadataSize));
            offset += metadataSize;
        }

        if (hasCorrelationId)
        {
            buffer[offset++] = (byte)correlationLength;
            Encoding.UTF8.GetBytes(correlationId, buffer.AsSpan(offset, correlationLength));
            offset += correlationLength;
        }

        if (hasFlowId)
        {
            Encoding.ASCII.GetBytes(
                header.FlowId!,
                buffer.AsSpan(offset, ZlinkStreamFlowId.EncodedLength)
            );
            offset += ZlinkStreamFlowId.EncodedLength;
            buffer[offset++] = FlowOriginToWire(header.FlowOrigin!.Value)!.Value;
        }

        if (hasActorSlot)
        {
            BinaryPrimitives.WriteUInt16BigEndian(
                buffer.AsSpan(offset, sizeof(ushort)),
                header.ActorSlot!.Value
            );
            offset += sizeof(ushort);
        }

        return buffer;
    }

    public ZlinkStreamHeader Decode(ReadOnlyMemory<byte> header, bool captureFlow = true)
    {
        var span = header.Span;
        if (span.Length < FixedPrefixSize + sizeof(byte))
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.FrameDecodeFailed,
                "Helper header is too short."
            );
        if (span[0] != ZlinkStreamFlowId.FormatMarker)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.FrameDecodeFailed,
                "Stream format marker is invalid."
            );

        var kind = (ZlinkStreamMessageKind)span[KindOffset];
        var codec = (ZlinkStreamCodec)span[CodecOffset];
        var flags = (ZlinkStreamHeaderFlags)span[FlagsOffset];
        ValidateEnum(kind, codec, flags);

        var offset = FixedPrefixSize;
        ZlinkStreamRequestSeq? requestSeq = null;
        if (flags.HasFlag(ZlinkStreamHeaderFlags.HasRequestSeq))
        {
            if (span.Length - offset < sizeof(ulong))
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Helper header request sequence is incomplete."
                );

            var requestSeqValue = BinaryPrimitives.ReadUInt64BigEndian(
                span.Slice(offset, sizeof(ulong))
            );
            if (requestSeqValue == 0)
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Request sequence must not be zero."
                );

            requestSeq = new ZlinkStreamRequestSeq(requestSeqValue);
            offset += sizeof(ulong);
        }

        if (span.Length - offset < sizeof(byte))
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.FrameDecodeFailed,
                "Helper header name length is missing."
            );

        var nameLength = span[offset++];
        if (span.Length - offset < nameLength)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.FrameDecodeFailed,
                "Helper header packet name is invalid."
            );

        var name = Encoding.UTF8.GetString(span.Slice(offset, nameLength));
        offset += nameLength;

        var metadata = ZlinkStreamMetadata.Empty;
        if (flags.HasFlag(ZlinkStreamHeaderFlags.HasMetadata))
        {
            if (span.Length - offset < sizeof(ushort))
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Helper header metadata length is missing."
                );

            var metadataLength = BinaryPrimitives.ReadUInt16BigEndian(
                span.Slice(offset, sizeof(ushort))
            );
            offset += sizeof(ushort);
            if (span.Length - offset < metadataLength)
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Helper header metadata is incomplete."
                );

            metadata = ZlinkStreamMetadataCodec.Decode(span.Slice(offset, metadataLength));
            offset += metadataLength;
        }

        string? correlationId = null;
        if (flags.HasFlag(ZlinkStreamHeaderFlags.HasCorrelationId))
        {
            if (span.Length - offset < sizeof(byte))
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Helper header correlation id length is missing."
                );

            var correlationLength = span[offset++];
            if (correlationLength == 0 || span.Length - offset < correlationLength)
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Helper header correlation id is invalid."
                );

            correlationId = Encoding.UTF8.GetString(span.Slice(offset, correlationLength));
            offset += correlationLength;
        }

        string? flowId = null;
        ZlinkStreamFlowOrigin? flowOrigin = null;
        if (flags.HasFlag(ZlinkStreamHeaderFlags.HasFlowId))
        {
            if (span.Length - offset < ZlinkStreamFlowId.EncodedLength + sizeof(byte))
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Helper header flow fields are incomplete."
                );

            if (captureFlow)
            {
                flowId = Encoding.ASCII.GetString(
                    span.Slice(offset, ZlinkStreamFlowId.EncodedLength)
                );
                offset += ZlinkStreamFlowId.EncodedLength;
                flowOrigin = FlowOriginFromWire(span[offset++]);
                if (!ZlinkStreamFlowId.IsValid(flowId) || flowOrigin is null)
                    throw ZlinkStreamConnector.Error(
                        ZlinkStreamErrorCode.FrameDecodeFailed,
                        "Helper header flow fields are invalid."
                    );
            }
            else
            {
                // At Off the pair is framing only: advance over it without
                // allocating, validating, or retaining observation state.
                offset += ZlinkStreamFlowId.EncodedLength + sizeof(byte);
            }
        }

        ushort? actorSlot = null;
        if (flags.HasFlag(ZlinkStreamHeaderFlags.HasActorSlot))
        {
            if (span.Length - offset < sizeof(ushort))
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Actor slot field is truncated."
                );
            actorSlot = BinaryPrimitives.ReadUInt16BigEndian(span.Slice(offset, sizeof(ushort)));
            offset += sizeof(ushort);
            if (actorSlot == 0)
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Actor slot must not be zero."
                );
        }

        if (offset != span.Length)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.FrameDecodeFailed,
                "Helper header contains trailing bytes."
            );

        ValidatePacketName(kind, name, ZlinkStreamErrorCode.FrameDecodeFailed);
        ValidateHeaderSemantics(
            kind,
            codec,
            flags,
            requestSeq is not null,
            metadata.Count > 0,
            correlationId is not null,
            flags.HasFlag(ZlinkStreamHeaderFlags.HasFlowId),
            actorSlot is not null
        );
        return new ZlinkStreamHeader(
            kind,
            codec,
            flags,
            requestSeq,
            name,
            metadata,
            correlationId,
            flowId,
            flowOrigin,
            actorSlot
        );
    }

    /// <summary>
    ///     Converts a <see cref="ZlinkStreamFlowOrigin" /> to its <c>flow_origin</c> wire
    ///     value. The wire values are 1..4 while the enum ordinals are 0..3, so the two
    ///     never travel through an integer cast (.NET spec §11).
    /// </summary>
    internal static byte? FlowOriginToWire(ZlinkStreamFlowOrigin origin) =>
        origin switch
        {
            ZlinkStreamFlowOrigin.Inbound => InboundWire,
            ZlinkStreamFlowOrigin.Timer => TimerWire,
            ZlinkStreamFlowOrigin.Application => ApplicationWire,
            ZlinkStreamFlowOrigin.Lifecycle => LifecycleWire,
            _ => null,
        };

    /// <summary>
    ///     Converts a <c>flow_origin</c> wire value back to <see cref="ZlinkStreamFlowOrigin" />,
    ///     or <see langword="null" /> when the byte is outside the closed set.
    /// </summary>
    internal static ZlinkStreamFlowOrigin? FlowOriginFromWire(byte wire) =>
        wire switch
        {
            InboundWire => ZlinkStreamFlowOrigin.Inbound,
            TimerWire => ZlinkStreamFlowOrigin.Timer,
            ApplicationWire => ZlinkStreamFlowOrigin.Application,
            LifecycleWire => ZlinkStreamFlowOrigin.Lifecycle,
            _ => null,
        };

    private static void ValidatePacketName(
        ZlinkStreamMessageKind kind,
        string name,
        ZlinkStreamErrorCode errorCode
    )
    {
        var isReply = kind is ZlinkStreamMessageKind.Response or ZlinkStreamMessageKind.Error;
        if (isReply)
        {
            if (name.Length != 0)
                throw ZlinkStreamConnector.Error(
                    errorCode,
                    "Response and error packets must not contain a packet name."
                );
            return;
        }

        ZlinkStreamConnector.ValidateName(name, kind == ZlinkStreamMessageKind.Control, errorCode);
    }

    private static void ValidateEnum(
        ZlinkStreamMessageKind kind,
        ZlinkStreamCodec codec,
        ZlinkStreamHeaderFlags flags
    )
    {
        if (!Enum.IsDefined(typeof(ZlinkStreamMessageKind), kind))
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.FrameDecodeFailed,
                "Unknown stream message kind."
            );

        if (!Enum.IsDefined(typeof(ZlinkStreamCodec), codec))
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.FrameDecodeFailed,
                "Unknown stream codec."
            );

        if ((flags & ~KnownFlags) != 0)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.FrameDecodeFailed,
                "Unknown stream header flag."
            );
    }

    private static void ValidateHeaderSemantics(
        ZlinkStreamMessageKind kind,
        ZlinkStreamCodec codec,
        ZlinkStreamHeaderFlags flags,
        bool hasRequestSeq,
        bool hasMetadata,
        bool hasCorrelationId,
        bool hasFlowId,
        bool hasActorSlot
    )
    {
        if (kind == ZlinkStreamMessageKind.Send && hasRequestSeq)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.FrameDecodeFailed,
                "Send packet must not contain a request sequence."
            );

        if (
            kind is ZlinkStreamMessageKind.Request or ZlinkStreamMessageKind.Response
            && !hasRequestSeq
        )
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.FrameDecodeFailed,
                "Request and response packets must contain a request sequence."
            );

        if (kind == ZlinkStreamMessageKind.Error && codec != ZlinkStreamCodec.Json)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.FrameDecodeFailed,
                "Error packet must use the JSON codec."
            );

        if (kind == ZlinkStreamMessageKind.Control)
        {
            if (flags != ZlinkStreamHeaderFlags.None)
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Control packet must not contain flags."
                );

            if (codec != ZlinkStreamCodec.Raw)
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Control packet must use the raw codec."
                );

            if (hasRequestSeq)
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Control packet must not contain a request sequence."
                );

            if (hasMetadata)
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Control packet must not contain metadata."
                );

            if (hasCorrelationId)
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Control packet must not contain a correlation id."
                );

            if (hasFlowId)
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Control packet must not contain flow fields."
                );

            if (hasActorSlot)
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.FrameDecodeFailed,
                    "Control packet must not contain an actor slot."
                );
        }
    }
}
