using System.Buffers.Binary;
using System.Text;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.Runtime.Actors;

internal sealed record ZLinkCanonicalActorAcceptedFrame(
    ZLinkActorAcceptedRecord Accepted,
    ZLinkBackendActorRef TargetActor,
    RoutingId TargetNodeRid)
{
    internal ZLinkActorHandoffFrame Frame => Accepted.Frame;
    internal ZLinkServiceWireCodec.RequestSourceFence Source =>
        Accepted.RequestSource;
}

internal static class ZLinkCanonicalActorAcceptedJournal
{
    private const string FrameContentType =
        "application/x-zlink-actor-frame-v1";
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    internal static byte[] Encode(
        ZLinkActorAcceptedRecord accepted,
        ZLinkBackendActorRef targetActor)
    {
        ArgumentNullException.ThrowIfNull(accepted);
        var frame = accepted.Frame;
        var source = accepted.RequestSource;
        var boundSession = frame.BoundSessionSource;
        ArgumentNullException.ThrowIfNull(frame);
        ValidateSource(source);
        if (!frame.SourceNodeRid.AsSpan().SequenceEqual(
                source.NodeRid.ToBytes())
            || frame.SourceNodeGeneration != source.NodeGeneration)
            throw new InvalidOperationException(
                "The accepted Actor frame does not match its request-source identity.");
        var operation = frame.RouteContext.OperationId;
        var request = (frame.Flags & 1U) != 0;
        var replyRoute = frame.RelocationReplyRouteId;
        if (operation == default
            || targetActor.ActorId is not { Length: > 0 }
            || targetActor.Generation == 0
            || targetActor.NodeRid.Size == 0
            || frame.RouteContext.TargetNodeGeneration == 0
            || frame.RouteContext.AuthorityOwnerGeneration == 0
            || frame.RouteContext.OwnerLeaseGeneration == 0
            || request && replyRoute == 0
            || !request && replyRoute != 0)
            throw new InvalidOperationException(
                "An accepted Actor frame requires exact operation, reply and authority fences.");

        using var stream = new MemoryStream();
        stream.WriteByte(request ? (byte)10 : (byte)9);
        stream.WriteByte(boundSession is null ? (byte)1 : (byte)4);
        using (var identity = new MemoryStream())
        {
            Text8(identity, source.NodeRid.ToHex());
            U64(identity, source.NodeGeneration);
            Text8(identity, source.OwnerId);
            U64(identity, source.LeaseGeneration);
            if (boundSession is { } bound)
            {
                if (bound.ActorId != targetActor.ActorId
                    || bound.ActorGeneration != targetActor.Generation
                    || bound.SessionRid.IsEmpty
                    || string.IsNullOrWhiteSpace(bound.BindingToken)
                    || bound.BindingGeneration == 0
                    || bound.SessionSequence == 0
                    || frame.RouteContext.IsDirectRoute
                    || !frame.RouteContext.IsBoundSessionRoute)
                    throw new InvalidOperationException(
                        "The accepted Actor bound-session fence is invalid.");
                Text8(identity, bound.ActorId);
                U64(identity, bound.ActorGeneration);
                Text8(identity, bound.SessionRid.ToHex());
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
        U32(stream, request ? 4U : 0U);
        U16(stream, request ? (ushort)sizeof(ulong) : (ushort)0);
        if (request) U64(stream, replyRoute);

        Text8(stream, targetActor.ActorId);
        U64(stream, targetActor.Generation);
        Text8(stream, targetActor.NodeRid.ToHex());
        U64(stream, frame.RouteContext.TargetNodeGeneration);
        U64(stream, frame.RouteContext.AuthorityOwnerGeneration);
        U64(stream, frame.RouteContext.OwnerLeaseGeneration);

        var payload = EncodeFramePayload(frame);
        stream.WriteByte(1); // application-payload-envelope-v1
        using (var body = new MemoryStream())
        {
            Text8(body, ZLinkStreamProtocolDefaults.DecodeHeader(frame.Header).Name);
            Text8(body, FrameContentType);
            U32(body, checked((uint)payload.Length));
            body.Write(payload);
            U32(stream, checked((uint)body.Length));
            body.Position = 0;
            body.CopyTo(stream);
        }
        var encoded = stream.ToArray();
        if (!ZLinkRelocationEnvelopeCodec.TryValidateCanonicalFrozenRecord(encoded))
            throw new InvalidOperationException(
                "The accepted Actor frame does not satisfy the canonical frozen-record contract.");
        return encoded;
    }

    internal static ZLinkCanonicalActorAcceptedFrame Decode(
        ReadOnlySpan<byte> encoded,
        long arrivalIndex)
    {
        if (!ZLinkRelocationEnvelopeCodec.TryValidateCanonicalFrozenRecord(encoded))
            throw new InvalidDataException(
                "The accepted Actor frozen record is malformed.");
        var reader = new Reader(encoded);
        var kind = reader.Byte();
        if (kind is not (9 or 10))
            throw new InvalidDataException("The accepted Actor record kind is invalid.");
        var sourceKind = reader.Byte();
        if (sourceKind is not (1 or 4))
            throw new InvalidDataException("The accepted Actor source kind is invalid.");
        var sourceReader = new Reader(reader.Bytes(reader.U16()));
        var sourceNodeRid = TextRid(sourceReader.Text8());
        var sourceNodeGeneration = sourceReader.U64();
        var sourceOwnerId = sourceReader.Text8();
        var sourceOwnerLease = sourceReader.U64();
        var source = new ZLinkServiceWireCodec.RequestSourceFence(
            sourceOwnerId,
            sourceOwnerLease,
            sourceNodeRid,
            sourceNodeGeneration);
        ZLinkActorBoundSessionHandoffFence? boundSession = null;
        if (sourceKind == 4)
        {
            var sourceActorId = sourceReader.Text8();
            var sourceActorGeneration = sourceReader.U64();
            var sourceSessionRid = TextRid(sourceReader.Text8());
            var bindingGeneration = sourceReader.U64();
            var sessionSequence = sourceReader.U64();
            boundSession = new ZLinkActorBoundSessionHandoffFence(
                sourceActorId,
                sourceActorGeneration,
                sourceSessionRid,
                string.Empty,
                bindingGeneration,
                sessionSequence);
        }
        sourceReader.End();
        ValidateDecodedSource(source);
        if (reader.Byte() != 0)
            throw new InvalidDataException("Actor frame metadata must be embedded in its exact header.");
        var operation = new MeshOperationId(reader.U64(), reader.U64());
        var operationKind = reader.U32();
        var replyReader = new Reader(reader.Bytes(reader.U16()));
        var replyRoute = kind == 10 ? replyReader.U64() : 0;
        replyReader.End();
        if (operationKind != (kind == 10 ? 4U : 0U))
            throw new InvalidDataException("The accepted Actor operation kind is invalid.");
        var actor = new ZLinkBackendActorRef(
            default,
            reader.Text8(),
            reader.U64());
        var targetNodeRid = TextRid(reader.Text8());
        var targetNodeGeneration = reader.U64();
        var authorityOwnerGeneration = reader.U64();
        var ownerLeaseGeneration = reader.U64();
        actor = actor with { NodeRid = targetNodeRid };
        if (reader.Byte() != 1)
            throw new InvalidDataException("The accepted Actor payload version is invalid.");
        var application = new Reader(reader.Bytes(checked((int)reader.U32())));
        _ = application.Text8();
        if (!string.Equals(application.Text8(), FrameContentType,
                StringComparison.Ordinal))
            throw new InvalidDataException("The accepted Actor payload type is invalid.");
        var payload = application.Bytes(checked((int)application.U32()));
        application.End();
        reader.End();
        var frame = DecodeFramePayload(payload, actor, source, boundSession,
            operation, replyRoute, targetNodeGeneration,
            authorityOwnerGeneration, ownerLeaseGeneration, arrivalIndex);
        return new ZLinkCanonicalActorAcceptedFrame(
            new ZLinkActorAcceptedRecord(frame, source, actor),
            actor,
            targetNodeRid);
    }

    private static byte[] EncodeFramePayload(ZLinkActorHandoffFrame frame)
    {
        using var stream = new MemoryStream();
        stream.WriteByte(frame.BoundSessionSource is null ? (byte)1 : (byte)2);
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
        ZLinkBackendActorRef actor,
        ZLinkServiceWireCodec.RequestSourceFence requestSource,
        ZLinkActorBoundSessionHandoffFence? boundSession,
        MeshOperationId operation,
        ulong replyRoute,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        long arrivalIndex)
    {
        var reader = new Reader(encoded);
        var payloadVersion = reader.Byte();
        if (payloadVersion is not (1 or 2)
            || (payloadVersion == 2) != (boundSession is not null))
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
        if (hop > 8 || ((flags & 1U) != 0) != (replyRoute != 0))
            throw new InvalidDataException("The accepted Actor route context is invalid.");
        return new ZLinkActorHandoffFrame(
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
                operation, hop, targetNodeGeneration,
                authorityOwnerGeneration, ownerLeaseGeneration,
                replyRoute, replyFlags, capability,
                IsBoundSessionRoute: boundSession is not null),
            requestSource.NodeGeneration,
            requestSource,
            replyRoute,
            encoded.Length,
            boundSession);
    }

    private static void ValidateSource(
        ZLinkServiceWireCodec.RequestSourceFence source)
    {
        if (source.NodeRid.Size == 0 || source.NodeGeneration == 0
            || string.IsNullOrWhiteSpace(source.OwnerId)
            || source.LeaseGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(source));
    }

    private static void ValidateDecodedSource(
        ZLinkServiceWireCodec.RequestSourceFence source)
    {
        try
        {
            ValidateSource(source);
        }
        catch (ArgumentOutOfRangeException error)
        {
            throw new InvalidDataException(
                "The accepted Actor request-source fence is invalid.",
                error);
        }
    }

    private static RoutingId TextRid(string value)
    {
        try { return RoutingId.FromHex(value); }
        catch (Exception error) when (error is ArgumentException or FormatException)
        { throw new InvalidDataException("The accepted Actor RID is invalid.", error); }
    }

    private static void OptionalText8(Stream stream, string? value)
    {
        stream.WriteByte(value is null ? (byte)0 : (byte)1);
        if (value is not null) Text8(stream, value);
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
        if (value.Length > byte.MaxValue) throw new InvalidDataException("bytes8 exceeds its bound.");
        stream.WriteByte(checked((byte)value.Length)); stream.Write(value);
    }

    private static void Bytes32(Stream stream, ReadOnlySpan<byte> value)
    { U32(stream, checked((uint)value.Length)); stream.Write(value); }

    private static void U16(Stream stream, ushort value)
    { Span<byte> bytes = stackalloc byte[2]; BinaryPrimitives.WriteUInt16BigEndian(bytes, value); stream.Write(bytes); }
    private static void U32(Stream stream, uint value)
    { Span<byte> bytes = stackalloc byte[4]; BinaryPrimitives.WriteUInt32BigEndian(bytes, value); stream.Write(bytes); }
    private static void U64(Stream stream, ulong value)
    { Span<byte> bytes = stackalloc byte[8]; BinaryPrimitives.WriteUInt64BigEndian(bytes, value); stream.Write(bytes); }

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
            try { return StrictUtf8.GetString(Bytes(Byte())); }
            catch (DecoderFallbackException error)
            { throw new InvalidDataException("Canonical text8 is invalid.", error); }
        }
        internal ReadOnlySpan<byte> Bytes(int count)
        {
            if (count < 0 || _offset > _bytes.Length - count) throw new EndOfStreamException();
            var result = _bytes.Slice(_offset, count); _offset += count; return result;
        }
        internal void End()
        { if (_offset != _bytes.Length) throw new InvalidDataException("Canonical record has trailing bytes."); }
    }
}
