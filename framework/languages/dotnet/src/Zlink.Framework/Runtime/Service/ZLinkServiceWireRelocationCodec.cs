using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Service;

internal static partial class ZLinkServiceWireCodec
{
    internal readonly record struct RelocationWireId(ulong High, ulong Low)
    {
        internal bool IsEmpty => High == 0 && Low == 0;
    }

    internal readonly record struct RelocationCoordinatorFence(
        string OwnerId,
        ulong LeaseGeneration,
        RoutingId NodeRid,
        ulong NodeGeneration,
        string ExpectedAuthorityStoreVersion);

    internal readonly record struct RequestSourceFence(
        string OwnerId,
        ulong LeaseGeneration,
        RoutingId NodeRid,
        ulong NodeGeneration);

    internal readonly record struct ReplyRelayRecord(
        MeshOperationId OperationId,
        ulong ReplyRouteId,
        RelocationWireId RelocationId,
        ulong TargetAttemptGeneration,
        RelocationCoordinatorFence Coordinator,
        ulong ParticipantId,
        ulong Sequence,
        uint TerminalResult,
        ServiceWireConstants.FrameworkErrorCode FailureCode);

    internal readonly record struct ReplyRelayAckRecord(
        RelocationWireId RelocationId,
        RelocationCoordinatorFence Coordinator,
        MeshOperationId OperationId,
        ulong ReplyRouteId,
        RequestSourceFence RequestSource,
        byte Status);

    internal static byte[] EncodeReplyRelay(ReplyRelayRecord record)
    {
        ValidateOperation(record.OperationId);
        if (record.ReplyRouteId == 0
            || record.RelocationId.IsEmpty
            || record.TargetAttemptGeneration == 0
            || record.ParticipantId == 0
            || record.Sequence == 0
            || !IsTerminalFailureValid(
                record.TerminalResult,
                record.FailureCode))
            throw new ArgumentOutOfRangeException(nameof(record));
        ValidateCoordinator(record.Coordinator);

        var context = new WireWriter();
        context.U64(record.RelocationId.High);
        context.U64(record.RelocationId.Low);
        context.U64(record.TargetAttemptGeneration);
        WriteCoordinator(context, record.Coordinator);
        context.U64(record.ParticipantId);
        context.U64(record.Sequence);
        if (context.Count > ushort.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(record));

        var body = new WireWriter();
        body.U64(record.OperationId.High);
        body.U64(record.OperationId.Low);
        body.U64(record.ReplyRouteId);
        body.U8(2); // reply-relay-context-kind.maintenanceRelocation
        body.U16(checked((ushort)context.Count));
        body.Bytes(context.ToArray());
        body.U32(record.TerminalResult);
        body.U32((uint)record.FailureCode);
        var result = Prefix(
            ServiceWireConstants.Command.ReplyRelay,
            ServiceWireConstants.Flag.None,
            body.Count);
        body.CopyTo(result.AsSpan(5));
        return result;
    }

    internal static bool TryDecodeReplyRelay(
        ReadOnlySpan<byte> bytes,
        out ReplyRelayRecord record,
        out DecodeError error)
    {
        record = default;
        if (!TryDecodePrefix(bytes, out var command, out var flags, out error))
            return false;
        if (command != ServiceWireConstants.Command.ReplyRelay)
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
        if (!TryOperation(ref reader, out var operation)
            || !reader.TryU64(out var replyRouteId) || replyRouteId == 0
            || !reader.TryU8(out var contextKind) || contextKind != 2
            || !reader.TryU16(out var contextLength)
            || !reader.TrySlice(contextLength, out var contextBytes))
            return Fail(ref reader, out error);
        var context = new WireReader(contextBytes);
        if (!TryRelocationId(ref context, out var relocation)
            || !context.TryU64(out var attempt) || attempt == 0
            || !TryCoordinator(ref context, out var coordinator)
            || !context.TryU64(out var participantId) || participantId == 0
            || !context.TryU64(out var sequence) || sequence == 0
            || context.Remaining != 0
            || !reader.TryU32(out var terminalResult)
            || !reader.TryU32(out var failureValue)
            || !IsTerminalFailureValid(
                terminalResult,
                (ServiceWireConstants.FrameworkErrorCode)failureValue))
            return Fail(ref reader, out error);
        if (reader.Remaining != 0)
        {
            error = DecodeError.TrailingByte;
            return false;
        }
        record = new ReplyRelayRecord(
            operation,
            replyRouteId,
            relocation,
            attempt,
            coordinator,
            participantId,
            sequence,
            terminalResult,
            (ServiceWireConstants.FrameworkErrorCode)failureValue);
        error = DecodeError.None;
        return true;
    }

    internal static byte[] EncodeReplyRelayAck(ReplyRelayAckRecord record)
    {
        if (record.RelocationId.IsEmpty
            || record.ReplyRouteId == 0
            || record.Status is not (1 or 2))
            throw new ArgumentOutOfRangeException(nameof(record));
        ValidateCoordinator(record.Coordinator);
        ValidateOperation(record.OperationId);
        ValidateRequestSource(record.RequestSource);

        var body = new WireWriter();
        body.U64(record.RelocationId.High);
        body.U64(record.RelocationId.Low);
        WriteCoordinator(body, record.Coordinator);
        body.U64(record.OperationId.High);
        body.U64(record.OperationId.Low);
        body.U64(record.ReplyRouteId);
        WriteRequestSource(body, record.RequestSource);
        body.U8(record.Status);
        var result = Prefix(
            ServiceWireConstants.Command.ReplyRelayAck,
            ServiceWireConstants.Flag.None,
            body.Count);
        body.CopyTo(result.AsSpan(5));
        return result;
    }

    internal static bool TryDecodeReplyRelayAck(
        ReadOnlySpan<byte> bytes,
        out ReplyRelayAckRecord record,
        out DecodeError error)
    {
        record = default;
        if (!TryDecodePrefix(bytes, out var command, out var flags, out error))
            return false;
        if (command != ServiceWireConstants.Command.ReplyRelayAck)
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
        if (!TryRelocationId(ref reader, out var relocation)
            || !TryCoordinator(ref reader, out var coordinator)
            || !TryOperation(ref reader, out var operation)
            || !reader.TryU64(out var replyRouteId) || replyRouteId == 0
            || !TryRequestSource(ref reader, out var requestSource)
            || !reader.TryU8(out var status)
            || status is not (1 or 2))
            return Fail(ref reader, out error);
        if (reader.Remaining != 0)
        {
            error = DecodeError.TrailingByte;
            return false;
        }
        record = new ReplyRelayAckRecord(
            relocation,
            coordinator,
            operation,
            replyRouteId,
            requestSource,
            status);
        error = DecodeError.None;
        return true;
    }

    private static void ValidateOperation(MeshOperationId operation)
    {
        if (operation.High == 0 && operation.Low == 0)
            throw new ArgumentOutOfRangeException(nameof(operation));
    }

    private static bool IsTerminalFailureValid(
        uint terminalResult,
        ServiceWireConstants.FrameworkErrorCode failureCode)
    {
        if (!Enum.IsDefined(failureCode))
            return false;
        if (terminalResult == 0)
            return failureCode == ServiceWireConstants.FrameworkErrorCode.None;
        if (terminalResult is 101 or 103 or >= 108 and <= 113)
            return failureCode == ServiceWireConstants.FrameworkErrorCode.None;
        if (failureCode == ServiceWireConstants.FrameworkErrorCode.None)
            return false;
        return failureCode switch
        {
            ServiceWireConstants.FrameworkErrorCode.ActorRouteNotFound
                or ServiceWireConstants.FrameworkErrorCode.SpotRouteNotFound
                or ServiceWireConstants.FrameworkErrorCode.ActorSessionNotBound
                or ServiceWireConstants.FrameworkErrorCode.HandlerNotFound
                or ServiceWireConstants.FrameworkErrorCode.RouteHandlerNotFound
                or ServiceWireConstants.FrameworkErrorCode.ActorDispatchHandlerNotFound
                or ServiceWireConstants.FrameworkErrorCode.RequestTargetNotFound =>
                terminalResult == 102,
            ServiceWireConstants.FrameworkErrorCode.PayloadDecodeFailed
                or ServiceWireConstants.FrameworkErrorCode.RequestProtocolError =>
                terminalResult == 104,
            ServiceWireConstants.FrameworkErrorCode.ActorCreateFailed
                or ServiceWireConstants.FrameworkErrorCode.SpotCreateFailed
                or ServiceWireConstants.FrameworkErrorCode.RouteNotConnected
                or ServiceWireConstants.FrameworkErrorCode.RequestFailed
                or ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut
                or ServiceWireConstants.FrameworkErrorCode.WorkerFailed
                or ServiceWireConstants.FrameworkErrorCode.RelocationDataLost =>
                terminalResult == 105,
            ServiceWireConstants.FrameworkErrorCode.RequestRejected
                or ServiceWireConstants.FrameworkErrorCode.WorkerQueueFull
                or ServiceWireConstants.FrameworkErrorCode.ActorCreateRejected =>
                terminalResult == 106,
            ServiceWireConstants.FrameworkErrorCode.ActorAlreadyExists
                or ServiceWireConstants.FrameworkErrorCode.ActorTypeMismatch
                or ServiceWireConstants.FrameworkErrorCode.SpotTypeMismatch
                or ServiceWireConstants.FrameworkErrorCode.ActorLocationStale
                or ServiceWireConstants.FrameworkErrorCode.SpotGenerationStale
                or ServiceWireConstants.FrameworkErrorCode.SpotMoving =>
                terminalResult == 107,
            _ => false
        };
    }

    private static void ValidateCoordinator(RelocationCoordinatorFence fence)
    {
        ArgumentException.ThrowIfNullOrEmpty(fence.OwnerId);
        ArgumentException.ThrowIfNullOrEmpty(fence.ExpectedAuthorityStoreVersion);
        if (fence.LeaseGeneration == 0 || fence.NodeRid.IsEmpty
            || fence.NodeGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(fence));
    }

    private static void ValidateRequestSource(RequestSourceFence fence)
    {
        ArgumentException.ThrowIfNullOrEmpty(fence.OwnerId);
        if (fence.LeaseGeneration == 0 || fence.NodeRid.IsEmpty
            || fence.NodeGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(fence));
    }

    private static void WriteCoordinator(
        WireWriter writer,
        RelocationCoordinatorFence fence)
    {
        writer.Text8(fence.OwnerId);
        writer.U64(fence.LeaseGeneration);
        writer.Rid(fence.NodeRid);
        writer.U64(fence.NodeGeneration);
        writer.Text16(fence.ExpectedAuthorityStoreVersion, requireNonEmpty: true);
    }

    private static void WriteRequestSource(WireWriter writer, RequestSourceFence fence)
    {
        writer.Text8(fence.OwnerId);
        writer.U64(fence.LeaseGeneration);
        writer.Rid(fence.NodeRid);
        writer.U64(fence.NodeGeneration);
    }

    private static bool TryCoordinator(
        ref WireReader reader,
        out RelocationCoordinatorFence fence)
    {
        fence = default;
        if (!reader.TryText8(out var ownerId)
            || !reader.TryU64(out var lease) || lease == 0
            || !reader.TryRid(out var nodeRid)
            || !reader.TryU64(out var generation) || generation == 0
            || !reader.TryText16(out var storeVersion, requireNonEmpty: true))
            return false;
        fence = new RelocationCoordinatorFence(
            ownerId, lease, nodeRid, generation, storeVersion);
        return true;
    }

    private static bool TryRequestSource(
        ref WireReader reader,
        out RequestSourceFence fence)
    {
        fence = default;
        if (!reader.TryText8(out var ownerId)
            || !reader.TryU64(out var lease) || lease == 0
            || !reader.TryRid(out var nodeRid)
            || !reader.TryU64(out var generation) || generation == 0)
            return false;
        fence = new RequestSourceFence(ownerId, lease, nodeRid, generation);
        return true;
    }

    private static bool TryOperation(
        ref WireReader reader,
        out MeshOperationId operation)
    {
        operation = default;
        if (!reader.TryU64(out var high) || !reader.TryU64(out var low)
            || high == 0 && low == 0)
            return false;
        operation = new MeshOperationId(high, low);
        return true;
    }

    private static bool TryRelocationId(
        ref WireReader reader,
        out RelocationWireId relocation)
    {
        relocation = default;
        if (!reader.TryU64(out var high) || !reader.TryU64(out var low)
            || high == 0 && low == 0)
            return false;
        relocation = new RelocationWireId(high, low);
        return true;
    }

    private static bool Fail(ref WireReader reader, out DecodeError error)
    {
        error = reader.Truncated ? DecodeError.TruncatedField : DecodeError.InvalidField;
        return false;
    }
}
