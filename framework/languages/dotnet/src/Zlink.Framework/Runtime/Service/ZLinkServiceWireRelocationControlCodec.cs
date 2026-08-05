using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Runtime.Service;

internal static partial class ZLinkServiceWireCodec
{
    private const int MaxRelocationParticipants = 2_048;

    internal sealed record RelocationCandidateRecord(
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

    internal sealed record RelocationParticipantRecord(
        ulong ParticipantId,
        byte Kind,
        RoutingId SessionOwnerNodeRid,
        ulong SessionOwnerNodeGeneration,
        string? SessionOwnerId,
        ulong SessionOwnerLeaseGeneration,
        RoutingId SessionRid,
        ulong BindingGeneration,
        ulong AllowanceMessages,
        ulong AllowanceBytes);

    internal sealed record RelocationParticipantProgressRecord(
        ulong ParticipantId,
        ulong AcceptedBoundary,
        ulong ReplayCursor);

    internal sealed record RelocationParticipantTerminalRecord(
        ulong ParticipantId,
        ulong HighWater);

    internal sealed record RelocationRootRecord(
        string Reference,
        uint ChecksumCrc32c);

    internal sealed record RelocationPrepareRecord(
        RelocationWireId RelocationId,
        ulong TargetAttemptGeneration,
        byte RoundKind,
        RelocationCoordinatorFence Coordinator,
        RelocationCandidateRecord Candidate,
        byte InitiatorRole,
        RelocationObjectRecord Object,
        RoutingId SourceNodeRid,
        ulong SourceNodeGeneration,
        ulong RequiredMessages,
        ulong RequiredBytes,
        IReadOnlyList<RelocationParticipantRecord> Participants,
        RelocationRootRecord? Root,
        ulong ApplicationVersion);

    internal sealed record RelocationReadyRecord(
        RelocationWireId RelocationId,
        ulong TargetAttemptGeneration,
        byte RoundKind,
        RelocationCoordinatorFence Coordinator,
        RelocationCandidateRecord Candidate,
        RelocationObjectRecord Object,
        byte Role,
        ulong OfferedMessages,
        ulong OfferedBytes,
        IReadOnlyList<RelocationParticipantRecord> Participants,
        ulong SourceNodeGeneration,
        ulong TargetNodeGeneration,
        ulong ReservationGeneration,
        RelocationRootRecord? Root,
        ulong ApplicationVersion,
        IReadOnlyList<RelocationParticipantProgressRecord> ParticipantProgress);

    internal sealed record RelocationReservedRecord(
        RelocationWireId RelocationId,
        ulong TargetAttemptGeneration,
        byte RoundKind,
        RelocationCoordinatorFence Coordinator,
        RelocationCandidateRecord Candidate,
        ulong ReservationGeneration,
        IReadOnlyList<RelocationParticipantRecord> Participants);

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
        ulong ParticipantId,
        ulong Sequence,
        FrozenRecord FrozenRecord);

    internal sealed record RelocationAckRecord(
        RelocationWireId RelocationId,
        ulong TargetAttemptGeneration,
        RelocationCoordinatorFence Coordinator,
        byte SenderRole,
        ulong ParticipantId,
        ulong HighWater);

    internal sealed record RelocationSealRecord(
        RelocationWireId RelocationId,
        ulong TargetAttemptGeneration,
        RelocationCoordinatorFence Coordinator,
        byte SenderRole,
        bool Response,
        IReadOnlyList<RelocationParticipantTerminalRecord> Participants);

    internal sealed record RelocationCompleteRecord(
        RelocationWireId RelocationId,
        ulong TargetAttemptGeneration,
        RelocationCoordinatorFence Coordinator,
        byte SenderRole,
        RequestSourceFence Source,
        byte SourceCleanupState);

    internal static byte[] EncodeRelocationPrepare(RelocationPrepareRecord record)
    {
        ValidateRelocationCommon(record.RelocationId, record.TargetAttemptGeneration,
            record.Coordinator);
        ValidateRound(record.RoundKind);
        ValidateCandidate(record.Candidate);
        ValidateRole(record.InitiatorRole);
        if (record.SourceNodeRid.IsEmpty || record.SourceNodeGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(record));
        var body = new WireWriter();
        WriteRelocationId(body, record.RelocationId);
        body.U64(record.TargetAttemptGeneration);
        body.U8(record.RoundKind);
        WriteCoordinator(body, record.Coordinator);
        WriteCandidate(body, record.Candidate);
        body.U8(record.InitiatorRole);
        WriteRelocationObject(body, record.Object);
        body.Rid(record.SourceNodeRid);
        body.U64(record.SourceNodeGeneration);
        body.U64(record.RequiredMessages);
        body.U64(record.RequiredBytes);
        WriteParticipants(body, record.Participants);
        WriteRoot(body, record.Root);
        body.U64(record.ApplicationVersion);
        return Finish(ServiceWireConstants.Command.RelocationPrepare,
            ServiceWireConstants.Flag.None, body);
    }

    internal static byte[] EncodeRelocationReady(RelocationReadyRecord record)
    {
        ValidateRelocationCommon(record.RelocationId, record.TargetAttemptGeneration,
            record.Coordinator);
        ValidateRound(record.RoundKind);
        ValidateCandidate(record.Candidate);
        ValidateRole(record.Role);
        if (record.SourceNodeGeneration == 0 || record.TargetNodeGeneration == 0
            || record.ReservationGeneration == 0
            || !IsReadyRoleShape(record.Role, record.OfferedMessages,
                record.OfferedBytes, record.Participants.Count))
            throw new ArgumentOutOfRangeException(nameof(record));
        var body = new WireWriter();
        WriteRelocationId(body, record.RelocationId);
        body.U64(record.TargetAttemptGeneration);
        body.U8(record.RoundKind);
        WriteCoordinator(body, record.Coordinator);
        WriteCandidate(body, record.Candidate);
        WriteRelocationObject(body, record.Object);
        body.U8(record.Role);
        body.U64(record.OfferedMessages);
        body.U64(record.OfferedBytes);
        WriteParticipants(body, record.Participants);
        var extension = new WireWriter();
        extension.Tlv(2, U64Bytes(record.SourceNodeGeneration));
        extension.Tlv(3, U64Bytes(record.TargetNodeGeneration));
        extension.Tlv(4, U64Bytes(record.ReservationGeneration));
        if (record.Root is { } root)
        {
            var reference = new WireWriter();
            reference.Text16(root.Reference, requireNonEmpty: true);
            extension.Tlv(5, reference.ToArray());
            extension.Tlv(6, U32Bytes(root.ChecksumCrc32c));
        }
        extension.Tlv(8, U64Bytes(record.ApplicationVersion));
        var progress = new WireWriter();
        WriteParticipantProgress(progress, record.ParticipantProgress);
        extension.Tlv(9, progress.ToArray());
        body.U32(checked((uint)extension.Count));
        body.Bytes(extension.ToArray());
        return Finish(ServiceWireConstants.Command.RelocationReady,
            ServiceWireConstants.Flag.Extension, body);
    }

    internal static byte[] EncodeRelocationReserved(RelocationReservedRecord record)
    {
        ValidateRelocationCommon(record.RelocationId, record.TargetAttemptGeneration,
            record.Coordinator);
        ValidateRound(record.RoundKind);
        ValidateCandidate(record.Candidate);
        if (record.ReservationGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(record));
        var body = new WireWriter();
        WriteRelocationId(body, record.RelocationId);
        body.U64(record.TargetAttemptGeneration);
        body.U8(record.RoundKind);
        WriteCoordinator(body, record.Coordinator);
        WriteCandidate(body, record.Candidate);
        body.U64(record.ReservationGeneration);
        WriteParticipants(body, record.Participants);
        return Finish(ServiceWireConstants.Command.RelocationReserved,
            ServiceWireConstants.Flag.None, body);
    }

    internal static byte[] EncodeRelocationData(RelocationDataRecord record)
    {
        ValidateRelocationCommon(record.RelocationId, record.TargetAttemptGeneration,
            record.Coordinator);
        ValidateRole(record.SenderRole);
        if (record.ParticipantId == 0 || record.Sequence == 0)
            throw new ArgumentOutOfRangeException(nameof(record));
        var body = new WireWriter();
        WriteRelocationId(body, record.RelocationId);
        body.U64(record.TargetAttemptGeneration);
        WriteCoordinator(body, record.Coordinator);
        body.U8(record.SenderRole);
        body.U64(record.ParticipantId);
        body.U64(record.Sequence);
        if (!ZLinkRelocationEnvelopeCodec.TryValidateCanonicalFrozenRecord(
                record.FrozenRecord.Encoded.Span))
            throw new ArgumentOutOfRangeException(nameof(record));
        body.Bytes(record.FrozenRecord.Encoded.Span);
        return Finish(ServiceWireConstants.Command.RelocationData,
            ServiceWireConstants.Flag.None, body);
    }

    internal static byte[] EncodeRelocationAck(RelocationAckRecord record)
    {
        ValidateRelocationCommon(record.RelocationId, record.TargetAttemptGeneration,
            record.Coordinator);
        ValidateRole(record.SenderRole);
        if (record.ParticipantId == 0)
            throw new ArgumentOutOfRangeException(nameof(record));
        var body = new WireWriter();
        WriteRelocationId(body, record.RelocationId);
        body.U64(record.TargetAttemptGeneration);
        WriteCoordinator(body, record.Coordinator);
        body.U8(record.SenderRole);
        body.U64(record.ParticipantId);
        body.U64(record.HighWater);
        return Finish(ServiceWireConstants.Command.RelocationAck,
            ServiceWireConstants.Flag.None, body);
    }

    internal static byte[] EncodeRelocationSeal(RelocationSealRecord record)
    {
        ValidateRelocationCommon(record.RelocationId, record.TargetAttemptGeneration,
            record.Coordinator);
        ValidateRole(record.SenderRole);
        var body = new WireWriter();
        WriteRelocationId(body, record.RelocationId);
        body.U64(record.TargetAttemptGeneration);
        WriteCoordinator(body, record.Coordinator);
        body.U8(record.SenderRole);
        body.U8(record.Response ? (byte)1 : (byte)0);
        WriteParticipantTerminals(body, record.Participants);
        return Finish(ServiceWireConstants.Command.RelocationSeal,
            ServiceWireConstants.Flag.None, body);
    }

    internal static byte[] EncodeRelocationComplete(RelocationCompleteRecord record)
    {
        ValidateRelocationCommon(record.RelocationId, record.TargetAttemptGeneration,
            record.Coordinator);
        ValidateRole(record.SenderRole);
        ValidateRequestSource(record.Source);
        if (record.SourceCleanupState > 2)
            throw new ArgumentOutOfRangeException(nameof(record));
        var body = new WireWriter();
        WriteRelocationId(body, record.RelocationId);
        body.U64(record.TargetAttemptGeneration);
        WriteCoordinator(body, record.Coordinator);
        body.U8(record.SenderRole);
        WriteRequestSource(body, record.Source);
        body.U8(record.SourceCleanupState);
        return Finish(ServiceWireConstants.Command.RelocationComplete,
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
            || !reader.TryU8(out var round) || !IsRound(round)
            || !TryCoordinator(ref reader, out var coordinator)
            || !TryCandidate(ref reader, out var candidate)
            || !reader.TryU8(out var role) || !IsRole(role)
            || !TryRelocationObject(ref reader, out var relocationObject)
            || !reader.TryRid(out var sourceRid)
            || !reader.TryU64(out var sourceGeneration) || sourceGeneration == 0
            || !reader.TryU64(out var requiredMessages)
            || !reader.TryU64(out var requiredBytes)
            || !TryParticipants(ref reader, out var participants)
            || !TryRoot(ref reader, out var root)
            || !reader.TryU64(out var applicationVersion))
            return DecodeFailure(ref reader, out error);
        record = new RelocationPrepareRecord(id, attempt, round, coordinator,
            candidate, role, relocationObject, sourceRid, sourceGeneration,
            requiredMessages, requiredBytes, participants, root,
            applicationVersion);
        return End(ref reader, out error);
    }

    internal static bool TryDecodeRelocationReady(ReadOnlySpan<byte> bytes,
        out RelocationReadyRecord record, out DecodeError error)
    {
        record = null!;
        if (!Begin(bytes, ServiceWireConstants.Command.RelocationReady,
                ServiceWireConstants.Flag.Extension, out var reader, out error))
            return false;
        if (!TryRelocationId(ref reader, out var id)
            || !reader.TryU64(out var attempt) || attempt == 0
            || !reader.TryU8(out var round) || !IsRound(round)
            || !TryCoordinator(ref reader, out var coordinator)
            || !TryCandidate(ref reader, out var candidate)
            || !TryRelocationObject(ref reader, out var relocationObject)
            || !reader.TryU8(out var role) || !IsRole(role)
            || !reader.TryU64(out var offeredMessages)
            || !reader.TryU64(out var offeredBytes)
            || !TryParticipants(ref reader, out var participants)
            || !IsReadyRoleShape(role, offeredMessages, offeredBytes,
                participants.Count)
            || !TryReadyExtension(ref reader, out var sourceGeneration,
                out var targetGeneration, out var reservationGeneration,
                out var root, out var applicationVersion, out var progress))
            return DecodeFailure(ref reader, out error);
        record = new RelocationReadyRecord(id, attempt, round, coordinator,
            candidate, relocationObject, role, offeredMessages, offeredBytes,
            participants, sourceGeneration, targetGeneration,
            reservationGeneration, root, applicationVersion, progress);
        return End(ref reader, out error);
    }

    private static bool IsReadyRoleShape(byte role, ulong offeredMessages,
        ulong offeredBytes, int participantCount)
        => role == 2
            ? offeredMessages != 0 && offeredBytes != 0 && participantCount == 0
            : role == 1 && offeredMessages == 0 && offeredBytes == 0
                && participantCount != 0;

    internal static bool TryDecodeRelocationReserved(ReadOnlySpan<byte> bytes,
        out RelocationReservedRecord record, out DecodeError error)
    {
        record = null!;
        if (!Begin(bytes, ServiceWireConstants.Command.RelocationReserved,
                ServiceWireConstants.Flag.None, out var reader, out error))
            return false;
        if (!TryRelocationId(ref reader, out var id)
            || !reader.TryU64(out var attempt) || attempt == 0
            || !reader.TryU8(out var round) || !IsRound(round)
            || !TryCoordinator(ref reader, out var coordinator)
            || !TryCandidate(ref reader, out var candidate)
            || !reader.TryU64(out var generation) || generation == 0
            || !TryParticipants(ref reader, out var participants))
            return DecodeFailure(ref reader, out error);
        record = new RelocationReservedRecord(id, attempt, round, coordinator,
            candidate, generation, participants);
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
            || !reader.TryU64(out var participant) || participant == 0
            || !reader.TryU64(out var sequence) || sequence == 0
            || !reader.TrySlice(reader.Remaining, out var frozenBytes))
            return DecodeFailure(ref reader, out error);
        if (!ZLinkRelocationEnvelopeCodec.TryValidateCanonicalFrozenRecord(
                frozenBytes, out var frozenTruncated))
        {
            if (frozenTruncated) reader.MarkTruncated();
            return DecodeFailure(ref reader, out error);
        }
        var frozen = new FrozenRecord(frozenBytes.ToArray());
        record = new RelocationDataRecord(id, attempt, coordinator, role,
            participant, sequence, frozen);
        return End(ref reader, out error);
    }

    internal static bool TryDecodeRelocationAck(ReadOnlySpan<byte> bytes,
        out RelocationAckRecord record, out DecodeError error)
    {
        record = null!;
        if (!Begin(bytes, ServiceWireConstants.Command.RelocationAck,
                ServiceWireConstants.Flag.None, out var reader, out error))
            return false;
        if (!TryRelocationId(ref reader, out var id)
            || !reader.TryU64(out var attempt) || attempt == 0
            || !TryCoordinator(ref reader, out var coordinator)
            || !reader.TryU8(out var role) || !IsRole(role)
            || !reader.TryU64(out var participant) || participant == 0
            || !reader.TryU64(out var highWater))
            return DecodeFailure(ref reader, out error);
        record = new RelocationAckRecord(id, attempt, coordinator, role,
            participant, highWater);
        return End(ref reader, out error);
    }

    internal static bool TryDecodeRelocationSeal(ReadOnlySpan<byte> bytes,
        out RelocationSealRecord record, out DecodeError error)
    {
        record = null!;
        if (!Begin(bytes, ServiceWireConstants.Command.RelocationSeal,
                ServiceWireConstants.Flag.None, out var reader, out error))
            return false;
        if (!TryRelocationId(ref reader, out var id)
            || !reader.TryU64(out var attempt) || attempt == 0
            || !TryCoordinator(ref reader, out var coordinator)
            || !reader.TryU8(out var role) || !IsRole(role)
            || !reader.TryU8(out var response) || response > 1
            || !TryParticipantTerminals(ref reader, out var participants))
            return DecodeFailure(ref reader, out error);
        record = new RelocationSealRecord(id, attempt, coordinator, role,
            response == 1, participants);
        return End(ref reader, out error);
    }

    internal static bool TryDecodeRelocationComplete(ReadOnlySpan<byte> bytes,
        out RelocationCompleteRecord record, out DecodeError error)
    {
        record = null!;
        if (!Begin(bytes, ServiceWireConstants.Command.RelocationComplete,
                ServiceWireConstants.Flag.None, out var reader, out error))
            return false;
        if (!TryRelocationId(ref reader, out var id)
            || !reader.TryU64(out var attempt) || attempt == 0
            || !TryCoordinator(ref reader, out var coordinator)
            || !reader.TryU8(out var role) || !IsRole(role)
            || !TryRequestSource(ref reader, out var source)
            || !reader.TryU8(out var cleanup) || cleanup > 2)
            return DecodeFailure(ref reader, out error);
        record = new RelocationCompleteRecord(id, attempt, coordinator, role,
            source, cleanup);
        return End(ref reader, out error);
    }

    private static void WriteRelocationId(WireWriter writer, RelocationWireId id)
    {
        if (id.IsEmpty) throw new ArgumentOutOfRangeException(nameof(id));
        writer.U64(id.High);
        writer.U64(id.Low);
    }

    private static void WriteCandidate(WireWriter writer,
        RelocationCandidateRecord candidate)
    {
        ValidateCandidate(candidate);
        writer.Rid(candidate.NodeRid);
        writer.U64(candidate.NodeGeneration);
        writer.Text8(candidate.OwnerId);
        writer.U64(candidate.OwnerLeaseGeneration);
    }

    private static bool TryCandidate(ref WireReader reader,
        out RelocationCandidateRecord candidate)
    {
        candidate = null!;
        if (!reader.TryRid(out var rid)
            || !reader.TryU64(out var generation) || generation == 0
            || !reader.TryText8(out var owner)
            || !reader.TryU64(out var lease) || lease == 0)
            return false;
        candidate = new RelocationCandidateRecord(rid, generation, owner, lease);
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
            || !reader.TrySlice(length, out var bytes)) return false;
        var body = new WireReader(bytes);
        string stable = string.Empty;
        string id;
        ulong generation;
        ulong expected = 0;
        if (kind == 3)
        {
            if (!body.TryText8(out stable) || !body.TryText8(out id)
                || !body.TryU64(out generation) || generation == 0)
                return false;
        }
        else if (!body.TryText8(out id)
                 || !body.TryU64(out generation) || generation == 0
                 || !body.TryU64(out expected) || expected == 0)
            return false;
        if (body.Remaining != 0) return false;
        value = new RelocationObjectRecord(kind, stable, id, generation, expected);
        return true;
    }

    private static void WriteParticipants(WireWriter writer,
        IReadOnlyList<RelocationParticipantRecord> participants)
    {
        if (participants.Count > MaxRelocationParticipants)
            throw new ArgumentOutOfRangeException(nameof(participants));
        ValidateSorted(participants.Select(static p => p.ParticipantId));
        writer.U32(checked((uint)participants.Count));
        foreach (var participant in participants)
        {
            writer.U64(participant.ParticipantId);
            var identity = new WireWriter();
            if (participant.Kind == 2)
            {
                if (participant.SessionOwnerNodeRid.IsEmpty
                    || participant.SessionOwnerNodeGeneration == 0
                    || string.IsNullOrEmpty(participant.SessionOwnerId)
                    || participant.SessionOwnerLeaseGeneration == 0
                    || participant.SessionRid.IsEmpty
                    || participant.BindingGeneration == 0)
                    throw new ArgumentOutOfRangeException(nameof(participants));
                identity.Rid(participant.SessionOwnerNodeRid);
                identity.U64(participant.SessionOwnerNodeGeneration);
                identity.Text8(participant.SessionOwnerId!);
                identity.U64(participant.SessionOwnerLeaseGeneration);
                identity.Rid(participant.SessionRid);
                identity.U64(participant.BindingGeneration);
            }
            else if (participant.Kind != 1)
                throw new ArgumentOutOfRangeException(nameof(participants));
            writer.U8(participant.Kind);
            writer.U16(checked((ushort)identity.Count));
            writer.Bytes(identity.ToArray());
            writer.U64(participant.AllowanceMessages);
            writer.U64(participant.AllowanceBytes);
        }
    }

    private static bool TryParticipants(ref WireReader reader,
        out IReadOnlyList<RelocationParticipantRecord> participants)
    {
        participants = [];
        if (!reader.TryU32(out var count)
            || count > MaxRelocationParticipants) return false;
        var result = new List<RelocationParticipantRecord>(checked((int)count));
        ulong previous = 0;
        for (var index = 0; index < count; index++)
        {
            if (!reader.TryU64(out var id) || id == 0 || id <= previous
                || !reader.TryU8(out var kind) || kind is < 1 or > 2
                || !reader.TryU16(out var length)
                || !reader.TrySlice(length, out var bytes)) return false;
            previous = id;
            var identity = new WireReader(bytes);
            RoutingId ownerRid = default, sessionRid = default;
            ulong ownerGeneration = 0, ownerLease = 0, binding = 0;
            string? ownerId = null;
            if (kind == 2 && (!identity.TryRid(out ownerRid)
                || !identity.TryU64(out ownerGeneration) || ownerGeneration == 0
                || !identity.TryText8(out ownerId)
                || !identity.TryU64(out ownerLease) || ownerLease == 0
                || !identity.TryRid(out sessionRid)
                || !identity.TryU64(out binding) || binding == 0)) return false;
            if (identity.Remaining != 0
                || !reader.TryU64(out var messages)
                || !reader.TryU64(out var bytesAllowance)) return false;
            result.Add(new RelocationParticipantRecord(id, kind, ownerRid,
                ownerGeneration, ownerId, ownerLease, sessionRid, binding,
                messages, bytesAllowance));
        }
        participants = result;
        return true;
    }

    private static void WriteParticipantProgress(WireWriter writer,
        IReadOnlyList<RelocationParticipantProgressRecord> progress)
    {
        if (progress.Count > MaxRelocationParticipants)
            throw new ArgumentOutOfRangeException(nameof(progress));
        ValidateSorted(progress.Select(static p => p.ParticipantId));
        writer.U32(checked((uint)progress.Count));
        foreach (var item in progress)
        {
            if (item.ReplayCursor > item.AcceptedBoundary)
                throw new ArgumentOutOfRangeException(nameof(progress));
            writer.U64(item.ParticipantId);
            writer.U64(item.AcceptedBoundary);
            writer.U64(item.ReplayCursor);
        }
    }

    private static bool TryParticipantProgress(ref WireReader reader,
        out IReadOnlyList<RelocationParticipantProgressRecord> progress)
    {
        progress = [];
        if (!reader.TryU32(out var count)
            || count > MaxRelocationParticipants) return false;
        var result = new List<RelocationParticipantProgressRecord>((int)count);
        ulong previous = 0;
        for (var index = 0; index < count; index++)
        {
            if (!reader.TryU64(out var id) || id == 0 || id <= previous
                || !reader.TryU64(out var boundary)
                || !reader.TryU64(out var cursor) || cursor > boundary)
                return false;
            previous = id;
            result.Add(new RelocationParticipantProgressRecord(id, boundary, cursor));
        }
        progress = result;
        return true;
    }

    private static void WriteParticipantTerminals(WireWriter writer,
        IReadOnlyList<RelocationParticipantTerminalRecord> participants)
    {
        if (participants.Count > MaxRelocationParticipants)
            throw new ArgumentOutOfRangeException(nameof(participants));
        ValidateSorted(participants.Select(static p => p.ParticipantId));
        writer.U32(checked((uint)participants.Count));
        foreach (var item in participants)
        {
            writer.U64(item.ParticipantId);
            writer.U64(item.HighWater);
        }
    }

    private static bool TryParticipantTerminals(ref WireReader reader,
        out IReadOnlyList<RelocationParticipantTerminalRecord> participants)
    {
        participants = [];
        if (!reader.TryU32(out var count)
            || count > MaxRelocationParticipants) return false;
        var result = new List<RelocationParticipantTerminalRecord>((int)count);
        ulong previous = 0;
        for (var index = 0; index < count; index++)
        {
            if (!reader.TryU64(out var id) || id == 0 || id <= previous
                || !reader.TryU64(out var highWater)) return false;
            previous = id;
            result.Add(new RelocationParticipantTerminalRecord(id, highWater));
        }
        participants = result;
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

    private static bool TryRoot(ref WireReader reader, out RelocationRootRecord? root)
    {
        root = null;
        if (!reader.TryU8(out var has) || has > 1
            || !reader.TryU16(out var length)
            || !reader.TrySlice(length, out var bytes)) return false;
        var body = new WireReader(bytes);
        if (has == 1)
        {
            if (!body.TryText16(out var reference, requireNonEmpty: true)
                || !body.TryU32(out var checksum)) return false;
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
            || !IsTerminalFailureValid(record.TerminalResult, record.FailureCode))
            throw new ArgumentOutOfRangeException(nameof(record));
        writer.U8(13); // mesh-record-kind.relocationControl
        var source = new WireWriter();
        // frozen-source-identity fixes node identity before the lease owner.
        source.Rid(record.Source.NodeRid);
        source.U64(record.Source.NodeGeneration);
        source.Text8(record.Source.OwnerId);
        source.U64(record.Source.LeaseGeneration);
        writer.U8(1); // frozen-source-kind.node
        writer.U16(checked((ushort)source.Count));
        writer.Bytes(source.ToArray());
        writer.U8(0); // hasMetadata
        WriteOperationId(writer, record.OperationId);
        writer.U32(0); // mesh-operation-kind.none
        writer.U16(0); // frozen-reply-route.none
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
            || !reader.TrySlice(sourceLength, out var sourceBytes)) return false;
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
                (ServiceWireConstants.FrameworkErrorCode)failureValue)) return false;
        var operation = new MeshOperationId(operationHigh, operationLow);
        var source = new RequestSourceFence(sourceOwner, sourceLease,
            sourceRid, sourceGeneration);
        record = new FrozenRelocationControlRecord(source, operation, phase,
            role, id, relocationObject, terminal,
            (ServiceWireConstants.FrameworkErrorCode)failureValue);
        return true;
    }

    private static bool TryReadyExtension(ref WireReader reader,
        out ulong sourceGeneration, out ulong targetGeneration,
        out ulong reservationGeneration, out RelocationRootRecord? root,
        out ulong applicationVersion,
        out IReadOnlyList<RelocationParticipantProgressRecord> progress)
    {
        sourceGeneration = targetGeneration = reservationGeneration =
            applicationVersion = 0;
        root = null;
        progress = [];
        if (!reader.TryU32(out var total)
            || !reader.TrySlice(checked((int)total), out var bytes)) return false;
        var extension = new WireReader(bytes);
        byte previous = 0;
        var required = new HashSet<byte>();
        string? reference = null;
        uint? checksum = null;
        while (extension.Remaining > 0)
        {
            if (!extension.TryU8(out var id) || id <= previous
                || !extension.TryU32(out var length) || length > extension.Remaining
                || !extension.TrySlice((int)length, out var field)) return false;
            previous = id;
            var value = new WireReader(field);
            switch (id)
            {
                case 2:
                    if (!value.TryU64(out sourceGeneration) || sourceGeneration == 0)
                        return false;
                    required.Add(id);
                    break;
                case 3:
                    if (!value.TryU64(out targetGeneration) || targetGeneration == 0)
                        return false;
                    required.Add(id);
                    break;
                case 4:
                    if (!value.TryU64(out reservationGeneration)
                        || reservationGeneration == 0) return false;
                    required.Add(id);
                    break;
                case 5:
                    if (!value.TryText16(out reference, requireNonEmpty: true))
                        return false;
                    break;
                case 6:
                    if (!value.TryU32(out var decodedChecksum)) return false;
                    checksum = decodedChecksum;
                    break;
                case 8:
                    if (!value.TryU64(out applicationVersion)) return false;
                    required.Add(id);
                    break;
                case 9:
                    if (!TryParticipantProgress(ref value, out progress)) return false;
                    required.Add(id);
                    break;
            }
            if (value.Remaining != 0 && id is 2 or 3 or 4 or 5 or 6 or 8 or 9)
                return false;
        }
        if (!required.SetEquals([2, 3, 4, 8, 9])
            || (reference is null) != (checksum is null)) return false;
        if (reference is not null)
            root = new RelocationRootRecord(reference, checksum!.Value);
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

    private static bool DecodeFailure(ref WireReader reader, out DecodeError error)
    {
        error = reader.Truncated ? DecodeError.TruncatedField : DecodeError.InvalidField;
        return false;
    }

    private static byte[] Finish(ServiceWireConstants.Command command,
        ServiceWireConstants.Flag flags, WireWriter body)
    {
        var result = Prefix(command, flags, body.Count);
        body.CopyTo(result.AsSpan(5));
        return result;
    }

    private static byte[] U64Bytes(ulong value)
    {
        var writer = new WireWriter();
        writer.U64(value);
        return writer.ToArray();
    }

    private static byte[] U32Bytes(uint value)
    {
        var writer = new WireWriter();
        writer.U32(value);
        return writer.ToArray();
    }

    private static void ValidateRelocationCommon(RelocationWireId id,
        ulong attempt, RelocationCoordinatorFence coordinator)
    {
        if (id.IsEmpty || attempt == 0)
            throw new ArgumentOutOfRangeException(nameof(id));
        ValidateCoordinator(coordinator);
    }

    private static void ValidateCandidate(RelocationCandidateRecord candidate)
    {
        ArgumentNullException.ThrowIfNull(candidate);
        ArgumentException.ThrowIfNullOrEmpty(candidate.OwnerId);
        if (candidate.NodeRid.IsEmpty || candidate.NodeGeneration == 0
            || candidate.OwnerLeaseGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(candidate));
    }

    private static void ValidateRole(byte role)
    {
        if (!IsRole(role)) throw new ArgumentOutOfRangeException(nameof(role));
    }

    private static bool IsRole(byte role) => role is >= 1 and <= 3;
    private static void ValidateRound(byte round)
    {
        if (!IsRound(round)) throw new ArgumentOutOfRangeException(nameof(round));
    }
    private static bool IsRound(byte round) => round is >= 1 and <= 3;

    private static void ValidateSorted(IEnumerable<ulong> ids)
    {
        ulong previous = 0;
        foreach (var id in ids)
        {
            if (id == 0 || id <= previous)
                throw new ArgumentOutOfRangeException(nameof(ids));
            previous = id;
        }
    }
}
