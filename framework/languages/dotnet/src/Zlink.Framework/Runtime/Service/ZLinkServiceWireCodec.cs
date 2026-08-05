using System.Buffers.Binary;
using System.Text;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Service;

internal static partial class ZLinkServiceWireCodec
{
    internal enum DecodeError
    {
        None = 0,
        InvalidMagic,
        UnsupportedVersion,
        UnknownCommand,
        ForbiddenFlag,
        InvalidField,
        TruncatedField,
        TrailingByte
    }

    internal readonly record struct LivenessRecord(
        ServiceWireConstants.Command Command,
        ulong ProbeId);

    internal readonly record struct ApplicationRecord(
        ServiceWireConstants.Command Command,
        ulong Correlation,
        string? ChannelName,
        bool HasMetadata);

    internal readonly record struct LogicalMulticastRecord(
        string ChannelName,
        string Topic,
        string SourceSpotId,
        bool HasMetadata);

    internal readonly record struct ReplyRecord(
        ulong Correlation,
        int TerminalResult,
        uint FailureCode,
        byte[] Tail);

    internal readonly record struct StatefulRecord(
        ServiceWireConstants.Command Command,
        ulong Correlation,
        MeshOperationId OperationId,
        string SourceSpotId,
        string TargetSpotId,
        ulong TargetSpotGeneration,
        ActorRef TargetActor,
        RoutingId TargetNodeRid,
        ulong TargetNodeGeneration,
        ulong AuthorityOwnerGeneration,
        ulong OwnerLeaseGeneration,
        byte MessageFollowHopCount,
        ulong DeadlineUnixMs,
        bool HasMetadata);

    internal readonly record struct UserSpotOperationRecord(
        ServiceWireConstants.Command Command,
        UserSpotCreateOperation Create,
        UserSpotCloseOperation Close);

    internal readonly record struct ActorCreateOperationRecord(
        ActorCreateOperation Operation);

    internal readonly record struct ActorDestroyOperationRecord(
        ActorDestroyOperation Operation);

    internal readonly record struct InstanceSpotActivationRecord(
        InstanceSpotActivationOperation Operation,
        bool HasMetadata);

    internal readonly record struct AdmissionRecord(
        string MeshName,
        string SecurityIdentity,
        uint EffectiveMaxMessageBytes,
        string AdvertisedEndpoint,
        ulong LifecycleGeneration,
        ulong DescriptorRevision,
        IReadOnlyDictionary<string, uint> Channels,
        byte RuntimeState,
        long ApplicationVersion,
        byte ObjectRole,
        uint PlacementWeight,
        uint ActiveCapacityLimit,
        uint PendingCapacityLimit,
        uint ActiveCapacityUsed,
        uint PendingCapacityUsed,
        IReadOnlyDictionary<byte, byte[]> ExtensionFields,
        byte[] DescriptorBytes);

    internal static byte[] EncodeLiveness(
        ServiceWireConstants.Command command,
        ulong probeId)
    {
        if (command is not (ServiceWireConstants.Command.LivenessProbe
            or ServiceWireConstants.Command.LivenessAck))
            throw new ArgumentOutOfRangeException(nameof(command));
        if (probeId == 0)
            throw new ArgumentOutOfRangeException(nameof(probeId));

        var bytes = Prefix(command, ServiceWireConstants.Flag.None, sizeof(ulong));
        BinaryPrimitives.WriteUInt64BigEndian(bytes.AsSpan(5), probeId);
        return bytes;
    }

    internal static bool TryDecodeLiveness(
        ReadOnlySpan<byte> bytes,
        out LivenessRecord record,
        out DecodeError error)
    {
        record = default;
        if (!TryDecodePrefix(bytes, out var command, out var flags, out error))
            return false;
        if (command is not (ServiceWireConstants.Command.LivenessProbe
            or ServiceWireConstants.Command.LivenessAck))
        {
            error = DecodeError.UnknownCommand;
            return false;
        }
        if (flags != ServiceWireConstants.Flag.None)
        {
            error = DecodeError.ForbiddenFlag;
            return false;
        }
        if (bytes.Length < 13)
        {
            error = DecodeError.TruncatedField;
            return false;
        }
        if (bytes.Length > 13)
        {
            error = DecodeError.TrailingByte;
            return false;
        }

        var probeId = BinaryPrimitives.ReadUInt64BigEndian(bytes[5..]);
        if (probeId == 0)
        {
            error = DecodeError.InvalidField;
            return false;
        }

        record = new LivenessRecord(command, probeId);
        error = DecodeError.None;
        return true;
    }

    internal static byte[] EncodeApplication(
        ServiceWireConstants.Command command,
        ulong correlation,
        string? channelName,
        bool hasMetadata)
    {
        var request = command is ServiceWireConstants.Command.NodeRequest
            or ServiceWireConstants.Command.ChannelRequest;
        var channel = command is ServiceWireConstants.Command.ChannelSend
            or ServiceWireConstants.Command.ChannelRequest;
        if (command is not (ServiceWireConstants.Command.NodeSend
            or ServiceWireConstants.Command.NodeRequest
            or ServiceWireConstants.Command.ChannelSend
            or ServiceWireConstants.Command.ChannelRequest))
            throw new ArgumentOutOfRangeException(nameof(command));
        if (request != (correlation != 0))
            throw new ArgumentOutOfRangeException(nameof(correlation));

        var encodedChannel = channel
            ? EncodeText(channelName
                ?? throw new ArgumentNullException(nameof(channelName)))
            : Array.Empty<byte>();
        var bodyLength = (request ? sizeof(ulong) : 0) + encodedChannel.Length;
        var flags = hasMetadata
            ? ServiceWireConstants.Flag.Metadata
            : ServiceWireConstants.Flag.None;
        var bytes = Prefix(command, flags, bodyLength);
        var offset = 5;
        if (request)
        {
            BinaryPrimitives.WriteUInt64BigEndian(bytes.AsSpan(offset), correlation);
            offset += sizeof(ulong);
        }
        encodedChannel.CopyTo(bytes, offset);
        return bytes;
    }

    internal static bool TryDecodeApplication(
        ReadOnlySpan<byte> bytes,
        out ApplicationRecord record,
        out DecodeError error)
    {
        record = default;
        if (!TryDecodePrefix(bytes, out var command, out var flags, out error))
            return false;
        if (command is not (ServiceWireConstants.Command.NodeSend
            or ServiceWireConstants.Command.NodeRequest
            or ServiceWireConstants.Command.ChannelSend
            or ServiceWireConstants.Command.ChannelRequest))
        {
            error = DecodeError.UnknownCommand;
            return false;
        }
        if ((flags & ~ServiceWireConstants.Flag.Metadata) != 0)
        {
            error = DecodeError.ForbiddenFlag;
            return false;
        }

        var offset = 5;
        var request = command is ServiceWireConstants.Command.NodeRequest
            or ServiceWireConstants.Command.ChannelRequest;
        ulong correlation = 0;
        if (request)
        {
            if (bytes.Length - offset < sizeof(ulong))
            {
                error = DecodeError.TruncatedField;
                return false;
            }
            correlation = BinaryPrimitives.ReadUInt64BigEndian(bytes[offset..]);
            offset += sizeof(ulong);
            if (correlation == 0)
            {
                error = DecodeError.InvalidField;
                return false;
            }
        }

        string? channelName = null;
        if (command is ServiceWireConstants.Command.ChannelSend
            or ServiceWireConstants.Command.ChannelRequest)
        {
            if (!TryDecodeText(bytes[offset..], out channelName, out var consumed, out error))
                return false;
            offset += consumed;
        }

        if (offset != bytes.Length)
        {
            error = DecodeError.TrailingByte;
            return false;
        }

        record = new ApplicationRecord(
            command,
            correlation,
            channelName,
            (flags & ServiceWireConstants.Flag.Metadata) != 0);
        error = DecodeError.None;
        return true;
    }

    internal static byte[] EncodeLogicalMulticast(
        string channelName,
        string topic,
        string sourceSpotId,
        bool hasMetadata)
    {
        var encodedChannel = EncodeText(channelName);
        var encodedTopic = EncodeText(topic);
        var encodedSourceSpot = EncodeText(sourceSpotId);
        var bodyLength = encodedChannel.Length
                         + encodedTopic.Length
                         + encodedSourceSpot.Length;
        var flags = hasMetadata
            ? ServiceWireConstants.Flag.Metadata
            : ServiceWireConstants.Flag.None;
        var bytes = Prefix(
            ServiceWireConstants.Command.LogicalMulticast,
            flags,
            bodyLength);
        var offset = 5;
        encodedChannel.CopyTo(bytes, offset);
        offset += encodedChannel.Length;
        encodedTopic.CopyTo(bytes, offset);
        offset += encodedTopic.Length;
        encodedSourceSpot.CopyTo(bytes, offset);
        return bytes;
    }

    internal static bool TryDecodeLogicalMulticast(
        ReadOnlySpan<byte> bytes,
        out LogicalMulticastRecord record,
        out DecodeError error)
    {
        record = default;
        if (!TryDecodePrefix(bytes, out var command, out var flags, out error))
            return false;
        if (command != ServiceWireConstants.Command.LogicalMulticast)
        {
            error = DecodeError.UnknownCommand;
            return false;
        }
        if ((flags & ~ServiceWireConstants.Flag.Metadata) != 0)
        {
            error = DecodeError.ForbiddenFlag;
            return false;
        }

        var offset = 5;
        if (!TryDecodeText(bytes[offset..], out var channelName, out var consumed, out error))
            return false;
        offset += consumed;
        if (!TryDecodeText(bytes[offset..], out var topic, out consumed, out error))
            return false;
        offset += consumed;
        if (!TryDecodeText(bytes[offset..], out var sourceSpotId, out consumed, out error))
            return false;
        offset += consumed;
        if (offset != bytes.Length)
        {
            error = DecodeError.TrailingByte;
            return false;
        }

        record = new LogicalMulticastRecord(
            channelName,
            topic,
            sourceSpotId,
            (flags & ServiceWireConstants.Flag.Metadata) != 0);
        error = DecodeError.None;
        return true;
    }

    internal static byte[] EncodeReply(
        ulong correlation,
        int terminalResult,
        uint failureCode,
        ReadOnlySpan<byte> tail = default)
    {
        if (correlation == 0)
            throw new ArgumentOutOfRangeException(nameof(correlation));
        if (tail.Length > ushort.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(tail));

        var bytes = Prefix(
            ServiceWireConstants.Command.Reply,
            ServiceWireConstants.Flag.None,
            sizeof(ulong) + sizeof(uint) + sizeof(uint) + sizeof(ushort) + tail.Length);
        var span = bytes.AsSpan(5);
        BinaryPrimitives.WriteUInt64BigEndian(span, correlation);
        BinaryPrimitives.WriteInt32BigEndian(span[8..], terminalResult);
        BinaryPrimitives.WriteUInt32BigEndian(span[12..], failureCode);
        BinaryPrimitives.WriteUInt16BigEndian(span[16..], checked((ushort)tail.Length));
        tail.CopyTo(span[18..]);
        return bytes;
    }

    internal static bool TryDecodeReply(
        ReadOnlySpan<byte> bytes,
        out ReplyRecord record,
        out DecodeError error)
    {
        record = default;
        if (!TryDecodePrefix(bytes, out var command, out var flags, out error))
            return false;
        if (command != ServiceWireConstants.Command.Reply)
        {
            error = DecodeError.UnknownCommand;
            return false;
        }
        if (flags != ServiceWireConstants.Flag.None)
        {
            error = DecodeError.ForbiddenFlag;
            return false;
        }
        if (bytes.Length < 23)
        {
            error = DecodeError.TruncatedField;
            return false;
        }

        var span = bytes[5..];
        var correlation = BinaryPrimitives.ReadUInt64BigEndian(span);
        var terminalResult = BinaryPrimitives.ReadInt32BigEndian(span[8..]);
        var failureCode = BinaryPrimitives.ReadUInt32BigEndian(span[12..]);
        var tailLength = BinaryPrimitives.ReadUInt16BigEndian(span[16..]);
        if (correlation == 0)
        {
            error = DecodeError.InvalidField;
            return false;
        }
        if (span.Length - 18 < tailLength)
        {
            error = DecodeError.TruncatedField;
            return false;
        }
        if (span.Length - 18 > tailLength)
        {
            error = DecodeError.TrailingByte;
            return false;
        }

        record = new ReplyRecord(
            correlation,
            terminalResult,
            failureCode,
            span.Slice(18, tailLength).ToArray());
        error = DecodeError.None;
        return true;
    }

    internal static byte[] EncodeActorDestroyReply(
        ulong correlation,
        RequestResult terminalResult,
        ServiceWireConstants.FrameworkErrorCode failureCode,
        ActorDestroyCompletion? completion)
    {
        if (terminalResult != RequestResult.Ok)
            return EncodeReply(correlation, (int)terminalResult, (uint)failureCode);
        if (failureCode != ServiceWireConstants.FrameworkErrorCode.None
            || completion is null)
            throw new ArgumentOutOfRangeException(nameof(completion));
        return EncodeReply(
            correlation,
            (int)terminalResult,
            (uint)failureCode,
            [completion.Destroyed ? (byte)1 : (byte)0]);
    }

    internal static bool TryDecodeActorDestroyReply(
        ReplyRecord reply,
        out ActorDestroyCompletion? completion,
        out DecodeError error)
    {
        completion = null;
        if (reply.TerminalResult != (int)RequestResult.Ok)
        {
            // Transport boundary terminals such as cancellation and shutdown
            // intentionally have no framework error code. Typed domain
            // failures carry one, but neither form may carry a success tail.
            if (reply.Tail.Length != 0)
            {
                error = DecodeError.InvalidField;
                return false;
            }
            error = DecodeError.None;
            return true;
        }
        if (reply.FailureCode != 0
            || reply.Tail.Length != 1
            || reply.Tail[0] > 1)
        {
            error = DecodeError.InvalidField;
            return false;
        }
        completion = new ActorDestroyCompletion(reply.Tail[0] == 1);
        error = DecodeError.None;
        return true;
    }

    internal static byte[] EncodeUserSpotCreateReply(
        ulong correlation,
        RequestResult terminalResult,
        ServiceWireConstants.FrameworkErrorCode failureCode,
        UserSpotCreateCompletion? completion)
    {
        if (terminalResult != RequestResult.Ok)
        {
            if (completion is not null)
                throw new ArgumentException(
                    "A failed User Spot create reply cannot carry a success tail.",
                    nameof(completion));
            return EncodeReply(correlation, (int)terminalResult, (uint)failureCode);
        }
        if (failureCode != ServiceWireConstants.FrameworkErrorCode.None
            || completion is null
            || completion.ObjectGeneration == 0
            || !ZLinkSpotId.IsValid(completion.SpotId))
            throw new ArgumentOutOfRangeException(nameof(completion));

        var tail = new WireWriter();
        tail.U8((byte)completion.Result);
        tail.Text8(completion.SpotId);
        tail.U64(completion.ObjectGeneration);
        return EncodeReply(
            correlation,
            (int)terminalResult,
            (uint)failureCode,
            tail.ToArray());
    }

    internal static byte[] EncodeUserSpotCloseReply(
        ulong correlation,
        RequestResult terminalResult,
        ServiceWireConstants.FrameworkErrorCode failureCode,
        UserSpotCloseCompletion? completion)
    {
        if (terminalResult != RequestResult.Ok)
        {
            if (completion is not null)
                throw new ArgumentException(
                    "A failed User Spot close reply cannot carry a success tail.",
                    nameof(completion));
            return EncodeReply(correlation, (int)terminalResult, (uint)failureCode);
        }
        if (failureCode != ServiceWireConstants.FrameworkErrorCode.None
            || completion is null)
            throw new ArgumentOutOfRangeException(nameof(completion));

        return EncodeReply(
            correlation,
            (int)terminalResult,
            (uint)failureCode,
            [completion.Closed ? (byte)1 : (byte)0]);
    }

    internal static byte[] EncodeActorCreateReply(
        ulong correlation,
        RequestResult terminalResult,
        ServiceWireConstants.FrameworkErrorCode failureCode,
        ActorCreateCompletion? completion)
    {
        if (terminalResult != RequestResult.Ok)
        {
            if (completion is not null)
                throw new ArgumentException(
                    "A failed Actor create reply cannot carry a success tail.",
                    nameof(completion));
            return EncodeReply(correlation, (int)terminalResult, (uint)failureCode);
        }
        if (failureCode != ServiceWireConstants.FrameworkErrorCode.None
            || completion is null
            || completion.Result is < ActorCreateResult.Existing
                or > ActorCreateResult.Rejected)
            throw new ArgumentOutOfRangeException(nameof(completion));

        var selected = new WireWriter();
        if (completion.Result is ActorCreateResult.Existing
            or ActorCreateResult.Created)
        {
            if (string.IsNullOrEmpty(completion.Actor.ActorId)
                || completion.Actor.ObjectGeneration == 0
                || string.IsNullOrEmpty(completion.Actor.MeshName)
                || completion.Actor.NodeRid.IsEmpty)
                throw new ArgumentOutOfRangeException(nameof(completion));
            selected.Rid(completion.Actor.NodeRid);
            selected.Text8(completion.Actor.ActorId);
            selected.U64(completion.Actor.ObjectGeneration);
        }
        else if (!string.IsNullOrEmpty(completion.Actor.ActorId)
                 || completion.Actor.ObjectGeneration != 0
                 || !string.IsNullOrEmpty(completion.Actor.MeshName)
                 || !completion.Actor.NodeRid.IsEmpty)
        {
            throw new ArgumentOutOfRangeException(nameof(completion));
        }

        var tail = new WireWriter();
        tail.U8((byte)completion.Result);
        tail.U16(checked((ushort)selected.Count));
        tail.Bytes(selected.ToArray());
        return EncodeReply(
            correlation,
            (int)terminalResult,
            (uint)failureCode,
            tail.ToArray());
    }

    internal static bool TryDecodeActorCreateReply(
        ReplyRecord reply,
        string meshName,
        out ActorCreateCompletion? completion,
        out DecodeError error)
    {
        completion = null;
        if (reply.TerminalResult != (int)RequestResult.Ok)
        {
            if (reply.Tail.Length != 0)
            {
                error = DecodeError.InvalidField;
                return false;
            }
            error = DecodeError.None;
            return true;
        }
        if (reply.FailureCode != 0)
        {
            error = DecodeError.InvalidField;
            return false;
        }

        var reader = new WireReader(reply.Tail);
        if (!reader.TryU8(out var encodedResult)
            || encodedResult is < (byte)ActorCreateResult.Existing
                or > (byte)ActorCreateResult.Rejected
            || !reader.TryU16(out var selectedLength)
            || !reader.TrySlice(selectedLength, out var selectedBytes)
            || reader.Remaining != 0)
        {
            error = reader.Truncated
                ? DecodeError.TruncatedField
                : reader.Remaining != 0
                    ? DecodeError.TrailingByte
                    : DecodeError.InvalidField;
            return false;
        }

        var result = (ActorCreateResult)encodedResult;
        var selected = new WireReader(selectedBytes);
        ActorRef actor = default;
        if (result is ActorCreateResult.Existing or ActorCreateResult.Created)
        {
            if (!selected.TryRid(out var nodeRid)
                || nodeRid.IsEmpty
                || !selected.TryText8(out var actorId)
                || !selected.TryU64(out var objectGeneration)
                || objectGeneration == 0
                || selected.Remaining != 0)
            {
                error = selected.Truncated
                    ? DecodeError.TruncatedField
                    : selected.Remaining != 0
                        ? DecodeError.TrailingByte
                        : DecodeError.InvalidField;
                return false;
            }
            actor = new ActorRef(
                actorId,
                objectGeneration,
                meshName,
                nodeRid);
        }
        else if (selected.Remaining != 0)
        {
            error = DecodeError.InvalidField;
            return false;
        }

        completion = new ActorCreateCompletion(result, actor);
        error = DecodeError.None;
        return true;
    }

    internal static bool TryDecodeUserSpotReply(
        ReplyRecord reply,
        MeshOperationKind operationKind,
        out MeshRecordPayload? completion,
        out DecodeError error)
    {
        completion = null;
        if (operationKind is not (MeshOperationKind.UserSpotCreate
            or MeshOperationKind.UserSpotClose))
        {
            if (reply.Tail.Length != 0)
            {
                error = DecodeError.InvalidField;
                return false;
            }
            error = DecodeError.None;
            return true;
        }

        if (reply.TerminalResult != (int)RequestResult.Ok)
        {
            if (reply.Tail.Length != 0)
            {
                error = DecodeError.InvalidField;
                return false;
            }
            error = DecodeError.None;
            return true;
        }
        if (reply.FailureCode != 0)
        {
            error = DecodeError.InvalidField;
            return false;
        }

        var reader = new WireReader(reply.Tail);
        if (operationKind == MeshOperationKind.UserSpotCreate)
        {
            if (!reader.TryU8(out var result)
                || result is < (byte)UserSpotCreateResult.Existing
                    or > (byte)UserSpotCreateResult.Rejected
                || !reader.TryText8(out var spotId)
                || !reader.TryU64(out var objectGeneration)
                || objectGeneration == 0
                || reader.Remaining != 0)
            {
                error = reader.Truncated
                    ? DecodeError.TruncatedField
                    : reader.Remaining != 0
                        ? DecodeError.TrailingByte
                        : DecodeError.InvalidField;
                return false;
            }
            completion = new UserSpotCreateCompletion(
                (UserSpotCreateResult)result,
                spotId,
                objectGeneration);
        }
        else
        {
            if (!reader.TryU8(out var closed)
                || closed > 1
                || reader.Remaining != 0)
            {
                error = reader.Truncated
                    ? DecodeError.TruncatedField
                    : reader.Remaining != 0
                        ? DecodeError.TrailingByte
                        : DecodeError.InvalidField;
                return false;
            }
            completion = new UserSpotCloseCompletion(closed == 1);
        }

        error = DecodeError.None;
        return true;
    }

    internal static byte[] EncodeSpot(
        ServiceWireConstants.Command command,
        ulong correlation,
        MeshOperationId operationId,
        string sourceSpotId,
        string targetSpotId,
        ulong targetSpotGeneration,
        RoutingId targetNodeRid,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        bool hasMetadata,
        byte messageFollowHopCount = 0,
        ulong deadlineUnixMs = 0)
    {
        var request = command == ServiceWireConstants.Command.SpotRequest;
        if (command is not (ServiceWireConstants.Command.SpotSend
            or ServiceWireConstants.Command.SpotRequest)
            || request != (correlation != 0)
            || operationId.High == 0
            || operationId.Low == 0
            || targetSpotGeneration == 0
            || targetNodeGeneration == 0
            || authorityOwnerGeneration == 0
            || ownerLeaseGeneration == 0
            || (request && (deadlineUnixMs is 0 or > long.MaxValue))
            || (!request && deadlineUnixMs != 0)
            || messageFollowHopCount > 8)
            throw new ArgumentOutOfRangeException(nameof(command));
        var body = new WireWriter();
        if (request)
        {
            body.U64(correlation);
            body.U64(deadlineUnixMs);
        }
        body.U64(operationId.High);
        body.U64(operationId.Low);
        body.U8(messageFollowHopCount);
        body.Text8(sourceSpotId);
        body.Text8(targetSpotId);
        body.U64(targetSpotGeneration);
        body.Rid(targetNodeRid);
        body.U64(targetNodeGeneration);
        body.U64(authorityOwnerGeneration);
        body.U64(ownerLeaseGeneration);
        var result = Prefix(
            command,
            hasMetadata ? ServiceWireConstants.Flag.Metadata : ServiceWireConstants.Flag.None,
            body.Count);
        body.CopyTo(result.AsSpan(5));
        return result;
    }

    internal static long MeasureSpotMessageFollowEncodedBytes(
        bool request,
        MeshOperationId operationId,
        string sourceSpotId,
        string targetSpotId,
        ulong targetSpotGeneration,
        RoutingId targetNodeRid,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        byte messageFollowHopCount,
        IReadOnlyList<Message> parts,
        ReadOnlyMemory<byte> metadata,
        ulong deadlineUnixMs = 1)
    {
        ArgumentNullException.ThrowIfNull(parts);
        var head = EncodeSpot(
            request
                ? ServiceWireConstants.Command.SpotRequest
                : ServiceWireConstants.Command.SpotSend,
            request ? 1UL : 0,
            operationId,
            sourceSpotId,
            targetSpotId,
            targetSpotGeneration,
            targetNodeRid,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration,
            !metadata.IsEmpty,
            messageFollowHopCount,
            request ? deadlineUnixMs : 0);
        var bytes = checked((long)head.LongLength + metadata.Length);
        foreach (var part in parts)
            bytes = checked(bytes + part.Size);
        return bytes;
    }

    internal static byte[] EncodeActor(
        ServiceWireConstants.Command command,
        ulong correlation,
        MeshOperationId operationId,
        ActorRef targetActor,
        RoutingId targetNodeRid,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        bool hasMetadata,
        byte messageFollowHopCount = 0,
        ulong deadlineUnixMs = 0)
    {
        var request = command == ServiceWireConstants.Command.ActorRequest;
        if (command is not (ServiceWireConstants.Command.ActorSend
            or ServiceWireConstants.Command.ActorRequest)
            || request != (correlation != 0)
            || operationId.High == 0
            || operationId.Low == 0
            || targetActor.ObjectGeneration == 0
            || string.IsNullOrEmpty(targetActor.MeshName)
            || targetActor.NodeRid != targetNodeRid
            || targetNodeGeneration == 0
            || authorityOwnerGeneration == 0
            || ownerLeaseGeneration == 0
            || messageFollowHopCount > 8
            || request != (deadlineUnixMs != 0)
            || deadlineUnixMs > long.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(command));
        var body = new WireWriter();
        if (request)
        {
            body.U64(correlation);
            body.U64(deadlineUnixMs);
        }
        body.U64(operationId.High);
        body.U64(operationId.Low);
        body.U8(messageFollowHopCount);
        body.U8(0); // optional source Actor: absent
        body.U16(0);
        body.Text8(targetActor.ActorId);
        body.U64(targetActor.ObjectGeneration);
        body.Rid(targetNodeRid);
        body.U64(targetNodeGeneration);
        body.U64(authorityOwnerGeneration);
        body.U64(ownerLeaseGeneration);
        var result = Prefix(
            command,
            hasMetadata ? ServiceWireConstants.Flag.Metadata : ServiceWireConstants.Flag.None,
            body.Count);
        body.CopyTo(result.AsSpan(5));
        return result;
    }

    internal static byte[] EncodeActorDestroy(ActorDestroyOperation operation)
    {
        if (operation.Correlation == 0
            || operation.Actor.ObjectGeneration == 0
            || string.IsNullOrEmpty(operation.Actor.MeshName)
            || operation.TargetNodeRid.IsEmpty
            || operation.Actor.NodeRid != operation.TargetNodeRid
            || operation.TargetNodeGeneration == 0
            || operation.AuthorityOwnerGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(operation));
        var body = new WireWriter();
        body.U64(operation.Correlation);
        body.Text8(operation.Actor.ActorId);
        body.U64(operation.Actor.ObjectGeneration);
        body.Rid(operation.TargetNodeRid);
        body.U64(operation.TargetNodeGeneration);
        body.U64(operation.AuthorityOwnerGeneration);
        var result = Prefix(
            ServiceWireConstants.Command.ActorDestroy,
            ServiceWireConstants.Flag.None,
            body.Count);
        body.CopyTo(result.AsSpan(5));
        return result;
    }

    internal static bool TryDecodeActorDestroy(
        ReadOnlySpan<byte> bytes,
        string meshName,
        out ActorDestroyOperationRecord record,
        out DecodeError error)
    {
        record = default;
        if (!TryDecodePrefix(bytes, out var command, out var flags, out error))
            return false;
        if (command != ServiceWireConstants.Command.ActorDestroy)
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
        if (!reader.TryU64(out var correlation)
            || correlation == 0
            || !reader.TryText8(out var actorId)
            || !reader.TryU64(out var objectGeneration)
            || objectGeneration == 0
            || !reader.TryRid(out var targetNodeRid)
            || !reader.TryU64(out var targetNodeGeneration)
            || targetNodeGeneration == 0
            || !reader.TryU64(out var authorityOwnerGeneration)
            || authorityOwnerGeneration == 0
            || reader.Remaining != 0)
        {
            error = reader.Truncated
                ? DecodeError.TruncatedField
                : reader.Remaining != 0
                    ? DecodeError.TrailingByte
                    : DecodeError.InvalidField;
            return false;
        }
        record = new ActorDestroyOperationRecord(
            new ActorDestroyOperation(
                correlation,
                new ActorRef(
                    actorId,
                    objectGeneration,
                    meshName,
                    targetNodeRid),
                targetNodeRid,
                targetNodeGeneration,
                authorityOwnerGeneration));
        error = DecodeError.None;
        return true;
    }

    internal static bool TryDecodeStateful(
        ReadOnlySpan<byte> bytes,
        string meshName,
        out StatefulRecord record,
        out DecodeError error)
    {
        record = default;
        if (!TryDecodePrefix(bytes, out var command, out var flags, out error))
            return false;
        if (command is not (ServiceWireConstants.Command.SpotSend
            or ServiceWireConstants.Command.SpotRequest
            or ServiceWireConstants.Command.ActorSend
            or ServiceWireConstants.Command.ActorRequest))
        {
            error = DecodeError.UnknownCommand;
            return false;
        }
        if ((flags & ~ServiceWireConstants.Flag.Metadata) != 0)
        {
            error = DecodeError.ForbiddenFlag;
            return false;
        }

        var reader = new WireReader(bytes[5..]);
        var request = command is ServiceWireConstants.Command.SpotRequest
            or ServiceWireConstants.Command.ActorRequest;
        ulong correlation = 0;
        ulong deadlineUnixMs = 0;
        if (request
            && (!reader.TryU64(out correlation) || correlation == 0))
        {
            error = reader.Truncated ? DecodeError.TruncatedField : DecodeError.InvalidField;
            return false;
        }
        if (request
            && (!reader.TryU64(out deadlineUnixMs)
                || deadlineUnixMs is 0 or > long.MaxValue))
        {
            error = reader.Truncated
                ? DecodeError.TruncatedField
                : DecodeError.InvalidField;
            return false;
        }
        if (!reader.TryU64(out var operationHigh)
            || operationHigh == 0
            || !reader.TryU64(out var operationLow)
            || operationLow == 0)
        {
            error = reader.Truncated ? DecodeError.TruncatedField : DecodeError.InvalidField;
            return false;
        }
        if (!reader.TryU8(out var messageFollowHopCount)
            || messageFollowHopCount > 8)
        {
            error = reader.Truncated
                ? DecodeError.TruncatedField
                : DecodeError.InvalidField;
            return false;
        }

        string sourceSpotId = string.Empty;
        string targetSpotId = string.Empty;
        ulong targetSpotGeneration = 0;
        ActorRef targetActor = default;
        string targetActorId = string.Empty;
        ulong targetActorGeneration = 0;
        if (command is ServiceWireConstants.Command.SpotSend
            or ServiceWireConstants.Command.SpotRequest)
        {
            if (!reader.TryText8(out sourceSpotId)
                || !reader.TryText8(out targetSpotId)
                || !reader.TryU64(out targetSpotGeneration)
                || targetSpotGeneration == 0)
            {
                error = reader.Truncated
                    ? DecodeError.TruncatedField
                    : DecodeError.InvalidField;
                return false;
            }
        }
        else
        {
            if (!reader.TryU8(out var hasSource)
                || hasSource != 0
                || !reader.TryU16(out var sourceLength)
                || sourceLength != 0
                || !reader.TryText8(out targetActorId)
                || !reader.TryU64(out targetActorGeneration)
                || targetActorGeneration == 0)
            {
                error = reader.Truncated
                    ? DecodeError.TruncatedField
                    : DecodeError.InvalidField;
                return false;
            }
        }

        if (!reader.TryRid(out var targetNodeRid)
            || !reader.TryU64(out var targetNodeGeneration)
            || targetNodeGeneration == 0
            || !reader.TryU64(out var authorityOwnerGeneration)
            || authorityOwnerGeneration == 0
            || !reader.TryU64(out var ownerLeaseGeneration)
            || ownerLeaseGeneration == 0
            || reader.Remaining != 0)
        {
            error = reader.Truncated
                ? DecodeError.TruncatedField
                : reader.Remaining != 0
                    ? DecodeError.TrailingByte
                    : DecodeError.InvalidField;
            return false;
        }
        if (!string.IsNullOrEmpty(targetActorId))
            targetActor = new ActorRef(
                targetActorId,
                targetActorGeneration,
                meshName,
                targetNodeRid);
        record = new StatefulRecord(
            command,
            correlation,
            new MeshOperationId(operationHigh, operationLow),
            sourceSpotId,
            targetSpotId,
            targetSpotGeneration,
            targetActor,
            targetNodeRid,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration,
            messageFollowHopCount,
            deadlineUnixMs,
            (flags & ServiceWireConstants.Flag.Metadata) != 0);
        error = DecodeError.None;
        return true;
    }

    internal static byte[] EncodeInstanceSpotActivation(
        InstanceSpotActivationOperation operation,
        bool hasMetadata)
    {
        ValidateInstanceActivation(operation);
        var target = operation.Target;
        var route = new WireWriter();
        route.Rid(target.TargetNodeRid);
        route.U64(target.TargetNodeGeneration);
        route.Text8(target.TargetSpotId);
        route.Text8(target.MeshName);
        route.Text8(target.StableType);
        route.Text8(target.DescriptorVersion);
        route.U64(operation.DeadlineUnixMs);

        var body = new WireWriter();
        body.U8(2);
        body.U16(checked((ushort)route.Count));
        body.Bytes(route.ToArray());
        body.U64(operation.SourceNodeGeneration);
        body.Rid(operation.SourceNodeRid);
        WriteOptionalText8(body, operation.SourceSpotId);
        body.U8(operation.IsRequest ? (byte)2 : (byte)1);
        WriteOperationId(body, operation.OperationId);
        if (operation.IsRequest)
            body.U64(operation.ReplyRouteId);

        var result = Prefix(
            ServiceWireConstants.Command.InstanceSpot,
            hasMetadata
                ? ServiceWireConstants.Flag.Metadata
                : ServiceWireConstants.Flag.None,
            body.Count);
        body.CopyTo(result.AsSpan(5));
        return result;
    }

    internal static bool TryDecodeInstanceSpotActivation(
        ReadOnlySpan<byte> bytes,
        out InstanceSpotActivationRecord record,
        out DecodeError error)
    {
        record = default;
        if (!TryDecodePrefix(bytes, out var command, out var flags, out error))
            return false;
        if (command != ServiceWireConstants.Command.InstanceSpot)
        {
            error = DecodeError.UnknownCommand;
            return false;
        }
        if ((flags & ~ServiceWireConstants.Flag.Metadata) != 0)
        {
            error = DecodeError.ForbiddenFlag;
            return false;
        }

        var reader = new WireReader(bytes[5..]);
        if (!reader.TryU8(out var version)
            || version != 2
            || !reader.TryU16(out var routeLength)
            || !reader.TrySlice(routeLength, out var routeBytes))
        {
            error = reader.Truncated
                ? DecodeError.TruncatedField
                : DecodeError.InvalidField;
            return false;
        }
        var route = new WireReader(routeBytes);
        if (!route.TryRid(out var targetNodeRid)
            || !route.TryU64(out var targetNodeGeneration)
            || targetNodeGeneration == 0
            || !route.TryText8(out var targetSpotId)
            || !route.TryText8(out var meshName)
            || !route.TryText8(out var stableType)
            || !route.TryText8(out var descriptorVersion)
            || !route.TryU64(out var deadlineUnixMs)
            || deadlineUnixMs is 0 or > long.MaxValue
            || route.Remaining != 0
            || !reader.TryU64(out var sourceNodeGeneration)
            || sourceNodeGeneration == 0
            || !reader.TryRid(out var sourceNodeRid)
            || !reader.TryOptionalText8(out var sourceSpotId)
            || !reader.TryU8(out var operationKind)
            || operationKind is not (1 or 2)
            || !TryReadOperationId(ref reader, out var operationId))
        {
            error = reader.Truncated || route.Truncated
                ? DecodeError.TruncatedField
                : route.Remaining != 0
                    ? DecodeError.TrailingByte
                    : DecodeError.InvalidField;
            return false;
        }
        var request = operationKind == 2;
        ulong replyRouteId = 0;
        if (request
            && (!reader.TryU64(out replyRouteId) || replyRouteId == 0)
            || reader.Remaining != 0)
        {
            error = reader.Truncated
                ? DecodeError.TruncatedField
                : reader.Remaining != 0
                    ? DecodeError.TrailingByte
                    : DecodeError.InvalidField;
            return false;
        }

        record = new InstanceSpotActivationRecord(
            new InstanceSpotActivationOperation(
                new InstanceSpotActivationTarget(
                    meshName,
                    targetNodeRid,
                    targetNodeGeneration,
                    targetSpotId,
                    stableType,
                    descriptorVersion),
                sourceNodeRid,
                sourceNodeGeneration,
                sourceSpotId ?? string.Empty,
                operationId,
                request,
                replyRouteId,
                deadlineUnixMs),
            (flags & ServiceWireConstants.Flag.Metadata) != 0);
        error = DecodeError.None;
        return true;
    }

    internal static byte[] EncodeUserSpotCreate(UserSpotCreateOperation operation)
    {
        ValidateOperationIdentity(
            operation.Correlation,
            operation.OperationId,
            operation.SourceNodeRid,
            operation.SourceNodeGeneration,
            operation.DeadlineUnixMs);
        if (!ZLinkSpotId.IsValid(operation.SpotId)
            || string.IsNullOrWhiteSpace(operation.StableType))
            throw new ArgumentOutOfRangeException(nameof(operation));
        ValidateReservation(operation.Reservation);

        var body = new WireWriter();
        body.U64(operation.Correlation);
        WriteOperationId(body, operation.OperationId);
        body.Rid(operation.SourceNodeRid);
        body.U64(operation.SourceNodeGeneration);
        body.Text8(operation.SpotId);
        body.Text8(operation.StableType);
        WriteReservation(body, operation.Reservation);
        body.U64(operation.DeadlineUnixMs);
        var result = Prefix(
            ServiceWireConstants.Command.UserSpotCreate,
            ServiceWireConstants.Flag.None,
            body.Count);
        body.CopyTo(result.AsSpan(5));
        return result;
    }

    internal static byte[] EncodeUserSpotClose(UserSpotCloseOperation operation)
    {
        ValidateOperationIdentity(
            operation.Correlation,
            operation.OperationId,
            operation.SourceNodeRid,
            operation.SourceNodeGeneration,
            operation.DeadlineUnixMs);
        ValidateCloseFence(operation.Target);

        var fenceBody = new WireWriter();
        fenceBody.Text8(operation.Target.SpotId);
        fenceBody.U64(operation.Target.ObjectGeneration);
        fenceBody.Rid(operation.Target.TargetNodeRid);
        fenceBody.U64(operation.Target.TargetNodeGeneration);
        fenceBody.U64(operation.Target.AuthorityOwnerGeneration);
        fenceBody.Text16(operation.Target.ExpectedStoreVersion, requireNonEmpty: true);

        var body = new WireWriter();
        body.U64(operation.Correlation);
        WriteOperationId(body, operation.OperationId);
        body.Rid(operation.SourceNodeRid);
        body.U64(operation.SourceNodeGeneration);
        body.U8(1);
        body.U16(checked((ushort)fenceBody.Count));
        body.Bytes(fenceBody.ToArray());
        body.U64(operation.DeadlineUnixMs);
        var result = Prefix(
            ServiceWireConstants.Command.UserSpotClose,
            ServiceWireConstants.Flag.None,
            body.Count);
        body.CopyTo(result.AsSpan(5));
        return result;
    }

    internal static byte[] EncodeActorCreate(ActorCreateOperation operation)
    {
        ValidateOperationIdentity(
            operation.Correlation,
            operation.OperationId,
            operation.SourceNodeRid,
            operation.SourceNodeGeneration,
            operation.DeadlineUnixMs);
        if (string.IsNullOrEmpty(operation.ActorId)
            || string.IsNullOrWhiteSpace(operation.StableType))
            throw new ArgumentOutOfRangeException(nameof(operation));
        ValidateReservation(operation.Reservation);

        var body = new WireWriter();
        body.U64(operation.Correlation);
        WriteOperationId(body, operation.OperationId);
        body.Rid(operation.SourceNodeRid);
        body.U64(operation.SourceNodeGeneration);
        body.Text8(operation.ActorId);
        body.Text8(operation.StableType);
        WriteReservation(body, operation.Reservation);
        body.U64(operation.DeadlineUnixMs);
        var result = Prefix(
            ServiceWireConstants.Command.ActorCreate,
            ServiceWireConstants.Flag.None,
            body.Count);
        body.CopyTo(result.AsSpan(5));
        return result;
    }

    internal static bool TryDecodeActorCreateOperation(
        ReadOnlySpan<byte> bytes,
        out ActorCreateOperationRecord record,
        out DecodeError error)
    {
        record = default;
        if (!TryDecodePrefix(bytes, out var command, out var flags, out error))
            return false;
        if (command != ServiceWireConstants.Command.ActorCreate)
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
        if (!reader.TryU64(out var correlation)
            || correlation == 0
            || !TryReadOperationId(ref reader, out var operationId)
            || !reader.TryRid(out var sourceNodeRid)
            || !reader.TryU64(out var sourceNodeGeneration)
            || sourceNodeGeneration == 0
            || !reader.TryText8(out var actorId)
            || !reader.TryText8(out var stableType)
            || !TryReadReservation(ref reader, out var reservation)
            || !reader.TryU64(out var deadlineUnixMs)
            || deadlineUnixMs is 0 or > long.MaxValue
            || reader.Remaining != 0)
        {
            error = reader.Truncated
                ? DecodeError.TruncatedField
                : reader.Remaining != 0
                    ? DecodeError.TrailingByte
                    : DecodeError.InvalidField;
            return false;
        }

        record = new ActorCreateOperationRecord(
            new ActorCreateOperation(
                correlation,
                operationId,
                sourceNodeRid,
                sourceNodeGeneration,
                actorId,
                stableType,
                reservation,
                deadlineUnixMs));
        error = DecodeError.None;
        return true;
    }

    internal static bool TryDecodeUserSpotOperation(
        ReadOnlySpan<byte> bytes,
        out UserSpotOperationRecord record,
        out DecodeError error)
    {
        record = default;
        if (!TryDecodePrefix(bytes, out var command, out var flags, out error))
            return false;
        if (command is not (ServiceWireConstants.Command.UserSpotCreate
            or ServiceWireConstants.Command.UserSpotClose))
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
        if (!reader.TryU64(out var correlation)
            || correlation == 0
            || !TryReadOperationId(ref reader, out var operationId)
            || !reader.TryRid(out var sourceNodeRid)
            || !reader.TryU64(out var sourceNodeGeneration)
            || sourceNodeGeneration == 0)
        {
            error = reader.Truncated ? DecodeError.TruncatedField : DecodeError.InvalidField;
            return false;
        }

        if (command == ServiceWireConstants.Command.UserSpotCreate)
        {
            if (!reader.TryText8(out var spotId)
                || !reader.TryText8(out var stableType)
                || !TryReadReservation(ref reader, out var reservation)
                || !reader.TryU64(out var deadlineUnixMs)
                || deadlineUnixMs is 0 or > long.MaxValue
                || reader.Remaining != 0)
            {
                error = reader.Truncated
                    ? DecodeError.TruncatedField
                    : reader.Remaining != 0
                        ? DecodeError.TrailingByte
                        : DecodeError.InvalidField;
                return false;
            }
            record = new UserSpotOperationRecord(
                command,
                new UserSpotCreateOperation(
                    correlation,
                    operationId,
                    sourceNodeRid,
                    sourceNodeGeneration,
                    spotId,
                    stableType,
                    reservation,
                    deadlineUnixMs),
                default);
        }
        else
        {
            if (!reader.TryU8(out var version)
                || version != 1
                || !reader.TryU16(out var fenceLength)
                || !reader.TrySlice(fenceLength, out var fenceBytes))
            {
                error = reader.Truncated ? DecodeError.TruncatedField : DecodeError.InvalidField;
                return false;
            }
            var fenceReader = new WireReader(fenceBytes);
            if (!fenceReader.TryText8(out var spotId)
                || !fenceReader.TryU64(out var objectGeneration)
                || objectGeneration == 0
                || !fenceReader.TryRid(out var targetNodeRid)
                || !fenceReader.TryU64(out var targetNodeGeneration)
                || targetNodeGeneration == 0
                || !fenceReader.TryU64(out var authorityOwnerGeneration)
                || authorityOwnerGeneration == 0
                || !fenceReader.TryText16(out var expectedStoreVersion, requireNonEmpty: true)
                || fenceReader.Remaining != 0
                || !reader.TryU64(out var deadlineUnixMs)
                || deadlineUnixMs is 0 or > long.MaxValue
                || reader.Remaining != 0)
            {
                error = reader.Truncated || fenceReader.Truncated
                    ? DecodeError.TruncatedField
                    : reader.Remaining != 0 || fenceReader.Remaining != 0
                        ? DecodeError.TrailingByte
                        : DecodeError.InvalidField;
                return false;
            }
            record = new UserSpotOperationRecord(
                command,
                default,
                new UserSpotCloseOperation(
                    correlation,
                    operationId,
                    sourceNodeRid,
                    sourceNodeGeneration,
                    new UserSpotCloseFence(
                        spotId,
                        objectGeneration,
                        targetNodeRid,
                        targetNodeGeneration,
                        authorityOwnerGeneration,
                        expectedStoreVersion),
                    deadlineUnixMs));
        }

        error = DecodeError.None;
        return true;
    }

    internal static byte[] EncodeRouteAdmission(
        ServiceWireConstants.Command command,
        string meshName,
        string advertisedEndpoint,
        ulong lifecycleGeneration,
        ulong descriptorRevision,
        IReadOnlyDictionary<string, uint> channels,
        byte objectRole = 0,
        //  descriptor-extension field 1. Schema는 preparing=0·serving=1·
        //  draining=2를 이미 정의하고, admission guard도 field 1을 변경 가능한
        //  것으로 둔다. 기본값 serving을 유지해야 golden fixture가 바이트
        //  동일하게 남는다.
        byte runtimeState = 1,
        string securityIdentity = "none",
        uint effectiveMaxMessageBytes = uint.MaxValue)
    {
        if (command is not (ServiceWireConstants.Command.Hello
            or ServiceWireConstants.Command.Admit
            or ServiceWireConstants.Command.Update))
            throw new ArgumentOutOfRangeException(nameof(command));
        if (lifecycleGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(lifecycleGeneration));
        if (descriptorRevision == 0)
            throw new ArgumentOutOfRangeException(nameof(descriptorRevision));
        if (objectRole > (byte)ZLinkMeshNodeObjectRole.Server)
            throw new ArgumentOutOfRangeException(nameof(objectRole));
        ArgumentException.ThrowIfNullOrWhiteSpace(securityIdentity);
        if (effectiveMaxMessageBytes == 0)
            throw new ArgumentOutOfRangeException(nameof(effectiveMaxMessageBytes));
        ArgumentNullException.ThrowIfNull(channels);

        var body = new WireWriter();
        body.Text8(meshName);
        body.Text8(securityIdentity);
        body.U32(effectiveMaxMessageBytes);
        body.U64(lifecycleGeneration);
        body.U64(descriptorRevision);
        body.Text16(advertisedEndpoint, requireNonEmpty: true);
        var orderedChannels = channels.OrderBy(
            static entry => entry.Key,
            StringComparer.Ordinal).ToArray();
        if (orderedChannels.Length > ushort.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(channels));
        body.U16((ushort)orderedChannels.Length);
        foreach (var (name, weight) in orderedChannels)
        {
            if (weight > ZLinkSocketConfig.MaximumPeerWeight)
                throw new ArgumentOutOfRangeException(nameof(channels));
            body.Text8(name);
            body.U32(weight);
        }
        body.Bytes(EncodeDescriptorExtension(objectRole, runtimeState));

        var admission = new WireWriter();
        admission.U8(1); // service-topology-kind.routeMesh
        admission.U32(checked((uint)body.Count));
        admission.Bytes(body.ToArray());

        var result = Prefix(command, ServiceWireConstants.Flag.None, admission.Count);
        admission.CopyTo(result.AsSpan(5));
        return result;
    }

    internal static bool TryDecodeRouteAdmission(
        ReadOnlySpan<byte> bytes,
        out ServiceWireConstants.Command command,
        out AdmissionRecord admission,
        out DecodeError error)
    {
        admission = default;
        if (!TryDecodePrefix(bytes, out command, out var flags, out error))
            return false;
        if (command is not (ServiceWireConstants.Command.Hello
            or ServiceWireConstants.Command.Admit
            or ServiceWireConstants.Command.Update))
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
        if (!reader.TryU8(out var topology)
            || topology != 1
            || !reader.TryU32(out var bodyLength)
            || bodyLength != reader.Remaining)
        {
            error = reader.Truncated ? DecodeError.TruncatedField : DecodeError.InvalidField;
            return false;
        }
        if (!reader.TryText8(out var meshName)
            || !reader.TryText8(out var securityIdentity)
            || !reader.TryU32(out var maxMessageBytes)
            || maxMessageBytes == 0
            || !reader.TryU64(out var lifecycleGeneration)
            || lifecycleGeneration == 0
            || !reader.TryU64(out var descriptorRevision)
            || descriptorRevision == 0
            || !reader.TryText16(out var endpoint, requireNonEmpty: true)
            || !reader.TryU16(out var channelCount))
        {
            error = reader.Truncated ? DecodeError.TruncatedField : DecodeError.InvalidField;
            return false;
        }

        var channels = new Dictionary<string, uint>(
            channelCount,
            StringComparer.Ordinal);
        string? previousChannel = null;
        for (var index = 0; index < channelCount; index++)
        {
            if (!reader.TryText8(out var channel)
                || !reader.TryU32(out var weight)
                || weight > ZLinkSocketConfig.MaximumPeerWeight
                || (previousChannel is not null
                    && StringComparer.Ordinal.Compare(previousChannel, channel) >= 0)
                || !channels.TryAdd(channel, weight))
            {
                error = reader.Truncated ? DecodeError.TruncatedField : DecodeError.InvalidField;
                return false;
            }
            previousChannel = channel;
        }

        if (!TryDecodeDescriptorExtension(
                ref reader,
                out var runtimeState,
                out var applicationVersion,
                out var objectRole,
                out var placementWeight,
                out var activeCapacityLimit,
                out var pendingCapacityLimit,
                out var activeCapacityUsed,
                out var pendingCapacityUsed,
                out var extensionFields))
        {
            error = reader.Truncated ? DecodeError.TruncatedField : DecodeError.InvalidField;
            return false;
        }
        if (reader.Remaining != 0)
        {
            error = DecodeError.TrailingByte;
            return false;
        }

        admission = new AdmissionRecord(
            meshName,
            securityIdentity,
            maxMessageBytes,
            endpoint,
            lifecycleGeneration,
            descriptorRevision,
            channels,
            runtimeState,
            applicationVersion,
            objectRole,
            placementWeight,
            activeCapacityLimit,
            pendingCapacityLimit,
            activeCapacityUsed,
            pendingCapacityUsed,
            extensionFields,
            bytes[10..].ToArray());
        error = DecodeError.None;
        return true;
    }

    internal static byte[] EncodeText(string value)
    {
        ArgumentException.ThrowIfNullOrEmpty(value);
        if (value.Contains('\0'))
            throw new ArgumentException("Text must not contain NUL.", nameof(value));
        var count = Encoding.UTF8.GetByteCount(value);
        if (count is 0 or > byte.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(value));

        var bytes = new byte[count + 1];
        bytes[0] = (byte)count;
        Encoding.UTF8.GetBytes(value, bytes.AsSpan(1));
        return bytes;
    }

    private static void ValidateOperationIdentity(
        ulong correlation,
        MeshOperationId operationId,
        RoutingId sourceNodeRid,
        ulong sourceNodeGeneration,
        ulong deadlineUnixMs)
    {
        if (correlation == 0
            || (operationId.High == 0 && operationId.Low == 0)
            || sourceNodeRid.IsEmpty
            || sourceNodeGeneration == 0
            || deadlineUnixMs is 0 or > long.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(correlation));
    }

    private static void ValidateReservation(ObjectReservationFence reservation)
    {
        if (string.IsNullOrWhiteSpace(reservation.ReservationId)
            || string.IsNullOrWhiteSpace(reservation.ExpectedStoreVersion)
            || reservation.ObjectGeneration == 0
            || reservation.AuthorityOwnerGeneration == 0
            || reservation.TargetNodeRid.IsEmpty
            || reservation.TargetNodeGeneration == 0
            || string.IsNullOrWhiteSpace(reservation.TargetOwnerId)
            || reservation.TargetOwnerLeaseGeneration == 0
            || reservation.PendingCapacityDelta == 0)
            throw new ArgumentOutOfRangeException(nameof(reservation));
    }

    private static void ValidateCloseFence(UserSpotCloseFence target)
    {
        if (!ZLinkSpotId.IsValid(target.SpotId)
            || target.ObjectGeneration == 0
            || target.TargetNodeRid.IsEmpty
            || target.TargetNodeGeneration == 0
            || target.AuthorityOwnerGeneration == 0
            || string.IsNullOrWhiteSpace(target.ExpectedStoreVersion))
            throw new ArgumentOutOfRangeException(nameof(target));
    }

    private static void WriteOperationId(WireWriter writer, MeshOperationId operationId)
    {
        writer.U64(operationId.High);
        writer.U64(operationId.Low);
    }

    private static bool TryReadOperationId(
        ref WireReader reader,
        out MeshOperationId operationId)
    {
        operationId = default;
        if (!reader.TryU64(out var high)
            || !reader.TryU64(out var low)
            || (high == 0 && low == 0))
            return false;
        operationId = new MeshOperationId(high, low);
        return true;
    }

    private static void WriteReservation(
        WireWriter writer,
        ObjectReservationFence reservation)
    {
        writer.Text8(reservation.ReservationId);
        writer.Text16(reservation.ExpectedStoreVersion, requireNonEmpty: true);
        writer.U64(reservation.ObjectGeneration);
        writer.U64(reservation.AuthorityOwnerGeneration);
        writer.Rid(reservation.TargetNodeRid);
        writer.U64(reservation.TargetNodeGeneration);
        writer.Text8(reservation.TargetOwnerId);
        writer.U64(reservation.TargetOwnerLeaseGeneration);
        writer.U32(reservation.PendingCapacityDelta);
    }

    private static void ValidateInstanceActivation(
        InstanceSpotActivationOperation operation)
    {
        var target = operation.Target;
        if (target.TargetNodeRid.IsEmpty
            || target.TargetNodeGeneration == 0
            || !ZLinkSpotId.IsValid(target.TargetSpotId)
            || string.IsNullOrWhiteSpace(target.MeshName)
            || string.IsNullOrWhiteSpace(target.StableType)
            || string.IsNullOrWhiteSpace(target.DescriptorVersion)
            || operation.SourceNodeRid.IsEmpty
            || operation.SourceNodeGeneration == 0
            || operation.OperationId == default
            || operation.DeadlineUnixMs is 0 or > long.MaxValue
            || operation.IsRequest != (operation.ReplyRouteId != 0))
            throw new ArgumentOutOfRangeException(nameof(operation));
    }

    private static void WriteOptionalText8(WireWriter writer, string? value)
    {
        // optional-text8 carries the u8 length itself; length 0 means absent.
        if (string.IsNullOrEmpty(value))
        {
            writer.U8(0);
            return;
        }
        writer.Text8(value);
    }

    private static void WriteOptionalRid(WireWriter writer, RoutingId value)
    {
        if (value.IsEmpty)
        {
            writer.U8(0);
            return;
        }
        writer.Rid(value);
    }

    private static bool TryReadOptionalRid(
        ref WireReader reader,
        out RoutingId value)
    {
        value = default;
        if (!reader.TryU8(out var length))
            return false;
        if (length == 0) return true;
        if (!reader.TrySlice(length, out var encoded))
            return false;
        value = RoutingId.From(encoded);
        return true;
    }

    private static bool TryReadOptionalText8(
        ref WireReader reader,
        out string? value)
    {
        value = null;
        if (!reader.TryU8(out var present) || present > 1)
            return false;
        if (present == 0) return true;
        return reader.TryText8(out value);
    }

    private static bool TryReadReservation(
        ref WireReader reader,
        out ObjectReservationFence reservation)
    {
        reservation = default;
        if (!reader.TryText8(out var reservationId)
            || !reader.TryText16(out var expectedStoreVersion, requireNonEmpty: true)
            || !reader.TryU64(out var objectGeneration)
            || objectGeneration == 0
            || !reader.TryU64(out var authorityOwnerGeneration)
            || authorityOwnerGeneration == 0
            || !reader.TryRid(out var targetNodeRid)
            || !reader.TryU64(out var targetNodeGeneration)
            || targetNodeGeneration == 0
            || !reader.TryText8(out var targetOwnerId)
            || !reader.TryU64(out var targetOwnerLeaseGeneration)
            || targetOwnerLeaseGeneration == 0
            || !reader.TryU32(out var pendingCapacityDelta)
            || pendingCapacityDelta == 0)
            return false;
        reservation = new ObjectReservationFence(
            reservationId,
            expectedStoreVersion,
            objectGeneration,
            authorityOwnerGeneration,
            targetNodeRid,
            targetNodeGeneration,
            targetOwnerId,
            targetOwnerLeaseGeneration,
            pendingCapacityDelta);
        return true;
    }

    private static byte[] Prefix(
        ServiceWireConstants.Command command,
        ServiceWireConstants.Flag flags,
        int bodyLength)
    {
        var bytes = new byte[5 + bodyLength];
        bytes[0] = ServiceWireConstants.Magic0;
        bytes[1] = ServiceWireConstants.Magic1;
        bytes[2] = ServiceWireConstants.WireMajor;
        bytes[3] = (byte)command;
        bytes[4] = (byte)flags;
        return bytes;
    }

    private static byte[] EncodeDescriptorExtension(byte objectRole, byte runtimeState)
    {
        var fields = new WireWriter();
        fields.Tlv(1, [runtimeState]); // runtime-state

        var applicationVersion = new byte[sizeof(long)];
        BinaryPrimitives.WriteInt64BigEndian(applicationVersion, 0);
        fields.Tlv(2, applicationVersion);

        var capabilities = new WireWriter();
        capabilities.U16(1);
        capabilities.Text8(ServiceWireConstants.RequiredCapability);
        fields.Tlv(6, capabilities.ToArray());
        fields.Tlv(7, [objectRole]);
        fields.TlvU32(8, 100);
        fields.TlvU32(9, 10_000);
        fields.TlvU32(10, 128);
        fields.TlvU32(11, 0);
        fields.TlvU32(12, 0);

        var result = new WireWriter();
        result.U32(checked((uint)fields.Count));
        result.Bytes(fields.ToArray());
        return result.ToArray();
    }

    private static bool TryDecodeDescriptorExtension(
        ref WireReader reader,
        out byte runtimeState,
        out long applicationVersion,
        out byte objectRole,
        out uint placementWeight,
        out uint activeCapacityLimit,
        out uint pendingCapacityLimit,
        out uint activeCapacityUsed,
        out uint pendingCapacityUsed,
        out IReadOnlyDictionary<byte, byte[]> fields)
    {
        runtimeState = 0;
        applicationVersion = 0;
        objectRole = 0;
        placementWeight = 0;
        activeCapacityLimit = 0;
        pendingCapacityLimit = 0;
        activeCapacityUsed = 0;
        pendingCapacityUsed = 0;
        fields = new Dictionary<byte, byte[]>();
        if (!reader.TryU32(out var length) || length > reader.Remaining)
            return false;
        if (!reader.TrySlice(checked((int)length), out var extensionBytes))
            return false;

        var extension = new WireReader(extensionBytes);
        var required = new HashSet<byte>();
        var preserved = new Dictionary<byte, byte[]>();
        byte previousId = 0;
        var hasCapability = false;
        while (extension.Remaining > 0)
        {
            if (!extension.TryU8(out var id)
                || id <= previousId
                || !extension.TryU32(out var fieldLength)
                || fieldLength > extension.Remaining
                || !extension.TrySlice(checked((int)fieldLength), out var field))
                return false;
            previousId = id;
            preserved.Add(id, field.ToArray());
            if (id is 1 or 2 or 6 or 7 or 8 or 9 or 10 or 11 or 12)
                required.Add(id);
            var value = new WireReader(field);
            switch (id)
            {
                case 1:
                    if (!value.TryU8(out runtimeState)
                        || runtimeState > 4
                        || value.Remaining != 0)
                        return false;
                    break;
                case 2:
                {
                    if (!value.TryU64(out var encodedVersion)
                        || encodedVersion > long.MaxValue
                        || value.Remaining != 0)
                        return false;
                    applicationVersion = checked((long)encodedVersion);
                    break;
                }
                case 3:
                    if (!TryDecodeSortedText8Vector(ref value)
                        || value.Remaining != 0)
                        return false;
                    break;
                case 4:
                    if (!TryDecodeStatefulCapabilities(ref value)
                        || value.Remaining != 0)
                        return false;
                    break;
                case 5:
                    if (!value.TryOptionalText8(out _)
                        || value.Remaining != 0)
                        return false;
                    break;
                case 6:
                {
                    if (!value.TryU16(out var count) || count > 1024)
                        return false;
                    byte[]? previousCapability = null;
                    for (var index = 0; index < count; index++)
                    {
                        if (!value.TryText8(out var capability))
                            return false;
                        var encodedCapability = Encoding.UTF8.GetBytes(capability);
                        if (previousCapability is not null
                            && previousCapability.AsSpan()
                                .SequenceCompareTo(encodedCapability) >= 0)
                            return false;
                        previousCapability = encodedCapability;
                        hasCapability |= string.Equals(
                            capability,
                            ServiceWireConstants.RequiredCapability,
                            StringComparison.Ordinal);
                    }
                    if (value.Remaining != 0)
                        return false;
                    break;
                }
                case 7:
                    if (!value.TryU8(out objectRole)
                        || objectRole > 2
                        || value.Remaining != 0)
                        return false;
                    break;
                case 8:
                    if (!value.TryU32(out placementWeight)
                        || placementWeight > ZLinkSocketConfig.MaximumPeerWeight
                        || value.Remaining != 0)
                        return false;
                    break;
                case 9:
                    if (!value.TryU32(out activeCapacityLimit)
                        || activeCapacityLimit == 0
                        || value.Remaining != 0)
                        return false;
                    break;
                case 10:
                    if (!value.TryU32(out pendingCapacityLimit)
                        || value.Remaining != 0)
                        return false;
                    break;
                case 11:
                    if (!value.TryU32(out activeCapacityUsed)
                        || value.Remaining != 0)
                        return false;
                    break;
                case 12:
                    if (!value.TryU32(out pendingCapacityUsed)
                        || value.Remaining != 0)
                        return false;
                    break;
            }
        }

        if (!hasCapability
            || !required.SetEquals(new byte[] { 1, 2, 6, 7, 8, 9, 10, 11, 12 })
            || activeCapacityUsed > activeCapacityLimit
            || pendingCapacityUsed > pendingCapacityLimit)
            return false;
        fields = preserved;
        return true;
    }

    private static bool TryDecodeSortedText8Vector(ref WireReader reader)
    {
        if (!reader.TryU16(out var count) || count > 1024)
            return false;
        byte[]? previous = null;
        for (var index = 0; index < count; index++)
        {
            if (!reader.TryText8(out var item))
                return false;
            var current = Encoding.UTF8.GetBytes(item);
            if (previous is not null
                && previous.AsSpan().SequenceCompareTo(current) >= 0)
                return false;
            previous = current;
        }
        return true;
    }

    private static bool TryDecodeStatefulCapabilities(ref WireReader reader)
    {
        if (!reader.TryU16(out var count) || count > 1024)
            return false;
        byte previousKind = 0;
        byte[]? previousType = null;
        for (var index = 0; index < count; index++)
        {
            if (!reader.TryU8(out var objectKind)
                || objectKind is < 1 or > 3
                || !reader.TryText8(out var type)
                || !reader.TryU8(out var relocationPolicy)
                || relocationPolicy > 2
                || !TryDecodeSortedText8Vector(ref reader)
                || !reader.TryU32(out var activeCapacityLimit)
                || activeCapacityLimit == 0
                || !reader.TryU32(out _)
                || !reader.TryU8(out var hasSnapshotAdapter)
                || hasSnapshotAdapter > 1
                || !reader.TryU64(out var available)
                || available > long.MaxValue)
                return false;

            var encodedType = Encoding.UTF8.GetBytes(type);
            if (objectKind < previousKind
                || (objectKind == previousKind
                    && previousType is not null
                    && previousType.AsSpan().SequenceCompareTo(encodedType) >= 0))
                return false;
            previousKind = objectKind;
            previousType = encodedType;
        }
        return true;
    }

    private static bool TryDecodePrefix(
        ReadOnlySpan<byte> bytes,
        out ServiceWireConstants.Command command,
        out ServiceWireConstants.Flag flags,
        out DecodeError error)
    {
        command = default;
        flags = default;
        if (bytes.Length < 5)
        {
            error = DecodeError.TruncatedField;
            return false;
        }
        if (bytes[0] != ServiceWireConstants.Magic0
            || bytes[1] != ServiceWireConstants.Magic1)
        {
            error = DecodeError.InvalidMagic;
            return false;
        }
        if (bytes[2] != ServiceWireConstants.WireMajor)
        {
            error = DecodeError.UnsupportedVersion;
            return false;
        }

        command = (ServiceWireConstants.Command)bytes[3];
        flags = (ServiceWireConstants.Flag)bytes[4];
        error = DecodeError.None;
        return true;
    }

    private static bool TryDecodeText(
        ReadOnlySpan<byte> bytes,
        out string value,
        out int consumed,
        out DecodeError error)
    {
        value = string.Empty;
        consumed = 0;
        if (bytes.Length < 1)
        {
            error = DecodeError.TruncatedField;
            return false;
        }
        var count = bytes[0];
        if (count == 0)
        {
            error = DecodeError.InvalidField;
            return false;
        }
        if (bytes.Length < count + 1)
        {
            error = DecodeError.TruncatedField;
            return false;
        }
        var content = bytes.Slice(1, count);
        if (content.IndexOf((byte)0) >= 0)
        {
            error = DecodeError.InvalidField;
            return false;
        }
        try
        {
            value = new UTF8Encoding(false, true).GetString(content);
        }
        catch (DecoderFallbackException)
        {
            error = DecodeError.InvalidField;
            return false;
        }
        consumed = count + 1;
        error = DecodeError.None;
        return true;
    }

    private sealed class WireWriter
    {
        private readonly List<byte> _bytes = new();
        internal int Count => _bytes.Count;
        internal void U8(byte value) => _bytes.Add(value);
        internal void U16(ushort value)
        {
            _bytes.Add((byte)(value >> 8));
            _bytes.Add((byte)value);
        }
        internal void U32(uint value)
        {
            _bytes.Add((byte)(value >> 24));
            _bytes.Add((byte)(value >> 16));
            _bytes.Add((byte)(value >> 8));
            _bytes.Add((byte)value);
        }
        internal void U64(ulong value)
        {
            U32((uint)(value >> 32));
            U32((uint)value);
        }
        internal void Text8(string value) => Bytes(EncodeText(value));
        internal void Rid(RoutingId value)
        {
            if (value.IsEmpty)
                throw new ArgumentException("Routing id is required.", nameof(value));
            U8(checked((byte)value.Size));
            Bytes(value.ToBytes());
        }
        internal void Text16(string value, bool requireNonEmpty)
        {
            ArgumentNullException.ThrowIfNull(value);
            if (requireNonEmpty)
                ArgumentException.ThrowIfNullOrEmpty(value);
            if (value.Contains('\0'))
                throw new ArgumentException("Text must not contain NUL.", nameof(value));
            var encoded = Encoding.UTF8.GetBytes(value);
            if (encoded.Length > ushort.MaxValue)
                throw new ArgumentOutOfRangeException(nameof(value));
            U16((ushort)encoded.Length);
            Bytes(encoded);
        }
        internal void Bytes(ReadOnlySpan<byte> value)
        {
            foreach (var item in value)
                _bytes.Add(item);
        }
        internal void Tlv(byte id, ReadOnlySpan<byte> value)
        {
            U8(id);
            U32(checked((uint)value.Length));
            Bytes(value);
        }
        internal void TlvU32(byte id, uint value)
        {
            Span<byte> bytes = stackalloc byte[sizeof(uint)];
            BinaryPrimitives.WriteUInt32BigEndian(bytes, value);
            Tlv(id, bytes);
        }
        internal byte[] ToArray() => _bytes.ToArray();
        internal void CopyTo(Span<byte> destination) =>
            _bytes.ToArray().CopyTo(destination);
    }

    private ref struct WireReader
    {
        private ReadOnlySpan<byte> _bytes;
        private int _offset;
        internal WireReader(ReadOnlySpan<byte> bytes)
        {
            _bytes = bytes;
            _offset = 0;
            Truncated = false;
        }
        internal int Remaining => _bytes.Length - _offset;
        internal bool Truncated { get; private set; }
        internal void MarkTruncated() => Truncated = true;
        internal bool TryU8(out byte value)
        {
            if (Remaining < 1)
            {
                Truncated = true;
                value = 0;
                return false;
            }
            value = _bytes[_offset++];
            return true;
        }
        internal bool TryU16(out ushort value)
        {
            if (Remaining < sizeof(ushort))
            {
                Truncated = true;
                value = 0;
                return false;
            }
            value = BinaryPrimitives.ReadUInt16BigEndian(_bytes[_offset..]);
            _offset += sizeof(ushort);
            return true;
        }
        internal bool TryU32(out uint value)
        {
            if (Remaining < sizeof(uint))
            {
                Truncated = true;
                value = 0;
                return false;
            }
            value = BinaryPrimitives.ReadUInt32BigEndian(_bytes[_offset..]);
            _offset += sizeof(uint);
            return true;
        }
        internal bool TryU64(out ulong value)
        {
            if (Remaining < sizeof(ulong))
            {
                Truncated = true;
                value = 0;
                return false;
            }
            value = BinaryPrimitives.ReadUInt64BigEndian(_bytes[_offset..]);
            _offset += sizeof(ulong);
            return true;
        }
        internal bool TrySlice(int length, out ReadOnlySpan<byte> value)
        {
            if (length < 0 || Remaining < length)
            {
                Truncated = true;
                value = default;
                return false;
            }
            value = _bytes.Slice(_offset, length);
            _offset += length;
            return true;
        }
        internal bool TryText8(out string value)
        {
            if (!TryU8(out var length) || length == 0)
            {
                value = string.Empty;
                return false;
            }
            return TryUtf8(length, out value);
        }
        internal bool TryOptionalText8(out string? value)
        {
            if (!TryU8(out var length))
            {
                value = null;
                return false;
            }
            if (length == 0)
            {
                value = null;
                return true;
            }
            if (!TryUtf8(length, out var decoded))
            {
                value = null;
                return false;
            }
            value = decoded;
            return true;
        }
        internal bool TryRid(out RoutingId value)
        {
            if (!TryU8(out var length) || length == 0
                || !TrySlice(length, out var encoded))
            {
                value = default;
                return false;
            }
            value = RoutingId.From(encoded);
            return true;
        }
        internal bool TryText16(out string value, bool requireNonEmpty)
        {
            if (!TryU16(out var length) || (requireNonEmpty && length == 0))
            {
                value = string.Empty;
                return false;
            }
            return TryUtf8(length, out value);
        }
        private bool TryUtf8(int length, out string value)
        {
            if (!TrySlice(length, out var encoded) || encoded.IndexOf((byte)0) >= 0)
            {
                value = string.Empty;
                return false;
            }
            try
            {
                value = new UTF8Encoding(false, true).GetString(encoded);
                return true;
            }
            catch (DecoderFallbackException)
            {
                value = string.Empty;
                return false;
            }
        }
    }
}
