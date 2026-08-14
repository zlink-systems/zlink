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

    internal sealed record RelocationRootRecord(
        string Reference,
        uint ChecksumCrc32c);

    internal sealed record RelocationPrepareRecord(
        RelocationWireId RelocationId,
        ulong TargetAttemptGeneration,
        RelocationCoordinatorFence Coordinator,
        RelocationTargetRecord Target,
        byte InitiatorRole,
        RelocationObjectRecord Object,
        RoutingId SourceNodeRid,
        ulong SourceNodeGeneration,
        RelocationRootRecord? Root,
        ulong ApplicationVersion);

    internal sealed record RelocationReadyRecord(
        RelocationWireId RelocationId,
        ulong TargetAttemptGeneration,
        RelocationCoordinatorFence Coordinator,
        RelocationTargetRecord Target,
        RelocationObjectRecord Object,
        byte SenderRole);

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
        RelocationObjectRecord Object);

    internal static byte[] EncodeRelocationPrepare(RelocationPrepareRecord record)
    {
        ValidateRelocationCommon(record.RelocationId,
            record.TargetAttemptGeneration, record.Coordinator);
        ValidateTarget(record.Target);
        ValidateRole(record.InitiatorRole);
        if (record.SourceNodeRid.IsEmpty || record.SourceNodeGeneration == 0
            || record.ApplicationVersion > long.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(record));

        var body = new WireWriter();
        WriteRelocationId(body, record.RelocationId);
        body.U64(record.TargetAttemptGeneration);
        WriteCoordinator(body, record.Coordinator);
        WriteTarget(body, record.Target);
        body.U8(record.InitiatorRole);
        WriteRelocationObject(body, record.Object);
        body.Rid(record.SourceNodeRid);
        body.U64(record.SourceNodeGeneration);
        WriteRoot(body, record.Root);
        body.U64(record.ApplicationVersion);
        return Finish(ServiceWireConstants.Command.RelocationPrepare,
            ServiceWireConstants.Flag.None, body);
    }

    internal static byte[] EncodeRelocationReady(RelocationReadyRecord record)
    {
        ValidateRelocationCommon(record.RelocationId,
            record.TargetAttemptGeneration, record.Coordinator);
        ValidateTarget(record.Target);
        ValidateRole(record.SenderRole);

        var body = new WireWriter();
        WriteRelocationId(body, record.RelocationId);
        body.U64(record.TargetAttemptGeneration);
        WriteCoordinator(body, record.Coordinator);
        WriteTarget(body, record.Target);
        WriteRelocationObject(body, record.Object);
        body.U8(record.SenderRole);
        return Finish(ServiceWireConstants.Command.RelocationReady,
            ServiceWireConstants.Flag.None, body);
    }

    internal static byte[] EncodeRelocationData(RelocationDataRecord record)
    {
        ValidateRelocationCommon(record.RelocationId,
            record.TargetAttemptGeneration, record.Coordinator);
        ValidateRole(record.SenderRole);
        if (!ZLinkRelocationEnvelopeCodec.TryValidateCanonicalFrozenRecord(
                record.FrozenRecord.Encoded.Span))
            throw new ArgumentOutOfRangeException(nameof(record));

        var body = new WireWriter();
        WriteRelocationId(body, record.RelocationId);
        body.U64(record.TargetAttemptGeneration);
        WriteCoordinator(body, record.Coordinator);
        body.U8(record.SenderRole);
        WriteRelocationObject(body, record.Object);
        body.Bytes(record.FrozenRecord.Encoded.Span);
        return Finish(ServiceWireConstants.Command.RelocationData,
            ServiceWireConstants.Flag.None, body);
    }

    internal static byte[] EncodeRelocationCutover(RelocationCutoverRecord record)
    {
        ValidateRelocationCommon(record.RelocationId,
            record.TargetAttemptGeneration, record.Coordinator);
        ValidateRole(record.SenderRole);

        var body = new WireWriter();
        WriteRelocationId(body, record.RelocationId);
        body.U64(record.TargetAttemptGeneration);
        WriteCoordinator(body, record.Coordinator);
        body.U8(record.SenderRole);
        WriteRelocationObject(body, record.Object);
        return Finish(ServiceWireConstants.Command.RelocationCutover,
            ServiceWireConstants.Flag.None, body);
    }

    internal static bool TryDecodeRelocationPrepare(ReadOnlySpan<byte> bytes,
        out RelocationPrepareRecord record, out DecodeError error)
    {
        record = null!;
        if (!Begin(bytes, ServiceWireConstants.Command.RelocationPrepare,
                ServiceWireConstants.Flag.None, out var reader, out error))
            return false;
        if (!TryRelocationId(ref reader, out var id)
            || !reader.TryU64(out var attempt) || attempt == 0
            || !TryCoordinator(ref reader, out var coordinator)
            || !TryTarget(ref reader, out var target)
            || !reader.TryU8(out var role) || !IsRole(role)
            || !TryRelocationObject(ref reader, out var relocationObject)
            || !reader.TryRid(out var sourceRid)
            || !reader.TryU64(out var sourceGeneration) || sourceGeneration == 0
            || !TryRoot(ref reader, out var root)
            || !reader.TryU64(out var applicationVersion)
            || applicationVersion > long.MaxValue)
            return DecodeFailure(ref reader, out error);
        record = new RelocationPrepareRecord(id, attempt, coordinator, target,
            role, relocationObject, sourceRid, sourceGeneration, root,
            applicationVersion);
        return End(ref reader, out error);
    }

    internal static bool TryDecodeRelocationReady(ReadOnlySpan<byte> bytes,
        out RelocationReadyRecord record, out DecodeError error)
    {
        record = null!;
        if (!Begin(bytes, ServiceWireConstants.Command.RelocationReady,
                ServiceWireConstants.Flag.None, out var reader, out error))
            return false;
        if (!TryRelocationId(ref reader, out var id)
            || !reader.TryU64(out var attempt) || attempt == 0
            || !TryCoordinator(ref reader, out var coordinator)
            || !TryTarget(ref reader, out var target)
            || !TryRelocationObject(ref reader, out var relocationObject)
            || !reader.TryU8(out var role) || !IsRole(role))
            return DecodeFailure(ref reader, out error);
        record = new RelocationReadyRecord(id, attempt, coordinator, target,
            relocationObject, role);
        return End(ref reader, out error);
    }

    internal static bool TryDecodeRelocationData(ReadOnlySpan<byte> bytes,
        out RelocationDataRecord record, out DecodeError error)
    {
        record = null!;
        if (!Begin(bytes, ServiceWireConstants.Command.RelocationData,
                ServiceWireConstants.Flag.None, out var reader, out error))
            return false;
        if (!TryRelocationId(ref reader, out var id)
            || !reader.TryU64(out var attempt) || attempt == 0
            || !TryCoordinator(ref reader, out var coordinator)
            || !reader.TryU8(out var role) || !IsRole(role)
            || !TryRelocationObject(ref reader, out var relocationObject)
            || !reader.TrySlice(reader.Remaining, out var frozenBytes))
            return DecodeFailure(ref reader, out error);
        if (!ZLinkRelocationEnvelopeCodec.TryValidateCanonicalFrozenRecord(
                frozenBytes, out var frozenTruncated))
        {
            if (frozenTruncated)
                reader.MarkTruncated();
            return DecodeFailure(ref reader, out error);
        }
        record = new RelocationDataRecord(id, attempt, coordinator, role,
            relocationObject, new FrozenRecord(frozenBytes.ToArray()));
        return End(ref reader, out error);
    }

    internal static bool TryDecodeRelocationCutover(ReadOnlySpan<byte> bytes,
        out RelocationCutoverRecord record, out DecodeError error)
    {
        record = null!;
        if (!Begin(bytes, ServiceWireConstants.Command.RelocationCutover,
                ServiceWireConstants.Flag.None, out var reader, out error))
            return false;
        if (!TryRelocationId(ref reader, out var id)
            || !reader.TryU64(out var attempt) || attempt == 0
            || !TryCoordinator(ref reader, out var coordinator)
            || !reader.TryU8(out var role) || !IsRole(role)
            || !TryRelocationObject(ref reader, out var relocationObject))
            return DecodeFailure(ref reader, out error);
        record = new RelocationCutoverRecord(id, attempt, coordinator, role,
            relocationObject);
        return End(ref reader, out error);
    }

    private static void WriteRelocationId(WireWriter writer, RelocationWireId id)
    {
        if (id.IsEmpty)
            throw new ArgumentOutOfRangeException(nameof(id));
        writer.U64(id.High);
        writer.U64(id.Low);
    }

    private static void WriteTarget(WireWriter writer,
        RelocationTargetRecord target)
    {
        ValidateTarget(target);
        writer.Rid(target.NodeRid);
        writer.U64(target.NodeGeneration);
        writer.Text8(target.OwnerId);
        writer.U64(target.OwnerLeaseGeneration);
    }

    private static bool TryTarget(ref WireReader reader,
        out RelocationTargetRecord target)
    {
        target = null!;
        if (!reader.TryRid(out var rid)
            || !reader.TryU64(out var generation) || generation == 0
            || !reader.TryText8(out var owner)
            || string.IsNullOrEmpty(owner)
            || !reader.TryU64(out var lease) || lease == 0)
            return false;
        target = new RelocationTargetRecord(rid, generation, owner, lease);
        return true;
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

    private static void WriteRoot(WireWriter writer, RelocationRootRecord? root)
    {
        var body = new WireWriter();
        if (root is not null)
        {
            body.Text16(root.Reference, requireNonEmpty: true);
            body.U32(root.ChecksumCrc32c);
        }
        writer.U8(root is null ? (byte)0 : (byte)1);
        writer.U16(checked((ushort)body.Count));
        writer.Bytes(body.ToArray());
    }

    private static bool TryRoot(ref WireReader reader,
        out RelocationRootRecord? root)
    {
        root = null;
        if (!reader.TryU8(out var has) || has > 1
            || !reader.TryU16(out var length)
            || !reader.TrySlice(length, out var bytes))
            return false;
        var body = new WireReader(bytes);
        if (has == 1)
        {
            if (!body.TryText16(out var reference, requireNonEmpty: true)
                || !body.TryU32(out var checksum))
                return false;
            root = new RelocationRootRecord(reference, checksum);
        }
        return body.Remaining == 0 && (has == 1 || length == 0);
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

    private static bool Begin(ReadOnlySpan<byte> bytes,
        ServiceWireConstants.Command expectedCommand,
        ServiceWireConstants.Flag expectedFlags, out WireReader reader,
        out DecodeError error)
    {
        reader = default;
        if (!TryDecodePrefix(bytes, out var command, out var flags, out error))
            return false;
        if (command != expectedCommand)
        {
            error = DecodeError.UnknownCommand;
            return false;
        }
        if (flags != expectedFlags)
        {
            error = DecodeError.ForbiddenFlag;
            return false;
        }
        reader = new WireReader(bytes[5..]);
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

    private static byte[] Finish(ServiceWireConstants.Command command,
        ServiceWireConstants.Flag flags, WireWriter body)
    {
        var result = Prefix(command, flags, body.Count);
        body.CopyTo(result.AsSpan(5));
        return result;
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

    private static void ValidateRole(byte role)
    {
        if (!IsRole(role))
            throw new ArgumentOutOfRangeException(nameof(role));
    }

    private static bool IsRole(byte role) => role is >= 1 and <= 3;
}
