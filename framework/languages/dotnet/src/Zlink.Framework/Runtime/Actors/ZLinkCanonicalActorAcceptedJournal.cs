using System.Buffers.Binary;
using System.Text;
using Systems.Zlink.Framework.Runtime.Protocol;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.Runtime.Actors;

internal sealed record ZLinkCanonicalActorAcceptedFrame(
    ZLinkActorAcceptedRecord Accepted,
    ZLinkBackendActorRef TargetActor,
    RoutingId TargetNodeRid
)
{
    internal ZLinkActorHandoffFrame Frame => Accepted.Frame;
    internal ZLinkServiceWireCodec.RequestSourceFence Source => Accepted.RequestSource;
}

internal static class ZLinkCanonicalActorAcceptedJournal
{
    private const byte FramePayloadVersion = 1;
    private const byte BoundSessionFramePayloadVersion = 2;
    private const uint RequestFlag = 1U;
    private const string FrameContentType = "application/x-zlink-actor-frame-v1";
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    internal static byte[] Encode(
        ZLinkActorAcceptedRecord accepted,
        ZLinkBackendActorRef targetActor
    )
    {
        ArgumentNullException.ThrowIfNull(accepted);
        var frame = accepted.Frame;
        var source = accepted.RequestSource;
        var boundSession = frame.BoundSessionSource;
        ArgumentNullException.ThrowIfNull(frame);
        ValidateSource(source);
        if (
            !frame.SourceNodeRid.AsSpan().SequenceEqual(source.NodeRid.ToBytes())
            || frame.SourceNodeGeneration != source.NodeGeneration
        )
            throw new InvalidOperationException(
                "The accepted Actor frame does not match its request-source identity."
            );
        var operation = frame.RouteContext.OperationId;
        var request = (frame.Flags & RequestFlag) != 0;
        var replyRoute = frame.RelocationReplyRouteId;
        if (
            operation == default
            || targetActor.ActorId is not { Length: > 0 }
            || targetActor.Generation == 0
            || targetActor.NodeRid.Size == 0
            || frame.RouteContext.TargetNodeGeneration == 0
            || frame.RouteContext.AuthorityOwnerGeneration == 0
            || frame.RouteContext.OwnerLeaseGeneration == 0
            || request && replyRoute == 0
            || !request && replyRoute != 0
        )
            throw new InvalidOperationException(
                "An accepted Actor frame requires exact operation, reply and authority fences."
            );

        var multipart = CanWriteMultipart(frame, source, targetActor, request, replyRoute);
        using var stream = new MemoryStream();
        stream.WriteByte(
            request
                ? (byte)ServiceWireCodec.MeshRecordKind.ActorRequest
                : (byte)ServiceWireCodec.MeshRecordKind.ActorSend
        );
        stream.WriteByte(
            boundSession is null
                ? (byte)ServiceWireCodec.FrozenSourceKind.Node
                : (byte)ServiceWireCodec.FrozenSourceKind.BoundSession
        );
        using (var identity = new MemoryStream())
        {
            WriteRid(identity, source.NodeRid, multipart);
            U64(identity, source.NodeGeneration);
            Text8(identity, source.OwnerId);
            U64(identity, source.LeaseGeneration);
            if (boundSession is { } bound)
            {
                if (
                    bound.ActorId != targetActor.ActorId
                    || bound.SessionRid.IsEmpty
                    || string.IsNullOrWhiteSpace(bound.BindingToken)
                    || bound.BindingGeneration == 0
                    || bound.SessionSequence == 0
                    || frame.RouteContext.IsDirectRoute
                    || !frame.RouteContext.IsBoundSessionRoute
                )
                    throw new InvalidOperationException(
                        "The accepted Actor bound-session fence is invalid."
                    );
                Text8(identity, bound.ActorId);
                U64(identity, bound.ActorGeneration);
                WriteRid(identity, bound.SessionRid, multipart);
                U64(identity, bound.BindingGeneration);
                U64(identity, bound.SessionSequence);
            }
            U16(stream, checked((ushort)identity.Length));
            identity.Position = 0;
            identity.CopyTo(stream);
        }
        stream.WriteByte(0); // metadata is carried byte-exact in the frame header
        U64(stream, operation.High);
        U64(stream, operation.Low);
        U32(
            stream,
            request
                ? (uint)ServiceWireCodec.MeshOperationKind.ActorRequest
                : (uint)ServiceWireCodec.MeshOperationKind.None
        );
        U16(stream, request ? (ushort)sizeof(ulong) : (ushort)0);
        if (request)
            U64(stream, replyRoute);

        Text8(stream, targetActor.ActorId);
        U64(stream, targetActor.Generation);
        WriteRid(stream, targetActor.NodeRid, multipart);
        U64(stream, frame.RouteContext.TargetNodeGeneration);
        U64(stream, frame.RouteContext.AuthorityOwnerGeneration);
        U64(stream, frame.RouteContext.OwnerLeaseGeneration);

        stream.Write(
            multipart
                ? ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(
                    new ReadOnlyMemory<byte>[] { frame.Header, frame.Body }
                )
                : ZLinkApplicationPayloadEnvelopeCodec.Encode(
                    ZLinkStreamProtocolDefaults.DecodeHeader(frame.Header).Name,
                    FrameContentType,
                    EncodeFramePayload(frame)
                )
        );
        var encoded = stream.ToArray();
        if (!ZLinkRelocationEnvelopeCodec.TryValidateCanonicalFrozenRecord(encoded))
            throw new InvalidOperationException(
                "The accepted Actor frame does not satisfy the canonical frozen-record contract."
            );
        return encoded;
    }

    internal static ZLinkCanonicalActorAcceptedFrame Decode(
        ReadOnlySpan<byte> encoded,
        long arrivalIndex
    )
    {
        if (!ZLinkRelocationEnvelopeCodec.TryValidateCanonicalFrozenRecord(encoded))
            throw new InvalidDataException("The accepted Actor frozen record is malformed.");
        var record = ServiceWireCodec.DecodeFrozenRecord(
            encoded.ToArray(),
            ServiceWireCodec.DecodeContext.Empty with
            {
                EffectiveCompleteMessageBytes = encoded.Length,
                EffectiveCompleteMessageBytesMinusActualEnvelopeOverhead = encoded.Length,
            }
        );
        var (request, fence, payload) = record.Body switch
        {
            ServiceWireCodec.FrozenRecordBodyCase8 send => (false, send.TargetActor, send.Payload),
            ServiceWireCodec.FrozenRecordBodyCase9 call => (true, call.TargetActor, call.Payload),
            _ => throw new InvalidDataException("The accepted Actor record kind is invalid."),
        };
        var legacy = IsLegacyContentType(payload.ContentType.Value);
        var (source, boundSession) = DecodeSource(record.Source, legacy);
        var replyRoute = record.ReplyRoute is ServiceWireCodec.FrozenReplyRouteCase3 reply
            ? reply.ReplyRouteId.Value
            : 0;
        var operation = new MeshOperationId(
            record.OperationId.High.Value,
            record.OperationId.Low.Value
        );
        var targetNodeRid = DecodeRid(fence.TargetNodeRid.RoutingId, legacy);
        var actor = new ZLinkBackendActorRef(
            targetNodeRid,
            fence.Actor.ActorId.Value,
            fence.Actor.ObjectGeneration.Value
        );
        var route = new ZLinkBackendActorRouteContext(
            operation,
            0,
            fence.TargetNodeGeneration.Value,
            fence.ExpectedAuthorityOwnerGeneration.Value,
            fence.ExpectedOwnerLeaseGeneration.Value,
            replyRoute,
            request ? RequestFlag : 0,
            IsBoundSessionRoute: boundSession is not null
        );
        var frame = legacy
            ? DecodeFramePayload(
                payload.Payload.Value,
                source,
                boundSession,
                operation,
                replyRoute,
                route.TargetNodeGeneration,
                route.AuthorityOwnerGeneration,
                route.OwnerLeaseGeneration,
                arrivalIndex
            )
            : DecodeMultipartFrame(
                payload,
                request,
                actor,
                source,
                boundSession,
                route,
                arrivalIndex,
                encoded.Length
            );
        return new ZLinkCanonicalActorAcceptedFrame(
            new ZLinkActorAcceptedRecord(frame, source, actor),
            actor,
            targetNodeRid
        );
    }

    private static bool CanWriteMultipart(
        ZLinkActorHandoffFrame frame,
        ZLinkServiceWireCodec.RequestSourceFence source,
        ZLinkBackendActorRef actor,
        bool request,
        ulong replyRoute
    )
    {
        // A direct reply uses the frozen request source and reply route.
        // The callback capability stays with the source Actor's in-memory handoff.
        var flags = request ? RequestFlag : 0;
        var replyActorIsDerived =
            (frame.ReplyActorNodeRid.Length == 0 && frame.ReplyActorGeneration == 0)
            || (
                frame.ReplyActorNodeRid.AsSpan().SequenceEqual(actor.NodeRid.ToBytes())
                && frame.ReplyActorGeneration == actor.Generation
            );
        return replyActorIsDerived
            && frame.RequestId == replyRoute
            && frame.Flags == flags
            && frame.RouteContext.MessageFollowHopCount == 0
            && (frame.RouteContext.ReplyFlags == 0 || frame.RouteContext.ReplyFlags == flags)
            && (
                frame.RouteContext.ReplyCapability is null
                || (request && frame.RouteContext.IsDirectRoute)
            )
            && (
                frame.BoundSessionSource is { } bound
                    ? frame.SourceSessionRid.AsSpan().SequenceEqual(bound.SessionRid.ToBytes())
                        && bound.BindingToken
                            == BindingToken(source, bound.SessionRid, bound.BindingGeneration)
                    : frame.SourceSessionRid.Length == 0
            );
    }

    private static (
        ZLinkServiceWireCodec.RequestSourceFence Source,
        ZLinkActorBoundSessionHandoffFence? Bound
    ) DecodeSource(ServiceWireCodec.FrozenSourceIdentity identity, bool legacy)
    {
        ZLinkServiceWireCodec.RequestSourceFence source;
        ZLinkActorBoundSessionHandoffFence? bound = null;
        switch (identity)
        {
            case ServiceWireCodec.FrozenSourceIdentityCase0 node:
                source = new(
                    node.SourceOwnerId.Value,
                    node.SourceOwnerLeaseGeneration.Value,
                    DecodeRid(node.SourceNodeRid.RoutingId, legacy),
                    node.SourceNodeGeneration.Value
                );
                break;
            case ServiceWireCodec.FrozenSourceIdentityCase2 actor when !legacy:
                source = new(
                    actor.SourceOwnerId.Value,
                    actor.SourceOwnerLeaseGeneration.Value,
                    actor.SourceNodeRid.RoutingId,
                    actor.SourceNodeGeneration.Value
                );
                break;
            case ServiceWireCodec.FrozenSourceIdentityCase3 session:
                source = new(
                    session.SourceOwnerId.Value,
                    session.SourceOwnerLeaseGeneration.Value,
                    DecodeRid(session.SourceNodeRid.RoutingId, legacy),
                    session.SourceNodeGeneration.Value
                );
                var sessionRid = DecodeRid(session.SourceSessionRid.RoutingId, legacy);
                var bindingGeneration = session.SourceBindingGeneration.Value;
                bound = new(
                    session.SourceActor.ActorId.Value,
                    session.SourceActor.ObjectGeneration.Value,
                    sessionRid,
                    legacy ? string.Empty : BindingToken(source, sessionRid, bindingGeneration),
                    bindingGeneration,
                    session.SourceSessionSequence.Value
                );
                break;
            default:
                throw new InvalidDataException("The accepted Actor source kind is invalid.");
        }
        ValidateDecodedSource(source);
        return (source, bound);
    }

    private static string BindingToken(
        ZLinkServiceWireCodec.RequestSourceFence source,
        RoutingId sessionRid,
        ulong bindingGeneration
    ) =>
        ZLinkActorBoundSessionBindingToken.Canonical(
            source.NodeRid,
            source.NodeGeneration,
            source.OwnerId,
            source.LeaseGeneration,
            sessionRid,
            bindingGeneration
        );

    private static ZLinkActorHandoffFrame DecodeMultipartFrame(
        ServiceWireCodec.ApplicationPayloadEnvelopeV1 payload,
        bool request,
        ZLinkBackendActorRef actor,
        ZLinkServiceWireCodec.RequestSourceFence source,
        ZLinkActorBoundSessionHandoffFence? boundSession,
        ZLinkBackendActorRouteContext route,
        long arrivalIndex,
        int encodedLength
    )
    {
        var envelope = new ZLinkApplicationPayloadEnvelope(
            payload.PacketName.Value,
            payload.ContentType.Value,
            payload.Payload.Value
        );
        if (
            !ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(
                envelope,
                out var parts
            )
        )
            throw new InvalidDataException("The accepted Actor payload type is invalid.");
        try
        {
            if (parts.Length != 2)
                throw new InvalidDataException(
                    "The accepted Actor multipart must contain two parts."
                );
            var header = parts[0].AsReadOnlySpan().ToArray();
            if (
                ZLinkStreamProtocolDefaults.DecodeHeader(header).Kind
                != (request ? ZlinkStreamMessageKind.Request : ZlinkStreamMessageKind.Send)
            )
                throw new InvalidDataException("The accepted Actor STREAM kind is invalid.");
            return new ZLinkActorHandoffFrame(
                Zlink.Framework.Runtime.Dispatch.ZLinkApplicationJobOrigin.Remote,
                actor.NodeRid.ToBytes().ToArray(),
                actor.Generation,
                source.NodeRid.ToBytes().ToArray(),
                boundSession is { } bound ? bound.SessionRid.ToBytes().ToArray() : [],
                route.ReplyRequestId,
                request ? RequestFlag : 0,
                header,
                parts[1].AsReadOnlySpan().ToArray(),
                arrivalIndex,
                route,
                source.NodeGeneration,
                source,
                route.ReplyRequestId,
                encodedLength,
                boundSession,
                CanonicalAcceptedSequence: checked((ulong)arrivalIndex)
            );
        }
        catch (ZlinkStreamException error)
        {
            throw new InvalidDataException("The accepted Actor STREAM header is invalid.", error);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    private static byte[] EncodeFramePayload(ZLinkActorHandoffFrame frame)
    {
        using var stream = new MemoryStream();
        stream.WriteByte(
            frame.BoundSessionSource is null ? FramePayloadVersion : BoundSessionFramePayloadVersion
        );
        Bytes8(stream, frame.ReplyActorNodeRid);
        U64(stream, frame.ReplyActorGeneration);
        Bytes8(stream, frame.SourceSessionRid);
        U64(stream, frame.RequestId);
        U32(stream, frame.Flags);
        stream.WriteByte(frame.RouteContext.MessageFollowHopCount);
        U32(stream, frame.RouteContext.ReplyFlags);
        OptionalText8(stream, frame.RouteContext.ReplyCapability);
        Bytes32(stream, frame.Header);
        Bytes32(stream, frame.Body);
        if (frame.BoundSessionSource is { } boundSession)
            Text8(stream, boundSession.BindingToken);
        return stream.ToArray();
    }

    private static ZLinkActorHandoffFrame DecodeFramePayload(
        ReadOnlySpan<byte> encoded,
        ZLinkServiceWireCodec.RequestSourceFence requestSource,
        ZLinkActorBoundSessionHandoffFence? boundSession,
        MeshOperationId operation,
        ulong replyRoute,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        long arrivalIndex
    )
    {
        var reader = new Reader(encoded);
        var payloadVersion = reader.Byte();
        if (
            payloadVersion is not (FramePayloadVersion or BoundSessionFramePayloadVersion)
            || (payloadVersion == BoundSessionFramePayloadVersion) != (boundSession is not null)
        )
            throw new InvalidDataException("The accepted Actor frame payload version is invalid.");
        var replyActorRid = reader.Bytes(reader.Byte()).ToArray();
        var replyActorGeneration = reader.U64();
        var sessionRid = reader.Bytes(reader.Byte()).ToArray();
        var requestId = reader.U64();
        var flags = reader.U32();
        var hop = reader.Byte();
        var replyFlags = reader.U32();
        var capability = reader.Byte() == 1 ? reader.Text8() : null;
        var header = reader.Bytes(checked((int)reader.U32())).ToArray();
        var body = reader.Bytes(checked((int)reader.U32())).ToArray();
        if (boundSession is not null)
            boundSession = boundSession with { BindingToken = reader.Text8() };
        reader.End();
        if (
            hop > ZLinkServiceWireCodec.MessageFollowMaximumHopCount
            || ((flags & RequestFlag) != 0) != (replyRoute != 0)
        )
            throw new InvalidDataException("The accepted Actor route context is invalid.");
        return new ZLinkActorHandoffFrame(
            Zlink.Framework.Runtime.Dispatch.ZLinkApplicationJobOrigin.Remote,
            replyActorRid,
            replyActorGeneration,
            requestSource.NodeRid.ToBytes().ToArray(),
            sessionRid,
            requestId,
            flags,
            header,
            body,
            arrivalIndex,
            new ZLinkBackendActorRouteContext(
                operation,
                hop,
                targetNodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration,
                replyRoute,
                replyFlags,
                capability,
                IsBoundSessionRoute: boundSession is not null
            ),
            requestSource.NodeGeneration,
            requestSource,
            replyRoute,
            encoded.Length,
            boundSession,
            CanonicalAcceptedSequence: checked((ulong)arrivalIndex)
        );
    }

    private static void ValidateSource(ZLinkServiceWireCodec.RequestSourceFence source)
    {
        if (
            source.NodeRid.Size == 0
            || source.NodeGeneration == 0
            || string.IsNullOrWhiteSpace(source.OwnerId)
            || source.LeaseGeneration == 0
        )
            throw new ArgumentOutOfRangeException(nameof(source));
    }

    private static void ValidateDecodedSource(ZLinkServiceWireCodec.RequestSourceFence source)
    {
        try
        {
            ValidateSource(source);
        }
        catch (ArgumentOutOfRangeException error)
        {
            throw new InvalidDataException(
                "The accepted Actor request-source fence is invalid.",
                error
            );
        }
    }

    private static void WriteRid(Stream stream, RoutingId rid, bool multipart)
    {
        if (multipart)
            Bytes8(stream, rid.ToBytes());
        else
            Text8(stream, rid.ToHex());
    }

    internal static bool IsLegacyContentType(string contentType) => contentType == FrameContentType;

    internal static RoutingId DecodeRid(RoutingId rid, bool legacy)
    {
        if (!legacy)
            return rid;
        try
        {
            return RoutingId.FromHex(StrictUtf8.GetString(rid.ToBytes()));
        }
        catch (Exception error) when (error is ArgumentException or FormatException)
        {
            throw new InvalidDataException("The accepted Actor RID is invalid.", error);
        }
    }

    private static void OptionalText8(Stream stream, string? value)
    {
        stream.WriteByte(value is null ? (byte)0 : (byte)1);
        if (value is not null)
            Text8(stream, value);
    }

    private static void Text8(Stream stream, string value)
    {
        var bytes = StrictUtf8.GetBytes(value);
        if (bytes.Length is < 1 or > byte.MaxValue || bytes.AsSpan().Contains((byte)0))
            throw new InvalidDataException("Canonical text8 is invalid.");
        stream.WriteByte(checked((byte)bytes.Length));
        stream.Write(bytes);
    }

    private static void Bytes8(Stream stream, ReadOnlySpan<byte> value)
    {
        if (value.Length > byte.MaxValue)
            throw new InvalidDataException("bytes8 exceeds its bound.");
        stream.WriteByte(checked((byte)value.Length));
        stream.Write(value);
    }

    private static void Bytes32(Stream stream, ReadOnlySpan<byte> value)
    {
        U32(stream, checked((uint)value.Length));
        stream.Write(value);
    }

    private static void U16(Stream stream, ushort value)
    {
        Span<byte> bytes = stackalloc byte[2];
        BinaryPrimitives.WriteUInt16BigEndian(bytes, value);
        stream.Write(bytes);
    }

    private static void U32(Stream stream, uint value)
    {
        Span<byte> bytes = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(bytes, value);
        stream.Write(bytes);
    }

    private static void U64(Stream stream, ulong value)
    {
        Span<byte> bytes = stackalloc byte[8];
        BinaryPrimitives.WriteUInt64BigEndian(bytes, value);
        stream.Write(bytes);
    }

    private ref struct Reader(ReadOnlySpan<byte> bytes)
    {
        private ReadOnlySpan<byte> _bytes = bytes;
        private int _offset;

        internal byte Byte() => Bytes(1)[0];

        internal ushort U16() => BinaryPrimitives.ReadUInt16BigEndian(Bytes(2));

        internal uint U32() => BinaryPrimitives.ReadUInt32BigEndian(Bytes(4));

        internal ulong U64() => BinaryPrimitives.ReadUInt64BigEndian(Bytes(8));

        internal string Text8()
        {
            try
            {
                return StrictUtf8.GetString(Bytes(Byte()));
            }
            catch (DecoderFallbackException error)
            {
                throw new InvalidDataException("Canonical text8 is invalid.", error);
            }
        }

        internal ReadOnlySpan<byte> Bytes(int count)
        {
            if (count < 0 || _offset > _bytes.Length - count)
                throw new EndOfStreamException();
            var result = _bytes.Slice(_offset, count);
            _offset += count;
            return result;
        }

        internal void End()
        {
            if (_offset != _bytes.Length)
                throw new InvalidDataException("Canonical record has trailing bytes.");
        }
    }
}
