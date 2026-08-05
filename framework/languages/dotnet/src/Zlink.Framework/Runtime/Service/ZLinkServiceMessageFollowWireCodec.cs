using System.Buffers.Binary;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Service;

internal static partial class ZLinkServiceWireCodec
{
    internal const byte MessageFollowActorKind = 1;
    internal const byte MessageFollowSpotKind = 2;
    internal const byte MessageFollowVersion = 1;
    internal const byte MessageFollowMaximumHopCount = 8;
    internal const uint MessageFollowMaximumQueuedMessages = 1024;
    internal const uint MessageFollowMaximumQueuedBytes = 16 * 1024 * 1024;

    /// <summary>
    /// A route fence carried by the infrastructure-only Message Follow record.
    /// ObjectKind is 1 for Actor and 2 for Spot; ObjectId is interpreted by
    /// that kind and is never dispatched as application data.
    /// </summary>
    internal readonly record struct MessageFollowRoute
    {
        internal MessageFollowRoute(
            byte objectKind,
            string objectId,
            ulong objectGeneration,
            RoutingId targetNodeRid,
            ulong targetNodeGeneration,
            ulong authorityOwnerGeneration,
            ulong ownerLeaseGeneration)
        {
            if (objectKind is not (MessageFollowActorKind
                or MessageFollowSpotKind)
                || string.IsNullOrWhiteSpace(objectId)
                || objectId.Contains('\0')
                || objectGeneration == 0
                || targetNodeRid.IsEmpty
                || targetNodeGeneration == 0
                || authorityOwnerGeneration == 0
                || ownerLeaseGeneration == 0)
                throw new ArgumentOutOfRangeException(nameof(objectId));
            ObjectKind = objectKind;
            ObjectId = objectId;
            ObjectGeneration = objectGeneration;
            TargetNodeRid = targetNodeRid;
            TargetNodeGeneration = targetNodeGeneration;
            AuthorityOwnerGeneration = authorityOwnerGeneration;
            OwnerLeaseGeneration = ownerLeaseGeneration;
        }

        internal byte ObjectKind { get; }
        internal string ObjectId { get; }
        internal ulong ObjectGeneration { get; }
        internal RoutingId TargetNodeRid { get; }
        internal ulong TargetNodeGeneration { get; }
        internal ulong AuthorityOwnerGeneration { get; }
        internal ulong OwnerLeaseGeneration { get; }
        internal bool IsActor => ObjectKind == MessageFollowActorKind;
    }

    /// <summary>
    /// The closed command-50 route notification. It contains no application
    /// payload and is consumed before the application dispatch path.
    /// </summary>
    internal readonly record struct MessageFollowRecord
    {
        internal MessageFollowRecord(
            MessageFollowRoute source,
            MessageFollowRoute target,
            byte hopCount,
            uint queuedMessages,
            uint queuedBytes,
            MeshOperationId originalOperation,
            ulong originalReplyRouteId)
        {
            if (source.ObjectKind != target.ObjectKind
                || !string.Equals(
                    source.ObjectId,
                    target.ObjectId,
                    StringComparison.Ordinal)
                || source.ObjectGeneration != target.ObjectGeneration
                || hopCount is 0 or > MessageFollowMaximumHopCount
                || queuedMessages > MessageFollowMaximumQueuedMessages
                || queuedBytes > MessageFollowMaximumQueuedBytes
                || originalOperation == default)
                throw new ArgumentOutOfRangeException(nameof(source));
            Source = source;
            Target = target;
            HopCount = hopCount;
            QueuedMessages = queuedMessages;
            QueuedBytes = queuedBytes;
            OriginalOperation = originalOperation;
            OriginalReplyRouteId = originalReplyRouteId;
        }

        internal MessageFollowRoute Source { get; }
        internal MessageFollowRoute Target { get; }
        internal byte HopCount { get; }
        internal uint QueuedMessages { get; }
        internal uint QueuedBytes { get; }
        internal MeshOperationId OriginalOperation { get; }
        internal ulong OriginalReplyRouteId { get; }
    }

    internal static byte[] EncodeMessageFollow(MessageFollowRecord record)
    {
        var body = new WireWriter();
        WriteMessageFollowRoute(body, record.Source);
        WriteMessageFollowRoute(body, record.Target);
        body.U8(record.HopCount);
        body.U32(record.QueuedMessages);
        body.U32(record.QueuedBytes);
        WriteOperationId(body, record.OriginalOperation);
        body.U64(record.OriginalReplyRouteId);

        if (body.Count > MessageFollowMaximumQueuedBytes)
            throw new ArgumentOutOfRangeException(nameof(record));

        var bytes = Prefix(
            ServiceWireConstants.Command.MessageFollow,
            ServiceWireConstants.Flag.None,
            checked(sizeof(byte) + sizeof(uint) + body.Count));
        bytes[5] = MessageFollowVersion;
        BinaryPrimitives.WriteUInt32BigEndian(
            bytes.AsSpan(6), checked((uint)body.Count));
        body.CopyTo(bytes.AsSpan(10));
        return bytes;
    }

    internal static bool TryDecodeMessageFollow(
        ReadOnlySpan<byte> bytes,
        out MessageFollowRecord record,
        out DecodeError error)
    {
        record = default;
        try
        {
            if (!TryDecodePrefix(bytes, out var command, out var flags, out error))
                return false;
            if (command != ServiceWireConstants.Command.MessageFollow)
            {
                error = DecodeError.UnknownCommand;
                return false;
            }
            if (flags != ServiceWireConstants.Flag.None)
            {
                error = DecodeError.ForbiddenFlag;
                return false;
            }

            var reader = new WireReader(bytes[5..]);
            if (!reader.TryU8(out var version))
                return DecodeMessageFollowFailure(ref reader, out error);
            if (version != MessageFollowVersion)
            {
                error = DecodeError.UnsupportedVersion;
                return false;
            }
            if (!reader.TryU32(out var bodyLength)
                || bodyLength > MessageFollowMaximumQueuedBytes
                || bodyLength != (uint)reader.Remaining
                || bodyLength > int.MaxValue
                || !reader.TrySlice((int)bodyLength, out var encodedBody))
                return DecodeMessageFollowFailure(ref reader, out error);

            var body = new WireReader(encodedBody);
            if (!TryReadMessageFollowRoute(
                    ref body,
                    out var source)
                || !TryReadMessageFollowRoute(
                    ref body,
                    out var target)
                || !body.TryU8(out var hopCount)
                || !body.TryU32(out var queuedMessages)
                || !body.TryU32(out var queuedBytes)
                || !TryReadOperationId(ref body, out var operation)
                || !body.TryU64(out var replyRouteId)
                || body.Remaining != 0)
                return DecodeMessageFollowFailure(ref body, out error);

            record = new MessageFollowRecord(
                source,
                target,
                hopCount,
                queuedMessages,
                queuedBytes,
                operation,
                replyRouteId);
            error = DecodeError.None;
            return true;
        }
        catch (Exception exception)
            when (exception is ArgumentException or FormatException)
        {
            record = default;
            error = DecodeError.InvalidField;
            return false;
        }
    }

    private static void WriteMessageFollowRoute(
        WireWriter writer,
        MessageFollowRoute route)
    {
        var body = new WireWriter();
        body.Text8(route.ObjectId);
        body.U64(route.ObjectGeneration);
        body.Rid(route.TargetNodeRid);
        body.U64(route.TargetNodeGeneration);
        body.U64(route.AuthorityOwnerGeneration);
        body.U64(route.OwnerLeaseGeneration);
        if (body.Count > ushort.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(route));
        writer.U8(route.ObjectKind);
        writer.U16(checked((ushort)body.Count));
        writer.Bytes(body.ToArray());
    }

    private static bool TryReadMessageFollowRoute(
        ref WireReader reader,
        out MessageFollowRoute route)
    {
        route = default;
        if (!reader.TryU8(out var objectKind)
            || !reader.TryU16(out var bodyLength)
            || !reader.TrySlice(bodyLength, out var encodedBody))
            return false;

        var body = new WireReader(encodedBody);
        if (!body.TryText8(out var objectId)
            || !body.TryU64(out var objectGeneration)
            || !body.TryRid(out var targetNodeRid)
            || !body.TryU64(out var targetNodeGeneration)
            || !body.TryU64(out var authorityOwnerGeneration)
            || !body.TryU64(out var ownerLeaseGeneration)
            || body.Remaining != 0)
            return false;

        route = new MessageFollowRoute(
            objectKind,
            objectId,
            objectGeneration,
            targetNodeRid,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);
        return true;
    }

    private static bool DecodeMessageFollowFailure(
        ref WireReader reader,
        out DecodeError error)
    {
        error = reader.Truncated
            ? DecodeError.TruncatedField
            : DecodeError.InvalidField;
        return false;
    }
}
