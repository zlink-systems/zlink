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

        return ServiceWirePilotCodec.EncodeReplyRelay33(new(
            ToGenerated(record.OperationId),
            record.ReplyRouteId,
            new ServiceWirePilotCodec.MaintenanceRelocationReplyContext(
                ToGenerated(record.RelocationId),
                record.TargetAttemptGeneration,
                ToGenerated(record.Coordinator),
                record.ParticipantId,
                record.Sequence),
            record.TerminalResult,
            (uint)record.FailureCode)).Single();
    }

    internal static bool TryDecodeReplyRelay(
        ReadOnlySpan<byte> bytes,
        out ReplyRelayRecord record,
        out DecodeError error)
    {
        record = default;
        if (!TryDecodeGenerated(
                bytes,
                ServiceWireConstants.Command.ReplyRelay,
                static value => ServiceWirePilotCodec.DecodeReplyRelay33([value]),
                out var generated,
                out error))
            return false;
        if (generated.Context is not
            ServiceWirePilotCodec.MaintenanceRelocationReplyContext context
            || generated.Payload is not null)
        {
            error = DecodeError.InvalidField;
            return false;
        }
        record = new ReplyRelayRecord(
            FromGenerated(generated.Operation),
            generated.ReplyRouteId,
            FromGenerated(context.Relocation),
            context.TargetAttemptGeneration,
            FromGenerated(context.Coordinator),
            context.ParticipantId,
            context.Sequence,
            generated.TerminalResult,
            (ServiceWireConstants.FrameworkErrorCode)generated.FailureCode);
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

        return ServiceWirePilotCodec.EncodeReplyRelayAck46(new(
            ToGenerated(record.RelocationId),
            ToGenerated(record.Coordinator),
            ToGenerated(record.OperationId),
            record.ReplyRouteId,
            ToGenerated(record.RequestSource),
            record.Status));
    }

    internal static bool TryDecodeReplyRelayAck(
        ReadOnlySpan<byte> bytes,
        out ReplyRelayAckRecord record,
        out DecodeError error)
    {
        record = default;
        if (!TryDecodeGenerated(
                bytes,
                ServiceWireConstants.Command.ReplyRelayAck,
                ServiceWirePilotCodec.DecodeReplyRelayAck46,
                out var generated,
                out error))
            return false;
        record = new ReplyRelayAckRecord(
            FromGenerated(generated.Relocation),
            FromGenerated(generated.Coordinator),
            FromGenerated(generated.Operation),
            generated.ReplyRouteId,
            FromGenerated(generated.RequestSource),
            generated.Status);
        return true;
    }

    private static ServiceWirePilotCodec.OperationId ToGenerated(
        MeshOperationId value) => new(value.High, value.Low);

    private static MeshOperationId FromGenerated(
        ServiceWirePilotCodec.OperationId value) => new(value.High, value.Low);

    private static ServiceWirePilotCodec.RelocationId ToGenerated(
        RelocationWireId value) => new(value.High, value.Low);

    private static RelocationWireId FromGenerated(
        ServiceWirePilotCodec.RelocationId value) => new(value.High, value.Low);

    private static ServiceWirePilotCodec.CoordinatorFence ToGenerated(
        RelocationCoordinatorFence value) => new(
        value.OwnerId,
        value.LeaseGeneration,
        value.NodeRid.ToBytes().ToArray(),
        value.NodeGeneration,
        value.ExpectedAuthorityStoreVersion);

    private static RelocationCoordinatorFence FromGenerated(
        ServiceWirePilotCodec.CoordinatorFence value) => new(
        value.CoordinatorOwnerId,
        value.CoordinatorLeaseGeneration,
        RoutingId.From(value.CoordinatorNodeRid),
        value.CoordinatorNodeGeneration,
        value.ExpectedAuthorityStoreVersion);

    private static ServiceWirePilotCodec.RequestSourceFence ToGenerated(
        RequestSourceFence value) => new(
        value.OwnerId,
        value.LeaseGeneration,
        value.NodeRid.ToBytes().ToArray(),
        value.NodeGeneration);

    private static RequestSourceFence FromGenerated(
        ServiceWirePilotCodec.RequestSourceFence value) => new(
        value.SourceOwnerId,
        value.SourceOwnerLeaseGeneration,
        RoutingId.From(value.SourceNodeRid),
        value.SourceNodeGeneration);

    private static ServiceWirePilotCodec.TargetFence ToGenerated(
        RelocationTargetRecord value) => new(
        value.NodeRid.ToBytes().ToArray(),
        value.NodeGeneration,
        value.OwnerId,
        value.OwnerLeaseGeneration);

    private static RelocationTargetRecord FromGenerated(
        ServiceWirePilotCodec.TargetFence value) => new(
        RoutingId.From(value.TargetNodeRid),
        value.TargetNodeGeneration,
        value.TargetOwnerId,
        value.TargetOwnerLeaseGeneration);

    private static ServiceWirePilotCodec.RelocationObjectIdentity ToGenerated(
        RelocationObjectRecord value)
    {
        if (value.Kind is < 1 or > 3 || value.ObjectGeneration == 0
            || string.IsNullOrEmpty(value.ObjectId)
            || value.Kind != 3 && value.ExpectedAuthorityOwnerGeneration == 0
            || value.Kind == 3
            && (string.IsNullOrEmpty(value.StableType)
                || value.ExpectedAuthorityOwnerGeneration != 0))
            throw new ArgumentOutOfRangeException(nameof(value));
        return value.Kind switch
        {
            1 => new ServiceWirePilotCodec.RelocationActorIdentity(
                value.ObjectId,
                value.ObjectGeneration,
                value.ExpectedAuthorityOwnerGeneration),
            2 => new ServiceWirePilotCodec.RelocationUserSpotIdentity(
                value.ObjectId,
                value.ObjectGeneration,
                value.ExpectedAuthorityOwnerGeneration),
            3 => new ServiceWirePilotCodec.RelocationInstanceSpotIdentity(
                value.StableType,
                value.ObjectId,
                value.ObjectGeneration),
            _ => throw new ArgumentOutOfRangeException(nameof(value))
        };
    }

    private static RelocationObjectRecord FromGenerated(
        ServiceWirePilotCodec.RelocationObjectIdentity value) => value switch
    {
        ServiceWirePilotCodec.RelocationActorIdentity actor => new(
            1,
            string.Empty,
            actor.ActorId,
            actor.ObjectGeneration,
            actor.ExpectedAuthorityOwnerGeneration),
        ServiceWirePilotCodec.RelocationUserSpotIdentity spot => new(
            2,
            string.Empty,
            spot.SpotId,
            spot.ObjectGeneration,
            spot.ExpectedAuthorityOwnerGeneration),
        ServiceWirePilotCodec.RelocationInstanceSpotIdentity spot => new(
            3,
            spot.InstanceType,
            spot.SpotId,
            spot.ObjectGeneration,
            0),
        _ => throw new InvalidDataException("Unknown relocation object identity.")
    };

    private static ServiceWirePilotCodec.RelocationRole ToGeneratedRole(
        byte value) => value switch
    {
        1 => ServiceWirePilotCodec.RelocationRole.Source,
        2 => ServiceWirePilotCodec.RelocationRole.Target,
        3 => ServiceWirePilotCodec.RelocationRole.Coordinator,
        _ => throw new ArgumentOutOfRangeException(nameof(value))
    };

    private static byte FromGeneratedRole(
        ServiceWirePilotCodec.RelocationRole value) => value switch
    {
        ServiceWirePilotCodec.RelocationRole.Source => 1,
        ServiceWirePilotCodec.RelocationRole.Target => 2,
        ServiceWirePilotCodec.RelocationRole.Coordinator => 3,
        _ => throw new InvalidDataException("Unknown relocation role.")
    };

    private static void ValidateOperation(MeshOperationId operation)
    {
        if (operation.High == 0 && operation.Low == 0)
            throw new ArgumentOutOfRangeException(nameof(operation));
    }

    private static bool IsTerminalFailureValid(
        uint terminalResult,
        ServiceWireConstants.FrameworkErrorCode failureCode) =>
        ServiceWireConstants.ValidTerminalFailure(
            terminalResult, (uint)failureCode);

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
}
