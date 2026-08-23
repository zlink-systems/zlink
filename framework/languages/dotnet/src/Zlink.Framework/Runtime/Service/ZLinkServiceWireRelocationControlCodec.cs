using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Runtime.Service;

internal static partial class ZLinkServiceWireCodec
{
    internal sealed record RelocationTargetRecord(
        RoutingId NodeRid,
        ulong NodeGeneration,
        string OwnerId,
        ulong OwnerLeaseGeneration);

    internal sealed record RelocationObjectRecord(
        byte Kind,
        string StableType,
        string ObjectId,
        ulong ObjectGeneration,
        ulong ExpectedAuthorityOwnerGeneration);

    //  Schema bounds relocationLogicalBytes / relocationChunkCount /
    //  relocationChunkBytes (service-wire-v1.schema.json) — the payload
    //  manifest and state-chunk fields must stay within these limits.
    internal const ulong RelocationLogicalBytesBound = 274_877_906_944;
    internal const uint RelocationChunkCountBound = 4_096;
    internal const uint RelocationChunkBytesBound = 67_108_864;

    internal sealed record RelocationPrepareRecord(
        RelocationWireId RelocationId,
        ulong TargetAttemptGeneration,
        RelocationCoordinatorFence Coordinator,
        RelocationTargetRecord Target,
        byte InitiatorRole,
        RelocationObjectRecord Object,
        RoutingId SourceNodeRid,
        ulong SourceNodeGeneration,
        ulong PayloadTotalLength,
        uint PayloadChunkCount,
        uint PayloadChecksumCrc32c,
        ulong ApplicationVersion);

    internal sealed record RelocationReadyRecord(
        RelocationWireId RelocationId,
        ulong TargetAttemptGeneration,
        RelocationCoordinatorFence Coordinator,
        RelocationTargetRecord Target,
        RelocationObjectRecord Object,
        byte SenderRole);

    internal sealed record RelocationFailedRecord(
        RelocationWireId RelocationId,
        ulong TargetAttemptGeneration,
        RelocationCoordinatorFence Coordinator,
        RelocationTargetRecord Target,
        RelocationObjectRecord Object,
        byte SenderRole,
        ServiceWireConstants.FrameworkErrorCode FailureCode);

    internal sealed record FrozenRelocationControlRecord(
        RequestSourceFence Source,
        MeshOperationId OperationId,
        byte Phase,
        byte Role,
        RelocationWireId RelocationId,
        RelocationObjectRecord Object,
        uint TerminalResult,
        ServiceWireConstants.FrameworkErrorCode FailureCode);

    internal sealed record FrozenRecord(ReadOnlyMemory<byte> Encoded);

    internal sealed record RelocationDataRecord(
        RelocationWireId RelocationId,
        ulong TargetAttemptGeneration,
        RelocationCoordinatorFence Coordinator,
        byte SenderRole,
        RelocationObjectRecord Object,
        FrozenRecord FrozenRecord);

    internal sealed record RelocationCutoverRecord(
        RelocationWireId RelocationId,
        ulong TargetAttemptGeneration,
        RelocationCoordinatorFence Coordinator,
        byte SenderRole,
        RelocationObjectRecord Object,
        ulong BoundaryRecordCount,
        uint BoundaryChecksumCrc32c);

    internal sealed record RelocationStateRecord(
        RelocationWireId RelocationId,
        ulong TargetAttemptGeneration,
        RelocationCoordinatorFence Coordinator,
        byte SenderRole,
        RelocationObjectRecord Object,
        uint ChunkOrdinal,
        ReadOnlyMemory<byte> ChunkData);

    internal static byte[] EncodeRelocationPrepare(RelocationPrepareRecord record)
    {
        ValidateRelocationCommon(record.RelocationId,
            record.TargetAttemptGeneration, record.Coordinator);
        ValidateTarget(record.Target);
        if (record.InitiatorRole != 1)
            throw new ArgumentOutOfRangeException(nameof(record));
        _ = ToGenerated(record.Object);
        if (record.SourceNodeRid.IsEmpty || record.SourceNodeGeneration == 0
            || record.ApplicationVersion > long.MaxValue
            || record.PayloadTotalLength > RelocationLogicalBytesBound
            || record.PayloadChunkCount > RelocationChunkCountBound)
            throw new ArgumentOutOfRangeException(nameof(record));
        return ServiceWirePilotCodec.EncodeRelocationPrepare40(new(
            ToGenerated(record.RelocationId),
            record.TargetAttemptGeneration,
            ToGenerated(record.Coordinator),
            ToGenerated(record.Target),
            ServiceWirePilotCodec.RelocationRole.Source,
            ToGenerated(record.Object),
            record.SourceNodeRid.ToBytes().ToArray(),
            record.SourceNodeGeneration,
            record.PayloadTotalLength,
            record.PayloadChunkCount,
            record.PayloadChecksumCrc32c,
            checked((long)record.ApplicationVersion)));
    }

    internal static byte[] EncodeRelocationReady(RelocationReadyRecord record)
    {
        ValidateRelocationCommon(record.RelocationId,
            record.TargetAttemptGeneration, record.Coordinator);
        ValidateTarget(record.Target);
        if (record.SenderRole != 2)
            throw new ArgumentOutOfRangeException(nameof(record));
        _ = ToGenerated(record.Object);
        return ServiceWirePilotCodec.EncodeRelocationReady30(new(
            ToGenerated(record.RelocationId),
            record.TargetAttemptGeneration,
            ToGenerated(record.Coordinator),
            ToGenerated(record.Target),
            ToGenerated(record.Object),
            ServiceWirePilotCodec.RelocationRole.Target));
    }

    internal static byte[] EncodeRelocationFailed(RelocationFailedRecord record)
    {
        ValidateRelocationCommon(record.RelocationId,
            record.TargetAttemptGeneration, record.Coordinator);
        ValidateTarget(record.Target);
        if (record.SenderRole != 2
            || record.FailureCode == ServiceWireConstants.FrameworkErrorCode.None)
            throw new ArgumentOutOfRangeException(nameof(record));
        _ = ToGenerated(record.Object);
        try
        {
            return ServiceWirePilotCodec.EncodeRelocationFailed53(new(
                ToGenerated(record.RelocationId),
                record.TargetAttemptGeneration,
                ToGenerated(record.Coordinator),
                ToGenerated(record.Target),
                ToGenerated(record.Object),
                ServiceWirePilotCodec.RelocationRole.Target,
                (uint)record.FailureCode));
        }
        catch (InvalidDataException error)
        {
            throw new ArgumentOutOfRangeException(nameof(record), error);
        }
    }

    internal static byte[] EncodeRelocationData(RelocationDataRecord record)
    {
        ValidateRelocationCommon(record.RelocationId,
            record.TargetAttemptGeneration, record.Coordinator);
        if (record.SenderRole != 1)
            throw new ArgumentOutOfRangeException(nameof(record));
        _ = ToGenerated(record.Object);
        if (!ZLinkRelocationEnvelopeCodec.TryValidateCanonicalFrozenRecord(
                record.FrozenRecord.Encoded.Span))
            throw new ArgumentOutOfRangeException(nameof(record));
        return ServiceWirePilotCodec.EncodeRelocationData31(new(
            ToGenerated(record.RelocationId),
            record.TargetAttemptGeneration,
            ToGenerated(record.Coordinator),
            ServiceWirePilotCodec.RelocationRole.Source,
            ToGenerated(record.Object),
            record.FrozenRecord.Encoded.ToArray()));
    }

    internal static byte[] EncodeRelocationCutover(RelocationCutoverRecord record)
    {
        ValidateRelocationCommon(record.RelocationId,
            record.TargetAttemptGeneration, record.Coordinator);
        if (record.SenderRole != 1)
            throw new ArgumentOutOfRangeException(nameof(record));
        _ = ToGenerated(record.Object);
        if (record.BoundaryRecordCount > long.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(record));
        return ServiceWirePilotCodec.EncodeRelocationCutover34(new(
            ToGenerated(record.RelocationId),
            record.TargetAttemptGeneration,
            ToGenerated(record.Coordinator),
            ServiceWirePilotCodec.RelocationRole.Source,
            ToGenerated(record.Object),
            record.BoundaryRecordCount,
            record.BoundaryChecksumCrc32c));
    }

    internal static byte[] EncodeRelocationState(RelocationStateRecord record)
    {
        ValidateRelocationCommon(record.RelocationId,
            record.TargetAttemptGeneration, record.Coordinator);
        if (record.SenderRole != 1)
            throw new ArgumentOutOfRangeException(nameof(record));
        _ = ToGenerated(record.Object);
        if (record.ChunkData.Length > RelocationChunkBytesBound)
            throw new ArgumentOutOfRangeException(nameof(record));
        return ServiceWirePilotCodec.EncodeRelocationState52(new(
            ToGenerated(record.RelocationId),
            record.TargetAttemptGeneration,
            ToGenerated(record.Coordinator),
            ServiceWirePilotCodec.RelocationRole.Source,
            ToGenerated(record.Object),
            record.ChunkOrdinal,
            record.ChunkData.ToArray()));
    }

    internal static bool TryDecodeRelocationPrepare(ReadOnlySpan<byte> bytes,
        out RelocationPrepareRecord record, out DecodeError error)
    {
        record = null!;
        if (!TryDecodeGenerated(bytes,
                ServiceWireConstants.Command.RelocationPrepare,
                ServiceWirePilotCodec.DecodeRelocationPrepare40,
                out var generated, out error))
            return false;
        record = new RelocationPrepareRecord(
            FromGenerated(generated.Relocation),
            generated.TargetAttemptGeneration,
            FromGenerated(generated.Coordinator),
            FromGenerated(generated.Target),
            FromGeneratedRole(generated.InitiatorRole),
            FromGenerated(generated.Object),
            RoutingId.From(generated.SourceNodeRid),
            generated.SourceNodeGeneration,
            generated.PayloadTotalLength,
            generated.PayloadChunkCount,
            generated.PayloadChecksumCrc32c,
            checked((ulong)generated.ApplicationVersion));
        return true;
    }

    internal static bool TryDecodeRelocationReady(ReadOnlySpan<byte> bytes,
        out RelocationReadyRecord record, out DecodeError error)
    {
        record = null!;
        if (!TryDecodeGenerated(bytes,
                ServiceWireConstants.Command.RelocationReady,
                ServiceWirePilotCodec.DecodeRelocationReady30,
                out var generated, out error))
            return false;
        record = new RelocationReadyRecord(
            FromGenerated(generated.Relocation),
            generated.TargetAttemptGeneration,
            FromGenerated(generated.Coordinator),
            FromGenerated(generated.Target),
            FromGenerated(generated.Object),
            FromGeneratedRole(generated.SenderRole));
        return true;
    }

    internal static bool TryDecodeRelocationFailed(ReadOnlySpan<byte> bytes,
        out RelocationFailedRecord record, out DecodeError error)
    {
        record = null!;
        if (!TryDecodeGenerated(bytes,
                ServiceWireConstants.Command.RelocationFailed,
                ServiceWirePilotCodec.DecodeRelocationFailed53,
                out var generated, out error))
            return false;
        record = new RelocationFailedRecord(
            FromGenerated(generated.Relocation),
            generated.TargetAttemptGeneration,
            FromGenerated(generated.Coordinator),
            FromGenerated(generated.Target),
            FromGenerated(generated.Object),
            FromGeneratedRole(generated.SenderRole),
            (ServiceWireConstants.FrameworkErrorCode)generated.FailureCode);
        return true;
    }

    internal static bool TryDecodeRelocationData(ReadOnlySpan<byte> bytes,
        out RelocationDataRecord record, out DecodeError error)
    {
        record = null!;
        if (!TryDecodeGenerated(bytes,
                ServiceWireConstants.Command.RelocationData,
                ServiceWirePilotCodec.DecodeRelocationData31,
                out var generated, out error))
        {
            if (error == DecodeError.TrailingByte)
                error = DecodeError.InvalidField;
            return false;
        }
        record = new RelocationDataRecord(
            FromGenerated(generated.Relocation),
            generated.TargetAttemptGeneration,
            FromGenerated(generated.Coordinator),
            FromGeneratedRole(generated.SenderRole),
            FromGenerated(generated.Object),
            new FrozenRecord(generated.Record));
        return true;
    }

    internal static bool TryDecodeRelocationCutover(ReadOnlySpan<byte> bytes,
        out RelocationCutoverRecord record, out DecodeError error)
    {
        record = null!;
        if (!TryDecodeGenerated(bytes,
                ServiceWireConstants.Command.RelocationCutover,
                ServiceWirePilotCodec.DecodeRelocationCutover34,
                out var generated, out error))
            return false;
        record = new RelocationCutoverRecord(
            FromGenerated(generated.Relocation),
            generated.TargetAttemptGeneration,
            FromGenerated(generated.Coordinator),
            FromGeneratedRole(generated.SenderRole),
            FromGenerated(generated.Object),
            generated.BoundaryRecordCount,
            generated.BoundaryChecksumCrc32c);
        return true;
    }

    internal static bool TryDecodeRelocationState(ReadOnlySpan<byte> bytes,
        out RelocationStateRecord record, out DecodeError error)
    {
        record = null!;
        if (!TryDecodeGenerated(bytes,
                ServiceWireConstants.Command.RelocationState,
                ServiceWirePilotCodec.DecodeRelocationState52,
                out var generated, out error))
            return false;
        record = new RelocationStateRecord(
            FromGenerated(generated.Relocation),
            generated.TargetAttemptGeneration,
            FromGenerated(generated.Coordinator),
            FromGeneratedRole(generated.SenderRole),
            FromGenerated(generated.Object),
            generated.ChunkOrdinal,
            generated.ChunkData);
        return true;
    }

    private static void WriteRelocationId(WireWriter writer, RelocationWireId id)
    {
        if (id.IsEmpty)
            throw new ArgumentOutOfRangeException(nameof(id));
        writer.U64(id.High);
        writer.U64(id.Low);
    }

    private static void WriteRelocationObject(WireWriter writer,
        RelocationObjectRecord value)
    {
        if (value.Kind is < 1 or > 3 || value.ObjectGeneration == 0
            || string.IsNullOrEmpty(value.ObjectId)
            || value.Kind != 3 && value.ExpectedAuthorityOwnerGeneration == 0
            || value.Kind == 3
            && (string.IsNullOrEmpty(value.StableType)
                || value.ExpectedAuthorityOwnerGeneration != 0))
            throw new ArgumentOutOfRangeException(nameof(value));
        var body = new WireWriter();
        if (value.Kind == 3)
        {
            body.Text8(value.StableType);
            body.Text8(value.ObjectId);
            body.U64(value.ObjectGeneration);
        }
        else
        {
            body.Text8(value.ObjectId);
            body.U64(value.ObjectGeneration);
            body.U64(value.ExpectedAuthorityOwnerGeneration);
        }
        writer.U8(value.Kind);
        writer.U16(checked((ushort)body.Count));
        writer.Bytes(body.ToArray());
    }

    private static bool TryRelocationObject(ref WireReader reader,
        out RelocationObjectRecord value)
    {
        value = null!;
        if (!reader.TryU8(out var kind) || kind is < 1 or > 3
            || !reader.TryU16(out var length)
            || !reader.TrySlice(length, out var bytes))
            return false;
        var body = new WireReader(bytes);
        string stable = string.Empty;
        string id;
        ulong generation;
        ulong expected = 0;
        if (kind == 3)
        {
            if (!body.TryText8(out stable) || !body.TryText8(out id)
                || string.IsNullOrEmpty(stable) || string.IsNullOrEmpty(id)
                || !body.TryU64(out generation) || generation == 0)
                return false;
        }
        else if (!body.TryText8(out id)
                 || string.IsNullOrEmpty(id)
                 || !body.TryU64(out generation) || generation == 0
                 || !body.TryU64(out expected) || expected == 0)
        {
            return false;
        }
        if (body.Remaining != 0)
            return false;
        value = new RelocationObjectRecord(kind, stable, id, generation,
            expected);
        return true;
    }

    private static void WriteFrozenRelocationControl(WireWriter writer,
        FrozenRelocationControlRecord record)
    {
        ValidateRequestSource(record.Source);
        if (record.OperationId.High != 0 || record.OperationId.Low != 0
            || record.Phase > 9 || !IsRole(record.Role)
            || !IsTerminalFailureValid(record.TerminalResult,
                record.FailureCode))
            throw new ArgumentOutOfRangeException(nameof(record));
        writer.U8(13);
        var source = new WireWriter();
        source.Rid(record.Source.NodeRid);
        source.U64(record.Source.NodeGeneration);
        source.Text8(record.Source.OwnerId);
        source.U64(record.Source.LeaseGeneration);
        writer.U8(1);
        writer.U16(checked((ushort)source.Count));
        writer.Bytes(source.ToArray());
        writer.U8(0);
        WriteOperationId(writer, record.OperationId);
        writer.U32(0);
        writer.U16(0);
        writer.U8(record.Phase);
        writer.U8(record.Role);
        WriteRelocationId(writer, record.RelocationId);
        WriteRelocationObject(writer, record.Object);
        writer.U32(record.TerminalResult);
        writer.U32((uint)record.FailureCode);
    }

    internal static FrozenRecord EncodeFrozenRelocationControl(
        FrozenRelocationControlRecord record)
    {
        var writer = new WireWriter();
        WriteFrozenRelocationControl(writer, record);
        return new FrozenRecord(writer.ToArray());
    }

    internal static bool TryDecodeFrozenRelocationControl(
        FrozenRecord frozen, out FrozenRelocationControlRecord record)
    {
        var reader = new WireReader(frozen.Encoded.Span);
        return TryFrozenRelocationControl(ref reader, out record)
               && reader.Remaining == 0;
    }

    private static bool TryFrozenRelocationControl(ref WireReader reader,
        out FrozenRelocationControlRecord record)
    {
        record = null!;
        if (!reader.TryU8(out var recordKind) || recordKind != 13
            || !reader.TryU8(out var sourceKind) || sourceKind != 1
            || !reader.TryU16(out var sourceLength)
            || !reader.TrySlice(sourceLength, out var sourceBytes))
            return false;
        var sourceReader = new WireReader(sourceBytes);
        if (!sourceReader.TryRid(out var sourceRid) || sourceRid.IsEmpty
            || !sourceReader.TryU64(out var sourceGeneration)
            || sourceGeneration == 0
            || !sourceReader.TryText8(out var sourceOwner)
            || string.IsNullOrEmpty(sourceOwner)
            || !sourceReader.TryU64(out var sourceLease) || sourceLease == 0
            || sourceReader.Remaining != 0
            || !reader.TryU8(out var metadata) || metadata != 0
            || !reader.TryU64(out var operationHigh)
            || !reader.TryU64(out var operationLow)
            || operationHigh != 0 || operationLow != 0
            || !reader.TryU32(out var operationKind) || operationKind != 0
            || !reader.TryU16(out var replyLength) || replyLength != 0
            || !reader.TryU8(out var phase) || phase > 9
            || !reader.TryU8(out var role) || !IsRole(role)
            || !TryRelocationId(ref reader, out var id)
            || !TryRelocationObject(ref reader, out var relocationObject)
            || !reader.TryU32(out var terminal)
            || !reader.TryU32(out var failureValue)
            || !IsTerminalFailureValid(terminal,
                (ServiceWireConstants.FrameworkErrorCode)failureValue))
            return false;
        record = new FrozenRelocationControlRecord(
            new RequestSourceFence(sourceOwner, sourceLease, sourceRid,
                sourceGeneration),
            new MeshOperationId(operationHigh, operationLow), phase, role, id,
            relocationObject, terminal,
            (ServiceWireConstants.FrameworkErrorCode)failureValue);
        return true;
    }

    private static bool End(ref WireReader reader, out DecodeError error)
    {
        if (reader.Remaining != 0)
        {
            error = DecodeError.TrailingByte;
            return false;
        }
        error = DecodeError.None;
        return true;
    }

    private static bool DecodeFailure(ref WireReader reader,
        out DecodeError error)
    {
        error = reader.Truncated
            ? DecodeError.TruncatedField
            : DecodeError.InvalidField;
        return false;
    }

    private static void ValidateRelocationCommon(RelocationWireId id,
        ulong attempt, RelocationCoordinatorFence coordinator)
    {
        if (id.IsEmpty || attempt == 0)
            throw new ArgumentOutOfRangeException(nameof(id));
        ValidateCoordinator(coordinator);
    }

    private static void ValidateTarget(RelocationTargetRecord target)
    {
        ArgumentNullException.ThrowIfNull(target);
        ArgumentException.ThrowIfNullOrEmpty(target.OwnerId);
        if (target.NodeRid.IsEmpty || target.NodeGeneration == 0
            || target.OwnerLeaseGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(target));
    }

    private static bool IsRole(byte role) => role is >= 1 and <= 3;
}
